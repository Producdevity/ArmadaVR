#!/usr/bin/env python3
"""Test Turnip's display descriptor helper in a diskless virtio-gpu QEMU guest."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import platform
import stat
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def initramfs(files):
    archive = bytearray()

    def entry(name, mode, data=b'', rdev=(0, 0)):
        name = name.encode() + b'\0'
        fields = (1, mode, 0, 0, 1, 0, len(data), 0, 0, *rdev, len(name), 0)
        archive.extend(b'070701' + ''.join(f'{value:08x}' for value in fields).encode() + name)
        archive.extend(b'\0' * (-len(archive) % 4))
        archive.extend(data)
        archive.extend(b'\0' * (-len(archive) % 4))

    directories = {'dev', 'proc', 'sys'}
    for name in files:
        directories.update(str(p) for p in Path(name).parents if str(p) != '.')
    for name in sorted(directories, key=lambda p: (p.count('/'), p)):
        entry(name, stat.S_IFDIR | 0o755)
    entry('dev/console', stat.S_IFCHR | 0o600, rdev=(5, 1))
    for name, data in sorted(files.items()):
        entry(name, stat.S_IFREG | 0o755, data)
    entry('TRAILER!!!', 0)
    return bytes(archive)


def run(args):
    kernel, turnip, output = (p.absolute() for p in (args.kernel, args.turnip, args.output))
    if output.exists() or output.is_symlink() or any(c in str(p) for p in (kernel, turnip, output) for c in ':,'):
        raise ValueError('Choose a new output directory and paths without commas or colons')
    build = json.loads((kernel / 'build.json').read_text())
    if build.get('status') != 'compiled' or build.get('qemu_transports') is not True:
        raise ValueError('Requires a compiled QEMU transport kernel; physical builds are refused')
    expected = {item['path']: item['sha256'] for item in build['artifacts']}
    for name in ('Image', '.config'):
        if (kernel / name).is_symlink() or digest(kernel / name) != expected.get(name):
            raise ValueError('Kernel artifact mismatch: ' + name)
    config = (kernel / '.config').read_text().splitlines()
    for name in ('CONFIG_DRM', 'CONFIG_DRM_VIRTIO_GPU', 'CONFIG_DEVTMPFS'):
        if name + '=y' not in config:
            raise ValueError('Requires built-in ' + name)
    spec = importlib.util.spec_from_file_location('turnip_artifact', ROOT / 'tools/turnip-artifact.py')
    validator = importlib.util.module_from_spec(spec); spec.loader.exec_module(validator)
    validator.validate(turnip)
    driver = json.loads((turnip / 'build.json').read_text())
    image = driver['identity']['image']
    if not image.startswith('sha256:') or len(image) != 71 or any(c not in '0123456789abcdef' for c in image[7:]):
        raise ValueError('Turnip build image must be an immutable SHA-256 ID')
    fixture = ROOT / 'tests/kgsl-display.c'
    if driver['kgsl_display_test']['source_sha256'] != digest(fixture):
        raise ValueError('Display test changed since compilation')
    output.mkdir(parents=True)
    report = {'status': 'started', 'target': 'qemu-kgsl-display',
              'scope': 'real virtio-gpu DRM descriptor ownership; no KGSL rendering or panel execution',
              'hardware_verified': False, 'flash_image': False,
              'kernel_build_sha256': digest(kernel / 'build.json'), 'kernel_sha256': expected['Image'],
              'turnip_build_sha256': digest(turnip / 'build.json'),
              'fixture_sha256': digest(fixture), 'runner_sha256': digest(Path(__file__)),
              'image': image, 'commands': []}
    started = time.monotonic()

    def execute(command, log, timeout):
        report['commands'].append(command)
        with (output / log).open('wb') as stream:
            subprocess.run(command, stdin=subprocess.DEVNULL, stdout=stream, stderr=subprocess.STDOUT,
                           check=True, timeout=timeout)

    try:
        # Resolve the test's exact ELF dependencies inside its immutable build image.
        export = '''from pathlib import Path
import hashlib,json,re,shutil,subprocess
binary=Path('/input/kgsl-display')
log=subprocess.check_output(['ldd',str(binary)],text=True)
if 'not found' in log:raise RuntimeError(log)
interpreter=re.search(r'Requesting program interpreter: ([^\\]]+)',subprocess.check_output(['readelf','-l',str(binary)],text=True)).group(1)
paths=set(re.findall(r'(?:=>\\s+|^\\s*)(/[^\\s]+)',log,re.M));paths.add(interpreter)
result={}
for path in sorted(paths):
 source=Path(path);destination=Path('/output/dependencies')/path.lstrip('/')
 destination.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(source,destination)
 result[path]=hashlib.sha256(destination.read_bytes()).hexdigest()
Path('/output/dependencies.json').write_text(json.dumps(result,indent=2)+'\\n')
print(log)
'''
        execute([args.engine, 'run', '--rm', '--network', 'none', '--read-only', '--cpus', '1',
                 '--memory', '256m', '--memory-swap', '256m', '--pids-limit', '64',
                 '-v', f'{turnip}:/input:ro', '-v', f'{output}:/output', '--entrypoint', 'python3',
                 image, '-B', '-c', export], 'dependencies.log', 60)
        files = {'init': (turnip / 'kgsl-display').read_bytes()}
        for name, sha in json.loads((output / 'dependencies.json').read_text()).items():
            relative = name.lstrip('/')
            if not name.startswith('/') or '..' in Path(relative).parts:
                raise ValueError('Invalid dependency path')
            path = output / 'dependencies' / relative
            if digest(path) != sha:
                raise ValueError('Dependency hash mismatch')
            files[relative] = path.read_bytes()
        (output / 'initramfs.cpio').write_bytes(initramfs(files))
        execute([args.qemu, '-name', 'Armada KGSL display descriptor test', '-machine', 'virt', '-accel', args.accel,
                 '-cpu', 'host' if args.accel in ('hvf', 'kvm') else 'max', '-smp', '2', '-m', '1024',
                 '-nodefaults', '-nographic', '-monitor', 'none', '-serial', 'stdio', '-no-reboot',
                 '-nic', 'none', '-device', 'virtio-gpu-pci', '-kernel', str(kernel / 'Image'),
                 '-initrd', str(output / 'initramfs.cpio'),
                 '-append', 'console=ttyAMA0 earlycon rdinit=/init selinux=0 audit=0 armada.kgsl_display_test=1'],
                'boot.log', 45)
        log = (output / 'boot.log').read_text(errors='replace')
        cases = ['PRIMARY_AND_RENDER', 'MASTER_CONTENTION', 'LEASE_IDENTITY', 'REACQUIRE_CLEANUP', 'ALL']
        if any('KGSL_DISPLAY_' + name + '_PASS' not in log for name in cases) or 'reboot: Power down' not in log:
            raise RuntimeError('Missing acceptance or clean power-off; inspect boot.log')
        if any(marker in log for marker in ('KGSL_DISPLAY_FAIL', 'WARNING:', 'BUG:', 'Kernel panic', 'Oops:')):
            raise RuntimeError('Test assertion or kernel failure; inspect boot.log')
        validator.validate(turnip)
        if (digest(kernel / 'Image') != expected['Image'] or
                digest(kernel / 'build.json') != report['kernel_build_sha256'] or
                digest(turnip / 'build.json') != report['turnip_build_sha256'] or
                digest(fixture) != report['fixture_sha256'] or digest(Path(__file__)) != report['runner_sha256']):
            raise ValueError('Inputs changed during execution')
        report.update(status='passed', cases=cases)
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        report.update(status='failed', error=str(error))
        raise
    finally:
        report['artifacts_sha256'] = {str(p.relative_to(output)): digest(p) for p in sorted(output.rglob('*'))
                                       if p.is_file() and p != output / 'result.json'}
        report['elapsed_seconds'] = round(time.monotonic() - started, 3)
        (output / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
    print(output / 'result.json')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('kernel', type=Path)
    parser.add_argument('--turnip', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--engine', default=os.environ.get('CONTAINER_ENGINE', 'docker'))
    parser.add_argument('--qemu', default='qemu-system-aarch64')
    parser.add_argument('--accel', choices=('hvf', 'kvm', 'tcg'),
                        default='hvf' if platform.system() == 'Darwin' and platform.machine() == 'arm64' else 'tcg')
    try:
        run(parser.parse_args())
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        parser.exit(1, f'{error}\n')
