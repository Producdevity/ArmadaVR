import importlib.util
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location("kernel_build", Path(__file__).parents[1] / "tools/kernel-build.py")
kernel = importlib.util.module_from_spec(spec)
spec.loader.exec_module(kernel)


class KernelPatchTests(unittest.TestCase):
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
