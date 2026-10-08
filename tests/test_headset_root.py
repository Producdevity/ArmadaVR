import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


spec = importlib.util.spec_from_file_location("headset_root", Path(__file__).parents[1] / "tools/build-headset-root.py")
root = importlib.util.module_from_spec(spec)
spec.loader.exec_module(root)


class RootExportTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.directory = Path(self.temp.name)
        self.archive = self.directory / "rootfs.tar"
        self.archive.write_bytes(b"retained userspace export\n" * 200)
        self.image = "sha256:" + "a" * 64
        self.manifest = {"target": "headset-root-offline", "status": "built", "container_image": self.image,
                         "export_sha256": hashlib.sha256(self.archive.read_bytes()).hexdigest()}
        self.save()

    def tearDown(self):
        self.temp.cleanup()

    def save(self):
        (self.directory / "manifest.json").write_text(json.dumps(self.manifest))

    def test_detached_export_preserves_userspace_provenance(self):
        result = root.read_export(self.directory)
        self.assertEqual(result["container_image"], self.image)
        self.assertEqual(result["expanded_bytes"], self.archive.stat().st_size)
        self.assertEqual(result["sha256"], self.manifest["export_sha256"])
        self.assertEqual(result["archive_sha256"], result["sha256"])

    def test_reuse_still_requires_the_original_image(self):
        with self.assertRaisesRegex(ValueError, "same immutable"):
            root.read_export(self.directory, "sha256:" + "b" * 64)
        root.read_export(self.directory, self.image)

    def test_failed_or_unidentified_exports_are_refused(self):
        for key, value in (("status", "failed"), ("target", "other"), ("container_image", "runtime:latest"),
                           ("export_sha256", None)):
            with self.subTest(key=key):
                old = self.manifest[key]
                self.manifest[key] = value
                self.save()
                with self.assertRaisesRegex(ValueError, "successful root build"):
                    root.read_export(self.directory)
                self.manifest[key] = old

    def test_changed_export_is_refused(self):
        self.archive.write_bytes(b"changed")
        with self.assertRaisesRegex(ValueError, "checksum mismatch"):
            root.read_export(self.directory)

    def test_symlink_archive_and_manifest_are_refused(self):
        for name in ("rootfs.tar", "manifest.json"):
            path = self.directory / name
            real = self.directory / (name + ".real")
            path.rename(real)
            path.symlink_to(real.name)
            with self.assertRaisesRegex(ValueError, "regular file"):
                root.read_export(self.directory)
            path.unlink()
            real.rename(path)

    def test_expansion_limit_is_enforced(self):
        with self.assertRaisesRegex(ValueError, "expanded-size limit"):
            root.export_digest(self.archive, limit=10)

    @unittest.skipUnless(shutil.which("zstd"), "zstd is required to test compressed exports")
    def test_compressed_export_checks_the_original_uncompressed_hash(self):
        compressed = self.directory / "rootfs.tar.zst"
        subprocess.run(["zstd", "-q", str(self.archive), "-o", str(compressed)], check=True)
        size = self.archive.stat().st_size
        self.archive.unlink()
        result = root.read_export(self.directory)
        self.assertTrue(result["compressed"])
        self.assertEqual(result["sha256"], self.manifest["export_sha256"])
        self.assertEqual(result["expanded_bytes"], size)
        self.assertNotEqual(result["archive_sha256"], result["sha256"])
        self.assertEqual(sorted(p.name for p in self.directory.iterdir()), ["manifest.json", "rootfs.tar.zst"])
        with self.assertRaisesRegex(ValueError, "expanded-size limit"):
            root.export_digest(compressed, True, limit=10)
        compressed.write_bytes(compressed.read_bytes()[:-3])
        with self.assertRaisesRegex(ValueError, "Cannot decompress"):
            root.read_export(self.directory)

    def test_storage_includes_unpacked_scratch_without_duplicate_export(self):
        size = 7 * 1024**3
        budget = root.storage_required(8, size, False, 1024)
        self.assertEqual(budget["export_bytes"], 0)
        self.assertGreater(budget["scratch_bytes"], size)
        self.assertGreater(sum(budget.values()), 15 * 1024**3)
        self.assertEqual(root.storage_required(8, size, True)["export_bytes"], size)


class RuntimeRPMTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.directory = Path(self.temp.name)
        self.package = self.directory / "library.aarch64.rpm"
        self.package.write_bytes(b"fixture; actual signatures are checked in Linux")
        self.manifest = {"schema_version": 1, "packages": [{"path": self.package.name,
                         "sha256": hashlib.sha256(self.package.read_bytes()).hexdigest()}]}
        self.save()

    def tearDown(self):
        self.temp.cleanup()

    def save(self):
        (self.directory / "manifest.json").write_text(json.dumps(self.manifest))

    def test_pinned_package_inventory(self):
        result = root.read_rpms(self.directory)
        self.assertEqual(result["packages"], self.manifest["packages"])
        self.assertEqual(result["bytes"], self.package.stat().st_size)

    def test_changed_and_extra_packages_are_refused(self):
        extra = self.directory / "unlisted.rpm"
        extra.write_bytes(b"extra")
        with self.assertRaisesRegex(ValueError, "extra files"):
            root.read_rpms(self.directory)
        extra.unlink()
        self.package.write_bytes(b"changed")
        with self.assertRaisesRegex(ValueError, "checksum mismatch"):
            root.read_rpms(self.directory)

    def test_paths_duplicates_and_symlinks_are_refused(self):
        for name in ("../library.rpm", "/tmp/library.rpm", "has spaces.rpm", "library.rpm\n"):
            self.manifest["packages"][0]["path"] = name
            self.save()
            with self.assertRaisesRegex(ValueError, "Invalid or duplicate"):
                root.read_rpms(self.directory)
        self.manifest["packages"][0]["path"] = self.package.name
        self.manifest["packages"] *= 2
        self.save()
        with self.assertRaisesRegex(ValueError, "Invalid or duplicate"):
            root.read_rpms(self.directory)
        self.manifest["packages"].pop()
        self.save()
        self.package.unlink()
        self.package.symlink_to("manifest.json")
        with self.assertRaisesRegex(ValueError, "regular file"):
            root.read_rpms(self.directory)
