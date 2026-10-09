#!/usr/bin/env python3
"""Test Android container kernel interfaces in diskless QEMU, without Docker."""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]


def load(name, filename):
    spec = importlib.util.spec_from_file_location(name, ROOT / 'tools' / filename)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


builder = load('android_kernel_inputs', 'build-headset-initramfs.py')
archive = load('android_initramfs', 'test-dma-buf-sync.py')

REQUIRED = ('ANDROID_BINDER_IPC', 'ANDROID_BINDERFS', 'USER_NS', 'PID_NS', 'IPC_NS', 'UTS_NS',
            'NET_NS', 'NAMESPACES', 'MEMFD_CREATE', 'SECCOMP', 'SECCOMP_FILTER',
            'DEVTMPFS', 'PROC_FS', 'TMPFS', 'BINFMT_ELF')


def kernel_inputs(kernel):
    build, hashes, _, _ = builder.inputs(kernel)
    if build.get('qemu_transports') is not True:
        raise ValueError('Requires a QEMU transport kernel; physical builds are refused')
    config = set((kernel / 'resolved.config').read_text().splitlines())
    for name in REQUIRED:
        if f'CONFIG_{name}=y' not in config:
            raise ValueError(f'Test requires built-in CONFIG_{name}')
    return build, hashes


def check_log(log):
    markers = ['ANDROID_PIDNS_PASS container_pid=1', 'ANDROID_USERNS_PASS host_uid=1000 container_uid=0', 'ANDROID_BINDER_ISOLATION_PASS',
               'ANDROID_BINDER_TRANSACTION_PASS request=1 reply=1 fd=1 sender_identity=1',
               'ANDROID_BINDER_POLLFREE_PASS iterations=32', 'ANDROID_MEMFD_PASS', 'ANDROID_SECCOMP_PASS', 'ANDROID_CONTAINER_CHILD_PASS',
               'ANDROID_CONTAINER_KERNEL_PASS', 'reboot: Power down']
    markers += [f'ANDROID_BINDER_PASS name={name} protocol=8 independent_contexts=2'
                for name in ('anbox-binder', 'anbox-hwbinder', 'anbox-vndbinder')]
    if any(log.count(marker) != 1 for marker in markers):
        raise RuntimeError('Missing or repeated kernel acceptance markers; inspect boot.log')
    if any(marker in log for marker in ('ANDROID_CONTAINER_FAIL', 'ANDROID_CONTAINER_KERNEL_FAIL',
                                       'WARNING:', 'BUG:', 'Kernel panic', 'Oops:')):
        raise RuntimeError('Kernel or test failure; inspect boot.log')
    overlay_pass = log.count('ANDROID_ROOTLESS_OVERLAY_PASS')
    overlay_unavailable = log.count('ANDROID_ROOTLESS_OVERLAY_UNAVAILABLE errno=')
    if overlay_pass + overlay_unavailable != 1:
        raise RuntimeError('Missing or inconsistent rootless OverlayFS result')
    return {'rootless_binderfs': True, 'independent_binder_contexts': 6,
            'binder_transaction_and_fd_passing': True, 'binder_pollfree_iterations': 32,
            'pid_namespace_and_procfs': True, 'memfd_sharing_and_seals': True, 'seccomp_filter': True,
            'native_rootless_overlayfs': bool(overlay_pass)}


def run(args):
    kernel, output = args.kernel.absolute(), args.output.absolute()
    fixture = ROOT / 'tests/android-container/init.c'
    if output.exists() or output.is_symlink():
        raise ValueError('Choose a new output directory; existing evidence is preserved')
    _, hashes = kernel_inputs(kernel)
    if shutil.disk_usage(output.parent if output.parent.exists() else ROOT).free < 2 * 1024**3:
        raise ValueError('Keep at least 2 GiB free for the bounded compiler and VM test')
    output.mkdir(parents=True)
    report = {'status': 'started', 'scope': 'Android kernel ABI in a diskless QEMU guest',
              'lepton_runtime_verified': False, 'hardware_verified': False, 'flash_image': False,
              'kernel_build_sha256': builder.digest(kernel / 'build.json'),
              'kernel_inputs_sha256': hashes, 'fixture_sha256': builder.digest(fixture),
              'runner_sha256': builder.digest(Path(__file__)), 'commands': []}
    started = time.monotonic()
    env = {**os.environ, 'ZIG_GLOBAL_CACHE_DIR': str(output / 'zig-cache'),
           'ZIG_LOCAL_CACHE_DIR': str(output / 'zig-local')}

    def execute(command, name, timeout):
        report['commands'].append(command)
        with (output / name).open('wb') as stream:
            subprocess.run(command, env=env, stdin=subprocess.DEVNULL, stdout=stream,
                           stderr=subprocess.STDOUT, timeout=timeout, check=True)

    try:
        shutil.copyfile(fixture, output / 'fixture.c')
        shutil.copyfile(Path(__file__), output / 'runner.py')
        report['zig_version'] = subprocess.check_output([args.zig, 'version'], env=env, text=True, timeout=10).strip()
        execute([args.zig, 'cc', '-target', 'aarch64-linux-musl', '-static', '-O2', '-Wall', '-Wextra',
                 '-Werror', str(fixture), '-o', str(output / 'init')], 'build.log', 120)
        binary = (output / 'init').read_bytes()
        if binary[:6] != b'\x7fELF\x02\x01' or binary[18:20] != b'\xb7\x00':
            raise ValueError('Compiler did not produce a little-endian ARM64 ELF')
        (output / 'initramfs.cpio').write_bytes(archive.initramfs(binary))
        execute([args.qemu, '-name', 'Armada Android container ABI', '-machine', 'virt', '-accel', args.accel,
                 '-cpu', 'host' if args.accel in ('hvf', 'kvm') else 'max', '-smp', '2', '-m', '512',
                 '-nodefaults', '-nographic', '-monitor', 'none', '-serial', 'stdio', '-no-reboot',
                 '-nic', 'none', '-kernel', str(kernel / 'Image'), '-initrd', str(output / 'initramfs.cpio'),
                 '-append', 'console=ttyAMA0 rdinit=/init selinux=0 audit=0 panic=-1 armada.android_container_test=1'],
                'boot.log', 45)
        report['capabilities'] = check_log((output / 'boot.log').read_text(errors='replace'))
        if any(builder.digest(kernel / name) != sha for name, sha in hashes.items()):
            raise ValueError('Kernel input changed during execution')
        for path, expected in ((kernel / 'build.json', report['kernel_build_sha256']),
                               (fixture, report['fixture_sha256']), (Path(__file__), report['runner_sha256'])):
            if builder.digest(path) != expected:
                raise ValueError(f'Test input changed during execution: {path}')
        report['integration_blockers'] = ([] if report['capabilities']['native_rootless_overlayfs'] else
                                          ['native_rootless_overlayfs_unavailable'])
        report['status'] = 'passed'
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        report.update(status='failed', error=str(error))
        raise
    finally:
        report['artifacts_sha256'] = {p.name: builder.digest(p) for p in sorted(output.iterdir())
                                     if p.is_file() and p.name != 'result.json'}
        report['elapsed_seconds'] = round(time.monotonic() - started, 3)
        (output / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
    print(output / 'result.json')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('kernel', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--zig', default='zig')
    parser.add_argument('--qemu', default='qemu-system-aarch64')
    parser.add_argument('--accel', choices=('hvf', 'kvm', 'tcg'),
                        default='hvf' if platform.system() == 'Darwin' and platform.machine() == 'arm64' else 'tcg')
    try:
        run(parser.parse_args())
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        parser.exit(1, f'{error}\n')
