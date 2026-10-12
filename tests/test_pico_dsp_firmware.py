import importlib.util
import json
from pathlib import Path
import struct
import tempfile
import unittest

from test_quest_firmware import mdt

spec = importlib.util.spec_from_file_location('pico_dsp', Path(__file__).parents[1] / 'tools/prepare-pico-dsp.py')
dsp = importlib.util.module_from_spec(spec)
spec.loader.exec_module(dsp)


def fat_image():
    sector, fat_sectors, root_entries = 4096, 3, 128
    start = (1 + 2 * fat_sectors + 1) * sector
    data = bytearray(start + sector)
    struct.pack_into('<H', data, 11, sector)
    data[13] = 4
    struct.pack_into('<H', data, 14, 1)
    data[16] = 2
    struct.pack_into('<H', data, 17, root_entries)
    struct.pack_into('<H', data, 19, (start + 4096 * 16384) // sector)
    struct.pack_into('<H', data, 22, fat_sectors)
    data[510:512] = b'\x55\xaa'
    for offset in (sector, sector * (1 + fat_sectors)):
        struct.pack_into('<3H', data, offset, 0xfff8, 0xffff, 0xffff)
    root = sector * (1 + 2 * fat_sectors)
    data[root:root + 11] = b'FIRMWAREBIN'
    data[root + 11] = 32
    struct.pack_into('<H', data, root + 26, 2)
    struct.pack_into('<I', data, root + 28, 4)
    data[start:start + 4] = b'data'
    return data, root, start


class PicoFATTests(unittest.TestCase):
    def test_unallocated_tail_and_last_cluster_slack_need_not_be_present(self):
        data, _, _ = fat_image()
        volume = dsp.FirmwareVolume(data)
        self.assertGreater(volume.declared_bytes, len(data))
        self.assertEqual(volume.read('FIRMWARE.BIN'), b'data')
        self.assertEqual(len(volume.owners), 1)

    def test_file_content_truncation_is_rejected(self):
        data, root, _ = fat_image()
        struct.pack_into('<I', data, root + 28, 4097)
        with self.assertRaisesRegex(ValueError, 'Truncated FAT file'):
            dsp.FirmwareVolume(data)

    def test_fat_copy_disagreement_cycle_and_orphan_are_rejected(self):
        data, _, _ = fat_image()
        data[4096 + 4] = 3
        with self.assertRaisesRegex(ValueError, 'FAT copies disagree'):
            dsp.FirmwareVolume(data)
        for cluster, value, error in ((2, 2, 'cyclic'), (3, 0xffff, 'Unreferenced')):
            data, _, _ = fat_image()
            for offset in (4096, 16384):
                struct.pack_into('<H', data, offset + cluster * 2, value)
            with self.assertRaisesRegex(ValueError, error):
                dsp.FirmwareVolume(data)

    def test_cross_linked_file_is_rejected(self):
        data, root, _ = fat_image()
        data[root + 32:root + 64] = data[root:root + 32]
        data[root + 32:root + 43] = b'OTHER   BIN'
        with self.assertRaisesRegex(ValueError, 'shared FAT chain'):
            dsp.FirmwareVolume(data)

    def test_long_name_sequence_and_checksum(self):
        data, root, _ = fat_image()
        short = bytes(data[root:root + 32])
        data[root + 32:root + 64] = short
        entry = bytearray(32)
        entry[0], entry[11], entry[13] = 0x41, 15, dsp.FirmwareVolume.checksum(short[:11])
        text = 'firmware.bin\0'.encode('utf-16le')
        entry[1:11], entry[14:26], entry[28:32] = text[:10], text[10:22], text[22:26]
        data[root:root + 32] = entry
        self.assertEqual(dsp.FirmwareVolume(data).read('firmware.bin'), b'data')
        data[root + 13] ^= 1
        with self.assertRaisesRegex(ValueError, 'long-name sequence mismatch'):
            dsp.FirmwareVolume(data)


class PicoDSPBundleTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name)
        self.files = self.directory / 'files'
        self.files.mkdir()
        for processor in ('adsp', 'cdsp', 'slpi'):
            (self.files / (processor + '.mdt')).write_bytes(mdt() + bytes(16))
            (self.files / (processor + '.b01')).write_bytes(bytes(16))
            (self.files / (processor + '.b02')).write_bytes(b'data')
        for name, (domain, subdomain, instance) in dsp.DOMAIN_FILES.items():
            (self.files / name).write_text(json.dumps({'sr_domain': {'soc': 'msm', 'domain': domain,
                'subdomain': subdomain, 'qmi_instance_id': instance}, 'sr_service': [{'provider': 'tms', 'service': 'servreg'}]}))
        files, domains = dsp.inspect_files(self.files)
        self.record = {'schema_version': 1, 'status': 'prepared', 'component': 'pico-neo3-dsp',
                       'reference_build': 'smartcm.1696861506', 'source_image_sha256': 'a' * 64,
                       'files': files, 'service_domains': domains}
        (self.directory / 'manifest.json').write_text(json.dumps(self.record))

    def test_complete_bundle_and_service_instances(self):
        self.assertEqual(dsp.validate(self.directory), self.record)
        self.assertEqual({i['domain']['qmi_instance_id'] for i in self.record['service_domains']}, {74, 76, 90})

    def test_truncated_segment_and_changed_metadata_are_rejected(self):
        path = self.files / 'adsp.b02'
        path.write_bytes(b'dat')
        with self.assertRaisesRegex(ValueError, 'segment length'):
            dsp.validate(self.directory)
        path.write_bytes(b'data')
        (self.files / 'slpi.b01').write_bytes(b'x' * 16)
        with self.assertRaisesRegex(ValueError, 'signing metadata differs'):
            dsp.validate(self.directory)

    def test_inconsistent_domains_changed_payload_and_symlinks_are_rejected(self):
        path = self.files / 'slpius.jsn'
        data = path.read_text()
        path.write_text(data.replace('sensor_pd', 'audio_pd'))
        with self.assertRaisesRegex(ValueError, 'service domain'):
            dsp.validate(self.directory)
        path.write_text(data)
        (self.files / 'cdsp.b02').write_bytes(b'evil')
        with self.assertRaisesRegex(ValueError, 'bundle changed'):
            dsp.validate(self.directory)
        (self.files / 'cdsp.b02').unlink()
        (self.files / 'cdsp.b02').symlink_to('adsp.b02')
        with self.assertRaisesRegex(ValueError, 'regular file'):
            dsp.validate(self.directory)
