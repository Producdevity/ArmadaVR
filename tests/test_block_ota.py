import hashlib
import importlib.util
import io
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import types
import unittest
import zipfile


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location('block_ota', ROOT / 'tools/extract-block-ota.py')
OTA = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(OTA)
TRANSFER = b'4\n3\n0\n0\nnew 2,2,3\nzero 2,1,2\nnew 2,0,1\nerase 2,3,4\n'
LAYOUT = b'remove_all_groups\nadd_group qti 16384\nadd system qti\nresize system 16384\n'


class BlockTransferTests(unittest.TestCase):
    def test_scattered_new_blocks_and_canonical_discard(self):
        commands, counts = OTA.transfers(TRANSFER, 16384)
        self.assertEqual(counts, {'new': 2, 'zero': 1, 'erase': 1})
        with tempfile.TemporaryFile() as target:
            OTA.reconstruct(io.BytesIO(b'a' * 4096 + b'b' * 4096), target, commands, 16384)
            target.seek(0)
            self.assertEqual(target.read(), b'b' * 4096 + b'\0' * 4096 + b'a' * 4096 + b'\0' * 4096)

    def test_truncated_and_trailing_new_data(self):
        commands, _ = OTA.transfers(TRANSFER, 16384)
        for size in (0, 8191, 8193):
            with self.subTest(size=size), tempfile.TemporaryFile() as target:
                with self.assertRaises(ValueError):
                    OTA.reconstruct(io.BytesIO(b'a' * size), target, commands, 16384)

    def test_rejects_incremental_and_invalid_ranges(self):
        cases = [TRANSFER.replace(b'4\n', b'3\n', 1),
                 TRANSFER.replace(b'3\n', b'4\n', 1),
                 TRANSFER.replace(b'0\n0\n', b'1\n1\n', 1),
                 TRANSFER.replace(b'new 2,2,3', b'move 2,2,3'),
                 TRANSFER.replace(b'new 2,2,3', b'new 3,2,3'),
                 TRANSFER.replace(b'new 2,2,3', b'new 2,3,2'),
                 TRANSFER.replace(b'new 2,2,3', b'new 2,2,5'),
                 TRANSFER.replace(b'new 2,2,3', b'new 2,-1,3'),
                 TRANSFER.replace(b'new 2,2,3', b'new 2,0,1'),
                 TRANSFER.replace(b'new 2,2,3\n', b''),
                 TRANSFER.replace(b'new 2,2,3', b'new 4,2,3,0,1')]
        for data in cases:
            with self.subTest(data=data), self.assertRaises(ValueError):
                OTA.transfers(data, 16384)

    def test_layout_limits(self):
        self.assertEqual(OTA.layout(LAYOUT, 'system'), 16384)
        cases = [LAYOUT.replace(b'remove_all_groups\n', b''),
                 LAYOUT + b'resize system 16384\n', LAYOUT + b'add system qti\n',
                 LAYOUT.replace(b'resize system 16384', b'resize system 16385'),
                 LAYOUT.replace(b'resize system 16384', b'resize system 0'),
                 LAYOUT.replace(b'resize system 16384', b'resize system 8589938688')]
        for data in cases:
            with self.subTest(data=data), self.assertRaises(ValueError):
                OTA.layout(data, 'system')


@unittest.skipUnless(shutil.which('brotli'), 'brotli command required for full reconstruction')
class BlockOtaTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.directory = Path(self.tmp.name)
        self.archive = self.directory / 'ota.zip'
        self.compressed = subprocess.check_output(['brotli', '--stdout'], input=b'a' * 4096 + b'b' * 4096)
        self.make_archive()

    def make_archive(self, compressed=None, patch=b''):
        with zipfile.ZipFile(self.archive, 'w') as archive:
            archive.writestr('META-INF/com/android/metadata', 'ota-type=BLOCK\npre-device=test\npost-build-incremental=b1\n')
            archive.writestr('dynamic_partitions_op_list', LAYOUT)
            archive.writestr('system.transfer.list', TRANSFER)
            archive.writestr('system.new.dat.br', self.compressed if compressed is None else compressed)
            archive.writestr('system.patch.dat', patch)
        self.args = types.SimpleNamespace(ota=self.archive, partition='system', output=self.directory / 'result',
                                         expected_sha256=hashlib.sha256(self.archive.read_bytes()).hexdigest(),
                                         expected_device='test', expected_build='b1', brotli=shutil.which('brotli'))

    def test_complete_zip_stream_reconstruction(self):
        OTA.run(self.args)
        image = (self.args.output / 'system.img').read_bytes()
        self.assertEqual(image, b'b' * 4096 + b'\0' * 4096 + b'a' * 4096 + b'\0' * 4096)
        receipt = json.loads((self.args.output / 'reconstruction.json').read_text())
        self.assertEqual(receipt['image_sha256'], hashlib.sha256(image).hexdigest())
        self.assertFalse(receipt['avb_verified'])
        self.assertFalse(receipt['restore_verified'])

    def test_hash_and_identity_mismatch_leave_no_output(self):
        for field, bad in [('expected_sha256', '0' * 64), ('expected_device', 'other'),
                           ('expected_build', 'b2'), ('partition', '../system')]:
            original = getattr(self.args, field)
            setattr(self.args, field, bad)
            with self.subTest(field=field), self.assertRaises(ValueError):
                OTA.run(self.args)
            self.assertFalse(self.args.output.exists())
            setattr(self.args, field, original)

    def test_no_existing_output_or_symlink_input(self):
        self.args.output.mkdir()
        with self.assertRaises(ValueError):
            OTA.run(self.args)
        self.args.output.rmdir()
        self.args.ota = self.directory / 'linked.zip'
        self.args.ota.symlink_to(self.archive)
        with self.assertRaises(OSError):
            OTA.run(self.args)
        self.assertFalse(self.args.output.exists())

    def test_nonregular_input_rejected_without_blocking(self):
        self.args.ota = Path('/dev/null')
        with self.assertRaises(ValueError):
            OTA.run(self.args)
        self.assertFalse(self.args.output.exists())

    def test_nonempty_patch_rejected(self):
        self.make_archive(patch=b'old-data-dependency')
        with self.assertRaises(ValueError):
            OTA.run(self.args)
        self.assertFalse(self.args.output.exists())

    def test_corrupt_brotli_never_publishes_complete_image(self):
        self.make_archive(compressed=b'not a brotli stream')
        with self.assertRaises(ValueError):
            OTA.run(self.args)
        self.assertFalse((self.args.output / 'system.img').exists())
        self.assertFalse((self.args.output / 'reconstruction.json').exists())


if __name__ == '__main__':
    unittest.main()
