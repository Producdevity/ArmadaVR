import importlib.util
import tempfile
import stat
import unittest
import zipfile
from pathlib import Path

spec = importlib.util.spec_from_file_location("fetch_steam", Path(__file__).parents[1] / "tools/fetch-steam.py")
steam = importlib.util.module_from_spec(spec)
spec.loader.exec_module(steam)


class ArchiveTests(unittest.TestCase):
    def test_allows_parent_symlink_only_within_install_root(self):
        for link, allowed in (("../library.so", True), ("../../../escape", False)):
            with self.subTest(link=link), tempfile.TemporaryDirectory() as directory:
                root = Path(directory) / "root"
                root.mkdir()
                archive = Path(directory) / "test.zip"
                item = zipfile.ZipInfo("libs/sub/link")
                item.external_attr = (stat.S_IFLNK | 0o777) << 16
                with zipfile.ZipFile(archive, "w") as z:
                    z.writestr(item, link)
                if allowed:
                    steam.unpack(archive, root)
                    self.assertTrue((root / "libs/sub/link").is_symlink())
                else:
                    with self.assertRaises(ValueError):
                        steam.unpack(archive, root)

    def test_normalizes_valve_archive_separators(self):
        self.assertEqual(str(steam.member_path("steamrtarm64\\libs/libcurl.so")), "steamrtarm64/libs/libcurl.so")

    def test_rejects_escape_paths(self):
        for name in ("../escape", "/absolute", "C:\\escape", "safe\\..\\escape"):
            with self.subTest(name=name), self.assertRaises(ValueError):
                steam.member_path(name)

    def test_does_not_extract_through_existing_symlink(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "root"
            root.mkdir()
            (root / "link").symlink_to(Path(directory), target_is_directory=True)
            archive = Path(directory) / "test.zip"
            with zipfile.ZipFile(archive, "w") as z:
                z.writestr("link/escaped", "contents")
            with self.assertRaises(ValueError):
                steam.unpack(archive, root)
            self.assertFalse((Path(directory) / "escaped").exists())
