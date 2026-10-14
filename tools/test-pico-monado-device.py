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
    parser.add_argument('--eigen', type=Path)
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
    for name in ('xrt_config_build.h', 'xrt_config_have.h'):
        template = (include / ('xrt/' + name + '.cmake_in')).read_text()
        (generated / name).write_text(re.sub(r'^#cmakedefine .+$', '', template, flags=re.MULTILINE))
    inputs = [ROOT / name for name in ['tests/pico-monado-device.c', 'src/pico-monado-device.c', 'src/pico-monado-input.c']]
    inputs += [auxiliary / ('util/' + name) for name in [
        'u_device.c', 'u_device_ni.c', 'u_device_id.c', 'u_limited_unique_id.cpp',
        'u_visibility_mask.c', 'u_debug.c', 'u_logging.c',
    ]]
    inputs.append(source / 'src/external/cjson/cjson/cJSON.c')
    prober_inputs = [ROOT / 'tests/pico-monado-prober.c', ROOT / 'src/pico-monado-prober.c']
    flags = ['-O1', '-g1', '-ffunction-sections', '-fdata-sections', '-pthread',
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
        prober = output / 'pico-monado-prober'
        run([args.cc, '-std=gnu11', *flags, *sanitize, '-Wall', '-Wextra', '-Werror',
             *map(str, prober_inputs), objects[-1], '-lm', '-o', str(prober)], 'prober-compile.log')
        run([str(prober)], 'prober-test.log',
            env=dict(os.environ, UBSAN_OPTIONS='halt_on_error=1',
                     ASAN_OPTIONS='detect_leaks=' + ('0' if sys.platform == 'darwin' else '1')))
        log = (output / 'prober-test.log').read_text()
        if 'PICO_MONADO_PROBER_PASS checks=123 sessions=100' not in log:
            raise ValueError('Prober tests did not complete')
        print(log.splitlines()[-1])
        hmd_inputs = [ROOT / name for name in ('tests/pico-monado-hmd.c', 'src/pico-monado-hmd.c', 'src/pico-slam-tracking.c')]
        hmd_inputs += [auxiliary / ('math/' + name) for name in ('m_base.cpp', 'm_predict.c', 'm_relation_history.cpp', 'm_quatexpmap.cpp')]
        eigen = ['-isystem', str(args.eigen.resolve())] if args.eigen else subprocess.check_output(['pkg-config', '--cflags', 'eigen3'], text=True).split()
        hmd_objects = []
        for index, path in enumerate(hmd_inputs):
            obj = output / ('hmd-' + str(index) + '.o')
            compiler = args.cxx if path.suffix == '.cpp' else args.cc
            standard = '-std=c++17' if path.suffix == '.cpp' else '-std=gnu11'
            warnings = ['-Wall', '-Wextra', '-Werror'] if index < 3 else ['-w']
            run([compiler, standard, *flags, *eigen, *sanitize, *warnings,
                 '-c', str(path), '-o', str(obj)], 'hmd-' + str(index) + '-compile.log', timeout=180)
            hmd_objects.append(str(obj))
        hmd = output / 'pico-monado-hmd'
        run([args.cxx, *sanitize, *hmd_objects, *objects[3:], '-pthread', '-lm', collect,
             '-o', str(hmd)], 'hmd-link.log')
        run([str(hmd)], 'hmd-test.log', env=dict(os.environ, UBSAN_OPTIONS='halt_on_error=1',
            ASAN_OPTIONS='detect_leaks=' + ('0' if sys.platform == 'darwin' else '1')))
        hmd_log = (output / 'hmd-test.log').read_text()
        if 'PICO_HMD_VISUAL_TRACKING_PASS' not in hmd_log or 'PICO_MONADO_HMD_PASS' not in hmd_log or 'joined=1064' not in hmd_log or 'PICO_HMD_RADIAL_PASS samples=12740' not in hmd_log:
            raise ValueError('HMD callbacks and ownership checks did not complete')
        report['hmd_inputs_sha256'] = {str(p): digest(p) for p in hmd_inputs}
        report['hmd_binary_sha256'] = digest(hmd)
        print(hmd_log, end='')
        config_inputs = [ROOT / ('src/' + name + '.c') for name in ('pico-hmd-config', 'pico-hmd-prober')]
        config_inputs.append(ROOT / 'tests/pico-hmd-config.c')
        config_binary = output / 'pico-hmd-config'
        run([args.cc, '-std=gnu11', *flags, *sanitize, '-Wall', '-Wextra', '-Werror',
             *map(str, config_inputs), objects[-1], '-lm', '-o', str(config_binary)], 'hmd-config-compile.log')
        run([str(config_binary)], 'hmd-config-test.log', env=dict(os.environ, UBSAN_OPTIONS='halt_on_error=1',
            ASAN_OPTIONS='detect_leaks=' + ('0' if sys.platform == 'darwin' else '1')))
        if 'PICO_HMD_CONFIG_PASS refusals=39 creates=3' not in (output / 'hmd-config-test.log').read_text():
            raise ValueError('HMD configuration or prober tests did not complete')
        if 'PICO_HMD_ANALYTIC_CONFIG_PASS refusals=14' not in (output / 'hmd-config-test.log').read_text():
            raise ValueError('Analytic HMD configuration checks did not complete')
        report['hmd_config_inputs_sha256'] = {str(p.relative_to(ROOT)): digest(p) for p in config_inputs}
        report['hmd_config_binary_sha256'] = digest(config_binary)
        report['status'] = 'passed'
    except Exception as error:
        report.update(status='failed', error=str(error))
        raise
    finally:
        report['inputs_sha256'] = {str(path): digest(path) for path in inputs + prober_inputs}
        report['headers_sha256'] = {str(path.relative_to(ROOT)): digest(path) for path in (ROOT / 'src').glob('*.h')}
        report['runner_sha256'] = digest(Path(__file__))
        report['generated_config_sha256'] = digest(generated / 'xrt_config_build.h')
        (output / 'result.json').write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    main()
