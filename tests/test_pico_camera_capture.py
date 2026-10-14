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

    def test_request_lifecycle(self):
        self.compile_test("pico-camera-request", "PICO_CAMERA_REQUEST_PASS",
                          ["pico-camera-control.c", "pico-camera-events.c", "pico-camera-isp.c",
                           "pico-camera-stream.c", "pico-camera-capture.c"])

    def test_pipeline_lifecycle(self):
        self.compile_test("pico-camera-pipeline", "PICO_CAMERA_PIPELINE_PASS",
                          ["pico-camera-control.c", "pico-camera-events.c"])

    def test_sensor_setup(self):
        self.compile_test("pico-camera-sensor-setup", "PICO_CAMERA_SENSOR_SETUP_PASS",
                          ["pico-camera-control.c", "pico-camera-events.c", "pico-camera-sensor.c", "pico-camera-isp.c"])

    def test_probe_setup(self):
        self.compile_test("pico-camera-probe-setup", "PICO_CAMERA_PROBE_SETUP_PASS",
                          ["pico-camera-control.c", "pico-camera-events.c", "pico-camera-sensor.c", "pico-camera-isp.c"])

    def test_session_acquisition(self):
        self.compile_test("pico-camera-session", "PICO_CAMERA_SESSION_PASS",
                          ["pico-camera-control.c", "pico-camera-events.c", "pico-camera-isp.c"])

    def test_complete_device_setup(self):
        self.compile_test("pico-camera-rig", "PICO_CAMERA_RIG_PASS",
                          ["pico-camera-session.c", "pico-camera-control.c", "pico-camera-events.c",
                           "pico-camera-isp.c", "pico-camera-sensor.c", "pico-camera-sensor-setup.c",
                           "pico-camera-ife.c", "pico-camera-front-end.c", "pico-camera-pipeline.c"])

    def test_ife_commands(self):
        self.compile_test("pico-camera-ife", "PICO_CAMERA_IFE_PASS")

    def test_front_end_setup(self):
        self.compile_test("pico-camera-front-end", "PICO_CAMERA_FRONT_END_PASS",
                          ["pico-camera-control.c", "pico-camera-events.c", "pico-camera-ife.c", "pico-camera-isp.c"])

    def test_stereo_transport(self):
        self.compile_test("pico-camera-transport", "PICO_CAMERA_TRANSPORT_PASS",
                          ["pico-camera-request.c", "pico-camera-control.c", "pico-camera-events.c", "pico-camera-isp.c",
                           "pico-camera-stream.c", "pico-camera-capture.c"])

    def test_buffer_ownership(self):
        root = Path(__file__).resolve().parents[1]
        with tempfile.TemporaryDirectory() as temp:
            binary = Path(temp) / "buffer"
            subprocess.run([
                "cc", "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                str(root / "tests/pico-camera-buffer.c"), str(root / "src/pico-camera-events.c"),
                "-o", str(binary),
            ], check=True, capture_output=True, text=True, timeout=30)
            result = subprocess.run([str(binary)], check=True, capture_output=True, text=True, timeout=30)
            self.assertIn("PICO_CAMERA_BUFFER_PASS", result.stdout)

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
