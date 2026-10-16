from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


class QuestMcuTransactionTests(unittest.TestCase):
    def test_sequence_lifetime_and_deadlines(self):
        root = Path(__file__).resolve().parents[1]
        compiler = shutil.which("cc")
        self.assertIsNotNone(compiler)
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / "transaction"
            subprocess.run([
                compiler, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-I", str(root / "src"),
                str(root / "src/quest-mcu-control.c"), str(root / "src/quest-mcu-transaction.c"),
                str(root / "tests/quest-mcu-transaction.c"), "-o", str(binary),
            ], check=True, capture_output=True, text=True, timeout=30)
            result = subprocess.run([str(binary)], check=True, capture_output=True, text=True, timeout=10)
            self.assertIn("completions=509 faults=17 hardware=0", result.stdout)
