from pathlib import Path
import subprocess
import tempfile
import unittest


class ImuTimeAlignmentTests(unittest.TestCase):
    def test_alignment_and_stream_boundaries(self):
        root = Path(__file__).resolve().parents[1]
        with tempfile.TemporaryDirectory() as temp:
            executable = Path(temp) / "imu-time-align"
            subprocess.run([
                "cc", "-std=c11", "-O2", "-ffp-contract=off", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-I", str(root / "src"),
                str(root / "src/imu-time-align.c"), str(root / "tests/imu-time-align.c"),
                "-lm", "-o", str(executable),
            ], check=True, capture_output=True, text=True, timeout=30)
            result = subprocess.run([str(executable)], check=True, capture_output=True,
                                    text=True, timeout=10)
            self.assertIn("time-aligned vector comparisons and rejection checks passed", result.stdout)
