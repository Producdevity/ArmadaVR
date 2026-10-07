import hashlib
import importlib.util
import io
import os
from pathlib import Path
import sys
import tarfile
import tempfile
import unittest
from unittest.mock import patch

TOOLS = Path(__file__).resolve().parents[1] / "tools"
sys.path.insert(0, str(TOOLS))
spec = importlib.util.spec_from_file_location("kernel_source_archive", TOOLS / "kernel-source.py")
source_tool = importlib.util.module_from_spec(spec)
spec.loader.exec_module(source_tool)
spec = importlib.util.spec_from_file_location("kernel_build", TOOLS / "kernel-build.py")
build_tool = importlib.util.module_from_spec(spec)
spec.loader.exec_module(build_tool)


class KernelArchiveTests(unittest.TestCase):
    def test_modinfo_zero_format_retains_module_abi_and_dependencies(self):
        data = "filename: /kernel/gpu.ko\0name=gpu\0vermagic=5.10.246 SMP aarch64\0depends=iommu,clock\0softdep=pre: regulator\0alias=of:N*T*Ctest,gpu\0"
        metadata = build_tool.module_metadata(data)
        self.assertEqual(metadata["name"], ["gpu"])
        self.assertEqual(metadata["vermagic"], ["5.10.246 SMP aarch64"])
        self.assertEqual(metadata["depends"], ["iommu,clock"])
        self.assertEqual(metadata["softdep"], ["pre: regulator"])
        with self.assertRaises(ValueError):
            build_tool.module_metadata("filename: /kernel/gpu.ko\0")

    def fixture(self, root, entries):
        archive = root / "source.tar.gz"
        with tarfile.open(archive, "w:gz") as stream:
            for name, kind, payload in entries:
                member = tarfile.TarInfo("fixture/" + name)
                member.type = kind
                if kind == tarfile.REGTYPE:
                    member.size = len(payload)
                    member.mode = 0o755
                    stream.addfile(member, io.BytesIO(payload))
                else:
                    member.linkname = payload
                    stream.addfile(member)
        return {"full_source": {"size": archive.stat().st_size,
                                "sha256": hashlib.sha256(archive.read_bytes()).hexdigest(),
                                "archive_root": "fixture"}}, archive

    def test_file_permission_and_exact_symlink_verification(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            profile, archive = self.fixture(root, [("dir", tarfile.DIRTYPE, ""),
                                                    ("file", tarfile.REGTYPE, b"original"),
                                                    ("link", tarfile.SYMTYPE, "dir/.././file")])
            source = root / "source"
            with patch.object(source_tool, "require_case_sensitive"):
                source_tool.full_source(profile, archive, source)
            self.assertEqual(os.readlink(source / "link"), "dir/.././file")
            self.assertEqual(source_tool.full_source(profile, archive, source, True)["entries"], 3)
            (source / "file").chmod(0o644)
            with self.assertRaisesRegex(ValueError, "permission"):
                source_tool.full_source(profile, archive, source, True)
            (source / "file").chmod(0o755)
            (source / "file").write_bytes(b"modified")
            with self.assertRaisesRegex(ValueError, "differs"):
                source_tool.full_source(profile, archive, source, True)

    def test_archive_corruption_rejected_before_extraction(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            profile, archive = self.fixture(root, [("file", tarfile.REGTYPE, b"original")])
            data = bytearray(archive.read_bytes())
            data[-1] ^= 1
            archive.write_bytes(data)
            with self.assertRaisesRegex(ValueError, "checksum"):
                source_tool.full_source(profile, archive, root / "source")
            self.assertFalse((root / "source").exists())

    def test_escape_duplicate_and_special_entries_rejected(self):
        entries = [[("../escape", tarfile.REGTYPE, b"bad")],
                   [("escape", tarfile.SYMTYPE, "../outside")],
                   [("pipe", tarfile.FIFOTYPE, "")],
                   [("file", tarfile.REGTYPE, b"one"), ("file", tarfile.REGTYPE, b"two")]]
        for entry in entries:
            with self.subTest(entry=entry), tempfile.TemporaryDirectory() as temp:
                root = Path(temp)
                profile, archive = self.fixture(root, entry)
                with patch.object(source_tool, "require_case_sensitive"), self.assertRaises(ValueError):
                    source_tool.full_source(profile, archive, root / "source")
                self.assertFalse((root / "escape").exists())

    def test_case_collision_rejected_or_preserved_on_native_filesystem(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / "CaseProbe").touch()
            insensitive = (root / "caseprobe").exists()
            profile, archive = self.fixture(root, [("A.h", tarfile.REGTYPE, b"upper"),
                                                    ("a.h", tarfile.REGTYPE, b"lower")])
            source = root / "source"
            if insensitive:
                with self.assertRaisesRegex(ValueError, "case-sensitive"):
                    source_tool.full_source(profile, archive, source)
                self.assertFalse(source.exists())
            else:
                source_tool.full_source(profile, archive, source)
                source_tool.full_source(profile, archive, source, True)
                self.assertNotEqual((source / "A.h").read_bytes(), (source / "a.h").read_bytes())

    def test_config_changes_include_removed_and_changed_symbols(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            requested, resolved = root / "requested", root / "resolved"
            requested.write_text('CONFIG_KEPT=m\nCONFIG_REMOVED=y\nCONFIG_DEFAULT="zstd"\n# CONFIG_DISABLED is not set\n')
            resolved.write_text('CONFIG_KEPT=m\nCONFIG_DEFAULT="lzo"\nCONFIG_DISABLED=y\n')
            self.assertEqual(build_tool.config_changes(requested, resolved), [
                {"symbol": "CONFIG_DEFAULT", "requested": '"zstd"', "resolved": '"lzo"'},
                {"symbol": "CONFIG_DISABLED", "requested": "n", "resolved": "y"},
                {"symbol": "CONFIG_REMOVED", "requested": "y", "resolved": "n"}])
