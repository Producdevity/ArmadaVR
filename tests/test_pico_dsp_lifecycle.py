from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest


class PicoDSPLifecycleTests(unittest.TestCase):
    def test_load_stop_and_remove_ownership(self):
        root = Path(__file__).resolve().parents[1]
        patch = root / 'patches/kernel/pico-neo3/0035-slpi-loader-lifetime.patch'
        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp)
            shutil.copytree(root / 'tests/fixtures/pico-dsp-lifecycle', directory, dirs_exist_ok=True)
            driver = directory / 'drivers/sensors/sensors_ssc.c'
            for original in (True, False):
                if not original:
                    subprocess.run(['patch', '--batch', '--fuzz=0', '--forward', '-p1', '-i', str(patch)],
                                   cwd=directory, check=True, capture_output=True, timeout=10)
                source = driver.read_text()
                start = source.index('#define IMAGE_LOAD_CMD')
                end = source.index('/*\n * Read virtual QTimer')
                (directory / 'driver.inc').write_text(source[start:end])
                binary = directory / 'test'
                build = subprocess.run(['cc', '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                                '-Wno-unused-parameter', '-Wno-unused-function', '-Wno-unused-variable',
                                '-pthread', '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                                '-I', temp, *(['-DEXPECT_OLD'] if original else []),
                                str(root / 'tests/pico-dsp-lifecycle.c'), '-o', str(binary)],
                               capture_output=True, text=True, timeout=30)
                self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
                result = subprocess.run([str(binary)], check=True, capture_output=True, text=True, timeout=30)
                match = re.fullmatch(r'PICO_DSP_LIFECYCLE_PASS cases=(\d+) defects=(\d+) premature_publications=(\d+)\n', result.stdout)
                self.assertIsNotNone(match)
                self.assertEqual(tuple(map(int, match.groups())), (640, 640, 640) if original else (4747, 0, 0))
