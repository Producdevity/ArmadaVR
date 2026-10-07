import base64
import hashlib
import importlib.util
import io
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest
import zipfile


ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("ota", ROOT / "tools/verify-ota.py")
ota = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ota)


@unittest.skipUnless(shutil.which("openssl"), "OpenSSL is required")
class OtaTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.workspace = tempfile.TemporaryDirectory()
        cls.base = Path(cls.workspace.name)
        for name in ("device", "other"):
            subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-sha256", "-days", "1",
                            "-subj", "/CN=OTA fixture", "-set_serial", "12345", "-keyout", str(cls.base / (name + ".key")),
                            "-out", str(cls.base / (name + ".pem"))], check=True, capture_output=True)
            with zipfile.ZipFile(cls.base / (name + ".zip"), "w") as archive:
                archive.writestr("releasekey.x509.pem", (cls.base / (name + ".pem")).read_bytes())

    @classmethod
    def tearDownClass(cls):
        cls.workspace.cleanup()

    def setUp(self):
        self.case = tempfile.TemporaryDirectory(dir=self.base)
        self.path = Path(self.case.name) / "ota.zip"
        self.package()

    def tearDown(self):
        self.case.cleanup()

    def package(self, signer="device", build="12345", device="eureka", bad_hash=False, duplicate=False, bad_header=False, attributes=False):
        manifest = b"manifest fixture"
        header = struct.pack(">4sQQI", b"CrAU", 2, len(manifest) + int(bad_header), 0)
        metadata = header + manifest
        payload = metadata + b"payload fixture" * 100
        digest = base64.b64encode(hashlib.sha256(payload if not bad_hash else b"other").digest()).decode()
        properties = (f"FILE_SIZE={len(payload)}\nFILE_HASH={digest}\nMETADATA_SIZE={len(metadata)}\n"
                      f"METADATA_HASH={base64.b64encode(hashlib.sha256(metadata).digest()).decode()}\n")
        data = io.BytesIO()
        with zipfile.ZipFile(data, "w") as archive:
            archive.writestr("META-INF/com/android/metadata", f"ota-type=AB\npre-device={device}\npost-build-incremental={build}\n")
            archive.writestr("payload_properties.txt", properties)
            archive.writestr("payload.bin", payload)
            archive.writestr("META-INF/com/android/otacert", (self.base / (signer + ".pem")).read_bytes())
            if duplicate:
                import warnings
                with warnings.catch_warnings():
                    warnings.simplefilter("ignore", UserWarning)
                    archive.writestr("payload.bin", payload)
        body = data.getvalue()
        self.assertEqual(body[-22:-18], b"PK\x05\x06")
        signature = subprocess.run(["openssl", "cms", "-sign", "-binary", *([] if attributes else ["-noattr"]), "-md", "sha256", "-nosmimecap",
                                    "-signer", str(self.base / (signer + ".pem")), "-inkey", str(self.base / (signer + ".key")),
                                    "-outform", "DER"], input=body[:-2], check=True, capture_output=True).stdout
        message = b"signed fixture\0"
        comment_size = len(message) + len(signature) + 6
        self.path.write_bytes(body[:-2] + struct.pack("<H", comment_size) + message + signature +
                              struct.pack("<HHH", len(signature) + 6, 65535, comment_size))

    def verify(self, trust="device", build="12345"):
        return ota.verify(self.path, self.base / (trust + ".zip"), "eureka", build)

    def test_authentication_preserves_inputs_and_proof_limits(self):
        before = self.path.read_bytes()
        report = self.verify()
        self.assertTrue(report["ota_signature_verified_with_supplied_device_certificates"])
        self.assertTrue(report["payload_hash_verified"])
        self.assertEqual(report["authenticated_metadata"]["post-build-incremental"], "12345")
        for key in ("flash_ready", "recovery_restore_verified", "headset_boot_acceptance_verified", "device_rollback_acceptance_verified"):
            self.assertFalse(report[key])
        self.assertEqual(self.path.read_bytes(), before)

    def test_package_certificate_cannot_establish_trust(self):
        self.package(signer="other")
        with self.assertRaisesRegex(ValueError, "signature does not verify"):
            self.verify()

    def test_matching_issuer_serial_with_wrong_key_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "signature does not verify"):
            self.verify(trust="other")

    def test_signed_body_corruption_is_rejected(self):
        data = bytearray(self.path.read_bytes())
        data[100] ^= 1
        self.path.write_bytes(data)
        with self.assertRaisesRegex(ValueError, "signature does not verify"):
            self.verify()

    def test_signed_wrong_device_and_build_are_rejected(self):
        with self.assertRaisesRegex(ValueError, "build mismatch"):
            self.verify(build="67890")
        self.package(device="seacliff")
        with self.assertRaisesRegex(ValueError, "device, type or build mismatch"):
            self.verify()

    def test_authenticated_inconsistent_payload_is_rejected(self):
        self.package(bad_hash=True)
        with self.assertRaisesRegex(ValueError, "Payload hash mismatch"):
            self.verify()
        self.package(bad_header=True)
        with self.assertRaisesRegex(ValueError, "Payload header"):
            self.verify()

    def test_authenticated_duplicate_entries_are_rejected(self):
        self.package(duplicate=True)
        with self.assertRaisesRegex(ValueError, "duplicate OTA entries"):
            self.verify()

    def test_footer_bounds_and_ambiguous_end_record_are_rejected(self):
        original = self.path.read_bytes()
        for tail in (b"\0" * 6, struct.pack("<HHH", 65535, 65535, 1)):
            self.path.write_bytes(original[:-6] + tail)
            with self.assertRaisesRegex(ValueError, "footer"):
                self.verify()
        data = bytearray(original)
        data[-16:-12] = b"PK\x05\x06"
        self.path.write_bytes(data)
        with self.assertRaisesRegex(ValueError, "ambiguous ZIP"):
            self.verify()

    def test_device_files_and_symlinks_are_rejected(self):
        self.path.unlink()
        self.path.symlink_to("/dev/null")
        with self.assertRaises(OSError):
            self.verify()
        with self.assertRaisesRegex(ValueError, "regular file"):
            ota.open_regular(Path("/dev/null"), 1024)

    def test_cms_attributes_are_rejected_even_with_a_trusted_signature(self):
        self.package(attributes=True)
        with self.assertRaisesRegex(ValueError, "without signed or unsigned attributes"):
            self.verify()

    def test_malformed_der_is_rejected_before_crypto(self):
        for data in (b"\x30\x80", b"\x30\x81\x01\0", b"\x30\x82\0\x80", b"\x30\x08\0", b"\x1f\0"):
            with self.subTest(data=data), self.assertRaises(ValueError):
                ota.check_signature_format(data)


if __name__ == "__main__":
    unittest.main()
