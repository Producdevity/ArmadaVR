from pathlib import Path
import subprocess
import tempfile
import unittest


class PicoCameraCaptureTests(unittest.TestCase):
    def test_completion_and_ownership(self):
        self.compile_test("pico-camera-capture", "PICO_CAMERA_CAPTURE_PASS")

    def test_kernel_events(self):
        self.compile_test("pico-camera-events", "PICO_CAMERA_EVENTS_PASS")

    def test_control_ioctls(self):
        self.compile_test("pico-camera-control", "PICO_CAMERA_CONTROL_PASS", ["pico-camera-events.c"])

    def test_device_acquisition(self):
        self.compile_test("pico-camera-control", "PICO_CAMERA_ACQUIRE_PASS",
                          ["pico-camera-events.c"], "pico-camera-acquire")

    def test_camera_memory(self):
        self.compile_test("pico-camera-control", "PICO_CAMERA_MEMORY_PASS",
                          ["pico-camera-events.c"], "pico-camera-memory")

    def test_event_transport(self):
        self.compile_test("pico-camera-control", "PICO_CAMERA_EVENT_IO_PASS",
                          ["pico-camera-events.c"], "pico-camera-event-io")

    def test_camera_stream(self):
        self.compile_test("pico-camera-stream", "PICO_CAMERA_STREAM_PASS",
                          ["pico-camera-events.c", "pico-camera-capture.c"])

    def compile_test(self, name, marker, dependencies=(), test_name=None):
        root = Path(__file__).resolve().parents[1]
        with tempfile.TemporaryDirectory() as temp:
            binary = Path(temp) / "capture"
            subprocess.run([
                "cc", "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                "-I", str(root / "src"), str(root / "src" / (name + ".c")),
                str(root / "tests" / ((test_name or name) + ".c")),
                *[str(root / "src" / dependency) for dependency in dependencies], "-o", str(binary),
            ], check=True, capture_output=True, text=True, timeout=30)
            result = subprocess.run([str(binary)], check=True, capture_output=True, text=True, timeout=30)
            self.assertIn(marker, result.stdout)
