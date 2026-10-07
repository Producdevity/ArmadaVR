#!/usr/bin/env python3
"""Download and verify the pinned Steam Runtime, Proton and FEX rootfs archives."""
import argparse
import hashlib
import json
import tempfile
import urllib.request
from pathlib import Path


def fetch(component, directory):
    name = component["file"]
    if Path(name).name != name or component["url"].split(":", 1)[0] != "https":
        raise ValueError("Expected an HTTPS archive URL and a plain filename")
    target = directory / name
    if target.is_symlink():
        raise ValueError(f"Refusing a symlink: {target}")
    if not target.exists():
        with tempfile.NamedTemporaryFile(dir=directory, prefix=name + ".", suffix=".partial") as output:
            partial = Path(output.name)
            with urllib.request.urlopen(component["url"], timeout=30) as source:
                size = 0
                while chunk := source.read(1024**2):
                    size += len(chunk)
                    if size > component["size"]:
                        raise ValueError(f"Download exceeds expected size: {name}")
                    output.write(chunk)
            output.flush()
            with partial.open("rb") as source:
                digest = hashlib.file_digest(source, "sha256").hexdigest()
            if size != component["size"] or digest != component["sha256"]:
                raise ValueError(f"Archive checksum/size mismatch: {name}")
            target.hardlink_to(partial)
    with target.open("rb") as source:
        digest = hashlib.file_digest(source, "sha256").hexdigest()
    if target.stat().st_size != component["size"] or digest != component["sha256"]:
        raise ValueError(f"Archive checksum/size mismatch: {name}")
    print(f"Verified {component['name']}: {target}", flush=True)


def main():
    profile = Path(__file__).resolve().parents[1] / "profiles/vr-runtime.json"
    components = json.loads(profile.read_text())["components"]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", nargs="?", type=Path, default=Path("output/downloads/runtime"))
    parser.add_argument("--component", action="append", choices=[item["file"] for item in components],
                        help="Fetch only these named archives; repeat to select several")
    args = parser.parse_args()
    try:
        args.directory.mkdir(parents=True, exist_ok=True)
        for component in components:
            if args.component and component["file"] not in args.component:
                continue
            print(f"Fetching {component['name']}", flush=True)
            fetch(component, args.directory)
    except (OSError, ValueError) as error:
        parser.exit(1, f"{error}\n")


if __name__ == "__main__":
    main()
