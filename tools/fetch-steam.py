#!/usr/bin/env python3
"""Fetch pinned public Steam ARM64 client files into a new local directory."""
import argparse
import hashlib
import json
import shutil
import stat
import tarfile
import tempfile
import urllib.request
import zipfile
from pathlib import Path, PurePosixPath


def member_path(name):
    name = name.replace("\\", "/")
    path = PurePosixPath(name)
    if path.is_absolute() or not path.parts or ".." in path.parts or ":" in name:
        raise ValueError(f"Invalid archive path: {name!r}")
    return path


def unpack(archive, root):
    root = root.resolve()
    with zipfile.ZipFile(archive) as source:
        if sum(item.file_size for item in source.infolist()) > 1024**3:
            raise ValueError("Archive exceeds the 1 GiB extraction limit")
        for item in source.infolist():
            relative = member_path(item.filename)
            target = root.joinpath(*relative.parts)
            target.resolve().relative_to(root)
            if target.is_symlink():
                raise ValueError(f"Refusing to overwrite a symlink: {relative}")
            if item.filename.replace("\\", "/").endswith("/"):
                target.mkdir(parents=True, exist_ok=True)
                continue
            target.parent.mkdir(parents=True, exist_ok=True)
            mode = item.external_attr >> 16
            if stat.S_ISLNK(mode):
                link = source.read(item).decode().replace("\\", "/")
                link_path = PurePosixPath(link)
                if link_path.is_absolute() or not link_path.parts or ":" in link:
                    raise ValueError(f"Invalid symlink target: {link!r}")
                (target.parent / link_path).resolve().relative_to(root)
                target.symlink_to(str(link_path))
            else:
                with source.open(item) as src, target.open("xb") as dst:
                    shutil.copyfileobj(src, dst, length=1024**2)
                with target.open("rb") as installed:
                    elf = installed.read(4) == b"\x7fELF"
                target.chmod(0o755 if elf or target.suffix == ".sh" or mode & 0o111 else 0o644)


def unpack_tar(archive, root):
    with tarfile.open(archive) as source:
        if sum(item.size for item in source.getmembers()) > 1024**3:
            raise ValueError("Runtime exceeds the 1 GiB extraction limit")
        source.extractall(root, filter="data")


def fetch(package, base, cache):
    filename = package["file"]
    if len(member_path(filename).parts) != 1:
        raise ValueError("Package filename must not contain directories")
    target = cache / filename
    if not target.exists():
        partial = target.with_name(target.name + ".partial")
        try:
            with urllib.request.urlopen(base + filename, timeout=30) as src, partial.open("wb") as dst:
                size = 0
                while chunk := src.read(1024**2):
                    size += len(chunk)
                    if size > package["size"]:
                        raise ValueError(f"Download exceeds manifest size: {filename}")
                    dst.write(chunk)
            partial.rename(target)
        finally:
            partial.unlink(missing_ok=True)
    with target.open("rb") as src:
        digest = hashlib.file_digest(src, "sha256").hexdigest()
    if target.stat().st_size != package["size"] or digest != package["sha256"]:
        raise ValueError(f"Package checksum/size mismatch: {target}")
    return target


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("--cache", type=Path, default=Path("output/downloads/steam"))
    args = parser.parse_args()
    if args.directory.exists():
        parser.error("Client directory already exists; choose a new directory")
    lock_path = Path(__file__).resolve().parents[1] / "profiles/steam-client.json"
    lock = json.loads(lock_path.read_text())
    args.directory.parent.mkdir(parents=True, exist_ok=True)
    args.cache.mkdir(parents=True, exist_ok=True)
    try:
        with tempfile.TemporaryDirectory(prefix=".steam-", dir=args.directory.parent) as work:
            root = Path(work) / "client"
            root.mkdir()
            for package in lock["packages"]:
                print(f"Fetching {package['name']}", flush=True)
                archive = fetch(package, lock["package_base_url"], args.cache)
                unpack(archive, root)
            scout = root / "ubuntu12_32/steam-runtime.tar.xz"
            unpack_tar(scout, scout.parent)
            runtime = lock["runtime"]
            print(f"Fetching {runtime['name']}", flush=True)
            archive = fetch(runtime, runtime["base_url"], args.cache)
            unpack_tar(archive, root)
            binary = root / "steamrtarm64/steam"
            with binary.open("rb") as src:
                header = src.read(20)
            if header[:6] != b"\x7fELF\x02\x01" or header[18:20] != b"\xb7\x00":
                raise ValueError("Expected an ELF64 little-endian AArch64 client")
            (root / "armada-client-manifest.json").write_text(lock_path.read_text())
            root.rename(args.directory)
    except (OSError, ValueError, zipfile.BadZipFile, tarfile.TarError) as error:
        parser.exit(1, f"{error}\n")
    print(f"ARM64 client files installed in {args.directory}; UI/game compatibility remains unverified")


if __name__ == "__main__":
    main()
