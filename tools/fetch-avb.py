#!/usr/bin/env python3
"""Fetch the pinned AOSP AVB tool into a new local directory."""
import argparse
import base64
import hashlib
import json
from pathlib import Path
import urllib.request


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    args = parser.parse_args()
    profile = json.loads((Path(__file__).resolve().parents[1] / "profiles/avb.json").read_text())
    try:
        args.directory.mkdir(exist_ok=False, parents=True)
        with urllib.request.urlopen(profile["url"], timeout=30) as response:
            data = base64.b64decode(response.read(1024 * 1024), validate=True)
        if len(data) != profile["bytes"] or hashlib.sha256(data).hexdigest() != profile["sha256"]:
            raise ValueError("Pinned AOSP AVB tool checksum or size mismatch")
        with (args.directory / "avbtool.py").open("xb") as output:
            output.write(data)
        with (args.directory / "source.json").open("x") as output:
            json.dump(profile, output, indent=2)
            output.write("\n")
        print(args.directory / "avbtool.py")
    except (OSError, ValueError) as error:
        parser.exit(1, f"{error}\n")


if __name__ == "__main__":
    main()
