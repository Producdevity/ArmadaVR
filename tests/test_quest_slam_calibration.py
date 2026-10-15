import copy
import ctypes
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
SPEC = importlib.util.spec_from_file_location('quest_slam_calibration', ROOT / 'tools/quest-slam-calibration.py')
CAL = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CAL)
sys.path.insert(0, str(ROOT / 'tests'))
from test_quest_calibration import fixture


def unit():
    data = fixture(True)
    camera = data['CameraCalibration'][0]
    camera['DeviceFromCamera'][3] = 0.05
    camera['Distortion']['Coefficients'] = [.01, -.02, .03, -.04, .05, -.06, .007, -.008]
    other = copy.deepcopy(camera)
    other['Id'] = '3'
    other['DeviceFromCamera'][3] = -0.05
    data['CameraCalibration'].append(other)
    return data


def noise():
    return {'imu_update_rate': 800, **{k: [0.01, 0.02, 0.03] for k in CAL.NOISE_FIELDS}}


class QuestSlamCalibrationTests(unittest.TestCase):
    def convert(self, data=None, ids=None, frame='original', parameters=None):
        return CAL.convert(json.dumps(data or unit()), 'TEST-UNIT', ids or ['3', '0'], frame,
                           noise() if parameters is None else parameters)

    def test_geometry_order_and_single_application_of_imu_calibration(self):
        result, record = self.convert()
        c = result['value0']
        self.assertEqual([v['px'] for v in c['T_imu_cam']], [-.05, .05])
        self.assertEqual([v['qw'] for v in c['T_imu_cam']], [1, 1])
        self.assertEqual([x['id'] for x in record['camera_order']], ['3', '0'])
        self.assertEqual(c['resolution'], [[1280, 1024], [1280, 1024]])
        self.assertEqual(c['intrinsics'][0]['camera_type'], 'fisheye624')
        intr = c['intrinsics'][0]['intrinsics']
        self.assertEqual(list(intr.values()), [500, 500, 640, 512, .01, -.02, .03, -.04, .05, -.06, .007, -.008, 0, 0, 0, 0])
        self.assertEqual(c['calib_accel_bias'], [0] * 9)
        self.assertEqual(c['calib_gyro_bias'], [0] * 12)
        self.assertEqual(c['cam_time_offset_ns'], 0)
        for key in ('active_calibration_verified', 'physical_tracking_verified', 'camera_identity_verified', 'rate_and_noise_verified', 'automatic_startup'):
            self.assertIs(record[key], False)

    def test_rotated_translated_imu_extrinsics_and_accelerometer_frame(self):
        data = unit()
        data['ImuCalibration']['DeviceFromImu'] = [0,-1,0,1, 1,0,0,2, 0,0,1,3, 0,0,0,1]
        data['ImuCalibration']['Accelerometer']['RectificationMatrix'] = [0,-2,.2, 3,.1,.3, 0,0,4]
        original, _ = self.convert(data, ['0', '3'])
        aligned, record = self.convert(data, ['0', '3'], 'accelerometer')
        p = original['value0']['T_imu_cam'][0]
        self.assertAlmostEqual(p['px'], -2)
        self.assertAlmostEqual(p['py'], .95)
        self.assertAlmostEqual(p['pz'], -3)
        self.assertAlmostEqual(p['qz'], -math.sqrt(.5))
        self.assertAlmostEqual(p['qw'], math.sqrt(.5))
        p = aligned['value0']['T_imu_cam'][0]
        self.assertAlmostEqual(p['px'], .95)
        self.assertAlmostEqual(p['py'], 2)
        self.assertAlmostEqual(abs(p['qz']), 1)
        self.assertAlmostEqual(p['qw'], 0)
        self.assertEqual(record['device_from_effective_imu'][3::4], [1, 2, 3, 1])

    def test_quaternion_half_turns(self):
        for axis in range(3):
            r = [0] * 9
            for i in range(3):
                r[i * 3 + i] = 1 if i == axis else -1
            expected = [0] * 4
            expected[axis] = 1
            self.assertEqual(CAL.quaternion(r), expected)

    def test_refuses_ambiguous_identity_geometry_and_noise(self):
        for ids in (['0'], ['0', '0'], ['0', '1'], ['0', '03'], ['0', '-1'], ['0', '256'], ['0', '٣']):
            with self.subTest(ids=ids), self.assertRaises(ValueError):
                self.convert(ids=ids)
        for field, value in [('imu_update_rate', 0), ('imu_update_rate', True), ('gyro_noise_std', [0, 1, 1]), ('accel_bias_std', [1, 2]), ('gyro_bias_std', [1, float('nan'), 2])]:
            n = noise()
            n[field] = value
            with self.subTest(field=field), self.assertRaises(ValueError):
                self.convert(parameters=n)
        with self.assertRaises(ValueError):
            self.convert(parameters={})
        with self.assertRaises(ValueError):
            self.convert(frame='inferred')
        data = unit()
        data['CameraCalibration'][0]['Shutter'] = {'Type': 'Rolling', 'ReadoutTimeSec': .01}
        with self.assertRaises(ValueError):
            self.convert(data)
        data = unit()
        data['ImuCalibration']['Gyroscope']['RectificationMatrix'][0] = -1
        with self.assertRaises(ValueError):
            self.convert(data)
        with self.assertRaises(ValueError):
            CAL.convert(json.dumps(unit()), 'WRONG-UNIT', ['0', '3'], 'original', noise())

    def test_private_export_no_overwrite_or_symlink_inputs(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source, n, output = root / 'unit.json', root / 'noise.json', root / 'out'
            source.write_text(json.dumps(unit()))
            n.write_text(json.dumps(noise()))
            CAL.export(source, 'TEST-UNIT', ['0', '3'], 'original', n, output)
            self.assertEqual(stat.S_IMODE(output.stat().st_mode), 0o700)
            before = {p.name: p.read_bytes() for p in output.iterdir()}
            for p in output.iterdir():
                self.assertEqual(stat.S_IMODE(p.stat().st_mode), 0o600)
            with self.assertRaises(FileExistsError):
                CAL.export(source, 'TEST-UNIT', ['0', '3'], 'original', n, output)
            self.assertEqual(before, {p.name: p.read_bytes() for p in output.iterdir()})
            link = root / 'linked.json'
            link.symlink_to(source)
            with self.assertRaises(OSError):
                CAL.export(link, 'TEST-UNIT', ['0', '3'], 'original', n, root / 'bad')
            self.assertFalse((root / 'bad').exists())

    def test_rotation_matches_native_rectification(self):
        with tempfile.TemporaryDirectory() as directory:
            library = Path(directory) / 'rectification.so'
            subprocess.run(['cc', '-shared', '-fPIC', '-std=c11', '-ffp-contract=off', str(ROOT / 'src/imu-calibration.c'), '-lm', '-o', str(library)], check=True, capture_output=True)
            native = ctypes.CDLL(str(library))
            native.imu_split_rectification.argtypes = [ctypes.POINTER(ctypes.c_double)] * 3
            native.imu_split_rectification.restype = ctypes.c_bool
            vector = ctypes.c_double * 9
            for i in range(100):
                angle = i * .061
                c, s = math.cos(angle), math.sin(angle)
                rotation = [c,-s,0, s,c,0, 0,0,1]
                matrix = CAL.multiply(rotation, [1+.01*i,.02,.03, 0,2+.003*i,.04, 0,0,3])
                q, correction = vector(), vector()
                self.assertTrue(native.imu_split_rectification(vector(*matrix), q, correction))
                for a, b in zip(CAL.split_rotation(matrix), q):
                    self.assertAlmostEqual(a, b, places=14)


if __name__ == '__main__':
    unittest.main()
