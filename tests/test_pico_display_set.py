import hashlib
import importlib.util
from pathlib import Path
import shutil
import struct
import sys
import unittest

import test_pico_overlay_binding as fixture

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
spec = importlib.util.spec_from_file_location('pico_display_set', ROOT / 'tools/verify-pico-display-set.py')
verify = importlib.util.module_from_spec(spec)
spec.loader.exec_module(verify)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def boot_image(pack):
    header = bytearray(4096)
    header[:8] = b'ANDROID!'
    for offset, value in ((8, 64), (16, 32), (36, 4096), (40, 2), (1644, 1660), (1648, len(pack))):
        struct.pack_into('<I', header, offset, value)
    return bytes(header) + b'kernel'.ljust(4096, b'\0') + b'ramdisk'.ljust(4096, b'\0') + pack.ljust((len(pack) + 4095) & ~4095, b'\0')


def table(payloads):
    cursor = 32 + 32 * len(payloads)
    entries = bytearray()
    for index, payload in enumerate(payloads):
        entries.extend(struct.pack('>8I', len(payload), cursor, index + 10, 0, 0, 0, 0, 0))
        cursor += len(payload)
    return struct.pack('>8I', 0xd7b7ab1e, cursor, 32, 32, len(payloads), 32, 4096, 0) + entries + b''.join(payloads)


@unittest.skipUnless(all(shutil.which(n) for n in ('dtc', 'fdtoverlay', 'fdtput')), 'dtc tools required')
class PicoDisplaySetTests(unittest.TestCase):
    def setUp(self):
        self.fixture = fixture.PicoOverlayTests()
        self.fixture.setUp()
        self.addCleanup(self.fixture.tearDown)
        self.fixture.fixture()
        self.root = self.fixture.root
        self.base = self.fixture.base.read_bytes()
        self.overlay = self.fixture.source.read_bytes()
        self.paths = {name: self.root / (name + '.img') for name in ('stock_boot', 'boot', 'stock_dtbo', 'dtbo')}
        for name in ('stock_boot', 'boot'):
            self.paths[name].write_bytes(boot_image(self.base * 2))
        self.paths['stock_dtbo'].write_bytes(table([self.overlay] * 2) + b'stock trailer')
        self.fixture.build('binding')
        self.bound = (self.root / 'binding/panel-mux.dtbo').read_bytes()
        data, _, _ = verify.dtbo.replace(self.paths['stock_dtbo'].read_bytes(), self.bound, 0, hashlib.sha256(self.overlay).hexdigest())
        self.paths['dtbo'].write_bytes(data)
        self.hashes = {n: digest(p) for n, p in self.paths.items()}

    def check(self, name='result', base=0, index=0):
        return verify.verify(self.paths, self.hashes, base, index, '/soc/mdp/panel', '/soc/i2c/bridge',
                             '/soc/display', '/soc/mdp/previous', self.root / name)

    def test_complete_pair_preserves_other_entries_and_physical_limits(self):
        report = self.check()
        self.assertEqual((report['base_count'], report['overlay_count']), (2, 2))
        self.assertTrue(report['stock_base_pack_unchanged'])
        self.assertTrue(report['other_overlays_unchanged'])
        for name in ('device_accessed', 'live_selection_verified', 'display_verified', 'boot_verified', 'recovery_verified', 'flash_ready'):
            self.assertFalse(report[name])
        self.assertEqual({n: digest(p) for n, p in self.paths.items()}, self.hashes)
        self.assertEqual(verify.read_fdt((self.root / 'result/selected.dtb').read_bytes()),
                         verify.read_fdt((self.root / 'binding/panel-mux.dtb').read_bytes()))
        with self.assertRaisesRegex(ValueError, 'new verification'):
            self.check()

    def test_wrong_hash_and_symlink_inputs_refused(self):
        for name in self.paths:
            original = self.hashes[name]
            self.hashes[name] = '0' * 64
            with self.subTest(name=name), self.assertRaisesRegex(ValueError, 'SHA-256'):
                self.check()
            self.hashes[name] = original
        path = self.paths['boot']
        path.rename(self.root / 'saved.img')
        path.symlink_to('saved.img')
        with self.assertRaises(OSError):
            self.check()
        self.assertFalse((self.root / 'result').exists())

    def test_any_base_tree_change_is_refused(self):
        changed = bytearray(self.base)
        changed[28:32] = struct.pack('>I', 1)
        self.paths['boot'].write_bytes(boot_image(self.base + changed))
        self.hashes['boot'] = digest(self.paths['boot'])
        with self.assertRaisesRegex(ValueError, 'changed the stock base'):
            self.check()
        self.assertFalse((self.root / 'result').exists())

    def test_original_overlay_wrong_entry_or_changed_identifiers_refused(self):
        original = self.paths['dtbo'].read_bytes()
        changes = [table([self.overlay] * 2), table([self.overlay, self.bound]),
                   table([self.bound] * 2)]
        changed = bytearray(original)
        struct.pack_into('>I', changed, 40, 999)
        changes.append(changed)
        for data in changes:
            self.paths['dtbo'].write_bytes(data)
            self.hashes['dtbo'] = digest(self.paths['dtbo'])
            with self.assertRaisesRegex(ValueError, 'exactly the requested panel'):
                self.check()
            self.assertFalse((self.root / 'result').exists())

    def test_bad_indices_trailer_and_truncated_pack_refused(self):
        for base, index in ((-1, 0), (2, 0), (0, -1), (0, 2), (False, 0)):
            with self.subTest(base=base, index=index), self.assertRaisesRegex(ValueError, 'index outside'):
                self.check(base=base, index=index)
        self.paths['dtbo'].write_bytes(self.paths['dtbo'].read_bytes() + b'trailer')
        self.hashes['dtbo'] = digest(self.paths['dtbo'])
        with self.assertRaisesRegex(ValueError, 'without a trailer'):
            self.check()
        for pack in (b'', self.base[:-1], self.base + b'junk'):
            with self.subTest(size=len(pack)), self.assertRaises(ValueError):
                verify.base_pack(boot_image(pack))
