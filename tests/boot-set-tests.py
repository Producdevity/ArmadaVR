#!/usr/bin/env python3
"""Exercise the boot-set verifier against images signed by the pinned AOSP tool."""
import argparse
import hashlib
import importlib.util
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("boot_set", ROOT / "tools/verify-boot-set.py")
boot_set = importlib.util.module_from_spec(spec)
spec.loader.exec_module(boot_set)


class BootSetTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.avb = boot_set.load_avb(AVBTOOL)
        cls.workspace = tempfile.TemporaryDirectory()
        cls.base = Path(cls.workspace.name)
        cls.original = cls.base / "original"
        cls.original.mkdir()
        for name in ("key", "other-key"):
            subprocess.run(["openssl", "genpkey", "-algorithm", "RSA", "-pkeyopt", "rsa_keygen_bits:2048",
                            "-out", str(cls.base / (name + ".pem"))], check=True, capture_output=True)
        cls.command("extract_public_key", "--key", cls.base / "key.pem", "--output", cls.base / "key.avbpubkey")
        (cls.original / "metadata").write_text("ota-type=AB\npre-device=eureka\npost-build-incremental=12345\n")
        for name in boot_set.BOOT_PARTITIONS:
            image = cls.original / (name + ".img")
            image.write_bytes((name.encode() + b"\0" * 16) * 256)
            cls.command("add_hash_footer", "--image", image, "--partition_name", name,
                        "--partition_size", 131072, "--salt", "01020304")
        cls.command("make_vbmeta_image", "--output", cls.original / "vbmeta_system.img",
                    "--algorithm", "SHA256_RSA2048", "--key", cls.base / "key.pem", "--rollback_index", 42)
        cls.make_root(cls.original)

    @classmethod
    def tearDownClass(cls):
        cls.workspace.cleanup()

    @classmethod
    def command(cls, *args):
        return subprocess.run([sys.executable, "-B", str(AVBTOOL), *map(str, args)],
                              check=True, capture_output=True, text=True)

    @classmethod
    def make_root(cls, directory, *extra):
        args = ["make_vbmeta_image", "--output", directory / "vbmeta.img", "--algorithm", "SHA256_RSA2048",
                "--key", cls.base / "key.pem", "--rollback_index", 42,
                "--chain_partition", f"vbmeta_system:1:{cls.base / 'key.avbpubkey'}"]
        for name in boot_set.BOOT_PARTITIONS:
            args += ["--include_descriptors_from_image", directory / (name + ".img")]
        cls.command(*args, *extra)

    def setUp(self):
        self.case = tempfile.TemporaryDirectory(dir=self.base)
        self.directory = Path(self.case.name) / "images"
        shutil.copytree(self.original, self.directory)

    def tearDown(self):
        self.case.cleanup()

    def verify(self, build="12345"):
        return boot_set.verify_set(self.directory, build, self.avb)

    def test_signed_set_and_unsigned_footers_preserve_inputs_and_proof_limits(self):
        def hashes():
            return {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in self.directory.iterdir()}
        before = hashes()
        result = self.verify()
        self.assertTrue(result["boot_integrity_verified"])
        self.assertEqual(result["images"]["boot"]["signature"], "unsigned")
        self.assertEqual(result["images"]["vbmeta"]["signature"], "verified_with_embedded_key")
        self.assertEqual(result["chains"][0]["rollback_index_location"], 1)
        for flag in ("manufacturer_key_trust_verified", "ota_metadata_authenticated",
                     "device_rollback_acceptance_verified", "headset_boot_acceptance_verified", "recovery_restore_verified"):
            self.assertFalse(result[flag])
        self.assertEqual(before, hashes())

    def test_wrong_declared_build_and_duplicate_metadata_are_rejected(self):
        with self.assertRaisesRegex(ValueError, "Firmware build mismatch"):
            self.verify("67890")
        with (self.directory / "metadata").open("a") as stream:
            stream.write("post-build-incremental=12345\n")
        with self.assertRaisesRegex(ValueError, "Duplicate OTA"):
            self.verify()

    def test_payload_corruption_is_rejected(self):
        with (self.directory / "boot.img").open("r+b") as stream:
            stream.seek(100)
            stream.write(b"corruption")
        with self.assertRaisesRegex(ValueError, "payload hash mismatch"):
            self.verify()

    def test_regenerated_footer_cannot_hide_stale_signed_metadata(self):
        path = self.directory / "boot.img"
        self.command("erase_footer", "--image", path)
        with path.open("r+b") as stream:
            stream.write(b"changed")
        self.command("add_hash_footer", "--image", path, "--partition_name", "boot",
                     "--partition_size", 131072, "--salt", "01020304")
        with self.assertRaisesRegex(ValueError, "payload hash mismatch"):
            self.verify()

    def test_signature_corruption_is_rejected(self):
        with (self.directory / "vbmeta.img").open("r+b") as stream:
            stream.seek(300)
            data = stream.read(1)
            stream.seek(300)
            stream.write(bytes([data[0] ^ 1]))
        with self.assertRaisesRegex(ValueError, "signature"):
            self.verify()

    def test_unsigned_root_is_rejected(self):
        self.command("make_vbmeta_image", "--output", self.directory / "vbmeta.img")
        with self.assertRaisesRegex(ValueError, "Unsigned metadata"):
            self.verify()

    def test_untrusted_chain_replacement_is_rejected(self):
        self.command("make_vbmeta_image", "--output", self.directory / "vbmeta_system.img",
                     "--algorithm", "SHA256_RSA2048", "--key", self.base / "other-key.pem")
        with self.assertRaisesRegex(ValueError, "Chained key"):
            self.verify()

    def test_verification_disable_flags_are_rejected(self):
        self.make_root(self.directory, "--flags", 2)
        with self.assertRaisesRegex(ValueError, "verification flags"):
            self.verify()

    def test_missing_signed_partition_coverage_is_rejected(self):
        self.command("make_vbmeta_image", "--output", self.directory / "vbmeta.img",
                     "--algorithm", "SHA256_RSA2048", "--key", self.base / "key.pem")
        with self.assertRaisesRegex(ValueError, "does not cover every boot partition"):
            self.verify()

    def test_hashtree_partitions_are_explicitly_unchecked(self):
        path = self.directory / "system.img"
        path.write_bytes(b"system" * 1024)
        self.command("add_hashtree_footer", "--image", path, "--partition_name", "system",
                     "--partition_size", 131072, "--salt", "01020304", "--do_not_generate_fec")
        self.command("make_vbmeta_image", "--output", self.directory / "vbmeta_system.img",
                     "--algorithm", "SHA256_RSA2048", "--key", self.base / "key.pem",
                     "--include_descriptors_from_image", path)
        result = self.verify()
        self.assertTrue(result["boot_integrity_verified"])
        self.assertFalse(result["complete_partition_verification"])
        self.assertEqual(result["unchecked_partitions"], [{"metadata": "vbmeta_system", "partition": "system",
                                                          "reason": "outside boot-set verification scope"}])

    def test_conflicting_rollback_slots_are_rejected(self):
        self.command("make_vbmeta_image", "--output", self.directory / "vbmeta_leaf.img",
                     "--algorithm", "SHA256_RSA2048", "--key", self.base / "key.pem")
        self.command("make_vbmeta_image", "--output", self.directory / "vbmeta_system.img",
                     "--algorithm", "SHA256_RSA2048", "--key", self.base / "key.pem",
                     "--chain_partition", f"vbmeta_leaf:1:{self.base / 'key.avbpubkey'}")
        with self.assertRaisesRegex(ValueError, "Duplicate rollback index location"):
            self.verify()

    def test_vbmeta_block_and_descriptor_bounds_are_rejected(self):
        path = self.directory / "boot.img"
        original = path.read_bytes()
        footer = self.avb.AvbFooter(original[-64:])
        offset = footer.vbmeta_offset
        for field, value, message in (("descriptors_size", 2**32, "exceeds its block"),
                                       ("auxiliary_data_block_size", 2**32, "block bounds")):
            with self.subTest(field=field):
                header = self.avb.AvbVBMetaHeader(original[offset:offset + 256])
                setattr(header, field, value)
                path.write_bytes(original[:offset] + header.encode() + original[offset + 256:])
                with self.assertRaisesRegex(ValueError, message):
                    self.verify()
        path.write_bytes(original)
        header = self.avb.AvbVBMetaHeader(original[offset:offset + 256])
        with path.open("r+b") as stream:
            stream.seek(offset + 256 + header.authentication_data_block_size + header.descriptors_offset + 8)
            stream.write(struct.pack("!Q", 2**32))
        with self.assertRaisesRegex(ValueError, "descriptor bounds"):
            self.verify()

    def test_footer_outside_file_and_truncated_vbmeta_are_rejected(self):
        with (self.directory / "boot.img").open("r+b") as stream:
            stream.seek(-64 + 20, 2)
            stream.write(struct.pack("!Q", 131072))
        with self.assertRaisesRegex(ValueError, "footer bounds"):
            self.verify()
        with (self.directory / "vbmeta.img").open("r+b") as stream:
            stream.truncate(255)
            stream.seek(0)
            with self.assertRaisesRegex(ValueError, "too short"):
                boot_set.read_avb(self.avb, stream)

    def test_missing_chain_and_device_symlink_are_rejected(self):
        (self.directory / "vbmeta_system.img").unlink()
        with self.assertRaises(FileNotFoundError):
            self.verify()
        (self.directory / "boot.img").unlink()
        (self.directory / "boot.img").symlink_to("/dev/null")
        with self.assertRaises(OSError):
            self.verify()
        with self.assertRaisesRegex(ValueError, "regular file"):
            boot_set.open_regular(Path("/dev/null"), 1024)

    def test_modified_tool_is_not_executed(self):
        tool = self.directory / "avbtool.py"
        tool.write_bytes(AVBTOOL.read_bytes() + b"\nraise RuntimeError('executed modified source')\n")
        with self.assertRaises(ValueError):
            boot_set.load_avb(tool)

    def test_cli_reports_failure_without_a_success_document(self):
        result = subprocess.run([sys.executable, "-B", str(ROOT / "tools/verify-boot-set.py"),
                                 str(self.directory), "--expected-build", "67890", "--avbtool", str(AVBTOOL)],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 1)
        self.assertEqual(result.stdout, "")
        self.assertIn("Firmware build mismatch", result.stderr)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("avbtool", type=Path)
    args = parser.parse_args()
    AVBTOOL = args.avbtool.resolve()
    unittest.main(argv=[sys.argv[0]], verbosity=2)
