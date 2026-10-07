import hashlib
import importlib.util
import io
from pathlib import Path
import struct
import tempfile
import unittest

spec = importlib.util.spec_from_file_location("syncboss", Path(__file__).parents[1] / "tools/syncboss-replay.py")
syncboss = importlib.util.module_from_spec(spec)
spec.loader.exec_module(syncboss)


class SyncbossTests(unittest.TestCase):
    def test_concatenated_and_fragmented_records(self):
        packet = struct.pack("<BBBBqBBB", 2, 12, 0, 1, -12345, 201, 0, 4) + bytes.fromhex("deadbeef")
        driver = struct.pack("<BBBBqBqII", 3, 21, 1, 0, 0, 2, 999, 2, 1)

        class Fragmented(io.BytesIO):
            def read(self, count):
                return super().read(min(count, 1))

        for stream in (io.BytesIO(packet + driver), Fragmented(packet + driver)):
            records = list(syncboss.decode_stream(stream, hashlib.sha256()))
            self.assertEqual(len(records), 2)
            self.assertEqual(records[0]["payload_hex"], "deadbeef")
            self.assertEqual(records[0]["nsync"]["offset_us"], -12345)
            self.assertEqual(records[1]["byte_offset"], len(packet))
            self.assertEqual(records[1]["driver_message_type"], 2)
            self.assertIsNone(records[1]["remote"]["offset_us"])

    def test_invalid_headers_and_lengths_fail(self):
        invalid = [bytes([1, 12]), bytes([3, 12]),
                   struct.pack("<BBBBq", 2, 12, 2, 0, 0),
                   struct.pack("<BBBBq", 2, 12, 0, 3, 0),
                   struct.pack("<BBBBqBBB", 2, 12, 0, 1, 0, 5, 0, 253)]
        for record in invalid:
            with self.subTest(record=record), self.assertRaises(ValueError):
                syncboss.decode_record(record)
        with self.assertRaisesRegex(ValueError, "Truncated"):
            list(syncboss.decode_stream(io.BytesIO(bytes([3, 21, 0])), hashlib.sha256()))

    def test_offline_only_and_no_tracking_claim(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "capture"
            path.write_bytes(struct.pack("<BBBBqBBB", 2, 12, 0, 0, 999, 5, 0, 0))
            report = syncboss.replay(path)
            self.assertEqual(report["records"], 1)
            self.assertEqual(report["valid_nsync_offsets"], 0)
            self.assertFalse(report["tracking_verified"])
            link = Path(temp) / "link"
            link.symlink_to(path)
            with self.assertRaisesRegex(ValueError, "regular capture"):
                syncboss.replay(link)
        with self.assertRaisesRegex(ValueError, "regular capture"):
            syncboss.replay(Path("/dev/null"))
