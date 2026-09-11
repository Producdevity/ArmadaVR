import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch


spec = importlib.util.spec_from_file_location("assembly", Path(__file__).parents[1] / "tools/assemble-quest-boot.py")
assembly = importlib.util.module_from_spec(spec)
spec.loader.exec_module(assembly)


def table(payloads):
    start = 32 + 32 * len(payloads)
    entries = bytearray()
    for index, payload in enumerate(payloads):
        entries.extend(struct.pack(">8I", len(payload), start, index, 0, 0, 0, 0, 0))
        start += len(payload)
    return bytearray(struct.pack(">8I", 0xd7b7ab1e, start, 32, 32, len(payloads), 32, 4096, 0)
                     + entries + b"".join(payloads))


class ArgumentTests(unittest.TestCase):
    def test_final_empty_cmdline_survives_unpack(self):
        args = assembly.parse_arguments(b"--header_version\x004\x00--cmdline\x00\x00")
        self.assertEqual(args, ["--header_version", "4", "--cmdline", ""])
        assembly.replace(args, "--cmdline", "root=LABEL=armada-vr-root ro")
        self.assertEqual(args[-1], "root=LABEL=armada-vr-root ro")

    def test_malformed_or_duplicate_option_vectors_are_rejected(self):
        for data in (b"", b"--cmdline\0", b"--cmdline\0root=x", b"--x\0a\0--x\0b\0", b"value\0other\0"):
            with self.subTest(data=data), self.assertRaises(ValueError):
                assembly.parse_arguments(data)

    def test_missing_or_ambiguous_replacement_is_rejected(self):
        for args in (["--kernel", "a"], ["--cmdline", "a", "--cmdline", "b"], ["--kernel", "--cmdline"]):
            with self.subTest(args=args), self.assertRaisesRegex(ValueError, "Missing or ambiguous"):
                assembly.replace(args, "--cmdline", "root=x")

    def test_label_cannot_add_kernel_arguments_or_select_an_android_path(self):
        for label in ("", "x" * 17, "userdata rw", "/dev/block/by-name/userdata", "x\ninit=/bin/sh", "é"):
            with self.subTest(label=label), self.assertRaisesRegex(ValueError, "Root label"):
                assembly.root_arguments(label)
        args = assembly.root_arguments("armada-vr-root").split()
        self.assertIn("root=LABEL=armada-vr-root", args)
        self.assertIn("ro", args)
        self.assertIn("rootflags=noload", args)
        self.assertIn("systemd.volatile=overlay", args)
        self.assertIn("rd.emergency=poweroff", args)


class OverlayTests(unittest.TestCase):
    def setUp(self):
        self.payloads = [struct.pack(">II", 0xd00dfeed, 40) + bytes([i]) * 32 for i in (1, 2)]
        self.compiled = {hashlib.sha256(p).hexdigest(): f"board-{i}.dtbo" for i, p in enumerate(self.payloads)}

    def test_reference_order_and_board_identifiers_are_preserved(self):
        records = assembly.overlay_table(table(self.payloads[::-1]), self.compiled)
        self.assertEqual([r["compiled_path"] for r in records], ["board-1.dtbo", "board-0.dtbo"])
        self.assertEqual([r["identifiers"][0] for r in records], [0, 1])

    def test_truncated_or_unsupported_geometry_is_rejected(self):
        good = table(self.payloads)
        with self.assertRaisesRegex(ValueError, "Truncated"):
            assembly.overlay_table(good[:31], self.compiled)
        for offset, value in ((0, 0), (4, len(good) + 1), (8, 28), (12, 16), (16, 129), (20, 64), (24, 2048), (28, 1)):
            data = good.copy()
            struct.pack_into(">I", data, offset, value)
            with self.subTest(offset=offset), self.assertRaisesRegex(ValueError, "geometry"):
                assembly.overlay_table(data, self.compiled)

    def test_payload_cannot_reference_table_or_leave_image(self):
        for size, start in ((40, 32), (40, 150), (39, 96)):
            data = table(self.payloads)
            struct.pack_into(">II", data, 32, size, start)
            with self.subTest(size=size, start=start), self.assertRaisesRegex(ValueError, "outside"):
                assembly.overlay_table(data, self.compiled)

    def test_aliased_payload_ranges_are_rejected(self):
        data = table(self.payloads)
        struct.pack_into(">II", data, 64, 40, 96)
        with self.assertRaisesRegex(ValueError, "Overlapping"):
            assembly.overlay_table(data, self.compiled)

    def test_invalid_fdt_size_and_magic_are_rejected(self):
        for magic, size in ((0, 40), (0xd00dfeed, 41)):
            data = table(self.payloads)
            struct.pack_into(">II", data, 96, magic, size)
            with self.subTest(magic=magic, size=size), self.assertRaisesRegex(ValueError, "FDT header"):
                assembly.overlay_table(data, self.compiled)

    def test_reference_must_match_compiled_bytes(self):
        data = table(self.payloads)
        data[-1] ^= 1
        with self.assertRaisesRegex(ValueError, "does not match"):
            assembly.overlay_table(data, self.compiled)

    def test_missing_or_repeated_board_is_rejected(self):
        for payloads in (self.payloads[:1], [self.payloads[0], self.payloads[0]]):
            with self.subTest(count=len(payloads)), self.assertRaisesRegex(ValueError, "exactly once"):
                assembly.overlay_table(table(payloads), self.compiled)


class ToolIntegrityTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.directory = self.root / "tools"
        self.directory.mkdir()
        (self.root / "profiles").mkdir()
        data = b"pinned tool fixture\n"
        self.tool = self.directory / "mkbootimg.py"
        self.tool.write_bytes(data)
        profile = {"files": [{"path": self.tool.name, "bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}]}
        (self.root / "profiles/mkbootimg.json").write_text(json.dumps(profile))
        self.patcher = patch.object(assembly, "ROOT", self.root)
        self.patcher.start()

    def tearDown(self):
        self.patcher.stop()
        self.temp.cleanup()

    def test_source_record_is_optional_but_extra_executable_is_rejected(self):
        assembly.checked_tools(self.directory)
        (self.directory / "source.json").write_text("{}")
        assembly.checked_tools(self.directory)
        (self.directory / "hashlib.py").write_text("unexpected import override")
        with self.assertRaisesRegex(ValueError, "unexpected files"):
            assembly.checked_tools(self.directory)

    def test_changed_and_missing_tool_are_rejected(self):
        self.tool.write_bytes(b"changed tool fixture")
        with self.assertRaisesRegex(ValueError, "mismatch"):
            assembly.checked_tools(self.directory)
        self.tool.unlink()
        with self.assertRaisesRegex(ValueError, "missing"):
            assembly.checked_tools(self.directory)

    def test_symlinked_tree_or_dependency_is_rejected(self):
        link = self.root / "linked-tools"
        link.symlink_to(self.directory, target_is_directory=True)
        with self.assertRaisesRegex(ValueError, "symlinks"):
            assembly.checked_tools(link)
        self.tool.rename(self.root / "original")
        self.tool.symlink_to(self.root / "original")
        with self.assertRaisesRegex(ValueError, "symlinks"):
            assembly.checked_tools(self.directory)
