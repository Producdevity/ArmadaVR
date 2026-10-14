import hashlib
import importlib.util
import math
from pathlib import Path
import struct
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('camera_module', ROOT / 'tools/pico-camera-module.py')
camera = importlib.util.module_from_spec(spec)
spec.loader.exec_module(camera)


def words(*values):
    return struct.pack('<' + 'Q' * len(values), *values)


def fixture():
    driver = bytearray(808)
    for offset, value in ((24, 8), (32, 2), (40, 0xe0), (64, 0x300a), (72, 0x1234), (80, 0xffff)):
        struct.pack_into('<Q', driver, offset, value)
    resolution = bytearray(288)
    values = {1: 1, 2: 4, 3: 800, 4: 500, 7: 24000000, 8: 1, 9: 1,
              11: 1, 14: 14, 18: 1, 19: 5, 30: 1, 31: 9, 34: 1, 35: 10}
    for index, value in values.items():
        struct.pack_into('<Q', resolution, index * 8, value)
    struct.pack_into('<d', resolution, 80, 60)
    return [
        [1, 'sensorDriverData', 1, 0, driver],
        [2, 'sensorName', 0, 0xffffffff, b'ov-test\0'],
        [3, 'resolutionData', 0, 0xffffffff, resolution],
        [4, 'streamConfiguration', 0, 0xffffffff, words(0, 0x2a, 0, 0, 640, 400, 8, 1)],
        [5, 'regSetting', 0, 0xffffffff, words(0, 6, 0x100, 1, 7, 2, 1, 0, 1, 8)],
        [6, 'slaveAddr', 0, 0xffffffff, b''],
        [7, 'registerData', 0, 0xffffffff, words(128)],
        [8, 'delayUs', 0, 0xffffffff, words(1000)],
        [9, 'capability', 0, 0xffffffff, words(0)],
        [10, 'mipiFlags', 0, 0xffffffff, words(1)],
        [11, 'initSettings', 0, 0xffffffff, words(0, 1, 5)],
    ]


def encode(symbols):
    table, data = bytearray(), bytearray()
    for identifier, name, version, mode, content in symbols:
        table += struct.pack('<Q32sQQQQ', identifier, name.encode(), version, mode, len(data), len(content))
        data += content
    header = bytearray(232)
    header[:28] = b'QTI Chromatix Header'.ljust(28, b'\0')
    struct.pack_into('<IHHI', header, 28, 232 + len(table) + len(data) + 32, 2, 0, 0)
    struct.pack_into('<II', header, 152, 160, 3)
    for index, (start, length) in enumerate(((232, len(table)), (232 + len(table), len(data)),
                                           (232 + len(table) + len(data), 32))):
        header[160 + index * 24:184 + index * 24] = words(index, start, length)
    return bytes(header + table + data + bytes(32))


class CameraModuleTests(unittest.TestCase):
    def test_decode_modes_and_register_references(self):
        result = camera.Module(encode(fixture())).inspect()
        self.assertEqual(result['sensor_name'], 'ov-test')
        self.assertEqual(result['slave_address'], 0xe0)
        self.assertEqual(result['sensor_id_register'], 0x300a)
        mode = result['resolutions']['3'][0]
        self.assertEqual(mode['streams'][0]['width'], 640)
        self.assertEqual(mode['streams'][0]['bits'], 8)
        self.assertEqual((mode['fps'], mode['settle_count'], mode['mipi_flags']), (60, 14, [1]))
        self.assertEqual(result['initialization']['11'], [{'selector': 0, 'settings': {'symbol': 5, 'count': 1}}])
        self.assertEqual(result['register_tables']['5'], [{'slave_address': None, 'address': 0x100,
            'values': [128], 'address_type': 2, 'data_type': 1, 'operation': 0, 'delay_us': 1000}])

    def test_truncation_and_wrong_header(self):
        data = encode(fixture())
        for size in (0, 20, 159, 231, len(data) - 1):
            with self.subTest(size=size), self.assertRaises(ValueError):
                camera.Module(data[:size])
        for offset, value in ((0, 0), (28, 1), (32, 1), (152, 0), (156, 4), (160, 3), (168, 0)):
            changed = bytearray(data)
            changed[offset] = value
            with self.subTest(offset=offset), self.assertRaises(ValueError):
                camera.Module(changed)

    def test_overlapping_and_out_of_bounds_sections(self):
        for offset, value in ((192, 232), (200, 0xfffffff8), (224, 40), (216, 240)):
            data = bytearray(encode(fixture()))
            struct.pack_into('<Q', data, offset, value)
            with self.subTest(offset=offset), self.assertRaises(ValueError):
                camera.Module(data)

    def test_invalid_symbol_identity_and_ranges(self):
        for column, value in ((0, 1), (1, 'bad/name')):
            symbols = fixture()
            symbols[1][column] = value
            with self.subTest(value=value), self.assertRaises(ValueError):
                camera.Module(encode(symbols))
        for offset, value in ((232, 0), (232 + 56, 0xfffffff8), (232 + 64, 0xfffffff8)):
            data = bytearray(encode(fixture()))
            struct.pack_into('<Q', data, offset, value)
            with self.subTest(offset=offset), self.assertRaises(ValueError):
                camera.Module(data)

    def test_reference_type_count_and_field_width(self):
        for index, column, value in ((6, 1, 'delayUs'), (6, 2, 1), (6, 4, b''),
                                      (6, 4, words(1 << 32)), (9, 4, words(32)),
                                      (4, 2, 1), (2, 3, 0)):
            symbols = fixture()
            symbols[index][column] = value
            with self.subTest(index=index, value=value), self.assertRaises(ValueError):
                camera.Module(encode(symbols)).inspect()
        symbols = fixture()
        symbols[4][4] = words(2, 6, 0x100, 1, 7, 2, 1, 0, 1, 8)
        with self.assertRaises(ValueError):
            camera.Module(encode(symbols)).inspect()

    def test_unknown_reference_and_incomplete_records(self):
        for index, data in ((4, words(0, 6, 0x100, 1, 99999, 2, 1, 0, 1, 8)),
                            (4, bytes(72)), (2, bytes(280)), (10, bytes(16))):
            symbols = fixture()
            symbols[index][4] = data
            with self.subTest(index=index), self.assertRaises(ValueError):
                camera.Module(encode(symbols)).inspect()

    def test_nonfinite_frame_rate(self):
        for value in (math.nan, math.inf, -1, 0):
            symbols = fixture()
            struct.pack_into('<d', symbols[2][4], 80, value)
            with self.subTest(value=value), self.assertRaises(ValueError):
                camera.Module(encode(symbols)).inspect()

    def test_checksum_regular_file_and_scope(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'module.bin'
            path.write_bytes(encode(fixture()))
            digest = hashlib.sha256(path.read_bytes()).hexdigest()
            result = camera.run(path, digest)
            self.assertFalse(result['active_sensor_verified'])
            self.assertFalse(result['calibration_verified'])
            self.assertEqual(result['device_actions'], [])
            with self.assertRaisesRegex(ValueError, 'checksum'):
                camera.run(path, '0' * 64)
            link = Path(tmp) / 'link'
            link.symlink_to(path)
            with self.assertRaises(ValueError):
                camera.run(link, digest)
