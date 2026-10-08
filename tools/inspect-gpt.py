#!/usr/bin/env python3
"""Validate separate primary/backup GPT headers and arrays from local backups."""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import struct
import uuid
import zlib

HEADER = struct.Struct("<8sIIIIQQQQ16sQIII")
MAX_ARRAY_BYTES = 1024 * 1024


def header(data, sector_size, total_sectors, current_lba):
    if len(data) != sector_size:
        raise ValueError("GPT header must contain exactly one logical sector")
    fields = HEADER.unpack_from(data)
    (signature, revision, size, crc, reserved, current, alternate, first, last,
     disk_guid, entries_lba, entry_count, entry_size, entries_crc) = fields
    if signature != b"EFI PART" or revision != 0x10000:
        raise ValueError("Unsupported GPT signature or revision")
    if not HEADER.size <= size <= sector_size or reserved or any(data[size:]):
        raise ValueError("Invalid GPT header size or reserved bytes")
    checked = bytearray(data[:size])
    checked[16:20] = bytes(4)
    if zlib.crc32(checked) != crc:
        raise ValueError("GPT header CRC mismatch")
    expected_alternate = total_sectors - 1 if current_lba == 1 else 1
    if current != current_lba or alternate != expected_alternate:
        raise ValueError("GPT header locations disagree with device capacity")
    if not 2 <= first <= last < total_sectors - 1 or not any(disk_guid):
        raise ValueError("Invalid GPT usable range or disk GUID")
    if entry_size < 128 or entry_size & (entry_size - 1) or not entry_count:
        raise ValueError("Invalid GPT entry dimensions")
    array_bytes = entry_count * entry_size
    if array_bytes > MAX_ARRAY_BYTES:
        raise ValueError("GPT entry array exceeds the inspection limit")
    blocks = (array_bytes + sector_size - 1) // sector_size
    if current_lba == 1:
        if not 2 <= entries_lba or entries_lba + blocks > first:
            raise ValueError("Primary GPT array overlaps a header or usable storage")
    elif not last < entries_lba or entries_lba + blocks > current_lba:
        raise ValueError("Backup GPT array overlaps a header or usable storage")
    return {"revision": revision, "header_size": size, "current_lba": current,
            "alternate_lba": alternate, "first_usable_lba": first,
            "last_usable_lba": last, "disk_guid": str(uuid.UUID(bytes_le=disk_guid)),
            "entries_lba": entries_lba, "entry_count": entry_count,
            "entry_size": entry_size, "entries_crc32": entries_crc,
            "array_bytes": array_bytes, "array_sectors": blocks}


def array(data, info, sector_size):
    if len(data) != info["array_sectors"] * sector_size:
        raise ValueError("GPT array must contain its complete logical sectors")
    entries = data[:info["array_bytes"]]
    if zlib.crc32(entries) != info["entries_crc32"]:
        raise ValueError("GPT entry-array CRC mismatch")
    return entries


def partitions(data, info, sector_size):
    result, seen_guids = [], set()
    for index in range(info["entry_count"]):
        entry = data[index * info["entry_size"]:(index + 1) * info["entry_size"]]
        type_guid, part_guid = entry[:16], entry[16:32]
        if not any(type_guid):
            continue
        first, last, attributes = struct.unpack_from("<QQQ", entry, 32)
        if not info["first_usable_lba"] <= first <= last <= info["last_usable_lba"]:
            raise ValueError(f"Partition {index + 1} is outside usable storage")
        if not any(part_guid) or part_guid in seen_guids:
            raise ValueError(f"Partition {index + 1} has a missing or duplicate GUID")
        seen_guids.add(part_guid)
        try:
            name = entry[56:128].decode("utf-16le").split("\0", 1)[0]
        except UnicodeDecodeError as error:
            raise ValueError(f"Partition {index + 1} has an invalid UTF-16 name") from error
        result.append({"index": index + 1, "name": name,
                       "type_guid": str(uuid.UUID(bytes_le=type_guid)),
                       "partition_guid": str(uuid.UUID(bytes_le=part_guid)),
                       "first_lba": first, "last_lba": last,
                       "sectors": last - first + 1,
                       "bytes": (last - first + 1) * sector_size,
                       "attributes": attributes})
    ordered = sorted(result, key=lambda part: part["first_lba"])
    for previous, following in zip(ordered, ordered[1:]):
        if previous["last_lba"] >= following["first_lba"]:
            raise ValueError("GPT partitions overlap")
    return result


def inspect(primary_header, primary_entries, backup_header, backup_entries,
            sector_size, total_sectors):
    if sector_size not in (512, 4096) or not 6 <= total_sectors <= (2**64 - 1) // sector_size:
        raise ValueError("Invalid logical sector size or device capacity")
    primary = header(primary_header, sector_size, total_sectors, 1)
    backup = header(backup_header, sector_size, total_sectors, total_sectors - 1)
    shared = ("revision", "header_size", "first_usable_lba", "last_usable_lba",
              "disk_guid", "entry_count", "entry_size", "entries_crc32")
    if any(primary[key] != backup[key] for key in shared):
        raise ValueError("Primary and backup GPT metadata disagree")
    first = array(primary_entries, primary, sector_size)
    second = array(backup_entries, backup, sector_size)
    if first != second:
        raise ValueError("Primary and backup GPT arrays disagree")
    entries = partitions(first, primary, sector_size)
    names = Counter(entry["name"] for entry in entries if entry["name"])
    artifacts = {name: {"bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}
                 for name, data in (("primary_header", primary_header),
                                    ("primary_entries", primary_entries),
                                    ("backup_header", backup_header),
                                    ("backup_entries", backup_entries))}
    return {"schema_version": 1, "purpose": "offline GPT backup validation",
            "device_accessed": False, "restore_verified": False,
            "sector_size": sector_size, "total_sectors": total_sectors,
            "bytes": sector_size * total_sectors, "primary": primary, "backup": backup,
            "partitions": entries, "ambiguous_names": sorted(name for name, count in names.items() if count > 1),
            "artifacts": artifacts}


def bounded_read(path, limit):
    if path.is_symlink() or not path.is_file() or path.stat().st_size > limit:
        raise ValueError(f"Expected a bounded regular backup file: {path}")
    with path.open("rb") as stream:
        data = stream.read(limit + 1)
    if len(data) > limit:
        raise ValueError(f"Backup exceeds the inspection limit: {path}")
    return data


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("primary_header", "primary_entries", "backup_header", "backup_entries"):
        parser.add_argument(name, type=Path)
    parser.add_argument("--sector-size", type=int, choices=(512, 4096), required=True)
    parser.add_argument("--total-sectors", type=int, required=True)
    args = parser.parse_args()
    try:
        result = inspect(bounded_read(args.primary_header, args.sector_size),
                         bounded_read(args.primary_entries, MAX_ARRAY_BYTES),
                         bounded_read(args.backup_header, args.sector_size),
                         bounded_read(args.backup_entries, MAX_ARRAY_BYTES),
                         args.sector_size, args.total_sectors)
        print(json.dumps(result, indent=2))
    except (OSError, ValueError) as error:
        parser.exit(1, f"{error}\n")


if __name__ == "__main__":
    main()
