from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from test_pico_splash import prepare, ROOT


class PicoSplashMuxTests(unittest.TestCase):
    def test_splash_checks_route_before_clock_and_supply_votes(self):
        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp)
            prepare(directory)
            for original in (True, False):
                if not original:
                    subprocess.run(['patch', '--batch', '--fuzz=0', '--forward', '-p1', '-i',
                                    str(ROOT / 'patches/kernel/pico-neo3/0040-splash-native-mux.patch')],
                                   cwd=directory, capture_output=True, check=True, timeout=10)
                    shutil.copy2(directory / 'techpack/display/msm/dsi/dsi_display.c', directory / 'fixed/display.inc')
                binary = directory / 'test'
                command = ['cc', '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                           '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                           '-I', str(directory / 'fixed'), *(['-DEXPECT_MUX_OLD'] if original else []),
                           str(ROOT / 'tests/pico-splash-mux.c'), '-o', str(binary)]
                build = subprocess.run(command, capture_output=True, text=True, timeout=30)
                self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
                result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=30)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn('PICO_SPLASH_MUX_PASS refusals=90 defects=' + ('720' if original else '0'), result.stdout)
