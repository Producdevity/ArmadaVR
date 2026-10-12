#!/usr/bin/env python3
"""Build the Pico SSC session and test cancellation without QRTR hardware."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
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
    parser.add_argument('--ssc-bundle', type=Path, required=True)
    parser.add_argument('--cc', default='cc')
    parser.add_argument('--cxx', default='c++')
    parser.add_argument('--sanitizer', choices=('address,undefined', 'none'), default='address,undefined')
    args = parser.parse_args()
    if sys.platform != 'linux':
        raise ValueError('Use a Linux ARM64 environment for the native SSC bundle')
    profile = json.loads((ROOT / 'profiles/monado.json').read_text())
    if digest(args.archive) != profile['full_source']['sha256']:
        raise ValueError('Monado archive differs from the pinned revision')
    spec = importlib.util.spec_from_file_location('ssc_artifact', ROOT / 'tools/ssc-artifact.py')
    ssc_artifact = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(ssc_artifact)
    ssc_bundle = args.ssc_bundle.resolve()
    ssc_record = ssc_artifact.package(ssc_bundle)
    ssc_prefix = ssc_bundle / ssc_artifact.PREFIX
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    source = output / 'source' / profile['full_source']['archive_root']
    with tarfile.open(args.archive) as stream:
        stream.extractall(output / 'source', filter='data')
    ssc_source = output / 'ssc-source'
    with tarfile.open(ssc_prefix / 'share/ssc/source.tar.gz') as stream:
        stream.extractall(ssc_source, filter='data')
    ssc_header = next(ssc_source.rglob('libssc-sensor.h'))
    if digest(ssc_header) != json.loads((ROOT / 'profiles/ssc.json').read_text())['test_source_sha256']['src/libssc-sensor.h']:
        raise ValueError('SSC header differs from pinned source')
    pkg = shlex.split(subprocess.check_output(['pkg-config', '--cflags', '--libs', 'gio-2.0'], text=True))
    auxiliary = source / 'src/xrt/auxiliary'
    include = source / 'src/xrt/include'
    generated = output / 'include/xrt'
    generated.mkdir(parents=True)
    template = (include / 'xrt/xrt_config_build.h.cmake_in').read_text()
    (generated / 'xrt_config_build.h').write_text(re.sub(r'^#cmakedefine .+$', '', template, flags=re.MULTILINE))
    inputs = [ROOT / name for name in ['tests/ssc-session.c', 'src/pico-ssc-session.c', 'src/pico-monado-device.c', 'src/pico-monado-input.c',
                                       'src/pico-controller.c', 'src/pico-controller-stream.c', 'src/ssc-clock.c']]
    inputs += [auxiliary / ('util/' + name) for name in [
        'u_device.c', 'u_device_ni.c', 'u_device_id.c', 'u_limited_unique_id.cpp',
        'u_visibility_mask.c', 'u_debug.c', 'u_logging.c',
    ]]
    inputs.append(source / 'src/external/cjson/cjson/cJSON.c')
    flags = ['-O1', '-g', '-ffunction-sections', '-fdata-sections', '-pthread',
             '-I', str(ROOT / 'src'), '-I', str(ssc_header.parent), *pkg, '-I', str(output / 'include'),
             '-isystem', str(include), '-isystem', str(auxiliary),
             '-isystem', str(source / 'src/external/cjson')]
    sanitize = [] if args.sanitizer == 'none' else ['-fsanitize=' + args.sanitizer, '-fno-sanitize-recover=all']
    report = {'status': 'started', 'monado_commit': profile['commit'], 'archive_sha256': digest(args.archive),
              'commands': [], 'sanitizer': args.sanitizer, 'hardware_verified': False, 'ssc_bundle': ssc_record,
              'scope': 'Actual SSC and Monado code; cancelled startup and absent QRTR transport only. Stream behavior requires the isolated kernel VM.'}

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
            warning = ['-Wall', '-Wextra', '-Werror'] if path in inputs[:7] else ['-w']
            run([compiler, standard, *flags, *sanitize, *warning, '-c', str(path), '-o', str(obj)], name + '-compile.log')
            objects.append(str(obj))
        binary = output / 'ssc-session'
        run([args.cxx, *sanitize, *objects, '-pthread', '-lm', '-Wl,--gc-sections', '-Wl,--as-needed',
             '-L', str(ssc_prefix / 'lib64'), '-l:libssc.so.2', *pkg, '-o', str(binary)], 'link.log')
        run([str(binary), 'no-qrtr'], 'test.log', timeout=180,
            env=dict(os.environ, UBSAN_OPTIONS='halt_on_error=1', ASAN_OPTIONS='detect_leaks=1',
                     LD_LIBRARY_PATH=str(ssc_prefix / 'lib64')))
        log = (output / 'test.log').read_text()
        if log.count('SSC_SESSION_PASS mode=no-qrtr') != 100:
            raise ValueError('Session tests did not complete')
        print('SSC_SESSION_PASS cycles=100')
        report['status'] = 'passed'
    except Exception as error:
        report.update(status='failed', error=str(error))
        raise
    finally:
        report['inputs_sha256'] = {str(path): digest(path) for path in inputs}
        report['headers_sha256'] = {str(path.relative_to(ROOT)): digest(path) for path in (ROOT / 'src').glob('*.h')}
        report['runner_sha256'] = digest(Path(__file__))
        report['ssc_header_sha256'] = digest(ssc_header)
        if (output / 'ssc-session').is_file():
            report['binary_sha256'] = digest(output / 'ssc-session')
        report['generated_config_sha256'] = digest(generated / 'xrt_config_build.h')
        (output / 'result.json').write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    main()
