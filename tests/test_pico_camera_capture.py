from pathlib import Path
import subprocess
import tempfile
import unittest


class PicoCameraCaptureTests(unittest.TestCase):
    def test_completion_and_ownership(self):
        root = Path(__file__).resolve().parents[1]
        with tempfile.TemporaryDirectory() as temp:
            binary = Path(temp) / "capture"
            subprocess.run([
                "cc", "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                "-I", str(root / "src"), str(root / "src/pico-camera-capture.c"),
                str(root / "tests/pico-camera-capture.c"), "-o", str(binary),
            ], check=True, capture_output=True, text=True, timeout=30)
            result = subprocess.run([str(binary)], check=True, capture_output=True, text=True, timeout=30)
            self.assertIn("PICO_CAMERA_CAPTURE_PASS", result.stdout)
