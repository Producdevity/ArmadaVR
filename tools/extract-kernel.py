#!/usr/bin/env python3
"""Extract a Linux ARM64 Image from Fedora's EFI zboot wrapper for QEMU."""
import struct
import subprocess
import sys
from pathlib import Path


def extract(data):
    if data[56:60] == b"ARM\x64":
        return data
    if data[:4] != b"MZ\x00\x00" or data[4:8] != b"zimg" or len(data) < 64:
        raise ValueError("Expected a Linux ARM64 Image or EFI zboot image")
    offset, size = struct.unpack_from("<II", data, 8)
    if offset < 64 or size == 0 or offset + size > len(data):
        raise ValueError("Invalid EFI zboot payload bounds")
    compression = data[24:56].split(b"\0", 1)[0]
    if compression != b"zstd":
        raise ValueError(f"Unsupported zboot compression: {compression!r}")
    result = subprocess.run(["zstd", "-dq", "--stdout"], input=data[offset:offset+size],
                            capture_output=True, check=True, timeout=30).stdout
    if result[56:60] != b"ARM\x64":
        raise ValueError("Decompressed payload is not a Linux ARM64 Image")
    return result


if __name__ == "__main__":
    source, destination = map(Path, sys.argv[1:])
    payload = extract(source.read_bytes())
    destination.write_bytes(payload)
    print(f"ARM64 kernel: {len(payload)} bytes")
