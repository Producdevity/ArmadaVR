from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest


class PicoPanelCleanupTests(unittest.TestCase):
    def test_cleanup_errors_and_retry_ownership(self):
        root = Path(__file__).resolve().parents[1]
        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp)
            shutil.copytree(root / 'tests/fixtures/pico-panel-cleanup', directory, dirs_exist_ok=True)
            for original in (True, False):
                if not original:
                    subprocess.run(['patch', '--batch', '--fuzz=0', '--forward', '-p1', '-i',
                                    str(root / 'patches/kernel/pico-neo3/0037-panel-cleanup-state.patch')],
                                   cwd=directory, check=True, capture_output=True, timeout=10)
                source = (directory / 'techpack/display/msm/dsi/dsi_pwr.c').read_text()
                header = (directory / 'techpack/display/msm/dsi/dsi_pwr.h').read_text()
                declarations = header[header.index('struct dsi_vreg {'):header.index('/**\n * dsi_pwr_of_get_vreg_data')]
                start = source.index('static int dsi_pwr_enable_vregs' if original else 'static int dsi_pwr_cleanup_vregs')
                lower = source[start:source.index('/**\n * dsi_pwr_of_get_vreg_data', start)]
                start = source.index('int dsi_pwr_enable_regulator(')
                upper = source[start:source.index('/*\n * dsi_pwr_panel_regulator_mode_set', start)]
                (directory / 'types.inc').write_text(declarations)
                (directory / 'driver.inc').write_text(lower + upper)
                binary = directory / 'test'
                command = ['cc', '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                           '-Wno-sign-compare', '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                           '-I', temp, *(['-DEXPECT_OLD'] if original else []),
                           str(root / 'tests/pico-panel-cleanup.c'), '-o', str(binary)]
                build = subprocess.run(command, capture_output=True, text=True, timeout=30)
                self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
                result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=30)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                match = re.fullmatch(r'PICO_PANEL_CLEANUP_PASS cases=(\d+) defects=(\d+)\n', result.stdout)
                self.assertIsNotNone(match)
                self.assertEqual(int(match[1]), 11384)
                self.assertEqual(int(match[2]), 60324 if original else 0)
