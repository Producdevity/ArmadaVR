#!/usr/bin/env python3
"""Verify Pico graphics bundle loading and opt-in controllers without headset devices."""
import argparse
import errno
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import platform
import socket
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def module(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / 'tools' / (name + '.py'))
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('monado', 'turnip', 'ssc', 'output'):
        parser.add_argument('--' + name, type=Path, required=True)
    args = parser.parse_args()
    if sys.platform != 'linux' or platform.machine() != 'aarch64':
        raise ValueError('Run inside an isolated native ARM64 Linux container')
    if any(Path(p).exists() for p in ('/dev/kgsl-3d0', '/dev/dri', '/dev/bus/usb')):
        raise ValueError('Do not expose graphics or USB devices to this offline test')
    try:
        transport = socket.socket(42, socket.SOCK_DGRAM)
    except OSError as error:
        if error.errno not in (errno.EAFNOSUPPORT, errno.EPROTONOSUPPORT):
            raise
    else:
        transport.close()
        raise ValueError('This test requires a kernel without QRTR transport')
    monado, turnip, ssc = [module(n + '-artifact') for n in ('monado', 'turnip', 'ssc')]
    args.monado, args.turnip, args.ssc = [p.resolve() for p in (args.monado, args.turnip, args.ssc)]

    def inventory():
        result = {'monado': monado.package(args.monado), 'turnip': turnip.validate(args.turnip),
                  'ssc': ssc.package(args.ssc)}
        if result['monado'].get('ssc_dependency') != result['ssc']:
            raise ValueError('Monado was not built with this exact SSC bundle')
        return result

    original = inventory()
    output = args.output.resolve()
    if any(output.is_relative_to(p) or p.is_relative_to(output) for p in (args.monado, args.turnip, args.ssc)):
        raise ValueError('Test output must be separate from the input bundles')
    output.mkdir(parents=True, exist_ok=False)
    mono = args.monado / monado.PREFIX
    sensor = args.ssc / ssc.PREFIX
    environment = {'PATH': '/usr/bin:/bin', 'HOME': str(output), 'XDG_CONFIG_HOME': str(output / 'config'),
                   'LD_LIBRARY_PATH': str(sensor / 'lib64') + ':' + str(mono / 'lib64'),
                   'LD_BIND_NOW': '1', 'P_LOG': 'debug'}
    report = {'status': 'started', 'bundles': original, 'commands': [], 'results': {},
              'hardware_verified': False, 'rendering_verified': False, 'device_actions': [],
              'scope': 'Native symbol loading and controller registration without QRTR, USB or graphics devices',
              'runner_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
              'environment': environment, 'platform': platform.uname()._asdict()}

    def run(name, command, extra=None):
        report['commands'].append({'name': name, 'argv': command, 'environment_overrides': extra or {}})
        result = subprocess.run(command, capture_output=True, text=True, env=dict(environment, **(extra or {})), timeout=30)
        text = result.stdout + result.stderr
        (output / (name + '.log')).write_text(text)
        report['results'][name] = {'returncode': result.returncode}
        return result.returncode, text

    try:
        for name, path in [('service', mono / 'bin/monado-service'), ('cli', mono / 'bin/monado-cli'),
                           ('turnip', args.turnip / turnip.LIBRARY), ('ssc', sensor / 'bin/ssccli')]:
            code, text = run('loader-' + name, ['ldd', '-r', str(path)])
            if code or 'not found' in text or 'undefined symbol' in text:
                raise ValueError('Unresolved runtime dependency: ' + name)
        libraries = [mono / 'lib64/libopenxr_monado.so', mono / 'lib64/libmonado.so',
                     args.turnip / turnip.LIBRARY, sensor / 'lib64/libssc.so.2']
        code, text = run('immediate-loading', [sys.executable, '-c',
            'import ctypes,os,sys; loaded=[ctypes.CDLL(p,mode=os.RTLD_NOW|os.RTLD_LOCAL) for p in sys.argv[1:]]; print("PICO_LIBRARY_LOAD_PASS")',
            *map(str, libraries)])
        if code or 'PICO_LIBRARY_LOAD_PASS' not in text:
            raise ValueError('Immediate library loading failed')
        fixture = output / 'synthetic-config.json'
        fixture.write_text(json.dumps({'status': 'passed', 'read_only': True, 'device': 'PICOA7H10',
            'fingerprint': 'Pico/A7H10/PICOA7H10:10/5.8.4.0/smartcm.1696861506:user/test-keys',
            'controller': {'mcu_type': 1, 'radio_type': 2, 'touch_supported': True}}) + '\n')
        modes = [('disabled', {}), ('missing-counter', {'PICO_SSC_CONFIG': str(fixture)}),
                 ('enabled', {'PICO_SSC_CONFIG': str(fixture), 'PICO_SSC_COUNTER': '1'}),
                 ('invalid', {'PICO_SSC_CONFIG': str(output / 'absent.json'), 'PICO_SSC_COUNTER': '1'})]
        for name, extra in modes:
            code, text = run(name, [str(mono / 'bin/monado-cli'), 'test'], extra)
            if code or any(('Pico Neo3 ' + hand + ' controller' in text) != (name == 'enabled') for hand in ('left', 'right')):
                raise ValueError('Unexpected controller registration: ' + name)
        code, text = run('missing-ssc', [str(mono / 'bin/monado-cli'), 'test'],
                         {'LD_LIBRARY_PATH': str(mono / 'lib64')})
        if not code or 'libssc.so.2' not in text:
            raise ValueError('Missing SSC dependency did not refuse startup')
        if inventory() != original:
            raise ValueError('Input bundles changed during testing')
        report['status'] = 'passed'
    except Exception as error:
        report.update(status='failed', error=str(error))
        raise
    finally:
        report['logs_sha256'] = {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(output.glob('*.log'))}
        (output / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
    print('PICO_USERSPACE_PASS')


if __name__ == '__main__':
    main()
