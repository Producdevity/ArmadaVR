#!/usr/bin/env python3
"""Fetch or verify the pinned Qualcomm service-mapper source archives."""
import argparse
import hashlib
import json
from pathlib import Path
import urllib.request


ROOT = Path(__file__).resolve().parents[1]


def verify(directory):
    profile = json.loads((ROOT / "profiles/qcom-services.json").read_text())
    for item in profile["sources"]:
        path = directory / (item["name"] + ".tar.gz")
        if path.is_symlink() or not path.is_file() or path.stat().st_size != item["bytes"]:
            raise ValueError(f"Missing, non-regular or wrong-sized source archive: {path}")
        if hashlib.sha256(path.read_bytes()).hexdigest() != item["sha256"]:
            raise ValueError(f"Source checksum mismatch: {path}")
    return profile


def fetch(directory):
    if directory.is_symlink():
        raise ValueError("Source directory must not be a symlink")
    profile = json.loads((ROOT / "profiles/qcom-services.json").read_text())
    directory.mkdir(parents=True, exist_ok=True)
    for item in profile["sources"]:
        path = directory / (item["name"] + ".tar.gz")
        if path.exists() or path.is_symlink():
            continue
        with urllib.request.urlopen(item["url"], timeout=30) as response:
            data = response.read(item["bytes"] + 1)
        if len(data) != item["bytes"] or hashlib.sha256(data).hexdigest() != item["sha256"]:
            raise ValueError(f"Downloaded source checksum or size mismatch: {item['name']}")
        with path.open("xb") as stream:
            stream.write(data)
    verify(directory)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("--verify-only", action="store_true")
    args = parser.parse_args()
    try:
        (verify if args.verify_only else fetch)(args.directory)
        print(f"Verified Qualcomm service sources: {args.directory}")
    except (OSError, ValueError) as error:
        parser.exit(1, f"{error}\n")
