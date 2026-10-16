import hashlib
import importlib.util
from pathlib import Path
import struct
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('quest_module', ROOT / 'tools/quest-camera-module.py')
camera = importlib.util.module_from_spec(spec)
spec.loader.exec_module(camera)


def words(*values):
    return struct.pack('<' + 'I' * len(values), *values)


def fixture():
    driver, config, mode, stream = bytearray(600), bytearray(262), bytearray(260), bytearray(116)
    def set_fields(data, fields):
        for offset, value in fields.items():
            struct.pack_into('<I', data, offset, value)
    set_fields(driver, {0: 1, 12: 4, 16: 3, 52: 0xa0, 56: 2, 60: 1, 64: 0x1234, 68: 0x5678,
                        72: 0xffff, 76: 1, 80: 1, 84: 4, 88: 1, 92: 5, 240: 1, 244: 6,
                        368: 1, 372: 10, 376: 0, 380: 2, 448: 1, 452: 14})
    struct.pack_into('<H', config, 0, 5)
    set_fields(config, {18: 4, 22: 3, 26: 1, 30: 18, 98: 1, 102: 19, 110: 1, 114: 20, 118: 1, 122: 21})
    set_fields(mode, {16: 4, 20: 1, 24: 9, 36: 1000, 40: 800, 52: 48000000, 56: 2, 60: 1,
                      80: 2, 100: 18, 104: 1, 124: 1, 128: 10})
    struct.pack_into('<d', mode, 64, 59.94)
    set_fields(stream, {0: 2, 4: 22, 8: 2, 12: 23, 16: 0x2b, 28: 3, 32: 4, 36: 640,
                        40: 481, 52: 10, 64: 1})
    values = [('sensorDriverData', driver, 0), ('', b'', 0xffffffff), ('sensorName', b'test', 0xffffffff),
              ('powerSetting', words(1, 24000000, 5), 0xffffffff),
              ('powerSetting', words(2, 0, 3), 0xffffffff),
              ('resolutionInfo', words(1, 7, 1, 8), 0xffffffff), ('sensorVersion', words(9), 0xffffffff),
              ('resolutionData', mode, 0xffffffff), ('streamConfiguration', stream, 0xffffffff),
              ('regSetting', words(1, 11, 0x100, 2, 12, 2, 1, 0, 1, 13), 0xffffffff),
              ('slaveAddr', struct.pack('<H', 0xe0), 0xffffffff), ('registerData', words(1, 2), 0xffffffff),
              ('delayUs', words(1500), 0xffffffff), ('initSettings', words(7, 1, 10), 0xffffffff),
              ('cameraModuleData', words(1, 0, 0, 1, 16), 0), ('moduleConfiguration', config, 0xffffffff),
              ('reserved', b'', 0xffffffff), ('sensorSlaveAddress', struct.pack('<H', 0xa2), 0xffffffff),
              ('sensorI2CFrequencyMode', words(3), 0xffffffff), ('laneAssign', struct.pack('<H', 0x3210), 0xffffffff),
              ('isComboMode', b'\1', 0xffffffff), ('vc', words(0, 2), 0xffffffff), ('vcMap', words(1, 3), 0xffffffff)]
    return [[i + 1, name, 1, tag, bytearray(data)] for i, (name, data, tag) in enumerate(values)]


def encode(symbols):
    table, data = bytearray(), bytearray()
    for identifier, name, version, mode, content in symbols:
        table += struct.pack('<I32sQIII', identifier, name.encode(), version, mode, len(data), len(content))
        data += content
    header = bytearray(204)
    header[:28] = b'QTI Chromatix Header'.ljust(28, b'\0')
    struct.pack_into('<IHHI', header, 28, 204 + len(table) + len(data) + 20, 3, 0, 1)
    struct.pack_into('<QII', header, 152, 1, 168, 3)
    for kind, (offset, size) in enumerate([(204, len(table)), (204 + len(table), len(data)),
                                         (204 + len(table) + len(data), 20)]):
        struct.pack_into('<III', header, 168 + kind * 12, kind, offset, size)
    return bytes(header + table + data + bytes(20))


class QuestCameraModuleTests(unittest.TestCase):
    def test_compact_fields_and_optional_references(self):
        result = camera.Module(encode(fixture())).inspect()
        self.assertEqual(result['sensor_name'], 'test')
        self.assertEqual(result['configurations'], [{'camera_id': 5, 'sensor_name': 'test', 'slave_address': 0xa2,
                                                   'frequency': 3, 'lane_assign': 0x3210, 'combo': 1}])
        self.assertEqual(result['power_up'], [{'sequence_type': 1, 'value': 24000000, 'delay_ms': 5}])
        group = result['resolution_groups'][0]
        self.assertEqual(group['sensor_version'], 9)
        mode = group['modes'][0]
        self.assertEqual((mode['fps'], mode['pixel_clock'], mode['lanes'], mode['cphy']), (59.94, 48000000, 2, 1))
        self.assertEqual(mode['streams'], [{'virtual_channels': [0, 2], 'virtual_channel_map': [1, 3],
                                          'data_type': 43, 'x': 3, 'y': 4, 'width': 640, 'height': 481, 'bits': 10, 'type': 1}])
        self.assertEqual(mode['settings'], [{'slave_address': 0xe0, 'address': 0x100, 'values': [1, 2],
                                            'address_type': 2, 'data_type': 1, 'operation': 0, 'delay_us': 1500}])
        self.assertEqual(result['stream_off'], [])
        self.assertEqual(result['initialization'], [{'selector': 7, 'settings': result['stream_on']}])

    def test_optional_absence_uses_empty_shared_symbol(self):
        symbols = fixture()
        for offset in [26, 98, 110, 118]:
            struct.pack_into('<II', symbols[15][4], offset, 0, 2)
        result = camera.Module(encode(symbols)).inspect()['configurations'][0]
        for name in ['slave_address', 'frequency', 'lane_assign', 'combo']:
            self.assertIsNone(result[name])

    def test_every_truncated_prefix_is_rejected(self):
        data = encode(fixture())
        for size in range(len(data)):
            with self.subTest(size=size), self.assertRaises(ValueError):
                camera.Module(data[:size]).inspect()

    def test_rejects_container_and_symbol_corruption(self):
        data = encode(fixture())
        for offset, value in [(28, len(data) + 1), (32, 2), (36, 0), (152, 5), (160, 160), (164, 4),
                              (172, 203), (180, 0), (184, 204), (188, 0xffffffff),
                              (204, 0), (204, 99), (260, 1), (244, 2), (252, 0xffffffff)]:
            modified = bytearray(data)
            struct.pack_into('<I', modified, offset, value)
            with self.subTest(offset=offset, value=value), self.assertRaises(ValueError):
                camera.Module(modified).inspect()
        with self.assertRaises(ValueError):
            camera.Module(data + b'\0').inspect()

    def test_rejects_bad_schema_reference_counts_and_names(self):
        for symbol, offset, value in [(0, 0, 2), (0, 16, 999), (0, 80, 2), (0, 244, 8),
                                       (0, 452, 2), (15, 26, 2), (15, 30, 19), (15, 122, 20),
                                       (5, 4, 3), (7, 24, 2), (8, 4, 23), (9, 16, 99)]:
            symbols = fixture()
            struct.pack_into('<I', symbols[symbol][4], offset, value)
            with self.subTest(symbol=symbol, offset=offset), self.assertRaises(ValueError):
                camera.Module(encode(symbols)).inspect()
        for name in ['test\0', '../camera', 'x' * 33]:
            symbols = fixture(); symbols[2][1] = name
            with self.subTest(name=name), self.assertRaises(ValueError):
                camera.Module(encode(symbols)).inspect()

    def test_rejects_nonfinite_or_nonpositive_rate(self):
        for value in [float('nan'), float('inf'), -1, 0]:
            symbols = fixture(); struct.pack_into('<d', symbols[7][4], 64, value)
            with self.subTest(value=value), self.assertRaises(ValueError):
                camera.Module(encode(symbols)).inspect()

    def test_checksum_and_no_hardware_claim(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'module.bin'; path.write_bytes(encode(fixture()))
            digest = hashlib.sha256(path.read_bytes()).hexdigest()
            result = camera.run(path, digest)
            self.assertEqual(result['device_actions'], [])
            self.assertFalse(result['active_sensor_verified'])
            self.assertFalse(result['capture_configuration_ready'])
            with self.assertRaises(ValueError):
                camera.run(path, '0' * 64)
            alias = path.with_name('alias'); alias.symlink_to(path)
            with self.assertRaises(ValueError):
                camera.run(alias, digest)
