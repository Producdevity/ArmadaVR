#!/usr/bin/env python3
"""Build the two-sensor SSC session and check cancellation without QRTR hardware."""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import platform
import shlex
import subprocess
import sys
import tarfile

ROOT = Path(__file__).resolve().parents[1]


def run(args):
    if sys.platform != 'linux' or platform.machine() != 'aarch64':
        raise ValueError('Use an offline Linux ARM64 build environment')
    spec = importlib.util.spec_from_file_location('ssc', ROOT / 'tools/ssc-artifact.py')
    ssc = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(ssc)
    bundle = args.ssc_bundle.resolve()
    record = ssc.package(bundle)
    prefix = bundle / ssc.PREFIX
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    header = output / 'libssc-sensor.h'
    with tarfile.open(prefix / 'share/ssc/source.tar.gz') as archive:
        members = [m for m in archive if m.name.endswith('/src/libssc-sensor.h')]
        if len(members) != 1 or not members[0].isfile() or members[0].size > 65536:
            raise ValueError('Expected one pinned SSC header')
        header.write_bytes(archive.extractfile(members[0]).read())
    profile = json.loads((ROOT / 'profiles/ssc.json').read_text())
    if ssc.digest(header) != profile['test_source_sha256']['src/libssc-sensor.h']:
        raise ValueError('SSC header changed')
    sources = ['src/ssc-imu-session.c', 'src/ssc-imu-session.h', 'src/ssc-imu.c',
               'src/ssc-imu.h', 'src/ssc-clock.c', 'src/ssc-clock.h',
               'src/ssc-calibration.c', 'src/ssc-calibration.h', 'tests/ssc-imu-session.c']
    report = {'status': 'started', 'ssc_bundle': record, 'hardware_verified': False,
              'inputs_sha256': {n: ssc.digest(ROOT / n) for n in sources},
              'runner_sha256': ssc.digest(Path(__file__)), 'commands': []}
    pkg = shlex.split(subprocess.check_output(['pkg-config', '--cflags', '--libs', 'gio-2.0'], text=True))
    environment = dict(os.environ, LD_LIBRARY_PATH=str(prefix / 'lib64'),
                       ASAN_OPTIONS='detect_leaks=1', UBSAN_OPTIONS='halt_on_error=1')

    def execute(command, name, expected=0):
        report['commands'].append(command)
        result = subprocess.run(command, env=environment, capture_output=True, text=True, timeout=120)
        (output / name).write_text(result.stdout + result.stderr)
        if result.returncode != expected:
            raise ValueError(name + ' failed: ' + result.stdout + result.stderr)
        return result.stdout

    try:
        variants = {'asan': 'address,undefined', 'ubsan': 'undefined', 'release': None}
        for variant, sanitizer in variants.items():
            binary = output / ('ssc-imu-session-' + variant)
            flags = ['-fsanitize=' + sanitizer, '-fno-sanitize-recover=all'] if sanitizer else []
            execute(['cc', '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', *flags,
                     '-I', str(ROOT / 'src'), '-I', str(output),
                     *(str(ROOT / n) for n in sources if n.endswith('.c')),
                     '-L', str(prefix / 'lib64'), '-l:libssc.so.2', *pkg, '-o', str(binary)], variant + '-compile.log')
            if variant == 'asan':
                log = execute([str(binary), 'no-qrtr'], 'no-qrtr.log')
                if log.count('SSC_IMU_SESSION_PASS mode=no-qrtr') != 100:
                    raise ValueError('The cancellation suite did not complete 100 cycles')
            else:
                execute([str(binary), 'normal'], variant + '-vm-refusal.log', 2)
        if report['inputs_sha256'] != {n: ssc.digest(ROOT / n) for n in sources} or ssc.package(bundle) != record:
            raise ValueError('SSC session inputs changed during testing')
        report.update(status='passed', binaries_sha256={
            'ssc-imu-session-' + variant: ssc.digest(output / ('ssc-imu-session-' + variant))
            for variant in variants})
    except Exception as error:
        report.update(status='failed', error=str(error))
        raise
    finally:
        (output / 'result.json').write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ssc-bundle', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    run(parser.parse_args())
