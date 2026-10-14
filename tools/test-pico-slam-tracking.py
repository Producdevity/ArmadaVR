#!/usr/bin/env python3
"""Verify bounded Pico 6DoF output with the pinned Monado pose-history implementation."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tarfile

ROOT = Path(__file__).resolve().parents[1]


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('archive', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--eigen', type=Path, help='Optional Eigen header directory')
    parser.add_argument('--cc', default='cc')
    parser.add_argument('--cxx', default='c++')
    parser.add_argument('--sanitizer', choices=('address,undefined', 'undefined', 'thread', 'none'), default='address,undefined')
    args = parser.parse_args()
    profile = json.loads((ROOT / 'profiles/monado.json').read_text())
    if digest(args.archive) != profile['full_source']['sha256']:
        raise ValueError('Monado archive differs from the pinned revision')
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    source = output / 'source' / profile['full_source']['archive_root']
    with tarfile.open(args.archive) as stream:
        stream.extractall(output / 'source', filter='data')
    auxiliary = source / 'src/xrt/auxiliary'
    include = source / 'src/xrt/include'
    generated = output / 'include/xrt'
    generated.mkdir(parents=True)
    for name in ('xrt_config_build.h', 'xrt_config_have.h'):
        template = (include / ('xrt/' + name + '.cmake_in')).read_text()
        (generated / name).write_text(re.sub(r'^#cmakedefine .+$', '', template, flags=re.MULTILINE))
    inputs = [ROOT / name for name in ['tests/pico-slam-tracking.c', 'src/pico-slam-tracking.c']]
    inputs += [auxiliary / ('math/' + name) for name in ['m_base.cpp', 'm_predict.c', 'm_relation_history.cpp', 'm_quatexpmap.cpp']]
    inputs += [auxiliary / ('util/' + name) for name in ['u_logging.c', 'u_debug.c']]
    inputs.append(source / 'src/external/cjson/cjson/cJSON.c')
    eigen = ['-isystem', str(args.eigen.resolve())] if args.eigen else subprocess.check_output(['pkg-config', '--cflags', 'eigen3'], text=True).split()
    flags = ['-O1', '-g1', '-ffunction-sections', '-fdata-sections', '-pthread',
             *eigen, '-I', str(ROOT / 'src'), '-I', str(output / 'include'),
             '-isystem', str(include), '-isystem', str(auxiliary),
             '-isystem', str(source / 'src/external/cjson')]
    sanitize = [] if args.sanitizer == 'none' else ['-fsanitize=' + args.sanitizer, '-fno-sanitize-recover=all']
    report = {'status': 'started', 'monado_commit': profile['commit'], 'archive_sha256': digest(args.archive),
              'commands': [], 'sanitizer': args.sanitizer, 'hardware_verified': False,
              'scope': 'Actual Monado relation history with synthetic calibrated 6DoF poses; no device activation'}

    def run(command, log, timeout=60, env=None):
        report['commands'].append(command)
        with (output / log).open('wb') as stream:
            subprocess.run(command, check=True, stdout=stream, stderr=subprocess.STDOUT, timeout=timeout, env=env)

    try:
        objects = []
        for index, path in enumerate(inputs):
            name = str(index) + '-' + path.stem
            obj = output / (name + '.o')
            compiler = args.cxx if path.suffix == '.cpp' else args.cc
            standard = '-std=c++17' if path.suffix == '.cpp' else '-std=gnu11'
            warning = ['-Wall', '-Wextra', '-Werror'] if path in inputs[:2] else ['-w']
            run([compiler, standard, *flags, *sanitize, *warning, '-c', str(path), '-o', str(obj)], name + '-compile.log', timeout=180)
            objects.append(str(obj))
        binary = output / 'pico-slam-tracking'
        collect = '-Wl,-dead_strip' if sys.platform == 'darwin' else '-Wl,--gc-sections'
        run([args.cxx, *sanitize, *objects, '-pthread', '-lm', collect, '-o', str(binary)], 'link.log')
        run([str(binary)], 'test.log', env=dict(os.environ, UBSAN_OPTIONS='halt_on_error=1', TSAN_OPTIONS='halt_on_error=1', ASAN_OPTIONS='detect_leaks=' + ('0' if sys.platform == 'darwin' else '1')))
        log = (output / 'test.log').read_text()
        if 'PICO_SLAM_TRACKING_PASS' not in log or 'generations=1000 poses=10000 reader_queries=20000' not in log:
            raise ValueError('Tracking tests did not complete')
        report['binary_sha256'] = digest(binary)
        print(log, end='')
        report['status'] = 'passed'
    except Exception as error:
        report.update(status='failed', error=str(error))
        raise
    finally:
        report['inputs_sha256'] = {str(path): digest(path) for path in inputs}
        report['headers_sha256'] = {str(path.relative_to(ROOT)): digest(path) for path in (ROOT / 'src').glob('*.h')}
        report['runner_sha256'] = digest(Path(__file__))
        report['generated_config_sha256'] = digest(generated / 'xrt_config_build.h')
        report['generated_have_sha256'] = digest(generated / 'xrt_config_have.h')
        (output / 'result.json').write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    main()
