from pathlib import Path
import subprocess
import tempfile
import unittest


class IonBufferTests(unittest.TestCase):
    def test_allocation_sync_and_failure_ownership(self):
        root = Path(__file__).resolve().parents[1]
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / "ion-buffer"
            subprocess.run(
                [
                    "cc",
                    "-std=c11",
                    "-O1",
                    "-g",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-fsanitize=address,undefined",
                    str(root / "tests/ion-buffer.c"),
                    "-o",
                    str(binary),
                ],
                check=True,
                capture_output=True,
                timeout=30,
            )
            result = subprocess.run(
                [str(binary)], check=True, capture_output=True, text=True, timeout=15
            )
            self.assertIn(
                "ION_BUFFER_UNIT_PASS allocations=384 failure_paths=5 sync_modes=6",
                result.stdout,
            )
