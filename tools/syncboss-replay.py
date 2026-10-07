#!/usr/bin/env python3
"""Decode offline Syncboss FIFO records; sensor payloads remain opaque."""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import stat
import struct

MAX_CAPTURE_BYTES = 64 * 1024 * 1024
HEADER_LENGTHS = {2: 12, 3: 21}
OFFSET_STATES = ("invalid", "valid", "error")


def offset_info(state, value):
    if state >= len(OFFSET_STATES):
        raise ValueError("Unknown timestamp-offset state")
    return {"status": OFFSET_STATES[state], "offset_us": value if state == 1 else None}


def decode_record(data):
    if len(data) < 2:
        return None
    version, header_length = data[:2]
    if version not in HEADER_LENGTHS or header_length != HEADER_LENGTHS[version]:
        raise ValueError("Unsupported Syncboss header version/length")
    if len(data) < header_length:
        return None
    from_driver, nsync_state, nsync_offset = struct.unpack_from("<BBq", data, 2)
    if from_driver not in (0, 1):
        raise ValueError("Invalid from_driver flag")
    record = {"header_version": version, "from_driver": bool(from_driver),
              "nsync": offset_info(nsync_state, nsync_offset)}
    if version == 3:
        remote_state, remote_offset = struct.unpack_from("<Bq", data, 12)
        record["remote"] = offset_info(remote_state, remote_offset)
    if from_driver:
        length = header_length + 8
        if len(data) < length:
            return None
        message_type, message_data = struct.unpack_from("<II", data, header_length)
        record.update(driver_message_type=message_type, driver_message_data=message_data)
    else:
        if len(data) < header_length + 3:
            return None
        packet_type, sequence, payload_length = struct.unpack_from("<BBB", data, header_length)
        if payload_length > 252:
            raise ValueError("Syncboss packet exceeds the vendor FIFO payload limit")
        length = header_length + 3 + payload_length
        if len(data) < length:
            return None
        record.update(packet_type=packet_type, sequence=sequence,
                      payload_hex=bytes(data[header_length + 3:length]).hex())
    return record, length


def decode_stream(stream, digest):
    pending, total, position = bytearray(), 0, 0
    while chunk := stream.read(4096):
        total += len(chunk)
        if total > MAX_CAPTURE_BYTES:
            raise ValueError("Capture exceeds 64 MiB")
        digest.update(chunk)
        pending.extend(chunk)
        while pending:
            decoded = decode_record(pending)
            if decoded is None:
                break
            record, length = decoded
            record["byte_offset"] = position
            yield record
            del pending[:length]
            position += length
    if pending:
        raise ValueError(f"Truncated Syncboss record at byte {position}")


def replay(path, records_path=None):
    if path.is_symlink() or not stat.S_ISREG(path.stat().st_mode):
        raise ValueError("Use a regular capture file, never a live device or pipe")
    if path.stat().st_size > MAX_CAPTURE_BYTES:
        raise ValueError("Capture exceeds 64 MiB")
    digest = hashlib.sha256()
    summary = {"format": "syncboss-fifo", "records": 0, "driver_messages": 0,
               "valid_nsync_offsets": 0, "packet_types": Counter(), "tracking_verified": False}
    output = records_path.open("x") if records_path else None
    try:
        with path.open("rb") as stream:
            for record in decode_stream(stream, digest):
                summary["records"] += 1
                summary["driver_messages"] += record["from_driver"]
                summary["valid_nsync_offsets"] += record["nsync"]["status"] == "valid"
                if "packet_type" in record:
                    summary["packet_types"][str(record["packet_type"])] += 1
                if output:
                    output.write(json.dumps(record) + "\n")
    finally:
        if output:
            output.close()
    summary["input_sha256"] = digest.hexdigest()
    return summary


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", type=Path)
    parser.add_argument("--records", type=Path, help="New JSONL file for decoded records")
    args = parser.parse_args()
    try:
        print(json.dumps(replay(args.capture, args.records), indent=2))
    except (OSError, ValueError, struct.error) as error:
        parser.exit(1, f"{error}\n")


if __name__ == "__main__":
    main()
