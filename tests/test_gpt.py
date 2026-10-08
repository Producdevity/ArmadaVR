import importlib.util
from pathlib import Path
import struct
import tempfile
import unittest
import uuid
import zlib

spec = importlib.util.spec_from_file_location("inspect_gpt", Path(__file__).parents[1] / "tools/inspect-gpt.py")
gpt = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gpt)


class GPTTests(unittest.TestCase):
    def fixture(self, sector_size=4096, modify_entry=None, modify_header=None):
        total, count, entry_size = 4096, 128, 128
        array_sectors = count * entry_size // sector_size
        first, last = 2 + array_sectors, total - 2 - array_sectors
        entries = bytearray(count * entry_size)
        type_guid = uuid.UUID("0fc63daf-8483-4772-8e79-3d69d8477de4").bytes_le
        for index, (name, start, length) in enumerate((("boot", first + 4, 20), ("persist", first + 30, 8))):
            unique = uuid.UUID(int=index + 1).bytes_le
            struct.pack_into("<16s16sQQQ72s", entries, index * entry_size,
                             type_guid, unique, start, start + length - 1, 0,
                             name.encode("utf-16le"))
        if modify_entry:
            modify_entry(entries, first, last)
        crc = zlib.crc32(entries)
        headers = []
        for current, alternate, table in ((1, total - 1, 2), (total - 1, 1, total - 1 - array_sectors)):
            data = bytearray(sector_size)
            struct.pack_into("<8sIIIIQQQQ16sQIII", data, 0, b"EFI PART", 0x10000,
                             92, 0, 0, current, alternate, first, last,
                             uuid.UUID(int=42).bytes_le, table, count, entry_size, crc)
            if modify_header:
                modify_header(data, current)
            struct.pack_into("<I", data, 16, zlib.crc32(data[:92]))
            headers.append(bytes(data))
        return [headers[0], bytes(entries), headers[1], bytes(entries), sector_size, total]

    def test_both_logical_sector_sizes_and_hashes(self):
        for sector_size in (512, 4096):
            with self.subTest(sector_size=sector_size):
                result = gpt.inspect(*self.fixture(sector_size))
                self.assertEqual([entry["name"] for entry in result["partitions"]], ["boot", "persist"])
                self.assertEqual(result["partitions"][0]["bytes"], 20 * sector_size)
                self.assertEqual(result["artifacts"]["primary_entries"], result["artifacts"]["backup_entries"])
                self.assertFalse(result["device_accessed"])
                self.assertFalse(result["restore_verified"])

    def test_corrupt_header_and_array_crc(self):
        for item in range(4):
            with self.subTest(item=item):
                arguments = self.fixture()
                data = bytearray(arguments[item])
                data[40 if item in (0, 2) else 56] ^= 1
                arguments[item] = bytes(data)
                with self.assertRaisesRegex(ValueError, "CRC"):
                    gpt.inspect(*arguments)

    def test_short_headers_and_arrays(self):
        for item in range(4):
            with self.subTest(item=item):
                arguments = self.fixture()
                arguments[item] = arguments[item][:-1]
                with self.assertRaises(ValueError):
                    gpt.inspect(*arguments)

    def test_wrong_capacity_is_not_inferred_from_the_table(self):
        arguments = self.fixture()
        arguments[-1] += 1
        with self.assertRaisesRegex(ValueError, "capacity"):
            gpt.inspect(*arguments)

    def test_invalid_dimensions_and_metadata_even_with_valid_crc(self):
        cases = [(8, "<I", 0x20000), (12, "<I", 91), (20, "<I", 1),
                 (24, "<Q", 8), (80, "<I", 0), (80, "<I", 100000), (84, "<I", 192)]
        for offset, kind, value in cases:
            with self.subTest(offset=offset, value=value):
                arguments = self.fixture(modify_header=lambda data, _: struct.pack_into(kind, data, offset, value))
                with self.assertRaises(ValueError):
                    gpt.inspect(*arguments)

    def test_arrays_cannot_overlap_usable_storage(self):
        for primary in (True, False):
            with self.subTest(primary=primary):
                def move(data, current):
                    if (current == 1) == primary:
                        struct.pack_into("<Q", data, 72, 100)
                with self.assertRaisesRegex(ValueError, "overlaps"):
                    gpt.inspect(*self.fixture(modify_header=move))

    def test_primary_and_backup_disk_identity_must_match(self):
        def different_disk(data, current):
            if current != 1:
                data[56] ^= 1
        with self.assertRaisesRegex(ValueError, "disagree"):
            gpt.inspect(*self.fixture(modify_header=different_disk))

    def test_partition_bounds_and_overlap(self):
        for start, end in ((1, 5), (100, 4095), (100, 90), (10, 20)):
            with self.subTest(start=start, end=end):
                def move(data, *_):
                    struct.pack_into("<QQ", data, 128 + 32, start, end)
                with self.assertRaises(ValueError):
                    gpt.inspect(*self.fixture(modify_entry=move))

    def test_duplicate_or_missing_partition_guids(self):
        for value in (bytes(16), uuid.UUID(int=1).bytes_le):
            with self.subTest(value=value):
                def change(data, *_):
                    data[128 + 16:128 + 32] = value
                with self.assertRaisesRegex(ValueError, "GUID"):
                    gpt.inspect(*self.fixture(modify_entry=change))

    def test_duplicate_names_remain_explicitly_ambiguous(self):
        def same_name(data, *_):
            data[128 + 56:128 + 128] = data[56:128]
        result = gpt.inspect(*self.fixture(modify_entry=same_name))
        self.assertEqual(result["ambiguous_names"], ["boot"])

    def test_invalid_utf16_name(self):
        def invalid_name(data, *_):
            struct.pack_into("<H", data, 56, 0xD800)
        with self.assertRaisesRegex(ValueError, "UTF-16"):
            gpt.inspect(*self.fixture(modify_entry=invalid_name))

    def test_input_limits_and_no_symlink_or_device_reads(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            data = root / "header"
            data.write_bytes(bytes(4096))
            self.assertEqual(len(gpt.bounded_read(data, 4096)), 4096)
            link = root / "link"
            link.symlink_to(data)
            for path, limit in ((data, 512), (link, 4096), (root, 4096), (Path("/dev/null"), 4096)):
                with self.subTest(path=path, limit=limit), self.assertRaises(ValueError):
                    gpt.bounded_read(path, limit)
        for sector_size, total in ((2048, 4096), (4096, 5), (4096, 2**64)):
            with self.subTest(sector_size=sector_size, total=total), self.assertRaises(ValueError):
                gpt.inspect(bytes(), bytes(), bytes(), bytes(), sector_size, total)
