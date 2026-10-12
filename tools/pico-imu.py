#!/usr/bin/env python3
"""Capture a bounded stock Neo3 IMU stream, or analyze an existing capture offline."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import math
from pathlib import Path
import re
import shlex
import statistics
import subprocess
import uuid

FINGERPRINT = 'Pico/A7H10/PICOA7H10:10/5.8.4.0/smartcm.1696861506:user/test-keys'
TRACKING_LIBRARY = '/system/lib64/libpvrtrackingservice.so'
TRACKING_SHA256 = '4b7f74b80607a2863559ab6496f4a8abe6778ccaec834074a923a2064cfe0a9a'
ROOT = Path(__file__).resolve().parents[1]


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def analyze(text):
    if len(text) > 4 * 1024 * 1024:
        raise ValueError('Capture exceeds bounded ring size')
    lines = text.splitlines()
    if not lines or lines[0] != 'PICO_IMU_CAPTURE 1' or lines[-1] != 'PICO_IMU_CAPTURE_PASS':
        raise ValueError('Incomplete or unsupported capture')
    clocks = {}
    samples = {35: [], 16: []}
    sequence = 0
    declared = None
    sensors, tokens = set(), {}
    for line in lines[1:-1]:
        fields = line.split()
        if not fields:
            raise ValueError('Empty record')
        if fields[0] == 'CLOCK':
            if len(fields) != 7 or fields[1] not in ('before', 'after') or \
                    fields[2] not in ('monotonic', 'boottime'):
                raise ValueError('Invalid clock record')
            before, ticks, hz, after = map(int, fields[3:])
            if min(before, ticks) < 0 or hz <= 0 or hz > 2**32 - 1 or \
                    after < before or after > 2**63 - 1 or ticks > 2**64 - 1:
                raise ValueError('Invalid counter bracket')
            counter_ns = ticks * 10**9 // hz
            quantum = (10**9 + hz - 1) // hz
            bracket = {'before_ns': before, 'after_ns': after, 'ticks': ticks, 'hz': hz,
                       'offset_ns': before + (after - before) // 2 - counter_ns,
                       'uncertainty_ns': (after - before + 1) // 2 + quantum + 2}
            clocks.setdefault((fields[1], fields[2]), []).append(bracket)
        elif fields[0] == 'SAMPLE':
            if len(fields) != 10:
                raise ValueError('Invalid sample record')
            index, kind, timestamp = map(int, fields[1:4])
            values = list(map(float, fields[4:]))
            if index != sequence + 1 or index > 8192 or kind not in samples or \
                    not 0 < timestamp < 2**63 or not all(map(math.isfinite, values)):
                raise ValueError('Invalid sample identity, sequence or payload')
            if samples[kind] and timestamp <= samples[kind][-1]['counter_ns']:
                raise ValueError('Sample timestamp did not advance')
            samples[kind].append({'sequence': index, 'counter_ns': timestamp,
                                  'vector': values[:3], 'vendor_aux': values[3:]})
            sequence = index
        elif fields[0] == 'COUNTS':
            if declared is not None or len(fields) != 3:
                raise ValueError('Duplicate or invalid counts')
            declared = tuple(map(int, fields[1:]))
        elif fields[0] == 'TOKEN':
            if len(fields) != 3 or int(fields[1]) not in samples or \
                    int(fields[1]) in tokens or int(fields[2]) <= 0:
                raise ValueError('Invalid sensor token')
            tokens[int(fields[1])] = int(fields[2])
        elif fields[0] == 'SENSOR':
            if len(fields) < 5 or int(fields[1]) not in samples or int(fields[1]) in sensors or \
                    fields[-2] != 'direct=1' or fields[-1] not in (
                        'max_rate_level=1', 'max_rate_level=2', 'max_rate_level=3'):
                raise ValueError('Unsupported direct sensor')
            sensors.add(int(fields[1]))
        else:
            raise ValueError('Unknown capture record')
    if sensors != {35, 16} or set(tokens) != sensors or len(set(tokens.values())) != 2:
        raise ValueError('Missing or ambiguous sensor identity')
    if declared != (len(samples[35]), len(samples[16])) or min(map(len, samples.values())) <= 10:
        raise ValueError('Incomplete sensor streams')
    if len(clocks) != 4 or any(len(value) != 16 for value in clocks.values()):
        raise ValueError('Missing counter calibration')
    if len({c['hz'] for values in clocks.values() for c in values}) != 1:
        raise ValueError('Counter frequency changed')
    best = {key: min(value, key=lambda c: c['uncertainty_ns']) for key, value in clocks.items()}
    for clock in ('monotonic', 'boottime'):
        before, after = best['before', clock], best['after', clock]
        duration = after['before_ns'] - before['after_ns']
        if not 10**9 < duration < 5 * 10**9 or after['ticks'] <= before['ticks'] or \
                max(before['uncertainty_ns'], after['uncertainty_ns']) > 10**6:
            raise ValueError('Unbounded or discontinuous clock calibration')
        if abs(after['offset_ns'] - before['offset_ns']) > \
                before['uncertainty_ns'] + after['uncertainty_ns'] + 100_000:
            raise ValueError('Counter mapping changed during capture')
    first, last = best['before', 'monotonic'], best['after', 'monotonic']
    offset_low = min(c['offset_ns'] - c['uncertainty_ns'] for c in (first, last))
    offset_high = max(c['offset_ns'] + c['uncertainty_ns'] for c in (first, last))
    offset = (offset_low + offset_high) // 2
    uncertainty = (offset_high - offset_low + 1) // 2
    for stream in samples.values():
        for sample in stream:
            mapped = sample['counter_ns'] + offset
            if mapped - uncertainty < first['before_ns'] - 100_000_000 or \
                    mapped + uncertainty > last['after_ns']:
                raise ValueError('Sample does not fit the measured counter clock')
            sample['monotonic_ns'] = mapped
    summaries = {}
    for kind, stream in samples.items():
        times = [sample['counter_ns'] for sample in stream]
        periods = [b - a for a, b in zip(times, times[1:])]
        summaries[str(kind)] = {
            'count': len(stream), 'median_period_ns': statistics.median(periods),
            'min_period_ns': min(periods), 'max_period_ns': max(periods),
            'mean_rate_hz': (len(times) - 1) * 10**9 / (times[-1] - times[0]),
            'last_sample_age_ns': last['after_ns'] - stream[-1]['monotonic_ns'],
        }
    pairs = len({s['counter_ns'] for s in samples[35]} & {s['counter_ns'] for s in samples[16]})
    return {'status': 'passed', 'clock_model': 'explicit-counter-nanoseconds-to-monotonic',
            'counter_hz': first['hz'], 'monotonic_offset_ns': offset,
            'mapping_uncertainty_ns': uncertainty, 'exact_timestamp_pairs': pairs,
            'clocks': {':'.join(key): value for key, value in best.items()},
            'streams': summaries, 'samples': samples,
            'calibration_applied': False, 'pose_estimated': False}


def direct_connections(text):
    matches = re.findall(r'^\s*(\d+) direct connections\b', text, re.MULTILINE)
    if matches != ['0']:
        raise ValueError('Sensor service is busy or its channel state is unknown')


def capture(args):
    args.output.mkdir(mode=0o700, parents=False, exist_ok=False)
    source = ROOT / 'tools/pico-imu-capture.c'
    binary = args.output / 'pico-imu-capture'
    remote = '/data/local/tmp/armada-vr-imu-' + uuid.uuid4().hex
    report = {'status': 'started', 'utc': datetime.now(timezone.utc).isoformat(),
              'serial': args.serial, 'source_sha256': digest(source),
              'collector_sha256': digest(Path(__file__)), 'remote_directory': remote,
              'boot_chain_writes': False, 'commands': []}
    created = False
    failure = None

    def adb(*command, check=True, timeout=15):
        result = subprocess.run([args.adb, '-P', str(args.port), '-s', args.serial, *command],
                                capture_output=True, text=True, timeout=timeout)
        report['commands'].append({'arguments': command, 'returncode': result.returncode,
                                   'stdout': result.stdout, 'stderr': result.stderr})
        if check and result.returncode:
            raise ValueError('ADB command failed: ' + ' '.join(command))
        return result

    def shell(*command, **kwargs):
        return adb('shell', shlex.join(command), **kwargs)

    try:
        compilers = list(args.ndk.glob('toolchains/llvm/prebuilt/*/bin/aarch64-linux-android29-clang'))
        if len(compilers) != 1:
            raise ValueError('Expected one installed Android NDK compiler')
        command = [str(compilers[0]), '-std=c11', '-O2', '-g', '-Wall', '-Wextra', '-Werror',
                   str(source), '-landroid', '-o', str(binary)]
        built = subprocess.run(command, capture_output=True, text=True, timeout=60)
        report['build'] = {'arguments': command, 'returncode': built.returncode,
                           'stdout': built.stdout, 'stderr': built.stderr}
        if built.returncode or digest(source) != report['source_sha256']:
            raise ValueError('Diagnostic compilation failed or source changed')
        report['binary_sha256'] = digest(binary)
        fingerprint = shell('getprop', 'ro.build.fingerprint').stdout.strip()
        device = shell('getprop', 'ro.product.device').stdout.strip()
        if fingerprint != FINGERPRINT or device != 'PICOA7H10':
            raise ValueError('Unsupported Pico firmware or device')
        if shell('sha256sum', TRACKING_LIBRARY).stdout.split() != [TRACKING_SHA256, TRACKING_LIBRARY]:
            raise ValueError('Stock tracking library changed')
        boot = shell('cat', '/proc/sys/kernel/random/boot_id').stdout.strip()
        if not re.fullmatch(r'[0-9a-f]{8}(?:-[0-9a-f]{4}){3}-[0-9a-f]{12}', boot):
            raise ValueError('Invalid boot identity')
        before = shell('dumpsys', 'sensorservice').stdout
        (args.output / 'sensorservice-before.txt').write_text(before)
        direct_connections(before)
        shell('mkdir', remote)
        created = True
        adb('push', str(binary), remote + '/probe')
        if shell('sha256sum', remote + '/probe').stdout.split() != [report['binary_sha256'], remote + '/probe']:
            raise ValueError('Uploaded diagnostic hash mismatch')
        shell('chmod', '700', remote + '/probe')
        captured = shell('timeout', '12', remote + '/probe', '--capture', check=False, timeout=20)
        (args.output / 'capture.log').write_text(captured.stdout)
        (args.output / 'capture.stderr').write_text(captured.stderr)
        report['capture_exit'] = captured.returncode
        if shell('getprop', 'ro.build.fingerprint').stdout.strip() != fingerprint or \
                shell('cat', '/proc/sys/kernel/random/boot_id').stdout.strip() != boot:
            raise ValueError('Device identity changed during capture')
        report['boot_identity_unchanged'] = True
        after = shell('dumpsys', 'sensorservice').stdout
        (args.output / 'sensorservice-after.txt').write_text(after)
        direct_connections(after)
        report['direct_channels_released'] = True
        if captured.returncode:
            raise ValueError('Device diagnostic did not complete')
        analysis = analyze(captured.stdout)
        (args.output / 'analysis.json').write_text(json.dumps(analysis, indent=2) + '\n')
        report['status'] = 'passed'
    except BaseException as error:
        failure = error
        report.update(status='failed', error=repr(error))
    finally:
        if created:
            try:
                present = shell('sha256sum', remote + '/probe', check=False)
                if present.returncode or present.stdout.split() != [report.get('binary_sha256'), remote + '/probe']:
                    raise ValueError('Temporary file could not be verified for removal')
                shell('rm', remote + '/probe')
                shell('rmdir', remote)
                report['temporary_binary_removed'] = True
            except BaseException as error:
                report.update(status='failed', cleanup_error=repr(error))
                failure = failure or error
        report['files'] = {p.name: digest(p) for p in args.output.iterdir() if p.is_file()}
        (args.output / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
    if failure:
        raise failure
    print(json.dumps({key: report[key] for key in ('status', 'direct_channels_released',
                                                  'boot_identity_unchanged', 'temporary_binary_removed')}))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='command', required=True)
    collect = commands.add_parser('capture', help='Run a two-second stock sensor diagnostic')
    collect.add_argument('--ndk', type=Path, required=True)
    collect.add_argument('--adb', default='adb')
    collect.add_argument('--port', type=int, default=5037)
    collect.add_argument('--serial', required=True)
    collect.add_argument('--output', type=Path, required=True)
    offline = commands.add_parser('analyze', help='Validate and map an existing capture offline')
    offline.add_argument('capture', type=Path)
    offline.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if args.command == 'capture':
        capture(args)
    else:
        result = analyze(args.capture.read_text())
        with args.output.open('x') as output:
            json.dump(result, output, indent=2)
            output.write('\n')


if __name__ == '__main__':
    main()
