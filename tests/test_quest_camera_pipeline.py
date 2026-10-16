from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


class QuestCameraPipelineTests(unittest.TestCase):
    def test_start_stop_ownership(self):
        root = Path(__file__).resolve().parents[1]
        compiler = shutil.which("cc")
        self.assertIsNotNone(compiler)
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / "match"
            subprocess.run([
                compiler, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-I", str(root / "src"),
                str(root / "src/quest-camera-control.c"), str(root / "src/quest-camera-pipeline.c"),
                str(root / "tests/quest-camera-pipeline.c"),
                "-o", str(binary),
            ], check=True, capture_output=True, text=True, timeout=30)
            result = subprocess.run([str(binary)], check=True, capture_output=True, text=True, timeout=10)
            self.assertIn("QUEST_CAMERA_PIPELINE_PASS cycles=600 faults=80", result.stdout)
