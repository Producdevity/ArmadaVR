#!/usr/bin/env python3
"""Check local Quest boot-set integrity; never contact or modify a device."""
import argparse
from contextlib import ExitStack
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import struct
import types


ROOT = Path(__file__).resolve().parents[1]
BOOT_PARTITIONS = ("boot", "vendor_boot", "dtbo")


def open_regular(path, limit):
    fd = os.open(path, os.O_RDONLY | os.O_NONBLOCK | os.O_NOFOLLOW)
    stream = os.fdopen(fd, "rb")
    info = os.fstat(fd)
    if not stat.S_ISREG(info.st_mode) or not 0 < info.st_size <= limit:
        stream.close()
        raise ValueError(f"Expected a nonempty regular file of at most {limit} bytes: {path}")
    return stream


def load_avb(path):
    profile = json.loads((ROOT / "profiles/avb.json").read_text())
    with open_regular(path, profile["bytes"]) as stream:
        data = stream.read()
    if len(data) != profile["bytes"] or hashlib.sha256(data).hexdigest() != profile["sha256"]:
        raise ValueError("AOSP AVB tool does not match profiles/avb.json")
    module = types.ModuleType("aosp_avb")
    module.__file__ = str(path)
    exec(compile(data, str(path), "exec"), module.__dict__)
    return module


def read_exact(stream, size):
    data = stream.read(size)
    if len(data) != size:
        raise ValueError("Truncated image or metadata")
    return data


def read_avb(avb, stream):
    size = os.fstat(stream.fileno()).st_size
    if size < 256:
        raise ValueError("Image is too short for VBMeta")
    stream.seek(-64, 2)
    footer_data = read_exact(stream, 64)
    footer = avb.AvbFooter(footer_data) if footer_data[:4] == b"AVBf" else None
    offset, available = 0, size
    if footer:
        if (footer.version_major != 1 or footer.version_minor != 0 or
                not 0 < footer.original_image_size <= footer.vbmeta_offset or
                not 256 <= footer.vbmeta_size <= 1024**2 or
                footer.vbmeta_offset + footer.vbmeta_size > size - 64):
            raise ValueError("Invalid AVB footer bounds or version")
        offset, available = footer.vbmeta_offset, footer.vbmeta_size
    stream.seek(offset)
    header_blob = read_exact(stream, 256)
    header = avb.AvbVBMetaHeader(header_blob)
    auth, aux = header.authentication_data_block_size, header.auxiliary_data_block_size
    total = 256 + auth + aux
    if auth % 64 or aux % 64 or total > min(available, 1024**2):
        raise ValueError("Invalid VBMeta block bounds")
    if footer and total != footer.vbmeta_size:
        raise ValueError("AVB footer size disagrees with VBMeta")
    for field, block in (("hash", auth), ("signature", auth), ("public_key", aux),
                         ("public_key_metadata", aux), ("descriptors", aux)):
        if getattr(header, field + "_offset") + getattr(header, field + "_size") > block:
            raise ValueError(f"VBMeta {field} exceeds its block")
    if (header.required_libavb_version_major != avb.AVB_VERSION_MAJOR or
            header.required_libavb_version_minor > avb.AVB_VERSION_MINOR):
        raise ValueError("Unsupported required libavb version")
    blob = header_blob + read_exact(stream, auth + aux)
    algorithm, _ = avb.lookup_algorithm_by_type(header.algorithm_type)
    if algorithm != "NONE" and not avb.verify_vbmeta_signature(header, blob):
        raise ValueError("VBMeta signature does not match its embedded key")
    start = 256 + auth
    key = blob[start + header.public_key_offset:start + header.public_key_offset + header.public_key_size]
    begin = start + header.descriptors_offset
    descriptors_blob = blob[begin:begin + header.descriptors_size]
    cursor = 0
    while cursor < len(descriptors_blob):
        if len(descriptors_blob) - cursor < 16:
            raise ValueError("Truncated AVB descriptor header")
        _, count = struct.unpack_from("!QQ", descriptors_blob, cursor)
        if count % 8 or cursor + 16 + count > len(descriptors_blob):
            raise ValueError("Invalid AVB descriptor bounds")
        cursor += 16 + count
    descriptors = avb.parse_descriptors(descriptors_blob)
    stream.seek(0)
    record = {"bytes": size, "sha256": hashlib.file_digest(stream, "sha256").hexdigest(),
              "algorithm": algorithm, "signature": "unsigned" if algorithm == "NONE" else "verified_with_embedded_key",
              "public_key_sha256": hashlib.sha256(key).hexdigest() if key else None,
              "rollback_index": header.rollback_index, "rollback_index_location": header.rollback_index_location,
              "flags": header.flags, "original_image_size": footer.original_image_size if footer else None}
    return record, header, descriptors, key


def verify_hash(descriptor, stream, original_size):
    if (descriptor.hash_algorithm not in ("sha256", "sha512") or not descriptor.digest or
            descriptor.image_size != original_size or descriptor.flags != 0):
        raise ValueError("Unsupported boot hash algorithm, size, flags or missing digest")
    digest = hashlib.new(descriptor.hash_algorithm, descriptor.salt)
    stream.seek(0)
    remaining = descriptor.image_size
    while remaining:
        data = read_exact(stream, min(remaining, 1024**2))
        digest.update(data)
        remaining -= len(data)
    if digest.digest() != descriptor.digest:
        raise ValueError(f"{descriptor.partition_name} payload hash mismatch")


def verify_set(directory, expected_build, avb):
    report = {"scope": "offline boot-set integrity", "boot_integrity_verified": False,
              "manufacturer_key_trust_verified": False, "ota_metadata_authenticated": False,
              "device_rollback_acceptance_verified": False, "headset_boot_acceptance_verified": False,
              "recovery_restore_verified": False, "images": {}, "chains": [], "unchecked_partitions": []}
    with ExitStack() as stack:
        with open_regular(directory / "metadata", 1024**2) as stream:
            lines = stream.read().decode().splitlines()
        metadata = {}
        for line in lines:
            key, value = line.split("=", 1)
            if key in metadata:
                raise ValueError(f"Duplicate OTA metadata field: {key}")
            metadata[key] = value
        if metadata.get("pre-device") != "eureka" or metadata.get("ota-type") != "AB":
            raise ValueError("Expected Eureka A/B OTA metadata")
        build = metadata.get("post-build-incremental")
        if not re.fullmatch(r"[0-9]+", expected_build) or build != expected_build:
            raise ValueError(f"Firmware build mismatch: expected {expected_build}, input reports {build}")
        report["declared_build"] = build
        opened, parsed = {}, {}

        def load(name):
            if not re.fullmatch(r"[a-z0-9_]+", name) or name in parsed or len(parsed) >= 16:
                raise ValueError("Invalid, repeated or excessive AVB chain partition")
            stream = stack.enter_context(open_regular(directory / (name + ".img"), 256 * 1024**2))
            opened[name] = stream
            item = read_avb(avb, stream)
            parsed[name] = item
            report["images"][name] = item[0]
            return item

        for name in BOOT_PARTITIONS:
            record, _, descriptors, _ = load(name)
            if record["original_image_size"] is None or record["flags"] != 0:
                raise ValueError(f"Missing footer or nonzero VBMeta flags in {name}")
            own = [d for d in descriptors if isinstance(d, avb.AvbHashDescriptor)]
            if len(own) != 1 or own[0].partition_name != name:
                raise ValueError(f"Expected one matching footer hash descriptor for {name}")
            verify_hash(own[0], opened[name], record["original_image_size"])
            record["footer_payload_hash_verified"] = True

        covered = set()
        locations = set()

        def verify_metadata(name, expected_key=None, chain_location=None):
            record, header, descriptors, key = load(name)
            if record["signature"] != "verified_with_embedded_key" or header.flags != 0:
                raise ValueError(f"Unsigned metadata or nonzero verification flags in {name}")
            if expected_key is not None and (key != expected_key or header.rollback_index_location != 0):
                raise ValueError(f"Chained key or rollback header mismatch in {name}")
            location = header.rollback_index_location if chain_location is None else chain_location
            if location in locations:
                raise ValueError("Duplicate rollback index location")
            locations.add(location)
            for descriptor in descriptors:
                if isinstance(descriptor, avb.AvbHashDescriptor) and descriptor.partition_name in BOOT_PARTITIONS:
                    part = descriptor.partition_name
                    if part in covered:
                        raise ValueError(f"Duplicate signed descriptor for {part}")
                    verify_hash(descriptor, opened[part], parsed[part][0]["original_image_size"])
                    covered.add(part)
                    report["images"][part]["signed_payload_hash_verified"] = True
                elif isinstance(descriptor, avb.AvbChainPartitionDescriptor):
                    if descriptor.flags != 0 or descriptor.rollback_index_location == 0:
                        raise ValueError("Unsupported chain flags or rollback index location")
                    verify_metadata(descriptor.partition_name, descriptor.public_key, descriptor.rollback_index_location)
                    report["chains"].append({"parent": name, "partition": descriptor.partition_name,
                                             "rollback_index_location": descriptor.rollback_index_location,
                                             "embedded_key_matches_parent": True})
                elif isinstance(descriptor, (avb.AvbHashDescriptor, avb.AvbHashtreeDescriptor)):
                    report["unchecked_partitions"].append({"metadata": name, "partition": descriptor.partition_name,
                                                            "reason": "outside boot-set verification scope"})
                elif not isinstance(descriptor, avb.AvbPropertyDescriptor):
                    raise ValueError("Unsupported descriptor in signed metadata")

        verify_metadata("vbmeta")
        if covered != set(BOOT_PARTITIONS):
            raise ValueError("Signed metadata does not cover every boot partition")
        report["boot_integrity_verified"] = True
        report["complete_partition_verification"] = not report["unchecked_partitions"]
        return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("--expected-build", required=True)
    parser.add_argument("--avbtool", type=Path, required=True)
    args = parser.parse_args()
    try:
        avb = load_avb(args.avbtool)
        report = verify_set(args.directory, args.expected_build, avb)
    except (OSError, ValueError, LookupError, struct.error) as error:
        parser.exit(1, f"{error}\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
