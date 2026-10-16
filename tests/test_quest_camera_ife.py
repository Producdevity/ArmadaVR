from pathlib import Path
import subprocess
import tempfile
import unittest


class QuestCameraIfeTests(unittest.TestCase):
    def test_commands_and_bounds(self):
        root = Path(__file__).resolve().parents[1]
        with tempfile.TemporaryDirectory() as tmp:
            binary = Path(tmp) / 'ife'
            subprocess.run(['cc', '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                            '-I', str(root / 'src'), str(root / 'src/quest-camera-ife.c'),
                            str(root / 'tests/quest-camera-ife.c'), '-o', str(binary)],
                           check=True, capture_output=True, text=True, timeout=30)
            result = subprocess.run([str(binary)], check=True, capture_output=True, text=True, timeout=10)
            self.assertIn('QUEST_CAMERA_IFE_PASS cases=3872 hardware=0', result.stdout)
