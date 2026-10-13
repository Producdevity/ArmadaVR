from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest


class PicoSSCProbeTests(unittest.TestCase):
    def test_probe_errors_and_open_file_lifetime(self):
        root = Path(__file__).resolve().parents[1]
        patches = root / 'patches/kernel/pico-neo3'
        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp)
            shutil.copytree(root / 'tests/fixtures/pico-dsp-lifecycle', directory, dirs_exist_ok=True)
            for original, patch in ((True, '0035-slpi-loader-lifetime.patch'),
                                    (False, '0036-ssc-character-lifetime.patch')):
                subprocess.run(['patch', '--batch', '--fuzz=0', '--forward', '-p1', '-i', str(patches / patch)],
                               cwd=directory, check=True, capture_output=True, timeout=10)
                source = (directory / 'drivers/sensors/sensors_ssc.c').read_text()
                start = source.index('static int sensors_ssc_probe')
                end = source.index('static const struct of_device_id', start)
                (directory / 'probe.inc').write_text(source[start:end])
                binary = directory / 'test'
                build = subprocess.run(['cc', '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                                        '-Wno-unused-function', '-fsanitize=address,undefined',
                                        '-fno-sanitize-recover=all', '-I', temp,
                                        *(['-DEXPECT_OLD'] if original else []),
                                        str(root / 'tests/pico-ssc-probe.c'), '-o', str(binary)],
                                       capture_output=True, text=True, timeout=30)
                self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
                run = subprocess.run([str(binary)], capture_output=True, text=True, timeout=15)
                self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
                match = re.fullmatch(r'PICO_SSC_PROBE_PASS cases=(\d+) defects=(\d+)\n', run.stdout)
                self.assertIsNotNone(match)
                self.assertEqual(int(match[1]), 147)
                self.assertEqual(int(match[2]), 143 if original else 0)
