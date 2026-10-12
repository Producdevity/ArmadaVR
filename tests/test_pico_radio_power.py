from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest


class PicoRadioPowerTests(unittest.TestCase):
    def test_rf_gpio_failures_unwind_radio_power(self):
        root = Path(__file__).resolve().parents[1]
        patch = root / "patches/kernel/pico-neo3/0032-wlan-rffem-errors.patch"
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            shutil.copytree(root / "tests/fixtures/pico-radio-power", directory, dirs_exist_ok=True)
            for old in (True, False):
                if not old:
                    subprocess.run(["patch", "--batch", "--fuzz=0", "--forward", "-p1", "-i", str(patch)],
                                   cwd=directory, check=True, capture_output=True, timeout=15)
                executable = directory / "radio-power"
                subprocess.run(["cc", "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                                "-Wno-unused-parameter", "-fsanitize=address,undefined", "-isystem", temporary,
                                *(["-DEXPECT_OLD"] if old else []), str(root / "tests/pico-radio-power.c"),
                                "-o", str(executable)], check=True, capture_output=True, timeout=30)
                result = subprocess.run([str(executable)], check=True, capture_output=True, text=True, timeout=15)
                invalid = int(re.search(r"; (\d+) ignored errors", result.stdout)[1])
                self.assertEqual(invalid, 96 if old else 0)
