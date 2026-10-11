import copy
import importlib.util
import json
from pathlib import Path
import stat
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("quest_calibration", ROOT / "tools/extract-quest-calibration.py")
CAL = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CAL)
IDENTITY = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]


def fixture(online=False):
    offset = {"Model": "Constant", "ConstantOffset": [0.1, 0.2, 0.3]}
    if online:
        offset = {"Model": "TemperaturePiecewiseLinear", "CoordinateSystem": "Raw",
                  "DataDescription": "temperature, offset.x, offset.y, offset.z",
                  "TemperatureOffsetPairs": [[10, 0, 0, 0], [30, 0.1, -0.2, 0.3]]}
    imu = {"Id": "imu0", "SensorType": "ICM45688", "DeviceFromImu": IDENTITY[:],
           "Accelerometer": {"Model": "Linear", "RectificationMatrix": [1, 0, 0, 0, 1, 0, 0, 0, 1],
                             "DtAccelRef": 0, "Offset": copy.deepcopy(offset)},
           "Gyroscope": {"Model": "Linear", "RectificationMatrix": [1, 0, 0, 0, 1, 0, 0, 0, 1],
                         "DtGyroRef": 0, "Offset": copy.deepcopy(offset)}}
    data = {"Device": {"SerialNumber": "TEST-UNIT", "DeviceType": "Eureka"},
            "FileFormat": {"Version": "1"}, "ImuCalibration": imu}
    if online:
        data["CameraCalibration"] = [{
            "Id": "0", "SensorType": "OG01A", "ImageSize": [1280, 1024],
            "DeviceFromCamera": IDENTITY[:], "Shutter": {"Type": "Global"},
            "Projection": {"Model": "PinholeSymmetric", "Coefficients": [500, 640, 512]},
            "Distortion": {"Model": "Fisheye62", "Coefficients": [0] * 8},
        }]
    return data


class QuestCalibrationTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.capture = self.root / "capture.txt"
        self.output = self.root / "unit"
        self.factory = json.dumps(fixture()) + "\n"
        self.online = json.dumps(fixture(True), indent=2)
        self.sensor = {"aname": "Syncboss IMU Android Sensor", "dataRate": 800.0,
                       "info": {"model": "ICM45688"},
                       "calibration": {"factory": self.factory, "online": self.online}}

    def write_capture(self):
        self.capture.write_text("Sensor List:\n0x00000002) " + json.dumps(self.sensor) + " | vendor | ver: 2\n")

    def reject(self):
        self.write_capture()
        with self.assertRaises((ValueError, KeyError, TypeError)):
            CAL.extract(self.capture, "TEST-UNIT", self.output)
        self.assertFalse(self.output.exists())

    def test_preserves_exact_embedded_blobs_and_private_modes(self):
        self.write_capture()
        result = CAL.extract(self.capture, "TEST-UNIT", self.output)
        self.assertEqual((self.output / "factory.json").read_text(), self.factory)
        self.assertEqual((self.output / "online.json").read_text(), self.online)
        self.assertEqual(stat.S_IMODE(self.output.stat().st_mode), 0o700)
        for path in self.output.iterdir():
            self.assertEqual(stat.S_IMODE(path.stat().st_mode), 0o600)
        self.assertEqual(result["files"]["online.json"]["camera_count"], 1)
        self.assertFalse(result["physical_tracking_verified"])
        self.assertFalse(result["calibration_applied"])
        with self.assertRaises(FileExistsError):
            CAL.extract(self.capture, "TEST-UNIT", self.output)
        self.assertEqual((self.output / "factory.json").read_text(), self.factory)

    def test_rejects_cross_unit_data_before_creating_output(self):
        for kind in ("factory", "online"):
            with self.subTest(kind=kind):
                data = json.loads(self.sensor["calibration"][kind])
                data["Device"]["SerialNumber"] = "ANOTHER-UNIT"
                old = self.sensor["calibration"][kind]
                self.sensor["calibration"][kind] = json.dumps(data)
                self.reject()
                self.sensor["calibration"][kind] = old

    def test_rejects_duplicate_descriptors_keys_and_nonfinite_numbers(self):
        self.write_capture()
        text = self.capture.read_text()
        self.capture.write_text(text + text)
        with self.assertRaises(ValueError):
            CAL.extract(self.capture, "TEST-UNIT", self.output)
        for payload in ('{"Device":{},"Device":{}}', '{"value":NaN}', '{"value":1e999}'):
            self.sensor["calibration"]["factory"] = payload
            self.reject()

    def test_rejects_unknown_models_and_malformed_geometry(self):
        cases = [
            (("FileFormat", "Version"), "2"),
            (("ImuCalibration", "SensorType"), "UNKNOWN"),
            (("ImuCalibration", "Accelerometer", "Model"), "Unknown"),
            (("ImuCalibration", "Accelerometer", "RectificationMatrix"), [0] * 9),
            (("ImuCalibration", "DeviceFromImu"), [0] * 16),
            (("ImuCalibration", "DeviceFromImu"), [-1] + IDENTITY[1:]),
            (("CameraCalibration", 0, "Projection", "Model"), "Other"),
            (("CameraCalibration", 0, "Projection", "Coefficients"), [0, 640, 512]),
            (("CameraCalibration", 0, "Distortion", "Coefficients"), [0] * 4),
            (("CameraCalibration", 0, "ImageSize"), [True, 1024]),
            (("CameraCalibration", 0, "DeviceFromCamera"), IDENTITY[:12] + [0, 0, 1, 1]),
            (("CameraCalibration",), []),
        ]
        for path, value in cases:
            with self.subTest(path=path, value=value):
                data = fixture(True)
                cursor = data
                for name in path[:-1]:
                    cursor = cursor[name]
                cursor[path[-1]] = value
                self.sensor["calibration"]["online"] = json.dumps(data)
                self.reject()

    def test_rejects_invalid_temperature_tables(self):
        for field, value in [
            ("CoordinateSystem", "Unknown"), ("Model", "Polynomial"),
            ("TemperatureOffsetPairs", [[20, 0, 0, 0], [10, 0, 0, 0]]),
            ("TemperatureOffsetPairs", [[10, 0, 0, 0], [10, 0, 0, 0]]),
            ("TemperatureOffsetPairs", [[10, 0, 0, 0]]),
            ("TemperatureOffsetPairs", [[10, 0, 0], [20, 0, 0]]),
        ]:
            with self.subTest(field=field, value=value):
                data = fixture(True)
                data["ImuCalibration"]["Accelerometer"]["Offset"][field] = value
                self.sensor["calibration"]["online"] = json.dumps(data)
                self.reject()

    def test_camera_identity_and_rolling_shutter(self):
        data = fixture(True)
        camera = data["CameraCalibration"][0]
        camera["Shutter"] = {"Type": "Rolling", "ReadoutTimeSec": 0.01}
        CAL.calibration(json.dumps(data), "TEST-UNIT", True)
        camera["Shutter"]["ReadoutTimeSec"] = -1
        with self.assertRaises(ValueError):
            CAL.calibration(json.dumps(data), "TEST-UNIT", True)
        camera["Shutter"]["ReadoutTimeSec"] = 0.01
        data["CameraCalibration"].append(copy.deepcopy(camera))
        with self.assertRaises(ValueError):
            CAL.calibration(json.dumps(data), "TEST-UNIT", True)

    def test_capture_boundaries(self):
        self.write_capture()
        link = self.root / "link"
        link.symlink_to(self.capture)
        with self.assertRaises(ValueError):
            CAL.extract(link, "TEST-UNIT", self.output)
        with self.assertRaises(ValueError):
            CAL.extract(self.capture, "", self.output)
        self.capture.write_bytes(b"x" * (2 * 1024 * 1024 + 1))
        with self.assertRaises(ValueError):
            CAL.extract(self.capture, "TEST-UNIT", self.output)
        self.assertFalse(self.output.exists())
