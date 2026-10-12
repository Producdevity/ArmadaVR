from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


class PicoControllerStreamTests(unittest.TestCase):
    def test_ordering_clock_and_reconnects(self):
        compiler = shutil.which('cc')
        self.assertIsNotNone(compiler, 'A C compiler is required for the driver tests')
        root = Path(__file__).resolve().parents[1]
        with tempfile.TemporaryDirectory() as temp:
            binary = Path(temp) / 'pico-controller-stream'
            subprocess.run([
                compiler, '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                '-fsanitize=address,undefined', '-I', str(root / 'src'),
                str(root / 'tests/pico-controller-stream.c'),
                *[str(root / 'src' / name) for name in ('pico-controller-stream.c', 'pico-controller.c', 'ssc-clock.c')],
                '-o', str(binary),
            ], check=True, capture_output=True, text=True, timeout=30)
            result = subprocess.run([str(binary)], check=True, capture_output=True, text=True, timeout=10)
            self.assertIn('Pico controller stream PASS:', result.stdout)
