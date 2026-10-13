#!/usr/bin/env python3
"""Build an initramfs-only Pico USB diagnostic; never contact a headset."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import stat
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "system/pico/diagnostic-init.c"
spec = importlib.util.spec_from_file_location("initramfs", ROOT / "tools/build-headset-initramfs.py")
kernel_tools = importlib.util.module_from_spec(spec)
spec.loader.exec_module(kernel_tools)
REQUIRED = ("DEVTMPFS", "PROC_FS", "SYSFS", "TMPFS", "CONFIGFS_FS", "USB_DWC3_MSM",
            "USB_CONFIGFS", "USB_CONFIGFS_SERIAL", "USB_F_SERIAL", "USB_U_SERIAL")


def serial(value):
    if not re.fullmatch(r"[A-Za-z0-9_.-]{1,64}", value) or value == "qemu":
        raise ValueError("An explicit physical headset serial of at most 64 characters is required")
    return value


def static_arm64(data):
    if len(data) < 64 or data[:7] != b"\x7fELF\x02\x01\x01" or struct.unpack_from("<HH", data, 16) != (2, 183):
        raise ValueError("Expected a static little-endian ARM64 executable")
    offset = struct.unpack_from("<Q", data, 32)[0]
    size, count = struct.unpack_from("<HH", data, 54)
    if size != 56 or not 0 < count < 128 or offset + size * count > len(data):
        raise ValueError("Invalid executable program headers")
    if any(struct.unpack_from("<I", data, offset + i * size)[0] in (2, 3) for i in range(count)):
        raise ValueError("Diagnostic must have no dynamic linker or dynamic dependencies")


def archive(binary, target_serial):
    static_arm64(binary)
    entries = [(name, stat.S_IFDIR | 0o755, b"") for name in (".", "dev", "proc", "sys", "etc")]
    entries += [("init", stat.S_IFREG | 0o755, binary),
                ("etc/pico-serial", stat.S_IFREG | 0o600, (target_serial + "\n").encode()),
                ("TRAILER!!!", 0, b"")]
    result = bytearray()
    for inode, (name, mode, data) in enumerate(entries, 1):
        encoded = name.encode() + b"\0"
        fields = (inode, mode, 0, 0, 1, 0, len(data), 0, 0, 0, 0, len(encoded), 0)
        result.extend(b"070701" + "".join(f"{v:08x}" for v in fields).encode() + encoded)
        result.extend(b"\0" * (-len(result) % 4))
        result.extend(data)
        result.extend(b"\0" * (-len(result) % 4))
    return result


def kernel_input(directory):
    build, hashes, _, _ = kernel_tools.inputs(directory)
    profile = json.loads((ROOT / "profiles/kernel/pico-neo3.json").read_text())
    if (build["qemu_transports"] is not False or build["variant"] != "linux-userspace" or
            build.get("repository") != profile["repository"] or build.get("commit") != profile["commit"]):
        raise ValueError("Diagnostic requires the pinned physical Pico kernel")
    config = set((directory / "resolved.config").read_text().splitlines())
    missing = [name for name in REQUIRED if f"CONFIG_{name}=y" not in config]
    if missing:
        raise ValueError("Diagnostic requires built-in drivers: " + ", ".join(missing))
    return hashes


def build(args):
    selected = serial(args.serial)
    hashes = kernel_input(args.kernel)
    compiler = shutil.which(args.zig)
    if not compiler:
        raise ValueError("Zig C compiler is unavailable")
    source_hash = kernel_tools.digest(SOURCE)
    args.output.mkdir(parents=True, exist_ok=False)
    environment = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=str(args.output / "compiler-cache"),
                       ZIG_LOCAL_CACHE_DIR=str(args.output / "local-cache"))
    command = [compiler, "cc", "-target", "aarch64-linux-musl", "-static", "-O2", "-g",
               "-Wall", "-Wextra", "-Werror", str(SOURCE), "-o", str(args.output / "init")]
    report = {"status": "started", "scope": "initramfs-only diagnostic; offline build",
              "flash_ready": False, "physical_boot_verified": False, "usb_hardware_verified": False,
              "kernel_build_sha256": kernel_tools.digest(args.kernel / "build.json"),
              "kernel_inputs": hashes, "source_sha256": source_hash, "command": command}
    try:
        report["compiler_version"] = subprocess.check_output([compiler, "version"], text=True, timeout=10).strip()
        with (args.output / "build.log").open("xb") as log:
            subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, env=environment, check=True, timeout=180)
        binary = (args.output / "init").read_bytes()
        for name, target in (("initramfs.cpio", selected), ("test-initramfs.cpio", "qemu")):
            (args.output / name).write_bytes(archive(binary, target))
        if source_hash != kernel_tools.digest(SOURCE) or hashes != kernel_input(args.kernel):
            raise ValueError("Build inputs changed during compilation")
        report["artifacts"] = [{"path": name, "bytes": (args.output / name).stat().st_size,
                                "sha256": kernel_tools.digest(args.output / name)}
                               for name in ("init", "initramfs.cpio", "test-initramfs.cpio")]
        report["cmdline"] = "rdinit=/init armada.pico=diagnostic selinux=1 enforcing=1"
        report["test_cmdline"] = "rdinit=/init armada.pico=diagnostic-test"
        report["status"] = "built"
    except Exception as error:
        report.update(status="failed", error=str(error))
        raise
    finally:
        (args.output / "build.json").write_text(json.dumps(report, indent=2) + "\n")
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("kernel", type=lambda value: Path(value).resolve())
    parser.add_argument("--serial", required=True)
    parser.add_argument("--output", required=True, type=lambda value: Path(value).absolute())
    parser.add_argument("--zig", default="zig")
    args = parser.parse_args()
    try:
        result = build(args)
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        parser.exit(1, f"{error}\n")
    print(json.dumps({k: result[k] for k in ("status", "flash_ready", "physical_boot_verified")}, indent=2))


if __name__ == "__main__":
    main()
