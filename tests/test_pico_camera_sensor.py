from pathlib import Path
import subprocess
import tempfile
import unittest


class CameraSensorTests(unittest.TestCase):
    def test_command_encoding(self):
        self.run_encoder('sensor')

    def test_power_encoding(self):
        self.run_encoder('power')

    def test_probe_encoding(self):
        self.run_encoder('probe')

    def run_encoder(self, kind):
        root = Path(__file__).resolve().parents[1]
        with tempfile.TemporaryDirectory() as tmp:
            binary = Path(tmp) / 'sensor'
            subprocess.run(['cc', '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                            '-I', str(root / 'src'), str(root / 'src/pico-camera-sensor.c'),
                            str(root / f'tests/pico-camera-{kind}.c'), '-o', str(binary)],
                           check=True, capture_output=True, text=True, timeout=30)
            result = subprocess.run([str(binary)], check=True, capture_output=True, text=True, timeout=10)
            self.assertIn(f'PICO_CAMERA_{kind.upper()}_PASS', result.stdout)
