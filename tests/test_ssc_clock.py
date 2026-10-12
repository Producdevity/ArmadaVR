from pathlib import Path
import shutil
import platform
import subprocess
import sys
import tempfile
import unittest


class SscClockTests(unittest.TestCase):
    def test_monotonic_counter_capture(self):
        compiler = shutil.which("cc")
        self.assertIsNotNone(compiler)
        root = Path(__file__).resolve().parents[1]
        native = sys.platform == "linux" and platform.machine() in ("aarch64", "arm64")
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / "ssc-clock-linux"
            subprocess.run([
                compiler, "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-I", str(root / "src"),
                str(root / "tests/ssc-clock-linux.c"), str(root / "src/ssc-clock-linux.c"),
                str(root / "src/ssc-clock.c"), *(["-Wl,--wrap=clock_gettime"] if native else []),
                "-o", str(binary),
            ], check=True, capture_output=True, text=True, timeout=30)
            result = subprocess.run([str(binary)], check=True, capture_output=True, text=True, timeout=10)
            self.assertIn("SSC_CLOCK_CAPTURE_PASS cycles=64" if native else
                          "SSC_CLOCK_CAPTURE_UNSUPPORTED_PASS", result.stdout)

    def test_clock_conversion_and_rejection(self):
        compiler = shutil.which("cc")
        self.assertIsNotNone(compiler, "A C compiler is required for the driver tests")
        root = Path(__file__).resolve().parents[1]
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / "ssc-clock"
            subprocess.run([
                compiler, "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-I", str(root / "src"),
                str(root / "tests/ssc-clock.c"), str(root / "src/ssc-clock.c"), "-o", str(binary),
            ], check=True, capture_output=True, text=True, timeout=30)
            result = subprocess.run([str(binary)], check=True, capture_output=True, text=True, timeout=10)
            self.assertIn("SSC clock alignment and rejection checks passed", result.stdout)
