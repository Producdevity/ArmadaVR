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
    for offset, value in ((24, 8), (32, 2), (40, 0xe0), (64, 0x300a), (72, 0x1234), (80, 0xffff), (48, 2), (56, 2), (88, 1),
                          (96, 2), (104, 12), (112, 1), (120, 13), (720, 1), (728, 20),
                          (488, 1), (496, 5), (504, 1), (512, 5), (560, 26), (576, 27), (584, 1), (592, 11), (352, 1), (360, 28)):
        struct.pack_into('<Q', driver, offset, value)
    resolution = bytearray(288)
    values = {1: 1, 2: 4, 3: 800, 4: 500, 7: 24000000, 8: 1, 9: 1,
              11: 1, 14: 14, 18: 1, 19: 5, 30: 1, 31: 9, 34: 1, 35: 10}
    for index, value in values.items():
        struct.pack_into('<Q', resolution, index * 8, value)
    struct.pack_into('<d', resolution, 80, 60)
    configuration = bytearray(360)
    for index, value in {3: 8, 4: 2, 5: 1, 6: 16, 7: 1, 8: 17,
                         27: 1, 28: 18, 29: 1, 30: 19}.items():
        struct.pack_into('<Q', configuration, index * 8, value)
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
        [12, 'powerSetting', 0, 0xffffffff, words(8, 0, 1, 0, 24000000, 18)],
        [13, 'powerSetting', 0, 0xffffffff, words(8, 1, 2)],
        [14, 'cameraModuleData', 1, 0, words(1, 0, 0, 1, 15)],
        [15, 'moduleConfiguration', 0, 0xffffffff, configuration],
        [16, 'sensorSlaveAddress', 0, 0xffffffff, words(0xe2)],
        [17, 'sensorI2CFrequencyMode', 0, 0xffffffff, words(1)],
        [18, 'laneAssign', 0, 0xffffffff, words(0x3210)],
        [19, 'isComboMode', 0, 0xffffffff, words(0)],
        [20, 'delayInfo', 0, 0xffffffff, words(0, 21, 1, 22, 1, 23, 0, 24, 1, 0)],
        [21, 'delayType', 0, 0xffffffff, b''],
        [22, 'linecount', 0, 0xffffffff, words(0)],
        [23, 'gain', 0, 0xffffffff, words(0)],
        [24, 'frameLengthLines', 0, 0xffffffff, b''],
        [26, 'masterSettings', 0, 0xffffffff, b''],
        [27, 'slaveSettings', 0, 0xffffffff, b''],
        [28, 'resolutionInfo', 0, 0xffffffff, words(1, 29, 1, 3)],
        [29, 'sensorVersion', 0, 0xffffffff, words(7)],
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

    def test_resolution_information_reference_and_optional_version(self):
        for flag in (0, 1, 2, 0xffffffff):
            symbols = fixture()
            symbols[-2][4] = words(flag, 29, 1, 3)
            symbols[-1][4] = words(7) if flag else b''
            result = camera.Module(encode(symbols)).inspect()
            self.assertEqual(result['resolution_information_reference'], {'symbol': 28, 'count': 1})
            self.assertEqual(result['resolution_information']['28'], [
                {'index': 0, 'sensor_version_flag': flag, 'sensor_version': 7 if flag else None,
                 'modes': {'symbol': 3, 'count': 1}}])
        symbols = fixture()
        struct.pack_into('<Q', symbols[0][4], 352, 2)
        symbols[-2][4] += words(0, 30, 0, 31)
        symbols += [[30, 'sensorVersion', 0, 0xffffffff, b''],
                    [31, 'resolutionData', 0, 0xffffffff, b'']]
        result = camera.Module(encode(symbols)).inspect()
        self.assertEqual(result['resolution_information']['28'][1], {
            'index': 1, 'sensor_version_flag': 0, 'sensor_version': None,
            'modes': {'symbol': 31, 'count': 0}})
        self.assertEqual(result['resolution_information_reference']['count'], 2)

    def test_invalid_resolution_information_references(self):
        for field, offset, value in ((0, 352, 2), (0, 360, 3), (-2, 8, 3),
                                     (-2, 0, 0), (-2, 16, 2), (-2, 24, 5), (-2, 24, 99999)):
            symbols = fixture()
            data = bytearray(symbols[field][4])
            struct.pack_into('<Q', data, offset, value)
            symbols[field][4] = data
            with self.subTest(field=field, offset=offset), self.assertRaises(ValueError):
                camera.Module(encode(symbols)).inspect()
        for version, mode, data in ((1, 0xffffffff, words(1, 29, 1, 3)),
                                    (0, 0, words(1, 29, 1, 3)), (0, 0xffffffff, words(1, 29, 1))):
            symbols = fixture()
            symbols[-2][2:] = [version, mode, data]
            with self.subTest(version=version, mode=mode), self.assertRaises(ValueError):
                camera.Module(encode(symbols)).inspect()

    def test_color_filter_pattern_is_not_csi_data_type(self):
        for color, pattern in enumerate((2, 3, 1, 0, 0, 6, None, None)):
            symbols = fixture()
            struct.pack_into('<Q', symbols[2][4], 0, color)
            mode = camera.Module(encode(symbols)).inspect()['resolutions']['3'][0]
            self.assertEqual(mode['color_filter_pattern'], color)
            self.assertEqual(mode['camif_sample_pattern'], pattern)
            self.assertEqual(mode['streams'][0]['data_type'], 0x2a)

    def test_stream_tables_and_initialization_reference(self):
        symbols = fixture()
        result = camera.Module(encode(symbols)).inspect()
        self.assertEqual(result['stream_settings'], {
            'stream_on': {'symbol': 5, 'count': 1}, 'stream_off': {'symbol': 5, 'count': 1},
            'master': None, 'slave': None})
        self.assertEqual(result['initialization_reference'], {'symbol': 11, 'count': 1})
        for offset, symbol_index in ((552, 24), (568, 25)):
            struct.pack_into('<Q', symbols[0][4], offset, 1)
            symbols[symbol_index][4] = words(1, 5)
        result = camera.Module(encode(symbols)).inspect()
        self.assertEqual(result['stream_settings']['master'], {'symbol': 5, 'count': 1})
        self.assertEqual(result['stream_settings']['slave'], {'symbol': 5, 'count': 1})

    def test_invalid_stream_and_initialization_references(self):
        for offset, value in ((488, 2), (496, 4), (504, 0), (512, 99999),
                              (552, 2), (560, 27), (568, 1), (584, 2), (592, 5)):
            symbols = fixture()
            struct.pack_into('<Q', symbols[0][4], offset, value)
            with self.subTest(offset=offset), self.assertRaises(ValueError):
                camera.Module(encode(symbols)).inspect()

    def test_power_and_csi_configuration(self):
        result = camera.Module(encode(fixture())).inspect()
        self.assertEqual((result['address_type'], result['data_type'], result['frequency']), (2, 2, 1))
        self.assertEqual(result['power_up'], [{'sequence_type': 8, 'value': 0, 'delay_ms': 1},
                                            {'sequence_type': 0, 'value': 24000000, 'delay_ms': 18}])
        self.assertEqual(result['power_down'], [{'sequence_type': 8, 'value': 1, 'delay_ms': 2}])
        self.assertEqual(result['configurations'], [{'index': 0, 'sensor_name': 'ov-test',
            'slave_address': 0xe2, 'frequency': 1, 'lane_assign': 0x3210, 'combo': 0}])

    def test_probe_pipeline_delay_selection(self):
        result = camera.Module(encode(fixture())).inspect()
        self.assertEqual(result['probe_pipeline_delay'], 1)
        self.assertEqual(result['delays'], [{'type': None, 'line_count_delay': 0,
            'gain_delay': 0, 'frame_length_delay': None, 'pipeline_delay': 1}])
        symbols = fixture()
        struct.pack_into('<Q', symbols[0][4], 720, 3)
        symbols[19][4] += words(1, 21, 1, 22, 1, 23, 0, 24, 2, 0)
        symbols[19][4] += words(1, 21, 1, 22, 1, 23, 0, 24, 3, 0)
        # Optional references share a table; absent fields require an empty one.
        symbols.append([25, 'delayType', 0, 0xffffffff, b''])
        symbols[19][4] = bytearray(symbols[19][4])
        struct.pack_into('<Q', symbols[19][4], 8, 25)
        symbols[20][4] = words(0)
        self.assertEqual(camera.Module(encode(symbols)).inspect()['probe_pipeline_delay'], 3)
        symbols[20][4] = words(1)
        self.assertEqual(camera.Module(encode(symbols)).inspect()['probe_pipeline_delay'], 1)
        struct.pack_into('<Q', symbols[0][4], 720, 0)
        symbols[19][4] = b''
        self.assertIsNone(camera.Module(encode(symbols)).inspect()['probe_pipeline_delay'])

    def test_invalid_delay_metadata(self):
        for index, value in ((0, 2), (1, 99999), (8, 1 << 32)):
            symbols = fixture()
            delay = bytearray(symbols[19][4])
            struct.pack_into('<Q', delay, index * 8, value)
            symbols[19][4] = delay
            with self.subTest(index=index), self.assertRaises(ValueError):
                camera.Module(encode(symbols)).inspect()

    def test_missing_optional_csi_values_remain_unknown(self):
        symbols = fixture()
        for index, reference in ((5, 15), (7, 16), (27, 17), (29, 18)):
            struct.pack_into('<Q', symbols[14][4], index * 8, 0)
            symbols[reference][4] = b''
        result = camera.Module(encode(symbols)).inspect()['configurations'][0]
        self.assertEqual([result[key] for key in ('slave_address', 'frequency', 'lane_assign', 'combo')], [None] * 4)

    def test_bad_power_and_configuration_references(self):
        for index, column, value in ((11, 1, 'regSetting'), (12, 4, bytes(16)), (13, 2, 2),
                                    (14, 4, bytes(352)), (17, 4, words(65536)), (18, 4, words(256))):
            symbols = fixture()
            symbols[index][column] = value
            with self.subTest(index=index), self.assertRaises(ValueError):
                camera.Module(encode(symbols)).inspect()
        symbols = fixture()
        struct.pack_into('<Q', symbols[14][4], 27 * 8, 2)
        with self.assertRaises(ValueError):
            camera.Module(encode(symbols)).inspect()

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

class NativeConfigurationTests(unittest.TestCase):
    def inventory(self):
        symbols = fixture()
        struct.pack_into('<Q', symbols[2][4], 0, 4)
        return camera.Module(encode(symbols)).inspect()

    def select(self, data=None, **choices):
        selection = dict(configuration=0, group=0, mode=0, initialization=0, synchronization='none')
        selection.update(choices)
        return camera.select_native(self.inventory() if data is None else data, **selection)

    def test_explicit_selection_resolves_normalized_tables(self):
        import copy
        data = self.inventory()
        before = copy.deepcopy(data)
        result = self.select(data)
        self.assertEqual(data, before)
        self.assertEqual(result['probe']['bus'], dict(address_type=2, data_type=2, frequency=1,
                                                     include_slave=True, slave_address=0xe2))
        self.assertEqual(result['tables']['mode'], {
            'bus': dict(address_type=2, data_type=1, frequency=1, include_slave=False, slave_address=0xe2),
            'writes': [dict(address=0x100, value=128, delay_us=1000)]})
        self.assertEqual(result['sensor_version'], 7)
        self.assertEqual(result['phy'], dict(lanes=1, lane_map=0x3210, cphy=0, combo=0, settle_count=14, mipi_flags=2))
        self.assertIsNone(result['tables']['synchronize'])
        self.assertEqual(result['probe']['pipeline_delay'], 1)

    def test_configuration_overrides_keep_zero_and_explicit_addresses(self):
        data = self.inventory()
        data['configurations'][0]['frequency'] = 0
        data['register_tables']['5'][0]['slave_address'] = 0xe2
        result = self.select(data)
        self.assertEqual(result['tables']['mode']['bus']['frequency'], 0)
        self.assertTrue(result['tables']['mode']['bus']['include_slave'])
        data['configurations'][0]['frequency'] = None
        data['configurations'][0]['slave_address'] = None
        data['register_tables']['5'][0]['slave_address'] = 0xe0
        result = self.select(data)
        self.assertEqual(result['probe']['bus']['frequency'], 1)
        self.assertEqual(result['probe']['bus']['slave_address'], 0xe0)
        data['stream_settings']['master'] = data['stream_settings']['stream_on']
        self.assertEqual(self.select(data, synchronization='master')['tables']['synchronize'], result['tables']['stream_on'])

    def test_requires_each_selected_index_and_supported_tables(self):
        for field in ('configuration', 'group', 'mode', 'initialization'):
            for value in (-1, 1, None, True):
                with self.subTest(field=field, value=value), self.assertRaises(ValueError):
                    self.select(**{field: value})
        for role in ('master', 'slave', 'automatic', None):
            with self.subTest(role=role), self.assertRaises(ValueError):
                self.select(synchronization=role)
        for field, value in (('operation', 1), ('values', [1, 2]), ('address_type', 0),
                             ('data_type', 5), ('values', [256]), ('address', 65536),
                             ('slave_address', 0xe4), ('delay_us', 2001), ('delay_us', 32768000)):
            data = self.inventory(); data['register_tables']['5'][0][field] = value
            with self.subTest(field=field, value=value), self.assertRaises(ValueError):
                self.select(data)
        import copy
        for field, value in (('address_type', 1), ('data_type', 2), ('slave_address', 0xe2)):
            data = self.inventory(); row = copy.deepcopy(data['register_tables']['5'][0]); row[field] = value
            data['register_tables']['5'].append(row)
            with self.subTest(mixed=field), self.assertRaises(ValueError):
                self.select(data)

    def test_rejects_unsupported_geometry_probe_and_power(self):
        for field, value in (('color_filter_pattern', 0), ('camif_sample_pattern', None),
                             ('lanes', 0), ('lanes', 5), ('cphy', 2), ('settle_count', 65536)):
            data = self.inventory(); data['resolutions']['3'][0][field] = value
            with self.subTest(field=field), self.assertRaises(ValueError): self.select(data)
        for field, value in (('type', 0), ('x', 1), ('y', 1), ('bits', 10), ('data_type', 0x2b),
                             ('virtual_channel', 4), ('width', 4097), ('height', 0)):
            data = self.inventory(); data['resolutions']['3'][0]['streams'][0][field] = value
            with self.subTest(field=field), self.assertRaises(ValueError): self.select(data)
        for field, value in (('combo', 1), ('combo', None), ('lane_assign', None), ('lane_assign', 0x3214),
                             ('frequency', 4), ('slave_address', 3), ('sensor_name', 'different')):
            data = self.inventory(); data['configurations'][0][field] = value
            with self.subTest(field=field), self.assertRaises(ValueError): self.select(data)
        data = self.inventory(); data['resolutions']['3'][0]['lanes'] = 2
        data['configurations'][0]['lane_assign'] = 0
        with self.assertRaises(ValueError): self.select(data)
        for field, value in (('probe_pipeline_delay', 4), ('probe_pipeline_delay', None),
                             ('address_type', 0), ('sensor_id_register', 1 << 16)):
            data = self.inventory(); data[field] = value
            with self.subTest(field=field), self.assertRaises(ValueError): self.select(data)
        for rows in ([], [dict(sequence_type=12, value=0, delay_ms=0)],
                     [dict(sequence_type=0, value=0, delay_ms=32768)],
                     [dict(sequence_type=0, value=0, delay_ms=0)] * 12):
            data = self.inventory(); data['power_up'] = rows
            with self.subTest(rows=rows), self.assertRaises(ValueError): self.select(data)

    def test_cli_selection_is_hash_bound_and_never_programs_a_device(self):
        import json
        import subprocess
        import sys
        symbols = fixture(); struct.pack_into('<Q', symbols[2][4], 0, 4)
        raw = encode(symbols)
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / 'module.bin'; path.write_bytes(raw)
            args = [sys.executable, '-B', str(ROOT / 'tools/pico-camera-module.py'), str(path),
                    '--sha256', hashlib.sha256(raw).hexdigest(), '--configuration', '0',
                    '--resolution-group', '0', '--resolution', '0', '--initialization', '0',
                    '--synchronization', 'none']
            completed = subprocess.run(args, check=True, capture_output=True, text=True, timeout=10)
            result = json.loads(completed.stdout)
            self.assertEqual(result['source_sha256'], hashlib.sha256(raw).hexdigest())
            self.assertFalse(result['active_sensor_verified']); self.assertFalse(result['calibration_verified'])
            self.assertEqual(result['device_actions'], [])
            self.assertEqual(path.read_bytes(), raw)
            incomplete = subprocess.run(args[:-2], capture_output=True, text=True, timeout=10)
            self.assertNotEqual(incomplete.returncode, 0)
            self.assertFalse(incomplete.stdout)
