import importlib.util
from pathlib import Path
import re
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("pico_station_test", ROOT / "tools/test-pico-station.py")
DRIVER_TEST = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(DRIVER_TEST)


class PicoStationTests(unittest.TestCase):
    def test_native_driver_lifecycle(self):
        patch = (ROOT / "patches/kernel/pico-neo3/0024-station-notifications.patch").read_text()
        addition = patch.split("--- /dev/null\n+++ b/drivers/misc/pico-station.c\n", 1)[1]
        header, *lines = addition.splitlines(keepends=True)
        match = re.fullmatch(r"@@ -0,0 \+1,(\d+) @@\n", header)
        self.assertIsNotNone(match)
        self.assertEqual(len(lines), int(match[1]))
        self.assertTrue(all(line.startswith("+") for line in lines))
        driver = "".join(line[1:] for line in lines)
        with tempfile.TemporaryDirectory() as temp:
            DRIVER_TEST.test_driver(driver, Path(temp), "cc")
            self.assertIn("74 station lifecycle cases passed", (Path(temp) / "test.log").read_text())
