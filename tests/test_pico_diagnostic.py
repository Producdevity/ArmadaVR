import importlib.util
from pathlib import Path
import struct
import unittest

spec = importlib.util.spec_from_file_location("pico_diagnostic", Path(__file__).parents[1] / "tools/build-pico-diagnostic.py")
diagnostic = importlib.util.module_from_spec(spec)
spec.loader.exec_module(diagnostic)


class PicoDiagnosticTests(unittest.TestCase):
    def elf(self):
        data = bytearray(120)
        data[:7] = b"\x7fELF\x02\x01\x01"
        struct.pack_into("<HH", data, 16, 2, 183)
        struct.pack_into("<Q", data, 32, 64)
        struct.pack_into("<HH", data, 54, 56, 1)
        struct.pack_into("<I", data, 64, 1)
        return data

    def test_static_architecture_and_loader_refusals(self):
        diagnostic.static_arm64(self.elf())
        for offset, fmt, value in ((16, "H", 3), (18, "H", 62), (32, "Q", 1000),
                                   (54, "H", 1), (56, "H", 128), (64, "I", 2), (64, "I", 3)):
            with self.subTest(offset=offset, value=value):
                data = self.elf()
                struct.pack_into("<" + fmt, data, offset, value)
                with self.assertRaises(ValueError):
                    diagnostic.static_arm64(data)

    def test_serial_requires_explicit_physical_identity(self):
        self.assertEqual(diagnostic.serial("pico-test_1.2"), "pico-test_1.2")
        for value in ("", "qemu", "a" * 65, "../a", "a\nb", "a=b", "a b"):
            with self.subTest(value=value), self.assertRaises(ValueError):
                diagnostic.serial(value)

    def test_archives_have_same_binary_and_no_root_or_shell(self):
        for serial in ("physical-fixture", "qemu"):
            raw = diagnostic.archive(self.elf(), serial)
            offset, entries = 0, {}
            while offset < len(raw):
                self.assertEqual(raw[offset:offset + 6], b"070701")
                fields = [int(raw[offset + 6 + i * 8:offset + 14 + i * 8], 16) for i in range(13)]
                begin = offset + 110
                name = raw[begin:begin + fields[11] - 1].decode()
                begin = (begin + fields[11] + 3) & ~3
                entries[name] = (fields[1], raw[begin:begin + fields[6]])
                self.assertEqual(fields[2:4], [0, 0])
                offset = (begin + fields[6] + 3) & ~3
            self.assertEqual(set(entries), {".", "dev", "proc", "sys", "etc", "init", "etc/pico-serial", "TRAILER!!!"})
            self.assertEqual(entries["init"], (0o100755, self.elf()))
            self.assertEqual(entries["etc/pico-serial"], (0o100600, (serial + "\n").encode()))
