import importlib.util
from pathlib import Path
import struct
import unittest

spec = importlib.util.spec_from_file_location('wlan_schema', Path(__file__).parents[1] / 'tools/compare-wlan-schema.py')
schema = importlib.util.module_from_spec(spec)
spec.loader.exec_module(schema)


def table(ident=0x3001, size=16, variable=False):
    return struct.pack('<II', (1 << 24) | ident, 7 | (size << 12) | (int(variable) << 30))


def module(relocation=False):
    names = b'\0cmd_attr_list\0evt_attr_list\0'
    symbols = bytes(24) + struct.pack('<IBBHQQ', 1, 17, 0, 3, 0, 8) + struct.pack('<IBBHQQ', 15, 17, 0, 3, 8, 8)
    sections = [bytes(64)]
    start = 64 + 5 * 64
    payload = names + symbols + table() + table(0x4001)
    sections += [struct.pack('<IIQQQQIIQQ', 0, 3, 0, 0, start, len(names), 0, 0, 1, 0),
                 struct.pack('<IIQQQQIIQQ', 0, 2, 0, 0, start + len(names), len(symbols), 1, 0, 8, 24),
                 struct.pack('<IIQQQQIIQQ', 0, 1, 0, 0, start + len(names) + len(symbols), 16, 0, 0, 4, 0)]
    if relocation:
        sections += [struct.pack('<IIQQQQIIQQ', 0, 4, 0, 0, start + len(payload), 24, 2, 3, 8, 24)]
        payload += struct.pack('<QQq', 4, 257, 0)
    else:
        sections += [bytes(64)]
    header = struct.pack('<16sHHIQQQIHHHHHH', b'\x7fELF\x02\x01\x01' + bytes(9),
                         1, 183, 1, 0, 0, 64, 0, 64, 0, 0, 64, 5, 0)
    return bytearray(header + b''.join(sections) + payload)


class WlanSchemaTests(unittest.TestCase):
    def test_extract_tables_without_loading_a_module(self):
        result = schema.elf_tables(module())
        self.assertEqual(result, {'cmd_attr_list': table(), 'evt_attr_list': table(0x4001)})

    def test_relocated_schema_is_not_treated_as_plain_data(self):
        with self.assertRaisesRegex(ValueError, 'runtime relocation'):
            schema.elf_tables(module(True))

    def test_elf_architecture_and_bounds_are_checked(self):
        for offset, fmt, value in ((18, '<H', 62), (40, '<Q', 1 << 30),
                                   (64 + 2 * 64 + 32, '<Q', 1 << 30),
                                   (64 + 2 * 64 + 40, '<I', 99)):
            data = module()
            struct.pack_into(fmt, data, offset, value)
            with self.subTest(offset=offset), self.assertRaises(ValueError):
                schema.elf_tables(data)
        with self.assertRaises(ValueError):
            schema.elf_tables(module()[:-1])

    def test_truncated_duplicate_and_reserved_schema_bits(self):
        for data in (table()[:-1], table()[:4], table() + table(),
                     struct.pack('<II', 0x01003001, 0x80000000)):
            with self.subTest(data=data.hex()), self.assertRaises(ValueError):
                schema.decode_table(data)

    def test_size_flags_and_message_id_differences_are_reported(self):
        original = {'cmd_attr_list': table(), 'evt_attr_list': table(0x4001)}
        for replacement in (table(size=20), table(variable=True)):
            result = schema.compare(original, {**original, 'cmd_attr_list': replacement})
            self.assertEqual(len(result['cmd_attr_list']['changed']), 1)
            self.assertEqual(result['evt_attr_list']['equal_messages'], 1)
        result = schema.compare(original, {**original, 'cmd_attr_list': table(0x3002)})
        self.assertEqual(result['cmd_attr_list']['missing'], [0x3001])
        self.assertEqual(result['cmd_attr_list']['added'], [0x3002])
