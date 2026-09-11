import importlib.util
import subprocess
import unittest
from pathlib import Path
from unittest.mock import patch

spec = importlib.util.spec_from_file_location("probe", Path(__file__).parents[1] / "tools/probe-device.py")
probe = importlib.util.module_from_spec(spec)
spec.loader.exec_module(probe)


class ProbeTests(unittest.TestCase):
    def test_missing_or_failed_properties_remain_unknown(self):
        failed = {"returncode": 1, "stdout": "0"}
        result = probe.observations({"ro.boot.flash.locked": failed}, {})
        self.assertEqual(result["bootloader_reported_state"], "unknown")
        self.assertIsNone(result["adb_shell_uid"])
        self.assertIsNone(result["firmware_incremental"])

    def test_conflicting_bootloader_properties_are_not_unlock_evidence(self):
        result = probe.observations({
            "ro.boot.flash.locked": {"returncode": 0, "stdout": "1"},
            "ro.boot.vbmeta.device_state": {"returncode": 0, "stdout": "unlocked"},
        }, {})
        self.assertEqual(result["bootloader_reported_state"], "conflicting")

    def test_su_presence_is_separate_from_shell_privileges(self):
        queries = {"shell_identity": {"returncode": 0, "stdout": "uid=2000(shell) gid=2000(shell)"},
                   "root_binary": {"returncode": 0, "stdout": "/system/bin/su"}}
        result = probe.observations({}, queries)
        self.assertEqual(result["adb_shell_uid"], 2000)
        self.assertEqual(result["su_path"], "/system/bin/su")
        queries["shell_identity"]["stdout"] = "uid=0(root) gid=0(root)"
        self.assertEqual(probe.observations({}, queries)["adb_shell_uid"], 0)

    def test_unauthorized_device_is_not_probed(self):
        with patch.object(probe, "invoke", return_value={"returncode": 1, "stdout": "unauthorized"}) as invoke:
            with self.assertRaises(RuntimeError):
                probe.collect("adb", "chosen")
            invoke.assert_called_once_with("adb", "chosen", ["get-state"])

    def test_unknown_device_is_not_probed_beyond_properties(self):
        def invoke(adb, serial, args):
            self.assertTrue(args == ["get-state"] or args[:2] == ["shell", "getprop"])
            return {"returncode": 0, "stdout": "device" if args == ["get-state"] else "phone"}
        with patch.object(probe, "invoke", side_effect=invoke):
            with self.assertRaises(RuntimeError):
                probe.collect("adb", "phone")

    def test_explicit_serial_is_an_argument_not_shell_code(self):
        with patch.object(subprocess, "run", return_value=subprocess.CompletedProcess([], 0, "ok", "")) as run:
            probe.invoke("adb", "serial; touch /tmp/never", ["shell", "uname -a"])
            self.assertEqual(run.call_args.args[0], ["adb", "-s", "serial; touch /tmp/never", "shell", "uname -a"])
            self.assertNotIn("shell", run.call_args.kwargs)

    def test_missing_query_is_recorded_not_upgraded_to_evidence(self):
        def invoke(adb, serial, args):
            if args == ["get-state"]:
                return {"returncode": 0, "stdout": "device"}
            if args[:2] == ["shell", "getprop"]:
                return {"returncode": 0, "stdout": "Quest 3"}
            return {"returncode": 1, "stdout": "", "stderr": "Permission denied"}
        with patch.object(probe, "invoke", side_effect=invoke):
            report = probe.collect("adb", "chosen")
            self.assertFalse(report["interpretation"]["bootloader_unlock_proven"])
            self.assertEqual(report["queries"]["graphics"]["returncode"], 1)


if __name__ == "__main__":
    unittest.main()
