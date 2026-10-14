#!/usr/bin/env python3
"""Read Neo3 b1977 controller configuration without changing the headset."""
import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import re
import subprocess
import struct

FINGERPRINT = 'Pico/A7H10/PICOA7H10:10/5.8.4.0/smartcm.1696861506:user/test-keys'
LIBRARY = '/system/lib64/libstationopticsservice.so'
LIBRARY_SHA256 = '91b45470678ca7d614a7f12ccb21261f3cda273c4ba6b08c1edb153cd5b6286e'
CONFIG_APK = '/system/priv-app/configserverservice/configserverservice.apk'
CONFIG_SHA256 = '701ac92f7d5598f0f7dbd7fa5bdd276cbecc19c967e5472c3090396fb39f595f'

MOTION_LIBRARIES = (
    ('/system/lib64/libstationclient.pxr.so',
     '07a69e946bb7be2da582400de515855bb3d73fed1ec37f7ea1bab3f2f0e598d2'),
    ('/system/lib64/libpvrtrackingservice.so',
     '4b7f74b80607a2863559ab6496f4a8abe6778ccaec834074a923a2064cfe0a9a'),
    ('/system/lib64/libbinder.so',
     '0da05225ebbcd5c9c94a77e9ba1c09c7e211be2eabe535bcb66853aa95fcc034'),
)


def motion_status(text):
    lines = text.strip().splitlines()
    if len(lines) != 9 or lines[0] != 'Result: Parcel(' or lines[-1] != ')':
        raise ValueError('Unexpected controller status reply')
    words = []
    for index, line in enumerate(lines[1:-1]):
        match = re.fullmatch(r"\s*0x([0-9a-fA-F]{8}):\s+((?:[0-9a-fA-F]{8}\s+){4})'[^\r\n]*'", line)
        if not match or int(match[1], 16) != index * 16:
            raise ValueError('Invalid controller status framing')
        words.extend(int(word, 16) for word in match[2].split())
    if words[:4] != [0, 0, 1, 18]:
        raise ValueError('Controller status failed or has an unsupported payload')
    raw = struct.pack('<24I', *words[4:])
    result = {}
    for index, hand in enumerate(('left', 'right')):
        record = raw[index * 48:(index + 1) * 48]
        connected, = struct.unpack_from('<I', record)
        if connected not in (0, 1):
            raise ValueError('Unsupported controller connection state')
        result[hand] = {
            'connected': bool(connected), 'revision_byte': record[15],
            'double_scale_byte': record[42], 'axis_reversal_bypass_byte': record[43],
        }
    return {'settings': result, 'raw_status_hex': raw.hex()}


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


def collect(read, *, include_motion_status=False):
    fingerprint = read('getprop', 'ro.build.fingerprint')
    device = read('getprop', 'ro.product.device')
    if fingerprint != FINGERPRINT or device != 'PICOA7H10':
        raise ValueError('Unsupported Pico firmware or device; no Binder calls made')
    interfaces = ((LIBRARY, LIBRARY_SHA256), (CONFIG_APK, CONFIG_SHA256))
    if include_motion_status:
        interfaces += MOTION_LIBRARIES
    for path, digest in interfaces:
        if read('sha256sum', path).split() != [digest, path]:
            raise ValueError('Stock interface hash mismatch; no Binder calls made')
    boot = read('cat', '/proc/sys/kernel/random/boot_id')
    if not re.fullmatch(r'[0-9a-f]{8}(?:-[0-9a-f]{4}){3}-[0-9a-f]{12}', boot):
        raise ValueError('Invalid Android boot identity')
    config = snapshot(read)
    status = []
    if include_motion_status:
        status.append(motion_status(read('service', 'call', 'PicoStation-new', '19')))
    if snapshot(read) != config:
        raise ValueError('Controller configuration changed while reading')
    if include_motion_status:
        status.append(motion_status(read('service', 'call', 'PicoStation-new', '19')))
        if status[0]['settings'] != status[1]['settings']:
            raise ValueError('Controller connection or motion settings changed while reading')
    if read('cat', '/proc/sys/kernel/random/boot_id') != boot or \
            read('getprop', 'ro.build.fingerprint') != fingerprint:
        raise ValueError('Device restarted or firmware changed while reading')
    result = {'device': device, 'fingerprint': fingerprint, 'boot_id': boot, 'controller': config}
    if include_motion_status:
        result['motion_status'] = status
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--adb', default='adb')
    parser.add_argument('--serial', required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--motion-status', action='store_true',
                        help='also capture the stock controller motion configuration')
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
            if result.returncode or result.stderr or len(result.stdout) > 4096:
                raise ValueError('ADB read failed: ' + ' '.join(command))
            return result.stdout.strip()

        try:
            report.update(collect(read, include_motion_status=args.motion_status), status='passed')
        except Exception as error:
            report.update(status='failed', error=str(error))
            raise
        finally:
            json.dump(report, output, indent=2)
            output.write('\n')
    print(json.dumps(report['controller'], sort_keys=True))


if __name__ == '__main__':
    main()
