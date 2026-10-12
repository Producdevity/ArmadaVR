import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('ssc_artifact', Path(__file__).parents[1] / 'tools/ssc-artifact.py')
ssc = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ssc)


class SSCInstallTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)
        self.bundle = self.base / 'bundle'
        self.prefix = self.bundle / ssc.PREFIX
        (self.prefix / 'lib64').mkdir(parents=True)
        (self.prefix / 'bin').mkdir()
        (self.prefix / 'lib64/libssc.so.2').write_bytes(b'library fixture')
        (self.prefix / 'lib64/libssc.so').symlink_to('libssc.so.2')
        (self.prefix / 'bin/ssccli').write_bytes(b'cli fixture')
        (self.prefix / 'bin/ssccli').chmod(0o755)
        (self.bundle / 'build.json').write_text(json.dumps({'fixture': True}))
        self.record = {'build_sha256': ssc.digest(self.bundle / 'build.json'), 'contents': ssc.contents(self.prefix)}
        self.root = self.base / 'root'
        self.root.mkdir()

    def test_install_preserves_links_without_enabling_services(self):
        ssc.install(self.bundle, self.root, self.record)
        ssc.verify_install(self.root, self.record)
        self.assertFalse((self.root / 'etc').exists())
        self.assertEqual((self.root / ssc.INSTALL_PREFIX / 'lib64/libssc.so').readlink(), Path('libssc.so.2'))
        with self.assertRaisesRegex(ValueError, 'already supplies'):
            ssc.install(self.bundle, self.root, self.record)

    def test_changes_are_refused_before_writing(self):
        (self.prefix / 'bin/ssccli').write_bytes(b'changed')
        with self.assertRaisesRegex(ValueError, 'changed before installation'):
            ssc.install(self.bundle, self.root, self.record)
        self.assertEqual(list(self.root.iterdir()), [])

    def test_destination_symlink_is_refused(self):
        outside = self.base / 'outside'
        outside.mkdir()
        (self.root / 'opt').symlink_to(outside)
        with self.assertRaisesRegex(ValueError, 'contains a symlink'):
            ssc.install(self.bundle, self.root, self.record)
        self.assertEqual(list(outside.iterdir()), [])

    def test_broken_or_escaping_bundle_link_is_refused(self):
        link = self.prefix / 'lib64/libssc.so'
        link.unlink()
        for target in ('missing', '/etc/passwd', '../../../../../../etc/passwd'):
            link.symlink_to(target)
            with self.assertRaisesRegex(ValueError, 'escapes.*or is broken'):
                ssc.install(self.bundle, self.root, self.record)
            link.unlink()
        self.assertEqual(list(self.root.iterdir()), [])

    def test_privileged_mode_and_installed_corruption_are_refused(self):
        binary = self.prefix / 'bin/ssccli'
        binary.chmod(0o4755)
        with self.assertRaisesRegex(ValueError, 'file mode'):
            ssc.install(self.bundle, self.root, self.record)
        binary.chmod(0o755)
        ssc.install(self.bundle, self.root, self.record)
        (self.root / ssc.INSTALL_PREFIX / 'bin/ssccli').chmod(0o644)
        with self.assertRaisesRegex(ValueError, 'modes or hashes'):
            ssc.verify_install(self.root, self.record)
        (self.root / ssc.INSTALL_PREFIX / 'bin/ssccli').chmod(0o755)
        (self.root / ssc.BUILD_RECORD).write_text('corrupted')
        with self.assertRaisesRegex(ValueError, 'build record'):
            ssc.verify_install(self.root, self.record)

    def test_live_root_and_symlink_root_are_refused(self):
        alias = self.base / 'alias'
        alias.symlink_to(self.root)
        for root in (Path('/'), alias):
            with self.assertRaisesRegex(ValueError, 'offline staging root'):
                ssc.install(self.bundle, root, self.record)
