from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


class SnsLowLatencyTests(unittest.TestCase):
    def test_request_validation_and_remote_failures(self):
        root = Path(__file__).resolve().parents[1]
        compiler = shutil.which('cc')
        self.assertIsNotNone(compiler)
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / 'sns-low-latency'
            subprocess.run([compiler, '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                            '-I', str(root / 'src'), str(root / 'tests/sns-low-latency.c'),
                            str(root / 'src/sns-low-latency.c'),
                            '-o', str(binary)], check=True, capture_output=True, text=True, timeout=30)
            result = subprocess.run([str(binary)], check=True, capture_output=True, text=True, timeout=10)
            self.assertIn('SNS_LOW_LATENCY_PASS', result.stdout)
