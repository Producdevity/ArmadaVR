import importlib.util
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
spec = importlib.util.spec_from_file_location("dtbo_repack", ROOT / "tools/repack-dtbo.py")
dtbo = importlib.util.module_from_spec(spec)
spec.loader.exec_module(dtbo)


@unittest.skipUnless(shutil.which("dtc"), "dtc required")
class DtboRepackTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.payloads = []
        for n in range(3):
            result = subprocess.run(["dtc", "-I", "dts", "-O", "dtb"], capture_output=True,
                                    input=('/dts-v1/; / { value = "' + 'value' * (n + 1) + '"; };').encode(),
                                    check=True, timeout=10)
            self.payloads.append(result.stdout)
        size = 32 + 32 * 3 + sum(map(len, self.payloads))
        data = bytearray(struct.pack(">8I", 0xd7b7ab1e, size, 32, 32, 3, 32, 4096, 0))
        cursor = 128
        for n, payload in enumerate(self.payloads):
            data.extend(struct.pack(">8I", len(payload), cursor, n + 11, n + 22, 33, 44, 55, 66))
            cursor += len(payload)
        self.table = bytes(data) + b"".join(self.payloads)
        self.source = self.table + b"AVB metadata must not be copied" + bytes(64)

    def tearDown(self):
        self.temp.cleanup()

    def test_noop_exact_roundtrip_drops_partition_trailer(self):
        for n, payload in enumerate(self.payloads):
            result, before, after = dtbo.replace(self.source, payload, n, dtbo.digest(payload))
            self.assertEqual(result, self.table)
            self.assertEqual(before, after)

    def test_growing_and_shrinking_each_entry_preserves_others_and_identifiers(self):
        for index, original in enumerate(self.payloads):
            for replacement in self.payloads:
                result, before, after = dtbo.replace(self.source, replacement, index, dtbo.digest(original))
                self.assertEqual(len(result), len(self.table) + len(replacement) - len(original))
                self.assertEqual(len(after), 3)
                for n, entry in enumerate(after):
                    self.assertEqual(entry["identifiers"], before[n]["identifiers"])
                    self.assertEqual(result[entry["offset"]:entry["offset"] + entry["bytes"]],
                                     replacement if n == index else self.payloads[n])

    def test_bad_geometry_overlap_version_and_truncated_payload_refused(self):
        for offset, value in ((4, len(self.source) + 1), (12, 16), (16, 129), (20, 36),
                              (24, 2048), (28, 1), (32 + 4, 127), (64 + 4, 128), (32, 39)):
            data = bytearray(self.source)
            struct.pack_into(">I", data, offset, value)
            with self.subTest(offset=offset, value=value), self.assertRaises(ValueError):
                dtbo.table(data)
        with self.assertRaises(ValueError):
            dtbo.table(self.table[:-1])

    def test_wrong_index_original_hash_and_extra_replacement_bytes_refused(self):
        for index, h, payload in ((3, dtbo.digest(self.payloads[0]), self.payloads[0]),
                                  (-1, dtbo.digest(self.payloads[0]), self.payloads[0]),
                                  (0, '0' * 64, self.payloads[0]),
                                  (0, dtbo.digest(self.payloads[0]), self.payloads[0] + b'junk')):
            with self.subTest(index=index, h=h), self.assertRaises(ValueError):
                dtbo.replace(self.source, payload, index, h)

    def test_file_build_size_limit_preservation_and_existing_output(self):
        source, replacement, output = self.root / "source.img", self.root / "new.dtbo", self.root / "out"
        source.write_bytes(self.source)
        replacement.write_bytes(self.payloads[2])
        args = (source, replacement, output, dtbo.digest(self.source), dtbo.digest(self.payloads[2]),
                0, dtbo.digest(self.payloads[0]))
        with self.assertRaisesRegex(ValueError, "size limit"):
            dtbo.build(*args, 32)
        self.assertFalse(output.exists())
        report = dtbo.build(*args, 8 * 1024**2)
        self.assertFalse(report["flash_ready"])
        self.assertTrue(report["unsigned"])
        self.assertEqual(source.read_bytes(), self.source)
        self.assertEqual(replacement.read_bytes(), self.payloads[2])
        with self.assertRaisesRegex(ValueError, "new DTBO"):
            dtbo.build(*args, 8 * 1024**2)
