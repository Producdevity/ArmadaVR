import importlib.util
from pathlib import Path
import tempfile
import struct
import unittest

spec = importlib.util.spec_from_file_location("kernel_build", Path(__file__).parents[1] / "tools/kernel-build.py")
kernel = importlib.util.module_from_spec(spec)
spec.loader.exec_module(kernel)


class KernelPatchTests(unittest.TestCase):
    def layout_fixture(self, root, extra_bss=None):
        image = bytearray(64)
        struct.pack_into("<Q", image, 16, 0x4000)
        image[56:60] = b"ARM\x64"
        (root / "Image").write_bytes(image)
        names = b"\0.head.text\0.bss\0.symtab\0.strtab\0.shstrtab\0"
        symbol_names = b"\0_text\0_end\0__bss_start\0__bss_stop\0"
        symbols = bytes(24)
        for label, value in (("_text", 0x4000), ("_end", 0x8000),
                             ("__bss_start", 0x7000), ("__bss_stop", 0x7100)):
            symbols += struct.pack("<IBBHQQ", symbol_names.index(label.encode()), 0x10, 0, 0xfff1, value, 0)
        data = bytearray(64) + image
        sections = [(0,) * 10, (1, 1, 6, 0x4000, 64, 64, 0, 0, 4, 0),
                    (names.index(b".bss"), 8, 3, 0x7000, 128, 0x100, 0, 0, 8, 0)]
        sections.append((names.index(b".symtab"), 2, 0, 0, len(data), len(symbols), 4, 1, 8, 24))
        data.extend(symbols)
        sections.append((names.index(b".strtab"), 3, 0, 0, len(data), len(symbol_names), 0, 0, 1, 0))
        data.extend(symbol_names)
        sections.append((names.index(b".shstrtab"), 3, 0, 0, len(data), len(names), 0, 0, 1, 0))
        data.extend(names)
        if extra_bss is not None:
            sections.append((names.index(b".bss"), 8, 3, extra_bss, 128, 0x100, 0, 0, 8, 0))
        offset = len(data)
        for section in sections:
            data.extend(struct.pack("<IIQQQQIIQQ", *section))
        struct.pack_into("<16sHHIQQQIHHHHHH", data, 0, b"\x7fELF\x02\x01\x01", 2, 183, 1,
                         0x4000, 0, offset, 0, 64, 0, 0, 64, len(sections), 5)
        elf = root / "vmlinux"
        elf.write_bytes(data)
        return elf, root / "Image"

    def test_image_layout_accepts_initialized_extent(self):
        with tempfile.TemporaryDirectory() as directory:
            result = kernel.validate_image_layout(*self.layout_fixture(Path(directory)))
            self.assertEqual(result["image_size"], 0x4000)
            self.assertEqual(len(result["allocated_sections"]), 2)

    def test_image_layout_rejects_rtic_after_end(self):
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaisesRegex(ValueError, "outside Image extent: .bss"):
                kernel.validate_image_layout(*self.layout_fixture(Path(directory), extra_bss=0x8000))

    def test_image_layout_accepts_relocatable_kernel(self):
        with tempfile.TemporaryDirectory() as directory:
            elf, image = self.layout_fixture(Path(directory))
            data = bytearray(elf.read_bytes())
            struct.pack_into("<H", data, 16, 3)
            elf.write_bytes(data)
            self.assertEqual(kernel.validate_image_layout(elf, image)["image_size"], 0x4000)

    def test_image_layout_rejects_wrong_architecture(self):
        with tempfile.TemporaryDirectory() as directory:
            elf, image = self.layout_fixture(Path(directory))
            data = bytearray(elf.read_bytes())
            struct.pack_into("<H", data, 18, 62)
            elf.write_bytes(data)
            with self.assertRaisesRegex(ValueError, "AArch64"):
                kernel.validate_image_layout(elf, image)

    def test_image_layout_rejects_unzeroed_bss_inside_image(self):
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaisesRegex(ValueError, "outside kernel BSS"):
                kernel.validate_image_layout(*self.layout_fixture(Path(directory), extra_bss=0x7f00))

    def test_image_layout_rejects_wrong_header_extent(self):
        with tempfile.TemporaryDirectory() as directory:
            elf, image = self.layout_fixture(Path(directory))
            data = bytearray(image.read_bytes())
            struct.pack_into("<Q", data, 16, 0x5000)
            image.write_bytes(data)
            with self.assertRaisesRegex(ValueError, "extent disagrees"):
                kernel.validate_image_layout(elf, image)

    def test_image_layout_rejects_truncated_elf(self):
        with tempfile.TemporaryDirectory() as directory:
            elf, image = self.layout_fixture(Path(directory))
            elf.write_bytes(elf.read_bytes()[:-1])
            with self.assertRaisesRegex(ValueError, "Truncated kernel ELF"):
                kernel.validate_image_layout(elf, image)

    def audio_fixture(self, root):
        source, cache = root / "kernel", root / "cache"
        cache.mkdir()
        for name, target in {"include/soc/internal.h": "drivers/base/regmap/internal.h",
                             "soc/core.h": "drivers/pinctrl/core.h",
                             "soc/pinctrl-utils.h": "drivers/pinctrl/pinctrl-utils.h"}.items():
            header = source / target
            header.parent.mkdir(parents=True, exist_ok=True)
            header.write_text("header\n")
            link = source / "audio-kernel" / name
            link.parent.mkdir(parents=True, exist_ok=True)
            link.symlink_to(header)
        return source, cache

    def test_audio_build_preserves_source_and_uses_kernel_headers(self):
        with tempfile.TemporaryDirectory() as directory:
            source, cache = self.audio_fixture(Path(directory))
            before = kernel.tree_digest(source)
            audio = kernel.prepare_pico_audio(source, cache)
            self.assertEqual(kernel.tree_digest(source), before)
            self.assertEqual((audio / "soc/core.h").resolve(), (source / "drivers/pinctrl/core.h").resolve())
            self.assertTrue((audio / "Kbuild").is_file())
            (audio / "generated.o").write_bytes(b"object")
            next_audio = kernel.prepare_pico_audio(source, cache)
            self.assertNotEqual(audio, next_audio)
            self.assertFalse((next_audio / "generated.o").exists())

    def test_audio_build_rejects_unexpected_header_link(self):
        with tempfile.TemporaryDirectory() as directory:
            source, cache = self.audio_fixture(Path(directory))
            link = source / "audio-kernel/soc/core.h"
            link.unlink()
            link.symlink_to(source / "drivers/pinctrl/pinctrl-utils.h")
            with self.assertRaisesRegex(ValueError, "Unexpected Pico audio header link"):
                kernel.prepare_pico_audio(source, cache)
            self.assertEqual(list(cache.iterdir()), [])

    def test_changed_cached_source_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            pristine, cache = root / "pristine", root / "cache"
            pristine.mkdir()
            cache.mkdir()
            (pristine / "driver.c").write_text("original\n")
            source, digest = kernel.patched_source(pristine, cache, [])
            self.assertEqual(kernel.patched_source(pristine, cache, []), (source, digest))
            (source / "driver.c").write_text("modified\n")
            with self.assertRaisesRegex(ValueError, "source changed"):
                kernel.patched_source(pristine, cache, [])
            self.assertEqual((pristine / "driver.c").read_text(), "original\n")

    def test_symlink_target_and_executable_mode_are_part_of_identity(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "source"
            source.write_text("unchanged bytes")
            link = root / "link"
            link.symlink_to("source")
            original = kernel.tree_digest(root)
            link.unlink()
            link.symlink_to("elsewhere")
            self.assertNotEqual(kernel.tree_digest(root), original)
            link.unlink()
            link.symlink_to("source")
            source.chmod(0o755)
            self.assertNotEqual(kernel.tree_digest(root), original)
