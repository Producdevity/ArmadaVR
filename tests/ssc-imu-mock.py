#!/usr/bin/env python3
"""Serve two timestamped IMU streams in the isolated Pico SSC VM."""
import ctypes
import multiprocessing
import os
from pathlib import Path
import runpy
import struct
import sys
import time

if (Path('/proc/device-tree/compatible').read_bytes().split(b'\0')[0] != b'linux,dummy-virt' or
        'reframed.ssc_test=1' not in Path('/proc/cmdline').read_text().split()):
    raise SystemExit('This mock requires the isolated SSC VM')
build = Path(sys.argv[1]).resolve()
sys.path[:0] = [str(build / 'mocking'), str(build / 'mocking/ssc_server')]
namespace = runpy.run_path(str(build / 'mocking/ssc_server/ssc-server'), run_name='ssc_mock')
ssc, common = namespace['SSC'], namespace['SscCommon']
fault = sys.argv[2] if len(sys.argv) == 3 else None
assert fault in (None, 'gyro-silent', 'gyro-malformed', 'gyro-stall', 'gyro-unavailable', 'calibration',
                 'bias-normal', 'bias-single', 'bias-wrong-body', 'bias-missing-body',
                 'bias-malformed', 'bias-gyro-malformed', 'bias-unreliable', 'bias-silent', 'bias-stall',
                 'multi', 'bias-multi', 'duplicate-uid', 'bias-duplicate-uid', 'empty-uid', 'bias-empty-uid',
                 'rates', 'bias-rates', 'rate-missing', 'rate-malformed', 'bias-rate-malformed')
hmd_runtime = os.environ.get('SSC_TEST_HMD_RUNTIME') == '1'
if hmd_runtime:
    assert fault == 'bias-normal'
    clock = ctypes.CDLL('/ssc-test-clock.so')
    clock.ssc_test_ticks.restype = ctypes.c_uint64
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
                                'sample_rate': {'id': 6, 'value': 0.0 if is_calibration else 100.0 if hmd_runtime else 1000.0},
                                'rigid_body': {'id': 19, 'value': 1 if index == 3 and fault == 'bias-wrong-body' else 0},
                                'stream_type': {'id': 16, 'value': 1 if is_calibration else 0}}, 'measurements': []})
    if index == 3 and fault == 'bias-missing-body':
        del data[-1]['attributes']['rigid_body']
    if index < 2 and fault in ('rates', 'bias-rates', 'rate-missing', 'rate-malformed', 'bias-rate-malformed'):
        data[-1]['attributes']['sample_rate']['value'] = [5.0, 1000.0, 2000.0]
        if index == 1:
            if fault == 'rate-missing':
                del data[-1]['attributes']['sample_rate']
            if fault.endswith('rate-malformed'):
                data[-1]['attributes']['sample_rate']['value'] = [5.0, float('nan'), 1000.0]
ssc._read_data = staticmethod(lambda: data)

original_client_request = ssc.parse_protobuf_client_request


def client_request(message):
    result = original_client_request(message)
    if result[0] == 513 and (result[1], result[2]) in uids:
        envelope = common.SscClientRequest()
        envelope.ParseFromString(message)
        config = common.SscEnableConfigRequest()
        config.ParseFromString(envelope.request.msg)
        assert config.sample_rate == (100.0 if hmd_runtime else 1000.0)
        print('SSC_TEST_RATE', result[1], result[2], config.sample_rate, flush=True)
    return result


ssc.parse_protobuf_client_request = staticmethod(client_request)

selection_faults = ('multi', 'bias-multi', 'duplicate-uid', 'bias-duplicate-uid', 'empty-uid', 'bias-empty-uid')
if fault in selection_faults:
    from ssc_server import ssc_sensor_suid_pb2 as suid

    original_parse = ssc.parse_protobuf_discovery_request
    original_discovery = ssc.generate_protobuf_discovery_response

    def discovery_request(message):
        datatype = original_parse(message)
        if datatype in names:
            envelope = common.SscClientRequest()
            envelope.ParseFromString(message)
            request = suid.SscSuidRequest()
            request.ParseFromString(envelope.request.msg)
            assert request.HasField('only_default_values') and not request.only_default_values
        return datatype

    def discovery_response(datatype):
        message = original_discovery(datatype)
        if datatype not in names:
            return message
        envelope = common.SscClientResponse()
        envelope.ParseFromString(message)
        response = suid.SscSuidResponse()
        response.ParseFromString(envelope.response[0].msg)
        assert len(response.uid) == 1
        target = common.SscUid()
        target.CopyFrom(response.uid[0])
        del response.uid[:]
        decoy = response.uid.add()
        decoy.high, decoy.low = 0xBAD00001, 0xBAD00002
        response.uid.add().CopyFrom(target)
        decoy = response.uid.add()
        decoy.high, decoy.low = 0xBAD00003, 0xBAD00004
        if datatype == ('gyro_cal' if paired else 'gyro'):
            if fault.endswith('duplicate-uid'):
                response.uid.add().CopyFrom(target)
            if fault.endswith('empty-uid'):
                del response.uid[:]
        envelope.response[0].msg = response.SerializeToString()
        return envelope.SerializeToString()

    ssc.parse_protobuf_discovery_request = staticmethod(discovery_request)
    ssc.generate_protobuf_discovery_response = staticmethod(discovery_response)


def measurement(high, low, index):
    sensor = uids[(high, low)]
    is_calibration = calibration or sensor >= 2
    message = common.SscClientResponse()
    message.uid.high, message.uid.low = high, low
    event = message.response.add()
    event.msg_id = 1022 if is_calibration else 1025
    step = 1920000 if os.environ.get('SSC_TEST_HEAD_TIMING') == '1' else 1000
    event.timestamp = 0x123456789ABCDEF + (index + 1) * step
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
        elif fault in ('gyro-malformed', 'bias-gyro-malformed') or (fault == 'gyro-silent' and index >= 6):
            event.msg = b'\x10\x03'
            event.msg_id = 1025
    if hmd_runtime:
        message.uid.high, message.uid.low = high, low
        event.timestamp = index * 192000
        values = (0.25, 0.5, -0.75) if is_calibration else ((0.25, 10.31, -0.75) if sensor == 0 else (0.25, 0.85, -0.75))
        event.msg_id = (1022 if is_calibration else 1025) if Path('/hmd-phase').read_text() == 'running' else 1234
        event.msg = b'\x0a\x0c' + struct.pack('<3f', *values) + accuracy
    return message.SerializeToString()


ssc.generate_protobuf_sensor_measurement = staticmethod(measurement)
if hmd_runtime:
    runpy.run_path('/controller-mock.py', run_name='pico_controller_mock')['install'](ssc, common, clock.ssc_test_ticks)

    def runtime_measurements(self, high, low, node, port, transaction_id):
        sensor = uids.get((high, low))
        period = 192000 * (10 if sensor in (2, 3) else 5 if sensor is None else 1)
        deadline = clock.ssc_test_ticks() // period * period
        sequence = 0
        while True:
            now = clock.ssc_test_ticks()
            if now < deadline:
                time.sleep((deadline - now) / 19200000)
                continue
            # Both IMUs capture on the same 100 Hz clock. Missed synthetic
            # samples are dropped, rather than replayed with stale timestamps.
            capture = now // period * period
            index = sequence if sensor is None else capture // 192000
            payload = ssc.generate_protobuf_sensor_measurement(high, low, index)
            packet = ssc.generate_report_large_indication(payload, transaction_id)
            if self._qrtr.qrtr_sendto(self.sock, node, port, ctypes.c_char_p(packet), len(packet)) != 0:
                break
            sequence += 1
            deadline = capture + period

    namespace['SSCMockingServer']._sensor_measurement = runtime_measurements
multiprocessing.set_start_method('fork')
namespace['SSCMockingServer']().start()
