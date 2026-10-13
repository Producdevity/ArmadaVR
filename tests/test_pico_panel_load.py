from pathlib import Path
import re
import subprocess
import tempfile
import unittest


class PicoPanelLoadTests(unittest.TestCase):
    def test_failed_enable_restores_load_before_retry(self):
        root = Path(__file__).resolve().parents[1]
        patch = root / 'patches/kernel/pico-neo3/0034-panel-enable-load-rollback.patch'
        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp)
            driver = directory / 'techpack/display/msm/dsi/dsi_pwr.c'
            driver.parent.mkdir(parents=True)
            driver.write_bytes((root / 'tests/fixtures/pico-panel-load.inc').read_bytes())
            for original in (True, False):
                if not original:
                    subprocess.run(['patch', '--batch', '--fuzz=0', '--forward', '-p1', '-i', str(patch)],
                                   cwd=directory, check=True, capture_output=True, timeout=10)
                (directory / 'driver.inc').write_bytes(driver.read_bytes())
                binary = directory / 'test'
                subprocess.run(['cc', '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                                '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                                '-I', temp, *(['-DEXPECT_OLD'] if original else []),
                                str(root / 'tests/pico-panel-load.c'), '-o', str(binary)],
                               check=True, capture_output=True, text=True, timeout=30)
                result = subprocess.run([str(binary)], check=True, capture_output=True, text=True, timeout=15)
                matched = re.fullmatch(r'PICO_PANEL_LOAD_PASS cases=(\d+) load_defects=(\d+)\n', result.stdout)
                self.assertIsNotNone(matched)
                self.assertEqual(int(matched[1]), 432)
                self.assertEqual(int(matched[2]), 432 if original else 0)
