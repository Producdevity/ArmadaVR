#!/usr/bin/env python3
"""Check Pico camera/IMU delivery against the pinned Monado frame and SLAM interfaces."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tarfile

ROOT = Path(__file__).resolve().parents[1]


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('archive', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--cc', default='cc')
    args = parser.parse_args()
    profile = json.loads((ROOT / 'profiles/monado.json').read_text())
    if digest(args.archive) != profile['full_source']['sha256']:
        raise ValueError('Monado archive differs from the pinned revision')
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    with tarfile.open(args.archive) as stream:
        stream.extractall(output / 'source', filter='data')
    source = output / 'source' / profile['full_source']['archive_root']
    include = source / 'src/xrt/include'
    generated = output / 'include/xrt'
    generated.mkdir(parents=True)
    for name in ('xrt_config_build.h', 'xrt_config_have.h'):
        template = (include / ('xrt/' + name + '.cmake_in')).read_text()
        (generated / name).write_text(re.sub(r'^#cmakedefine .+$', '', template, flags=re.MULTILINE))
    paths = ['tests/pico-slam-input.c', 'src/pico-slam-input.c', 'src/pico-camera.c']
    command = [args.cc, '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
               '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
               '-I', str(ROOT / 'src'), '-I', str(output / 'include'), '-isystem', str(include),
               *[str(ROOT / name) for name in paths], '-o', str(output / 'pico-slam-input')]
    with (output / 'build.log').open('wb') as log:
        subprocess.run(command, check=True, stdout=log, stderr=subprocess.STDOUT, timeout=60)
    with (output / 'test.log').open('wb') as log:
        subprocess.run([str(output / 'pico-slam-input')], check=True, stdout=log, stderr=subprocess.STDOUT, timeout=30)
    if 'PICO_SLAM_INPUT_PASS' not in (output / 'test.log').read_text():
        raise ValueError('Missing successful input checks')
    inputs = paths + ['src/pico-camera.h', 'src/pico-slam-input.h', 'tools/test-pico-slam-input.py']
    report = {'status': 'passed', 'monado_commit': profile['commit'], 'archive_sha256': digest(args.archive),
              'command': command, 'sources': {name: digest(ROOT / name) for name in inputs},
              'binary_sha256': digest(output / 'pico-slam-input'),
              'result': (output / 'test.log').read_text().strip(), 'physical_tracking_verified': False,
              'scope': 'Owned synthetic camera frames and calibrated synthetic IMU vectors delivered to test SLAM sinks using real Monado interfaces. No estimator, calibration or device activation.'}
    (output / 'acceptance.json').write_text(json.dumps(report, indent=2) + '\n')
    print(report['result'])


if __name__ == '__main__':
    main()
