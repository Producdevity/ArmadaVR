import importlib.util
from pathlib import Path
import struct
import hashlib
import tempfile
import unittest

spec = importlib.util.spec_from_file_location("inspect_boot", Path(__file__).parents[1] / "tools/inspect-boot.py")
boot = importlib.util.module_from_spec(spec)
spec.loader.exec_module(boot)


def boot_v3():
    return struct.pack("<8s4I4II1536s", b"ANDROID!", 32, 64, 0, 1580,
                       0, 0, 0, 0, 3, b"console=test")


class BootImageTests(unittest.TestCase):
    def test_v3_geometry_does_not_inherit_legacy_offsets(self):
        result = boot.layout(boot_v3(), 12288)
        self.assertEqual(result["header_version"], 3)
        self.assertEqual(result["page_size"], 4096)
        self.assertEqual(result["cmdline"], "console=test")
        self.assertEqual(result["sections"], [{"name": "kernel", "offset": 4096, "size": 32},
                                               {"name": "ramdisk", "offset": 8192, "size": 64}])
        self.assertEqual(result["headset_compatibility"], "not established")
        self.assertEqual(result["signature_verification"], "not performed")

    def test_unknown_header_version_and_wrong_header_size_rejected(self):
        for offset, value in ((40, 5), (20, 1632)):
            header = bytearray(boot_v3())
            struct.pack_into("<I", header, offset, value)
            with self.subTest(offset=offset), self.assertRaises(ValueError):
                boot.layout(header, 12288)

    def test_truncated_header_payload_and_padding_rejected(self):
        with self.assertRaises(ValueError):
            boot.layout(boot_v3()[:44], 12288)
        for size in (4096, 8220, 12287):
            with self.subTest(size=size), self.assertRaises(ValueError):
                boot.layout(boot_v3(), size)

    def test_qemu_kernel_and_device_files_rejected(self):
        with self.assertRaisesRegex(ValueError, "not an Android boot"):
            boot.layout(bytes(4096), 4096)
        with self.assertRaisesRegex(ValueError, "regular file"):
            boot.inspect(Path("/dev/null"))

    def test_v4_vendor_fragments_and_section_hashes(self):
        header = bytearray(4096)
        header[:8] = b"VNDRBOOT"
        for offset, value in ((8, 4), (12, 4096), (24, 8), (2096, 2128),
                              (2112, 216), (2116, 2), (2120, 108)):
            struct.pack_into("<I", header, offset, value)
        table = b"".join(struct.pack("<III32s16I", 4, offset, kind, name, *range(16))
                         for offset, kind, name in ((0, 1, b"platform"), (4, 3, b"dlkm")))
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "vendor_boot.img"
            path.write_bytes(header + b"12345678" + bytes(4088) + table + bytes(4096 - len(table)))
            result = boot.inspect(path)
        self.assertEqual(result["sections"][0]["sha256"], hashlib.sha256(b"12345678").hexdigest())
        self.assertEqual([part["name"] for part in result["vendor_ramdisk_fragments"]], ["platform", "dlkm"])
        self.assertEqual(result["vendor_ramdisk_fragments"][1]["board_id"], list(range(16)))
        self.assertEqual(result["signature_verification"], "not performed")

    def test_vendor_fragment_bounds_and_overlap(self):
        def entry(offset, size):
            return struct.pack("<III32s16I", size, offset, 1, b"", *([0] * 16))
        for table, count in ((entry(7, 2), 1), (entry(0, 5) + entry(4, 4), 2), (entry(0, 8)[:-1], 1)):
            with self.subTest(table=table), self.assertRaises(ValueError):
                boot.ramdisk_fragments(table, count, 108, 8)


class Arm64ImageTests(unittest.TestCase):
    @staticmethod
    def header(offset=0, size=4096, flags=10):
        data = bytearray(64)
        struct.pack_into("<3Q", data, 8, offset, size, flags)
        data[56:60] = b"ARM\x64"
        return data

    def test_zero_text_offset_is_valid_and_does_not_inherit_quest_pro_offset(self):
        report = boot.arm64_layout(self.header(), 4096, 0x90000000)
        self.assertEqual(report["effective_text_offset"], 0)
        self.assertEqual(report["aligned_base_address"], 0x90000000)
        self.assertEqual(report["page_size"], 4096)
        self.assertFalse(report["ram_ownership_verified"])
        with self.assertRaisesRegex(ValueError, "2 MiB aligned"):
            boot.arm64_layout(self.header(), 4096, 0x90080000)

    def test_offset_and_page_size_follow_header_fields(self):
        for page, flags in ((None, 8), (4096, 10), (16384, 12), (65536, 14)):
            with self.subTest(page=page):
                report = boot.arm64_layout(self.header(offset=0x80000, flags=flags), 64, 0x90080000)
                self.assertEqual(report["page_size"], page)
                self.assertEqual(report["aligned_base_address"], 0x90000000)
        report = boot.arm64_layout(self.header(flags=3), 64)
        self.assertEqual(report["endianness"], "big")
        self.assertFalse(report["placement_anywhere_in_ram"])

    def test_legacy_unbounded_size_is_reported_without_trusting_offset_byte_order(self):
        report = boot.arm64_layout(self.header(offset=0x80000000000, size=0, flags=0), 512)
        self.assertEqual(report["effective_text_offset"], 0x80000)
        self.assertIsNone(report["minimum_ram_bytes"])
        self.assertIsNone(report["bytes_beyond_declared_image"])
        self.assertEqual(len(report["warnings"]), 1)

    def test_bss_extent_and_trailing_file_bytes_are_distinct(self):
        report = boot.arm64_layout(self.header(size=8192), 4096)
        self.assertEqual(report["minimum_ram_bytes"], 8192)
        self.assertEqual(report["bytes_beyond_declared_image"], 0)
        report = boot.arm64_layout(self.header(size=4096), 5000)
        self.assertEqual(report["bytes_beyond_declared_image"], 904)
        self.assertEqual(len(report["warnings"]), 1)
        self.assertEqual(report["sections"][0]["size"], 5000)

    def test_truncated_wrong_magic_and_reserved_fields_are_rejected(self):
        for data, size in ((self.header()[:60], 64), (self.header(), 63), (bytes(64), 64)):
            with self.subTest(data=data, size=size), self.assertRaisesRegex(ValueError, "header"):
                boot.arm64_layout(data, size)
        for offset in (24, 32, 40, 48):
            data = self.header()
            struct.pack_into("<Q", data, offset, 16 if offset == 24 else 1)
            with self.subTest(offset=offset), self.assertRaisesRegex(ValueError, "reserved"):
                boot.arm64_layout(data, 64)
        with self.assertRaisesRegex(ValueError, "smaller"):
            boot.arm64_layout(self.header(size=63), 64)

    def test_invalid_addresses_and_overflowing_extent_are_rejected(self):
        for address in (-1, 2**64, 0x90000001):
            with self.subTest(address=address), self.assertRaises(ValueError):
                boot.arm64_layout(self.header(), 64, address)
        with self.assertRaisesRegex(ValueError, "overflows"):
            boot.arm64_layout(self.header(size=4 * 1024**2), 64, 2**64 - 2 * 1024**2)
        with self.assertRaises(ValueError):
            boot.arm64_layout(self.header(offset=0x80000), 64, 0)

    def test_regular_file_hash_and_pe_header_offset_are_preserved(self):
        data = self.header(size=128)
        struct.pack_into("<I", data, 60, 64)
        data += bytes(64)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "Image"
            path.write_bytes(data)
            report = boot.inspect(path, arm64=True)
            self.assertEqual(report["pe_header_offset"], 64)
            self.assertEqual(report["sections"][0]["sha256"], hashlib.sha256(data).hexdigest())
            with self.assertRaisesRegex(ValueError, "not an Android boot"):
                boot.inspect(path)
        with self.assertRaisesRegex(ValueError, "regular file"):
            boot.inspect(Path("/dev/null"), arm64=True)

    def test_android_container_does_not_accept_raw_image_address_checks(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "boot.img"
            path.write_bytes(boot_v3() + bytes(12288 - len(boot_v3())))
            with self.assertRaisesRegex(ValueError, "raw ARM64"):
                boot.inspect(path, load_address=0x90000000)
