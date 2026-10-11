import hashlib
import importlib.util
import json
import os
from pathlib import Path
import stat
import struct
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('systemd_runtime', Path(__file__).parents[1] / 'tools/systemd-runtime.py')
runtime = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runtime)


def elf():
    data = bytearray(64)
    data[:6] = b'\x7fELF\x02\x01'
    struct.pack_into('<H', data, 18, 183)
    struct.pack_into('<Q', data, 32, 64)
    struct.pack_into('<HH', data, 54, 56, 0)
    return bytes(data)


class RuntimeTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.base = Path(self.temp.name)
        self.bundle = self.base / 'bundle'
        self.root = self.base / 'root'
        self.record = {'schema_version': 1, 'scope': 'initramfs', 'status': 'prepared',
                       'prefix': runtime.PREFIX, 'files': [], 'units': [],
                       'commands': [runtime.MANAGER, runtime.EXECUTOR], 'retained_commands': [],
                       'template_sha256': {}, 'launcher_sha256': hashlib.sha256(elf()).hexdigest()}
        for name in self.record['commands']:
            self.add('files/' + name, elf())
            self.add('files/' + runtime.PREFIX + '/' + name, elf())
            self.record['template_sha256'][name] = hashlib.sha256(elf()).hexdigest()
        self.save()

    def tearDown(self):
        self.temp.cleanup()

    def add(self, name, data):
        path = self.bundle / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        path.chmod(0o755)
        self.record['files'].append({'path': name, 'bytes': len(data), 'mode': 0o755,
                                     'sha256': hashlib.sha256(data).hexdigest()})

    def save(self):
        (self.bundle / 'manifest.json').write_text(json.dumps(self.record))

    def test_dracut_emergency_units_are_preserved(self):
        self.assertFalse(runtime.is_systemd_unit(
            b'#  This file is part of dracut.\n[Service]\nExecStart=/usr/bin/dracut-emergency\n'))
        self.assertFalse(runtime.is_systemd_unit(b'[Service]\nExecStart=/custom-handler\n'))
        self.assertTrue(runtime.is_systemd_unit(
            b'#  This file is part of systemd.\n[Service]\nExecStart=/usr/lib/systemd/systemd-sysctl\n'))

    def test_changed_bytes_or_mode_are_rejected(self):
        runtime.validate(self.bundle)
        path = self.bundle / 'files' / runtime.EXECUTOR
        path.chmod(0o644)
        with self.assertRaisesRegex(ValueError, 'mode mismatch'):
            runtime.validate(self.bundle)
        path.chmod(0o755)
        path.write_bytes(b'changed')
        with self.assertRaisesRegex(ValueError, 'checksum'):
            runtime.validate(self.bundle)

    def test_extra_files_are_rejected(self):
        (self.bundle / 'files/extra').write_bytes(b'unknown')
        with self.assertRaisesRegex(ValueError, 'Unexpected systemd runtime files'):
            runtime.validate(self.bundle)

    def test_manifest_cannot_install_arbitrary_root_files(self):
        self.add('files/etc/shadow', b'invalid')
        self.save()
        with self.assertRaisesRegex(ValueError, 'installation path'):
            runtime.validate(self.bundle)

    def test_parent_traversal_is_rejected(self):
        self.record['files'][0]['path'] = 'files/../../outside'
        self.save()
        with self.assertRaisesRegex(ValueError, 'Unsafe runtime path'):
            runtime.validate(self.bundle)

    def test_private_link_cannot_escape(self):
        name = 'files/' + runtime.PREFIX + '/usr/lib/escape'
        path = self.bundle / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.symlink_to('/etc/passwd')
        self.record['files'].append({'path': name, 'link': '/etc/passwd'})
        self.save()
        with self.assertRaisesRegex(ValueError, 'private runtime link'):
            runtime.validate(self.bundle)

    def test_standalone_executor_is_required(self):
        self.record['commands'].remove(runtime.EXECUTOR)
        del self.record['template_sha256'][runtime.EXECUTOR]
        self.record['files'] = [i for i in self.record['files'] if not i['path'].endswith(runtime.EXECUTOR)]
        for p in (self.bundle / 'files').rglob('systemd-executor'):
            p.unlink()
        self.save()
        with self.assertRaisesRegex(ValueError, 'manager or executor'):
            runtime.validate(self.bundle)

    def test_full_root_is_not_accepted_as_initramfs(self):
        self.root.mkdir()
        with self.assertRaisesRegex(ValueError, 'initramfs staging'):
            runtime.install(self.bundle, self.root)

    def test_installed_bytes_are_checked_again(self):
        for item in self.record['files']:
            p = self.root / item['path'].removeprefix('files/')
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_bytes(elf())
            p.chmod(0o755)
        metadata = self.root / 'usr/share/armada-vr/initramfs-systemd.json'
        metadata.parent.mkdir(parents=True)
        metadata.write_bytes((self.bundle / 'manifest.json').read_bytes())
        runtime.verify_install(self.bundle, self.root)
        (self.root / runtime.EXECUTOR).write_bytes(b'broken executor')
        with self.assertRaisesRegex(ValueError, 'Installed runtime file mismatch'):
            runtime.verify_install(self.bundle, self.root)

    def test_install_breaks_dracut_hardlinks_before_replacing_units(self):
        (self.root / 'etc').mkdir(parents=True)
        (self.root / 'etc/initrd-release').write_text('test')
        for name in self.record['commands']:
            p = self.root / name
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_bytes(elf())
        first = 'usr/lib/systemd/system/emergency.service'
        second = 'usr/lib/systemd/system/rescue.service'
        (self.root / first).parent.mkdir(parents=True)
        (self.root / first).write_bytes(b'original')
        os.link(self.root / first, self.root / second)
        os.link(self.root / first, self.root / 'unrelated-alias')
        for name, content in ((first, b'emergency'), (second, b'rescue')):
            self.record['units'].append(name)
            self.record['template_sha256'][name] = hashlib.sha256(b'original').hexdigest()
            self.add('files/' + name, content)
        self.save()
        with patch.object(runtime, 'commands', return_value=self.record['commands']):
            runtime.install(self.bundle, self.root)
        self.assertEqual((self.root / first).read_bytes(), b'emergency')
        self.assertEqual((self.root / second).read_bytes(), b'rescue')
        self.assertEqual((self.root / 'unrelated-alias').read_bytes(), b'original')

    def test_invalid_elf_bounds_and_architecture_are_rejected(self):
        self.assertEqual(runtime.needed(elf()), [])
        data = bytearray(elf())
        struct.pack_into('<H', data, 18, 62)
        with self.assertRaisesRegex(ValueError, 'ARM64'):
            runtime.needed(data)
        data = bytearray(elf())
        struct.pack_into('<H', data, 56, 1)
        with self.assertRaisesRegex(ValueError, 'program headers'):
            runtime.needed(data)


if __name__ == '__main__':
    unittest.main()
