#!/usr/bin/env python3
"""Audit saved Pico partition backups against both GPT copies, without USB access."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import stat


spec = importlib.util.spec_from_file_location("inspect_gpt", Path(__file__).with_name("inspect-gpt.py"))
gpt = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gpt)
GPT_PARTS = ("primary-header", "primary-entries", "backup-header", "backup-entries")


def regular(path, limit, contents=False):
    fd = os.open(path, os.O_RDONLY | os.O_NONBLOCK | os.O_NOFOLLOW)
    with os.fdopen(fd, "rb") as stream:
        before = os.fstat(stream.fileno())
        if not stat.S_ISREG(before.st_mode) or not 0 < before.st_size <= limit:
            raise ValueError(f"Expected a nonempty bounded regular file: {path}")
        data = stream.read() if contents else None
        digest = hashlib.sha256(data).hexdigest() if contents else hashlib.file_digest(stream, "sha256").hexdigest()
        after = os.fstat(stream.fileno())
        if (before.st_size, before.st_mtime_ns, before.st_ctime_ns) != (after.st_size, after.st_mtime_ns, after.st_ctime_ns):
            raise ValueError(f"Input changed during inspection: {path}")
    return data, {"bytes": before.st_size, "sha256": digest}


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"Duplicate JSON key: {key}")
        result[key] = value
    return result


def document(path):
    data, record = regular(path, 1024 * 1024, True)
    return json.loads(data, object_pairs_hook=unique_object), record


def audit(directory, manifest_sha256, required):
    if not re.fullmatch(r"[0-9a-f]{64}", manifest_sha256):
        raise ValueError("An independently retained manifest SHA-256 is required")
    if not required or len(required) != len(set(required)) or any(not re.fullmatch(r"[a-z0-9_]+", name) for name in required):
        raise ValueError("Specify distinct required partition names")
    manifest, manifest_record = document(directory / "backup-manifest.json")
    if manifest_record["sha256"] != manifest_sha256 or manifest.get("status") != "passed":
        raise ValueError("Backup manifest identity or acquisition status mismatch")
    tables, inventory, evidence = [], {}, {"backup-manifest.json": manifest_record}
    for lun in range(6):
        geometry, record = document(directory / f"lun{lun}-gpt.json")
        evidence[f"lun{lun}-gpt.json"] = record
        raw = []
        for part in GPT_PARTS:
            name = f"lun{lun}-{part}.bin"
            data, record = regular(directory / name, gpt.MAX_ARRAY_BYTES, True)
            evidence[name] = record
            raw.append(data)
        checked = gpt.inspect(*raw, geometry["sector_size"], geometry["total_sectors"])
        recorded = {key: value for key, value in geometry.items() if key != "lun"}
        if geometry.get("lun", lun) != lun or checked != recorded or checked["ambiguous_names"]:
            raise ValueError(f"LUN {lun}: recorded geometry differs from raw GPT or has ambiguous names")
        tables.append({"lun": lun, "sector_size": checked["sector_size"],
                       "total_sectors": checked["total_sectors"], "disk_guid": checked["primary"]["disk_guid"]})
        for item in checked["partitions"]:
            if not item["name"] or item["name"] in inventory:
                raise ValueError("Missing or ambiguous partition name across LUNs")
            inventory[item["name"]] = {"lun": lun, **item}
    acquired, seen_files = {}, set()
    for item in manifest["partitions"]:
        name, filename = item["name"], item["file"]
        if name in acquired or filename in seen_files or filename != name + ".bin" or not re.fullmatch(r"[a-z0-9_]+", name):
            raise ValueError("Repeated or invalid backup name/path")
        if name not in inventory or any(item.get(k) != v for k, v in inventory[name].items()):
            raise ValueError(f"Backup extent or partition identity differs from GPT: {name}")
        if type(item.get("device_read_passes")) is not int or item["device_read_passes"] < 2 or item.get("host_digest_verified") is not True:
            raise ValueError(f"Missing repeated-read acquisition evidence: {name}")
        _, actual = regular(directory / filename, item["bytes"])
        if actual != {"bytes": item["bytes"], "sha256": item["sha256"]}:
            raise ValueError(f"Backup bytes differ from acquisition manifest: {name}")
        sector_size = tables[item["lun"]]["sector_size"]
        acquired[name] = {**inventory[name], "sector_size": sector_size,
                          "byte_offset": item["first_lba"] * sector_size, "file": filename, **actual}
        evidence[filename] = actual
        seen_files.add(filename)
    total = sum(item["bytes"] for item in acquired.values())
    if total != manifest["total_bytes"]:
        raise ValueError("Backup manifest total disagrees with acquired files")
    unknown = sorted(set(required) - inventory.keys())
    if unknown:
        raise ValueError("Requested partitions absent from GPT: " + ", ".join(unknown))
    missing = sorted(set(required) - acquired.keys())
    return {"schema_version": 1, "scope": "offline partition backup integrity and coverage",
            "status": "incomplete" if missing else "verified",
            "device_accessed": False, "restore_verified": False, "flash_ready": False,
            "live_device_identity_verified": False, "cold_recovery_verified": False,
            "luns": tables, "partition_count": len(inventory), "backup_count": len(acquired),
            "backup_bytes": total, "requested_partitions": required,
            "missing_backups": missing, "covered_partitions": [acquired[name] for name in required if name in acquired],
            "not_backed_up": sorted(inventory.keys() - acquired.keys()), "inputs": evidence}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("--manifest-sha256", required=True)
    parser.add_argument("--require", action="append", required=True, metavar="PARTITION")
    args = parser.parse_args()
    try:
        result = audit(args.directory, args.manifest_sha256, args.require)
    except (OSError, ValueError, KeyError, TypeError) as error:
        parser.exit(1, f"{error}\n")
    print(json.dumps(result, indent=2))
    return 2 if result["missing_backups"] else 0


if __name__ == "__main__":
    raise SystemExit(main())
