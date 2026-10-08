#!/usr/bin/env python3
"""Test DMA-buffer sync-file ioctls with software fences in a diskless QEMU guest."""
import argparse
import hashlib
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


def initramfs(init, module=None):
    archive = bytearray()

    def entry(name, mode, data=b'', rdev=(0, 0)):
        name = name.encode() + b'\0'
        fields = (1, mode, 0, 0, 1, 0, len(data), 0, 0, *rdev, len(name), 0)
        archive.extend(b'070701' + ''.join(f'{value:08x}' for value in fields).encode() + name)
        archive.extend(b'\0' * (-len(archive) % 4))
        archive.extend(data)
        archive.extend(b'\0' * (-len(archive) % 4))

    for name in ('dev', 'proc', 'sys'):
        entry(name, stat.S_IFDIR | 0o755)
    entry('dev/console', stat.S_IFCHR | 0o600, rdev=(5, 1))
    entry('init', stat.S_IFREG | 0o755, init)
    if module is not None:
        entry('dma_buf_resv_test.ko', stat.S_IFREG | 0o600, module)
    entry('TRAILER!!!', 0)
    return bytes(archive)


def run(args):
    kernel, output = args.kernel.absolute(), args.output.absolute()
    fixture = ROOT / 'tests/dma-buf-sync.c'
    if output.exists() or output.is_symlink() or any(c in str(p) for p in (kernel, output, fixture) for c in (':', ',')):
        raise ValueError('Choose a new output directory and paths without commas or colons')
    build = json.loads((kernel / 'build.json').read_text())
    if build.get('status') != 'compiled' or build.get('qemu_transports') is not True:
        raise ValueError('Requires a compiled QEMU transport kernel; physical builds are refused')
    expected = {item['path']: item['sha256'] for item in build['artifacts']}
    config_name = '.config' if '.config' in expected else 'resolved.config'
    for name in ('Image', config_name):
        if (kernel / name).is_symlink() or digest(kernel / name) != expected.get(name):
            raise ValueError(f'Kernel artifact mismatch: {name}')
    config = (kernel / config_name).read_text().splitlines()
    for name in ('CONFIG_SYNC_FILE', 'CONFIG_SW_SYNC', 'CONFIG_UDMABUF', 'CONFIG_DEBUG_FS'):
        if f'{name}=y' not in config:
            raise ValueError(f'Test requires built-in {name}')
    image = build['container_image']
    if not image.startswith('sha256:') or len(image) != 71 or any(c not in '0123456789abcdef' for c in image[7:]):
        raise ValueError('Kernel compiler image must be an immutable SHA-256 ID')
    module = kernel / 'dma_buf_resv_test.ko' if args.reservation_module else None
    if module and (args.expect_unsupported or module.is_symlink() or digest(module) != expected.get(module.name)):
        raise ValueError('Reservation module must match the compiled test kernel artifact')
    output.mkdir(parents=True)
    report = {'status': 'started', 'target': 'qemu-dma-buf-sync-file',
              'scope': 'real DMA-buffer and software-fence ioctls; no GPU or panel execution',
              'hardware_verified': False, 'flash_image': False, 'expect_unsupported': args.expect_unsupported,
              'kernel_build_sha256': digest(kernel / 'build.json'), 'kernel_sha256': expected['Image'],
              'fixture_sha256': digest(fixture), 'runner_sha256': digest(Path(__file__)),
              'compiler_image': image, 'reservation_module_sha256': digest(module) if module else None, 'commands': []}
    started = time.monotonic()

    def execute(command, log, timeout):
        report['commands'].append(command)
        with (output / log).open('wb') as stream:
            subprocess.run(command, stdin=subprocess.DEVNULL, stdout=stream, stderr=subprocess.STDOUT,
                           check=True, timeout=timeout)

    try:
        execute([args.engine, 'run', '--rm', '--network', 'none', '--read-only', '--cpus', '1',
                 '--memory', '256m', '--memory-swap', '256m', '--pids-limit', '64',
                 '--tmpfs', '/tmp:rw,nosuid,size=32m', '-v', f'{fixture}:/test.c:ro',
                 '-v', f'{output}:/output', '--entrypoint', 'gcc', image,
                 '-static', '-O2', '-Wall', '-Wextra', '-Werror', '/test.c', '-o', '/output/init'], 'build.log', 60)
        (output / 'initramfs.cpio').write_bytes(initramfs((output / 'init').read_bytes(), module.read_bytes() if module else None))
        mode = 'unsupported' if args.expect_unsupported else '1'
        execute([args.qemu, '-name', 'Armada DMA-buffer sync test', '-machine', 'virt', '-accel', args.accel,
                 '-cpu', 'host' if args.accel in ('hvf', 'kvm') else 'max', '-smp', '2', '-m', '1024',
                 '-nodefaults', '-nographic', '-monitor', 'none', '-serial', 'stdio', '-no-reboot',
                 '-nic', 'none', '-kernel', str(kernel / 'Image'), '-initrd', str(output / 'initramfs.cpio'),
                 '-append', f'console=ttyAMA0 earlycon rdinit=/init selinux=0 audit=0 armada.dmabuf_test={mode}'],
                'boot.log', 45)
        log = (output / 'boot.log').read_text(errors='replace')
        cases = (['BASELINE_UNSUPPORTED'] if args.expect_unsupported else
                 ['EMPTY_AND_ABI', 'MAPPED_LIFETIME_256', 'INVALID_AND_COPYBACK', 'WRITERS', 'READ_WRITE_SEPARATION',
                  'SNAPSHOT', 'CROSS_PROCESS', '1024_IMPORTS', 'FD_EXHAUSTION', 'POLL_WRITER', 'ALL'])
        if module:
            cases.append('RESERVATION_MODULE')
            if any(f'DMA_BUF_RESV_{name}_PASS' not in log for name in ('WAIT_ALL', 'TEST_SIGNALED')):
                raise RuntimeError('Missing kernel reservation acceptance')
            if digest(module) != report['reservation_module_sha256']:
                raise ValueError('Reservation module changed during test')
        if any(f'DMA_BUF_SYNC_{name}_PASS' not in log for name in cases) or 'reboot: Power down' not in log:
            raise RuntimeError('Missing test acceptance or clean power-off; inspect boot.log')
        if any(marker in log for marker in ('DMA_BUF_SYNC_FAIL', 'DMA_BUF_RESV_FAIL', 'WARNING:', 'BUG:', 'Kernel panic', 'Oops:')):
            raise RuntimeError('Test assertion or kernel failure; inspect boot.log')
        if digest(kernel / 'Image') != expected['Image'] or digest(kernel / 'build.json') != report['kernel_build_sha256']:
            raise ValueError('Kernel input changed during test')
        if digest(fixture) != report['fixture_sha256'] or digest(Path(__file__)) != report['runner_sha256']:
            raise ValueError('Test source changed during execution')
        report.update(status='passed', cases=cases)
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        report.update(status='failed', error=str(error))
        raise
    finally:
        report['artifacts_sha256'] = {p.name: digest(p) for p in sorted(output.iterdir()) if p.is_file() and p.name != 'result.json'}
        report['elapsed_seconds'] = round(time.monotonic() - started, 3)
        (output / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
    print(output / 'result.json')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('kernel', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--reservation-module', action='store_true', help="Also load the kernel build record's reservation test module")
    parser.add_argument('--expect-unsupported', action='store_true', help='Confirm an unpatched baseline rejects both ioctls')
    parser.add_argument('--engine', default=os.environ.get('CONTAINER_ENGINE', 'docker'))
    parser.add_argument('--qemu', default='qemu-system-aarch64')
    parser.add_argument('--accel', choices=('hvf', 'kvm', 'tcg'),
                        default='hvf' if platform.system() == 'Darwin' and platform.machine() == 'arm64' else 'tcg')
    try:
        run(parser.parse_args())
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        parser.exit(1, f'{error}\n')
