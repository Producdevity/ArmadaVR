import importlib.util
import hashlib
import io
import json
import os
from pathlib import Path
import tarfile
import tempfile
import unittest


spec = importlib.util.spec_from_file_location("lepton", Path(__file__).parents[1] / "tools/prepare-lepton.py")
lepton = importlib.util.module_from_spec(spec)
spec.loader.exec_module(lepton)
spec = importlib.util.spec_from_file_location("lepton_artifact", Path(__file__).parents[1] / "tools/lepton-artifact.py")
artifact = importlib.util.module_from_spec(spec)
spec.loader.exec_module(artifact)


class ArchiveTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.path = Path(self.temp.name) / "payload.tar"

    def tearDown(self):
        self.temp.cleanup()

    def archive(self, entries):
        with tarfile.open(self.path, "w") as archive:
            for name, kind, data in entries:
                item = tarfile.TarInfo(name)
                item.mode = 0o755
                item.type = kind
                item.pax_headers = {"mtime": "1700000000.125"}
                if kind == tarfile.REGTYPE:
                    item.size = len(data)
                    archive.addfile(item, io.BytesIO(data))
                else:
                    item.linkname = data
                    archive.addfile(item)

    def test_guest_absolute_link_remains_data(self):
        self.archive([("images", tarfile.DIRTYPE, ""), ("images/rootfs", tarfile.DIRTYPE, ""),
                      ("images/rootfs/init", tarfile.SYMTYPE, "/system/bin/init"),
                      ("license", tarfile.REGTYPE, b"upstream license")])
        result = lepton.inventory(self.path)
        self.assertEqual(result["payloads"]["images/rootfs/init"]["link"], "/system/bin/init")
        self.assertEqual(result["entries"]["license"]["pax"], {"mtime": "1700000000.125"})
        self.assertEqual(result["expanded_bytes"], len(b"upstream license"))
        self.assertEqual(list(Path(self.temp.name).iterdir()), [self.path])

    def test_cannot_extract_beneath_guest_link(self):
        self.archive([("images", tarfile.DIRTYPE, ""), ("images/rootfs", tarfile.DIRTYPE, ""),
                      ("images/rootfs/system", tarfile.SYMTYPE, "/tmp"),
                      ("images/rootfs/system/file", tarfile.REGTYPE, b"unexpected")])
        with self.assertRaisesRegex(ValueError, "parent"):
            lepton.inventory(self.path)

    def test_duplicate_traversal_device_and_hardlink_members_are_refused(self):
        cases = [[("file", tarfile.REGTYPE, b"a"), ("./file", tarfile.REGTYPE, b"b")],
                 [("../file", tarfile.REGTYPE, b"a")], [("/file", tarfile.REGTYPE, b"a")],
                 [("device", tarfile.CHRTYPE, "")], [("hardlink", tarfile.LNKTYPE, "file")],
                 [("escape", tarfile.SYMTYPE, "/tmp")]]
        for entries in cases:
            with self.subTest(entries=entries):
                self.archive(entries)
                with self.assertRaises(ValueError):
                    lepton.inventory(self.path)

    def test_missing_parent_and_corrupt_tar_are_refused(self):
        self.archive([("missing/file", tarfile.REGTYPE, b"a")])
        with self.assertRaisesRegex(ValueError, "parent"):
            lepton.inventory(self.path)
        self.path.write_bytes(b"not a tar archive")
        with self.assertRaises(tarfile.TarError):
            lepton.inventory(self.path)

    def test_hash_validation_never_follows_a_link(self):
        target = Path(self.temp.name) / "original"
        target.write_bytes(b"retained input")
        self.path.symlink_to(target)
        with self.assertRaisesRegex(ValueError, "regular file"):
            lepton.digest(self.path)


class HostBundleTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.record = {"schema_version": 1, "files": []}
        for name, mode in artifact.HOST_FILES.items():
            path = self.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            data = (b"\x7fELF\x02\x01" + b"\0" * 10 + b"\x03\x00\xb7\x00" if name.endswith(".so") else
                    json.dumps({"layer": {"name": "VK_LAYER_fossilize", "library_path": "/usr/lib/libVkLayer_fossilize.so"}}).encode())
            path.write_bytes(data)
            path.chmod(mode)
            self.record["files"].append({"path": name, "bytes": len(data), "mode": mode,
                                         "sha256": hashlib.sha256(data).hexdigest()})
        self.save()

    def tearDown(self):
        self.temp.cleanup()

    def save(self):
        (self.root / "manifest.json").write_text(json.dumps(self.record))

    def test_complete_bundle_and_payload_tampering(self):
        self.assertEqual(artifact.validate_host(self.root)["files"], self.record["files"])
        (self.root / self.record["files"][0]["path"]).write_bytes(b"changed")
        with self.assertRaisesRegex(ValueError, "identity"):
            artifact.validate_host(self.root)

    def test_missing_duplicate_or_external_path_is_refused(self):
        original = list(self.record["files"])
        for files in (original[:-1], [original[0], original[0], original[2]],
                      [{**original[0], "path": "../outside"}, *original[1:]]):
            self.record["files"] = files
            self.save()
            with self.assertRaisesRegex(ValueError, "complete"):
                artifact.validate_host(self.root)

    def test_symlink_and_extra_file_are_refused(self):
        extra = self.root / "extra"
        extra.touch()
        with self.assertRaisesRegex(ValueError, "inventory"):
            artifact.validate_host(self.root)
        extra.unlink()
        path = self.root / self.record["files"][0]["path"]
        path.unlink()
        path.symlink_to("/tmp")
        with self.assertRaisesRegex(ValueError, "symlink"):
            artifact.validate_host(self.root)

    def test_wrong_architecture_fails_even_with_updated_hash(self):
        item = self.record["files"][0]
        path = self.root / item["path"]
        data = path.read_bytes()[:-2] + b"\x3e\x00"
        path.write_bytes(data)
        item["sha256"] = hashlib.sha256(data).hexdigest()
        self.save()
        with self.assertRaisesRegex(ValueError, "ARM64"):
            artifact.validate_host(self.root)


class InstallationTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.prefix = self.root / artifact.PREFIX
        self.prefix.mkdir(parents=True)
        self.prefix.chmod(0o755)
        self.file = self.prefix / "package.apk"
        self.file.write_bytes(b"APK timestamp fixture")
        self.file.chmod(0o644)
        os.utime(self.file, (1700000000, 1700000000))
        self.metadata = self.root / artifact.BUILD_RECORD
        self.metadata.parent.mkdir(parents=True)
        self.metadata.write_text(json.dumps({"fixture": True}))
        self.record = {"manifest_sha256": artifact.digest(self.metadata), "inventory": {"entries": {
            ".": {"type": "directory", "mode": 0o755},
            "package.apk": {"type": "file", "mode": 0o644, "bytes": self.file.stat().st_size,
                            "sha256": artifact.digest(self.file), "mtime": 1700000000}}}}

    def tearDown(self):
        self.temp.cleanup()

    def test_complete_installation_preserves_package_timestamp(self):
        artifact.verify_install(self.root, self.record)
        os.utime(self.file, (1700000001, 1700000001))
        with self.assertRaisesRegex(ValueError, "timestamp"):
            artifact.verify_install(self.root, self.record)

    def test_unexpected_files_and_modified_payload_are_refused(self):
        extra = self.prefix / "unexpected"
        extra.touch()
        with self.assertRaisesRegex(ValueError, "inventory"):
            artifact.verify_install(self.root, self.record)
        extra.unlink()
        self.file.write_bytes(b"replaced")
        with self.assertRaisesRegex(ValueError, "checksum"):
            artifact.verify_install(self.root, self.record)

    def test_installation_cannot_traverse_a_prefix_link(self):
        moved = self.prefix.with_name("original")
        self.prefix.rename(moved)
        self.prefix.symlink_to(moved)
        with self.assertRaisesRegex(ValueError, "symlink"):
            artifact.verify_install(self.root, self.record)

    def test_changed_build_record_is_refused(self):
        self.metadata.write_text("changed")
        with self.assertRaisesRegex(ValueError, "build record"):
            artifact.verify_install(self.root, self.record)


if __name__ == "__main__":
    unittest.main()
