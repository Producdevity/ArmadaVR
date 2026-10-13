from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


def prepare(directory):
    pairs = [('pico-splash-display.inc', 'dsi/dsi_display.c', 'display.inc'),
             ('pico-splash-kms.inc', 'sde/sde_kms.c', 'kms.inc')]
    for fixture, path, _ in pairs:
        target = directory / 'techpack/display/msm' / path
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(ROOT / 'tests/fixtures' / fixture, target)
    for variant in ('original', 'fixed'):
        if variant == 'fixed':
            subprocess.run(['patch', '--batch', '--fuzz=0', '--forward', '-p1', '-i',
                            str(ROOT / 'patches/kernel/pico-neo3/0039-splash-startup-errors.patch')],
                           cwd=directory, check=True, capture_output=True, timeout=10)
        includes = directory / variant
        includes.mkdir()
        for _, path, include in pairs:
            shutil.copy2(directory / 'techpack/display/msm' / path, includes / include)


class PicoSplashTests(unittest.TestCase):
    def test_power_references_and_error_propagation(self):
        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp)
            prepare(directory)
            for variant in ('original', 'fixed'):
                binary = directory / ('test-' + variant)
                command = ['cc', '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                           '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                           '-I', str(directory / variant),
                           *(['-DEXPECT_OLD'] if variant == 'original' else []),
                           str(ROOT / 'tests/pico-splash.c'), '-o', str(binary)]
                result = subprocess.run(command, capture_output=True, text=True, timeout=30)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=30)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                match = re.fullmatch(r'PICO_SPLASH_PASS cases=(\d+) defects=(\d+)\n', result.stdout)
                self.assertIsNotNone(match)
                self.assertEqual(int(match[1]), 7680)
                if variant == 'original':
                    self.assertGreater(int(match[2]), 0)
                else:
                    self.assertEqual(int(match[2]), 0)
