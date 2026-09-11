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
