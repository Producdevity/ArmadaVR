#!/usr/bin/env python3
"""Inspect a flattened device tree's RAM and reservations without booting it."""
import argparse
import hashlib
import json
from pathlib import Path

from devicetree import boot_memory


def run(args):
    if args.dtb.is_symlink() or not args.dtb.is_file() or args.dtb.stat().st_size > 16 * 1024**2:
        raise ValueError("Expected a regular device tree of at most 16 MiB")
    data = args.dtb.read_bytes()
    report = {"dtb_sha256": hashlib.sha256(data).hexdigest(), "memory": boot_memory(data),
              "scope": "supplied FDT only; not live allocator or peripheral ownership",
              "boot_verified": False, "staging_arena_verified": False}
    with args.output.open("x") as stream:
        json.dump(report, stream, indent=2)
        stream.write("\n")
    print(args.output)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("dtb", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    try:
        run(parser.parse_args())
    except (OSError, ValueError) as error:
        parser.exit(1, f"{error}\n")
