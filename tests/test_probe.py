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
            self.assertEqual(report["storage_geometry"]["devices"], [])

    def test_sysfs_capacity_uses_512_units_for_both_logical_block_sizes(self):
        for block, sectors in ((512, 1000), (4096, 125)):
            with self.subTest(block=block):
                reads = {"size_512_units": {"returncode": 0, "stdout": "1000"},
                         "logical_block_size": {"returncode": 0, "stdout": str(block)}}
                result = probe.storage_dimensions(reads)
                self.assertEqual(result["capacity_bytes"], 512000)
                self.assertEqual(result["logical_sectors"], sectors)
                self.assertTrue(result["gpt_inspector_supported"])

    def test_denied_invalid_and_overflowing_geometry_is_not_inferred(self):
        for size, block in (("0", "4096"), (str(2**64), "4096"), ("1000", "513"),
                            ("1000", "Permission denied"), ("1001", "4096")):
            with self.subTest(size=size, block=block):
                reads = {"size_512_units": {"returncode": 0, "stdout": size},
                         "logical_block_size": {"returncode": 0, "stdout": block}}
                result = probe.storage_dimensions(reads)
                self.assertIsNone(result["logical_sectors"])
                self.assertFalse(result["gpt_inspector_supported"])
        reads = {"size_512_units": {"returncode": 1, "stdout": "1000"},
                 "logical_block_size": {"returncode": 0, "stdout": "4096"}}
        self.assertIsNone(probe.storage_dimensions(reads)["capacity_bytes"])

    def test_storage_listing_filters_partitions_and_shell_metacharacters(self):
        calls = []
        def invoke(adb, serial, args):
            calls.append(args)
            output = "sda sda1 dm-0 loop0 sdf sdf1 sda;touch sda sda/../x" if len(calls) == 1 else "4096"
            return {"returncode": 0, "stdout": output, "stderr": ""}
        with patch.object(probe, "invoke", side_effect=invoke):
            result = probe.storage_geometry("adb", "chosen")
        self.assertEqual([device["name"] for device in result["devices"]], ["sda", "sdf"])
        self.assertEqual(len(calls), 7)
        self.assertFalse(result["partition_data_read"])
        self.assertFalse(result["firehose_lun_mapping_verified"])
        self.assertTrue(all("/sda/" in args[1] or "/sdf/" in args[1] for args in calls[1:]))

    def test_other_valid_block_size_is_not_supported_by_gpt_inspector(self):
        reads = {"size_512_units": {"returncode": 0, "stdout": "1000"},
                 "logical_block_size": {"returncode": 0, "stdout": "2048"}}
        result = probe.storage_dimensions(reads)
        self.assertEqual(result["logical_sectors"], 250)
        self.assertFalse(result["gpt_inspector_supported"])


if __name__ == "__main__":
    unittest.main()
