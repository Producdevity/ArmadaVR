#!/usr/bin/env python3
"""Verify pinned headset kernel sources and compile vendor device trees offline."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import subprocess
import tarfile
import tempfile
import time
import urllib.request

from devicetree import inventory

ROOT = Path(__file__).resolve().parents[1]
MAX_SOURCE_BYTES = 4 * 1024 * 1024


def full_archive(profile, archive):
    pin = profile.get("full_source")
    if not pin:
        raise ValueError("No complete source archive is pinned for this profile")
    if not archive.exists():
        archive.parent.mkdir(parents=True, exist_ok=True)
        started, total = time.monotonic(), 0
        with tempfile.NamedTemporaryFile(dir=archive.parent) as temporary:
            with urllib.request.urlopen(pin["url"], timeout=45) as response:
                while data := response.read(1024 * 1024):
                    total += len(data)
                    if total > pin["size"] or time.monotonic() - started > 600:
                        raise ValueError("Source download exceeded its size/time limit")
                    temporary.write(data)
            temporary.flush()
            temporary.seek(0)
            if total != pin["size"] or hashlib.file_digest(temporary, "sha256").hexdigest() != pin["sha256"]:
                raise ValueError("Source archive checksum/size mismatch")
            with archive.open("xb") as destination:
                temporary.seek(0)
                shutil.copyfileobj(temporary, destination, 1024 * 1024)
    if not archive.is_file() or archive.stat().st_size != pin["size"]:
        raise ValueError("Cached source archive size mismatch")
    with archive.open("rb") as stream:
        if hashlib.file_digest(stream, "sha256").hexdigest() != pin["sha256"]:
            raise ValueError("Cached source archive checksum mismatch")


def require_case_sensitive(directory):
    with tempfile.TemporaryDirectory(prefix="case-check-", dir=directory) as temp:
        probe = Path(temp) / "case-A"
        probe.touch()
        if probe.with_name("case-a").exists():
            raise ValueError("Full kernel sources require a case-sensitive filesystem; use the Linux build volume")


def extract_entry(stream, member, path, source):
    if member.isdir():
        path.mkdir(parents=True, exist_ok=True)
    elif member.isfile():
        path.parent.mkdir(parents=True, exist_ok=True)
        with stream.extractfile(member) as original, path.open("xb") as destination:
            shutil.copyfileobj(original, destination, 1024 * 1024)
        path.chmod(0o755 if member.mode & 0o111 else 0o644)
    elif member.issym():
        if PurePosixPath(member.linkname).is_absolute() or not (path.parent / member.linkname).resolve().is_relative_to(source.resolve()):
            raise ValueError(f"Source symlink escapes its directory: {member.name}")
        path.parent.mkdir(parents=True, exist_ok=True)
        path.symlink_to(member.linkname)
    else:
        raise ValueError(f"Unsupported source archive entry: {member.name}")


def full_source(profile, archive, source, verify_only=False):
    full_archive(profile, archive)
    if not verify_only:
        source.parent.mkdir(parents=True, exist_ok=True)
        require_case_sensitive(source.parent)
        source.mkdir(parents=True, exist_ok=False)
    paths, total = set(), 0
    with tarfile.open(archive, "r|gz") as stream:
        for member in stream:
            prefix, _, relative = member.name.partition("/")
            if prefix != profile["full_source"]["archive_root"]:
                raise ValueError("Unexpected source archive root")
            if not relative:
                continue
            path = source_path(source, relative)
            total += member.size
            if total > 3 * 1024**3 or len(paths) > 120000:
                raise ValueError("Source archive exceeds extraction limits")
            if relative in paths:
                raise ValueError(f"Duplicate source archive path: {relative}")
            paths.add(relative)
            if not verify_only:
                extract_entry(stream, member, path, source)
            elif member.isfile():
                if path.is_symlink() or not path.is_file() or path.stat().st_size != member.size:
                    raise ValueError(f"Source differs from archive: {relative}")
                if bool(path.stat().st_mode & 0o111) != bool(member.mode & 0o111):
                    raise ValueError(f"Source executable permission differs: {relative}")
                with path.open("rb") as local, stream.extractfile(member) as original:
                    if hashlib.file_digest(local, "sha256").digest() != hashlib.file_digest(original, "sha256").digest():
                        raise ValueError(f"Source differs from archive: {relative}")
            elif member.issym():
                if not path.is_symlink() or os.readlink(path) != member.linkname:
                    raise ValueError(f"Source symlink differs: {relative}")
            elif member.isdir():
                if path.is_symlink() or not path.is_dir():
                    raise ValueError(f"Source directory differs: {relative}")
            elif not member.isdir():
                raise ValueError(f"Unsupported source archive entry: {relative}")
    if verify_only:
        for path in source.rglob("*"):
            if path.relative_to(source).as_posix() not in paths:
                raise ValueError(f"Unexpected full-source path: {path.relative_to(source)}")
    return {"entries": len(paths), "bytes": total, "archive_sha256": profile["full_source"]["sha256"]}


def source_path(root, relative):
    path = PurePosixPath(relative)
    if path.is_absolute() or not path.parts or any(p in (".", "..") for p in relative.split("/")):
        raise ValueError(f"Invalid source path: {relative}")
    result = root.joinpath(*path.parts)
    if not result.resolve().is_relative_to(root.resolve()):
        raise ValueError(f"Source path escapes its directory: {relative}")
    return result


def read_profile(name):
    if not re.fullmatch(r"[a-z0-9-]+", name):
        raise ValueError("Invalid source profile")
    profile = json.loads((ROOT / "profiles/kernel" / f"{name}.json").read_text())
    if profile["schema_version"] != 1 or not re.fullmatch(r"[0-9a-f]{40}", profile["commit"]):
        raise ValueError("Source profile must pin a full Git commit")
    for digest in profile["files"].values():
        if not re.fullmatch(r"[0-9a-f]{64}", digest):
            raise ValueError("Source profile must pin SHA-256 for every file")
    return profile


def verify(profile, source):
    expected_paths = set(profile["files"]) | set(profile.get("symlinks", {}))
    for path in source.rglob("*"):
        if (path.is_file() or path.is_symlink()) and path.relative_to(source).as_posix() not in expected_paths:
            raise ValueError(f"Unexpected file in source subset: {path.relative_to(source)}")
    for relative, expected in profile["files"].items():
        path = source_path(source, relative)
        if path.is_symlink() or not path.is_file() or path.stat().st_size > MAX_SOURCE_BYTES:
            raise ValueError(f"Missing or invalid source: {relative}")
        if hashlib.sha256(path.read_bytes()).hexdigest() != expected:
            raise ValueError(f"Source checksum mismatch: {relative}")
    for relative, target in profile.get("symlinks", {}).items():
        path = source_path(source, relative)
        if not path.is_symlink() or str(path.readlink()) != target or not path.exists():
            raise ValueError(f"Source symlink mismatch: {relative}")


def fetch(profile, source):
    source.mkdir(parents=True, exist_ok=True)

    def download(item):
        relative, expected = item
        path = source_path(source, relative)
        if path.exists() or path.is_symlink():
            if path.is_symlink() or path.stat().st_size > MAX_SOURCE_BYTES or hashlib.sha256(path.read_bytes()).hexdigest() != expected:
                raise ValueError(f"Refusing to replace modified source: {relative}")
            return
        url = f"https://raw.githubusercontent.com/{profile['repository']}/{profile['commit']}/{relative}"
        with urllib.request.urlopen(url, timeout=45) as response:
            data = response.read(MAX_SOURCE_BYTES + 1)
        if len(data) > MAX_SOURCE_BYTES or hashlib.sha256(data).hexdigest() != expected:
            raise ValueError(f"Download checksum/size mismatch: {relative}")
        path.parent.mkdir(parents=True, exist_ok=True)
        with path.open("xb") as stream:
            stream.write(data)

    with ThreadPoolExecutor(max_workers=4) as pool:
        list(pool.map(download, profile["files"].items()))
    for relative, target in profile.get("symlinks", {}).items():
        path = source_path(source, relative)
        if PurePosixPath(target).is_absolute() or not (path.parent / target).resolve().is_relative_to(source.resolve()):
            raise ValueError(f"Source symlink escapes its directory: {relative}")
        if not path.is_symlink() and not path.exists():
            path.parent.mkdir(parents=True, exist_ok=True)
            path.symlink_to(target)
    verify(profile, source)


def build(profile, source, output, cc="cc"):
    if not profile.get("base"):
        raise ValueError(profile["device_tree_blocker"])
    verify(profile, source)
    if output.exists():
        raise ValueError("Build output exists; choose a new directory")
    for tool in (cc, "dtc", "fdtoverlay"):
        if not shutil.which(tool):
            raise ValueError(f"Install {tool} before building device trees")
    output.mkdir(parents=True)
    output = output.resolve()
    source = source.resolve()
    report = {"schema_version": 1, "repository": profile["repository"], "commit": profile["commit"],
              "kernel_version": profile["kernel_version"], "purpose": "offline vendor device-tree baseline",
              "hardware_boot_verified": False, "flash_image": False, "artifacts": [], "commands": []}
    report["source_manifest_sha256"] = hashlib.sha256(json.dumps(profile, sort_keys=True).encode()).hexdigest()
    report["tools"] = {tool: subprocess.check_output([tool, "--version" if tool != "fdtoverlay" else "-V"], text=True).strip()
                       for tool in (cc, "dtc", "fdtoverlay")}

    def run(command, log):
        report["commands"].append(command)
        with log.open("ab") as stream:
            subprocess.run(command, cwd=source, stdout=stream, stderr=stream, check=True, timeout=60)

    def record(path, kind):
        data = path.read_bytes()
        info = {"path": path.name, "kind": kind, "sha256": hashlib.sha256(data).hexdigest(), "bytes": len(data)}
        details = inventory(data)
        inventory_path = path.with_suffix(".json")
        inventory_path.write_text(json.dumps(details, indent=2) + "\n")
        info.update({k: v for k, v in details.items() if k != "nodes"})
        info["inventory"] = inventory_path.name
        report["artifacts"].append(info)

    with tempfile.TemporaryDirectory(prefix="armada-vr-dts-") as temp:
        for relative in [profile["base"], *profile["overlays"]]:
            stem = Path(relative).stem
            overlay = relative != profile["base"]
            compiled = output / (stem + (".dtbo" if overlay else ".dtb"))
            preprocessed = Path(temp) / (stem + ".dts")
            log = output / (stem + ".log")
            run([cc, "-E", "-nostdinc", "-undef", "-D__DTS__", "-x", "assembler-with-cpp",
                 *[arg for directory in profile["include_dirs"] for arg in ("-I", directory)],
                 relative, "-o", str(preprocessed)], log)
            run(["dtc", "-@", "-I", "dts", "-O", "dtb", "-b", "0", "-o", str(compiled), str(preprocessed)], log)
            record(compiled, "overlay" if overlay else "base")
            if overlay:
                merged = output / (stem.removesuffix("-overlay") + ".dtb")
                run(["fdtoverlay", "-i", str(output / (Path(profile["base"]).stem + ".dtb")),
                     "-o", str(merged), str(compiled)], log)
                record(merged, "merged-board")
    report["compiler_warnings"] = sum(log.read_text().count("Warning (") for log in output.glob("*.log"))
    (output / "build.json").write_text(json.dumps(report, indent=2) + "\n")
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("fetch", "verify", "build-dt", "fetch-full", "verify-full", "fetch-archive"))
    parser.add_argument("profile", choices=("quest3", "pico-neo3"))
    parser.add_argument("--source", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--cc", default="cc")
    parser.add_argument("--archive", type=Path)
    args = parser.parse_args()
    try:
        profile = read_profile(args.profile)
        source = args.source or ROOT / "output/kernel" / args.profile / ("full-source" if args.command.endswith("-full") else "source")
        if args.command == "fetch-archive":
            archive = args.archive or ROOT / "output/downloads" / f"{args.profile}-kernel.tar.gz"
            full_archive(profile, archive)
            print(f"Verified source archive: {archive}")
        elif args.command.endswith("-full"):
            archive = args.archive or ROOT / "output/downloads" / f"{args.profile}-kernel.tar.gz"
            result = full_source(profile, archive, source, args.command == "verify-full")
            print(f"Verified full source: {result['entries']} entries, {result['bytes']} bytes in {source}")
        elif args.command == "fetch":
            fetch(profile, source)
            print(f"Verified {len(profile['files'])} pinned source files in {source}")
        elif args.command == "verify":
            verify(profile, source)
            print(f"Verified {len(profile['files'])} pinned source files in {source}")
        else:
            output = args.output or ROOT / "output/kernel" / args.profile / "dt"
            report = build(profile, source, output, args.cc)
            print(f"Built {len(report['artifacts'])} vendor DT artifacts in {output}; no boot/flash validation")
    except (OSError, ValueError, subprocess.SubprocessError, tarfile.TarError) as error:
        parser.exit(1, f"{error}\n")


if __name__ == "__main__":
    main()
