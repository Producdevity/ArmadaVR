#!/usr/bin/env python3
"""Exercise Pico device callbacks with the pinned Monado implementation."""
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
    parser.add_argument('--cc', default='cc')
    parser.add_argument('--cxx', default='c++')
    parser.add_argument('--sanitizer', choices=('address,undefined', 'thread', 'none'), default='address,undefined')
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
    template = (include / 'xrt/xrt_config_build.h.cmake_in').read_text()
    (generated / 'xrt_config_build.h').write_text(re.sub(r'^#cmakedefine .+$', '', template, flags=re.MULTILINE))
    inputs = [ROOT / name for name in ['tests/pico-monado-device.c', 'src/pico-monado-device.c', 'src/pico-monado-input.c']]
    inputs += [auxiliary / ('util/' + name) for name in [
        'u_device.c', 'u_device_ni.c', 'u_device_id.c', 'u_limited_unique_id.cpp',
        'u_visibility_mask.c', 'u_debug.c', 'u_logging.c',
    ]]
    inputs.append(source / 'src/external/cjson/cjson/cJSON.c')
    flags = ['-O1', '-g', '-ffunction-sections', '-fdata-sections', '-pthread',
             '-I', str(ROOT / 'src'), '-I', str(output / 'include'),
             '-isystem', str(include), '-isystem', str(auxiliary),
             '-isystem', str(source / 'src/external/cjson')]
    sanitize = [] if args.sanitizer == 'none' else ['-fsanitize=' + args.sanitizer, '-fno-sanitize-recover=all']
    report = {'status': 'started', 'monado_commit': profile['commit'], 'archive_sha256': digest(args.archive),
              'commands': [], 'sanitizer': args.sanitizer, 'hardware_verified': False,
              'scope': 'Actual xrt_device callbacks and Monado helpers, synthetic events; no live SSC or application actions'}

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
            warning = ['-Wall', '-Wextra', '-Werror'] if path in inputs[:3] else ['-w']
            run([compiler, standard, *flags, *sanitize, *warning, '-c', str(path), '-o', str(obj)], name + '-compile.log')
            objects.append(str(obj))
        binary = output / 'pico-monado-device'
        collect = '-Wl,-dead_strip' if sys.platform == 'darwin' else '-Wl,--gc-sections'
        run([args.cxx, *sanitize, *objects, '-pthread', '-lm', collect, '-o', str(binary)], 'link.log')
        run([str(binary)], 'test.log', env=dict(os.environ, UBSAN_OPTIONS='halt_on_error=1', TSAN_OPTIONS='halt_on_error=1'))
        log = (output / 'test.log').read_text()
        if 'PICO_MONADO_DEVICE_PASS' not in log or 'events=20000 generations=1000' not in log:
            raise ValueError('Device tests did not complete')
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
        (output / 'result.json').write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    main()
