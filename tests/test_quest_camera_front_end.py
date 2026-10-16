from pathlib import Path
import subprocess
import tempfile
import unittest


class QuestCameraFrontEndTests(unittest.TestCase):
    def test_owned_configuration(self):
        root = Path(__file__).resolve().parents[1]
        with tempfile.TemporaryDirectory() as tmp:
            binary = Path(tmp) / 'setup'
            sources = ('control', 'memory', 'allocation', 'ife', 'isp', 'blob', 'packet', 'front-end')
            subprocess.run(['cc', '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                            *[f'-D{name}=quest_test_{name}' for name in ('ioctl', 'fcntl', 'mmap', 'munmap', 'close')],
                            '-I', str(root / 'src'), *[str(root / f'src/quest-camera-{name}.c') for name in sources],
                            str(root / 'tests/quest-camera-front-end.c'), '-o', str(binary)],
                           check=True, capture_output=True, text=True, timeout=30)
            result = subprocess.run([str(binary)], check=True, capture_output=True, text=True, timeout=10)
            self.assertIn('QUEST_CAMERA_FRONT_END_PASS cycles=804 faults=134 hardware=0', result.stdout)
