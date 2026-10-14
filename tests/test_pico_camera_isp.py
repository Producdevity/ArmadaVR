from pathlib import Path
import subprocess
import tempfile
import unittest


class PicoCameraISPTests(unittest.TestCase):
    def test_acquire_encoding(self):
        self.compile_test("pico-camera-isp", "PICO_CAMERA_ISP_PASS")

    def test_phy_encoding(self):
        self.compile_test("pico-camera-csiphy", "PICO_CAMERA_CSIPHY_PASS")

    def test_rdi_encoding(self):
        self.compile_test("pico-camera-rdi", "PICO_CAMERA_RDI_PASS")

    def test_packet_encoding(self):
        self.compile_test("pico-camera-packet", "PICO_CAMERA_PACKET_PASS")

    def compile_test(self, name, expected):
        root = Path(__file__).resolve().parents[1]
        with tempfile.TemporaryDirectory() as temp:
            binary = Path(temp) / name
            subprocess.run([
                "cc", "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-I", str(root / "src"),
                str(root / "tests" / (name + ".c")), str(root / "src/pico-camera-isp.c"),
                "-o", str(binary),
            ], check=True, capture_output=True, text=True, timeout=30)
            result = subprocess.run([str(binary)], check=True, capture_output=True, text=True, timeout=10)
            self.assertIn(expected, result.stdout)
