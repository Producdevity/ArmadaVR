from pathlib import Path
import re
import subprocess
import tempfile
import unittest


class PicoPanelMuxTests(unittest.TestCase):
    def test_gpio_ownership_and_native_route(self):
        root = Path(__file__).resolve().parents[1]
        patch = (root / "patches/kernel/pico-neo3/0025-native-panel-mux.patch").read_text()
        section = patch.split("+++ b/techpack/display/msm/dsi/pico_panel_mux.c\n", 1)[1]
        section = section.split("diff --git", 1)[0]
        header, *lines = section.splitlines(keepends=True)
        count = re.fullmatch(r"@@ -0,0 \+1,(\d+) @@\n", header)
        self.assertIsNotNone(count)
        self.assertEqual(len(lines), int(count[1]))
        self.assertTrue(all(line.startswith("+") for line in lines))
        driver = "".join(line[1:] for line in lines)
        driver = re.sub(r"^#include [^\n]+\n", "", driver, flags=re.M)
        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp)
            (directory / "driver.inc").write_text(driver)
            executable = directory / "panel-mux"
            subprocess.run([
                "cc", "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-I", temp,
                str(root / "tests/pico-panel-mux.c"), "-o", str(executable),
            ], check=True, capture_output=True, timeout=30)
            result = subprocess.run([str(executable)], check=True, capture_output=True,
                                    text=True, timeout=15)
            self.assertIn("panel mux ownership and state checks passed", result.stdout)
