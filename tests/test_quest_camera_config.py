import hashlib
import importlib.util
from pathlib import Path
import struct
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('quest_config', ROOT / 'tools/quest-camera-config.py')
config = importlib.util.module_from_spec(spec)
spec.loader.exec_module(config)


def varint(value):
    data = bytearray()
    while value >= 128:
        data.append((value & 127) | 128); value >>= 7
    data.append(value)
    return bytes(data)


def field(number, value, wire=0):
    data = varint((number << 3) | wire)
    if wire == 0:
        return data + varint(value)
    return data + (varint(len(value)) if wire == 2 else b'') + value


def fixture():
    stream = field(22, 640) + field(23, 513) + field(31, struct.pack('<f', 0.5), 5)
    mode = field(30, struct.pack('<d', 120), 1) + field(63, stream, 2)
    port = field(1, 4) + field(15, b'0x3210', 2)
    camx = field(2, port, 2) + field(90, mode, 2)
    camera = field(2, b'Left', 2) + field(40, camx, 2) + field(62, 1)
    group = field(1, b'Tracking', 2) + field(20, b'Left', 2)
    return field(1, field(1, 6), 2) + field(20, camera, 2) + field(30, group, 2)


class QuestCameraConfigTests(unittest.TestCase):
    def test_nested_stream_and_explicit_geometry(self):
        result = config.inspect(fixture())
        camera = result['cameras'][0]
        self.assertNotIn('ers_id', camera)
        self.assertEqual(camera['camx']['port'], {'phy': 4, 'lane_assign_hex': '0x3210'})
        self.assertEqual(camera['camx']['modes'], [{'fps': 120.0, 'streams': [
            {'width': 640, 'height': 513, 'isp_scale_x': 0.5}]}])
        self.assertEqual(result['sets'][0]['cameras'], ['Left'])

    def test_unknown_fields_are_bounded_and_skipped(self):
        for wire, value in [(0, 2**64-1), (1, b'12345678'), (2, b'opaque'), (5, b'1234')]:
            self.assertEqual(config.inspect(fixture() + field(999, value, wire)), config.inspect(fixture()))
        for data in [field(999, b'abc', 2)[:-1], field(999, b'12345678', 1)[:-1]]:
            with self.assertRaises(ValueError):
                config.inspect(fixture() + data)

    def test_rejects_bad_varints_tags_and_types(self):
        for data in [b'\x80', b'\x80\0', b'\x80'*10, b'\xff'*9+b'\x02', b'\0', b'\x0b',
                     field(1, 6), field(1 << 29, 0)]:
            with self.subTest(data=data), self.assertRaises(ValueError):
                config.decode(data)
        with self.assertRaises(ValueError):
            config.decode(field(1, 2**32), 'device')
        with self.assertRaises(ValueError):
            config.decode(field(4, 2), 'camera')

    def test_rejects_duplicate_singular_and_wrong_device(self):
        for data in [fixture()+field(1, field(1, 6), 2), field(1, field(1, 7), 2), b'']:
            with self.assertRaises(ValueError):
                config.inspect(data)

    def test_rejects_missing_or_duplicate_camera_references(self):
        for camera in [field(2, b'Left', 2), field(2, b'Right', 2),
                       field(1, 1)+field(2, b'Right', 2)+field(3, b'Unknown', 2)]:
            with self.assertRaises(ValueError):
                config.inspect(fixture()+field(20, camera, 2))
        with self.assertRaises(ValueError):
            config.inspect(fixture()+field(30, field(20, b'Unknown', 2), 2))

    def test_rejects_nonfinite_and_unbounded_fields(self):
        for kind, wire, schema, number in [('d', 1, 'mode', 30), ('f', 5, 'stream', 31)]:
            for value in [float('nan'), float('inf'), -float('inf')]:
                with self.assertRaises(ValueError):
                    config.decode(field(number, struct.pack('<'+kind, value), wire), schema)
        for value in [b'\xff', b'hello\0', b'x'*257]:
            with self.assertRaises(ValueError):
                config.decode(field(2, value, 2), 'camera')
        with self.assertRaises(ValueError):
            config.decode(field(999, 0)*4097)
        with self.assertRaises(ValueError):
            config.decode(b'x'*(config.MAX_BYTES+1))

    def test_hash_gate_and_no_capture_claim(self):
        with tempfile.TemporaryDirectory() as tmp:
            p = Path(tmp)/'config.bin'; p.write_bytes(fixture())
            digest = hashlib.sha256(p.read_bytes()).hexdigest()
            result = config.run(p, digest)
            self.assertFalse(result['capture_configuration_ready'])
            self.assertFalse(result['active_modes_verified'])
            self.assertFalse(result['templates_resolved'])
            self.assertEqual(result['device_actions'], [])
            with self.assertRaises(ValueError):
                config.run(p, '0'*64)
            alias = p.with_name('alias'); alias.symlink_to(p)
            with self.assertRaises(ValueError):
                config.run(alias, digest)
