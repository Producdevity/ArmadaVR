#!/usr/bin/env python3
"""Prepare pinned Basalt sources or build an isolated native ARM64 VIO bundle."""
import argparse
import hashlib
import json
import os
import re
import shlex
from pathlib import Path
import shutil
import subprocess
import tarfile
import urllib.request
import uuid
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
PROFILE = ROOT / 'profiles/basalt.json'
PREFIX = Path('stage/opt/reframed-vr/basalt')


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def tree_digest(root):
    records = []
    for directory, dirs, files in os.walk(root):
        dirs[:] = sorted(d for d in dirs if d != '.git')
        for name in list(dirs):
            path = Path(directory) / name
            if path.is_symlink():
                records.append([path.relative_to(root).as_posix(), 'symlink', os.readlink(path)])
                dirs.remove(name)
        for name in sorted(files):
            if name == '.git':
                continue
            path = Path(directory) / name
            relative = path.relative_to(root).as_posix()
            if path.is_symlink():
                records.append([relative, 'symlink', os.readlink(path)])
            elif path.is_file():
                records.append([relative, path.stat().st_mode & 0o111, digest(path)])
            else:
                raise ValueError('Unexpected source entry: ' + relative)
    records.sort()
    return hashlib.sha256(json.dumps(records, separators=(',', ':')).encode()).hexdigest()


def verify(source, profile):
    if tree_digest(source) != profile['source_tree_sha256']:
        raise ValueError('Basalt source or pinned dependency tree differs')
    if digest(source / 'thirdparty/vit/vit_interface.h') != profile['vit_header_sha256']:
        raise ValueError('Unexpected VIT interface')


def prepare(output, profile):
    output.mkdir(parents=True, exist_ok=False)
    source = output / 'source'
    for index, item in enumerate(profile['sources']):
        target = source / item['path']
        target.mkdir(parents=True, exist_ok=True)
        if any(target.iterdir()):
            raise ValueError('Source destination is not empty: ' + str(target))
        with (output / f'fetch-{index}.log').open('wb') as log:
            for command in [
                ['git', 'init', '-q', str(target)],
                ['git', '-C', str(target), 'fetch', '--depth', '1', item['url'], item['commit']],
                ['git', '-C', str(target), 'checkout', '--detach', 'FETCH_HEAD'],
            ]:
                subprocess.run(command, check=True, stdout=log, stderr=subprocess.STDOUT, timeout=120)
        actual = subprocess.check_output(['git', '-C', str(target), 'rev-parse', 'HEAD'], text=True).strip()
        if actual != item['commit']:
            raise ValueError('Fetched commit differs: ' + item['path'])
    for index, item in enumerate(profile['archives']):
        archive = output / f'dependency-{index}.tar.gz'
        with urllib.request.urlopen(item['url'], timeout=45) as response, archive.open('xb') as stream:
            size = 0
            while block := response.read(1024 * 1024):
                size += len(block)
                if size > item['size']:
                    raise ValueError('Dependency archive exceeds pinned size')
                stream.write(block)
        if size != item['size'] or digest(archive) != item['sha256']:
            raise ValueError('Dependency archive differs')
        extracted = output / f'archive-{index}'
        with tarfile.open(archive) as stream:
            stream.extractall(extracted, filter='data')
        target = source / item['path']
        if target.exists() and any(target.iterdir()):
            raise ValueError('Archive destination is not empty')
        shutil.copytree(extracted / item['root'], target, dirs_exist_ok=True)
    verify(source, profile)
    (output / 'source.json').write_text(json.dumps(profile, indent=2) + '\n')
    print(source)


def build(source, output, image, engine, profile):
    verify(source, profile)
    if not re.fullmatch(r'sha256:[0-9a-f]{64}', image):
        raise ValueError('Use an immutable local build image ID')
    metadata = json.loads(subprocess.check_output([engine, 'image', 'inspect', image], text=True))[0]
    if metadata['Architecture'] != 'arm64':
        raise ValueError('Build image must be ARM64')
    if ',' in str(source) or ',' in str(output):
        raise ValueError('Container bind paths cannot contain commas')
    output.mkdir(parents=True, exist_ok=False)
    name = 'reframed-basalt-' + uuid.uuid4().hex[:12]
    script = ('set -e\nrpm -qa | sort > /out/packages.txt\n'
              'cmake -S /source -B /out/build -G Ninja ' + shlex.join(profile['cmake_options']) +
              ' > /out/configure.log 2>&1\n'
              'cmake --build /out/build --parallel 1 > /out/compile.log 2>&1\n'
              'ctest --test-dir /out/build --output-on-failure --output-junit /out/ctest.xml -j1 > /out/ctest.log 2>&1\n'
              'DESTDIR=/out/stage cmake --install /out/build > /out/install.log 2>&1\n')
    command = [engine, 'run', '--rm', '--name', name, '--pull', 'never', '--network', 'none', '--read-only',
               '--cap-drop', 'ALL', '--security-opt', 'no-new-privileges',
               '--memory', '4g', '--memory-swap', '4g', '--cpus', '2', '--pids-limit', '128',
               '--tmpfs', '/tmp:rw,exec,size=128m',
               '--mount', f'type=bind,src={source},dst=/source,readonly',
               '--mount', f'type=bind,src={output},dst=/out', '--entrypoint', 'sh', image, '-c', script]
    report = {'status': 'started', 'profile': profile, 'build_image': metadata['Id'],
              'builder_sha256': digest(Path(__file__)), 'command': command, 'physical_tracking_verified': False}
    try:
        with (output / 'run.log').open('wb') as log:
            result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, timeout=1800)
            if result.returncode:
                raise RuntimeError('Basalt build failed; inspect preserved logs')
        tests = ET.parse(output / 'ctest.xml').getroot()
        if (int(tests.get('tests', '0')) != profile['test_count'] or
                any(int(tests.get(field, '0')) for field in ('failures', 'errors', 'skipped', 'disabled'))):
            raise ValueError('Expected all pinned Basalt tests to pass')
        library = output / PREFIX / 'lib64/libbasalt.so.2.0.1'
        with library.open('rb') as stream:
            header = stream.read(20)
        if header[:6] != b'\x7fELF\x02\x01' or header[18:20] != b'\xb7\x00':
            raise ValueError('Expected a native ARM64 library')
        verify(source, profile)
        share = output / PREFIX / 'share/basalt'
        share.mkdir(exist_ok=True, parents=True)
        shutil.copyfile(PROFILE, share / 'source-profile.json')
        for item in source.rglob('*'):
            if item.is_file() and re.fullmatch(r'(license|copying)(\..*)?', item.name, re.IGNORECASE):
                target = share / 'licenses' / item.relative_to(source)
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(item, target)
        interface = output / PREFIX / 'include/vit/vit_interface.h'
        interface.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source / 'thirdparty/vit/vit_interface.h', interface)
        pkgconfig = output / PREFIX / 'lib64/pkgconfig/basalt.pc'
        pkgconfig.write_text("prefix=/opt/reframed-vr/basalt\nlibdir=${prefix}/lib64\n"
                             "includedir=${prefix}/include\n\nName: Basalt\n"
                             "Description: Visual-inertial tracking\nVersion: 2.0.1\n"
                             "Libs: -L${libdir} -lbasalt\nCflags: -isystem ${includedir}\n")
        (share / 'source-tree-sha256.txt').write_text(profile['source_tree_sha256'] + '\n')
        report.update(status='passed', library_sha256=digest(library), library_bytes=library.stat().st_size,
                      stage_sha256=tree_digest(output / PREFIX), ctest_sha256=digest(output / 'ctest.xml'))
    except BaseException as error:
        report.update(status='failed', error=str(error))
        with (output / 'cleanup.log').open('wb') as log:
            try:
                subprocess.run([engine, 'rm', '-f', name], stdout=log, stderr=subprocess.STDOUT, timeout=30)
            except (OSError, subprocess.TimeoutExpired):
                pass
        raise
    finally:
        (output / 'build.json').write_text(json.dumps(report, indent=2) + '\n')
    print('Basalt ARM64 bundle: ' + str(output))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='command', required=True)
    fetch = sub.add_parser('prepare')
    fetch.add_argument('--output', required=True, type=Path)
    compile = sub.add_parser('build')
    compile.add_argument('source', type=Path)
    compile.add_argument('--output', required=True, type=Path)
    compile.add_argument('--image', required=True)
    compile.add_argument('--engine', default='docker')
    args = parser.parse_args()
    profile = json.loads(PROFILE.read_text())
    if args.command == 'prepare':
        prepare(args.output.resolve(), profile)
    else:
        build(args.source.resolve(), args.output.resolve(), args.image, args.engine, profile)


if __name__ == '__main__':
    main()
