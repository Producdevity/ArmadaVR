from pathlib import Path
import subprocess
import tempfile
import unittest


class QuestImuTests(unittest.TestCase):
    def test_calibrated_stream(self):
        root = Path(__file__).resolve().parents[1]
        with tempfile.TemporaryDirectory() as temp:
            executable = Path(temp) / "quest-imu"
            subprocess.run([
                "cc", "-std=c11", "-O2", "-ffp-contract=off", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-I", str(root / "src"),
                *[str(root / name) for name in (
                    "src/quest-imu.c", "src/syncboss-imu.c", "src/imu-calibration.c",
                    "src/imu-time-align.c", "tests/quest-imu.c")],
                "-lm", "-o", str(executable),
            ], check=True, capture_output=True, text=True, timeout=30)
            result = subprocess.run([str(executable)], check=True, capture_output=True,
                                    text=True, timeout=10)
            self.assertIn("8192 calibrated IMU stream cases and rejection checks passed", result.stdout)
            self.assertIn("9952 aligned stream cases and 288 history resets passed", result.stdout)
            self.assertIn("12288 FIFO calibration comparisons and transport rejection checks passed", result.stdout)
