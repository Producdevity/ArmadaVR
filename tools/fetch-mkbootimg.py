#!/usr/bin/env python3
"""Fetch the pinned AOSP boot-image tools into a new local directory."""
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
    profile = json.loads((Path(__file__).resolve().parents[1] / "profiles/mkbootimg.json").read_text())
    try:
        args.directory.mkdir(exist_ok=False, parents=True)
        for item in profile["files"]:
            with urllib.request.urlopen(item["url"], timeout=30) as response:
                data = base64.b64decode(response.read(1024**2), validate=True)
            if len(data) != item["bytes"] or hashlib.sha256(data).hexdigest() != item["sha256"]:
                raise ValueError(f"Pinned AOSP checksum or size mismatch: {item['path']}")
            path = args.directory / item["path"]
            path.parent.mkdir(parents=True, exist_ok=True)
            with path.open("xb") as stream:
                stream.write(data)
        with (args.directory / "source.json").open("x") as stream:
            json.dump(profile, stream, indent=2)
            stream.write("\n")
        print(args.directory)
    except (OSError, ValueError) as error:
        parser.exit(1, f"{error}\n")


if __name__ == "__main__":
    main()
