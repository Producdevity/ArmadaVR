from pathlib import Path
import re
import subprocess
import tempfile
import unittest


class PicoPanelRegulatorTests(unittest.TestCase):
    def test_failed_enable_does_not_suppress_retry(self):
        root = Path(__file__).resolve().parents[1]
        patch_path = root / "patches/kernel/pico-neo3/0030-panel-regulator-reference.patch"
        patch = patch_path.read_text()
        for header, body in re.findall(r"(@@ [^\n]+@@)\n(.*?)(?=@@ |\Z)", patch, re.S):
            old, new = re.fullmatch(r"@@ -\d+,(\d+) \+\d+,(\d+) @@", header).groups()
            self.assertEqual(sum(line[0] in " -" for line in body.splitlines()), int(old))
            self.assertEqual(sum(line[0] in " +" for line in body.splitlines()), int(new))
        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp)
            driver = directory / "techpack/display/msm/dsi/dsi_pwr.c"
            driver.parent.mkdir(parents=True)
            driver.write_bytes((root / "tests/fixtures/pico-panel-regulator.inc").read_bytes())
            for side in ("-", "+"):
                if side == "+":
                    subprocess.run([
                        "patch", "--batch", "--fuzz=0", "--forward", "-p1", "-i", str(patch_path),
                    ], cwd=directory, check=True, capture_output=True, timeout=15)
                (directory / "driver.inc").write_bytes(driver.read_bytes())
                executable = directory / "panel-regulator"
                subprocess.run([
                    "cc", "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-I", temp,
                    *(["-DEXPECT_OLD"] if side == "-" else []),
                    str(root / "tests/pico-panel-regulator.c"), "-o", str(executable),
                ], check=True, capture_output=True, timeout=30)
                result = subprocess.run([str(executable)], check=True, capture_output=True,
                                        text=True, timeout=15)
                defects = int(re.search(r"; (\d+) reference defects", result.stdout)[1])
                if side == "-":
                    self.assertGreater(defects, 0)
                else:
                    self.assertEqual(defects, 0)
