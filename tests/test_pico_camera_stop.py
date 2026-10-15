import importlib.util
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("pico_camera_stop", ROOT / "tools/test-pico-camera-stop.py")
camera_stop = importlib.util.module_from_spec(spec)
spec.loader.exec_module(camera_stop)


class PicoCameraStopTests(unittest.TestCase):
    def test_stop_failures_reach_caller_without_skipping_cleanup(self):
        with tempfile.TemporaryDirectory() as directory:
            report = camera_stop.run_tests(Path(directory) / "stop")
            self.assertTrue(report["passed"])
            self.assertEqual(report["runs"]["original"]["defects"], 1794)
            self.assertEqual(report["runs"]["patched"]["defects"], 0)
