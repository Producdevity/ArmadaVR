import importlib.util
from pathlib import Path
import tempfile
import sys
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
spec = importlib.util.spec_from_file_location("kernel_build_memory", ROOT / "tools/kernel-build.py")
build = importlib.util.module_from_spec(spec)
spec.loader.exec_module(build)


class KernelMemoryTests(unittest.TestCase):
    def test_quest_refuses_known_insufficient_limit(self):
        quest = {"repository": "facebookincubator/oculus-linux-kernel"}
        with self.assertRaisesRegex(ValueError, "at least 6 GiB"):
            build.require_build_memory(quest, True, False, 4 * 1024**3)
        build.require_build_memory(quest, True, False, 6 * 1024**3)
        build.require_build_memory(quest, True, False, None)
        build.require_build_memory(quest, True, True, 4 * 1024**3)
        build.require_build_memory({"repository": "bytedance/neo3-kernel"}, True, False, 4 * 1024**3)

    def test_failure_precedes_source_or_output_mutation(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            with patch.object(build.sys, "platform", "linux"), patch.object(build, "cgroup_memory_limit", return_value=4 * 1024**3):
                with self.assertRaisesRegex(ValueError, "at least 6 GiB"):
                    build.build({"repository": "facebookincubator/oculus-linux-kernel"}, root / "cache", root / "output", root / "missing.tar.gz", linux_userspace=True)
            self.assertEqual(list(root.iterdir()), [])
