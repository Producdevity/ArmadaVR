import hashlib
import importlib.util
import json
from pathlib import Path
import stat
import tempfile
import unittest

spec = importlib.util.spec_from_file_location("kernel_thermal", Path(__file__).parents[1] / "tools/test-kernel-thermal.py")
thermal = importlib.util.module_from_spec(spec)
spec.loader.exec_module(thermal)


class KernelThermalTests(unittest.TestCase):
    def test_requires_verified_qemu_kernel_and_scoped_cache(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "Image").write_bytes(b"test fixture")
            report = {"status": "compiled", "qemu_transports": True,
                      "artifacts": [{"path": "Image", "sha256": hashlib.sha256(b"test fixture").hexdigest()}],
                      "commands": [["make", "-C", "/work/linux-userspace/123/source",
                                    "O=/work/linux-userspace/123/build", "olddefconfig"]]}

            def save():
                (root / "build.json").write_text(json.dumps(report))

            save()
            self.assertEqual(thermal.kernel_inputs(root)[1], "/work/linux-userspace/123/build")
            report["qemu_transports"] = False
            save()
            with self.assertRaisesRegex(ValueError, "headset builds are refused"):
                thermal.kernel_inputs(root)
            report["qemu_transports"] = True
            report["commands"][0][3] = "O=/work/linux-userspace/../unsafe"
            save()
            with self.assertRaisesRegex(ValueError, "cache paths"):
                thermal.kernel_inputs(root)
            (root / "Image").write_bytes(b"tampered")
            with self.assertRaisesRegex(ValueError, "checksum mismatch"):
                thermal.kernel_inputs(root)

    def test_initramfs_contains_only_ram_fixture_and_console(self):
        archive = thermal.initramfs(b"init fixture", b"module fixture")
        offset, entries = 0, {}
        while offset < len(archive):
            self.assertEqual(archive[offset:offset + 6], b"070701")
            fields = [int(archive[offset + 6 + 8*i:offset + 14 + 8*i], 16) for i in range(13)]
            start = offset + 110
            name = archive[start:start + fields[11] - 1].decode()
            start = (start + fields[11] + 3) & ~3
            data = archive[start:start + fields[6]]
            offset = (start + len(data) + 3) & ~3
            entries[name] = (fields, data)
        self.assertEqual(set(entries), {"dev", "proc", "sys", "sbin", "dev/console", "init",
                                        "sbin/poweroff", "armada_thermal_test.ko", "TRAILER!!!"})
        self.assertTrue(stat.S_ISCHR(entries["dev/console"][0][1]))
        self.assertEqual(entries["dev/console"][0][9:11], [5, 1])
        self.assertEqual(entries["sbin/poweroff"][1], b"/init")
        self.assertEqual(entries["armada_thermal_test.ko"][1], b"module fixture")
