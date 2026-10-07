import importlib.util
import hashlib
import json
import tempfile
import unittest
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).parents[1] / "tools"))

spec = importlib.util.spec_from_file_location("vm", Path(__file__).parents[1] / "tools/run-vm.py")
vm = importlib.util.module_from_spec(spec)
spec.loader.exec_module(vm)


class VmTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        (self.root / "manifest.json").write_text(json.dumps({"target": "qemu-arm64", "hardware_flash_image": False}))
        for name in ("Image", "initramfs.img", "rootfs.ext4"):
            (self.root / name).touch()

    def test_snapshot_and_no_hardware_passthrough(self):
        cmd = vm.command(self.root, "qemu", "tcg")
        self.assertIn("snapshot=on", cmd[-1])
        self.assertIn("-nodefaults", cmd)
        self.assertEqual(cmd[cmd.index("-nic") + 1], "none")
        self.assertEqual(cmd[cmd.index("-m") + 1], "2048")
        self.assertNotIn("-device", cmd)

    def test_hardware_profile_is_rejected(self):
        (self.root / "manifest.json").write_text('{"target":"quest3","hardware_flash_image":false}')
        with self.assertRaises(ValueError):
            vm.command(self.root, "qemu", "tcg")

    def test_network_requires_desktop(self):
        with self.assertRaises(ValueError):
            vm.command(self.root, "qemu", "tcg", network=True)

    def test_hvf_disables_sme_with_explicit_regression_override(self):
        for accel, override, expected in (("hvf", None, True), ("hvf", False, False), ("tcg", None, False)):
            cmd = vm.command(self.root, "qemu", accel, disable_sme=override)
            self.assertEqual("arm64.nosme" in cmd[cmd.index("-append") + 1], expected)

    def test_desktop_network_has_no_forwarded_ports(self):
        (self.root / "manifest.json").write_text(json.dumps({"target": "qemu-arm64", "hardware_flash_image": False, "desktop_available": True}))
        cmd = vm.command(self.root, "qemu", "tcg", desktop=True, network=True)
        self.assertEqual(cmd[cmd.index("-nic") + 1], "user,model=virtio-net-pci")

    def test_desktop_requires_a_desktop_image(self):
        with self.assertRaises(ValueError):
            vm.command(self.root, "qemu", "tcg", desktop=True)

    def test_desktop_uses_overlay_and_only_emulated_input(self):
        (self.root / "manifest.json").write_text(json.dumps({"target": "qemu-arm64", "hardware_flash_image": False, "desktop_available": True}))
        cmd = vm.command(self.root, "qemu", "tcg", desktop=True)
        self.assertIn("desktop.qcow2", cmd[-1])
        self.assertNotIn("snapshot=on", cmd[-1])
        self.assertEqual(cmd[cmd.index("-m") + 1], "6144")
        self.assertEqual(cmd[cmd.index("-nic") + 1], "none")
        self.assertIn("virtio-keyboard-pci", cmd)
        self.assertIn("virtio-tablet-pci", cmd)
        self.assertNotIn("usb-host", " ".join(cmd))

    def test_symlink_is_rejected(self):
        (self.root / "rootfs.ext4").unlink()
        (self.root / "rootfs.ext4").symlink_to("/dev/null")
        with self.assertRaises(ValueError):
            vm.command(self.root, "qemu", "tcg")

    def test_abi_override_checks_variant_and_bytes(self):
        kernel = self.root / "kernel"
        kernel.mkdir()
        (kernel / "Image").write_bytes(b"abi kernel")
        report = {"status": "compiled", "qemu_transports": True, "artifacts": [
            {"path": "Image", "sha256": hashlib.sha256(b"abi kernel").hexdigest()}]}
        (kernel / "build.json").write_text(json.dumps(report))
        cmd = vm.command(self.root, "qemu", "hvf", kernel=kernel, disable_sme=True)
        self.assertEqual(cmd[cmd.index("-kernel") + 1], str(kernel.resolve() / "Image"))
        self.assertEqual(cmd[cmd.index("-cpu") + 1], "host")
        self.assertIn("arm64.nosme", cmd[cmd.index("-append") + 1])
        (kernel / "Image").write_bytes(b"changed")
        with self.assertRaisesRegex(ValueError, "checksum mismatch"):
            vm.command(self.root, "qemu", "hvf", kernel=kernel)
        report["qemu_transports"] = False
        (kernel / "build.json").write_text(json.dumps(report))
        with self.assertRaisesRegex(ValueError, "QEMU ABI variant"):
            vm.command(self.root, "qemu", "hvf", kernel=kernel)

    def test_desktop_remembers_abi_kernel_but_snapshot_and_base_ignore_it(self):
        kernel = self.root / "remembered-kernel"
        kernel.mkdir()
        (kernel / "Image").write_bytes(b"selected kernel")
        (kernel / "build.json").write_text(json.dumps({"status": "compiled", "qemu_transports": True,
            "artifacts": [{"path": "Image", "sha256": hashlib.sha256(b"selected kernel").hexdigest()}]}))
        manifest = json.loads((self.root / "manifest.json").read_text())
        manifest.update(desktop_available=True, desktop_kernel_abi=str(kernel))
        (self.root / "manifest.json").write_text(json.dumps(manifest))
        for desktop, base, expected in ((True, False, kernel / "Image"),
                                        (False, False, self.root / "Image"),
                                        (True, True, self.root / "Image")):
            cmd = vm.command(self.root, "qemu", "tcg", desktop=desktop, base_kernel=base)
            self.assertEqual(cmd[cmd.index("-kernel") + 1], str(expected.resolve()))
        (kernel / "Image").write_bytes(b"corrupt")
        with self.assertRaisesRegex(ValueError, "checksum mismatch"):
            vm.command(self.root, "qemu", "tcg", desktop=True)

    def test_base_kernel_overrides_stale_saved_selection(self):
        manifest = json.loads((self.root / "manifest.json").read_text())
        manifest.update(desktop_available=True, desktop_kernel_abi="invalid-relative-path")
        (self.root / "manifest.json").write_text(json.dumps(manifest))
        with self.assertRaisesRegex(ValueError, "must be absolute"):
            vm.command(self.root, "qemu", "tcg", desktop=True)
        cmd = vm.command(self.root, "qemu", "tcg", desktop=True, base_kernel=True)
        self.assertEqual(cmd[cmd.index("-kernel") + 1], str((self.root / "Image").resolve()))


if __name__ == "__main__":
    unittest.main()
