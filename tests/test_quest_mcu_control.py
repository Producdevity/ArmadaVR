from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


class QuestMcuControlTests(unittest.TestCase):
    def test_control_transport(self):
        root = Path(__file__).resolve().parents[1]
        compiler = shutil.which("cc")
        self.assertIsNotNone(compiler)
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / "mcu"
            subprocess.run([
                compiler, "-std=gnu11", "-O2", "-ffp-contract=off", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-I", str(root / "src"),
                str(root / "src/quest-camera-mcu.c"), str(root / "src/quest-mcu-control.c"), str(root / "tests/quest-mcu-control.c"),
                "-o", str(binary),
            ], check=True, capture_output=True, text=True, timeout=30)
            result = subprocess.run([str(binary)], check=True, capture_output=True, text=True, timeout=10)
            self.assertIn("QUEST_MCU_CONTROL_PASS", result.stdout)
