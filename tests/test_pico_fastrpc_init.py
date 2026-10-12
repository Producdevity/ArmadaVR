import importlib.util
from pathlib import Path
import tempfile
import unittest


class PicoFastRPCInitTests(unittest.TestCase):
    def test_channel_is_validated_before_security_policy(self):
        root = Path(__file__).resolve().parents[1]
        spec = importlib.util.spec_from_file_location(
            "pico_fastrpc_test", root / "tools/test-pico-fastrpc.py"
        )
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        with tempfile.TemporaryDirectory() as temporary:
            report = module.run_tests(Path(temporary) / "run")
            self.assertTrue(report["passed"])
            self.assertEqual(len(report["negative_controls"]), 2)
