import hashlib
import importlib.util
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

TOOLS = Path(__file__).resolve().parents[1] / "tools"
sys.path.insert(0, str(TOOLS))
from devicetree import boot_memory, inventory, read_fdt
spec = importlib.util.spec_from_file_location("kernel_source", TOOLS / "kernel-source.py")
source_tool = importlib.util.module_from_spec(spec)
spec.loader.exec_module(source_tool)


class DeviceTreeTests(unittest.TestCase):
    def memory_tree(self, contents):
        if not shutil.which("dtc"):
            self.skipTest("Requires dtc")
        with tempfile.TemporaryDirectory() as temp:
            source = Path(temp) / "memory.dts"
            output = Path(temp) / "memory.dtb"
            source.write_text('/dts-v1/; / { #address-cells = <2>; #size-cells = <2>; ' + contents + ' };')
            subprocess.run(["dtc", "-I", "dts", "-O", "dtb", "-o", str(output), str(source)],
                           check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            return output.read_bytes()

    def test_bootloader_ram_placeholder_and_dynamic_reservations(self):
        data = self.memory_tree('memory { device_type = "memory"; reg = <0 0 0 0>; }; reserved-memory { #address-cells = <2>; #size-cells = <2>; ranges; pool { size = <0 0x2000000>; alloc-ranges = <0 0 0 0xffffffff>; }; };')
        result = boot_memory(data)
        self.assertEqual(result["ram"], [])
        self.assertEqual(result["unresolved_nodes"], ["/memory"])
        self.assertEqual(result["dynamic_reservations"][0]["size"], 0x2000000)
        self.assertFalse(result["fixed_memory_map_complete"])

    def test_64_bit_ram_reservations_and_disabled_nodes(self):
        data = self.memory_tree('memory { device_type = "memory"; reg = <1 0 0 0x80000000>; }; reserved-memory { #address-cells = <2>; #size-cells = <2>; ranges; firmware { reg = <1 0x100000 0 0x200000>; no-map; }; disabled { status = "disabled"; reg = <0>; }; };')
        result = boot_memory(data)
        self.assertEqual(result["ram"][0]["start"], 1 << 32)
        self.assertEqual(result["reserved"], [{"path": "/reserved-memory/firmware", "start": (1 << 32) + 0x100000, "size": 0x200000}])
        self.assertTrue(result["fixed_memory_map_complete"])

    def test_unsafe_memory_geometry_and_overlap_rejected(self):
        for contents in (
            'memory { device_type = "memory"; reg = <0 0x80000000 0>; };',
            'memory { device_type = "memory"; reg = <0xffffffff 0xffffffff 0 2>; };',
            'memory { device_type = "memory"; reg = <0 0x80000000 0 0x200000 0 0x80100000 0 0x200000>; };',
            'reserved-memory { #address-cells = <2>; #size-cells = <2>; ranges = <0 0 0 0 0 0x100000>; };',
            'bus { memory { device_type = "memory"; reg = <0 0x80000000 0 0x200000>; }; };',
        ):
            with self.subTest(contents=contents), self.assertRaises(ValueError):
                boot_memory(self.memory_tree(contents))

    def test_fdt_reservation_map_bounds_and_ranges(self):
        data = bytearray(self.memory_tree('memory { device_type = "memory"; reg = <0 0x80000000 0 0x200000>; };'))
        structure = struct.unpack_from(">I", data, 8)[0]
        struct.pack_into(">I", data, 16, structure)
        with self.assertRaisesRegex(ValueError, "reservation map offset"):
            boot_memory(data)
        struct.pack_into(">I", data, 16, 40)
        struct.pack_into(">QQ", data, 40, 0x80000000, 0x1000)
        with self.assertRaisesRegex(ValueError, "Unterminated"):
            boot_memory(data)

    def test_incomplete_and_invalid_fdt_rejected(self):
        for data in (b"", bytes(40), struct.pack(">10I", 0xd00dfeed, 4096, 40, 44, 0, 17, 16, 0, 4, 4)):
            with self.subTest(data=data), self.assertRaises(ValueError):
                read_fdt(data)

    def test_source_changes_and_escaping_links_rejected(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / "board.dts").write_bytes(b"original")
            profile = {"files": {"board.dts": hashlib.sha256(b"original").hexdigest()}}
            source_tool.verify(profile, root)
            (root / "board.dts").write_bytes(b"modified")
            with self.assertRaisesRegex(ValueError, "checksum"):
                source_tool.verify(profile, root)
            (root / "outside").symlink_to("/etc")
            with self.assertRaisesRegex(ValueError, "escapes"):
                source_tool.source_path(root, "outside/hosts")
            for path in ("../file", "/file", "dir/../file", "./file"):
                with self.subTest(path=path), self.assertRaises(ValueError):
                    source_tool.source_path(root, path)

    def test_pico_missing_board_fails_before_build(self):
        with tempfile.TemporaryDirectory() as temp:
            with self.assertRaisesRegex(ValueError, "omits arch/arm64/boot/dts/vendor"):
                source_tool.build(source_tool.read_profile("pico-neo3"), Path(temp) / "missing", Path(temp) / "build")
            self.assertFalse((Path(temp) / "build").exists())

    @unittest.skipUnless(all(shutil.which(t) for t in ("cc", "dtc", "fdtoverlay")), "Requires C preprocessor and dtc tools")
    def test_compilation_overlay_selection_and_inherited_status(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            source = root / "source"
            source.mkdir()
            files = {
                "base.dts": '/dts-v1/; / { model = "base"; compatible = "test,board"; qcom,board-id = <1 0>; bus { compatible = "simple-bus"; status = "disabled"; child: sensor { compatible = "test,sensor"; }; }; };',
                "revision.dts": '/dts-v1/; /plugin/; / { model = "selection metadata"; compatible = "test,revision"; qcom,board-id = <2 3>; }; &child { revision = <2>; };',
            }
            for name, data in files.items():
                (source / name).write_text(data)
            profile = {"repository": "test/fixture", "commit": "0" * 40, "kernel_version": "fixture",
                       "base": "base.dts", "overlays": ["revision.dts"], "include_dirs": [],
                       "files": {name: hashlib.sha256(data.encode()).hexdigest() for name, data in files.items()}}
            report = source_tool.build(profile, source, root / "build")
            self.assertEqual(len(report["artifacts"]), 3)
            self.assertFalse(report["hardware_boot_verified"])
            merged = read_fdt((root / "build/revision.dtb").read_bytes())
            self.assertEqual(merged["/bus/sensor"]["revision"], struct.pack(">I", 2))
            self.assertEqual(merged["/"]["model"], b"base\0")
            self.assertEqual(report["artifacts"][1]["qcom,board-id"], [2, 3])
            details = inventory((root / "build/revision.dtb").read_bytes())
            child = next(node for node in details["nodes"] if node["path"] == "/bus/sensor")
            self.assertEqual(child["status"], "okay")
            self.assertFalse(child["enabled_in_tree"])
            with self.assertRaisesRegex(ValueError, "output exists"):
                source_tool.build(profile, source, root / "build")
