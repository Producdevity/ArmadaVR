import importlib.util
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location('pico_config', ROOT / 'tools/pico-controller-config.py')
CONFIG = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CONFIG)


class PicoControllerConfigTests(unittest.TestCase):
    def setUp(self):
        self.commands = []
        self.replies = {}
        self.boot = '12345678-1234-1234-1234-123456789abc'

    def read(self, *command):
        self.commands.append(command)
        if command in self.replies:
            reply = self.replies[command]
            return reply.pop(0) if isinstance(reply, list) else reply
        known = {
            ('getprop', 'ro.build.fingerprint'): CONFIG.FINGERPRINT,
            ('getprop', 'ro.product.device'): 'PICOA7H10',
            ('sha256sum', CONFIG.LIBRARY): CONFIG.LIBRARY_SHA256 + '  ' + CONFIG.LIBRARY,
            ('sha256sum', CONFIG.CONFIG_APK): CONFIG.CONFIG_SHA256 + '  ' + CONFIG.CONFIG_APK,
            ('cat', '/proc/sys/kernel/random/boot_id'): self.boot,
            ('service', 'call', 'PicoStation-new', '76'): "Result: Parcel(00000000 00000002   '........')",
            ('service', 'call', 'PicoStation-new', '74'): "Result: Parcel(00000000 00000001   '........')",
        }
        for key, value in [('sys_controller_chip', '31'), ('sys_controller_prototype', '32')]:
            known[('service', 'call', 'ConfigurationService', '3', 's16', 'com.pvr.configuration',
                   'i32', '0', 's16', key, 'i32', '-1')] = \
                f"Result: Parcel(00000000 00000001 000000{value}  '........?...')"
        self.assertIn(command, known, 'Collector attempted an unverified command')
        return known[command]

    def test_verified_snapshot(self):
        report = CONFIG.collect(self.read)
        self.assertEqual(report['controller'], {'mcu_type': 1, 'radio_type': 2, 'touch_supported': True})
        self.assertEqual(sum(command[:2] == ('service', 'call') for command in self.commands), 8)

    def test_interface_changes_refuse_before_binder(self):
        for command in [('getprop', 'ro.build.fingerprint'), ('getprop', 'ro.product.device'),
                        ('sha256sum', CONFIG.LIBRARY), ('sha256sum', CONFIG.CONFIG_APK)]:
            with self.subTest(command=command):
                self.replies = {command: 'different'}
                self.commands.clear()
                with self.assertRaises(ValueError):
                    CONFIG.collect(self.read)
                self.assertFalse(any(c[0] == 'service' for c in self.commands))

    def test_getter_error_and_disagreement(self):
        for text in ["Result: Parcel(ffffffff 00000002 '........')", 'Permission denied',
                     "Result: Parcel(00000000 00000000 '........')",
                     "Result: Parcel(00000000 00000001 '........')"]:
            self.replies = {('service', 'call', 'PicoStation-new', '76'): text}
            with self.subTest(text=text), self.assertRaises(ValueError):
                CONFIG.collect(self.read)

    def test_reboot_and_changed_settings(self):
        for command, replies in [
            (('cat', '/proc/sys/kernel/random/boot_id'), [self.boot, '87654321-1234-1234-1234-123456789abc']),
            (('getprop', 'ro.build.fingerprint'), [CONFIG.FINGERPRINT, 'changed']),
            (('service', 'call', 'PicoStation-new', '74'),
             ["Result: Parcel(00000000 00000001 '..')", "Result: Parcel(00000000 00000000 '..')"]),
        ]:
            self.replies = {command: replies}
            with self.subTest(command=command), self.assertRaises(ValueError):
                CONFIG.collect(self.read)

    def test_string_framing(self):
        for text in ["Result: Parcel(00000000 00000000 00000031 '..')",
                     "Result: Parcel(00000000 00000001 00310031 '..')",
                     "Result: Parcel(00000000 00000001 00000030 '..')",
                     "Result: Parcel(00000000 00000001 00000031 00000000 '..')",
                     "Result: Parcel(00000000 00000001 00000031 '..') extra"]:
            with self.subTest(text=text), self.assertRaises(ValueError):
                CONFIG.digit(text)
