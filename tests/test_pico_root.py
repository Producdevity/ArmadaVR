import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location("pico_root", Path(__file__).parents[1] / "tools/test-pico-root.py")
pico = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pico)


class PicoRootInputsTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name)
        self.kernel, self.initramfs, self.root = [self.directory / n for n in ("kernel", "initramfs", "root")]
        for directory in (self.kernel, self.initramfs, self.root):
            directory.mkdir()
        (self.kernel / "build.json").write_text("{}")
        (self.initramfs / "initramfs.img").write_bytes(b"initramfs")
        (self.root / "rootfs.ext4").write_bytes(b"root fixture")
        (self.root / "contents.json").write_text('{}')
        self.build = {"repository": "bytedance/neo3-kernel", "qemu_transports": True}
        self.initrd = {"status": "built", "qemu_transports": True, "quest_services": False,
                       "kernel_build_sha256": pico.builder.digest(self.kernel / "build.json"),
                       "artifacts": [{"path": "initramfs.img", "sha256": pico.builder.digest(self.initramfs / "initramfs.img")}]}
        self.report = {"status": "built", "profile": "pico-neo3", "qemu_transports": True,
                       "kernel_build_sha256": self.initrd["kernel_build_sha256"],
                       "initramfs_sha256": self.initrd["artifacts"][0]["sha256"],
                       "systemd_runtime_verified": True, "systemd_runtime_sha256": "a" * 64,
                       "sha256": {n: pico.builder.digest(self.root / n) for n in ("rootfs.ext4", "contents.json")}}
        self.save()

    def save(self):
        (self.initramfs / "build.json").write_text(json.dumps(self.initrd))
        (self.root / "manifest.json").write_text(json.dumps(self.report))

    def check(self):
        with patch.object(pico.builder, "inputs", return_value=(self.build, {"Image": "b" * 64}, "4.19", [])):
            return pico.root_input(self.kernel, self.initramfs, self.root)

    def test_matching_packaged_root_is_accepted(self):
        result = self.check()
        self.assertEqual(result[3], self.root / "rootfs.ext4")
        self.assertEqual(result[2]["systemd_runtime_sha256"], "a" * 64)

    def test_physical_and_cross_target_inputs_are_refused(self):
        for record, name, value in ((self.build, "qemu_transports", False),
                                     (self.build, "repository", "another/kernel"),
                                     (self.initrd, "quest_services", True),
                                     (self.report, "profile", "quest3"),
                                     (self.report, "qemu_transports", False),
                                     (self.report, "status", "failed")):
            with self.subTest(name=name, value=value):
                old = record[name]
                record[name] = value
                self.save()
                with self.assertRaisesRegex(ValueError, "Pico QEMU"):
                    self.check()
                record[name] = old

    def test_mismatched_kernel_and_initramfs_are_refused(self):
        for record, name in ((self.initrd, "kernel_build_sha256"),
                             (self.report, "kernel_build_sha256"), (self.report, "initramfs_sha256")):
            old = record[name]
            record[name] = "c" * 64
            self.save()
            with self.assertRaisesRegex(ValueError, "identities must match"):
                self.check()
            record[name] = old

    def test_corrupt_image_or_contents_are_refused(self):
        for name in ("rootfs.ext4", "contents.json"):
            path = self.root / name
            old = path.read_bytes()
            path.write_bytes(b"changed")
            with self.assertRaisesRegex(ValueError, "checksum mismatch"):
                self.check()
            path.write_bytes(old)

    def test_unverified_runtime_is_refused(self):
        self.report["systemd_runtime_verified"] = False
        self.save()
        with self.assertRaisesRegex(ValueError, "verified packaged"):
            self.check()


class PicoRootFixtureTests(unittest.TestCase):
    def test_fixture_archive_roundtrip(self):
        data = pico.cpio([("pico-fixture", 0o100755, b"payload")])
        self.assertEqual(data[:6], b"070701")
        fields = [int(data[i:i + 8], 16) for i in range(6, 110, 8)]
        self.assertEqual(fields[1], 0o100755)
        self.assertEqual(fields[6], 7)
        self.assertEqual(data[110:110 + fields[11]], b"pico-fixture\0")
        payload = (110 + fields[11] + 3) & ~3
        self.assertEqual(data[payload:payload + fields[6]], b"payload")
        self.assertIn(b"TRAILER!!!\0", data)

    def test_guest_probe_is_valid_python(self):
        compile(pico.PROBE, "pico-probe.py", "exec")


class GuestAgentTests(unittest.TestCase):
    def test_native_regular_agent_and_refusals(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "agent"
            data = bytearray(64)
            data[:7] = b"\x7fELF\x02\x01\x01"
            data[18:20] = b"\xb7\x00"
            path.write_bytes(data)
            self.assertEqual(pico.guest_agent(path), data)
            link = Path(directory) / "link"
            link.symlink_to(path)
            with self.assertRaises(ValueError):
                pico.guest_agent(link)
            for payload in (b"short", b"not ELF" + bytes(57), data[:18] + b"\x3e\x00" + data[20:]):
                path.write_bytes(payload)
                with self.assertRaises(ValueError):
                    pico.guest_agent(path)

    def test_agent_library_names_are_bounded_and_unique(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "libnuma.so.1"
            data = bytearray(64)
            data[:7] = b"\x7fELF\x02\x01\x01"
            data[18:20] = b"\xb7\x00"
            path.write_bytes(data)
            self.assertEqual(pico.agent_libraries([path]), {path.name: data})
            with self.assertRaises(ValueError):
                pico.agent_libraries([path, path])
            for name in ("bad name.so", "libx.so;false", "qemu-ga", "libx.so.1\n"):
                with self.assertRaises(ValueError):
                    pico.agent_libraries([Path(directory) / name])
