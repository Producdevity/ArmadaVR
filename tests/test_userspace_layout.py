import importlib.util
from pathlib import Path
import tempfile
import unittest


spec = importlib.util.spec_from_file_location('userspace_layout', Path(__file__).parents[1] / 'tools/userspace-layout.py')
layout = importlib.util.module_from_spec(spec)
spec.loader.exec_module(layout)


class UserspaceLayoutTests(unittest.TestCase):
    def test_current_paths_and_upstream_credits_are_accepted(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for name in ('opt/reframed-vr', 'usr/share/reframed-vr', 'usr/share/licenses/armada-packages'):
                (root / name).mkdir(parents=True)
            layout.validate(root)

    def test_legacy_runtime_or_enabled_service_is_refused_without_changes(self):
        for name in ('opt/armada-vr', 'usr/share/armada-vr',
                     'usr/libexec/armada-quest-boot',
                     'etc/systemd/system/armada-vr-lab.service',
                     'etc/systemd/system/multi-user.target.wants/armada-vr-lab.service'):
            with self.subTest(name=name), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                path = root / name
                path.parent.mkdir(parents=True)
                path.symlink_to('/missing-original-service')
                with self.assertRaisesRegex(ValueError, 'Pre-rename userspace'):
                    layout.validate(root)
                self.assertEqual(str(path.readlink()), '/missing-original-service')
