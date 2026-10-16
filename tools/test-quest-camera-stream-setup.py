#!/usr/bin/env python3
"""Exercise Quest stream setup and teardown excerpts with modeled memory and ISP calls."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tarfile

ROOT = Path(__file__).resolve().parents[1]
BASE = 'techpack/camera/drivers/cam_isp/cam_isp_context'
UTIL = 'techpack/camera/drivers/cam_utils/cam_packet_util'
PATCH = ROOT / 'patches/kernel/linux-userspace/0008-quest-camera-stream-setup-cleanup.patch'


def sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def section(text, start):
    offset = text.index(start)
    brace = text.index('{', offset)
    depth = 1
    end = brace + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    if text[end:end+1] == ';':
        end += 1
    return text[offset:end] + '\n'


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('archive', type=Path)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--cc', default='cc')
    args = p.parse_args()
    profile = json.loads((ROOT / 'profiles/kernel/quest3.json').read_text())
    archive_sha = sha(args.archive)
    if archive_sha != profile['full_source']['sha256']:
        raise ValueError('Expected the pinned Quest kernel archive')
    out = args.output.resolve(); out.mkdir(parents=True, exist_ok=False)
    harness = ROOT / 'tests/quest-camera-stream-setup.c'
    inputs = {str(path.relative_to(ROOT)): sha(path) for path in [Path(__file__), PATCH, harness]}
    report = {'status': 'started', 'inputs': inputs, 'archive_sha256': archive_sha,
              'kernel_commit': profile['commit'], 'runs': {}, 'device_actions': [],
              'physical_capture_verified': False, 'dma_quiescence_verified': False,
              'scope': 'Actual stream setup, stream teardown excerpt and KMD parser; allocation, memory references, lists and hardware callbacks modeled'}
    try:
        names = [BASE + '.c', BASE + '.h', UTIL + '.c', UTIL + '.h', 'techpack/camera/include/uapi/media/cam_defs.h']
        prefix = profile['full_source']['archive_root'] + '/'
        found = set()
        with tarfile.open(args.archive) as archive:
            for entry in archive:
                if not entry.name.startswith(prefix) or entry.name[len(prefix):] not in names:
                    continue
                name = entry.name[len(prefix):]
                if name in found or not entry.isfile() or entry.size > 4 * 1024**2:
                    raise ValueError('Unexpected source entry')
                found.add(name)
                path = out / name; path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(archive.extractfile(entry).read())
        if found != set(names):
            raise ValueError('Missing source')
        report['source_sha256'] = {n: sha(out / n) for n in names}
        shutil.copyfile(harness, out / 'test.c')
        api = (out / names[-1]).read_text()
        (out / 'api.inc').write_text(''.join(section(api, 'struct ' + name + ' {') for name in
            ['cam_packet_header', 'cam_packet', 'cam_cmd_buf_desc', 'cam_stream_image', 'cam_set_stream_mode']))
        (out / 'kmd.inc').write_text(section((out / (UTIL + '.h')).read_text(), 'struct cam_kmd_buf_info {'))
        util = (out / (UTIL + '.c')).read_text()
        functions = ['int cam_packet_util_get_cmd_mem_addr(', 'void cam_packet_util_put_cmd_mem_addr(',
                     'int cam_packet_util_validate_cmd_desc(', 'int cam_packet_util_get_kmd_buffer(']
        (out / 'packet-functions.inc').write_text(''.join(section(util, n) for n in functions))
        env = dict(os.environ, UBSAN_OPTIONS='halt_on_error=1', ASAN_OPTIONS='detect_leaks=' + str(int(sys.platform == 'linux')))
        for name, defects in [('original', 32520), ('patched', 0)]:
            if name == 'patched':
                result = subprocess.run(['patch', '--batch', '--fuzz=0', '-p1', '-i', str(PATCH)], cwd=out,
                                        capture_output=True, text=True, timeout=15)
                (out / 'patch.log').write_text(result.stdout + result.stderr); result.check_returncode()
            header = (out / (BASE + '.h')).read_text()
            (out / 'image.inc').write_text(section(header, 'struct cam_isp_stream_image {'))
            source = (out / (BASE + '.c')).read_text()
            setup = section(source, 'static int __cam_isp_ctx_set_stream_mode_in_acquired(')
            start = source.index('\tfor (i = 0; i < ctx_isp->num_stream_images; i++)')
            end = source.index('\n\ttrace_cam_context_state(', start)
            teardown = 'static void release_excerpt(struct cam_isp_context *ctx_isp)\n{\n\tuint32_t i;\n' + source[start:end] + '\n}\n'
            (out / 'functions.inc').write_text(setup + teardown)
            command = [args.cc, '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-Wno-type-limits',
                       '-fsanitize=address,undefined', '-fno-sanitize-recover=all', str(out / 'test.c'), '-o', str(out / name)]
            result = subprocess.run(command, capture_output=True, text=True, timeout=60)
            (out / (name + '-compile.log')).write_text(result.stdout + result.stderr); result.check_returncode()
            result = subprocess.run([str(out / name)], capture_output=True, text=True, env=env, timeout=30)
            (out / (name + '.log')).write_text(result.stdout + result.stderr)
            match = re.fullmatch(r'QUEST_CAMERA_STREAM_SETUP cycles=(\d+) cases=(\d+) defects=(\d+)\n', result.stdout)
            if not match or int(match[1]) != 8430 or int(match[2]) < 900000 or int(match[3]) != defects or result.returncode != bool(defects) or result.stderr:
                raise ValueError('Unexpected ' + name + ' regression result')
            report['runs'][name] = {'cycles': int(match[1]), 'cases': int(match[2]), 'defects': defects,
                                    'binary_sha256': sha(out / name), 'command': command}
        if inputs != {n: sha(ROOT / n) for n in inputs}:
            raise ValueError('Source changed during verification')
        report['status'] = 'passed'
    except Exception as error:
        report.update(status='failed', error=str(error)); raise
    finally:
        (out / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
    print(out / 'result.json')


if __name__ == '__main__':
    main()
