#!/usr/bin/env python3
"""Check Quest capability queries and request transport against the pinned vendor UAPI without devices."""
import argparse
import hashlib
import json
from pathlib import Path
import platform
import subprocess
import sys
import tarfile

ROOT = Path(__file__).resolve().parents[1]
INPUTS = ('src/quest-camera-control.c', 'src/quest-camera-control.h',
          'tests/quest-camera-control.c', 'tests/quest-camera-request.c', 'tools/test-quest-camera-control.py')


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def run(args):
    if sys.platform != 'linux' or platform.machine() != 'aarch64':
        raise ValueError('Use offline ARM64 Linux')
    profile = json.loads((ROOT / 'profiles/kernel/quest3.json').read_text())
    archive_sha = digest(args.archive)
    if archive_sha != profile['full_source']['sha256']:
        raise ValueError('Quest archive differs from pinned source')
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    inputs = {name: digest(ROOT / name) for name in INPUTS}
    report = {'status': 'started', 'archive_sha256': archive_sha, 'kernel_commit': profile['commit'],
              'inputs': inputs, 'device_actions': [], 'physical_capture_verified': False, 'commands': []}
    try:
        header = out / 'include/media'
        header.mkdir(parents=True)
        with tarfile.open(args.archive) as archive:
            prefix = profile['full_source']['archive_root'] + '/techpack/camera/include/uapi/media/'
            for member in archive:
                if member.isfile() and member.name.startswith(prefix):
                    if member.name != prefix + Path(member.name).name or member.size > 1024 * 1024:
                        raise ValueError('Unexpected camera header')
                    (header / Path(member.name).name).write_bytes(archive.extractfile(member).read())
        report['headers'] = {p.name: digest(p) for p in sorted(header.iterdir())}
        binaries = {}
        for name, marker in [('control', 'QUEST_CAMERA_QUERY_PASS ioctls=30 hardware=0 isp_entries=8'),
                             ('request', 'QUEST_CAMERA_REQUEST_PASS ioctls=282 cases=311 hardware=0 preserved_failure_handles=1')]:
            binary = out / name
            commands = [(['cc', '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                         '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-D__user=',
                         '-I', str(out / 'include'), '-I', str(ROOT / 'src'),
                         str(ROOT / 'src/quest-camera-control.c'), str(ROOT / ('tests/quest-camera-' + name + '.c')),
                         '-o', str(binary)], name + '-compile.log'), ([str(binary)], name + '-run.log')]
            for command, log in commands:
                report['commands'].append(command)
                p = subprocess.run(command, capture_output=True, text=True, timeout=60)
                (out / log).write_text(p.stdout + p.stderr)
                if p.returncode:
                    raise ValueError(log + ' failed')
            if marker not in (out / (name + '-run.log')).read_text():
                raise ValueError(name + ' checks did not complete')
            binaries[name] = digest(binary)
        if inputs != {name: digest(ROOT / name) for name in INPUTS}:
            raise ValueError('Source changed during test')
        report.update(status='passed', binaries_sha256=binaries)
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
