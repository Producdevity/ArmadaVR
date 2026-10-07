import importlib.util
import struct
import unittest
from pathlib import Path

spec = importlib.util.spec_from_file_location("kernel", Path(__file__).parents[1] / "tools/extract-kernel.py")
kernel = importlib.util.module_from_spec(spec)
spec.loader.exec_module(kernel)


class KernelTests(unittest.TestCase):
    def test_existing_arm64_image_is_preserved(self):
        data = bytearray(128)
        data[56:60] = b"ARM\x64"
        self.assertEqual(kernel.extract(bytes(data)), data)

    def test_rejects_non_kernel(self):
        with self.assertRaises(ValueError):
            kernel.extract(b"not a kernel")

    def test_rejects_truncated_compressed_payload(self):
        data = bytearray(64)
        data[:8] = b"MZ\0\0zimg"
        struct.pack_into("<II", data, 8, 64, 1024)
        with self.assertRaises(ValueError):
            kernel.extract(bytes(data))


if __name__ == "__main__":
    unittest.main()
