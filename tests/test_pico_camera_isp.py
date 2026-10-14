from pathlib import Path
import subprocess
import tempfile
import unittest


class PicoCameraISPTests(unittest.TestCase):
    def test_acquire_encoding(self):
        root = Path(__file__).resolve().parents[1]
        with tempfile.TemporaryDirectory() as temp:
            binary = Path(temp) / "pico-camera-isp"
            subprocess.run([
                "cc", "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-I", str(root / "src"),
                str(root / "tests/pico-camera-isp.c"), str(root / "src/pico-camera-isp.c"),
                "-o", str(binary),
            ], check=True, capture_output=True, text=True, timeout=30)
            result = subprocess.run([str(binary)], check=True, capture_output=True, text=True, timeout=10)
            self.assertIn("PICO_CAMERA_ISP_PASS", result.stdout)
