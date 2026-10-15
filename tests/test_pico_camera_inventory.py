import contextlib
import copy
import ctypes
import errno
import importlib.util
import io
import json
import os
from pathlib import Path
import stat
import struct
from types import SimpleNamespace
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('camera_inventory', ROOT / 'tools/pico-camera-inventory.py')
camera = importlib.util.module_from_spec(spec)
spec.loader.exec_module(camera)


def graph():
    return {
        'entities': [(1, b'video3', 0x10000, 0), (2, b'v4l-subdev8', 0x10001, 0),
                     (3, b'v4l-subdev12', 0x10008, 0)],
        'interfaces': [(11, 0x200, 0, 81, 3), (12, 0x203, 0, 81, 8)],
        'pads': [(21, 2, 2, 0), (22, 3, 1, 0)],
        'links': [(31, 11, 1, 0x10000001), (32, 12, 2, 0x10000001), (33, 21, 22, 1)],
    }


class Device:
    def __init__(self):
        self.calls = []
        self.graph = graph()
        self.driver, self.model = b'cam_req_mgr', b'cam-req-mgr-devnode'
        self.changes = 0
        self.fill_error = None
        self.change_count = False
        self.version = 1
        self.media_version = 0x41351

    def ioctl(self, fd, command, data, mutate):
        assert fd == 7 and mutate is True
        assert command in (camera.DEVICE_INFO, camera.GET_TOPOLOGY)
        self.calls.append(command)
        if command == camera.DEVICE_INFO:
            data[:] = struct.pack('<16s32s40s32sIII124x', self.driver, self.model,
                                  b'unit"\\\n', b'platform:cam_req_mgr', self.media_version, 0, self.media_version)
            return 0
        values = list(camera.TOPOLOGY.unpack(data))
        filling = bool(values[3])
        if filling and self.fill_error:
            error = self.fill_error
            if error == errno.ENOSPC:
                self.fill_error = None
                self.version += 1
            raise OSError(error, 'fixture')
        counts = []
        for i, (name, record, _) in enumerate(camera.RECORDS):
            rows = self.graph[name]
            counts.append(len(rows))
            if filling:
                assert values[1+3*i] == len(rows)
                payload = b''.join(record.pack(*row) for row in rows)
                if payload:
                    ctypes.memmove(values[3+3*i], payload, len(payload))
        if filling and self.changes:
            self.changes -= 1
            self.version += 1
            if self.change_count:
                counts[0] += 1
        values[0] = self.version
        for i, count in enumerate(counts):
            values[1+3*i] = count
        data[:] = camera.TOPOLOGY.pack(*values)
        return 0


class CameraInventoryTests(unittest.TestCase):
    def setUp(self):
        self.device = Device()
        self.ioctl = patch.object(camera.fcntl, 'ioctl', self.device.ioctl)
        self.ioctl.start()
        self.addCleanup(self.ioctl.stop)

    def description(self):
        info = camera.device_info(7)
        version, records = camera.topology(7)
        return camera.describe(info, version, records)

    def test_topology_preserves_custom_functions_and_unmapped_entities(self):
        report = self.description()
        self.assertEqual([e['role'] for e in report['entities']], ['request-manager', 'sensor', 'csiphy'])
        self.assertEqual([e['interfaces'] for e in report['entities']], [[11], [12], []])
        self.assertEqual(report['interfaces'][1]['minor'], 8)
        self.assertFalse(report['sensor_identity_verified'])
        self.assertFalse(report['hardware_capture_verified'])
        self.assertEqual(json.loads(json.dumps(report)), report)
        self.assertEqual(self.device.calls, [camera.DEVICE_INFO, camera.GET_TOPOLOGY, camera.GET_TOPOLOGY])

    def test_quest_roles_and_version_are_explicit(self):
        with self.assertRaises(ValueError):
            camera.device_info(7, 'quest3')
        self.device.media_version = 0x050af6
        info = camera.device_info(7, 'quest3')
        records = {name: [] for name in graph()}
        records['entities'] = [(i + 1, b'node\0', 0x10000 + i, 0) for i in range(22)]
        report = camera.describe(info, 1, records, 'quest3')
        self.assertEqual([e['role'] for e in report['entities']], [*camera.QUEST_ROLES, None])
        self.assertEqual(report['target'], 'quest3')
        self.assertFalse(report['hardware_capture_verified'])
        self.assertIsNone(camera.describe(info, 1, records)['entities'][20]['role'])
        for function, args in ((camera.device_info, (7, 'unknown')),
                               (camera.describe, (info, 1, records, 'unknown')),
                               (camera.collect, ('/dev/media0', 'unknown'))):
            with self.assertRaises(ValueError):
                function(*args)

    def test_sync_role_is_scoped_to_sync_model(self):
        self.device.driver, self.device.model = b'cam_sync', b'cam_sync_device'
        report = self.description()
        self.assertEqual([e['role'] for e in report['entities']], ['sync', None, None])

    def test_names_are_not_treated_as_paths(self):
        self.device.graph['entities'][0] = (1, b'../video"\\\n', 0x10000, 0)
        self.assertEqual(self.description()['entities'][0]['name'], '../video"\\\n')

    def test_empty_graph_is_inventory_not_activation(self):
        self.device.graph = {key: [] for key in graph()}
        report = self.description()
        self.assertEqual(report['entities'], [])
        self.assertFalse(report['active_configuration_verified'])

    def test_unterminated_names_are_refused(self):
        self.device.graph['entities'][0] = (1, b'x'*64, 0x10000, 0)
        with self.assertRaisesRegex(ValueError, 'Unterminated'):
            self.description()

    def test_wrong_media_device(self):
        for driver, model in ((b'uvcvideo', b'cam-req-mgr-devnode'), (b'cam_sync', b'other')):
            self.device.driver, self.device.model = driver, model
            with self.assertRaisesRegex(ValueError, 'Expected'):
                self.description()

    def test_old_abi_and_invalid_device_names(self):
        self.device.media_version = 0x412ff
        with self.assertRaisesRegex(ValueError, '4.19'):
            self.description()
        self.device.media_version = 0x41351
        self.device.driver = b'x' * 16
        with self.assertRaisesRegex(ValueError, 'Unterminated'):
            self.description()

    def test_native_guard_precedes_filesystem_access(self):
        for system, machine in (('darwin', 'arm64'), ('linux', 'x86_64')):
            with patch.object(camera.sys, 'platform', system), \
                    patch.object(camera.platform, 'machine', return_value=machine), \
                    patch.object(camera.Path, 'read_text', side_effect=AssertionError('must not read sysfs')), \
                    self.assertRaisesRegex(ValueError, 'ARM64 Linux'):
                camera.collect('/dev/media0')

    def test_retry_changed_graph(self):
        self.device.changes = 2
        self.assertEqual(self.description()['topology_version'], 3)
        self.assertEqual(len(self.device.calls), 7)

    def test_retry_count_change_and_enospc(self):
        self.device.changes = 1
        self.device.change_count = True
        self.assertEqual(self.description()['topology_version'], 2)
        self.device.fill_error = errno.ENOSPC
        self.assertEqual(self.description()['topology_version'], 3)

    def test_unstable_graph_and_ioctl_failure(self):
        self.device.changes = 3
        with self.assertRaisesRegex(ValueError, 'three collection'):
            self.description()
        self.assertEqual(len(self.device.calls), 7)
        self.device.fill_error = errno.EIO
        with self.assertRaises(OSError):
            self.description()

    def test_oversized_graph_refused_before_allocation(self):
        for name, _, limit in camera.RECORDS:
            with self.subTest(kind=name):
                self.device.graph = graph()
                self.device.graph[name] *= limit + 1
                with patch.object(camera.ctypes, 'create_string_buffer', side_effect=AssertionError('must not allocate')):
                    with self.assertRaisesRegex(ValueError, 'bounds'):
                        self.description()

    def test_invalid_object_ids_and_references(self):
        invalid = [
            ('entities', 0, (0, b'video0', 0x10000, 0)),
            ('entities', 0, (2, b'video0', 0x10000, 0)),
            ('interfaces', 0, (1, 0x200, 0, 81, 3)),
            ('pads', 0, (21, 99, 2, 0)),
            ('links', 0, (31, 99, 1, 0x10000001)),
            ('links', 0, (31, 1, 11, 0x10000001)),
            ('links', 0, (31, 11, 1, 0x20000001)),
            ('links', 2, (33, 21, 1, 1)),
        ]
        for kind, index, value in invalid:
            with self.subTest(value=value):
                self.device.graph = graph()
                self.device.graph[kind][index] = value
                with self.assertRaises(ValueError):
                    self.description()

    def collect(self, path='/dev/media2', rdev=None, changed=False, symlink=False):
        identity = SimpleNamespace(st_mode=stat.S_IFLNK if symlink else stat.S_IFCHR | 0o600,
                                   st_dev=5, st_ino=6, st_rdev=os.makedev(250, 2) if rdev is None else rdev)
        opened = copy.copy(identity)
        if changed:
            opened.st_ino += 1
        with contextlib.ExitStack() as stack:
            stack.enter_context(patch.object(camera.sys, 'platform', 'linux'))
            stack.enter_context(patch.object(camera.platform, 'machine', return_value='aarch64'))
            stack.enter_context(patch.object(camera.Path, 'read_text', return_value='250:2\n'))
            stack.enter_context(patch.object(camera.os, 'lstat', return_value=identity))
            stack.enter_context(patch.object(camera.os, 'fstat', return_value=opened))
            open_mock = stack.enter_context(patch.object(camera.os, 'open', return_value=7))
            close_mock = stack.enter_context(patch.object(camera.os, 'close'))
            try:
                return camera.collect(path)
            finally:
                if open_mock.called:
                    self.assertEqual(open_mock.call_args.args, (path, os.O_RDONLY | os.O_CLOEXEC | os.O_NONBLOCK | os.O_NOFOLLOW))
                    close_mock.assert_called_once_with(7)

    def test_opens_only_explicit_media_node_read_only(self):
        report = self.collect()
        self.assertEqual(report['path'], '/dev/media2')
        self.assertEqual(report['device_number'], [250, 2])

    def test_refuses_node_mismatch_and_changed_inode(self):
        for args in ({'rdev': os.makedev(81, 2)}, {'changed': True}, {'symlink': True}):
            with self.subTest(args=args), self.assertRaises(ValueError):
                self.collect(**args)
        self.assertEqual(self.device.calls, [])

    def test_refuses_other_paths(self):
        for path in ('/dev/video0', '/dev/v4l-subdev0', '/dev/media00', '/dev/media1000', 'media0', '/dev/media0/../video0'):
            with self.subTest(path=path), self.assertRaisesRegex(ValueError, 'explicit'):
                self.collect(path)
        self.assertEqual(self.device.calls, [])

    def test_always_closes_after_failure(self):
        self.device.fill_error = errno.ENODEV
        with self.assertRaises(OSError):
            self.collect()

    def test_cli_outputs_nothing_on_failure(self):
        with patch.object(camera.sys, 'argv', ['collector', '--media', '/dev/media0']), \
                patch.object(camera, 'collect', side_effect=ValueError('incomplete')), \
                contextlib.redirect_stdout(io.StringIO()) as output, \
                contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as result:
            camera.main()
        self.assertEqual(result.exception.code, 1)
        self.assertEqual(output.getvalue(), '')
