#!/usr/bin/env python3
"""Verify the Pico input adapter against headers from the pinned Monado archive."""
import argparse
import hashlib
import json
import os
from pathlib import Path
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
        raise ValueError('Monado source archive differs from the pinned revision')
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    archive_root = profile['full_source']['archive_root']
    with tarfile.open(args.archive) as stream:
        members = [m for m in stream.getmembers() if m.name.startswith(archive_root + '/src/xrt/include/')]
        if not members or any(not m.isfile() and not m.isdir() for m in members):
            raise ValueError('Unexpected Monado header archive')
        stream.extractall(output / 'source', members=members, filter='data')
    include = output / 'source' / archive_root / 'src/xrt/include'
    binary = output / 'pico-monado-input'
    command = [args.cc, '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
               '-fsanitize=address,undefined', '-I', str(ROOT / 'src'), '-isystem', str(include),
               str(ROOT / 'tests/pico-monado-input.c'), str(ROOT / 'src/pico-monado-input.c'),
               '-o', str(binary)]
    with (output / 'compile.log').open('wb') as log:
        subprocess.run(command, check=True, stdout=log, stderr=subprocess.STDOUT, timeout=60)
    env = dict(os.environ, UBSAN_OPTIONS='halt_on_error=1')
    result = subprocess.run([str(binary)], env=env, check=True, capture_output=True, text=True, timeout=15)
    (output / 'tests.log').write_text(result.stdout + result.stderr)
    if 'PICO_MONADO_INPUT_PASS checks=6912' not in result.stdout:
        raise ValueError('Pico Monado input tests did not complete')
    names = ['tests/pico-monado-input.c', 'src/pico-monado-input.c', 'src/pico-monado-input.h',
             'src/pico-controller-stream.h', 'src/pico-controller.h', 'src/ssc-clock.h']
    report = {'status': 'passed', 'monado_commit': profile['commit'], 'archive_sha256': digest(args.archive),
              'inputs_sha256': {name: digest(ROOT / name) for name in names}, 'command': command,
              'scope': 'Actual Monado input ABI with synthetic calibration; no device or runtime registration proof',
              'hardware_verified': False}
    (output / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
    print(result.stdout, end='')


if __name__ == '__main__':
    main()
