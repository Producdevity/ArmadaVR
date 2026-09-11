import hashlib
import importlib.util
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch


spec = importlib.util.spec_from_file_location("qcom_sources", Path(__file__).parents[1] / "tools/fetch-qcom-services.py")
sources = importlib.util.module_from_spec(spec)
spec.loader.exec_module(sources)


class QcomSourcesTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        (self.root / "profiles").mkdir()
        self.data = b"pinned source archive"
        profile = {"sources": [{"name": "qrtr", "bytes": len(self.data),
                               "sha256": hashlib.sha256(self.data).hexdigest(), "url": "https://example.test/archive"}]}
        (self.root / "profiles/qcom-services.json").write_text(json.dumps(profile))
        self.cache = self.root / "cache"
        self.cache.mkdir()
        self.archive = self.cache / "qrtr.tar.gz"
        self.root_patch = patch.object(sources, "ROOT", self.root)
        self.root_patch.start()

    def tearDown(self):
        self.root_patch.stop()
        self.temp.cleanup()

    def test_valid_cache_is_reused_without_network(self):
        self.archive.write_bytes(self.data)
        with patch.object(sources.urllib.request, "urlopen", side_effect=AssertionError("unexpected download")):
            sources.fetch(self.cache)
        self.assertEqual(self.archive.read_bytes(), self.data)

    def test_changed_cache_is_rejected_and_preserved(self):
        changed = b"x" + self.data[1:]
        self.archive.write_bytes(changed)
        with patch.object(sources.urllib.request, "urlopen", side_effect=AssertionError("unexpected download")):
            with self.assertRaisesRegex(ValueError, "checksum mismatch"):
                sources.fetch(self.cache)
        self.assertEqual(self.archive.read_bytes(), changed)

    def test_symlink_archive_is_rejected_without_touching_target(self):
        target = self.root / "original"
        target.write_bytes(self.data)
        self.archive.symlink_to(target)
        with self.assertRaisesRegex(ValueError, "non-regular"):
            sources.fetch(self.cache)
        self.assertTrue(self.archive.is_symlink())
        self.assertEqual(target.read_bytes(), self.data)

    def test_bad_downloads_are_not_persisted(self):
        for data in (b"x" + self.data[1:], self.data + b"x", self.data[:-1]):
            with self.subTest(data=data), patch.object(sources.urllib.request, "urlopen", return_value=io.BytesIO(data)):
                with self.assertRaisesRegex(ValueError, "checksum or size mismatch"):
                    sources.fetch(self.cache)
            self.assertFalse(self.archive.exists())

    def test_verified_download_is_saved_exactly(self):
        with patch.object(sources.urllib.request, "urlopen", return_value=io.BytesIO(self.data)) as download:
            sources.fetch(self.cache)
        download.assert_called_once_with("https://example.test/archive", timeout=30)
        self.assertEqual(self.archive.read_bytes(), self.data)
