from pathlib import Path
import subprocess
import tempfile
import unittest


class PicoFastRPCTests(unittest.TestCase):
    def test_vendor_ioctl_packing_and_error_propagation(self):
        root = Path(__file__).resolve().parents[1]
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / "pico-fastrpc"
            subprocess.run(
                [
                    "cc", "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                    str(root / "tests/pico-fastrpc.c"), "-o", str(binary),
                ],
                check=True, capture_output=True, timeout=30,
            )
            result = subprocess.run(
                [str(binary)], check=True, capture_output=True, text=True, timeout=15
            )
            self.assertIn("cases=4608", result.stdout)
            self.assertIn("PICO_FASTRPC_UNIT_PASS", result.stdout)
