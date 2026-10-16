from pathlib import Path
import subprocess
import tempfile
import unittest


class QuestCameraPoolTests(unittest.TestCase):
    def test_owned_stream_pool(self):
        root = Path(__file__).resolve().parents[1]
        with tempfile.TemporaryDirectory() as tmp:
            binary = Path(tmp) / 'setup'
            sources = ('buffer', 'memory', 'allocation', 'packet', 'pool', 'capture', 'metadata', 'pixels', 'shutter')
            subprocess.run(['cc', '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                            *[f'-D{name}=quest_test_{name}' for name in ('ioctl', 'fcntl', 'mmap', 'munmap', 'close')],
                            '-I', str(root / 'src'), *[str(root / f'src/quest-camera-{name}.c') for name in sources],
                            str(root / 'tests/quest-camera-pool.c'), '-o', str(binary)],
                           check=True, capture_output=True, text=True, timeout=30)
            result = subprocess.run([str(binary)], check=True, capture_output=True, text=True, timeout=30)
            self.assertIn('QUEST_CAMERA_POOL_PASS cycles=120 faults=2846 frames=512 hardware=0', result.stdout)
