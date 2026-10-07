import importlib.util
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location("kernel_build", Path(__file__).parents[1] / "tools/kernel-build.py")
kernel = importlib.util.module_from_spec(spec)
spec.loader.exec_module(kernel)


class KernelPatchTests(unittest.TestCase):
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
