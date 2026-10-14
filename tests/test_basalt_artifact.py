import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('basalt_artifact', Path(__file__).parents[1] / 'tools/basalt-artifact.py')
basalt = importlib.util.module_from_spec(spec)
spec.loader.exec_module(basalt)


class BasaltInstallTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.base = Path(self.temporary.name)
        self.bundle = self.base / 'bundle'
        self.prefix = self.bundle / basalt.PREFIX
        (self.prefix / 'lib64').mkdir(parents=True)
        (self.prefix / 'lib64/libbasalt.so.2').write_bytes(b'library fixture')
        (self.prefix / 'lib64/libbasalt.so').symlink_to('libbasalt.so.2')
        (self.bundle / 'build.json').write_text(json.dumps({'fixture': True}))
        self.record = {'build_sha256': basalt.digest(self.bundle / 'build.json'), 'contents': basalt.contents(self.prefix)}
        self.root = self.base / 'root'
        self.root.mkdir()

    def test_install_and_readback_preserve_links_and_refuse_overwrite(self):
        basalt.install(self.bundle, self.root, self.record)
        basalt.verify_install(self.root, self.record)
        self.assertTrue((self.root / basalt.INSTALL_PREFIX / 'lib64/libbasalt.so').is_symlink())
        with self.assertRaisesRegex(ValueError, 'already supplies'):
            basalt.install(self.bundle, self.root, self.record)
        self.assertFalse((self.root / 'etc').exists())

    def test_changed_input_is_rejected_before_installation(self):
        (self.prefix / 'lib64/libbasalt.so.2').write_bytes(b'changed')
        with self.assertRaisesRegex(ValueError, 'changed before installation'):
            basalt.install(self.bundle, self.root, self.record)
        self.assertEqual(list(self.root.iterdir()), [])

    def test_symlink_cannot_redirect_installation(self):
        outside = self.base / 'outside'
        outside.mkdir()
        (self.root / 'opt').symlink_to(outside, target_is_directory=True)
        with self.assertRaisesRegex(ValueError, 'symlink'):
            basalt.install(self.bundle, self.root, self.record)
        self.assertEqual(list(outside.iterdir()), [])

    def test_extra_file_or_changed_mode_is_rejected(self):
        basalt.install(self.bundle, self.root, self.record)
        prefix = self.root / basalt.INSTALL_PREFIX
        extra = prefix / 'unexpected'
        extra.write_text('extra')
        with self.assertRaisesRegex(ValueError, 'contents or modes differ'):
            basalt.verify_install(self.root, self.record)
        extra.unlink()
        library = prefix / 'lib64/libbasalt.so.2'
        library.chmod(0o755)
        with self.assertRaisesRegex(ValueError, 'contents or modes differ'):
            basalt.verify_install(self.root, self.record)

    def test_metadata_and_escaping_link_are_rejected(self):
        basalt.install(self.bundle, self.root, self.record)
        (self.root / basalt.BUILD_RECORD).write_text('changed')
        with self.assertRaisesRegex(ValueError, 'build record differs'):
            basalt.verify_install(self.root, self.record)
        link = self.prefix / 'lib64/libbasalt.so'
        link.unlink()
        link.symlink_to('/etc/passwd')
        with self.assertRaisesRegex(ValueError, 'escapes'):
            basalt.contents(self.prefix)


if __name__ == '__main__':
    unittest.main()
