from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class QuestImuStreamTests(unittest.TestCase):
    def test_copied_fifo_lifecycle(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp)
            flags = ["cc", "-std=c11", "-O1", "-g", "-ffp-contract=off", "-Wall", "-Wextra", "-Werror",
                     "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-I", str(ROOT / "src")]
            replacements = [f"-D{name}=quest_test_{alias}" for name, alias in (
                ("open", "open"), ("fstat", "fstat"), ("ioctl", "ioctl"),
                ("read", "read"), ("close", "close"), ("clock_gettime", "clock"))]
            result = subprocess.run([*flags, *replacements, "-c", str(ROOT / "src/quest-imu-stream.c"),
                            "-o", str(out / "stream.o")], capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([*flags, str(out / "stream.o"), *[str(ROOT / name) for name in (
                "src/quest-imu.c", "src/syncboss-imu.c", "src/imu-calibration.c",
                "src/imu-time-align.c", "tests/quest-imu-stream.c")], "-lm", "-o", str(out / "test")],
                capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(out / "test")], capture_output=True, text=True, timeout=15)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("QUEST_IMU_STREAM cases=1101 passed", result.stdout)


if __name__ == "__main__":
    unittest.main()
