import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location("kernel_abi", Path(__file__).parents[1] / "tools/test-kernel-abi.py")
abi = importlib.util.module_from_spec(spec)
spec.loader.exec_module(abi)


class KernelAbiTests(unittest.TestCase):
    def fixture(self, root):
        kernel, vm = root / "kernel", root / "vm"
        kernel.mkdir()
        vm.mkdir()
        sums = {}
        for directory, name in ((kernel, "Image"), (vm, "rootfs.ext4"), (vm, "initramfs.img")):
            data = name.encode()
            (directory / name).write_bytes(data)
            sums[name] = hashlib.sha256(data).hexdigest()
        (kernel / "build.json").write_text(json.dumps({"status": "compiled", "qemu_transports": True,
            "artifacts": [{"path": "Image", "sha256": sums["Image"]}]}))
        (vm / "manifest.json").write_text(json.dumps({"target": "qemu-arm64", "hardware_flash_image": False,
                                                    "sha256": sums}))
        return kernel, vm

    def test_changed_kernel_or_rootfs_is_rejected(self):
        for name in ("Image", "rootfs.ext4", "initramfs.img"):
            with self.subTest(name=name), tempfile.TemporaryDirectory() as directory:
                kernel, vm = self.fixture(Path(directory))
                self.assertEqual(len(abi.inputs(kernel, vm)), 3)
                ((kernel if name == "Image" else vm) / name).write_bytes(b"changed")
                with self.assertRaisesRegex(ValueError, "Checksum mismatch"):
                    abi.inputs(kernel, vm)

    def test_headset_variant_is_not_a_virtual_kernel(self):
        with tempfile.TemporaryDirectory() as directory:
            kernel, vm = self.fixture(Path(directory))
            (kernel / "build.json").write_text(json.dumps({"status": "compiled", "qemu_transports": False}))
            with self.assertRaisesRegex(ValueError, "QEMU transport"):
                abi.inputs(kernel, vm)

    def test_symlink_rootfs_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            kernel, vm = self.fixture(Path(directory))
            disk = vm / "rootfs.ext4"
            disk.rename(vm / "disk")
            disk.symlink_to("disk")
            with self.assertRaisesRegex(ValueError, "regular image"):
                abi.inputs(kernel, vm)
