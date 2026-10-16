from pathlib import Path
import subprocess
import tempfile
import unittest


class QuestCameraPacketTests(unittest.TestCase):
    def test_packet_and_image_bounds(self):
        root = Path(__file__).resolve().parents[1]
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / "packet"
            subprocess.run([
                "cc", "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-I", str(root / "src"),
                str(root / "src/quest-camera-packet.c"), str(root / "src/quest-camera-blob.c"), str(root / "tests/quest-camera-packet.c"),
                "-o", str(binary),
            ], check=True, capture_output=True, text=True, timeout=30)
            result = subprocess.run([str(binary)], check=True, capture_output=True, text=True, timeout=10)
            self.assertIn("QUEST_CAMERA_PACKET_PASS", result.stdout)
