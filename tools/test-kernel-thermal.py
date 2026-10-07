#!/usr/bin/env python3
"""Exercise the vendor thermal governor and critical shutdown in a diskless QEMU guest."""
import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import platform
import shutil
import stat
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def kernel_inputs(kernel):
    report = json.loads((kernel / "build.json").read_text())
    if report.get("status") != "compiled" or report.get("qemu_transports") is not True:
        raise ValueError("Requires a compiled QEMU transport kernel; headset builds are refused")
    expected = {item["path"]: item["sha256"] for item in report["artifacts"]}
    if (kernel / "Image").is_symlink() or digest(kernel / "Image") != expected.get("Image"):
        raise ValueError("Kernel Image checksum mismatch")
    command = next(item for item in report["commands"] if item[0] == "make" and "olddefconfig" in item)
    source = PurePosixPath(command[command.index("-C") + 1])
    build = PurePosixPath(next(item[2:] for item in command if item.startswith("O=")))
    for path in (source, build):
        if not path.is_relative_to("/work/linux-userspace") or ".." in path.parts:
            raise ValueError("Kernel cache paths must be under /work/linux-userspace")
    return str(source), str(build)


def initramfs(init, module):
    archive = bytearray()

    def entry(name, mode, data=b"", rdev=(0, 0)):
        encoded = name.encode() + b"\0"
        fields = (1, mode, 0, 0, 1, 0, len(data), 0, 0, *rdev, len(encoded), 0)
        archive.extend(b"070701" + "".join(f"{field:08x}" for field in fields).encode() + encoded)
        archive.extend(b"\0" * (-len(archive) % 4))
        archive.extend(data)
        archive.extend(b"\0" * (-len(archive) % 4))

    for name in ("dev", "proc", "sys", "sbin"):
        entry(name, stat.S_IFDIR | 0o755)
    entry("dev/console", stat.S_IFCHR | 0o600, rdev=(5, 1))
    entry("init", stat.S_IFREG | 0o755, init)
    entry("sbin/poweroff", stat.S_IFLNK | 0o777, b"/init")
    entry("armada_thermal_test.ko", stat.S_IFREG | 0o600, module)
    entry("TRAILER!!!", 0)
    return bytes(archive)


def run(args):
    kernel, output = args.kernel.resolve(), args.output.resolve()
    source, build = kernel_inputs(kernel)
    if output.exists() or any("," in str(path) for path in (kernel, output, ROOT)):
        raise ValueError("Choose a new output directory and paths without commas")
    output.mkdir(parents=True)
    fixture = ROOT / "tests/kernel-thermal"
    shutil.copytree(fixture, output / "module")
    report = {"schema_version": 1, "target": "qemu-kernel-thermal", "status": "started",
              "physical_control_tested": False, "flash_image": False,
              "kernel_build_sha256": digest(kernel / "build.json"), "kernel_sha256": digest(kernel / "Image"),
              "fixture_sha256": {p.name: digest(p) for p in sorted(fixture.iterdir()) if p.is_file()},
              "commands": []}
    started = time.monotonic()
    try:
        script = 'set -eu\nmake -C "$1" O="$2" M=/output/module "LD=ld.lld --threads=1 --lto-partitions=1" "PAHOLE_FLAGS=--skip_encoding_btf_enum64 --jobs=1" -j1 modules\ngcc -static -O2 -Wall -Wextra -Werror /test/init.c -o /output/init'
        command = [args.engine, "run", "--rm", "--network", "none", "--cpus", "1", "--memory", "1g",
                   "--memory-swap", "1g", "--pids-limit", "128",
                   "-v", f"{args.cache_volume}:/work:ro", "-v", f"{fixture}:/test:ro", "-v", f"{output}:/output"]
        for key, value in {"ARCH": "arm64", "LLVM": "1", "LLVM_IAS": "1", "REAL_CC": "clang",
                           "CROSS_COMPILE": "aarch64-linux-gnu-", "CROSS_COMPILE_COMPAT": "arm-linux-gnueabi-",
                           "CROSS_COMPILE_ARM32": "arm-linux-gnueabi-"}.items():
            command += ["-e", f"{key}={value}"]
        command += ["--entrypoint", "timeout", args.image, "--kill-after=5", "120", "bash", "-c", script,
                    "thermal-build", source, build]
        report["commands"].append(command)
        with (output / "build.log").open("wb") as log:
            subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=150)
        archive = initramfs((output / "init").read_bytes(), (output / "module/armada_thermal_test.ko").read_bytes())
        (output / "initramfs.cpio").write_bytes(archive)
        report["artifacts_sha256"] = {name: digest(output / name) for name in
                                      ("init", "module/armada_thermal_test.ko", "initramfs.cpio")}
        command = [args.qemu, "-name", "Armada thermal test", "-machine", "virt", "-accel", args.accel,
                   "-cpu", "host" if args.accel in ("hvf", "kvm") else "max", "-smp", "2", "-m", "1024",
                   "-nodefaults", "-nographic", "-monitor", "none", "-serial", "stdio", "-no-reboot",
                   "-nic", "none", "-kernel", str(kernel / "Image"), "-initrd", str(output / "initramfs.cpio"),
                   "-append", "console=ttyAMA0 earlycon rdinit=/init selinux=0 audit=0 armada.thermal_test=1"]
        report["cases"] = []
        for name in ("orderly", "stalled-helper"):
            boot_command = command.copy()
            if name == "stalled-helper":
                boot_command[-1] += " armada.thermal_stall=1"
            report["commands"].append(boot_command)
            case = {"name": name, "status": "started"}
            report["cases"].append(case)
            with (output / f"{name}.log").open("wb") as log:
                process = subprocess.Popen(boot_command, stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT)
                try:
                    case["returncode"] = process.wait(timeout=60)
                except subprocess.TimeoutExpired:
                    process.terminate()
                    try:
                        process.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait(timeout=5)
                    raise TimeoutError(f"Thermal {name} guest did not shut down within 60 seconds")
            boot = (output / f"{name}.log").read_text(errors="replace")
            expected = ["THERMAL_BELOW_CRITICAL_PASS", "reboot: Power down"]
            expected += (["THERMAL_ORDERLY_POWEROFF_PASS"] if name == "orderly" else
                         ["THERMAL_POWEROFF_STALLED", "Attempting kernel_power_off: Temperature too high"])
            if case["returncode"] or "THERMAL_TEST_FAIL" in boot or not all(item in boot for item in expected):
                case["status"] = "failed"
                raise RuntimeError(f"Kernel thermal acceptance failed; inspect {name}.log")
            if name == "orderly" and "Attempting kernel_power_off" in boot:
                raise RuntimeError("Emergency cutoff beat the orderly shutdown")
            case["status"] = "passed"
        report["status"] = "passed"
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        report.update(status="failed", error=str(error))
        raise
    finally:
        report["elapsed_seconds"] = round(time.monotonic() - started, 2)
        (output / "result.json").write_text(json.dumps(report, indent=2) + "\n")
    print(f"Kernel cooling, hysteresis, orderly and fallback shutdown passed in diskless QEMU: {output}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("kernel", type=Path)
    parser.add_argument("--output", type=Path, default=Path("output/kernel-thermal"))
    parser.add_argument("--cache-volume", default="armada-vr-quest3-kernel")
    parser.add_argument("--image", default="localhost/armada-vr:kernel")
    parser.add_argument("--engine", default=os.environ.get("CONTAINER_ENGINE", "docker"))
    parser.add_argument("--qemu", default="qemu-system-aarch64")
    parser.add_argument("--accel", choices=("hvf", "kvm", "tcg"),
                        default="hvf" if platform.system() == "Darwin" and platform.machine() == "arm64" else "tcg")
    args = parser.parse_args()
    if not args.cache_volume or any(char not in "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.-" for char in args.cache_volume):
        parser.error("cache-volume must name an existing container volume")
    try:
        run(args)
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        parser.exit(1, f"{error}\n")
