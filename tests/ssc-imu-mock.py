#!/usr/bin/env python3
"""Serve two timestamped IMU streams in the isolated Pico SSC VM."""
import multiprocessing
from pathlib import Path
import runpy
import struct
import sys

if (Path('/proc/device-tree/compatible').read_bytes().split(b'\0')[0] != b'linux,dummy-virt' or
        'armada.ssc_test=1' not in Path('/proc/cmdline').read_text().split()):
    raise SystemExit('This mock requires the isolated SSC VM')
build = Path(sys.argv[1]).resolve()
sys.path[:0] = [str(build / 'mocking'), str(build / 'mocking/ssc_server')]
namespace = runpy.run_path(str(build / 'mocking/ssc_server/ssc-server'), run_name='ssc_mock')
ssc, common = namespace['SSC'], namespace['SscCommon']
fault = sys.argv[2] if len(sys.argv) == 3 else None
assert fault in (None, 'gyro-silent', 'gyro-malformed', 'gyro-stall', 'gyro-unavailable', 'calibration',
                 'bias-normal', 'bias-single', 'bias-wrong-body', 'bias-missing-body',
                 'bias-malformed', 'bias-unreliable', 'bias-silent', 'bias-stall')
calibration = fault == 'calibration'
paired = fault is not None and fault.startswith('bias-')
data = [item for item in ssc._read_data() if item['sensor']['data_type'] not in ('accel', 'gyro')]
uids = {}
names = ('accel', 'gyro', 'accel_cal', 'gyro_cal') if paired else ('accel_cal', 'gyro_cal') if calibration else ('accel', 'gyro')
for index, name in enumerate(names):
    high, low = 0xACC10001 + index, 0xACC20001 + index
    uids[(high, low)] = index
    is_calibration = calibration or index >= 2
    data.append({'sensor': {'data_type': name, 'uid_high': hex(high), 'uid_low': hex(low)},
                 'attributes': {'name': {'id': 0, 'value': name}, 'vendor': {'id': 1, 'value': 'fixture'},
                                'available': {'id': 3, 'value': not (index == 1 and fault == 'gyro-unavailable')},
                                'sample_rate': {'id': 6, 'value': 0.0 if is_calibration else 1000.0},
                                'rigid_body': {'id': 19, 'value': 1 if index == 3 and fault == 'bias-wrong-body' else 0},
                                'stream_type': {'id': 16, 'value': 1 if is_calibration else 0}}, 'measurements': []})
    if index == 3 and fault == 'bias-missing-body':
        del data[-1]['attributes']['rigid_body']
ssc._read_data = staticmethod(lambda: data)


def measurement(high, low, index):
    sensor = uids[(high, low)]
    is_calibration = calibration or sensor >= 2
    message = common.SscClientResponse()
    message.uid.high, message.uid.low = high, low
    event = message.response.add()
    event.msg_id = 1022 if is_calibration else 1025
    event.timestamp = 0x123456789ABCDEF + (index + 1) * 1000
    accuracy = b'\x20\x03' if is_calibration else b'\x10\x03'
    values = (0.25, 0.5, -0.75) if sensor >= 2 else (-0.125 if sensor else 9.75, 2, -3.5)
    event.msg = b'\x0a\x0c' + struct.pack('<3f', *values) + accuracy
    if index % 6 == 1:
        event.msg = accuracy
    elif index % 6 == 2:
        message.uid.high ^= 0x8000000000000000
    elif index % 6 == 4:
        event.msg_id = 768
    if paired and is_calibration:
        if fault == 'bias-single' and index > 0:
            event.msg_id = 1234
        if sensor == 3:
            if fault == 'bias-malformed':
                event.msg = accuracy
            if fault == 'bias-unreliable':
                event.msg = b'\x0a\x0c' + struct.pack('<3f', *values) + b'\x20\x00'
            if fault == 'bias-stall' or (fault == 'bias-silent' and index > 0):
                event.msg_id = 1234
    if sensor == 1:
        if fault == 'gyro-stall':
            event.msg_id = 1234
        elif fault == 'gyro-malformed' or (fault == 'gyro-silent' and index >= 6):
            event.msg = b'\x10\x03'
            event.msg_id = 1025
    return message.SerializeToString()


ssc.generate_protobuf_sensor_measurement = staticmethod(measurement)
multiprocessing.set_start_method('fork')
namespace['SSCMockingServer']().start()
