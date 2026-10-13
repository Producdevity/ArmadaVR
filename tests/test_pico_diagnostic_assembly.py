import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('pico_assembly', Path(__file__).parents[1] / 'tools/assemble-pico-diagnostic.py')
assembly = importlib.util.module_from_spec(spec)
spec.loader.exec_module(assembly)


class DiagnosticInputsTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        data = bytearray(120)
        data[:7] = b'\x7fELF\x02\x01\x01'
        struct.pack_into('<HH', data, 16, 2, 183)
        struct.pack_into('<Q', data, 32, 64)
        struct.pack_into('<HH', data, 54, 56, 1)
        struct.pack_into('<I', data, 64, 1)
        (self.root / 'init').write_bytes(data)
        (self.root / 'initramfs.cpio').write_bytes(assembly.diagnostic.archive(data, 'device123'))
        (self.root / 'test-initramfs.cpio').write_bytes(assembly.diagnostic.archive(data, 'qemu'))
        self.hashes = {'Image': 'a' * 64}
        self.report = {'status': 'built', 'flash_ready': False, 'physical_boot_verified': False,
                       'kernel_build_sha256': 'b' * 64, 'kernel_inputs': self.hashes,
                       'source_sha256': assembly.digest(assembly.diagnostic.SOURCE),
                       'cmdline': assembly.CMDLINE, 'boot_serial': 'device123'}
        self.save()

    def save(self):
        self.report['artifacts'] = [{'path': name, 'bytes': (self.root / name).stat().st_size,
                                     'sha256': assembly.digest(self.root / name)}
                                    for name in ('init', 'initramfs.cpio', 'test-initramfs.cpio')]
        (self.root / 'build.json').write_text(json.dumps(self.report))

    def checked(self):
        return assembly.diagnostic_inputs(self.root, 'b' * 64, self.hashes)

    def test_physical_archive_is_reconstructed_from_the_checked_binary_and_serial(self):
        self.assertEqual(self.checked(), self.report)

    def test_changed_artifact_bytes_are_rejected(self):
        (self.root / 'init').write_bytes(b'changed')
        with self.assertRaisesRegex(ValueError, 'artifact changed'):
            self.checked()

    def test_test_ramdisk_cannot_replace_physical_even_with_matching_manifest(self):
        (self.root / 'initramfs.cpio').write_bytes((self.root / 'test-initramfs.cpio').read_bytes())
        self.save()
        with self.assertRaisesRegex(ValueError, 'physical serial'):
            self.checked()

    def test_extra_ramdisk_payload_is_rejected_even_with_matching_hash(self):
        with (self.root / 'initramfs.cpio').open('ab') as stream:
            stream.write(b'extra payload')
        self.save()
        with self.assertRaisesRegex(ValueError, 'ramdisk'):
            self.checked()

    def test_kernel_source_and_guard_mismatches_are_rejected(self):
        for key, value in (('kernel_build_sha256', 'c' * 64), ('kernel_inputs', {}),
                           ('source_sha256', 'd' * 64), ('flash_ready', True),
                           ('physical_boot_verified', True), ('cmdline', 'rdinit=/init'),
                           ('cmdline', assembly.CMDLINE + ' msm_drm.modeset=1')):
            with self.subTest(key=key):
                old = self.report[key]; self.report[key] = value; self.save()
                with self.assertRaisesRegex(ValueError, 'identity, kernel or display guard'):
                    self.checked()
                self.report[key] = old

    def test_qemu_cannot_be_the_physical_serial(self):
        self.report['boot_serial'] = 'qemu'; self.save()
        with self.assertRaisesRegex(ValueError, 'ro.boot.serialno'):
            self.checked()

    def test_duplicate_inventory_is_rejected(self):
        self.report['artifacts'][2] = self.report['artifacts'][1]
        (self.root / 'build.json').write_text(json.dumps(self.report))
        with self.assertRaisesRegex(ValueError, 'inventory'):
            self.checked()

    def test_symlinked_input_is_rejected(self):
        (self.root / 'init').rename(self.root / 'binary')
        (self.root / 'init').symlink_to('binary')
        with self.assertRaisesRegex(ValueError, 'regular file'):
            self.checked()


class LegacyHeaderTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.reference, self.output = self.root / 'stock', self.root / 'new'
        self.header = bytearray(1660)
        struct.pack_into('<I', self.header, 40, 2)
        self.reference.write_bytes(self.header)
        self.output.write_bytes(self.header)

    def test_only_the_unused_second_address_is_preserved(self):
        struct.pack_into('<I', self.header, 28, 0x12340000)
        self.reference.write_bytes(self.header)
        assembly.preserve_second_address(self.reference, self.output)
        self.assertEqual(self.output.read_bytes(), self.header)

    def test_live_second_payload_wrong_version_or_existing_address_is_refused(self):
        for offset, value in ((24, 1), (28, 1), (40, 3)):
            data = self.header.copy(); struct.pack_into('<I', data, offset, value)
            self.output.write_bytes(data)
            before = hashlib.sha256(data).hexdigest()
            with self.subTest(offset=offset), self.assertRaisesRegex(ValueError, 'empty second payload'):
                assembly.preserve_second_address(self.reference, self.output)
            self.assertEqual(assembly.digest(self.output), before)

    def test_truncated_header_is_refused(self):
        self.output.write_bytes(b'ANDROID!')
        with self.assertRaisesRegex(ValueError, 'empty second payload'):
            assembly.preserve_second_address(self.reference, self.output)
