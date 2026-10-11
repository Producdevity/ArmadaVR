#!/usr/bin/env python3
"""Prepare matched Frame systemd commands for a vendor-kernel initramfs."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path, PurePosixPath
import posixpath
import shutil
import stat
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('systemd_runtime', ROOT / 'tools/systemd-runtime.py')
runtime = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runtime)
SOURCE_SHA256 = '668da2b1d32e9b779a1ee27ed570961ceaaf83a5dfeadd8129b62603223c319d'


def prepare(args):
    from dissect.btrfs import Btrfs
    source, template, output = args.source.resolve(), args.template.resolve(), args.output.absolute()
    if output.exists() or output.is_symlink():
        raise ValueError('Choose a new output directory')
    if not (template / 'etc/initrd-release').is_file():
        raise ValueError('Template must be an unpacked vendor-kernel initramfs')
    if runtime.digest(source) != SOURCE_SHA256:
        raise ValueError('Frame repair filesystem does not match the verified source')
    selected = runtime.commands(template)
    template_hashes = {name: runtime.digest(template / name) for name in selected}
    output.mkdir(parents=True)
    files = output / 'files'
    private = files / runtime.PREFIX
    records, units, retained, commands = {}, [], [], []
    total = 0
    with source.open('rb') as stream:
        fs = Btrfs(stream)

        def extract(name, pending=()):
            nonlocal total
            name = posixpath.normpath('/' + name.lstrip('/'))
            if not name.startswith(('/usr/lib/', '/usr/bin/')) or name in pending or len(pending) > 32:
                raise ValueError('Invalid or cyclic private runtime dependency: ' + name)
            if name in records:
                return
            node = fs.get(name)
            dest = private / name.lstrip('/')
            dest.parent.mkdir(parents=True, exist_ok=True)
            if node.is_symlink():
                target = posixpath.normpath(node.link if node.link.startswith('/') else
                                            str(PurePosixPath(name).parent / node.link))
                extract(target, (*pending, name))
                link = os.path.relpath(private / target.lstrip('/'), dest.parent)
                dest.symlink_to(link)
                records[name] = {'source_link': node.link}
                return
            if not node.is_file() or node.size > 32 * 1024**2:
                raise ValueError('Unexpected runtime source: ' + name)
            data = node.open().read(node.size + 1)
            total += len(data)
            if len(data) != node.size or total > 150 * 1024**2:
                raise ValueError('Runtime source exceeds size limits')
            dest.write_bytes(data)
            dest.chmod(node.mode & 0o777)
            records[name] = {'sha256': hashlib.sha256(data).hexdigest(), 'bytes': len(data)}
            for dep in runtime.needed(data):
                for base in ('/usr/lib/', '/usr/lib/systemd/'):
                    try:
                        fs.get(base + dep)
                    except FileNotFoundError:
                        continue
                    extract(base + dep)
                    break
                else:
                    raise ValueError('Unresolved runtime dependency: ' + dep)

        for name in selected:
            try:
                fs.get('/' + name)
            except FileNotFoundError:
                if name != runtime.VALIDATEFS:
                    raise ValueError('Frame has no matching systemd command: ' + name)
                retained.append(name)
                continue
            extract(name)
            commands.append(name)
        extract('/usr/lib/ld-linux-aarch64.so.1')
        # These are loaded with dlopen and are absent from DT_NEEDED.
        for name in ('libkmod.so.2', 'libbpf.so.1', 'liblz4.so.1', 'libzstd.so.1', 'liblzma.so.5'):
            extract('/usr/lib/' + name)
        for path in sorted((template / 'usr/lib/systemd/system').iterdir()):
            if path.is_symlink() or not path.is_file():
                continue
            name = path.relative_to(template).as_posix()
            try:
                node = fs.get('/' + name)
            except FileNotFoundError:
                continue
            if not node.is_file() or node.size > 65536:
                raise ValueError('Invalid systemd unit: ' + name)
            data = node.open().read(node.size + 1)
            if len(data) != node.size:
                raise ValueError('Truncated systemd unit: ' + name)
            dest = files / name
            dest.parent.mkdir(parents=True, exist_ok=True)
            dest.write_bytes(data)
            dest.chmod(node.mode & 0o777)
            template_hashes[name] = runtime.digest(path)
            units.append(name)
    compiler = subprocess.check_output([args.zig, 'version'], text=True, timeout=10).strip()
    launcher = output / 'launcher'
    code = ROOT / 'system/systemd/launcher.c'
    code_sha = runtime.digest(code)
    command = [args.zig, 'cc', '-target', 'aarch64-linux-musl', '-static', '-Os', '-Werror', '-Wall',
               '-Wextra', str(code), '-o', str(launcher)]
    with tempfile.TemporaryDirectory(prefix='compiler-cache-', dir=output) as cache:
        env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=cache + '/global', ZIG_LOCAL_CACHE_DIR=cache + '/local')
        with (output / 'compile.log').open('wb') as log:
            subprocess.run(command, env=env, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=180)
    if runtime.needed(launcher.read_bytes()):
        raise ValueError('Systemd launcher must be static')
    for name in commands:
        dest = files / name
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(launcher, dest)
        dest.chmod(0o755)
    inventory = []
    for path in sorted(files.rglob('*')):
        name = path.relative_to(output).as_posix()
        if path.is_symlink():
            inventory.append({'path': name, 'link': os.readlink(path)})
        elif path.is_file():
            inventory.append({'path': name, 'bytes': path.stat().st_size,
                              'mode': stat.S_IMODE(path.stat().st_mode), 'sha256': runtime.digest(path)})
        elif not path.is_dir():
            raise ValueError('Unexpected runtime file type')
    if runtime.digest(source) != SOURCE_SHA256 or runtime.digest(code) != code_sha or any(
            runtime.digest(template / name) != sha for name, sha in template_hashes.items()):
        raise ValueError('Systemd preparation inputs changed')
    manifest = {'schema_version': 1, 'status': 'prepared', 'scope': 'initramfs',
                'prefix': runtime.PREFIX, 'source_sha256': SOURCE_SHA256,
                'source_systemd_version': '257.7-2.2', 'commands': commands, 'units': units,
                'retained_commands': retained, 'template_sha256': template_hashes, 'files': inventory,
                'source_files': records, 'launcher_sha256': runtime.digest(launcher),
                'launcher_source_sha256': code_sha, 'compiler_version': compiler, 'compile_command': command,
                'hardware_boot_verified': False, 'flash_ready': False}
    (output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    runtime.validate(output)
    print(output)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path, help='Unmodified 20260922.5153644 Frame repair rootfs-A.img')
    parser.add_argument('--template', required=True, type=Path, help='Unpacked initramfs from the same builder image')
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--zig', default='zig')
    try:
        prepare(parser.parse_args())
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        parser.exit(1, f'{error}\n')
