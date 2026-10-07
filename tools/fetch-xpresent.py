#!/usr/bin/env python3
"""Prepare the pinned XPresent source for the virtual SteamVR timing fix."""
import argparse
import hashlib
import io
import json
from pathlib import Path
import subprocess
import tarfile
import urllib.request

ROOT = Path(__file__).resolve().parents[1]


def fetch(directory):
    if directory.exists():
        raise ValueError("Output exists; choose a new XPresent source directory")
    profile = json.loads((ROOT / "profiles/steamvr-presentation.json").read_text())
    with urllib.request.urlopen(profile["xpresent_url"], timeout=30) as response:
        data = response.read(1024 * 1024 + 1)
    if len(data) > 1024 * 1024 or hashlib.sha256(data).hexdigest() != profile["xpresent_sha256"]:
        raise ValueError("XPresent archive checksum mismatch or download exceeds 1 MiB")
    directory.mkdir(parents=True)
    with tarfile.open(fileobj=io.BytesIO(data), mode="r:xz") as archive:
        for name in ("src/Xpresent.c", "include/X11/extensions/Xpresent.h", "COPYING"):
            member = archive.getmember("libXpresent-1.0.2/" + name)
            if not member.isfile() or member.size > 1024 * 1024:
                raise ValueError(f"Invalid XPresent source member: {name}")
            path = directory / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(archive.extractfile(member).read())
    patch = ROOT / "patches/xpresent/0001-steamvr-initial-timing.patch"
    subprocess.run(["patch", "--batch", "--fuzz=0", "-p1", "-i", str(patch)],
                   cwd=directory, check=True, timeout=10)
    print(f"Verified and patched XPresent: {directory}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    args = parser.parse_args()
    try:
        fetch(args.directory)
    except (OSError, ValueError, KeyError, tarfile.TarError, subprocess.SubprocessError) as error:
        parser.exit(1, f"{error}\n")
