from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


class QuestCameraPixelsTests(unittest.TestCase):
    def test_native_pixels_decoder(self):
        root = Path(__file__).resolve().parents[1]
        compiler = shutil.which("cc")
        self.assertIsNotNone(compiler)
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / "pixels"
            subprocess.run([
                compiler, "-std=c11", "-O2", "-ffp-contract=off", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-I", str(root / "src"),
                str(root / "src/quest-camera-pixels.c"), str(root / "tests/quest-camera-pixels.c"),
                "-o", str(binary),
            ], check=True, capture_output=True, text=True, timeout=30)
            result = subprocess.run([str(binary)], check=True, capture_output=True, text=True, timeout=10)
            self.assertIn("QUEST_CAMERA_PIXELS_PASS", result.stdout)
