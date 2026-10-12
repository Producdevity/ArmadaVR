import copy
import importlib.util
from pathlib import Path
import struct
import sys
import tempfile
import unittest

from test_quest_firmware import mdt

ROOT = Path(__file__).parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
spec = importlib.util.spec_from_file_location('pico_dsp_memory', ROOT / 'tools/verify-pico-dsp-memory.py')
verify = importlib.util.module_from_spec(spec)
spec.loader.exec_module(verify)


def cells(*values):
    return struct.pack('>' + 'I' * len(values), *values)


def relocatable():
    data = mdt()
    struct.pack_into('<I', data, 24, 4096)
    struct.pack_into('<I', data, 52 + 64 + 24, 1 << 27)
    return data


class PicoDSPMemoryTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.files = Path(self.temp.name)
        self.nodes = {'/': {'compatible': b'qcom,kona\0'}, '/soc': {},
                      '/reserved-memory': {'#address-cells': cells(2), '#size-cells': cells(2), 'ranges': b''}}
        self.memory = {'reserved': []}
        for index, (name, pas) in enumerate((('adsp', 1), ('cdsp', 18), ('slpi', 12)), 1):
            path = '/reserved-memory/' + name
            address = 0x88000000 + index * 0x200000
            self.nodes[path] = {'compatible': b'removed-dma-pool\0', 'no-map': b'',
                                'reg': struct.pack('>QQ', address, 4096), 'phandle': cells(index)}
            self.nodes['/soc/' + name] = {'compatible': b'qcom,pil-tz-generic\0',
                'qcom,firmware-name': (name + '\0').encode(), 'qcom,pas-id': cells(pas), 'memory-region': cells(index)}
            self.memory['reserved'].append({'path': path, 'start': address, 'size': 4096})
            (self.files / (name + '.mdt')).write_bytes(relocatable())

    def test_relocation_entry_and_allocation_fit_without_mutating_input(self):
        original = copy.deepcopy(self.nodes)
        result = verify.contract(self.nodes, self.memory, self.files)
        self.assertEqual(self.nodes, original)
        self.assertEqual([r['processor'] for r in result], ['adsp', 'cdsp', 'slpi'])
        for item in result:
            self.assertEqual(item['allocation_bytes'], 4096)
            self.assertEqual(item['unused_bytes'], 0)
            self.assertEqual(item['relocated_entry'], item['start'])

    def test_wrong_board_authentication_or_disabled_dependency(self):
        for path, key, value in (('/', 'compatible', b'qcom,kalama\0'),
                                 ('/soc', 'status', b'disabled\0'),
                                 ('/soc/slpi', 'qcom,pas-id', cells(1)),
                                 ('/soc/adsp', 'qcom,pil-no-auth', b''),
                                 ('/reserved-memory/cdsp', 'status', b'disabled\0')):
            nodes = copy.deepcopy(self.nodes)
            nodes[path][key] = value
            with self.subTest(path=path, key=key), self.assertRaises(ValueError):
                verify.contract(nodes, self.memory, self.files)

    def test_missing_duplicate_or_shared_reservation_is_rejected(self):
        for path, key, value in (('/soc/slpi', 'memory-region', cells(99)),
                                 ('/soc/cdsp', 'memory-region', cells(1)),
                                 ('/reserved-memory/cdsp', 'phandle', cells(1)),
                                 ('/reserved-memory/adsp', 'compatible', b'shared-dma-pool\0'),
                                 ('/reserved-memory/adsp', 'reusable', b'')):
            nodes = copy.deepcopy(self.nodes)
            nodes[path][key] = value
            with self.subTest(path=path, key=key), self.assertRaises(ValueError):
                verify.contract(nodes, self.memory, self.files)
        self.memory['reserved'].append({'path': 'another reservation', 'start': 0x88200000, 'size': 4096})
        with self.assertRaisesRegex(ValueError, 'overlaps'):
            verify.contract(self.nodes, self.memory, self.files)

    def test_rounded_allocation_cannot_exceed_reservation(self):
        data = relocatable()
        struct.pack_into('<I', data, 52 + 64 + 20, 4097)
        (self.files / 'adsp.mdt').write_bytes(data)
        with self.assertRaisesRegex(ValueError, 'does not fit'):
            verify.contract(self.nodes, self.memory, self.files)

    def test_bad_entry_alignment_and_nonrelocatable_segment(self):
        for offset, value, message in ((24, 0, 'entry'), (52 + 64 + 28, 4097, 'alignment'),
                                       (52 + 64 + 24, 0, 'Non-relocatable')):
            data = relocatable()
            struct.pack_into('<I', data, offset, value)
            with self.subTest(offset=offset), self.assertRaisesRegex(ValueError, message):
                verify.segment_layout(data, 'adsp')

    def test_overlapping_segments_are_rejected(self):
        data = relocatable()
        data += data[-32:]
        struct.pack_into('<H', data, 44, 4)
        struct.pack_into('<II', data, 52 + 16, len(data), len(data))
        with self.assertRaisesRegex(ValueError, 'Overlapping'):
            verify.segment_layout(data, 'adsp')
