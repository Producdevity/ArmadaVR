#!/usr/bin/env python3
"""Exercise actual Quest buffer completion/dequeue code with modeled lists and clocks."""
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
PATCH = ROOT / 'patches/kernel/linux-userspace/0007-quest-camera-buffer-timestamp.patch'


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
    if sha(args.archive) != profile['full_source']['sha256']:
        raise ValueError('Expected the pinned Quest kernel archive')
    out = args.output.resolve(); out.mkdir(parents=True, exist_ok=False)
    inputs = {str(path.relative_to(ROOT)): sha(path) for path in [Path(__file__), PATCH, ROOT / 'tests/quest-camera-buffer-time.c']}
    report = {'status': 'started', 'inputs': inputs, 'kernel_commit': profile['commit'], 'runs': {},
              'device_actions': [], 'physical_timing_verified': False, 'dma_quiescence_verified': False}
    try:
        names = [BASE + '.c', BASE + '.h', 'techpack/camera/include/uapi/media/cam_defs.h']
        with tarfile.open(args.archive) as archive:
            for name in names:
                entry = archive.getmember(profile['full_source']['archive_root'] + '/' + name)
                assert entry.isfile() and entry.size < 4 * 1024**2
                path = out / name; path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(archive.extractfile(entry).read())
        report['source_sha256'] = {n: sha(out / n) for n in names}
        shutil.copyfile(ROOT / 'tests/quest-camera-buffer-time.c', out / 'test.c')
        api = (out / names[-1]).read_text()
        (out / 'api.inc').write_text(''.join(section(api, 'struct ' + name + ' {') for name in
            ['cam_set_stream_mode_cmd_return', 'cam_set_stream_mode_frame_params', 'cam_set_stream_mode_cmd_get']))
        env = dict(os.environ, UBSAN_OPTIONS='halt_on_error=1', ASAN_OPTIONS='detect_leaks=' + str(int(sys.platform == 'linux')))
        for name, defects in [('original', 3000), ('patched', 0)]:
            if name == 'patched':
                result = subprocess.run(['patch', '--batch', '--fuzz=0', '-p1', '-i', str(PATCH)], cwd=out,
                                        capture_output=True, text=True, timeout=15)
                (out / 'patch.log').write_text(result.stdout + result.stderr); result.check_returncode()
            header = (out / (BASE + '.h')).read_text()
            (out / 'image.inc').write_text(section(header, 'struct cam_isp_stream_image {'))
            source = (out / (BASE + '.c')).read_text()
            functions = ['static int __cam_isp_ctx_rdi_stream_buf_done_in_activated_state(',
                         'static int __cam_isp_ctx_stream_mode_cmd_get_image(',
                         'static int __cam_isp_ctx_stream_mode_cmd_ret_image(']
            (out / 'functions.inc').write_text(''.join(section(source, n) for n in functions))
            command = [args.cc, '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
                       '-fsanitize=address,undefined', '-fno-sanitize-recover=all', str(out / 'test.c'), '-o', str(out / name)]
            result = subprocess.run(command, capture_output=True, text=True, timeout=60)
            (out / (name + '-compile.log')).write_text(result.stdout + result.stderr); result.check_returncode()
            result = subprocess.run([str(out / name)], capture_output=True, text=True, env=env, timeout=30)
            (out / (name + '.log')).write_text(result.stdout + result.stderr)
            match = re.fullmatch(r'QUEST_CAMERA_BUFFER_TIME cycles=1000 cases=(\d+) defects=(\d+)\n', result.stdout)
            if not match or int(match[1]) < 30000 or int(match[2]) != defects or result.returncode != bool(defects) or result.stderr:
                raise ValueError('Unexpected ' + name + ' regression result')
            report['runs'][name] = {'cases': int(match[1]), 'defects': defects, 'binary_sha256': sha(out / name), 'command': command}
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
