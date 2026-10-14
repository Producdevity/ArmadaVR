from pathlib import Path
import subprocess
import tempfile
import unittest


class CameraSensorTests(unittest.TestCase):
    def test_command_encoding(self):
        root = Path(__file__).resolve().parents[1]
        with tempfile.TemporaryDirectory() as tmp:
            binary = Path(tmp) / 'sensor'
            subprocess.run(['cc', '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                            '-I', str(root / 'src'), str(root / 'src/pico-camera-sensor.c'),
                            str(root / 'tests/pico-camera-sensor.c'), '-o', str(binary)],
                           check=True, capture_output=True, text=True, timeout=30)
            result = subprocess.run([str(binary)], check=True, capture_output=True, text=True, timeout=10)
            self.assertIn('PICO_CAMERA_SENSOR_PASS', result.stdout)
