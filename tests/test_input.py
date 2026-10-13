import importlib.util
from pathlib import Path
import unittest
from unittest.mock import patch
import subprocess
import tempfile

spec = importlib.util.spec_from_file_location("input_test", Path(__file__).parents[1] / "tools/test-input.py")
inputs = importlib.util.module_from_spec(spec)
spec.loader.exec_module(inputs)


class FragmentedConnection:
    def __init__(self, data):
        self.data = bytearray(data)

    def recv(self, count):
        chunk = self.data[:min(count, 3)]
        del self.data[:len(chunk)]
        return chunk


class RemoteInputTests(unittest.TestCase):
    def test_fragmented_packet_preserves_the_native_template(self):
        data = inputs.HEADER + bytes(range(256)) + bytes(112)
        self.assertEqual(inputs.receive(FragmentedConnection(data)), data)

    def test_closed_connection_and_wrong_protocol_are_rejected(self):
        with self.assertRaises(ConnectionError):
            inputs.receive(FragmentedConnection(inputs.HEADER))
        with self.assertRaisesRegex(ValueError, "Unsupported"):
            inputs.receive(FragmentedConnection(b"mndrmt2\0" + bytes(368)))

    def test_rendered_input_rejects_stale_and_reversed_eye_captures(self):
        red, blue = bytes((204, 13, 13)), bytes((13, 13, 204))
        baseline = (red * 640 + blue * 640) * 800
        with tempfile.TemporaryDirectory() as directory:
            with patch.object(inputs.subprocess, "run", return_value=subprocess.CompletedProcess([], 0, baseline)):
                with self.assertRaisesRegex(ValueError, "not visible in left"):
                    inputs.check_captures(Path(directory))
            reversed_eyes = (blue * 640 + red * 640) * 800
            with patch.object(inputs.subprocess, "run", return_value=subprocess.CompletedProcess([], 0, reversed_eyes)):
                with self.assertRaisesRegex(ValueError, "reversed"):
                    inputs.check_captures(Path(directory))

    def test_explicit_runtime_is_used_by_the_service_and_application_environment(self):
        for runtime in (None, "/opt/reframed-vr/monado/share/openxr/1/openxr_monado.json"):
            with self.subTest(runtime=runtime), tempfile.TemporaryDirectory() as directory:
                environment = {} if runtime is None else {"XR_RUNTIME_JSON": runtime}
                with patch.dict(inputs.os.environ, environment, clear=True), \
                        patch.object(inputs.subprocess, "Popen", side_effect=RuntimeError("captured")) as launch:
                    with self.assertRaisesRegex(RuntimeError, "captured"):
                        inputs.run(Path(directory) / "test", Path("xr-probe"))
                    self.assertEqual(launch.call_args.kwargs["env"]["XR_RUNTIME_JSON"],
                                     runtime or "/usr/share/openxr/1/openxr_monado.json")
