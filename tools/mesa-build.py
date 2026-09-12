#!/usr/bin/env python3
"""Build a pinned Mesa development driver without installing it."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import tarfile
import time

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("kernel_source", ROOT / "tools/kernel-source.py")
sources = importlib.util.module_from_spec(spec)
spec.loader.exec_module(sources)


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def tree_digest(root):
    entries = []
    for path in sorted(root.rglob("*")):
        relative = path.relative_to(root).as_posix()
        if path.is_symlink():
            entries.append([relative, "symlink", os.readlink(path)])
        elif path.is_file():
            entries.append([relative, path.stat().st_mode & 0o777, digest(path)])
        elif path.is_dir():
            entries.append([relative, "directory"])
        else:
            raise ValueError(f"Unexpected source file: {relative}")
    return hashlib.sha256(json.dumps(entries).encode()).hexdigest()


def build(profile, driver="lavapipe"):
    if sys.platform != "linux":
        raise ValueError("Use just build-mesa on macOS")
    archive, output = Path("/archive.tar.xz"), Path("/output")
    if any(output.iterdir()):
        raise ValueError("Output must be empty")
    sources.full_archive(profile, archive)
    patch_dir = ROOT / "patches/mesa"
    patches = sorted((patch_dir if driver == "lavapipe" else patch_dir / "turnip").glob("*.patch"))
    identity = {"profile": profile, "driver": driver,
                "patches": [{"path": str(patch.relative_to(ROOT)), "sha256": digest(patch)} for patch in patches],
                "image": os.environ["MESA_BUILD_IMAGE_ID"], "builder_sha256": digest(Path(__file__))}
    key = hashlib.sha256(json.dumps(identity, sort_keys=True).encode()).hexdigest()[:16]
    cache = Path("/work") / key
    source = cache / profile["full_source"]["archive_root"]
    build_dir, stamp = cache / "build", cache / "source.sha256"
    report = {"status": "started", "identity": identity, "commands": [], "artifacts": [],
              "limits": {"memory_bytes": 4 * 1024**3, "jobs": 2, "compile_seconds": 1800},
              "steamvr_rendering_verified": False, "hardware_driver_verified": False}
    started = time.monotonic()
    env = {**os.environ, "CC": "clang", "CXX": "clang++", "PYTHONDONTWRITEBYTECODE": "1",
           "LDFLAGS": "-fuse-ld=lld -Wl,--threads=1"}

    def run(command, name, timeout=180):
        report["commands"].append(command)
        print(f"Running {name}", flush=True)
        with (output / name).open("wb") as log:
            process = subprocess.Popen(command, env=env, stdout=log, stderr=subprocess.STDOUT,
                                       start_new_session=True)
            try:
                code = process.wait(timeout=timeout)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid, signal.SIGKILL)
                process.wait()
                raise
        if code:
            raise subprocess.CalledProcessError(code, command)

    try:
        cache.mkdir(parents=True, exist_ok=True)
        if not source.exists():
            with tarfile.open(archive) as stream:
                stream.extractall(cache, filter="data")
            for index, patch in enumerate(patches):
                run(["patch", "--batch", "--fuzz=0", "-d", str(source), "-p1", "-i", str(patch)], f"patch-{index + 1}.log")
            # Meson creates these even with wrap downloads disabled; fingerprint them too.
            (source / "subprojects/packagecache").mkdir(exist_ok=True)
            (source / "subprojects/.wraplock").touch(exist_ok=True)
            stamp.write_text(tree_digest(source))
        if not stamp.is_file() or tree_digest(source) != stamp.read_text():
            raise ValueError("Cached Mesa source changed or extraction was interrupted")
        if driver == "turnip":
            run([sys.executable, str(ROOT / "tests/kgsl-fence-tests.py"),
                 str(source / "src/freedreno/vulkan/tu_knl_kgsl.cc"), str(output)],
                "kgsl-fence-tests.log")
            report["kgsl_fence_tests"] = {
                "status": "passed",
                "scope": "software descriptor model; no GPU ioctls",
                "runner_sha256": digest(ROOT / "tests/kgsl-fence-tests.py"),
                "model_sha256": digest(ROOT / "tests/kgsl-fence-tests.cpp"),
                "helpers_sha256": digest(output / "kgsl-sync-under-test.h"),
                "executable_sha256": digest(output / "kgsl-fence-tests"),
            }
            run([sys.executable, str(ROOT / "tests/kgsl-wait-tests.py"),
                 str(source / "src/freedreno/vulkan/tu_knl_kgsl.cc"), str(output)],
                "kgsl-wait-tests.log")
            report["kgsl_wait_tests"] = {
                "status": "passed",
                "scope": "real poll with pipes modeling fences; no GPU ioctls",
                "runner_sha256": digest(ROOT / "tests/kgsl-wait-tests.py"),
                "model_sha256": digest(ROOT / "tests/kgsl-wait-tests.cpp"),
                "helpers_sha256": digest(output / "kgsl-wait-under-test.h"),
                "executable_sha256": digest(output / "kgsl-wait-tests"),
            }
            run([sys.executable, str(ROOT / "tests/kgsl-submit-tests.py"),
                 str(source / "src/freedreno/vulkan/tu_knl_kgsl.cc"), str(output)],
                "kgsl-submit-tests.log", timeout=120)
            report["kgsl_submit_tests"] = {
                "status": "passed",
                "scope": "actual wrapper with modeled DRM/native submission; real descriptor ownership",
                "runner_sha256": digest(ROOT / "tests/kgsl-submit-tests.py"),
                "model_sha256": digest(ROOT / "tests/kgsl-submit-tests.cpp"),
                "helpers_sha256": digest(output / "kgsl-submit-under-test.h"),
                "executable_sha256": digest(output / "kgsl-submit-tests"),
            }
            run([sys.executable, str(ROOT / "tests/kgsl-display-tests.py"),
                 str(source / "src/freedreno/vulkan/tu_kgsl_display.h"), str(output)],
                "kgsl-display-tests.log", timeout=120)
            report["kgsl_display_tests"] = {
                "status": "passed",
                "scope": "actual display helper with modeled DRM failures and real descriptor ownership",
                "runner_sha256": digest(ROOT / "tests/kgsl-display-tests.py"),
                "model_sha256": digest(ROOT / "tests/kgsl-display-tests.cpp"),
                "helpers_sha256": digest(output / "tu_kgsl_display.h"),
                "executable_sha256": digest(output / "kgsl-display-tests"),
            }
        report["source_sha256"] = stamp.read_text()
        (output / "packages.txt").write_text(subprocess.check_output(["rpm", "-qa"], text=True))
        if not (build_dir / "build.ninja").is_file():
            options = profile["meson_options" if driver == "lavapipe" else "turnip_meson_options"]
            run(["meson", "setup", str(build_dir), str(source), *options], "configure.log")
        if driver == "lavapipe":
            targets = "src/gallium/targets/lavapipe/"
            names = ["libvulkan_lvp.so", "lvp_icd.aarch64.json"]
            run(["ninja", "-C", str(build_dir), "-j2", *(targets + name for name in names)],
                "compile.log", timeout=1800)
        else:
            targets = "src/freedreno/vulkan/"
            names = ["libvulkan_freedreno.so", "freedreno_icd.aarch64.json"]
            run(["ninja", "-C", str(build_dir), "-j2"], "compile.log", timeout=1800)
            run(["meson", "test", "-C", str(build_dir), "--no-rebuild", "--suite", "freedreno",
                 "--num-processes", "1", "--print-errorlogs"], "freedreno-tests.log", timeout=180)
            tests = build_dir / "meson-logs/testlog.json"
            shutil.copy2(tests, output / "freedreno-tests.jsonl")
            outcomes = [json.loads(line) for line in tests.read_text().splitlines() if line]
            if not outcomes or any(test["result"] != "OK" for test in outcomes):
                raise ValueError("Freedreno offline tests did not all execute successfully")
            report["offline_tests_passed"] = len(outcomes)
            header = source / "src/freedreno/vulkan/tu_kgsl_drm_sync.h"
            shutil.copy2(header, output / header.name)
            drm_flags = subprocess.check_output(["pkg-config", "--cflags", "--libs", "libdrm"], text=True).split()
            run(["clang", "-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror",
                 "-I", str(header.parent), str(ROOT / "tests/kgsl-drm-sync.c"),
                 *drm_flags, "-o", str(output / "kgsl-drm-sync")], "kgsl-drm-sync-build.log")
            report["kgsl_drm_test"] = {
                "status": "compiled-not-run",
                "scope": "real kernel software fences; requires the QEMU ABI test kernel",
                "source_sha256": digest(ROOT / "tests/kgsl-drm-sync.c"),
                "header_sha256": digest(header),
            }
            run(["clang", "-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror",
                 "-I", str(header.parent), str(ROOT / "tests/kgsl-display.c"),
                 *drm_flags, "-o", str(output / "kgsl-display")], "kgsl-display-build.log")
            report["kgsl_display_test"] = {
                "status": "compiled-not-run",
                "scope": "real DRM descriptor ownership; requires a diskless QEMU transport kernel",
                "source_sha256": digest(ROOT / "tests/kgsl-display.c"),
                "header_sha256": digest(output / "tu_kgsl_display.h"),
            }
            names += ["tu_kgsl_drm_sync.h", "kgsl-drm-sync", "tu_kgsl_display.h", "kgsl-display"]
        for name in names:
            if name not in ("tu_kgsl_drm_sync.h", "kgsl-drm-sync", "tu_kgsl_display.h", "kgsl-display"):
                shutil.copy2(build_dir / targets / name, output / name)
        for path in (ROOT / "tests/vulkan-external-sync.c", ROOT / "src/vulkan-interop.c"):
            run(["clang", "-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror", str(path),
                 "-lvulkan", "-o", str(output / path.stem)], path.stem + "-build.log")
        if tree_digest(source) != report["source_sha256"]:
            raise ValueError("Mesa source changed during compilation")
        run(["ldd", str(output / names[0])], "dependencies.txt")
        if driver == "turnip":
            commands = json.loads((build_dir / "compile_commands.json").read_text())
            kgsl = [entry["command"] for entry in commands if entry["file"].endswith("/tu_knl_kgsl.cc")]
            if len(kgsl) != 1 or "-DHAVE_LIBDRM " not in kgsl[0] or "-DMESA_SYSTEM_HAS_KMS_DRM=1 " not in kgsl[0]:
                raise ValueError("KGSL compile omitted the DRM bridge")
            dynamic = subprocess.check_output(["readelf", "-d", str(output / names[0])], text=True)
            library = (output / names[0]).read_bytes()
            if ("libdrm.so.2" not in dynamic or b"TU_KGSL_DRM_SYNC" not in library or
                    b"TU_KGSL_DISPLAY" not in library):
                raise ValueError("Turnip library is missing the DRM bridge or dependency")
            (output / "kgsl-compile-command.txt").write_text(kgsl[0] + "\n")
            (output / "dynamic-dependencies.txt").write_text(dynamic)
            report["kgsl_drm_bridge_compiled"] = True
            report["kgsl_display_compiled"] = True
        evidence = ["kgsl-compile-command.txt", "dynamic-dependencies.txt"] if driver == "turnip" else []
        for name in (*names, "vulkan-external-sync", "vulkan-interop", *evidence):
            path = output / name
            report["artifacts"].append({"path": name, "bytes": path.stat().st_size, "sha256": digest(path)})
        report["status"] = "compiled"
    except Exception as error:
        report["status"], report["error"] = "failed", str(error)
        raise
    finally:
        report["elapsed_seconds"] = round(time.monotonic() - started, 2)
        report["resources"] = {name: (Path("/sys/fs/cgroup") / name).read_text().strip()
                               for name in ("memory.peak", "memory.events", "pids.peak")}
        (output / "build.json").write_text(json.dumps(report, indent=2) + "\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("fetch", "build"))
    parser.add_argument("--driver", choices=("lavapipe", "turnip"), default="lavapipe")
    args = parser.parse_args()
    profile = json.loads((ROOT / "profiles/mesa.json").read_text())
    if args.action == "fetch":
        archive = ROOT / "output/downloads/mesa-26.1.8.tar.xz"
        sources.full_archive(profile, archive)
        print(f"Verified {archive}")
    else:
        build(profile, args.driver)


if __name__ == "__main__":
    main()
