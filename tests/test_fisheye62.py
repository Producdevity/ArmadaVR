from pathlib import Path
import subprocess
import tempfile
import unittest


class Fisheye62Tests(unittest.TestCase):
    def test_inverse(self):
        root = Path(__file__).resolve().parents[1]
        with tempfile.TemporaryDirectory() as temp:
            executable = Path(temp) / "fisheye62-inverse"
            subprocess.run([
                "cc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-I", str(root / "src"),
                str(root / "src/fisheye62.c"), str(root / "tests/fisheye62-inverse.c"),
                "-lm", "-o", str(executable),
            ], check=True, capture_output=True, timeout=30)
            result = subprocess.run([str(executable)], check=True, capture_output=True,
                                    text=True, timeout=15)
            self.assertIn("28381 inverse projection cases", result.stdout)

    def test_projection(self):
        root = Path(__file__).resolve().parents[1]
        with tempfile.TemporaryDirectory() as temp:
            executable = Path(temp) / "fisheye62"
            subprocess.run([
                "cc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-I", str(root / "src"),
                str(root / "src/fisheye62.c"), str(root / "tests/fisheye62.c"),
                "-lm", "-o", str(executable),
            ], check=True, capture_output=True, timeout=30)
            result = subprocess.run([str(executable)], check=True, capture_output=True,
                                    text=True, timeout=10)
            self.assertIn("37441 Fisheye62 projection cases", result.stdout)
