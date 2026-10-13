from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


class SscCalibrationTests(unittest.TestCase):
    def test_wire_history_identity_and_bias_correction(self):
        root = Path(__file__).resolve().parents[1]
        compiler = shutil.which('cc')
        self.assertIsNotNone(compiler)
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / 'ssc-calibration'
            subprocess.run([compiler, '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                            '-I', str(root / 'src'), str(root / 'tests/ssc-calibration.c'),
                            str(root / 'src/ssc-calibration.c'), str(root / 'src/ssc-clock.c'),
                            '-o', str(binary)], check=True, capture_output=True, text=True, timeout=30)
            result = subprocess.run([str(binary)], check=True, capture_output=True, text=True, timeout=15)
            self.assertIn('SSC_CALIBRATION_UNIT_PASS', result.stdout)
