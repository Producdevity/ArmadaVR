import importlib.util
import json
from pathlib import Path
import struct
import tempfile
import unittest


spec = importlib.util.spec_from_file_location("firmware", Path(__file__).parents[1] / "tools/prepare-quest-firmware.py")
firmware = importlib.util.module_from_spec(spec)
spec.loader.exec_module(firmware)


def mdt():
    header = struct.pack("<16sHHIIIIIHHHHHH", b"\x7fELF\x01\x01\x01" + bytes(9),
                         2, 164, 1, 0, 52, 0, 0, 52, 32, 3, 0, 0, 0)
    segments = [(0, 0, 0, 0, 148, 148, 0, 0),
                (0, 4096, 0, 0, 16, 16, 2 << 24, 0),
                (1, 8192, 4096, 4096, 4, 8, 0, 4096)]
    return bytearray(header + b"".join(struct.pack("<8I", *s) for s in segments))


class MdtTests(unittest.TestCase):
    def test_selects_load_data_and_signing_metadata_without_header_blob(self):
        self.assertEqual(firmware.required_segments(mdt()), {"adsp.b01": 16, "adsp.b02": 4})

    def test_truncated_and_wrong_architecture_headers_are_rejected(self):
        for data in (mdt()[:40], mdt()[:-1], bytearray(mdt())):
            if len(data) == 148:
                struct.pack_into("<H", data, 18, 183)
            with self.subTest(length=len(data)), self.assertRaises(ValueError):
                firmware.required_segments(data)

    def test_mismatched_header_offset_and_metadata_extent_are_rejected(self):
        for offset, value in ((28, 56), (52 + 16, 149)):
            data = mdt()
            struct.pack_into("<I", data, offset, value)
            with self.subTest(offset=offset), self.assertRaises(ValueError):
                firmware.required_segments(data)

    def test_missing_signing_metadata_is_rejected(self):
        data = mdt()
        struct.pack_into("<I", data, 52 + 32 + 24, 0)
        with self.assertRaisesRegex(ValueError, "signing-metadata"):
            firmware.required_segments(data)

    def test_loadable_files_cannot_exceed_memory_extent(self):
        data = mdt()
        struct.pack_into("<I", data, 52 + 64 + 16, 9)
        with self.assertRaisesRegex(ValueError, "bounds"):
            firmware.required_segments(data)


class FirmwareBundleTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.files = self.root / "files"
        self.files.mkdir()
        (self.files / "adsp.mdt").write_bytes(mdt())
        (self.files / "adsp.b01").write_bytes(bytes(16))
        (self.files / "adsp.b02").write_bytes(b"data")
        domains = ("root_pd", "sensor_pd", "audio_pd", "charger_pd")
        for name, domain in zip(firmware.SERVICE_FILES, domains):
            value = {"sr_domain": {"soc": "msm", "domain": "adsp", "subdomain": domain, "qmi_instance_id": 74},
                     "sr_service": [{"provider": "tms", "service": "servreg"}]}
            (self.files / name).write_text(json.dumps(value))
        records, services = firmware.inspect_files(self.files)
        self.manifest = {"schema_version": 1, "component": "quest3-adsp", "status": "prepared",
                         "reference_build": "52433670036000520", "source_image_sha256": "a" * 64,
                         "files": records, "service_domains": services}
        self.save()

    def save(self):
        (self.root / "manifest.json").write_text(json.dumps(self.manifest))

    def tearDown(self):
        self.temp.cleanup()

    def test_bundle_preserves_source_identity_and_required_files(self):
        actual = firmware.validate(self.root)
        self.assertEqual(actual, self.manifest)
        self.assertEqual(len(actual["files"]), 7)

    def test_modified_same_length_payload_is_rejected(self):
        (self.files / "adsp.b02").write_bytes(b"nope")
        with self.assertRaisesRegex(ValueError, "does not match"):
            firmware.validate(self.root)

    def test_truncated_payload_and_missing_metadata_are_rejected(self):
        (self.files / "adsp.b02").write_bytes(b"x")
        with self.assertRaisesRegex(ValueError, "Truncated"):
            firmware.validate(self.root)
        (self.files / "adsp.b01").unlink()
        with self.assertRaisesRegex(ValueError, "regular file"):
            firmware.validate(self.root)

    def test_missing_charger_service_declaration_is_rejected(self):
        path = self.files / "battmgr.jsn"
        value = json.loads(path.read_text())
        value["sr_domain"]["subdomain"] = "unknown_pd"
        path.write_text(json.dumps(value))
        with self.assertRaisesRegex(ValueError, "charger_pd"):
            firmware.validate(self.root)

    def test_extra_blob_and_symlink_cannot_enter_bundle(self):
        extra = self.files / "adsp.b99"
        extra.write_bytes(b"extra")
        with self.assertRaisesRegex(ValueError, "unexpected files"):
            firmware.validate(self.root)
        extra.unlink()
        path = self.files / "adsp.b02"
        path.rename(self.root / "outside")
        path.symlink_to(self.root / "outside")
        with self.assertRaisesRegex(ValueError, "regular file"):
            firmware.validate(self.root)

    def test_forged_ready_status_and_invalid_source_hash_are_rejected(self):
        for key, value in (("status", "flash-ready"), ("source_image_sha256", "bad")):
            original = self.manifest[key]
            self.manifest[key] = value
            self.save()
            with self.subTest(key=key), self.assertRaisesRegex(ValueError, "manifest"):
                firmware.validate(self.root)
            self.manifest[key] = original


class GpuFirmwareTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.files = self.root / "files"
        self.files.mkdir()
        (self.files / "a740v3_zap.mdt").write_bytes(mdt())
        (self.files / "a740v3_zap.b01").write_bytes(bytes(16))
        (self.files / "a740v3_zap.b02").write_bytes(b"data")
        (self.files / "a740v3_sqe.fw").write_bytes(struct.pack("<4I", 0, 0x1234, 0, 0))
        self.gmu = struct.pack("<4I", 0x4000, 0, 1, 4) + struct.pack("<4I", 0x4000, 4, 0, 0) + b"data"
        (self.files / "gmu_gen70200.bin").write_bytes(self.gmu)
        records, details = firmware.inspect_files(self.files, "gpu")
        self.manifest = {"schema_version": 1, "component": "quest3-gpu", "status": "prepared",
                         "reference_build": "52433670036000520", "source_image_sha256": "a" * 64,
                         "files": records, "gpu": details}
        (self.root / "manifest.json").write_text(json.dumps(self.manifest))

    def tearDown(self):
        self.temp.cleanup()

    def test_gpu_component_cannot_replace_early_adsp_bundle(self):
        self.assertEqual(firmware.validate(self.root, "gpu"), self.manifest)
        with self.assertRaisesRegex(ValueError, "manifest"):
            firmware.validate(self.root)

    def test_only_mdt_referenced_zap_segments_are_packaged(self):
        self.assertEqual(firmware.required_segments(mdt(), "a740v3_zap"),
                         {"a740v3_zap.b01": 16, "a740v3_zap.b02": 4})
        (self.files / "a740v3_zap.mbn").write_bytes(b"alternate")
        with self.assertRaisesRegex(ValueError, "unexpected files"):
            firmware.validate(self.root, "gpu")

    def test_same_size_gpu_payload_changes_invalidate_bundle(self):
        (self.files / "a740v3_zap.b02").write_bytes(b"nope")
        with self.assertRaisesRegex(ValueError, "does not match"):
            firmware.validate(self.root, "gpu")

    def test_gmu_truncation_and_payload_without_backing_are_rejected(self):
        for blob, message in [(self.gmu[:15], "header"), (self.gmu[:-1], "payload"),
                              (self.gmu[16:], "preallocation")]:
            with self.subTest(message=message), self.assertRaisesRegex(ValueError, message):
                firmware.gmu_blocks(blob)

    def test_gmu_memory_overflow_and_metadata_order_are_rejected(self):
        overflow = struct.pack("<4I", 0xfffffffc, 0, 1, 8) + self.gmu[16:]
        late = self.gmu + self.gmu[:16]
        for blob, message in [(overflow, "address ranges"), (late, "precede")]:
            with self.subTest(message=message), self.assertRaisesRegex(ValueError, message):
                firmware.gmu_blocks(blob)

    def test_sqe_short_header_is_rejected_before_word_reads(self):
        (self.files / "a740v3_sqe.fw").write_bytes(bytes(4))
        with self.assertRaisesRegex(ValueError, "SQE"):
            firmware.validate(self.root, "gpu")
