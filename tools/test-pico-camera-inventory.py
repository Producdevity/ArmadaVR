#!/usr/bin/env python3
"""Verify the media collector's ABI against pinned Pico headers in offline ARM64 Linux."""
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
          'tests/test_pico_camera_inventory.py', 'tests/pico-camera-inventory-abi.c')


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def run(args):
    if sys.platform != 'linux' or platform.machine() != 'aarch64':
        raise ValueError('Use offline ARM64 Linux')
    profile = json.loads((ROOT / 'profiles/kernel/pico-neo3.json').read_text())
    archive_sha = digest(args.archive)
    if archive_sha != profile['full_source']['sha256']:
        raise ValueError('Kernel archive differs from the pinned Pico source')
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    report = {'status': 'started', 'kernel_commit': profile['commit'], 'archive_sha256': archive_sha,
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
    run(parser.parse_args())
