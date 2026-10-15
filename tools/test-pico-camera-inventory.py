#!/usr/bin/env python3
"""Verify the media collector against pinned headset headers in offline ARM64 Linux."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import platform
import subprocess
import sys
import tarfile

ROOT = Path(__file__).resolve().parents[1]
INPUTS = ('tools/pico-camera-inventory.py', 'tools/test-pico-camera-inventory.py',
          'tests/test_pico_camera_inventory.py', 'tests/pico-camera-inventory-abi.c', 'tests/quest-camera-inventory-abi.c')


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def run(args):
    if sys.platform != 'linux' or platform.machine() != 'aarch64':
        raise ValueError('Use offline ARM64 Linux')
    profile = json.loads((ROOT / 'profiles/kernel' / (args.target + '.json')).read_text())
    archive_sha = digest(args.archive)
    if archive_sha != profile['full_source']['sha256']:
        raise ValueError('Kernel archive differs from the pinned target source')
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    report = {'status': 'started', 'target': args.target, 'kernel_commit': profile['commit'], 'archive_sha256': archive_sha,
              'inputs_sha256': {name: digest(ROOT / name) for name in INPUTS},
              'physical_capture_verified': False, 'device_access': False, 'commands': []}

    def command(argv, log, expected=0):
        report['commands'].append(argv)
        result = subprocess.run(argv, cwd=ROOT, capture_output=True, text=True, timeout=60)
        (out / log).write_text(result.stdout + result.stderr)
        if result.returncode != expected:
            raise ValueError(log + ' failed; inspect the retained log')
        return result.stdout

    try:
        header = out / 'include/linux/media.h'
        header.parent.mkdir(parents=True)
        with tarfile.open(args.archive) as archive:
            name = profile['full_source']['archive_root'] + '/include/uapi/linux/media.h'
            header.write_bytes(archive.extractfile(name).read())
            if args.target == 'quest3':
                media = out / 'include/media'
                media.mkdir()
                for member in archive:
                    relative = member.name.removeprefix(profile['full_source']['archive_root'] + '/')
                    if member.isfile() and relative.startswith('techpack/camera/include/uapi/media/'):
                        if member.size > 1024 * 1024 or relative != 'techpack/camera/include/uapi/media/' + Path(relative).name:
                            raise ValueError('Unexpected camera header')
                        (media / Path(relative).name).write_bytes(archive.extractfile(member).read())
                report['camera_header_sha256'] = {path.name: digest(path) for path in media.iterdir()}
        report['media_header_sha256'] = digest(header)
        binary = out / 'media-abi'
        command(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-D__user=', '-I', str(out / 'include'),
                 str(ROOT / 'tests/pico-camera-inventory-abi.c'), '-o', str(binary)], 'build.log')
        abi = json.loads(command([str(binary)], 'abi.json'))
        spec = importlib.util.spec_from_file_location('inventory', ROOT / INPUTS[0])
        inventory = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(inventory)
        expected = {'device_info': inventory.DEVICE_INFO, 'get_topology': inventory.GET_TOPOLOGY,
                    'sizes': [256, inventory.TOPOLOGY.size, *(record.size for _, record, _ in inventory.RECORDS)],
                    'offsets': [120, 8, 16, 24, 32, 40, 48, 56, 64, 48, 12, 12]}
        if abi != expected:
            raise ValueError('Collector layout differs from pinned kernel headers')
        report['abi'] = abi
        if args.target == 'quest3':
            native = out / 'quest-abi'
            command(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-D__user=', '-I', str(out / 'include'),
                     str(ROOT / 'tests/quest-camera-inventory-abi.c'), '-o', str(native)], 'quest-build.log')
            quest = json.loads(command([str(native)], 'quest-abi.json'))
            expected = {'roles': list(range(0x10000, 0x10000 + len(inventory.QUEST_ROLES))),
                        'models': list(inventory.MODELS.values()), 'sync_role': 0x10000,
                        'control': 0xc01856c0, 'create_session': 0x10f, 'link': 0x111, 'schedule': 0x113,
                        'sizes': [24, 8, 268, 40, 16]}
            if quest != expected:
                raise ValueError('Quest inventory or command ABI differs from pinned headers')
            report['quest_abi'] = quest
            report['pico_control_opcodes_compatible'] = False
        command([sys.executable, '-B', '-m', 'unittest', 'discover', '-s', 'tests',
                 '-p', 'test_pico_camera_inventory.py', '-v'], 'tests.log')
        command([sys.executable, '-B', str(ROOT / INPUTS[0]), '--help'], 'help.log')
        command([sys.executable, '-B', str(ROOT / INPUTS[0]), '--media', '/dev/video0'], 'refusal.log', 1)
        if report['inputs_sha256'] != {name: digest(ROOT / name) for name in INPUTS}:
            raise ValueError('Source changed during verification')
        report['status'] = 'passed'
    except Exception as error:
        report.update(status='failed', error=str(error))
        raise
    finally:
        (out / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
    print(out / 'result.json')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('archive', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--target', choices=('pico-neo3', 'quest3'), default='pico-neo3')
    run(parser.parse_args())
