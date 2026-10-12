import copy
import importlib.util
import json
import math
from pathlib import Path
import stat
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location('pico_calibration', ROOT / 'tools/inspect-pico-calibration.py')
CAL = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CAL)


def record(name, fields, values, kind='flt'):
    return {name: {'owner': 'icm4x6xx', **{
        field: {'type': kind, 'ver': str(i), 'data': str(value)}
        for i, (field, value) in enumerate(zip(fields, values))}}}


class PicoCalibrationTests(unittest.TestCase):
    def test_field_identity_versions_and_numeric_refusals(self):
        name = 'icm4x6xx_0_platform.accel.fac_cal.bias'
        good = record(name, ['x', 'y', 'z'], [0.125, -2, 3e-5])
        parsed = CAL.group(json.dumps(good).encode(), name, 'icm4x6xx', ['x', 'y', 'z'], 'flt')
        self.assertEqual(parsed, {'values': [0.125, -2, 3e-5], 'versions': [0, 1, 2]})
        bad_values = [('type', 'int'), ('ver', '-1'), ('ver', '4294967296'), ('ver', 1),
                      ('data', 'nan'), ('data', 'inf'), ('data', '1e999'), ('data', True),
                      ('data', '0x1'), ('data', '1,2'), ('data', '1' * 129)]
        for key, value in bad_values:
            with self.subTest(key=key, value=value):
                bad = copy.deepcopy(good)
                bad[name]['x'][key] = value
                with self.assertRaises(ValueError):
                    CAL.group(json.dumps(bad).encode(), name, 'icm4x6xx', ['x', 'y', 'z'], 'flt')
        for data in (b'{"duplicate":1,"duplicate":2}', b'[]', b'{}', b' ' * 65537):
            with self.assertRaises(ValueError):
                CAL.group(data, name, 'icm4x6xx', ['x', 'y', 'z'], 'flt')
        for group_name, owner, fields in ((name + 'x', 'icm4x6xx', ['x', 'y', 'z']),
                                         (name, 'lsm6dst', ['x', 'y', 'z']),
                                         (name, 'icm4x6xx', ['x', 'y'])):
            with self.assertRaises(ValueError):
                CAL.group(json.dumps(good).encode(), group_name, owner, fields, 'flt')

    def test_complete_selection_keeps_unusable_placement_and_raw_orientation(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)

            def write(suffix, fields, values, kind='flt'):
                name = 'icm4x6xx_0_platform.' + suffix
                path = directory / name
                path.write_text(json.dumps(record(name, fields, values, kind)))
                return path

            fields = [f'{i}_{j}' for i in range(3) for j in range(3)]
            for sensor in ('accel', 'gyro'):
                write(sensor + '.fac_cal.bias', ['x', 'y', 'z'], [0.1, -0.2, 0.3])
                write(sensor + '.fac_cal.corr_mat', fields, [1.01, 0.01, 0, 0, 0.99, 0, 0, 0, 1])
            write('orient', ['x', 'y', 'z'], ['+y', '+x', '-z'], 'str')
            placement = write('placement', [str(i) for i in range(12)], [0] * 12)
            result = CAL.inspect(directory, 'icm4x6xx_0')
            self.assertEqual(len(result['files_sha256']), 6)
            self.assertTrue(result['placement_all_zero'])
            self.assertFalse(result['runtime_usable'])
            self.assertFalse(result['calibration_applied'])
            self.assertEqual(result['orientation_fields']['values'], ['+y', '+x', '-z'])
            self.assertTrue(math.isclose(result['sensors']['gyro']['determinant'], 0.9999))
            output = directory / 'inspected.json'
            command = [sys.executable, '-B', str(ROOT / 'tools/inspect-pico-calibration.py'),
                       str(directory), '--sensor', 'icm4x6xx_0', '--output', str(output)]
            subprocess.run(command, check=True, capture_output=True, timeout=5)
            self.assertEqual(json.loads(output.read_text()), result)
            self.assertEqual(stat.S_IMODE(output.stat().st_mode), 0o600)
            original = output.read_bytes()
            self.assertNotEqual(subprocess.run(command, capture_output=True, timeout=5).returncode, 0)
            self.assertEqual(output.read_bytes(), original)
            for axes in (['+x', '+x', '+z'], ['x', '+y', '+z']):
                write('orient', ['x', 'y', 'z'], axes, 'str')
                with self.assertRaises(ValueError):
                    CAL.inspect(directory, 'icm4x6xx_0')
            write('orient', ['x', 'y', 'z'], ['+x', '+y', '+z'], 'str')
            matrix = write('gyro.fac_cal.corr_mat', fields, [0] * 9)
            with self.assertRaises(ValueError):
                CAL.inspect(directory, 'icm4x6xx_0')
            matrix.write_bytes(placement.read_bytes())
            with self.assertRaises(ValueError):
                CAL.inspect(directory, 'icm4x6xx_0')
            matrix.unlink()
            matrix.symlink_to(placement)
            with self.assertRaises(OSError):
                CAL.inspect(directory, 'icm4x6xx_0')
            with self.assertRaises(ValueError):
                CAL.inspect(directory, '../icm4x6xx_0')

    def test_regular_file_limit(self):
        with self.assertRaises(ValueError):
            CAL.read_record('/dev/null')
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / 'record'
            path.write_bytes(b' ' * 65537)
            with self.assertRaises(ValueError):
                CAL.read_record(path)
