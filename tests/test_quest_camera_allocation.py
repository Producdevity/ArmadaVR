from pathlib import Path
import subprocess
import tempfile
import unittest


class QuestCameraAllocationTests(unittest.TestCase):
    def test_allocation_lifetime(self):
        root = Path(__file__).resolve().parents[1]
        with tempfile.TemporaryDirectory() as tmp:
            binary = Path(tmp) / 'allocation'
            subprocess.run(['cc', '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                            *[f'-D{name}=quest_test_{name}' for name in ('ioctl', 'fcntl', 'mmap', 'munmap', 'close')],
                            '-I', str(root / 'src'), str(root / 'src/quest-camera-memory.c'),
                            str(root / 'src/quest-camera-allocation.c'), str(root / 'tests/quest-camera-allocation.c'),
                            '-o', str(binary)], check=True, capture_output=True, text=True, timeout=30)
            result = subprocess.run([str(binary)], check=True, capture_output=True, text=True, timeout=10)
            self.assertIn('QUEST_CAMERA_ALLOCATION_PASS cycles=600 faults=51 hardware=0', result.stdout)
