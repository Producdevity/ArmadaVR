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
INPUTS = ('src/quest-camera-group.c', 'src/quest-camera-group.h', 'tests/quest-camera-group.c',
          'src/quest-camera-pipeline.c', 'src/quest-camera-pipeline.h', 'tests/quest-camera-pipeline.c',
          'src/quest-camera-stream.c', 'src/quest-camera-stream.h', 'tests/quest-camera-stream.c',
          'src/quest-mcu-transaction.c', 'src/quest-mcu-transaction.h', 'tests/quest-mcu-transaction.c',
          'src/quest-camera-capture.c', 'src/quest-camera-capture.h', 'tests/quest-camera-capture.c',
          'src/quest-camera-metadata.c', 'src/quest-camera-metadata.h',
          'src/quest-camera-shutter.c', 'src/quest-camera-shutter.h',
          'src/quest-camera-pixels.c', 'src/quest-camera-pixels.h', 'src/quest-mcu-control.c', 'src/quest-mcu-control.h',
          'src/quest-camera-mcu.c', 'src/quest-camera-mcu.h', 'tests/quest-mcu-control.c',
          'src/quest-camera-control.c', 'src/quest-camera-control.h',
          'src/quest-camera-isp.c', 'src/quest-camera-isp.h', 'tests/quest-camera-isp.c',
          'src/quest-camera-memory.c', 'src/quest-camera-memory.h',
          'tests/quest-camera-memory.c', 'tests/quest-camera-acquire.c', 'src/quest-camera-buffer.c', 'src/quest-camera-buffer.h', 'tests/quest-camera-buffer.c',
          'tests/quest-camera-control.c', 'tests/quest-camera-request.c', 'tests/quest-camera-link.c', 'tools/test-quest-camera-control.py')


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
    if args.library:
        library = args.library.resolve(strict=True)
        report['installed_library_sha256'] = digest(library)
        report['installed_library_path'] = str(library)
    try:
        header = out / 'include/media'
        header.mkdir(parents=True)
        with tarfile.open(args.archive) as archive:
            prefix = profile['full_source']['archive_root'] + '/techpack/camera/include/uapi/media/'
            for member in archive:
                if member.name == profile['full_source']['archive_root'] + '/drivers/staging/oculus/include/uapi/linux/syncboss.h':
                    if not member.isfile() or member.size > 1024 * 1024:
                        raise ValueError('Unexpected Syncboss header')
                    path = out / 'include/syncboss.h'
                    path.write_bytes(archive.extractfile(member).read())
                    report['syncboss_header_sha256'] = digest(path)
                if member.isfile() and member.name.startswith(prefix):
                    if member.name != prefix + Path(member.name).name or member.size > 1024 * 1024:
                        raise ValueError('Unexpected camera header')
                    (header / Path(member.name).name).write_bytes(archive.extractfile(member).read())
        report['headers'] = {p.name: digest(p) for p in sorted(header.iterdir())}
        binaries = {}
        for name, marker in [('control', 'QUEST_CAMERA_QUERY_PASS ioctls=30 hardware=0 isp_entries=8'),
                             ('request', 'QUEST_CAMERA_REQUEST_PASS ioctls=282 cases=311 hardware=0 preserved_failure_handles=1'),
                             ('link', 'QUEST_CAMERA_LINK_PASS '),
                             ('acquire', 'QUEST_CAMERA_ACQUIRE_PASS ioctls=318 hardware=0'),
                             ('memory', 'QUEST_CAMERA_MEMORY_PASS ioctls=6558 hardware=0'),
                             ('isp', 'QUEST_CAMERA_ISP_PASS cases=1049227 ioctls=12 hardware=0'),
                             ('buffer', 'QUEST_CAMERA_BUFFER_PASS ioctls=266 nonblocking=1 failure_output_preserved=1 hardware=0'),
                             ('mcu-control', 'QUEST_MCU_CONTROL_PASS cases=66652 '),
                             ('capture', 'QUEST_CAMERA_CAPTURE_PASS '),
                             ('mcu-transaction', 'QUEST_MCU_TRANSACTION_PASS '),
                             ('stream', 'QUEST_CAMERA_STREAM_PASS '),
                             ('pipeline', 'QUEST_CAMERA_PIPELINE_PASS cycles=600 faults=80 '),
                             ('group', 'QUEST_CAMERA_GROUP_PASS cycles=416 faults=269 ')]:
            binary = out / name
            source_names = (['quest-camera-group.c', 'quest-camera-isp.c', 'quest-camera-control.c'] if name == 'group' else
                            ['quest-camera-pipeline.c', 'quest-camera-control.c'] if name == 'pipeline' else
                            ['quest-camera-capture.c', 'quest-camera-metadata.c', 'quest-camera-pixels.c', 'quest-camera-shutter.c'] if name == 'capture' else
                            ['quest-mcu-control.c', 'quest-camera-mcu.c'] if name == 'mcu-control' else
                            ['quest-mcu-control.c', 'quest-mcu-transaction.c'] if name == 'mcu-transaction' else
                            ['quest-camera-stream.c', 'quest-camera-shutter.c'] if name == 'stream' else
                            ['quest-camera-isp.c', 'quest-camera-control.c'] if name == 'isp' else
                            ['quest-camera-' + name + '.c' if name in ('buffer', 'memory') else 'quest-camera-control.c'])
            test_name = 'quest-' + name + '.c' if name.startswith('mcu-') else 'quest-camera-' + name + '.c'
            commands = [(['cc', '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                         '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-D__user=',
                         *(['-Wl,--export-dynamic'] if name in ('capture', 'stream') else []),
                         *(['-DQUEST_CAMERA_VENDOR_TEST'] if name == 'group' else []),
                         *(['-DQUEST_MCU_VENDOR_TEST'] if name in ('mcu-control', 'stream') else []),
                         *(['-DQUEST_CAMERA_STREAM_INSTALLED'] if name == 'stream' and args.library else []),
                         *([f'-D{system}=quest_test_{stub}' for system, stub in
                            [('open', 'open'), ('fstat', 'fstat'), ('ioctl', 'ioctl'),
                             ('read', 'read'), ('close', 'close'), ('clock_gettime', 'clock')]]
                           if name == 'stream' and not args.library else []),
                         '-I', str(out / 'include'), '-I', str(ROOT / 'src'),
                         *([str(library), '-Wl,-rpath,' + str(library.parent)] if args.library else
                           [str(ROOT / ('src/' + source)) for source in source_names]),
                         str(ROOT / 'tests' / test_name),
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
        if args.library and digest(library) != report['installed_library_sha256']:
            raise ValueError('Installed library changed during test')
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
    parser.add_argument('--library', type=Path, help='Test a built Quest DSO instead of compiling transport sources')
    run(parser.parse_args())
