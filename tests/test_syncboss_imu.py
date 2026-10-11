from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


class SyncbossImuTests(unittest.TestCase):
    def test_native_decoder(self):
        compiler = shutil.which("cc")
        self.assertIsNotNone(compiler, "A C compiler is required for the driver tests")
        root = Path(__file__).resolve().parents[1]
        with tempfile.TemporaryDirectory() as temp:
            executable = Path(temp) / "syncboss-imu"
            subprocess.run([
                compiler, "-std=c11", "-O2", "-ffp-contract=off", "-Wall", "-Wextra", "-Werror",
                "-I", str(root / "src"), str(root / "tests/syncboss-imu.c"),
                str(root / "src/syncboss-imu.c"), "-o", str(executable),
            ], check=True, capture_output=True, text=True, timeout=30)
            result = subprocess.run([str(executable)], check=True, capture_output=True,
                                    text=True, timeout=10)
            self.assertIn("conversion and rejection checks passed", result.stdout)
