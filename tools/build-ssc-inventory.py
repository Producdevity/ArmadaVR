#!/usr/bin/env python3
"""Build a metadata-only SSC collector from the verified native runtime sources."""
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


def build(args):
    if sys.platform != 'linux' or platform.machine() != 'aarch64':
        raise ValueError('Use an offline Linux ARM64 build environment')
    spec = importlib.util.spec_from_file_location('ssc', ROOT / 'tools/ssc-artifact.py')
    ssc = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(ssc)
    bundle = args.ssc_bundle.resolve()
    record = ssc.package(bundle)
    prefix = bundle / ssc.PREFIX
    profile = json.loads((ROOT / 'profiles/ssc.json').read_text())
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    source, generated = out / 'source', out / 'generated'
    source.mkdir()
    generated.mkdir()
    names = ['tools/ssc-inventory.c', 'tools/build-ssc-inventory.py', 'tests/ssc-inventory.c']
    report = {'status': 'started', 'hardware_verified': False, 'ssc_bundle': record,
              'inputs_sha256': {n: ssc.digest(ROOT / n) for n in names}, 'commands': []}
    env = dict(os.environ, LD_LIBRARY_PATH=str(prefix / 'lib64'),
               ASAN_OPTIONS='detect_leaks=1', UBSAN_OPTIONS='halt_on_error=1')

    def run(command, name, expected=0):
        report['commands'].append(command)
        result = subprocess.run(command, capture_output=True, text=True, env=env, timeout=60)
        (out / name).write_text(result.stdout + result.stderr)
        if result.returncode != expected:
            raise ValueError(name + ' failed: ' + result.stdout + result.stderr)
        return result.stdout

    try:
        with tarfile.open(prefix / 'share/ssc/source.tar.gz') as archive:
            for name, expected in profile['test_source_sha256'].items():
                members = [m for m in archive.getmembers() if m.name.endswith('/' + name)]
                if len(members) != 1 or not members[0].isfile() or members[0].size > 262144:
                    raise ValueError('Expected one bounded pinned source: ' + name)
                path = source / name
                path.parent.mkdir(exist_ok=True)
                path.write_bytes(archive.extractfile(members[0]).read())
                if ssc.digest(path) != expected:
                    raise ValueError('Pinned source changed: ' + name)
        for i, patch in enumerate(profile['patches']):
            run(['patch', '--batch', '--fuzz=0', '--forward', '-p1', '-d', str(source),
                 '-i', str(ROOT / patch)], f'patch-{i}.log')
        for name, key in [('libssc-client.c', 'patched_report_source_sha256'),
                          ('libssc-sensor.c', 'patched_sensor_source_sha256')]:
            if ssc.digest(source / 'src' / name) != profile[key]:
                raise ValueError('Patched source differs: ' + name)
        run(['protoc-c', '--proto_path=' + str(source / 'data'), '--c_out=' + str(generated),
             str(source / 'data/ssc-common.proto'), str(source / 'data/ssc-sensor-suid.proto')], 'protoc.log')
        flags = shlex.split(subprocess.check_output(
            ['pkg-config', '--cflags', '--libs', 'gio-2.0', 'qmi-glib', 'qrtr-glib', 'libprotobuf-c'], text=True))
        inputs = [str(source / 'src' / n) for n in ('libssc-client.c', 'libssc-common.c', 'libssc-sensor.c')]
        inputs += [str(p) for p in sorted(generated.glob('*.c'))]
        report['binaries_sha256'] = {}
        for variant, sanitizer in [('release', None), ('ubsan', 'undefined'), ('unit', 'address,undefined')]:
            binary = out / ('ssc-inventory-' + variant)
            flags_extra = ['-fsanitize=' + sanitizer, '-fno-sanitize-recover=all'] if sanitizer else []
            target = ROOT / ('tests/ssc-inventory.c' if variant == 'unit' else 'tools/ssc-inventory.c')
            run(['cc', '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                 '-Wno-unused-parameter', *flags_extra, '-I', str(source / 'src'), '-I', str(generated),
                 str(target), *inputs, *flags, '-o', str(binary)], variant + '-build.log')
            if variant == 'unit':
                log = run([str(binary)], 'unit.log')
                if 'SSC_INVENTORY_UNIT_PASS' not in log:
                    raise ValueError('Unit checks did not finish')
                values = [json.loads(line)[0]['values'][0] for line in log.splitlines() if line.startswith('[')]
                if (len(values) != 4 or values[0] != 'line\n"\\\té' or
                        abs(values[1] - 1023.9) > 0.001 or
                        values[2] != {'fixed64': '18446744073709551615'} or values[3] is not False):
                    raise ValueError('Typed metadata did not survive JSON decoding')
            else:
                run([str(binary), '--collect', 'accel'], variant + '-refusal.log', 2)
            report['binaries_sha256'][binary.name] = ssc.digest(binary)
        if ssc.package(bundle) != record or report['inputs_sha256'] != {n: ssc.digest(ROOT / n) for n in names}:
            raise ValueError('Inputs changed during build')
        report['status'] = 'passed'
    except Exception as error:
        report.update(status='failed', error=str(error))
        raise
    finally:
        (out / 'result.json').write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ssc-bundle', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    build(parser.parse_args())
