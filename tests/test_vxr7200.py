from pathlib import Path
import subprocess
import tempfile
import unittest


class Vxr7200Tests(unittest.TestCase):
    def test_video_observations(self):
        root = Path(__file__).resolve().parents[1]
        with tempfile.TemporaryDirectory() as temp:
            executable = Path(temp) / "vxr7200-video"
            subprocess.run([
                "cc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-I", str(root / "src"),
                str(root / "src/vxr7200-transport.c"), str(root / "src/vxr7200-video.c"),
                str(root / "tests/vxr7200-video.c"), "-o", str(executable),
            ], check=True, capture_output=True, timeout=30)
            result = subprocess.run([str(executable)], check=True, capture_output=True,
                                    text=True, timeout=10)
            self.assertIn("4096 VXR7200 video observations and failure checks passed", result.stdout)

    def test_native_power(self):
        root = Path(__file__).resolve().parents[1]
        with tempfile.TemporaryDirectory() as temp:
            executable = Path(temp) / "vxr7200-power"
            subprocess.run([
                "cc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-I", str(root / "src"),
                str(root / "src/vxr7200-power.c"), str(root / "tests/vxr7200-power.c"),
                "-o", str(executable),
            ], check=True, capture_output=True, timeout=30)
            result = subprocess.run([str(executable)], check=True, capture_output=True,
                                    text=True, timeout=10)
            self.assertIn("124 VXR7200 power failure scenarios passed", result.stdout)

    def test_native_transport(self):
        root = Path(__file__).resolve().parents[1]
        with tempfile.TemporaryDirectory() as temp:
            executable = Path(temp) / "vxr7200"
            subprocess.run([
                "cc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-I", str(root / "src"),
                str(root / "src/vxr7200-transport.c"), str(root / "tests/vxr7200-transport.c"),
                "-o", str(executable),
            ], check=True, capture_output=True, timeout=30)
            result = subprocess.run([str(executable)], check=True, capture_output=True,
                                    text=True, timeout=10)
            self.assertIn("12581 VXR7200 transport checks passed", result.stdout)
