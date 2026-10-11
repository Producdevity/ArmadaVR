import importlib.util
import json
from pathlib import Path
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
