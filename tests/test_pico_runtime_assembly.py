import importlib.util
import json
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('runtime_assembly', Path(__file__).parents[1] / 'tools/assemble-pico-boot.py')
assembly = importlib.util.module_from_spec(spec)
spec.loader.exec_module(assembly)


class RuntimeAssemblyTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)
        self.kernel, self.initrd, self.root = (self.base / n for n in ('kernel', 'initrd', 'root'))
        for directory in (self.kernel, self.initrd, self.root):
            directory.mkdir()
        (self.kernel / 'build.json').write_text('{}')
        self.hashes = {'Image': 'a' * 64}
        common = {'status': 'built', 'qemu_transports': False, 'kernel_variant': 'linux-userspace',
                  'kernel_build_sha256': assembly.digest(self.kernel / 'build.json'),
                  'kernel_inputs_sha256': self.hashes, 'hardware_boot_verified': False,
                  'hardware_flash_image': False, 'pico_services': True,
                  'systemd_runtime_sha256': 'b' * 64, 'firmware': {'files': []}}
        self.rd = dict(common, pico_inputs_sha256={n: assembly.digest(assembly.ROOT / n)
                                                 for n in assembly.assembly.builder.PICO_INPUTS})
        (self.initrd / 'initramfs.img').write_bytes(b'ramdisk fixture')
        (self.initrd / 'contents.json').write_text('{}')
        self.record = dict(common, profile='pico-neo3', target='headset-root-offline',
                           root_label='reframed-vr-root', initramfs_sha256=assembly.digest(self.initrd / 'initramfs.img'))
        image = bytearray(4096)
        struct.pack_into('<I', image, 1028, 4)
        image[1080:1082] = b'\x53\xef'
        image[1144:1160] = b'reframed-vr-root'.ljust(16, b'\0')
        (self.root / 'rootfs.ext4').write_bytes(image)
        (self.root / 'contents.json').write_text('{}')
        (self.root / 'root-profile.json').write_text(json.dumps({'profile': 'pico-neo3', 'pico_services': True}))
        self.save()
        mocked = patch.object(assembly.diagnostic.diagnostic, 'kernel_input', return_value=self.hashes)
        mocked.start()
        self.addCleanup(mocked.stop)

    def save(self):
        for directory, report, name, paths in (
                (self.initrd, self.rd, 'build.json', ('initramfs.img', 'contents.json')),
                (self.root, self.record, 'manifest.json', ('rootfs.ext4', 'contents.json', 'root-profile.json'))):
            report['artifacts'] = [{'path': n, 'bytes': (directory / n).stat().st_size,
                                    'sha256': assembly.digest(directory / n)} for n in paths]
            if name == 'manifest.json':
                report['sha256'] = {a['path']: a['sha256'] for a in report['artifacts']}
            (directory / name).write_text(json.dumps(report))
        self.expected = assembly.digest(self.root / 'manifest.json')

    def checked(self):
        return assembly.runtime_inputs(self.kernel, self.initrd, self.root, self.expected)

    def test_matching_receipts_bind_the_external_root_and_label(self):
        result = self.checked()
        self.assertEqual(result['root_label'], 'reframed-vr-root')
        self.assertEqual(result['root_image_sha256'], assembly.digest(self.root / 'rootfs.ext4'))

    def test_changed_root_is_rejected(self):
        with (self.root / 'rootfs.ext4').open('ab') as stream:
            stream.write(b'changed')
        with self.assertRaisesRegex(ValueError, 'Artifact changed'):
            self.checked()

    def test_changed_manifest_cannot_rebind_a_different_root(self):
        self.record['root_label'] = 'another-root'
        (self.root / 'manifest.json').write_text(json.dumps(self.record))
        with self.assertRaisesRegex(ValueError, 'manifest checksum'):
            self.checked()

    def test_runtime_identity_mismatches_are_rejected(self):
        for key, value in (('qemu_transports', True), ('kernel_variant', 'qemu-abi'),
                           ('kernel_build_sha256', 'c' * 64), ('kernel_inputs_sha256', {}),
                           ('pico_services', False), ('quest_services', True), ('systemd_runtime_sha256', None),
                           ('hardware_boot_verified', True), ('hardware_flash_image', True)):
            for report in (self.rd, self.record):
                with self.subTest(key=key, root=report is self.record):
                    before = report.copy()
                    report[key] = value
                    self.save()
                    with self.assertRaisesRegex(ValueError, 'matching physical Pico'):
                        self.checked()
                    report.clear(); report.update(before)

    def test_firmware_ramdisk_and_startup_mismatches_are_rejected(self):
        for report, key, value in ((self.record, 'initramfs_sha256', 'd' * 64),
                                   (self.record, 'firmware', {'other': True}),
                                   (self.record, 'profile', 'quest3'),
                                   (self.rd, 'pico_inputs_sha256', {})):
            with self.subTest(key=key):
                before = report.copy()
                report[key] = value
                self.save()
                with self.assertRaisesRegex(ValueError, 'startup inputs differ'):
                    self.checked()
                report.clear(); report.update(before)

    def test_mismatched_root_component_receipt_is_rejected(self):
        self.record['ssc'] = {'build_sha256': 'e' * 64}
        self.save()
        with self.assertRaisesRegex(ValueError, 'component receipts disagree'):
            self.checked()

    def test_actual_ext4_label_and_geometry_are_checked(self):
        path = self.root / 'rootfs.ext4'
        original = path.read_bytes()
        for offset, data in ((1080, b'xx'), (1144, b'other-label'), (1028, struct.pack('<I', 5)),
                              (1048, struct.pack('<I', 100)), (1120, struct.pack('<I', 0x80))):
            with self.subTest(offset=offset):
                changed = bytearray(original)
                changed[offset:offset + len(data)] = data
                if offset == 1120:
                    struct.pack_into('<I', changed, 1360, 1)
                path.write_bytes(changed)
                self.save()
                with self.assertRaisesRegex(ValueError, 'ext4|geometry'):
                    self.checked()
        path.write_bytes(original)

    def test_unsafe_duplicate_missing_and_symlink_artifacts_are_rejected(self):
        for name in ('../elsewhere', '/tmp/elsewhere', 'nested/file', 'rootfs.ext4'):
            with self.subTest(name=name):
                self.save()
                self.record['artifacts'].append(dict(self.record['artifacts'][0], path=name))
                (self.root / 'manifest.json').write_text(json.dumps(self.record))
                self.expected = assembly.digest(self.root / 'manifest.json')
                with self.assertRaisesRegex(ValueError, 'Unsafe or repeated'):
                    self.checked()
        self.save()
        self.record['artifacts'] = self.record['artifacts'][1:]
        (self.root / 'manifest.json').write_text(json.dumps(self.record))
        self.expected = assembly.digest(self.root / 'manifest.json')
        with self.assertRaisesRegex(ValueError, 'Missing required'):
            self.checked()
        self.save()
        path = self.root / 'rootfs.ext4'
        path.rename(self.root / 'original.ext4')
        path.symlink_to('original.ext4')
        with self.assertRaisesRegex(ValueError, 'bounded regular'):
            self.checked()


if __name__ == '__main__':
    unittest.main()
