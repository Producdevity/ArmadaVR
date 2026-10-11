#!/usr/bin/env python3
"""Prepare a separate Lepton execution archive from an acquired payload."""
import argparse
import copy
import gzip
import hashlib
import io
import json
from pathlib import Path, PurePosixPath
import re
import subprocess
import tarfile
import tempfile

ROOT = Path(__file__).resolve().parents[1]
RELEASE = {"app": 3029110, "build": 25257944, "launcher": "v2.8.14", "image": "v2.8.11", "sdk": 30,
           "depots": {"3029111": "5805845383697258311", "3029112": "773148386728154884"}}
ORIGINALS = {
    "lepton": "f892362538effbefbbd70355cf1f659582ad78fec214f0e3eaf8adcc802a0f45",
    "liblepton/properties.sh": "ec6af956c92e93092bee71998c2fcbda9c0e25385e8c95f30cb631927ef6fa9d",
    "liblepton/mounting.sh": "4e53b2762198ef1350d80dcdfcabb3dcdf084db6cc2887ed94555a59321ab4f3",
    "liblepton/liblepton.sh": "26c7c56106ed43c28ce0cc71ae62a859d6c55770506e8044326becac6b1ca073",
    "images/rootfs_overlay/system/bin/cmd": "66337f13fc6fd1d68d07ae5ea174dd7120d8800113a4e52ebdc33b639d6b443e",
    "liblepton/baking.sh": "e5d11e2c3027f1a067225fd6141bbd95fe527734290cac38c722ed42d056c4b5",
}
ORIGINAL_INIT = "images/rootfs/system/bin/init"
ORIGINAL_INIT_SHA = "385ffeb8c732c7fa7f52e014c36d0c1d70b0ca9e53a69b35a008e37d9ab189aa"
OVERLAY_INIT = "images/rootfs_overlay/system/bin/init"
MAX_BYTES = 2 * 1024**3


def digest(path):
    path = Path(path)
    if path.is_symlink() or not path.is_file():
        raise ValueError("Expected a regular file: " + str(path))
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def name_of(member):
    name = member.name.removeprefix("./")
    if name == "." and member.isdir():
        return "."
    if (not name or PurePosixPath(name).is_absolute() or
            any(part in ("", ".", "..") for part in name.split("/"))):
        raise ValueError("Invalid archive path: " + member.name)
    return name


def inventory(path):
    records, payloads, total = {}, {}, 0
    with tarfile.open(path, "r:*") as archive:
        for member in archive:
            name = name_of(member)
            if name in records or len(records) >= 10000:
                raise ValueError("Duplicate path or excessive archive inventory: " + name)
            if not (member.isdir() or member.isfile() or member.issym()):
                raise ValueError("Unsupported archive member: " + name)
            if member.size < 0 or (not member.isfile() and member.size):
                raise ValueError("Invalid archive member size: " + name)
            total += member.size
            if total > MAX_BYTES:
                raise ValueError("Lepton payload exceeds the 2 GiB expanded limit")
            record = {"mode": member.mode, "uid": member.uid, "gid": member.gid,
                      "pax": member.pax_headers, "mtime": member.mtime}
            if member.isfile():
                with archive.extractfile(member) as stream:
                    sha = hashlib.file_digest(stream, "sha256").hexdigest()
                record.update(type="file", bytes=member.size, sha256=sha)
                payloads[name] = {"path": name, "bytes": member.size, "sha256": sha, "mode": member.mode}
            elif member.issym():
                target = member.linkname
                if not target:
                    raise ValueError("Empty link: " + name)
                # Android's absolute links resolve inside the future container.
                if (PurePosixPath(target).is_absolute() or ".." in PurePosixPath(target).parts) and not name.startswith("images/rootfs/"):
                    raise ValueError("Link escapes the Lepton installation: " + name)
                record.update(type="link", link=target)
                payloads[name] = {"path": name, "link": target, "mode": member.mode}
            else:
                record.update(type="directory")
            records[name] = record
    for name in records:
        for parent in PurePosixPath(name).parents:
            if str(parent) == ".":
                continue
            if records.get(str(parent), {}).get("type") != "directory":
                raise ValueError("Archive parent is absent or is not a directory: " + name)
    return {"entries": records, "payloads": payloads, "expanded_bytes": total}


def patch_identity():
    return {str(p.relative_to(ROOT)): digest(p) for p in sorted((ROOT / "patches/lepton").glob("*.patch"))}


def validate(directory):
    directory = Path(directory)
    manifest = directory / "manifest.json"
    manifest_sha = digest(manifest)
    record = json.loads(manifest.read_text())
    if (record.get("status") != "prepared" or record.get("release") != RELEASE or
            record.get("patches") != patch_identity() or record.get("preparer_sha256") != digest(Path(__file__))):
        raise ValueError("Lepton bundle does not match the current release and preparation sources")
    if digest(directory / "lepton.tar.gz") != record["archive_sha256"]:
        raise ValueError("Lepton archive checksum mismatch")
    if inventory(directory / "lepton.tar.gz") != record["inventory"]:
        raise ValueError("Lepton archive inventory mismatch")
    if digest(manifest) != manifest_sha:
        raise ValueError("Lepton manifest changed during validation")
    return {"manifest_sha256": manifest_sha, **record}


def prepare(args):
    preparer_sha = digest(Path(__file__))
    source, acquisition, init_dir, output = (p.absolute() for p in (args.archive, args.manifest, args.init_build, args.output))
    if output.exists() or output.is_symlink():
        raise ValueError("Choose a new output directory")
    subprocess.run([args.bash, "-c", '(( BASH_VERSINFO[0] >= 4 )) && [[ -v BASH_VERSION ]]'],
                   check=True, timeout=10)
    if not re.fullmatch("[0-9a-f]{64}", args.archive_sha256) or digest(source) != args.archive_sha256:
        raise ValueError("Acquired Lepton archive checksum mismatch")
    acquisition_sha = digest(acquisition)
    acquired = json.loads(acquisition.read_text())
    if any(acquired.get(k) != v for k, v in RELEASE.items()):
        raise ValueError("Unsupported acquired Lepton release")
    original = inventory(source)
    files = acquired["files"]
    if len({i["path"] for i in files}) != len(files) or original["payloads"] != {i["path"]: i for i in files}:
        raise ValueError("Acquired payload inventory differs from its manifest")
    for name, sha in {**ORIGINALS, ORIGINAL_INIT: ORIGINAL_INIT_SHA}.items():
        if original["payloads"].get(name, {}).get("sha256") != sha:
            raise ValueError("Unsupported original Lepton file: " + name)
    if OVERLAY_INIT in original["entries"]:
        raise ValueError("Acquired payload already overrides Android init")
    init_manifest = init_dir / "manifest.json"
    init_manifest_sha = digest(init_manifest)
    init_record = json.loads(init_manifest.read_text())
    init = init_dir / "init-candidate"
    init_sha = digest(init)
    if {"bytes": init.stat().st_size, "sha256": init_sha} != init_record["init-candidate"]:
        raise ValueError("Source-built init differs from its build record")
    with init.open("rb") as stream:
        header = stream.read(20)
    if header[:6] != b"\x7fELF\x02\x01" or header[16:20] != b"\x03\x00\xb7\x00":
        raise ValueError("Android init must be a native ARM64 position-independent ELF")
    patches = patch_identity()
    if len(patches) != 11:
        raise ValueError("Expected the eleven reviewed Lepton patches")
    replacements = {}
    with tempfile.TemporaryDirectory(prefix="armada-lepton-") as temporary:
        work = Path(temporary)
        with tarfile.open(source) as archive:
            for member in archive:
                name = name_of(member)
                if name in ORIGINALS:
                    path = work / name
                    path.parent.mkdir(parents=True, exist_ok=True)
                    path.write_bytes(archive.extractfile(member).read())
        for name in patches:
            command = ["patch", "--batch", "--forward", "--fuzz=0", "-p1", "-i", str(ROOT / name)]
            subprocess.run([*command, "--dry-run"], cwd=work, check=True, timeout=30)
            subprocess.run(command, cwd=work, check=True, timeout=30)
        for name in ORIGINALS:
            subprocess.run([args.bash, "-n", str(work / name)], check=True, timeout=10)
            replacements[name] = (work / name).read_bytes()
    replacements[OVERLAY_INIT] = init.read_bytes()
    output.mkdir(parents=True)
    part = output / "lepton.tar.gz.part"
    expected = copy.deepcopy(original)
    with tarfile.open(source) as archive, part.open("xb") as raw, gzip.GzipFile(filename="", mode="wb", fileobj=raw, mtime=0) as compressed, tarfile.open(fileobj=compressed, mode="w|") as destination:
        for member in archive:
            name = name_of(member)
            member = copy.deepcopy(member)
            if name in replacements:
                data = replacements[name]
                expected["expanded_bytes"] += len(data) - member.size
                member.size = len(data)
                expected["entries"][name].update(bytes=len(data), sha256=hashlib.sha256(data).hexdigest())
                expected["payloads"][name].update(bytes=len(data), sha256=hashlib.sha256(data).hexdigest())
                destination.addfile(member, io.BytesIO(data))
            else:
                destination.addfile(member, archive.extractfile(member) if member.isfile() else None)
        member = tarfile.TarInfo(OVERLAY_INIT)
        member.mode, member.size = 0o755, len(replacements[OVERLAY_INIT])
        destination.addfile(member, io.BytesIO(replacements[OVERLAY_INIT]))
        expected["entries"][OVERLAY_INIT] = {"mode": 0o755, "uid": 0, "gid": 0, "pax": {}, "mtime": 0,
                                            "type": "file", "bytes": member.size, "sha256": init_sha}
        expected["payloads"][OVERLAY_INIT] = {"path": OVERLAY_INIT, "mode": 0o755, "bytes": member.size, "sha256": init_sha}
        expected["expanded_bytes"] += member.size
    if inventory(part) != expected:
        raise ValueError("Prepared archive did not preserve its expected contents and metadata")
    if (digest(source) != args.archive_sha256 or digest(acquisition) != acquisition_sha or
            digest(init) != init_sha or digest(init_manifest) != init_manifest_sha or patches != patch_identity() or
            digest(Path(__file__)) != preparer_sha):
        raise ValueError("An input changed during Lepton preparation")
    part.rename(output / "lepton.tar.gz")
    record = {"status": "prepared", "release": RELEASE, "preparer_sha256": preparer_sha,
              "patches": patches, "acquired_archive_sha256": args.archive_sha256,
              "acquisition_manifest_sha256": acquisition_sha, "init_build_manifest_sha256": init_manifest_sha,
              "init_sha256": init_sha, "archive_sha256": digest(output / "lepton.tar.gz"),
              "inventory": expected, "originals_unchanged": True, "runtime_tested": False,
              "physical_boot_verified": False}
    (output / "manifest.json").write_text(json.dumps(record, indent=2) + "\n")
    print(output / "manifest.json")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("archive", type=Path)
    parser.add_argument("manifest", type=Path, help="Original acquisition file/hash/mode inventory")
    parser.add_argument("--archive-sha256", required=True)
    parser.add_argument("--init-build", type=Path, required=True, help="Compatible source-built init and its manifest.json")
    parser.add_argument("--bash", default="bash", help="Bash 4.2 or later for launcher syntax checks")
    parser.add_argument("--output", type=Path, required=True)
    try:
        prepare(parser.parse_args())
    except (OSError, ValueError, KeyError, tarfile.TarError, subprocess.SubprocessError) as error:
        parser.exit(1, str(error) + "\n")
