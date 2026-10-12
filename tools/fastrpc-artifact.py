"""Validate and install the native Pico FastRPC bundle into an offline root."""

import hashlib
import json
import os
from pathlib import Path
import shutil
import stat
import subprocess

ROOT = Path(__file__).resolve().parents[1]
PREFIX = "opt/armada-vr/fastrpc"
BUILD_RECORD = "usr/share/armada-vr/fastrpc-build.json"


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def contents(prefix):
    prefix = Path(prefix)
    if prefix.is_symlink() or not prefix.is_dir():
        raise ValueError("Expected a regular FastRPC prefix")
    result = {}
    for path in (prefix, *sorted(prefix.rglob("*"))):
        name = str(path.relative_to(prefix))
        mode = stat.S_IMODE(path.lstat().st_mode)
        if path.is_symlink():
            target = path.readlink()
            if target.is_absolute() or not path.is_file() or not path.resolve().is_relative_to(prefix.resolve()):
                raise ValueError("FastRPC link escapes its prefix or is broken: " + name)
            result[name] = {"symlink": str(target)}
        elif path.is_dir():
            if mode != 0o755:
                raise ValueError("Unexpected FastRPC directory mode: " + name)
        elif path.is_file() and mode in (0o644, 0o755):
            result[name] = {"sha256": digest(path), "mode": mode}
        else:
            raise ValueError("Unexpected FastRPC file type or mode: " + name)
    return result


def package(directory):
    directory = Path(directory)
    report = json.loads((directory / "build.json").read_text())
    if (report.get("status") != "compiled" or report.get("hardware_verified") is not False
            or report.get("flash_ready") is not False):
        raise ValueError("Expected an offline compiled FastRPC bundle")
    profile = json.loads((ROOT / "profiles/pico-fastrpc.json").read_text())
    names = ["profiles/pico-fastrpc.json", "tools/fastrpc-build.py", "tools/build-fastrpc.sh",
             "Containerfile.fastrpc", "tests/pico-rpcmem.c", "tests/pico-fastrpc-upstream.c",
             "tests/pico-fastrpc-runtime.c", "tests/pico-sensor-rpc.c", *profile["patches"],
             *("src/" + name for name in profile["sources"])]
    inputs = {name: digest(ROOT / name) for name in names}
    if report.get("profile") != profile or report.get("inputs_sha256") != inputs:
        raise ValueError("FastRPC profile or build inputs changed")
    prefix = directory / PREFIX
    tree = contents(prefix)
    if tree != report.get("contents"):
        raise ValueError("FastRPC inventory or hashes changed")
    files = {"lib64/libsdsprpc.so.1.0.0", "lib64/libpico-sensors.so.1",
             "include/pico-sensor-rpc.h", "include/sns-low-latency.h",
             "share/licenses/fastrpc/LICENSE.txt", "share/fastrpc/source.tar.gz"}
    files.update("share/fastrpc/" + name for name in names)
    links = {"lib64/libsdsprpc.so": "libsdsprpc.so.1.0.0",
             "lib64/libsdsprpc.so.1": "libsdsprpc.so.1.0.0",
             "lib64/libpico-sensors.so": "libpico-sensors.so.1"}
    for name, dependency in profile["runtime_dependencies"].items():
        files.update(("lib64/" + dependency["target"],
                      "share/licenses/" + name.split(":")[0] + "/copyright"))
        links["lib64/" + dependency["library"]] = dependency["target"]
    if (set(tree) != files | set(links)
            or any(tree[name] != {"symlink": target} for name, target in links.items())):
        raise ValueError("Unexpected FastRPC runtime inventory")
    for name in files:
        path = prefix / name
        executable = name.startswith("lib64/")
        if tree[name].get("mode") != (0o755 if executable else 0o644):
            raise ValueError("Unexpected FastRPC runtime mode: " + name)
        if executable:
            with path.open("rb") as stream:
                header = stream.read(20)
            if header[:7] != b"\x7fELF\x02\x01\x01" or header[18:20] != b"\xb7\x00":
                raise ValueError("Expected a native ARM64 FastRPC ELF: " + name)
        elif not path.stat().st_size:
            raise ValueError("Empty FastRPC metadata or license: " + name)
    for name, checksum in inputs.items():
        if digest(prefix / "share/fastrpc" / name) != checksum:
            raise ValueError("FastRPC packaged source differs: " + name)
    for name in ("pico-sensor-rpc.h", "sns-low-latency.h"):
        if digest(prefix / "include" / name) != inputs["src/" + name]:
            raise ValueError("FastRPC packaged header differs: " + name)
    if digest(prefix / "share/fastrpc/source.tar.gz") != profile["source_archive_sha256"]:
        raise ValueError("FastRPC source archive differs")
    return {"build_sha256": digest(directory / "build.json"), "contents": tree,
            "hardware_verified": False, "flash_ready": False, "automatic_startup": False}


def root_path(root, name):
    root = Path(root)
    if not root.is_dir() or root.is_symlink() or root.resolve() == Path("/"):
        raise ValueError("Expected an offline staging root")
    path = root / name
    for parent in (path, *path.parents):
        if parent == root:
            break
        if parent.is_symlink():
            raise ValueError("FastRPC destination contains a symlink")
    if not path.resolve().is_relative_to(root.resolve()):
        raise ValueError("FastRPC destination escapes its root")
    return path


def verify_install(root, record):
    if contents(root_path(root, PREFIX)) != record["contents"]:
        raise ValueError("Installed FastRPC inventory, modes or hashes differ")
    if digest(root_path(root, BUILD_RECORD)) != record["build_sha256"]:
        raise ValueError("Installed FastRPC build record differs")


def install(directory, root, record):
    directory = Path(directory)
    if (digest(directory / "build.json") != record["build_sha256"]
            or contents(directory / PREFIX) != record["contents"]):
        raise ValueError("FastRPC bundle changed before installation")
    prefix, metadata = root_path(root, PREFIX), root_path(root, BUILD_RECORD)
    if prefix.exists() or metadata.exists():
        raise ValueError("Root already supplies FastRPC")
    prefix.parent.mkdir(parents=True, exist_ok=True)
    metadata.parent.mkdir(parents=True, exist_ok=True)
    shutil.copytree(directory / PREFIX, prefix, symlinks=True)
    shutil.copyfile(directory / "build.json", metadata)
    metadata.chmod(0o644)
    verify_install(root, record)


def check_loader(root):
    root = Path(root).absolute()
    root_path(root, PREFIX)

    def enter_root():
        os.chroot(root)
        os.chdir("/")

    environment = {"LC_ALL": "C", "LD_TRACE_LOADED_OBJECTS": "1", "LD_WARN": "yes",
                   "LD_BIND_NOW": "yes", "LD_VERBOSE": "yes",
                   "LD_LIBRARY_PATH": "/" + PREFIX + "/lib64:/usr/lib64"}
    result = subprocess.run(["/lib/ld-linux-aarch64.so.1", "/" + PREFIX + "/lib64/libpico-sensors.so.1"],
                            preexec_fn=enter_root, env=environment, capture_output=True,
                            text=True, timeout=15)
    trace = result.stdout + result.stderr
    if (result.returncode or "not found" in trace or "undefined symbol" in trace
            or "/" + PREFIX + "/lib64/libsdsprpc.so.1" not in trace):
        raise ValueError("FastRPC root loader or relocations failed: " + trace)
    return {"status": "passed", "trace": trace, "physical_sensor_access": False}


def verify_image(image, record, destination):
    destination = Path(destination)
    if any(c in str(destination) for c in ('"', '\\', '\n', '\r')):
        raise ValueError("Invalid FastRPC readback directory")
    destination.mkdir(parents=True, exist_ok=False)
    for name in (PREFIX, BUILD_RECORD):
        parent = destination / Path(name).parent
        parent.mkdir(parents=True, exist_ok=True)
        subprocess.run(["debugfs", "-R", f'rdump /{name} "{parent}"', str(image)],
                       check=True, timeout=30, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    verify_install(destination, record)
