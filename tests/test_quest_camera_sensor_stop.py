import importlib.util
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("quest_camera_sensor_stop", ROOT / "tools/test-quest-camera-sensor-stop.py")
camera_sensor_stop = importlib.util.module_from_spec(spec)
spec.loader.exec_module(camera_sensor_stop)


class QuestCameraSensorStopTests(unittest.TestCase):
    def test_stop_failures_reach_caller_without_skipping_cleanup(self):
        with tempfile.TemporaryDirectory() as directory:
            report = camera_sensor_stop.run_tests(Path(directory) / "stop")
            self.assertTrue(report["passed"])
            self.assertEqual(report["runs"]["original"]["defects"], 4224)
            self.assertEqual(report["runs"]["patched"]["defects"], 0)
