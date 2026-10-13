#!/usr/bin/env python3
"""Verify a prepared Lepton bundle and its offline filesystem installation."""
import hashlib
import importlib.util
import json
import os
import re
from pathlib import Path
import stat
import shutil
import subprocess

PREFIX = "opt/reframed-vr/lepton"
BUILD_RECORD = "usr/share/reframed-vr/lepton-build.json"
ADB_LINK = "usr/lib/android-sdk/platform-tools/adb"
HELPERS = {"newuidmap": "cap_setuid=ep", "newgidmap": "cap_setgid=ep"}
HOST_FILES = {
    "usr/lib/libVkLayer_fossilize.so": 0o755,
    "usr/share/guestos/android/vendor/vulkan_layers/libVkLayer_fossilize.so": 0o755,
    "usr/share/vulkan/implicit_layer.d/VkLayer_fossilize.json": 0o644,
}
HOST_RECORD = "usr/share/reframed-vr/lepton-host.json"
STORAGE_CONFIG = "home/vr/.config/containers/storage.conf"
STORAGE_CONTENT = '[storage]\ndriver = "overlay"\n[storage.options.overlay]\nmount_program = "/usr/bin/fuse-overlayfs"\n'


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def validate(directory, host=None):
    spec = importlib.util.spec_from_file_location("lepton_prepare", Path(__file__).with_name("prepare-lepton.py"))
    prepare = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(prepare)
    record = prepare.validate(directory)
    for entry in record["inventory"]["entries"].values():
        if any(key not in ("mtime", "atime", "ctime", "path", "linkpath") for key in entry["pax"]):
            raise ValueError("Lepton root-image verification does not yet support extended archive attributes")
    if host is not None:
        record["host"] = validate_host(host)
    return record


def validate_host(directory):
    directory = Path(directory)
    manifest = root_path(directory, "manifest.json")
    sha = digest(manifest)
    record = json.loads(manifest.read_text())
    files = record.get("files", [])
    if (record.get("schema_version") != 1 or len(files) != len(HOST_FILES) or
            {item.get("path") for item in files} != set(HOST_FILES)):
        raise ValueError("Expected the complete native and Android Fossilize bundle")
    for item in files:
        path = root_path(directory, item["path"])
        info = path.stat()
        if (not stat.S_ISREG(info.st_mode) or info.st_size > 8 * 1024**2 or
                info.st_size != item["bytes"] or stat.S_IMODE(info.st_mode) != HOST_FILES[item["path"]] or
                item["mode"] != HOST_FILES[item["path"]] or
                not re.fullmatch(r"[0-9a-f]{64}", item["sha256"]) or digest(path) != item["sha256"]):
            raise ValueError("Fossilize file identity differs: " + item["path"])
        if path.suffix == ".so":
            with path.open("rb") as stream:
                header = stream.read(20)
            if header[:6] != b"\x7fELF\x02\x01" or header[16:20] != b"\x03\x00\xb7\x00":
                raise ValueError("Fossilize requires native ARM64 shared libraries")
        else:
            layer = json.loads(path.read_text())["layer"]
            if layer["name"] != "VK_LAYER_fossilize" or layer["library_path"] != "/usr/lib/libVkLayer_fossilize.so":
                raise ValueError("Unexpected Fossilize layer manifest")
    actual = {str(p.relative_to(directory)) for p in directory.rglob("*") if not p.is_dir() or p.is_symlink()}
    if actual != set(HOST_FILES) | {"manifest.json"} or digest(manifest) != sha:
        raise ValueError("Fossilize bundle inventory differs")
    return {"manifest_sha256": sha, **record}


def root_path(root, name):
    root = Path(root)
    if root.is_symlink() or not root.is_dir() or root.resolve() == Path("/"):
        raise ValueError("Expected a separate offline staging root")
    path = root / name
    for parent in (path, *path.parents):
        if parent == root:
            break
        if parent.is_symlink():
            raise ValueError("Lepton installation path contains a symlink")
    return path


def verify_install(root, record):
    prefix = root_path(root, PREFIX)
    actual = {".": prefix}
    for directory, dirs, files in os.walk(prefix, followlinks=False):
        for name in dirs + files:
            path = Path(directory) / name
            actual[str(path.relative_to(prefix))] = path
    expected = record["inventory"]["entries"]
    if set(actual) != set(expected):
        raise ValueError("Installed Lepton inventory differs")
    for name, entry in expected.items():
        path = actual[name]
        info = path.lstat()
        if stat.S_IMODE(info.st_mode) != entry["mode"]:
            raise ValueError("Installed Lepton mode differs: " + name)
        if entry["type"] == "file":
            if (not stat.S_ISREG(info.st_mode) or info.st_size != entry["bytes"] or
                    digest(path) != entry["sha256"] or int(info.st_mtime) != int(entry["mtime"])):
                raise ValueError("Installed Lepton file, timestamp or checksum differs: " + name)
        elif entry["type"] == "link":
            if not stat.S_ISLNK(info.st_mode) or os.readlink(path) != entry["link"]:
                raise ValueError("Installed Lepton link differs: " + name)
        elif not stat.S_ISDIR(info.st_mode):
            raise ValueError("Installed Lepton directory differs: " + name)
    metadata = root_path(root, BUILD_RECORD)
    if not metadata.is_file() or digest(metadata) != record["manifest_sha256"]:
        raise ValueError("Installed Lepton build record differs")


def install(directory, root, record):
    directory = Path(directory)
    archive, manifest = directory / "lepton.tar.gz", directory / "manifest.json"
    if digest(archive) != record["archive_sha256"] or digest(manifest) != record["manifest_sha256"]:
        raise ValueError("Lepton bundle changed before installation")
    prefix, metadata = root_path(root, PREFIX), root_path(root, BUILD_RECORD)
    if prefix.exists() or metadata.exists():
        raise ValueError("Staging root already supplies Lepton")
    prefix.mkdir(parents=True)
    metadata.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(["tar", "--xattrs", "--acls", "--numeric-owner", "-xpf", str(archive), "-C", str(prefix)],
                   check=True, timeout=60)
    shutil.copy2(manifest, metadata)
    verify_install(root, record)


def configure_runtime(root, host, host_record):
    root = Path(root)
    if validate_host(host) != host_record:
        raise ValueError("Fossilize bundle changed before installation")
    for name in [*HOST_FILES, "manifest.json"]:
        destination = root_path(root, HOST_RECORD if name == "manifest.json" else name)
        if destination.exists():
            raise ValueError("Staging root already supplies a Fossilize file: " + name)
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(Path(host) / name, destination)
    storage = root_path(root, STORAGE_CONFIG)
    if storage.exists():
        raise ValueError("Staging root already supplies a Podman storage override")
    accounts = [line.split(":") for line in root_path(root, "etc/passwd").read_text().splitlines()]
    account = [entry for entry in accounts if entry[0] == "vr"]
    if len(account) != 1 or len(account[0]) != 7 or account[0][5] != "/home/vr":
        raise ValueError("Expected the headset runtime user with its standard home")
    uid, gid = int(account[0][2]), int(account[0][3])
    if uid <= 0 or gid <= 0:
        raise ValueError("Lepton requires an unprivileged runtime user")
    storage.parent.mkdir(parents=True, exist_ok=True)
    storage.write_text(STORAGE_CONTENT)
    storage.chmod(0o644)
    for path in (storage, storage.parent, storage.parent.parent):
        os.chown(path, uid, gid)
    metadata = subprocess.check_output(["chroot", str(root), "/usr/bin/rpm", "-q", "--qf",
                                       "[%{FILENAMES} %{FILECAPS}\\n]", "shadow-utils"], text=True, timeout=30).splitlines()
    helpers = {}
    for name, capability in HELPERS.items():
        relative = "usr/bin/" + name
        path = root_path(root, relative)
        info = path.stat()
        if (not path.is_file() or info.st_uid != 0 or info.st_gid != 0 or
                stat.S_IMODE(info.st_mode) != 0o755 or "/" + relative + " " + capability not in metadata):
            raise ValueError("Unexpected UID-map helper or package metadata: " + name)
        # Docker exports omit the file capabilities recorded in the RPM database.
        subprocess.run(["chroot", str(root), "/usr/bin/setcap", capability, "/" + relative], check=True, timeout=10)
        actual = subprocess.check_output(["chroot", str(root), "/usr/bin/getcap", "/" + relative], text=True, timeout=10).strip()
        if actual != "/" + relative + " " + capability:
            raise ValueError("UID-map helper capability restoration failed: " + name)
        helpers[name] = {"sha256": digest(path), "mode": 0o755,
                         "capability": os.getxattr(path, "security.capability").hex()}
    adb = root_path(root, "usr/bin/adb")
    bridge = root_path(root, ADB_LINK)
    if not adb.is_file() or bridge.exists():
        raise ValueError("Expected Fedora ADB without an existing Android SDK bridge")
    bridge.parent.mkdir(parents=True, exist_ok=True)
    bridge.symlink_to("/usr/bin/adb")
    return {"helpers": helpers, "adb_sha256": digest(adb), "adb_link": "/usr/bin/adb", "host": host_record,
            "storage_config_sha256": digest(storage)}


def verify_runtime(root, record, capabilities):
    root = Path(root)
    for item in record["host"]["files"]:
        path = root_path(root, item["path"])
        if digest(path) != item["sha256"] or stat.S_IMODE(path.stat().st_mode) != item["mode"]:
            raise ValueError("Installed Fossilize file differs: " + item["path"])
    if digest(root_path(root, HOST_RECORD)) != record["host"]["manifest_sha256"]:
        raise ValueError("Installed Fossilize build record differs")
    storage = root_path(root, STORAGE_CONFIG)
    if (digest(storage) != record["storage_config_sha256"] or storage.read_text() != STORAGE_CONTENT or
            stat.S_IMODE(storage.stat().st_mode) != 0o644):
        raise ValueError("Installed rootless storage configuration differs")
    for name, expected in record["helpers"].items():
        if name not in HELPERS:
            raise ValueError("Unexpected UID-map helper")
        path = root_path(root, "usr/bin/" + name)
        info = path.stat()
        if (not stat.S_ISREG(info.st_mode) or info.st_uid != 0 or info.st_gid != 0 or
                stat.S_IMODE(info.st_mode) != expected["mode"] or
                digest(path) != expected["sha256"] or
                (Path(capabilities) / (name + ".cap")).read_bytes().hex() != expected["capability"]):
            raise ValueError("UID-map helper bytes, mode or capability differ: " + name)
    if digest(root_path(root, "usr/bin/adb")) != record["adb_sha256"]:
        raise ValueError("Installed ADB differs")
    parent = root_path(root, str(Path(ADB_LINK).parent))
    bridge = parent / "adb"
    if not bridge.is_symlink() or os.readlink(bridge) != record["adb_link"]:
        raise ValueError("Android SDK ADB bridge differs")
