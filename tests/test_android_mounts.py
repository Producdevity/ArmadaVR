import importlib.util
import io
from pathlib import Path
import tarfile
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('android_mounts', Path(__file__).parents[1] / 'tools/test-android-mounts.py')
mounts = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mounts)


class AndroidMountTests(unittest.TestCase):
    def test_requires_complete_unambiguous_guest_acceptance(self):
        log = '\n'.join(mounts.MARKERS)
        self.assertTrue(mounts.check_log(log)['persistent_copy_up_and_whiteouts'])
        for marker in mounts.MARKERS:
            with self.subTest(marker=marker):
                with self.assertRaises(RuntimeError):
                    mounts.check_log(log.replace(marker, 'missing'))
                with self.assertRaises(RuntimeError):
                    mounts.check_log(log + '\n' + marker)

    def test_rejects_failure_even_after_pass_markers(self):
        for failure in ('LEPTON_MOUNT_VM_FAIL', 'Kernel panic', 'WARNING:', 'Oops:', 'BUG:'):
            with self.subTest(failure=failure):
                with self.assertRaises(RuntimeError):
                    mounts.check_log('\n'.join(mounts.MARKERS) + '\n' + failure)

    def archive(self, root, extra=None, skip=None):
        path = root / 'dependencies.tar'
        names = ('bin/busybox', 'usr/bin/podman', 'usr/bin/fuse-overlayfs', 'usr/bin/crun',
                 'usr/bin/newuidmap', 'usr/bin/newgidmap', 'packages.txt')
        with tarfile.open(path, 'w') as archive:
            for name in names:
                if name == skip:
                    continue
                data = b'podman-test\n' if name == 'packages.txt' else b'fixture'
                entry = tarfile.TarInfo(name)
                entry.size = len(data)
                archive.addfile(entry, io.BytesIO(data))
            if extra:
                archive.addfile(extra)
        return path

    def test_accepts_bounded_dependencies(self):
        with tempfile.TemporaryDirectory() as directory:
            self.assertEqual(mounts.validate_archive(self.archive(Path(directory))), 'podman-test\n')

    def test_refuses_missing_helper(self):
        with tempfile.TemporaryDirectory() as directory:
            path = self.archive(Path(directory), skip='usr/bin/newuidmap')
            with self.assertRaisesRegex(ValueError, 'Missing dependency'):
                mounts.validate_archive(path)

    def test_refuses_device_nodes_and_escaping_paths(self):
        for name, kind in (('/absolute', tarfile.DIRTYPE), ('../escape', tarfile.DIRTYPE),
                           ('device', tarfile.CHRTYPE)):
            with self.subTest(name=name), tempfile.TemporaryDirectory() as directory:
                entry = tarfile.TarInfo(name)
                entry.type = kind
                with self.assertRaises(ValueError):
                    mounts.validate_archive(self.archive(Path(directory), extra=entry))
