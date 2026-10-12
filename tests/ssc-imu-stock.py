#!/usr/bin/env python3
"""Compare SSC IMU decoding with the retained b1977 protobuf descriptors."""

import argparse
import hashlib
import json
import math
from pathlib import Path
import random
import struct
import subprocess

from google.protobuf import descriptor_pb2, descriptor_pool, message_factory


def digest(data):
    return hashlib.sha256(data).hexdigest()


def run(args):
    binary = args.stock_library.read_bytes()
    library_sha = "9adc9c3c6aee833e3566bf3574844c038098385abd7c94c81aeedfd07edc0fa8"
    if digest(binary) != library_sha:
        raise ValueError("Expected the exact b1977 libssc.so")
    pool = descriptor_pool.DescriptorPool()
    pool.AddSerializedFile(descriptor_pb2.DESCRIPTOR.serialized_pb)
    descriptors = [
        ("nanopb.proto", 575989, 836, "72f91e0c896327e9d85e1269ea60655662407490c4cb0ee484368cf4aba5ec32"),
        ("sns_std_sensor.proto", 609204, 2417, "6868756f1e95aea4024c113cde31dc3839354714c24f93a7657028f78d9bc51b"),
    ]
    for name, offset, size, expected in descriptors:
        raw = binary[offset:offset + size]
        if digest(raw) != expected:
            raise ValueError("Descriptor changed: " + name)
        pool.AddSerializedFile(raw)
    message = message_factory.GetMessageClass(pool.FindMessageTypeByName("sns_std_sensor_event"))
    args.output.mkdir(parents=True, exist_ok=False)
    corpus = bytearray()
    rng = random.Random(0x53534349)
    accepted = refused = 0

    def add(payload):
        nonlocal accepted, refused
        parsed = message.FromString(payload)
        valid = parsed.IsInitialized() and len(parsed.data) == 3 and all(math.isfinite(x) for x in parsed.data)
        expected = struct.pack("<3fI", *parsed.data, parsed.status) if valid else bytes(16)
        corpus.extend(struct.pack("<II", len(payload), valid) + expected + payload)
        accepted += bool(valid)
        refused += not valid

    for index in range(4096):
        words = [rng.getrandbits(32) for _ in range(3)]
        values = struct.unpack("<3f", struct.pack("<3I", *words))
        status = index % 4
        generated = message(data=values, status=status)
        add(generated.SerializeToString())
        packed = struct.pack("<3f", *values)
        parts = [b"\x0d" + packed[:4], b"\x0a\x08" + packed[4:]]
        if index & 1:
            parts = [b"\x0a\x0c" + packed]
        parts.insert(rng.randrange(len(parts) + 1), b"\x10" + bytes([status]))
        parts.insert(rng.randrange(len(parts) + 1), b"\x9a\x06\x03abc")
        add(b"".join(parts))
    for count in range(7):
        for status in range(4):
            add(message(data=[float(i) for i in range(count)], status=status).SerializeToString())
    add(message(data=[1, 2, 3]).SerializePartialToString())
    for value in (float("nan"), float("inf"), -float("inf"), -0.0, 0.0, 2 ** -149):
        add(message(data=[value, 1, -1], status=0).SerializeToString())
    path = args.output / "corpus.bin"
    path.write_bytes(corpus)
    result = subprocess.run([str(args.test_binary.resolve()), str(path.resolve())],
                            capture_output=True, text=True, timeout=30)
    (args.output / "comparison.log").write_text(result.stdout + result.stderr)
    passed = result.returncode == 0 and f"SSC_IMU_CORPUS_PASS {accepted + refused}" in result.stdout
    report = {"status": "passed" if passed else "failed", "cases": accepted + refused,
              "accepted": accepted, "refused": refused, "corpus_sha256": digest(corpus),
              "test_binary_sha256": digest(args.test_binary.read_bytes()), "stock_library_sha256": library_sha,
              "descriptors": descriptors, "physical_stream_verified": False}
    (args.output / "result.json").write_text(json.dumps(report, indent=2) + "\n")
    if not passed:
        raise ValueError("SSC IMU comparison failed: " + result.stdout + result.stderr)
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--stock-library", required=True, type=Path)
    parser.add_argument("--test-binary", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    run(parser.parse_args())
