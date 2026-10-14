from pathlib import Path
import subprocess
import tempfile
import unittest


class PicoCameraTests(unittest.TestCase):
    def test_camera_frames(self):
        root = Path(__file__).resolve().parents[1]
        with tempfile.TemporaryDirectory() as temp:
            binary = Path(temp) / "pico-camera"
            subprocess.run([
                "cc", "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-I", str(root / "src"),
                str(root / "tests/pico-camera.c"), str(root / "src/pico-camera.c"),
                "-o", str(binary),
            ], check=True, capture_output=True, text=True, timeout=30)
            result = subprocess.run([str(binary)], check=True, capture_output=True, text=True, timeout=10)
            self.assertIn("PICO_CAMERA_PASS", result.stdout)
