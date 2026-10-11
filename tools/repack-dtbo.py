#!/usr/bin/env python3
"""Replace one overlay in a contiguous Android v0 DTBO table, offline only."""
import argparse
import hashlib
import json
from pathlib import Path
import struct

from devicetree import read_fdt


def digest(data):
    return hashlib.sha256(data).hexdigest()


def read_source(path, expected):
    if path.is_symlink() or not path.is_file() or not 32 <= path.stat().st_size <= 64 * 1024**2:
        raise ValueError("Expected a regular image of 32 bytes to 64 MiB")
    data = path.read_bytes()
    if digest(data) != expected:
        raise ValueError("Image SHA-256 mismatch")
    return data


def table(data):
    if len(data) < 32:
        raise ValueError("Truncated DTBO header")
    magic, total, header, entry_size, count, offset, page, version = struct.unpack_from(">8I", data)
    if ((magic, header, entry_size, offset, page, version) != (0xd7b7ab1e, 32, 32, 32, 4096, 0) or
            not 1 <= count <= 128 or not offset + count * entry_size <= total <= len(data)):
        raise ValueError("Unsupported DTBO table geometry")
    entries, cursor = [], offset + count * entry_size
    for index in range(count):
        size, start, *identifiers = struct.unpack_from(">8I", data, offset + index * entry_size)
        if start != cursor or not 40 <= size <= 4 * 1024**2 or start + size > total:
            raise ValueError("Requires bounded, contiguous DTBO entries in table order")
        payload = data[start:start + size]
        if struct.unpack_from(">II", payload) != (0xd00dfeed, size):
            raise ValueError("Overlay size disagrees with its FDT header")
        read_fdt(payload)
        entries.append({"index": index, "offset": start, "bytes": size,
                        "identifiers": identifiers, "sha256": digest(payload)})
        cursor += size
    if cursor != total:
        raise ValueError("Unaccounted bytes in DTBO table")
    return total, entries


def replace(data, payload, index, original_sha):
    total, entries = table(data)
    if type(index) is not int or not 0 <= index < len(entries):
        raise ValueError("Overlay index outside table")
    entry = entries[index]
    if entry["sha256"] != original_sha:
        raise ValueError("Selected original overlay SHA-256 mismatch")
    if not 40 <= len(payload) <= 4 * 1024**2 or struct.unpack_from(">II", payload) != (0xd00dfeed, len(payload)):
        raise ValueError("Requires one complete replacement FDT")
    read_fdt(payload)
    result = bytearray(data[:entry["offset"]] + payload + data[entry["offset"] + entry["bytes"]:total])
    delta = len(payload) - entry["bytes"]
    struct.pack_into(">I", result, 4, len(result))
    struct.pack_into(">I", result, 32 + index * 32, len(payload))
    for item in entries[index + 1:]:
        struct.pack_into(">I", result, 32 + item["index"] * 32 + 4, item["offset"] + delta)
    _, after = table(result)
    for before, current in zip(entries, after):
        if before["identifiers"] != current["identifiers"]:
            raise ValueError("Overlay identifiers changed")
        expected = digest(payload) if before["index"] == index else before["sha256"]
        if current["sha256"] != expected:
            raise ValueError("Unexpected overlay payload change")
    return bytes(result), entries, after


def build(source, replacement, output, source_sha, replacement_sha, index, original_sha, limit):
    if output.exists() or output.is_symlink():
        raise ValueError("Choose a new DTBO output directory")
    data, payload = read_source(source, source_sha), read_source(replacement, replacement_sha)
    result, before, after = replace(data, payload, index, original_sha)
    if not len(result) <= limit <= 64 * 1024**2:
        raise ValueError("Unsigned DTBO exceeds the explicit size limit")
    output.mkdir(parents=True)
    target = output / "dtbo-unsigned.img"
    target.write_bytes(result)
    if source.read_bytes() != data or replacement.read_bytes() != payload or target.read_bytes() != result:
        raise ValueError("DTBO inputs or output changed during assembly")
    total, _ = table(data)
    report = {"schema": 1, "status": "assembled", "source_sha256": source_sha,
              "replacement_sha256": replacement_sha, "index": index, "entries_before": before,
              "entries_after": after, "source_bytes_after_table_omitted": len(data) - total,
              "output_bytes": len(result), "output_sha256": digest(result), "size_limit": limit,
              "unsigned": True, "source_inputs_unchanged": True, "other_overlays_unchanged": True,
              "boot_verified": False, "recovery_verified": False, "flash_ready": False}
    (output / "build.json").write_text(json.dumps(report, indent=2) + "\n")
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("replacement", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--source-sha256", required=True)
    parser.add_argument("--replacement-sha256", required=True)
    parser.add_argument("--index", required=True, type=int)
    parser.add_argument("--original-overlay-sha256", required=True)
    parser.add_argument("--max-bytes", required=True, type=int)
    args = parser.parse_args()
    try:
        print(json.dumps(build(args.source, args.replacement, args.output, args.source_sha256,
                               args.replacement_sha256, args.index, args.original_overlay_sha256,
                               args.max_bytes), indent=2))
    except (OSError, ValueError) as error:
        parser.exit(1, "Cannot repack DTBO: " + str(error) + "\n")


if __name__ == "__main__":
    main()
