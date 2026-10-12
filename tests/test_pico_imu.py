from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


class PicoImuTests(unittest.TestCase):
    def test_pairing_clock_ordering_and_rejection(self):
        root = Path(__file__).resolve().parents[1]
        compiler = shutil.which('cc')
        self.assertIsNotNone(compiler)
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / 'pico-imu'
            subprocess.run([compiler, '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                            '-I', str(root / 'src'), str(root / 'tests/pico-imu.c'),
                            str(root / 'src/pico-imu.c'), str(root / 'src/ssc-clock.c'),
                            '-o', str(binary)], check=True, capture_output=True, text=True, timeout=30)
            result = subprocess.run([str(binary)], check=True, capture_output=True, text=True, timeout=10)
            self.assertIn('PICO_IMU_UNIT_PASS', result.stdout)
