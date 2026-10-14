import importlib.util
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("pico_usb_host", ROOT / "tools/test-pico-usb-host.py")
usb_host = importlib.util.module_from_spec(spec)
spec.loader.exec_module(usb_host)


class PicoUSBHostTests(unittest.TestCase):
    def test_power_failure_phy_and_notifier_cleanup(self):
        with tempfile.TemporaryDirectory() as directory:
            report = usb_host.run_tests(Path(directory) / "host")
            self.assertTrue(report["passed"])
            self.assertEqual(report["runs"]["original"]["defects"], 6208)
            self.assertEqual(report["runs"]["notifier"]["defects"], 2624)
            self.assertEqual(report["runs"]["patched"]["defects"], 0)
