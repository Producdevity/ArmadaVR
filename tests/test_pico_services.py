import importlib.util
import copy
import hashlib
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch


spec = importlib.util.spec_from_file_location("pico_services", Path(__file__).parents[1] / "tools/pico-services.py")
pico = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pico)


class PicoServicesTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.source = Path(self.temp.name) / "source"
        self.root = Path(self.temp.name) / "root"
        self.root.mkdir()
        elf = bytearray(64)
        elf[:7] = b"\x7fELF\x02\x01\x01"
        elf[18:20] = b"\xb7\x00"
        for name in pico.RUNTIME + pico.LICENSES + tuple(pico.PROVENANCE):
            path = self.source / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(elf if name in pico.RUNTIME else b"license")
            path.chmod(0o755 if name in pico.RUNTIME else 0o644)
        for name, reference in pico.PROVENANCE.items():
            (self.source / name).write_bytes((pico.ROOT / reference).read_bytes())
        (self.source / pico.LIBRARY_LINK).symlink_to(pico.LIBRARY_TARGET)

    def tearDown(self):
        self.temp.cleanup()

    def test_install_and_verify_preserve_files_modes_and_links(self):
        result = pico.install(self.source, self.root)
        pico.verify(self.root, result)
        self.assertFalse(result["dsp_startup"])
        self.assertEqual(result["required_cmdline"], "reframed.pico=qrtr")
        self.assertIn("ConditionPathExists=!/etc/initrd-release", (self.root / "usr/lib/systemd/system/reframed-pico-qrtr.service").read_text())
        with self.assertRaisesRegex(ValueError, "already exists"):
            pico.install(self.source, self.root)

    def test_changed_runtime_and_enablement_are_rejected(self):
        result = pico.install(self.source, self.root)
        binary = self.root / pico.RUNTIME[0]
        binary.chmod(0o644)
        with self.assertRaisesRegex(ValueError, "Changed Pico root service file"):
            pico.verify(self.root, result)
        binary.chmod(0o755)
        link = self.root / pico.LIBRARY_LINK
        link.unlink()
        link.symlink_to("other.so")
        with self.assertRaisesRegex(ValueError, "Changed Pico root service link"):
            pico.verify(self.root, result)

    def test_wrong_architecture_and_source_identity_refused_before_writing(self):
        path = self.source / pico.RUNTIME[0]
        original = path.read_bytes()
        path.write_bytes(original[:18] + b"\x3e\x00" + original[20:])
        with self.assertRaisesRegex(ValueError, "ARM64"):
            pico.install(self.source, self.root)
        self.assertEqual(list(self.root.iterdir()), [])
        path.write_bytes(original)
        (self.source / next(iter(pico.PROVENANCE))).write_bytes(b"{}")
        with self.assertRaisesRegex(ValueError, "source identity"):
            pico.install(self.source, self.root)
        self.assertEqual(list(self.root.iterdir()), [])

    def test_foreign_initramfs_and_quest_units_are_refused(self):
        path = self.root / "etc/initrd-release"
        path.parent.mkdir()
        path.write_text("initramfs")
        with self.assertRaisesRegex(ValueError, "not an initramfs"):
            pico.install(self.source, self.root)
        path.unlink()
        path = self.root / "usr/lib/systemd/system/reframed-quest-boot.service"
        path.parent.mkdir(parents=True)
        path.write_text("quest")
        with self.assertRaisesRegex(ValueError, "Quest startup"):
            pico.install(self.source, self.root)

    def test_escape_symlink_refused_before_any_file_is_installed(self):
        (self.root / "usr").symlink_to(self.source / "usr")
        with self.assertRaisesRegex(ValueError, "Unsafe Pico runtime destination"):
            pico.install(self.source, self.root)
        self.assertFalse((self.source / "usr/libexec").exists())

    def test_image_readback_rejects_absolute_and_parent_paths(self):
        for index, name in enumerate(("/usr/bin/qrtr-ns", "../qrtr-ns", "usr/../../qrtr-ns")):
            manifest = {"files": [{"path": name}], "links": {}}
            with self.subTest(name=name), patch.object(pico.subprocess, "run") as run:
                with self.assertRaisesRegex(ValueError, "Invalid Pico runtime image path"):
                    pico.verify_image("unused.ext4", manifest, self.root / str(index))
                run.assert_not_called()

    def mapper_fixture(self):
        records, domains = [], []
        for name, (processor, subdomain, instance) in pico.MAP_DOMAINS.items():
            data = name.encode()
            records.append({"path": name, "sha256": hashlib.sha256(data).hexdigest(), "bytes": len(data)})
            domains.append({"file": name, "domain": {"soc": "msm", "domain": processor,
                                                     "subdomain": subdomain, "qmi_instance_id": instance}})
            path = self.root / "usr/lib/firmware" / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
        (self.source / pico.MAPPER).write_bytes((self.source / pico.RUNTIME[0]).read_bytes())
        (self.source / pico.MAPPER).chmod(0o755)
        for name, reference in pico.MAPPER_PROVENANCE.items():
            (self.source / name).write_bytes((pico.ROOT / reference).read_bytes())
        (self.source / pico.MAPPER_LICENSE).write_text("license")
        return {"component": "pico-neo3-firmware", "status": "prepared", "schema_version": 1,
                "files": records, "dsp": {"files": copy.deepcopy(records), "service_domains": domains}}

    def test_mapper_install_checks_maps_and_preserves_provenance(self):
        firmware = self.mapper_fixture()
        result = pico.install(self.source, self.root, firmware)
        self.assertTrue(result["pd_mapper"])
        self.assertFalse(result["dsp_startup"])
        self.assertEqual((self.root / pico.MAP_CHECKSUMS).read_bytes(), pico.service_maps(firmware))
        self.assertEqual(len((self.root / pico.MAP_CHECKSUMS).read_text().splitlines()), 5)
        self.assertIn("BindsTo=reframed-pico-qrtr.service",
                      (self.root / "usr/lib/systemd/system/reframed-pico-pd-mapper.service").read_text())
        pico.verify(self.root, result)

    def test_gpu_only_bundle_does_not_enable_mapper(self):
        result = pico.install(self.source, self.root, {"component": "pico-neo3-gpu"})
        self.assertFalse(result["pd_mapper"])
        self.assertFalse((self.root / pico.MAPPER).exists())
        self.assertFalse((self.root / pico.MAP_CHECKSUMS).exists())

    def test_changed_or_missing_map_refused_before_runtime_install(self):
        firmware = self.mapper_fixture()
        path = self.root / "usr/lib/firmware/adspr.jsn"
        path.write_text("changed")
        with self.assertRaisesRegex(ValueError, "Changed installed Pico service map"):
            pico.install(self.source, self.root, firmware)
        self.assertFalse((self.root / pico.RUNTIME[0]).exists())
        path.unlink()
        with self.assertRaisesRegex(ValueError, "regular Pico runtime file"):
            pico.install(self.source, self.root, firmware)

    def test_mapper_patch_identity_is_required(self):
        firmware = self.mapper_fixture()
        (self.source / next(iter(pico.MAPPER_PROVENANCE))).write_text("old patch")
        with self.assertRaisesRegex(ValueError, "source identity mismatch"):
            pico.install(self.source, self.root, firmware)
        self.assertFalse((self.root / pico.RUNTIME[0]).exists())

    def test_map_manifest_rejects_wrong_domains_and_inconsistent_records(self):
        firmware = self.mapper_fixture()
        for change in (lambda m: m["dsp"]["service_domains"].pop(),
                       lambda m: m["dsp"]["service_domains"][0]["domain"].update(qmi_instance_id=90),
                       lambda m: m["dsp"]["files"][0].update(sha256="0" * 64),
                       lambda m: m["dsp"]["files"].append(m["dsp"]["files"][0]),
                       lambda m: m["dsp"]["service_domains"].append(m["dsp"]["service_domains"][0])):
            altered = copy.deepcopy(firmware)
            change(altered)
            with self.assertRaisesRegex(ValueError, "Pico service map"):
                pico.service_maps(altered)
