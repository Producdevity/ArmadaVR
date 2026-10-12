import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from unittest.mock import patch


spec = importlib.util.spec_from_file_location("headset_root", Path(__file__).parents[1] / "tools/build-headset-root.py")
root = importlib.util.module_from_spec(spec)
spec.loader.exec_module(root)


class RootTargetTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.directory = Path(self.temp.name)
        self.kernel = self.directory / "kernel"
        self.kernel.mkdir()
        (self.kernel / "build.json").write_text("{}")
        self.initramfs = self.directory / "initramfs"
        self.initramfs.mkdir()
        (self.initramfs / "initramfs.img").write_bytes(b"initramfs")
        self.fw = {"component": "pico-neo3-gpu"}
        self.initrd = {"status": "built", "quest_services": False, "firmware": self.fw,
                       "kernel_build_sha256": root.builder.digest(self.kernel / "build.json"),
                       "qemu_transports": False, "systemd_runtime_sha256": "a" * 64,
                       "artifacts": [{"path": "initramfs.img", "sha256": root.builder.digest(self.initramfs / "initramfs.img")}]}
        self.save()

    def tearDown(self):
        self.temp.cleanup()

    def save(self):
        (self.initramfs / "build.json").write_text(json.dumps(self.initrd))

    def select(self, runtime=True, scope="root", gpu=None, profile="pico-neo3"):
        with patch.object(root.builder, "inputs", return_value=({"qemu_transports": False}, {}, "4.19", [])), \
                patch.object(root.builder, "firmware_input", return_value=(self.directory, self.fw, root.builder.pico_firmware)) as firmware, \
                patch.object(root.builder.systemd_runtime, "validate", return_value={"scope": scope}):
            result = root.target_inputs(profile, self.kernel, self.directory, self.initramfs,
                                        self.directory if runtime else None, gpu)
            return result, firmware.call_args.args

    def test_pico_uses_its_firmware_without_quest_startup(self):
        result, call = self.select()
        self.assertEqual(call[1:], (None, self.directory, False))
        self.assertEqual(result[6], {"scope": "root"})
        self.assertEqual(result[7], {})

    def test_inconsistent_kernel_initramfs_and_firmware_are_refused(self):
        for name, value in (("quest_services", True), ("firmware", {}), ("kernel_build_sha256", "b" * 64),
                            ("qemu_transports", True), ("status", "failed")):
            with self.subTest(name=name):
                old = self.initrd[name]
                self.initrd[name] = value
                self.save()
                with self.assertRaisesRegex(ValueError, "matching headset"):
                    self.select()
                self.initrd[name] = old
        self.save()
        (self.initramfs / "initramfs.img").write_bytes(b"modified")
        with self.assertRaisesRegex(ValueError, "Initramfs checksum"):
            self.select()

    def test_pico_requires_both_compatible_runtime_stages(self):
        with self.assertRaisesRegex(ValueError, "both root and initramfs"):
            self.select(runtime=False)
        with self.assertRaisesRegex(ValueError, "root-scope"):
            self.select(scope="initramfs")
        self.initrd.pop("systemd_runtime_sha256")
        self.save()
        with self.assertRaisesRegex(ValueError, "both root and initramfs"):
            self.select()

    def test_separate_quest_gpu_bundle_is_refused_for_pico(self):
        with self.assertRaisesRegex(ValueError, "without --gpu-firmware"):
            self.select(gpu=self.directory)

    def test_pico_startup_requires_matching_initramfs_sources(self):
        self.initrd["pico_services"] = True
        self.initrd["pico_inputs_sha256"] = {n: root.builder.digest(root.ROOT / n) for n in root.builder.PICO_INPUTS}
        self.save()
        result, _ = self.select()
        self.assertEqual(result[7], self.initrd["pico_inputs_sha256"])
        self.initrd["pico_inputs_sha256"].clear()
        self.save()
        with self.assertRaisesRegex(ValueError, "Pico startup sources changed"):
            self.select()

    def test_pico_startup_cannot_be_selected_for_quest(self):
        self.initrd["pico_services"] = True
        self.save()
        with self.assertRaisesRegex(ValueError, "Invalid Pico startup"):
            self.select(profile="quest3")
        self.initrd["pico_services"] = 1
        self.save()
        with self.assertRaisesRegex(ValueError, "Invalid Pico startup"):
            self.select()

    def test_quest_contract_remains_required(self):
        self.initrd["quest_services"] = True
        self.initrd["quest_inputs_sha256"] = {n: root.builder.digest(root.ROOT / n) for n in root.builder.QUEST_INPUTS}
        self.save()
        result, call = self.select(profile="quest3", runtime=False)
        self.assertEqual(call[1:], (self.directory, None, True))
        self.assertEqual(result[7], self.initrd["quest_inputs_sha256"])
        self.initrd["quest_inputs_sha256"].clear()
        self.save()
        with self.assertRaisesRegex(ValueError, "Quest startup sources changed"):
            self.select(profile="quest3", runtime=False)


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

    def test_distribution_snapshot_version_filename(self):
        name = "passt-0^20261002.gcba3570-1.fc44.aarch64.rpm"
        self.package.rename(self.directory / name)
        self.manifest["packages"][0]["path"] = name
        self.save()
        self.assertEqual(root.read_rpms(self.directory)["packages"], self.manifest["packages"])

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
