#!/usr/bin/env python3
"""Test Quest camera packets against the pinned UAPI and kernel parser without devices."""
import argparse
import hashlib
import json
from pathlib import Path
import platform
import subprocess
import sys
import tarfile

ROOT = Path(__file__).resolve().parents[1]
INPUTS = ('src/quest-camera-packet.c', 'src/quest-camera-packet.h', 'src/quest-camera-isp.h',
          'tests/quest-camera-packet.c', 'tools/test-quest-camera-packet.py')


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
    report = {'status': 'started', 'archive_sha256': archive_sha, 'inputs': inputs,
              'kernel_commit': profile['commit'], 'device_actions': [],
              'physical_capture_verified': False, 'commands': []}
    try:
        include = out / 'include'
        (include / 'media').mkdir(parents=True)
        prefix = profile['full_source']['archive_root'] + '/techpack/camera/'
        targets = {'drivers/cam_utils/cam_packet_util.c', 'drivers/cam_utils/cam_packet_util.h'}
        sources = {}
        with tarfile.open(args.archive) as archive:
            for member in archive:
                if not member.isfile() or not member.name.startswith(prefix):
                    continue
                relative = member.name[len(prefix):]
                header = relative.startswith('include/uapi/media/')
                if not header and relative not in targets:
                    continue
                if member.size > 1024 * 1024 or (header and relative != 'include/uapi/media/' + Path(relative).name):
                    raise ValueError('Unexpected source entry')
                content = archive.extractfile(member).read()
                if header:
                    (include / 'media' / Path(relative).name).write_bytes(content)
                else:
                    (out / Path(relative).name).write_bytes(content)
                    sources[relative] = hashlib.sha256(content).hexdigest()
        if set(sources) != targets:
            raise ValueError('Missing kernel sources')
        source = (out / 'cam_packet_util.c').read_text()
        header = (out / 'cam_packet_util.h').read_text()
        start = header.index('struct cam_kmd_buf_info {')
        shim = header[start:header.index('\n};', start) + 3] + '\n'
        names = ['cam_packet_util_get_cmd_mem_addr', 'cam_packet_util_put_cmd_mem_addr',
                 'cam_packet_util_validate_cmd_desc', 'cam_packet_util_validate_packet',
                 'cam_packet_util_get_kmd_buffer']
        for name in names:
            token = ('void ' if name == names[1] else 'int ') + name + '('
            start = source.index(token)
            shim += source[start:source.index('\n}', start) + 2] + '\n'
        (include / 'quest-camera-kernel.inc').write_text(shim)
        report.update(kernel_sources_sha256=sources, kernel_functions=names,
                      kernel_shim_sha256=digest(include / 'quest-camera-kernel.inc'),
                      headers_sha256={p.name: digest(p) for p in sorted((include / 'media').iterdir())})
        if args.library:
            library = args.library.resolve(strict=True)
            report['installed_library_sha256'] = digest(library)
            implementation = [str(library), '-Wl,-rpath,' + str(library.parent)]
        else:
            implementation = [str(ROOT / 'src/quest-camera-packet.c')]
        binary = out / 'packet'
        commands = [(['cc', '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                     '-Wno-type-limits', '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                     '-D__user=', '-DQUEST_CAMERA_VENDOR_TEST', '-DQUEST_CAMERA_KERNEL_TEST',
                     '-I', str(include), '-I', str(ROOT / 'src'), *implementation,
                     str(ROOT / 'tests/quest-camera-packet.c'), '-o', str(binary)], 'compile.log'),
                    ([str(binary)], 'run.log')]
        for command, log in commands:
            report['commands'].append(command)
            result = subprocess.run(command, capture_output=True, text=True, timeout=60)
            (out / log).write_text(result.stdout + result.stderr)
            if result.returncode:
                raise ValueError(log + ' failed')
        if 'QUEST_CAMERA_PACKET_PASS' not in (out / 'run.log').read_text():
            raise ValueError('Packet checks did not complete')
        if inputs != {name: digest(ROOT / name) for name in INPUTS}:
            raise ValueError('Source changed during test')
        if args.library and digest(library) != report['installed_library_sha256']:
            raise ValueError('Installed library changed during test')
        report.update(status='passed', binary_sha256=digest(binary), result=(out / 'run.log').read_text().strip())
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
    parser.add_argument('--library', type=Path, help='Test a built Quest DSO instead of source')
    run(parser.parse_args())
