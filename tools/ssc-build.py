#!/usr/bin/env python3
"""Build and package pinned SSC sources inside an offline native ARM64 container."""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('ssc_artifact', ROOT / 'tools/ssc-artifact.py')
artifact = importlib.util.module_from_spec(spec)
spec.loader.exec_module(artifact)


def build(archive, output, image):
    if sys.platform != 'linux' or platform.machine() != 'aarch64':
        raise ValueError('Build SSC in a native ARM64 Linux container')
    if not re.fullmatch(r'sha256:[0-9a-f]{64}', image):
        raise ValueError('Record the immutable build image ID')
    profile = json.loads((ROOT / 'profiles/ssc.json').read_text())
    if artifact.digest(archive) != profile['source_archive_sha256']:
        raise ValueError('SSC source archive differs from the pinned revision')
    output.mkdir(parents=True, exist_ok=False)
    names = [*profile['patches'], 'tools/ssc-build.py', 'tools/test-ssc-reports.py',
             'tests/ssc-reports.c', 'tests/ssc-lifetime.c', 'src/pico-controller.c', 'src/pico-controller.h']
    report = {'status': 'started', 'hardware_verified': False, 'image': image, 'profile': profile,
              'inputs_sha256': {name: artifact.digest(ROOT / name) for name in names}, 'commands': []}

    def run(command, name, timeout=120, env=None):
        report['commands'].append(command)
        with (output / name).open('xb') as log:
            subprocess.run(command, check=True, timeout=timeout, stdout=log, stderr=subprocess.STDOUT, env=env)

    try:
        with tempfile.TemporaryDirectory(prefix='ssc-build-') as temporary:
            work = Path(temporary)
            with tarfile.open(archive) as stream:
                for member in stream.getmembers():
                    if not member.isdir() and not member.isfile():
                        raise ValueError('Unexpected SSC source archive entry')
                stream.extractall(work, filter='data')
            children = list(work.iterdir())
            if len(children) != 1 or not children[0].is_dir():
                raise ValueError('Expected one SSC archive source directory')
            source = children[0]
            for path in (source, *source.rglob('*')):
                path.chmod(0o755 if path.is_dir() or path.stat().st_mode & 0o111 else 0o644)
            run([sys.executable, '-B', str(ROOT / 'tools/test-ssc-reports.py'), str(source),
                 '--output', str(output / 'tests')], 'tests.log', 180)
            report['tests_sha256'] = artifact.digest(output / 'tests/result.json')
            patched = output / 'tests/patched'
            build_dir = work / 'build'
            run(['meson', 'setup', str(build_dir), str(patched), '--prefix=/opt/armada-vr/ssc',
                 '--libdir=lib64', '--buildtype=debugoptimized'], 'configure.log')
            run(['meson', 'compile', '-C', str(build_dir), '-j2'], 'compile.log')
            installed = work / 'install'
            run(['meson', 'install', '-C', str(build_dir), '--destdir', str(installed)], 'install.log')
            installed = installed / artifact.INSTALL_PREFIX
            prefix = output / artifact.PREFIX
            (prefix / 'lib64').mkdir(parents=True)
            (prefix / 'bin').mkdir()

            def copy(source_file, name, mode):
                target = prefix / name
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(source_file, target)
                target.chmod(mode)

            copy(installed / 'lib64/libssc.so.2', 'lib64/libssc.so.2', 0o755)
            copy(installed / 'bin/ssccli', 'bin/ssccli', 0o755)
            copy(source / 'LICENSE', 'share/licenses/libssc/LICENSE', 0o644)
            copy(archive, 'share/ssc/source.tar.gz', 0o644)
            copy(ROOT / 'profiles/ssc.json', 'share/ssc/profile.json', 0o644)
            for name in profile['patches']:
                copy(ROOT / name, 'share/ssc/' + Path(name).name, 0o644)
            report['dependencies'] = {}
            for name, dep in profile['runtime_dependencies'].items():
                version = subprocess.check_output(['rpm', '-q', name], text=True).strip()
                if version != dep['nevra']:
                    raise ValueError('SSC dependency version differs: ' + name)
                run(['rpm', '-V', name], 'verify-' + name + '.log', 30)
                link = Path('/usr/lib64') / dep['library']
                if not link.is_symlink() or str(link.readlink()) != dep['target']:
                    raise ValueError('SSC dependency ABI link differs: ' + name)
                copy(link.resolve(), 'lib64/' + dep['target'], 0o755)
                (prefix / 'lib64' / dep['library']).symlink_to(dep['target'])
                copy(Path('/usr/share/licenses') / name / dep['license'],
                     'share/licenses/' + name + '/' + dep['license'], 0o644)
                report['dependencies'][name] = version
            env = dict(os.environ, LD_LIBRARY_PATH=str(prefix / 'lib64'))
            run(['ldd', '-r', str(prefix / 'bin/ssccli')], 'dependencies.txt', env=env)
            text = (output / 'dependencies.txt').read_text()
            if 'not found' in text or 'undefined symbol' in text:
                raise ValueError('Unresolved SSC runtime dependency')
            run([str(prefix / 'bin/ssccli'), '--version'], 'version.txt', env=env)
            if '0.4.4' not in (output / 'version.txt').read_text():
                raise ValueError('Unexpected SSC runtime version')
            report['contents'] = artifact.contents(prefix)
            if report['inputs_sha256'] != {name: artifact.digest(ROOT / name) for name in names}:
                raise ValueError('SSC inputs changed during the build')
            report['status'] = 'compiled'
    except Exception as error:
        report.update(status='failed', error=str(error))
        raise
    finally:
        (output / 'build.json').write_text(json.dumps(report, indent=2) + '\n')
    artifact.package(output)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('archive', type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--image-id', required=True)
    args = parser.parse_args()
    build(args.archive.resolve(), args.output.resolve(), args.image_id)
