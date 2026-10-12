#!/usr/bin/env python3
"""Build the pinned Pico sensor RPC library in an offline ARM64 Linux container."""

import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import platform
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PREFIX = Path("opt/armada-vr/fastrpc")


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def verify_dependency_files(checksums, files):
    expected = {}
    for line in checksums.read_text().splitlines():
        checksum, name = line.split(maxsplit=1)
        if name in expected or not re.fullmatch(r"[0-9a-f]{32}", checksum):
            raise ValueError("Invalid dependency checksum manifest")
        expected[name] = checksum
    verified = {}
    for installed_name, path in files.items():
        with path.open("rb") as stream:
            actual = hashlib.file_digest(stream, "md5").hexdigest()
        if expected.get(installed_name.lstrip("/")) != actual:
            raise ValueError("Modified or unlisted dependency file: " + installed_name)
        verified[installed_name] = {"package_md5": actual, "sha256": digest(path)}
    return verified


def extract_source(archive, destination):
    with tarfile.open(archive) as stream:
        members = stream.getmembers()
        seen = set()
        total = 0
        for member in members:
            name = PurePosixPath(member.name)
            if (name.is_absolute() or ".." in name.parts or not name.parts or name in seen
                    or not (member.isfile() or member.isdir())
                    or member.size < 0 or member.size > 16 * 1024**2):
                raise ValueError("Unexpected entry in FastRPC source archive")
            seen.add(name)
            total += member.size
        if total > 128 * 1024**2 or len(members) > 10000:
            raise ValueError("FastRPC source archive exceeds extraction limits")
        for member in members:
            path = destination / member.name
            if member.isdir():
                path.mkdir(parents=True, exist_ok=True)
                path.chmod(0o755)
            else:
                path.parent.mkdir(parents=True, exist_ok=True)
                with stream.extractfile(member) as source, path.open("xb") as target:
                    shutil.copyfileobj(source, target)
                path.chmod(0o755 if member.mode & 0o111 else 0o644)


def build(archive, output, image):
    if sys.platform != "linux" or platform.machine() != "aarch64":
        raise ValueError("Build FastRPC in a native ARM64 Linux container")
    if not re.fullmatch(r"sha256:[0-9a-f]{64}", image):
        raise ValueError("Record the immutable compiler image ID")
    profile_path = ROOT / "profiles/pico-fastrpc.json"
    profile = json.loads(profile_path.read_text())
    if digest(archive) != profile["source_archive_sha256"]:
        raise ValueError("FastRPC archive differs from the pinned source")
    output.mkdir(parents=True, exist_ok=False)
    names = [
        "profiles/pico-fastrpc.json", "tools/fastrpc-build.py", "tools/build-fastrpc.sh",
        "Containerfile.fastrpc",
        "tests/pico-rpcmem.c", "tests/pico-fastrpc-upstream.c", "tests/pico-fastrpc-runtime.c",
        "tests/pico-sensor-rpc.c",
        *profile["patches"], *("src/" + name for name in profile["sources"]),
    ]
    report = {
        "status": "started", "image": image, "profile": profile,
        "inputs_sha256": {name: digest(ROOT / name) for name in names},
        "commands": [], "hardware_verified": False, "flash_ready": False,
    }

    def run(command, name, cwd=None, env=None, timeout=120, expected=0):
        report["commands"].append(command)
        with (output / name).open("xb") as log:
            result = subprocess.run(command, cwd=cwd, env=env, timeout=timeout,
                                    stdout=log, stderr=subprocess.STDOUT)
        if result.returncode != expected:
            raise RuntimeError(f"{name}: exit {result.returncode}, expected {expected}")

    try:
        with tempfile.TemporaryDirectory(prefix="pico-fastrpc-") as directory:
            work = Path(directory)
            extract_source(archive, work)
            source = work / profile["archive_root"]
            if list(work.iterdir()) != [source]:
                raise ValueError("Unexpected FastRPC archive root")
            for number, patch in enumerate(profile["patches"]):
                run(["patch", "--batch", "--fuzz=0", "-p1", "-i", str(ROOT / patch)],
                    f"patch-{number}.log", cwd=source)
            for name in profile["sources"]:
                shutil.copyfile(ROOT / "src" / name, source / "src" / name)
            source_inputs = {str(p.relative_to(source)): digest(p)
                             for p in source.rglob("*") if p.is_file()}
            report["source_sha256"] = hashlib.sha256(
                json.dumps(source_inputs, sort_keys=True).encode()
            ).hexdigest()
            environment = dict(os.environ, ASAN_OPTIONS="detect_leaks=1",
                               UBSAN_OPTIONS="halt_on_error=1")
            for test in ("pico-rpcmem", "pico-fastrpc-upstream", "pico-sensor-rpc"):
                binary = output / test
                extra = ([str(source / "src/pico-sensor-rpc.c"), str(source / "src/sns-low-latency.c")]
                         if test == "pico-sensor-rpc" else [])
                run(["gcc", "-std=c11", "-O1", "-g", "-UNDEBUG", "-Wall", "-Wextra",
                     "-Werror", "-Wno-unused-parameter", "-fsanitize=address,undefined",
                     "-fno-sanitize-recover=all", "-I", str(source / "inc"),
                     "-I", str(source / "src"), str(ROOT / "tests" / (test + ".c")),
                     *extra, "-o", str(binary)], test + "-build.log")
                run([str(binary)], test + ".log", env=environment, timeout=30)
                if (test.upper().replace("-", "_") + "_PASS") not in (output / (test + ".log")).read_text():
                    raise ValueError("Missing test completion marker: " + test)
            run(["autoreconf", "-fi"], "autoreconf.log", cwd=source)
            run(["./configure", "--prefix=/" + str(PREFIX),
                 "--libdir=/" + str(PREFIX / "lib64"), "CFLAGS=-O2 -g0"],
                "configure.log", cwd=source)
            run(["make", "-j2", "-C", "src", "libsdsprpc.la"], "compile.log", cwd=source,
                timeout=600)
            prefix = output / PREFIX
            (prefix / "lib64").mkdir(parents=True)

            def copy(source_file, target, mode=0o644):
                destination = prefix / target
                destination.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(source_file, destination)
                destination.chmod(mode)

            copy(source / "src/.libs/libsdsprpc.so.1.0.0", "lib64/libsdsprpc.so.1.0.0", 0o755)
            for name in ("libsdsprpc.so.1", "libsdsprpc.so"):
                (prefix / "lib64" / name).symlink_to("libsdsprpc.so.1.0.0")
            run(["gcc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", "-fPIC", "-shared",
                 "-I", str(source / "inc"), "-I", str(source / "src"),
                 str(source / "src/pico-sensor-rpc.c"), str(source / "src/sns-low-latency.c"),
                 "-L", str(prefix / "lib64"), "-lsdsprpc", "-Wl,-rpath,$ORIGIN", "-Wl,-z,defs",
                 "-Wl,-soname,libpico-sensors.so.1", "-o", str(prefix / "lib64/libpico-sensors.so.1")],
                "sensor-library-build.log")
            (prefix / "lib64/libpico-sensors.so").symlink_to("libpico-sensors.so.1")
            for name in ("pico-sensor-rpc.h", "sns-low-latency.h"):
                copy(source / "src" / name, "include/" + name)
            copy(source / "LICENSE.txt", "share/licenses/fastrpc/LICENSE.txt")
            copy(archive, "share/fastrpc/source.tar.gz")
            for name in names:
                copy(ROOT / name, "share/fastrpc/" + name)
            report["dependencies"] = {}
            for package, dependency in profile["runtime_dependencies"].items():
                installed = subprocess.check_output(["dpkg-query", "-W", package], text=True)
                if installed.strip().split() != [package, dependency["version"]]:
                    raise ValueError("Unexpected dependency version: " + package)
                library = Path("/usr/lib/aarch64-linux-gnu") / dependency["library"]
                if library.resolve().name != dependency["target"]:
                    raise ValueError("Unexpected dependency link: " + str(library))
                copyright_file = Path("/usr/share/doc") / dependency["copyright"]
                verified = verify_dependency_files(
                    Path("/var/lib/dpkg/info") / (package + ".md5sums"),
                    {str(library.resolve()): library.resolve(), str(copyright_file): copyright_file},
                )
                copy(library.resolve(), "lib64/" + dependency["target"], 0o755)
                (prefix / "lib64" / dependency["library"]).symlink_to(dependency["target"])
                copy(copyright_file,
                     "share/licenses/" + package.split(":")[0] + "/copyright")
                report["dependencies"][package] = {"version": dependency["version"], "files": verified}
            library_env = dict(os.environ, LD_LIBRARY_PATH=str(prefix / "lib64"))
            run(["ldd", "-r", str(prefix / "lib64/libsdsprpc.so.1.0.0"),
                 str(prefix / "lib64/libpico-sensors.so.1")],
                "dependencies.log", env=library_env)
            dependency_log = (output / "dependencies.log").read_text()
            if "not found" in dependency_log or "undefined symbol" in dependency_log:
                raise ValueError("Unresolved FastRPC dependency")
            run([sys.executable, "-c", "import ctypes,sys; ctypes.CDLL(sys.argv[1])",
                 str(prefix / "lib64/libsdsprpc.so.1.0.0")], "dlopen.log", env=library_env)
            run([sys.executable, "-c", "import ctypes,sys; ctypes.CDLL(sys.argv[1])",
                 str(prefix / "lib64/libpico-sensors.so.1")], "sensor-dlopen.log", env=library_env)
            run(["gcc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", "-I",
                 str(source / "inc"), "-I", str(source / "src"),
                 str(ROOT / "tests/pico-fastrpc-runtime.c"), "-ldl",
                 "-o", str(output / "runtime-test")], "runtime-test-build.log")
            run([str(output / "runtime-test")], "runtime-refusal.log", expected=2)
            if "PICO_RPCMEM_RUNTIME_REFUSED" not in (output / "runtime-refusal.log").read_text():
                raise ValueError("VM-only test refusal not confirmed")
            if any(digest(source / name) != value for name, value in source_inputs.items()):
                raise ValueError("Source files changed during compilation")
            report["contents"] = {
                str(p.relative_to(prefix)): {"symlink": str(p.readlink())} if p.is_symlink()
                else {"sha256": digest(p), "mode": p.stat().st_mode & 0o777}
                for p in sorted(prefix.rglob("*")) if p.is_file() or p.is_symlink()
            }
            if report["inputs_sha256"] != {name: digest(ROOT / name) for name in names}:
                raise ValueError("Build inputs changed during compilation")
            report["status"] = "compiled"
    except Exception as error:
        report.update(status="failed", error=str(error))
        raise
    finally:
        (output / "build.json").write_text(json.dumps(report, indent=2) + "\n")
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("archive", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--image", required=True)
    args = parser.parse_args()
    report = build(args.archive.resolve(), args.output.resolve(), args.image)
    print(json.dumps({"status": report["status"], "hardware_verified": False, "flash_ready": False}))


if __name__ == "__main__":
    main()
