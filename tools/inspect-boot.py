#!/usr/bin/env python3
"""Inspect an offline Android boot/vendor_boot file without unpacking or modifying it."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import stat
import struct


def layout(header, file_size):
    def u32(offset):
        if offset + 4 > len(header):
            raise ValueError("Truncated Android header")
        return struct.unpack_from("<I", header, offset)[0]

    def text(offset, size):
        return header[offset:offset + size].split(b"\0", 1)[0].decode("utf-8", errors="replace")

    magic = header[:8]
    parts = []
    if magic == b"ANDROID!":
        version = u32(40)
        if version not in range(5):
            raise ValueError(f"Unsupported boot header version: {version}")
        minimum = (1632, 1648, 1660, 1580, 1584)[version]
        header_size = u32(20) if version >= 3 else u32(1644) if version else minimum
        page_size = 4096 if version >= 3 else u32(36)
        parts = [("kernel", u32(8)), ("ramdisk", u32(12 if version >= 3 else 16))]
        if version < 3:
            parts.append(("second", u32(24)))
            if version >= 1:
                parts.append(("recovery_dtbo", u32(1632)))
            if version == 2:
                parts.append(("dtb", u32(1648)))
            command_line = text(64, 512) + text(608, 1024)
        else:
            command_line = text(44, 1536)
            if version == 4:
                parts.append(("boot_signature", u32(1580)))
        kind = "boot"
    elif magic == b"VNDRBOOT":
        version = u32(8)
        if version not in (3, 4):
            raise ValueError(f"Unsupported vendor_boot header version: {version}")
        minimum = 2112 if version == 3 else 2128
        header_size, page_size = u32(2096), u32(12)
        parts = [("vendor_ramdisk", u32(24)), ("dtb", u32(2100))]
        if version == 4:
            table_size, entry_count, entry_size = u32(2112), u32(2116), u32(2120)
            if table_size != entry_count * entry_size or (entry_count and entry_size < 108):
                raise ValueError("Invalid vendor ramdisk table geometry")
            parts += [("vendor_ramdisk_table", table_size), ("bootconfig", u32(2124))]
        command_line = text(28, 2048)
        kind = "vendor_boot"
    else:
        raise ValueError("Expected ANDROID! or VNDRBOOT magic; this is not an Android boot container")
    if header_size != minimum or len(header) < minimum or file_size < minimum:
        raise ValueError("Truncated or inconsistent Android header size")
    if page_size not in (2048, 4096, 8192, 16384):
        raise ValueError(f"Unsupported page size: {page_size}")
    if kind == "boot" and version < 3 and page_size < header_size:
        raise ValueError("Legacy boot header does not fit its header page")
    align = lambda size: (size + page_size - 1) // page_size * page_size
    cursor = align(header_size)
    sections = []
    for name, size in parts:
        if name == "recovery_dtbo" and size:
            declared, = struct.unpack_from("<Q", header, 1636)
            if declared != cursor:
                raise ValueError("Recovery DTBO offset disagrees with boot image geometry")
        if size:
            if cursor + size > file_size:
                raise ValueError(f"Truncated {name} payload")
            sections.append({"name": name, "offset": cursor, "size": size})
        cursor += align(size)
    if cursor > file_size:
        raise ValueError("Truncated Android image padding")
    return {"format": kind, "header_version": version, "header_size": header_size,
            "page_size": page_size, "cmdline": command_line, "sections": sections,
            "payload_end_aligned": cursor, "trailing_bytes": file_size - cursor,
            "signature_verification": "not performed", "headset_compatibility": "not established"}


def inspect(path):
    # Device files are excluded: this command accepts local copies only.
    if not stat.S_ISREG(path.stat().st_mode):
        raise ValueError("Input must be a regular file, not a device")
    with path.open("rb") as stream:
        info = os.fstat(stream.fileno())
        if not stat.S_ISREG(info.st_mode):
            raise ValueError("Input must be a regular file")
        header = stream.read(4096)
        report = layout(header, info.st_size)
        for section in report["sections"]:
            stream.seek(section["offset"])
            remaining = section["size"]
            digest = hashlib.sha256()
            while remaining:
                chunk = stream.read(min(remaining, 1024**2))
                if not chunk:
                    raise ValueError("Image changed or became truncated while inspecting")
                digest.update(chunk)
                remaining -= len(chunk)
            section["sha256"] = digest.hexdigest()
        if report["format"] == "vendor_boot" and report["header_version"] == 4:
            table_size, count, entry_size = struct.unpack_from("<III", header, 2112)
            if table_size > 1024**2:
                raise ValueError("Vendor ramdisk table exceeds the 1 MiB inspection limit")
            sections = {part["name"]: part for part in report["sections"]}
            table = sections.get("vendor_ramdisk_table")
            stream.seek(table["offset"] if table else 0)
            report["vendor_ramdisk_fragments"] = ramdisk_fragments(
                stream.read(table_size), count, entry_size,
                sections.get("vendor_ramdisk", {}).get("size", 0))
        stream.seek(0)
        report["sha256"] = hashlib.file_digest(stream, "sha256").hexdigest()
        report["size"] = info.st_size
        if info.st_size >= 64:
            stream.seek(-64, 2)
            report["avb_footer_magic_present"] = stream.read(4) == b"AVBf"
        return report


def ramdisk_fragments(table, count, entry_size, ramdisk_size):
    if len(table) != count * entry_size or (count and entry_size < 108):
        raise ValueError("Invalid vendor ramdisk table geometry")
    fragments = []
    for index in range(count):
        size, offset, kind, name, *board_id = struct.unpack_from("<III32s16I", table, index * entry_size)
        if offset + size > ramdisk_size:
            raise ValueError("Vendor ramdisk fragment lies outside its ramdisk section")
        fragments.append({"name": name.split(b"\0", 1)[0].decode("utf-8", errors="replace"),
                          "type": kind, "offset": offset, "size": size, "board_id": board_id})
    ranges = sorted((part["offset"], part["offset"] + part["size"])
                    for part in fragments if part["size"])
    if any(left[1] > right[0] for left, right in zip(ranges, ranges[1:])):
        raise ValueError("Overlapping vendor ramdisk fragments")
    return fragments


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", type=Path)
    args = parser.parse_args()
    try:
        print(json.dumps(inspect(args.image), indent=2))
    except (OSError, ValueError) as error:
        parser.exit(1, f"{error}\n")


if __name__ == "__main__":
    main()
