from pathlib import Path
import subprocess
import tempfile
import unittest


class ImuCalibrationTests(unittest.TestCase):
    def test_rectification_split(self):
        root = Path(__file__).resolve().parents[1]
        with tempfile.TemporaryDirectory() as temp:
            executable = Path(temp) / "imu-rectification"
            subprocess.run([
                "cc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-I", str(root / "src"),
                str(root / "src/imu-calibration.c"), str(root / "tests/imu-rectification.c"),
                "-lm", "-o", str(executable),
            ], check=True, capture_output=True, timeout=30)
            result = subprocess.run([str(executable)], check=True, capture_output=True,
                                    text=True, timeout=10)
            self.assertIn("12288 IMU rectification split cases", result.stdout)

    def test_calibration_math(self):
        root = Path(__file__).resolve().parents[1]
        with tempfile.TemporaryDirectory() as temp:
            executable = Path(temp) / "imu-calibration"
            subprocess.run([
                "cc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-I", str(root / "src"),
                str(root / "src/imu-calibration.c"), str(root / "tests/imu-calibration.c"),
                "-lm", "-o", str(executable),
            ], check=True, capture_output=True, timeout=30)
            result = subprocess.run([str(executable)], check=True, capture_output=True,
                                    text=True, timeout=10)
            self.assertIn("801 IMU calibration temperature cases", result.stdout)
