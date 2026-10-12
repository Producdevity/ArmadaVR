from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


class SscImuTests(unittest.TestCase):
    def test_payloads_stream_identity_timing_and_reconnects(self):
        root = Path(__file__).resolve().parents[1]
        compiler = shutil.which('cc')
        self.assertIsNotNone(compiler)
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / 'ssc-imu'
            subprocess.run([compiler, '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                            '-I', str(root / 'src'), str(root / 'tests/ssc-imu.c'),
                            str(root / 'src/ssc-imu.c'), str(root / 'src/ssc-clock.c'),
                            '-o', str(binary)], check=True, capture_output=True, text=True, timeout=30)
            result = subprocess.run([str(binary)], check=True, capture_output=True, text=True, timeout=15)
            self.assertIn('SSC_IMU_UNIT_PASS', result.stdout)
