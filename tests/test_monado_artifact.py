import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('monado_artifact', Path(__file__).parents[1] / 'tools/monado-artifact.py')
monado = importlib.util.module_from_spec(spec)
spec.loader.exec_module(monado)


class MonadoInstallTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.base = Path(self.temporary.name)
        self.bundle = self.base / 'bundle'
        prefix = self.bundle / monado.PREFIX
        (prefix / 'lib64').mkdir(parents=True)
        (prefix / 'bin').mkdir()
        (prefix / 'lib64/libmonado.so.25').write_bytes(b'library fixture')
        (prefix / 'lib64/libmonado.so').symlink_to('libmonado.so.25')
        (prefix / 'bin/monado-service').write_bytes(b'executable fixture')
        (prefix / 'bin/monado-service').chmod(0o755)
        (self.bundle / 'build.json').write_text(json.dumps({'fixture': True}))
        self.record = {'build_sha256': monado.digest(self.bundle / 'build.json'), 'contents': monado.contents(prefix)}
        self.root = self.base / 'root'
        self.root.mkdir()

    def test_copy_preserves_links_modes_and_leaves_existing_runtime_selection(self):
        active = self.root / 'usr/share/openxr/1/active_runtime.json'
        active.parent.mkdir(parents=True)
        active.write_text('existing runtime')
        monado.install(self.bundle, self.root, self.record)
        monado.verify_install(self.root, self.record)
        self.assertEqual(active.read_text(), 'existing runtime')
        self.assertTrue((self.root / monado.INSTALL_PREFIX / 'lib64/libmonado.so').is_symlink())
        with self.assertRaisesRegex(ValueError, 'already supplies'):
            monado.install(self.bundle, self.root, self.record)

    def test_changed_bundle_is_rejected_before_copying(self):
        (self.bundle / monado.PREFIX / 'bin/monado-service').write_bytes(b'changed')
        with self.assertRaisesRegex(ValueError, 'changed before installation'):
            monado.install(self.bundle, self.root, self.record)
        self.assertEqual(list(self.root.iterdir()), [])

    def test_symlink_ancestor_cannot_redirect_install(self):
        outside = self.base / 'outside'
        outside.mkdir()
        (self.root / 'opt').symlink_to(outside, target_is_directory=True)
        with self.assertRaisesRegex(ValueError, 'contains a symlink'):
            monado.install(self.bundle, self.root, self.record)
        self.assertEqual(list(outside.iterdir()), [])

    def test_installed_extra_file_mode_change_and_metadata_corruption_are_rejected(self):
        monado.install(self.bundle, self.root, self.record)
        prefix = self.root / monado.INSTALL_PREFIX
        extra = prefix / 'extra'
        extra.write_text('unexpected')
        with self.assertRaisesRegex(ValueError, 'inventory'):
            monado.verify_install(self.root, self.record)
        extra.unlink()
        service = prefix / 'bin/monado-service'
        service.chmod(0o644)
        with self.assertRaisesRegex(ValueError, 'permissions'):
            monado.verify_install(self.root, self.record)
        service.chmod(0o755)
        (self.root / monado.BUILD_RECORD).write_text('changed')
        with self.assertRaisesRegex(ValueError, 'build record differs'):
            monado.verify_install(self.root, self.record)

    def test_escaping_or_broken_library_link_is_rejected(self):
        link = self.bundle / monado.PREFIX / 'lib64/libmonado.so'
        link.unlink()
        link.symlink_to('/etc/passwd')
        with self.assertRaisesRegex(ValueError, 'escapes'):
            monado.install(self.bundle, self.root, self.record)
        self.assertEqual(list(self.root.iterdir()), [])

    def test_live_root_is_refused(self):
        with self.assertRaisesRegex(ValueError, 'offline staging root'):
            monado.install(self.bundle, Path('/'), self.record)

    def test_package_refuses_nonexecutable_service_and_privileged_modes(self):
        service = self.bundle / monado.PREFIX / 'bin/monado-service'
        with patch.object(monado, 'validate', return_value={}):
            service.chmod(0o644)
            with self.assertRaisesRegex(ValueError, 'executable mode'):
                monado.package(self.bundle)
            service.chmod(0o4755)
            with self.assertRaisesRegex(ValueError, 'installation permissions'):
                monado.package(self.bundle)
