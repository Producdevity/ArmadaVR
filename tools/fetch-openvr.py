#!/usr/bin/env python3
"""Fetch the checksum-pinned OpenVR header and license for the SteamVR probe."""
import argparse
import hashlib
import json
from pathlib import Path
import urllib.request

ROOT = Path(__file__).resolve().parents[1]


def fetch(directory):
    profile = json.loads((ROOT / "profiles/openvr.json").read_text())
    directory.mkdir(parents=True, exist_ok=True)
    for source, expected in profile["files"].items():
        destination = directory / Path(source).name
        if destination.is_symlink():
            raise ValueError(f"Expected a regular SDK file: {destination}")
        if destination.exists():
            if destination.stat().st_size > 1024**2:
                raise ValueError(f"Oversized SDK file: {destination}")
            data = destination.read_bytes()
        else:
            url = f"https://raw.githubusercontent.com/ValveSoftware/openvr/{profile['commit']}/{source}"
            with urllib.request.urlopen(url, timeout=30) as response:
                data = response.read(1024**2 + 1)
            if len(data) > 1024**2:
                raise ValueError(f"Oversized SDK download: {source}")
        if hashlib.sha256(data).hexdigest() != expected:
            raise ValueError(f"SDK checksum mismatch: {destination}")
        if not destination.exists():
            with destination.open("xb") as stream:
                stream.write(data)
    print(f"Verified OpenVR SDK {profile['commit']}: {directory}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", nargs="?", type=Path, default=ROOT / "output/openvr-sdk")
    args = parser.parse_args()
    try:
        fetch(args.directory)
    except (OSError, ValueError) as error:
        parser.exit(1, f"{error}\n")
