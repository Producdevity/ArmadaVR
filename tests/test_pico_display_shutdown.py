from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def prepare(directory):
    source = directory / 'techpack/display/msm/dsi/dsi_display.c'
    source.parent.mkdir(parents=True)
    tail = '\nstatic int __init dsi_display_register(void)\n'
    source.write_text((ROOT / 'tests/fixtures/pico-display-unprepare.inc').read_text() + tail)
    for variant in ('original', 'fixed'):
        (directory / variant).mkdir()
        if variant == 'fixed':
            subprocess.run(['patch', '--batch', '--fuzz=0', '--forward', '-p1', '-i',
                            str(ROOT / 'patches/kernel/pico-neo3/0041-display-shutdown-errors.patch')],
                           cwd=directory, check=True, capture_output=True, timeout=10)
        (directory / variant / 'display.inc').write_text(source.read_text().removesuffix(tail))


class DisplayShutdownTests(unittest.TestCase):
    def test_first_error_survives_all_later_cleanup_results(self):
        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp)
            prepare(directory)
            for variant in ('original', 'fixed'):
                binary = directory / variant / 'test'
                command = ['cc', '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                           '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                           '-I', str(binary.parent), *(['-DEXPECT_OLD'] if variant == 'original' else []),
                           str(ROOT / 'tests/pico-display-shutdown.c'), '-o', str(binary)]
                subprocess.run(command, check=True, capture_output=True, timeout=30)
                result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=30)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn('PICO_DISPLAY_SHUTDOWN_PASS cases=26244', result.stdout)
                if variant == 'fixed':
                    self.assertIn('defects=0', result.stdout)


if __name__ == '__main__':
    unittest.main()
