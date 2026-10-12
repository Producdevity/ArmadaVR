import importlib.util
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch


spec = importlib.util.spec_from_file_location("pico_services", Path(__file__).parents[1] / "tools/pico-services.py")
pico = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pico)


class PicoServicesTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.source = Path(self.temp.name) / "source"
        self.root = Path(self.temp.name) / "root"
        self.root.mkdir()
        elf = bytearray(64)
        elf[:7] = b"\x7fELF\x02\x01\x01"
        elf[18:20] = b"\xb7\x00"
        for name in pico.RUNTIME + pico.LICENSES + tuple(pico.PROVENANCE):
            path = self.source / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(elf if name in pico.RUNTIME else b"license")
            path.chmod(0o755 if name in pico.RUNTIME else 0o644)
        for name, reference in pico.PROVENANCE.items():
            (self.source / name).write_bytes((pico.ROOT / reference).read_bytes())
        (self.source / pico.LIBRARY_LINK).symlink_to(pico.LIBRARY_TARGET)

    def tearDown(self):
        self.temp.cleanup()

    def test_install_and_verify_preserve_files_modes_and_links(self):
        result = pico.install(self.source, self.root)
        pico.verify(self.root, result)
        self.assertFalse(result["dsp_startup"])
        self.assertEqual(result["required_cmdline"], "armada.pico=qrtr")
        self.assertIn("ConditionPathExists=!/etc/initrd-release", (self.root / "usr/lib/systemd/system/armada-pico-qrtr.service").read_text())
        with self.assertRaisesRegex(ValueError, "already exists"):
            pico.install(self.source, self.root)

    def test_changed_runtime_and_enablement_are_rejected(self):
        result = pico.install(self.source, self.root)
        binary = self.root / pico.RUNTIME[0]
        binary.chmod(0o644)
        with self.assertRaisesRegex(ValueError, "Changed Pico root service file"):
            pico.verify(self.root, result)
        binary.chmod(0o755)
        link = self.root / pico.LIBRARY_LINK
        link.unlink()
        link.symlink_to("other.so")
        with self.assertRaisesRegex(ValueError, "Changed Pico root service link"):
            pico.verify(self.root, result)

    def test_wrong_architecture_and_source_identity_refused_before_writing(self):
        path = self.source / pico.RUNTIME[0]
        original = path.read_bytes()
        path.write_bytes(original[:18] + b"\x3e\x00" + original[20:])
        with self.assertRaisesRegex(ValueError, "ARM64"):
            pico.install(self.source, self.root)
        self.assertEqual(list(self.root.iterdir()), [])
        path.write_bytes(original)
        (self.source / next(iter(pico.PROVENANCE))).write_bytes(b"{}")
        with self.assertRaisesRegex(ValueError, "source identity"):
            pico.install(self.source, self.root)
        self.assertEqual(list(self.root.iterdir()), [])

    def test_foreign_initramfs_and_quest_units_are_refused(self):
        path = self.root / "etc/initrd-release"
        path.parent.mkdir()
        path.write_text("initramfs")
        with self.assertRaisesRegex(ValueError, "not an initramfs"):
            pico.install(self.source, self.root)
        path.unlink()
        path = self.root / "usr/lib/systemd/system/armada-quest-boot.service"
        path.parent.mkdir(parents=True)
        path.write_text("quest")
        with self.assertRaisesRegex(ValueError, "Quest startup"):
            pico.install(self.source, self.root)

    def test_escape_symlink_refused_before_any_file_is_installed(self):
        (self.root / "usr").symlink_to(self.source / "usr")
        with self.assertRaisesRegex(ValueError, "Unsafe Pico runtime destination"):
            pico.install(self.source, self.root)
        self.assertFalse((self.source / "usr/libexec").exists())

    def test_image_readback_rejects_absolute_and_parent_paths(self):
        for index, name in enumerate(("/usr/bin/qrtr-ns", "../qrtr-ns", "usr/../../qrtr-ns")):
            manifest = {"files": [{"path": name}], "links": {}}
            with self.subTest(name=name), patch.object(pico.subprocess, "run") as run:
                with self.assertRaisesRegex(ValueError, "Invalid Pico runtime image path"):
                    pico.verify_image("unused.ext4", manifest, self.root / str(index))
                run.assert_not_called()
