from pathlib import Path
import subprocess
import tempfile
import unittest


class PicoCameraCaptureTests(unittest.TestCase):
    def test_completion_and_ownership(self):
        self.compile_test("pico-camera-capture", "PICO_CAMERA_CAPTURE_PASS")

    def test_kernel_events(self):
        self.compile_test("pico-camera-events", "PICO_CAMERA_EVENTS_PASS")

    def compile_test(self, name, marker):
        root = Path(__file__).resolve().parents[1]
        with tempfile.TemporaryDirectory() as temp:
            binary = Path(temp) / "capture"
            subprocess.run([
                "cc", "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                "-I", str(root / "src"), str(root / "src" / (name + ".c")),
                str(root / "tests" / (name + ".c")), "-o", str(binary),
            ], check=True, capture_output=True, text=True, timeout=30)
            result = subprocess.run([str(binary)], check=True, capture_output=True, text=True, timeout=30)
            self.assertIn(marker, result.stdout)
