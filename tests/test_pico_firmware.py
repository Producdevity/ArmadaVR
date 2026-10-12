import importlib.util
import json
from pathlib import Path
import shutil
import struct
import tempfile
import unittest
from unittest.mock import patch

from test_quest_firmware import mdt


ROOT = Path(__file__).parents[1]
spec = importlib.util.spec_from_file_location("pico_firmware", ROOT / "tools/prepare-pico-firmware.py")
firmware = importlib.util.module_from_spec(spec)
spec.loader.exec_module(firmware)
spec = importlib.util.spec_from_file_location("initramfs", ROOT / "tools/build-headset-initramfs.py")
initramfs = importlib.util.module_from_spec(spec)
spec.loader.exec_module(initramfs)


class PicoFirmwareTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.files = self.root / "files"
        self.files.mkdir()
        (self.files / "a650_zap.mdt").write_bytes(mdt())
        (self.files / "a650_zap.b01").write_bytes(bytes(16))
        (self.files / "a650_zap.b02").write_bytes(b"data")
        (self.files / "a650_sqe.fw").write_bytes(struct.pack("<4I", 0, 0x016DD109, 1, 2))
        self.gmu = struct.pack("<4I", 0x80000000, 0, 1, 4) + struct.pack("<4I", 0x80000000, 4, 0, 0) + b"data"
        (self.files / "a650_gmu.bin").write_bytes(self.gmu)
        records, details = firmware.inspect_files(self.files)
        self.manifest = {"schema_version": 1, "component": "pico-neo3-gpu", "status": "prepared",
                         "reference_build": "smartcm.1696861506", "source_image_sha256": "a" * 64,
                         "files": records, "gpu": details}
        self.save()

    def tearDown(self):
        self.temp.cleanup()

    def save(self):
        (self.root / "manifest.json").write_text(json.dumps(self.manifest))

    def test_complete_bundle_and_a650_address_map(self):
        self.assertEqual(firmware.validate(self.root), self.manifest)
        self.assertEqual(self.manifest["gpu"]["sqe_version"], 0x016DD109)
        with self.assertRaisesRegex(ValueError, "virtual address"):
            firmware.qcom.gmu_blocks(self.gmu)

    def test_truncated_or_changed_firmware_is_rejected(self):
        for name in ("a650_zap.mdt", "a650_zap.b01", "a650_zap.b02", "a650_sqe.fw", "a650_gmu.bin"):
            path = self.files / name
            original = path.read_bytes()
            with self.subTest(name=name):
                path.write_bytes(original[:-1])
                with self.assertRaises(ValueError):
                    firmware.validate(self.root)
                path.write_bytes(original)
        (self.files / "a650_zap.b02").write_bytes(b"nope")
        with self.assertRaisesRegex(ValueError, "does not match"):
            firmware.validate(self.root)

    def test_gmu_boundaries_missing_allocation_and_metadata_order(self):
        for data in (self.gmu[16:], self.gmu + struct.pack("<4I", 0, 0, 1, 4),
                     struct.pack("<4I", 0xbffffffc, 0, 1, 8) + self.gmu[16:],
                     struct.pack("<4I", 0x80000002, 0, 1, 4) + self.gmu[16:],
                     struct.pack("<4I", 0xc0000000, 0, 1, 4) + self.gmu[16:]):
            with self.subTest(data=data.hex()), self.assertRaises(ValueError):
                firmware.qcom.gmu_blocks(data, firmware.GMU_RANGES)

    def test_unexpected_files_and_symlinks_are_rejected(self):
        extra = self.files / "a740v3_sqe.fw"
        extra.write_bytes(b"wrong target")
        with self.assertRaisesRegex(ValueError, "unexpected files"):
            firmware.validate(self.root)
        extra.unlink()
        path = self.files / "a650_zap.b02"
        path.rename(self.root / "outside")
        path.symlink_to(self.root / "outside")
        with self.assertRaisesRegex(ValueError, "regular file"):
            firmware.validate(self.root)

    def test_wrong_manifest_identity_is_rejected(self):
        for name, value in (("component", "quest3-gpu"), ("reference_build", "52433670036000520"),
                            ("source_image_sha256", "bad"), ("status", "flash-ready")):
            old = self.manifest[name]
            self.manifest[name] = value
            self.save()
            with self.subTest(name=name), self.assertRaisesRegex(ValueError, "Invalid Pico"):
                firmware.validate(self.root)
            self.manifest[name] = old

    def test_builder_rejects_cross_target_and_quest_services_before_extraction(self):
        pico = json.loads((ROOT / "profiles/kernel/pico-neo3.json").read_text())
        quest = json.loads((ROOT / "profiles/kernel/quest3.json").read_text())
        self.assertEqual(initramfs.firmware_input(pico, None, self.root, False)[1], self.manifest)
        for kernel, quest_dir, pico_dir, services in (
                (quest, None, self.root, False), (pico, None, self.root, True),
                (pico, self.root, None, False), (pico, self.root, self.root, False),
                ({**pico, "commit": "unreviewed"}, None, self.root, False)):
            with self.subTest(kernel=kernel["repository"], services=services), self.assertRaises(ValueError):
                initramfs.firmware_input(kernel, quest_dir, pico_dir, services)
        with patch.object(initramfs.firmware, "validate", return_value={"component": "quest3-adsp"}):
            self.assertEqual(initramfs.firmware_input(quest, self.root, None, True)[1]["component"], "quest3-adsp")
        self.assertIsNone(initramfs.firmware_input(pico, None, None, False)[1])

    def dsp_bundle(self):
        directory = self.root / "dsp"
        files = directory / "files"
        files.mkdir(parents=True)
        for processor in ("adsp", "cdsp", "slpi"):
            (files / (processor + ".mdt")).write_bytes(mdt() + bytes(16))
            (files / (processor + ".b01")).write_bytes(bytes(16))
            (files / (processor + ".b02")).write_bytes(b"data")
        for name, (domain, subdomain, instance) in firmware.dsp.DOMAIN_FILES.items():
            (files / name).write_text(json.dumps({"sr_domain": {"soc": "msm", "domain": domain,
                "subdomain": subdomain, "qmi_instance_id": instance},
                "sr_service": [{"provider": "tms", "service": "servreg"}]}))
        records, domains = firmware.dsp.inspect_files(files)
        manifest = {"schema_version": 1, "status": "prepared", "component": "pico-neo3-dsp",
                    "reference_build": self.manifest["reference_build"], "source_image_sha256": "b" * 64,
                    "files": records, "service_domains": domains}
        (directory / "manifest.json").write_text(json.dumps(manifest))
        return directory, manifest

    def test_combined_bundle_preserves_sources_and_checks_both_components(self):
        directory, manifest = self.dsp_bundle()
        firmware.include_dsp(self.root, directory)
        result = firmware.validate(self.root)
        self.assertEqual(result["component"], "pico-neo3-firmware")
        self.assertEqual(result["dsp"], manifest)
        self.assertEqual(result["source_image_sha256"], self.manifest["source_image_sha256"])
        self.assertEqual(len(result["files"]), len(self.manifest["files"]) + len(manifest["files"]))
        pico = json.loads((ROOT / "profiles/kernel/pico-neo3.json").read_text())
        self.assertEqual(initramfs.firmware_input(pico, None, self.root, False)[1], result)
        for name in ("a650_zap.b02", "adsp.b02", "cdsp.b02", "slpi.b02"):
            path = self.files / name
            old = path.read_bytes()
            path.write_bytes(b"evil")
            with self.subTest(name=name), self.assertRaises(ValueError):
                firmware.validate(self.root)
            path.write_bytes(old)
        shutil.rmtree(directory)
        self.assertEqual(firmware.validate(self.root), result)

    def test_mixed_reference_builds_rejected_before_copy(self):
        directory, manifest = self.dsp_bundle()
        manifest["reference_build"] = "smartcm.1696861507"
        (directory / "manifest.json").write_text(json.dumps(manifest))
        before = {p.name: p.read_bytes() for p in self.files.iterdir()}
        with self.assertRaisesRegex(ValueError, "same reference build"):
            firmware.include_dsp(self.root, directory)
        self.assertEqual(before, {p.name: p.read_bytes() for p in self.files.iterdir()})
        self.assertEqual(firmware.validate(self.root), self.manifest)

    def test_combined_manifest_cannot_hide_or_misidentify_dsp_files(self):
        directory, _ = self.dsp_bundle()
        firmware.include_dsp(self.root, directory)
        result = firmware.validate(self.root)
        for replacement in ({**result, "component": "pico-neo3-gpu"},
                            {**result, "dsp": {**result["dsp"], "reference_build": "smartcm.1696861507"}},
                            {**result, "dsp": {**result["dsp"], "files": []}}):
            (self.root / "manifest.json").write_text(json.dumps(replacement))
            with self.assertRaises(ValueError):
                firmware.validate(self.root)
