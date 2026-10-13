#!/usr/bin/env python3
"""Verify Pico head orientation with the pinned Monado fusion implementation."""
import argparse
import hashlib
import importlib.util
import shlex
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
    parser.add_argument('--ssc-bundle', type=Path)
    parser.add_argument('--replay-capture', type=Path, help='Replay a saved stock log in sensor coordinates, without calibration')
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
    template = (include / 'xrt/xrt_config_build.h.cmake_in').read_text()
    (generated / 'xrt_config_build.h').write_text(re.sub(r'^#cmakedefine .+$', '', template, flags=re.MULTILINE))
    inputs = [ROOT / name for name in ['tests/pico-head-tracking.c', 'src/pico-head-tracking.c']]
    inputs += [auxiliary / ('math/' + name) for name in ['m_imu_3dof.c', 'm_filter_fifo.c', 'm_base.cpp']]
    eigen = subprocess.check_output(['pkg-config', '--cflags', 'eigen3'], text=True).split()
    flags = ['-O1', '-g1', '-ffunction-sections', '-fdata-sections', '-pthread',
             *eigen, '-I', str(ROOT / 'src'), '-I', str(output / 'include'),
             '-isystem', str(include), '-isystem', str(auxiliary),
             '-isystem', str(source / 'src/external/cjson')]
    sanitize = [] if args.sanitizer == 'none' else ['-fsanitize=' + args.sanitizer, '-fno-sanitize-recover=all']
    report = {'status': 'started', 'monado_commit': profile['commit'], 'archive_sha256': digest(args.archive),
              'commands': [], 'sanitizer': args.sanitizer, 'hardware_verified': False,
              'scope': 'Actual Monado orientation fusion with synthetic Pico IMU events; no device or runtime activation'}

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
        binary = output / 'pico-head-tracking'
        collect = '-Wl,-dead_strip' if sys.platform == 'darwin' else '-Wl,--gc-sections'
        run([args.cxx, *sanitize, *objects, '-pthread', '-lm', collect, '-o', str(binary)], 'link.log')
        run([str(binary)], 'test.log', env=dict(os.environ, UBSAN_OPTIONS='halt_on_error=1', TSAN_OPTIONS='halt_on_error=1', ASAN_OPTIONS='detect_leaks=1'))
        log = (output / 'test.log').read_text()
        if 'PICO_HEAD_TRACKING_PASS pairs=14211' not in log:
            raise ValueError('Tracking tests did not complete')
        report['binary_sha256'] = digest(binary)
        print(log, end='')
        if args.replay_capture:
            spec = importlib.util.spec_from_file_location('pico_imu', ROOT / 'tools/pico-imu.py')
            imu = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(imu)
            capture = args.replay_capture.read_bytes().decode('utf-8')
            analysis = imu.analyze(capture)
            (output / 'capture-analysis.json').write_text(json.dumps(analysis, indent=2) + '\n')
            samples = sorted([(e['sequence'], i, e) for i, kind in enumerate((35, 16))
                              for e in analysis['samples'][kind]])
            seen = [set(), set()]
            pairs, expected = 0, 0
            rows = []
            for _, sensor, event in samples:
                ticks = event['counter_ns']
                seen[sensor].add(ticks)
                if ticks in seen[1 - sensor]:
                    pairs += 1
                    if pairs > 1:
                        expected = event['monotonic_ns']
                rows.append(' '.join(map(str, [sensor, ticks, event['monotonic_ns'],
                    analysis['mapping_uncertainty_ns'], *event['vector'], expected])))
            replay_data = output / 'capture-events.txt'
            replay_data.write_text('\n'.join(rows) + '\n')
            replay_source = ROOT / 'tests/pico-head-replay.c'
            replay_object, replay_binary = output / 'replay.o', output / 'pico-head-replay'
            run([args.cc, '-std=gnu11', *flags, *sanitize, '-Wall', '-Wextra', '-Werror',
                 '-c', str(replay_source), '-o', str(replay_object)], 'replay-compile.log')
            run([args.cxx, *sanitize, *objects[1:], str(replay_object), '-pthread', '-lm', collect,
                 '-o', str(replay_binary)], 'replay-link.log')
            run([str(replay_binary), str(replay_data)], 'replay-test.log',
                env=dict(os.environ, ASAN_OPTIONS='detect_leaks=1', UBSAN_OPTIONS='halt_on_error=1'))
            if 'PICO_HEAD_REPLAY_PASS samples=' + str(len(samples)) + ' ' not in (output / 'replay-test.log').read_text():
                raise ValueError('Saved capture replay did not complete')
            report['replay'] = {'capture_sha256': hashlib.sha256(capture.encode()).hexdigest(),
                'source_sha256': digest(replay_source), 'analyzer_sha256': digest(ROOT / 'tools/pico-imu.py'),
                'events_sha256': digest(replay_data), 'binary_sha256': digest(replay_binary),
                'samples': len(samples), 'pairs': pairs, 'physical_tracking_verified': False,
                'scope': 'Raw saved Android vectors; identity axes, unit scales 1, accuracy 3 only for numerical testing. '
                         'No calibration or vendor auxiliary values applied. Monotonic replay arrival uses the latest capture time seen, '
                         'not measured latency; counter nanoseconds are replay identifiers, not native SSC ticks.'}
        if args.ssc_bundle:
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
            pkg = shlex.split(subprocess.check_output(['pkg-config', '--cflags', '--libs', 'gio-2.0'], text=True))
            session_sources = [ROOT / n for n in ('tests/pico-head-session.c', 'src/pico-head-session.c',
                'src/ssc-imu-session.c', 'src/ssc-imu.c', 'src/ssc-calibration.c', 'src/ssc-clock.c')]
            session_objects = []
            for index, path in enumerate(session_sources):
                obj = output / ('session-' + str(index) + '.o')
                run([args.cc, '-std=gnu11', *flags, *sanitize, *pkg, '-Wall', '-Wextra', '-Werror',
                     '-c', str(path), '-o', str(obj)], 'session-' + str(index) + '-compile.log')
                session_objects.append(str(obj))
            binary = output / 'pico-head-session'
            run([args.cxx, *sanitize, *objects[1:], *session_objects, *pkg,
                 '-L', str(prefix / 'lib64'), '-l:libssc.so.2', '-pthread', '-lm', collect,
                 '-o', str(binary)], 'session-link.log')
            environment = dict(os.environ, LD_LIBRARY_PATH=str(prefix / 'lib64'),
                               ASAN_OPTIONS='detect_leaks=1', UBSAN_OPTIONS='halt_on_error=1')
            run([str(binary), 'no-qrtr'], 'session-test.log', env=environment)
            if 'PICO_HEAD_SESSION_PASS mode=no-qrtr cycles=100' not in (output / 'session-test.log').read_text():
                raise ValueError('Session cancellation tests did not complete')
            report['session_inputs_sha256'] = {str(p.relative_to(ROOT)): digest(p) for p in session_sources}
            report['session_binary_sha256'] = digest(binary)
            report['ssc_bundle'] = record
            if ssc.package(bundle) != record:
                raise ValueError('SSC bundle changed during testing')
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
