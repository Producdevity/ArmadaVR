import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest


spec = importlib.util.spec_from_file_location("initramfs", Path(__file__).parents[1] / "tools/build-headset-initramfs.py")
initramfs = importlib.util.module_from_spec(spec)
spec.loader.exec_module(initramfs)


class InitramfsInputTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.modules = [{"name": ["ufs_qcom"], "vermagic": ["5.10.246 SMP aarch64"]}]
        self.report = {"status": "compiled", "variant": "linux-userspace", "qemu_transports": False,
                       "module_count": 1, "artifacts": []}
        for name in ("Image", "modules.tar.gz", "resolved.config"):
            (self.root / name).write_bytes(name.encode())
        self.save()

    def tearDown(self):
        self.temp.cleanup()

    def save(self):
        (self.root / "modules.json").write_text(json.dumps(self.modules))
        self.report["artifacts"] = [{"path": name, "sha256": hashlib.sha256((self.root / name).read_bytes()).hexdigest()}
                                    for name in ("Image", "modules.tar.gz", "modules.json", "resolved.config")]
        (self.root / "build.json").write_text(json.dumps(self.report))

    def test_consistent_kernel_inventory(self):
        _, _, version, names = initramfs.inputs(self.root)
        self.assertEqual(version, "5.10.246")
        self.assertEqual(names, ["ufs_qcom"])

    def test_changed_module_archive_is_rejected(self):
        (self.root / "modules.tar.gz").write_bytes(b"different kernel")
        with self.assertRaisesRegex(ValueError, "checksum mismatch"):
            initramfs.inputs(self.root)

    def test_kernel_variant_cannot_masquerade_as_hardware(self):
        self.report["qemu_transports"] = True
        self.save()
        with self.assertRaisesRegex(ValueError, "transport flag disagree"):
            initramfs.inputs(self.root)

    def test_mixed_module_releases_and_count_are_rejected(self):
        self.modules.append({"name": ["other"], "vermagic": ["5.10.237 SMP aarch64"]})
        self.report["module_count"] = 2
        self.save()
        with self.assertRaisesRegex(ValueError, "Inconsistent kernel"):
            initramfs.inputs(self.root)
        self.modules.pop()
        self.save()
        with self.assertRaisesRegex(ValueError, "Inconsistent kernel"):
            initramfs.inputs(self.root)

    def test_uncompiled_kernel_and_invalid_module_name_are_rejected(self):
        self.report["status"] = "failed"
        self.save()
        with self.assertRaisesRegex(ValueError, "compiled"):
            initramfs.inputs(self.root)
        self.report["status"] = "compiled"
        self.modules[0]["name"] = ["../foreign"]
        self.save()
        with self.assertRaisesRegex(ValueError, "module name"):
            initramfs.inputs(self.root)

    def test_symlink_and_device_inputs_are_rejected(self):
        original = self.root / "Image"
        original.rename(self.root / "real-image")
        original.symlink_to("real-image")
        with self.assertRaisesRegex(ValueError, "regular file"):
            initramfs.inputs(self.root)
        with self.assertRaisesRegex(ValueError, "regular file"):
            initramfs.digest(Path("/dev/null"))
