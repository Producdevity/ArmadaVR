#!/usr/bin/env python3
"""Verify bounded Pico 6DoF output with the pinned Monado pose-history implementation."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import platform
import shlex
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
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument('--session', action='store_true', help='Test owned VIT session lifecycle')
    mode.add_argument('--sensors', action='store_true', help='Test the runtime with the actual SSC library in offline ARM64 Linux')
    parser.add_argument('--ssc-bundle', type=Path)
    mode.add_argument('--runtime', action='store_true', help='Test SSC worker and VIT owner integration')
    mode.add_argument('--imu', action='store_true', help='Test the bounded SSC worker handoff')
    parser.add_argument('--cc', default='cc')
    parser.add_argument('--cxx', default='c++')
    parser.add_argument('--sanitizer', choices=('address,undefined', 'undefined', 'thread', 'none'), default='address,undefined')
    args = parser.parse_args()
    if args.sensors and (not args.ssc_bundle or sys.platform != 'linux' or platform.machine() != 'aarch64'):
        parser.error('--sensors requires --ssc-bundle and offline Linux ARM64')
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
    local = ['tests/pico-slam-tracking.c', 'src/pico-slam-tracking.c']
    if args.session:
        local = ['tests/pico-slam-session.c', 'src/pico-slam-session.c', 'src/pico-camera.c', 'src/pico-slam-input.c', 'src/pico-slam-tracking.c']
    if args.runtime or args.sensors:
        local = ['tests/pico-slam-runtime.c', 'src/pico-slam-runtime.c', 'src/pico-slam-imu.c', 'src/pico-slam-session.c', 'src/pico-camera.c', 'src/pico-slam-input.c', 'src/pico-slam-tracking.c']
    if args.sensors:
        local[0] = 'tests/pico-slam-sensors.c'
        local += ['src/ssc-imu-session.c', 'src/ssc-imu.c', 'src/ssc-calibration.c', 'src/ssc-clock.c']
    if args.imu:
        local = ['tests/pico-slam-imu.c', 'src/pico-slam-imu.c']
    inputs = [ROOT / name for name in local]
    inputs += [auxiliary / ('math/' + name) for name in ['m_base.cpp', 'm_predict.c', 'm_relation_history.cpp', 'm_quatexpmap.cpp']]
    inputs += [auxiliary / ('util/' + name) for name in ['u_logging.c', 'u_debug.c']]
    inputs.append(source / 'src/external/cjson/cjson/cJSON.c')
    if args.imu:
        inputs = inputs[:len(local)]
    eigen = [] if args.imu else (['-isystem', str(args.eigen.resolve())] if args.eigen else subprocess.check_output(['pkg-config', '--cflags', 'eigen3'], text=True).split())
    flags = ['-O1', '-g1', '-ffunction-sections', '-fdata-sections', '-pthread',
             *eigen, '-I', str(ROOT / 'src'), '-I', str(output / 'include'),
             '-isystem', str(include), '-isystem', str(auxiliary),
             '-isystem', str(source / 'src/external/cjson'),
             '-isystem', str(source / 'src/external/vit_includes')]
    sanitize = [] if args.sanitizer == 'none' else ['-fsanitize=' + args.sanitizer, '-fno-sanitize-recover=all']
    report = {'status': 'started', 'monado_commit': profile['commit'], 'archive_sha256': digest(args.archive),
              'commands': [], 'sanitizer': args.sanitizer, 'hardware_verified': False,
              'scope': 'Actual Monado relation history with synthetic calibrated 6DoF poses; no device activation'}

    if args.imu:
        report['scope'] = 'Bounded SSC pairing and cross-thread handoff with synthetic calibrated input; no device activation'

    if args.runtime:
        report['scope'] = 'Actual runtime/session/queue with synthetic SSC worker and VIT backend; no device activation'

    link_flags = []
    environment = dict(os.environ, UBSAN_OPTIONS='halt_on_error=1', TSAN_OPTIONS='halt_on_error=1', ASAN_OPTIONS='detect_leaks=' + ('0' if sys.platform == 'darwin' else '1'))
    if args.sensors:
        spec = importlib.util.spec_from_file_location('ssc', ROOT / 'tools/ssc-artifact.py')
        ssc = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(ssc)
        bundle = args.ssc_bundle.resolve()
        record = ssc.package(bundle)
        prefix = bundle / ssc.PREFIX
        with tarfile.open(prefix / 'share/ssc/source.tar.gz') as stream:
            headers = [m for m in stream if m.name.endswith('/src/libssc-sensor.h')]
            if len(headers) != 1 or not headers[0].isfile() or headers[0].size > 65536:
                raise ValueError('Expected one pinned SSC header')
            header = output / 'include/libssc-sensor.h'
            header.write_bytes(stream.extractfile(headers[0]).read())
        if digest(header) != json.loads((ROOT / 'profiles/ssc.json').read_text())['test_source_sha256']['src/libssc-sensor.h']:
            raise ValueError('SSC header changed')
        flags += shlex.split(subprocess.check_output(['pkg-config', '--cflags', 'gio-2.0'], text=True))
        link_flags = shlex.split(subprocess.check_output(['pkg-config', '--libs', 'gio-2.0'], text=True))
        link_flags += ['-L', str(prefix / 'lib64'), '-l:libssc.so.2']
        environment['LD_LIBRARY_PATH'] = str(prefix / 'lib64')
        report['ssc_bundle'] = record
        report['scope'] = 'Native SSC runtime startup/cancellation without QRTR; virtual endpoint scenarios run separately'

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
            warning = ['-Wall', '-Wextra', '-Werror'] if path in inputs[:len(local)] else ['-w']
            run([compiler, standard, *flags, *sanitize, *warning, '-c', str(path), '-o', str(obj)], name + '-compile.log', timeout=180)
            objects.append(str(obj))
        binary = output / 'pico-slam-tracking'
        collect = '-Wl,-dead_strip' if sys.platform == 'darwin' else '-Wl,--gc-sections'
        run([args.cxx, *sanitize, *objects, *link_flags, '-pthread', '-lm', collect, '-o', str(binary)], 'link.log')
        run([str(binary)] + (['no-qrtr'] if args.sensors else []), 'test.log', env=environment)
        log = (output / 'test.log').read_text()
        expected = 'PICO_SLAM_SENSORS_PASS mode=no-qrtr cycles=100' if args.sensors else 'PICO_SLAM_RUNTIME_PASS cycles=200 backend_faults=8 stream_faults=7 joined=215' if args.runtime else 'PICO_SLAM_IMU_PASS cycles=200 pairs=59800 threaded_pairs=10000 faults=22' if args.imu else 'PICO_SLAM_SESSION_PASS generations=1013' if args.session else 'PICO_SLAM_TRACKING_PASS'
        if expected not in log or (not args.session and not args.imu and not args.runtime and not args.sensors and 'generations=1000 poses=10000 reader_queries=20000' not in log):
            raise ValueError('Tracking tests did not complete')
        if args.sensors and ssc.package(bundle) != record:
            raise ValueError('SSC bundle changed during testing')
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
