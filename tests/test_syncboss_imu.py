from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


class SyncbossImuTests(unittest.TestCase):
    def run_fixture(self, name, message):
        compiler = shutil.which("cc")
        self.assertIsNotNone(compiler, "A C compiler is required for the driver tests")
        root = Path(__file__).resolve().parents[1]
        with tempfile.TemporaryDirectory() as temp:
            executable = Path(temp) / name
            subprocess.run([
                compiler, "-std=c11", "-O2", "-ffp-contract=off", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-I", str(root / "src"), str(root / f"tests/{name}.c"),
                str(root / "src/syncboss-imu.c"), "-o", str(executable),
            ], check=True, capture_output=True, text=True, timeout=30)
            result = subprocess.run([str(executable)], check=True, capture_output=True,
                                    text=True, timeout=10)
            self.assertIn(message, result.stdout)

    def test_native_decoder(self):
        self.run_fixture("syncboss-imu", "conversion and rejection checks passed")

    def test_fifo_decoder(self):
        self.run_fixture("syncboss-fifo", "FIFO framing and clock checks passed")

    def test_direct_decoder(self):
        self.run_fixture("syncboss-direct", "direct-channel framing and clock checks passed")

    def test_stream_ordering(self):
        self.run_fixture("syncboss-stream", "stream freshness and ordering checks passed")
