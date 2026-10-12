#!/usr/bin/env python3
"""Read Neo3 b1977 controller configuration without changing the headset."""
import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import re
import subprocess

FINGERPRINT = 'Pico/A7H10/PICOA7H10:10/5.8.4.0/smartcm.1696861506:user/test-keys'
LIBRARY = '/system/lib64/libstationopticsservice.so'
LIBRARY_SHA256 = '91b45470678ca7d614a7f12ccb21261f3cda273c4ba6b08c1edb153cd5b6286e'
CONFIG_APK = '/system/priv-app/configserverservice/configserverservice.apk'
CONFIG_SHA256 = '701ac92f7d5598f0f7dbd7fa5bdd276cbecc19c967e5472c3090396fb39f595f'


def parcel(text):
    match = re.fullmatch(r"Result: Parcel\(((?:[0-9a-fA-F]{8}\s+)+)'[^'\r\n]*'\)", text.strip())
    if not match:
        raise ValueError('Unexpected Binder reply')
    words = [int(word, 16) for word in match[1].split()]
    if not words or words[0] != 0:
        raise ValueError('Binder getter returned an exception')
    return words


def scalar(text, allowed):
    words = parcel(text)
    if len(words) != 2 or words[1] not in allowed:
        raise ValueError('Invalid controller scalar')
    return words[1]


def digit(text):
    words = parcel(text)
    if len(words) != 3 or words[1] != 1 or words[2] not in (ord('1'), ord('2')):
        raise ValueError('Invalid controller configuration string')
    return words[2] - ord('0')


def snapshot(read):
    radio = scalar(read('service', 'call', 'PicoStation-new', '76'), (1, 2))
    touch = scalar(read('service', 'call', 'PicoStation-new', '74'), (0, 1))
    values = []
    for key in ('sys_controller_chip', 'sys_controller_prototype'):
        values.append(digit(read('service', 'call', 'ConfigurationService', '3',
                                 's16', 'com.pvr.configuration', 'i32', '0',
                                 's16', key, 'i32', '-1')))
    if values[1] != radio:
        raise ValueError('Live radio type and stored configuration disagree')
    return {'mcu_type': values[0], 'radio_type': radio, 'touch_supported': bool(touch)}


def collect(read):
    fingerprint = read('getprop', 'ro.build.fingerprint')
    device = read('getprop', 'ro.product.device')
    if fingerprint != FINGERPRINT or device != 'PICOA7H10':
        raise ValueError('Unsupported Pico firmware or device; no Binder calls made')
    for path, digest in ((LIBRARY, LIBRARY_SHA256), (CONFIG_APK, CONFIG_SHA256)):
        if read('sha256sum', path).split() != [digest, path]:
            raise ValueError('Stock interface hash mismatch; no Binder calls made')
    boot = read('cat', '/proc/sys/kernel/random/boot_id')
    if not re.fullmatch(r'[0-9a-f]{8}(?:-[0-9a-f]{4}){3}-[0-9a-f]{12}', boot):
        raise ValueError('Invalid Android boot identity')
    config = snapshot(read)
    if snapshot(read) != config:
        raise ValueError('Controller configuration changed while reading')
    if read('cat', '/proc/sys/kernel/random/boot_id') != boot or \
            read('getprop', 'ro.build.fingerprint') != fingerprint:
        raise ValueError('Device restarted or firmware changed while reading')
    return {'device': device, 'fingerprint': fingerprint, 'boot_id': boot, 'controller': config}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--adb', default='adb')
    parser.add_argument('--serial', required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    # Reserve the evidence file before contacting the device; never overwrite it.
    with args.output.open('x') as output:
        report = {'status': 'started', 'serial': args.serial, 'read_only': True,
                  'utc': datetime.now(timezone.utc).isoformat(), 'reads': []}

        def read(*command):
            result = subprocess.run([args.adb, '-s', args.serial, 'shell', *command],
                                    capture_output=True, text=True, timeout=15)
            report['reads'].append({'command': command, 'returncode': result.returncode,
                                    'stdout': result.stdout, 'stderr': result.stderr})
            if result.returncode or result.stderr or len(result.stdout) > 1024:
                raise ValueError('ADB read failed: ' + ' '.join(command))
            return result.stdout.strip()

        try:
            report.update(collect(read), status='passed')
        except Exception as error:
            report.update(status='failed', error=str(error))
            raise
        finally:
            json.dump(report, output, indent=2)
            output.write('\n')
    print(json.dumps(report['controller'], sort_keys=True))


if __name__ == '__main__':
    main()
