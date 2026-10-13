#!/usr/bin/env python3
"""Drive full-runtime input tests through the isolated VM's real QRTR stack."""
import ctypes
import multiprocessing
from pathlib import Path
import runpy
import sys

if (Path('/proc/device-tree/compatible').read_bytes().split(b'\0')[0] != b'linux,dummy-virt' or
        'reframed.ssc_test=1' not in Path('/proc/cmdline').read_text().split()):
    raise SystemExit('This mock requires the isolated SSC test VM')


def install(ssc, common, counter):
    original_measurement = ssc.generate_protobuf_sensor_measurement
    uid = (0xBEE10001, 0xBEE20001)
    data = ssc._read_data()
    data.append({'sensor': {'data_type': 'ble_mcu', 'uid_high': hex(uid[0]), 'uid_low': hex(uid[1])},
                 'attributes': {'name': {'id': 0, 'value': 'ble_mcu'},
                                'vendor': {'id': 1, 'value': 'test'},
                                'available': {'id': 3, 'value': True},
                                'sample_rate': {'id': 6, 'value': 480.0},
                                'stream_type': {'id': 16, 'value': 0}},
                 'measurements': []})
    ssc._read_data = staticmethod(lambda: data)

    def measurement(high, low, index):
        if (high, low) != uid:
            return original_measurement(high, low, index)
        phase = Path('/controller-phase').read_text()
        left = index % 2 == 0
        pressed = phase == ('left' if left else 'right')
        record = bytearray(100)
        record[0], record[1] = 0xA5, 1
        record[7] = (0x40 if left else 0) | (4 if pressed else 0)
        record[8] = int(pressed)
        crc = 0
        for byte in record[:99]:
            crc ^= byte
            for _ in range(8):
                crc = (crc >> 1) ^ (0x8C if crc & 1 else 0)
        record[99] = crc
        message = common.SscClientResponse()
        message.uid.high, message.uid.low = uid
        event = message.response.add()
        event.msg_id = 1024
        event.timestamp = counter()
        event.msg = b'\x0a\x64' + record + b'\x10\x03' if phase != 'malformed' else b'\x10\x03'
        return message.SerializeToString()

    ssc.generate_protobuf_sensor_measurement = staticmethod(measurement)


if __name__ == '__main__':
    build = Path(sys.argv[1]).resolve()
    sys.path[:0] = [str(build / 'mocking'), str(build / 'mocking/ssc_server')]
    namespace = runpy.run_path(str(build / 'mocking/ssc_server/ssc-server'), run_name='ssc_mock')
    counter = ctypes.CDLL('/ssc-test-clock.so').ssc_test_ticks
    counter.restype = ctypes.c_uint64
    install(namespace['SSC'], namespace['SscCommon'], counter)
    multiprocessing.set_start_method('fork')
    namespace['SSCMockingServer']().start()
