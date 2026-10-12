import importlib.util
import hashlib
import io
from pathlib import Path
import tarfile
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("fastrpc_build", ROOT / "tools/fastrpc-build.py")
builder = importlib.util.module_from_spec(spec)
spec.loader.exec_module(builder)


class FastRPCSourceTests(unittest.TestCase):
    def test_dependency_integrity_checks_every_bundled_file(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            library = root / "library"
            library.write_bytes(b"library bytes")
            manifest = root / "md5sums"
            manifest.write_text(hashlib.md5(library.read_bytes()).hexdigest() + "  usr/lib/library\n")
            files = {"/usr/lib/library": library}
            result = builder.verify_dependency_files(manifest, files)
            self.assertEqual(result["/usr/lib/library"]["sha256"], builder.digest(library))
            with self.assertRaises(ValueError):
                builder.verify_dependency_files(manifest, {"/usr/lib/unlisted": library})
            library.write_bytes(b"modified bytes")
            with self.assertRaises(ValueError):
                builder.verify_dependency_files(manifest, files)
            library.unlink()
            with self.assertRaises(FileNotFoundError):
                builder.verify_dependency_files(manifest, files)

    def archive(self, path, entries):
        with tarfile.open(path, "w") as stream:
            for name, kind in entries:
                entry = tarfile.TarInfo(name)
                entry.type = kind
                entry.linkname = "target" if kind in (tarfile.SYMTYPE, tarfile.LNKTYPE) else ""
                entry.mode = 0o6755
                entry.size = 4 if kind == tarfile.REGTYPE else 0
                stream.addfile(entry, io.BytesIO(b"data") if entry.size else None)

    def test_extracts_sources_without_privileged_modes(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.archive(root / "source.tar", [("source", tarfile.DIRTYPE),
                                                ("source/build", tarfile.REGTYPE)])
            output = root / "output"
            output.mkdir()
            builder.extract_source(root / "source.tar", output)
            self.assertEqual((output / "source/build").read_bytes(), b"data")
            self.assertEqual((output / "source/build").stat().st_mode & 0o7777, 0o755)

    def test_rejects_links_traversal_devices_and_duplicates_before_writing(self):
        bad = [
            [("../escape", tarfile.REGTYPE)], [("/escape", tarfile.REGTYPE)],
            [("source/link", tarfile.SYMTYPE)], [("source/link", tarfile.LNKTYPE)],
            [("source/device", tarfile.CHRTYPE)],
            [("source/file", tarfile.REGTYPE), ("source/./file", tarfile.REGTYPE)],
        ]
        for entries in bad:
            with self.subTest(entries=entries), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                self.archive(root / "source.tar", entries)
                output = root / "output"
                output.mkdir()
                with self.assertRaises(ValueError):
                    builder.extract_source(root / "source.tar", output)
                self.assertEqual(list(output.iterdir()), [])
