#!/usr/bin/env python3
"""Test Lepton's three Podman overlay views on a verified QEMU Quest kernel."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import tarfile
import time

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('android_kernel', ROOT / 'tools/test-android-container.py')
android = importlib.util.module_from_spec(spec)
spec.loader.exec_module(android)
FIXTURES = ('mount-init.c', 'mount-root.sh', 'mount-user.sh', 'Containerfile')
MARKERS = ('LEPTON_MOUNT_ROOTLESS_PASS host_uid=1000', 'LEPTON_MOUNT_FIRST_PASS',
           'LEPTON_MOUNT_CONTEXT_ISOLATION_PASS', 'LEPTON_MOUNT_RESTART_PASS',
           'LEPTON_MOUNT_LOWER_UNCHANGED_PASS', 'LEPTON_MOUNT_LIFECYCLE_PASS',
           'LEPTON_MOUNT_VM_PASS', 'reboot: Power down')


def digest(path):
    return android.builder.digest(path)


def check_log(log):
    if any(log.count(marker) != 1 for marker in MARKERS):
        raise RuntimeError('Missing or repeated Podman mount acceptance; inspect boot.log')
    if any(marker in log for marker in ('LEPTON_MOUNT_VM_FAIL', 'WARNING:', 'BUG:', 'Kernel panic', 'Oops:')):
        raise RuntimeError('Guest or kernel failure; inspect boot.log')
    return {'rootless_podman': True, 'fuse_rootfs_and_explicit_overlays': 3,
            'persistent_copy_up_and_whiteouts': True, 'independent_contexts': 2,
            'read_only_rootfs': True, 'lower_inputs_unchanged': True,
            'restart_persistence': True, 'bounded_stop_and_mount_cleanup': True}


def validate_archive(path):
    with tarfile.open(path) as archive:
        entries = archive.getmembers()
        if sum(entry.size for entry in entries) > 192 * 1024**2 or len(entries) > 20000:
            raise ValueError('Dependency root exceeds the bounded guest filesystem')
        for entry in entries:
            parts = Path(entry.name).parts
            if entry.name.startswith('/') or '..' in parts or not parts:
                raise ValueError('Invalid dependency archive path')
            if not (entry.isfile() or entry.isdir() or entry.issym() or entry.islnk()):
                raise ValueError('Unsupported dependency archive entry')
        for name in ('bin/busybox', 'usr/bin/podman', 'usr/bin/fuse-overlayfs',
                     'usr/bin/crun', 'usr/bin/newuidmap', 'usr/bin/newgidmap', 'packages.txt'):
            try:
                entry = archive.getmember(name)
            except KeyError as error:
                raise ValueError(f'Missing dependency: {name}') from error
            if not entry.isfile():
                raise ValueError(f'Expected a regular dependency: {name}')
        return archive.extractfile('packages.txt').read().decode()


def run(args):
    kernel, output = args.kernel.absolute(), args.output.absolute()
    if output.exists() or output.is_symlink() or any(c in str(p) for p in (kernel, output) for c in (':', ',')):
        raise ValueError('Choose a new output directory and paths without commas or colons')
    _, hashes = android.kernel_inputs(kernel)
    config = (kernel / 'resolved.config').read_text().splitlines()
    for name in ('FUSE_FS', 'EXT4_FS', 'CGROUPS', 'VIRTIO_BLK'):
        if f'CONFIG_{name}=y' not in config:
            raise ValueError(f'Test requires built-in CONFIG_{name}')
    if shutil.disk_usage(output.parent if output.parent.exists() else ROOT).free < 2 * 1024**3:
        raise ValueError('Keep at least 2 GiB free for the bounded test')
    image = json.loads(subprocess.check_output([args.engine, 'image', 'inspect', args.image], text=True, timeout=15))[0]
    if image['Architecture'] != 'arm64' or not re.fullmatch(r'sha256:[0-9a-f]{64}', image['Id']):
        raise ValueError('Requires an immutable ARM64 dependency image')
    output.mkdir(parents=True)
    sources = {name: ROOT / 'tests/android-container' / name for name in FIXTURES}
    report = {'status': 'started', 'scope': 'rootless Podman overlay lifecycle on a QEMU transport kernel',
              'android_boot_verified': False, 'lepton_runtime_verified': False,
              'hardware_verified': False, 'flash_image': False, 'cgroup_delegation_verified': False,
              'kernel_inputs_sha256': hashes, 'kernel_build_sha256': digest(kernel / 'build.json'),
              'dependency_image': image['Id'], 'fixture_sha256': {name: digest(path) for name, path in sources.items()},
              'runner_sha256': digest(Path(__file__)), 'commands': []}
    started = time.monotonic()
    env = {**os.environ, 'ZIG_GLOBAL_CACHE_DIR': str(output / 'zig-cache'), 'ZIG_LOCAL_CACHE_DIR': str(output / 'zig-local')}

    def execute(command, log, timeout=60):
        report['commands'].append(command)
        with (output / log).open('wb') as stream:
            subprocess.run(command, stdin=subprocess.DEVNULL, stdout=stream, stderr=subprocess.STDOUT,
                           env=env, timeout=timeout, check=True)

    try:
        for name, path in sources.items():
            shutil.copyfile(path, output / name)
        shutil.copyfile(Path(__file__), output / 'runner.py')
        report['zig_version'] = subprocess.check_output([args.zig, 'version'], env=env, text=True, timeout=10).strip()
        execute([args.zig, 'cc', '-target', 'aarch64-linux-musl', '-static', '-O2', '-Wall', '-Wextra', '-Werror',
                 str(sources['mount-init.c']), '-o', str(output / 'init')], 'build.log', 120)
        init = (output / 'init').read_bytes()
        if init[:6] != b'\x7fELF\x02\x01' or init[18:20] != b'\xb7\x00':
            raise ValueError('Compiler did not produce an ARM64 ELF')
        (output / 'initramfs.cpio').write_bytes(android.archive.initramfs(init))
        cid = subprocess.check_output([args.engine, 'create', '--network', 'none', image['Id']], text=True, timeout=15).strip()
        if not re.fullmatch(r'[0-9a-f]{64}', cid):
            raise ValueError('Unexpected dependency container identity')
        try:
            report['commands'].append([args.engine, 'export', cid])
            with (output / 'rootfs.tar.part').open('wb') as archive, (output / 'export.log').open('wb') as errors:
                subprocess.run([args.engine, 'export', cid], stdin=subprocess.DEVNULL, stdout=archive,
                               stderr=errors, timeout=60, check=True)
        finally:
            execute([args.engine, 'rm', cid], 'remove-container.log')
        (output / 'rootfs.tar.part').rename(output / 'rootfs.tar')
        report['packages'] = validate_archive(output / 'rootfs.tar').splitlines()
        execute([args.engine, 'run', '--rm', '--network', 'none', '--read-only', '--memory', '512m',
                 '--memory-swap', '512m', '--cpus', '1', '--pids-limit', '64', '--tmpfs', '/tmp:rw,size=256m',
                 '-v', f'{output}:/output', '--entrypoint', '/bin/sh', image['Id'], '-ec',
                 'mkdir /tmp/root; tar -xf /output/rootfs.tar -C /tmp/root; '
                 'cp /output/init /tmp/root/init; chmod 755 /tmp/root/init; '
                 'cp /output/mount-root.sh /output/mount-user.sh /tmp/root/; '
                 'truncate -s 256M /output/rootfs.ext4; '
                 'mke2fs -q -t ext4 -O ^orphan_file,^metadata_csum_seed -d /tmp/root /output/rootfs.ext4'], 'filesystem.log')
        report['rootfs_sha256'] = digest(output / 'rootfs.ext4')
        execute([args.qemu, '-name', 'Armada Android mount test', '-machine', 'virt', '-accel', args.accel,
                 '-cpu', 'host' if args.accel in ('hvf', 'kvm') else 'max', '-smp', '2', '-m', '1024',
                 '-nodefaults', '-nographic', '-monitor', 'none', '-serial', 'stdio', '-no-reboot', '-nic', 'none',
                 '-kernel', str(kernel / 'Image'), '-initrd', str(output / 'initramfs.cpio'),
                 '-drive', f'file={output}/rootfs.ext4,if=virtio,format=raw,snapshot=on',
                 '-append', 'console=ttyAMA0 rdinit=/init selinux=0 audit=0 panic=-1 armada.lepton_mount_test=1'], 'boot.log', 90)
        report['capabilities'] = check_log((output / 'boot.log').read_text(errors='replace'))
        if android.kernel_inputs(kernel)[1] != hashes or digest(kernel / 'build.json') != report['kernel_build_sha256']:
            raise ValueError('Kernel inputs changed during execution')
        if digest(output / 'rootfs.ext4') != report['rootfs_sha256']:
            raise ValueError('Base guest disk changed despite snapshot writes')
        if any(digest(path) != report['fixture_sha256'][name] for name, path in sources.items()) or digest(Path(__file__)) != report['runner_sha256']:
            raise ValueError('Test sources changed during execution')
        report['status'] = 'passed'
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
    parser.add_argument('--image', default='localhost/armada-vr:android-container-tools')
    parser.add_argument('--engine', default=os.environ.get('CONTAINER_ENGINE', 'docker'))
    parser.add_argument('--zig', default='zig')
    parser.add_argument('--qemu', default='qemu-system-aarch64')
    parser.add_argument('--accel', choices=('hvf', 'kvm', 'tcg'),
                        default='hvf' if platform.system() == 'Darwin' and platform.machine() == 'arm64' else 'tcg')
    try:
        run(parser.parse_args())
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        parser.exit(1, f'{error}\n')
