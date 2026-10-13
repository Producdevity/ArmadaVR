import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

import test_gpt

spec = importlib.util.spec_from_file_location("pico_backup", Path(__file__).parents[1] / "tools/verify-pico-backup.py")
backup = importlib.util.module_from_spec(spec)
spec.loader.exec_module(backup)


class PicoBackupTests(unittest.TestCase):
    def fixture(self, directory, sector_size=4096):
        records = []
        for lun in range(6):
            def names(data, *_):
                if lun:
                    for index in range(2):
                        begin = index * 128 + 56
                        data[begin:begin + 72] = f"part{lun}_{index}".encode("utf-16le").ljust(72, b"\0")
            args = test_gpt.GPTTests().fixture(sector_size, modify_entry=names)
            report = backup.gpt.inspect(*args)
            report["lun"] = lun
            (directory / f"lun{lun}-gpt.json").write_text(json.dumps(report))
            for name, data in zip(backup.GPT_PARTS, args[:4]):
                (directory / f"lun{lun}-{name}.bin").write_bytes(data)
            if lun == 0:
                for item in report["partitions"]:
                    data = bytes([item["index"]]) * item["bytes"]
                    filename = item["name"] + ".bin"
                    (directory / filename).write_bytes(data)
                    records.append({"lun": lun, **item, "file": filename,
                                    "sha256": hashlib.sha256(data).hexdigest(),
                                    "device_read_passes": 2, "host_digest_verified": True})
        manifest = {"status": "passed", "partitions": records,
                    "total_bytes": sum(r["bytes"] for r in records)}
        return self.manifest(directory, manifest)

    def manifest(self, directory, data):
        path = directory / "backup-manifest.json"
        path.write_text(json.dumps(data))
        return hashlib.sha256(path.read_bytes()).hexdigest()

    def test_geometry_offsets_coverage_and_no_physical_claim(self):
        for size in (512, 4096):
            with tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                digest = self.fixture(root, size)
                result = backup.audit(root, digest, ["boot", "persist"])
                self.assertEqual(result["status"], "verified")
                self.assertEqual(result["partition_count"], 12)
                self.assertEqual(result["backup_count"], 2)
                self.assertEqual(len(result["not_backed_up"]), 10)
                boot = result["covered_partitions"][0]
                self.assertEqual(boot["byte_offset"], boot["first_lba"] * size)
                for key in ("device_accessed", "restore_verified", "flash_ready", "cold_recovery_verified", "live_device_identity_verified"):
                    self.assertIs(result[key], False)
                result = backup.audit(root, digest, ["boot", "part1_0"])
                self.assertEqual(result["status"], "incomplete")
                self.assertEqual(result["missing_backups"], ["part1_0"])
                with self.assertRaisesRegex(ValueError, "absent"):
                    backup.audit(root, digest, ["missing"])

    def test_corruption_truncation_and_input_identity(self):
        for target in ("boot.bin", "persist.bin", "lun0-primary-header.bin", "lun0-backup-entries.bin", "backup-manifest.json"):
            with self.subTest(target=target), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                digest = self.fixture(root)
                path = root / target
                path.write_bytes(path.read_bytes()[:-1])
                with self.assertRaises((ValueError, KeyError)):
                    backup.audit(root, digest, ["boot"])

    def test_wrong_lun_extent_guid_filename_and_read_proof(self):
        changes = [("lun", 1), ("first_lba", 1), ("partition_guid", "wrong"),
                   ("bytes", 512), ("file", "../boot.bin"), ("device_read_passes", 1),
                   ("device_read_passes", True), ("host_digest_verified", False)]
        for key, value in changes:
            with self.subTest(key=key), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                self.fixture(root)
                manifest = json.loads((root / "backup-manifest.json").read_text())
                manifest["partitions"][0][key] = value
                digest = self.manifest(root, manifest)
                with self.assertRaises(ValueError):
                    backup.audit(root, digest, ["boot"])

    def test_duplicate_records_and_json_keys(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            self.fixture(root)
            manifest = json.loads((root / "backup-manifest.json").read_text())
            manifest["partitions"].append(manifest["partitions"][0])
            digest = self.manifest(root, manifest)
            with self.assertRaisesRegex(ValueError, "Repeated"):
                backup.audit(root, digest, ["boot"])
        with self.assertRaisesRegex(ValueError, "Duplicate"):
            json.loads('{"bytes": 1, "bytes": 2}', object_pairs_hook=backup.unique_object)

    def test_changed_geometry_and_ambiguous_lun_names(self):
        for change in ("total_sectors", "duplicate"):
            with tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                digest = self.fixture(root)
                path = root / "lun1-gpt.json"
                data = json.loads(path.read_text())
                if change == "total_sectors":
                    data["total_sectors"] += 1
                else:
                    data = json.loads((root / "lun0-gpt.json").read_text())
                    for name in backup.GPT_PARTS:
                        (root / f"lun1-{name}.bin").write_bytes((root / f"lun0-{name}.bin").read_bytes())
                path.write_text(json.dumps(data))
                with self.assertRaises(ValueError):
                    backup.audit(root, digest, ["boot"])

    def test_symlink_special_file_and_duplicate_selection(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            digest = self.fixture(root)
            for required in ([], ["boot", "boot"], ["../boot"]):
                with self.assertRaises(ValueError):
                    backup.audit(root, digest, required)
            original = root / "boot.bin"
            original.rename(root / "original.bin")
            original.symlink_to("original.bin")
            with self.assertRaises(OSError):
                backup.audit(root, digest, ["boot"])
            with self.assertRaises(ValueError):
                backup.regular(Path("/dev/null"), 4096)
