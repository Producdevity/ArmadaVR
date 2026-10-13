from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest


class PicoPHYCleanupTests(unittest.TestCase):
    def test_pending_regulator_cleanup_overrides_cached_state(self):
        root = Path(__file__).resolve().parents[1]
        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp)
            shutil.copytree(root / 'tests/fixtures/pico-panel-cleanup', directory, dirs_exist_ok=True)
            phy = directory / 'techpack/display/msm/dsi/dsi_phy.c'
            shutil.copy2(root / 'tests/fixtures/pico-phy-cleanup.inc', phy)
            patches = root / 'patches/kernel/pico-neo3'
            subprocess.run(['patch', '--batch', '--fuzz=0', '--forward', '-p1', '-i',
                            str(patches / '0037-panel-cleanup-state.patch')],
                           cwd=directory, check=True, capture_output=True, timeout=10)
            source = (directory / 'techpack/display/msm/dsi/dsi_pwr.c').read_text()
            header = (directory / 'techpack/display/msm/dsi/dsi_pwr.h').read_text()
            declarations = header[header.index('struct dsi_vreg {'):header.index('/**\n * dsi_pwr_of_get_vreg_data')]
            start = source.index('static int dsi_pwr_cleanup_vregs')
            lower = source[start:source.index('/**\n * dsi_pwr_of_get_vreg_data', start)]
            start = source.index('int dsi_pwr_enable_regulator(')
            upper = source[start:source.index('/*\n * dsi_pwr_panel_regulator_mode_set', start)]
            (directory / 'types.inc').write_text(declarations)
            (directory / 'driver.inc').write_text(lower + upper)
            for original in (True, False):
                if not original:
                    subprocess.run(['patch', '--batch', '--fuzz=0', '--forward', '-p1', '-i',
                                    str(patches / '0038-phy-cleanup-state.patch')],
                                   cwd=directory, check=True, capture_output=True, timeout=10)
                shutil.copy2(phy, directory / 'phy.inc')
                binary = directory / 'test'
                command = ['cc', '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                           '-Wno-sign-compare', '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                           '-I', temp, *(['-DEXPECT_PHY_OLD'] if original else []),
                           str(root / 'tests/pico-phy-cleanup.c'), '-o', str(binary)]
                build = subprocess.run(command, capture_output=True, text=True, timeout=30)
                self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
                result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=30)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                match = re.fullmatch(r'PICO_PHY_CLEANUP_PASS cases=(\d+) defects=(\d+)\n', result.stdout)
                self.assertIsNotNone(match)
                self.assertEqual(int(match[1]), 39)
                self.assertEqual(int(match[2]), 78 if original else 0)
