#!/usr/bin/env python3
"""Boot a QEMU-transport vendor kernel against the existing ARM64 lab rootfs."""
import argparse
import hashlib
import json
import platform
from pathlib import Path
import subprocess
import time


def digest(path):
    if path.is_symlink() or not path.is_file() or "," in str(path):
        raise ValueError(f"Expected a regular image path without commas: {path}")
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def inputs(kernel, rootfs):
    build = json.loads((kernel / "build.json").read_text())
    manifest = json.loads((rootfs / "manifest.json").read_text())
    if build.get("status") != "compiled" or build.get("qemu_transports") is not True:
        raise ValueError("Expected a compiled vendor kernel with the separate QEMU transport configuration")
    if manifest.get("target") != "qemu-arm64" or manifest.get("hardware_flash_image") is not False:
        raise ValueError("Expected the QEMU-only lab rootfs manifest")
    expected = {entry["path"]: entry["sha256"] for entry in build["artifacts"]}
    checksums = {"Image": digest(kernel / "Image"),
                 "rootfs.ext4": digest(rootfs / "rootfs.ext4"),
                 "initramfs.img": digest(rootfs / "initramfs.img")}
    for name, value in checksums.items():
        wanted = expected.get(name) if name == "Image" else manifest["sha256"].get(name)
        if value != wanted:
            raise ValueError(f"Checksum mismatch: {name}")
    return checksums


def run(kernel, rootfs, output, qemu, accel, timeout):
    if output.exists():
        raise ValueError("Output exists; choose a new report directory")
    checksums = inputs(kernel, rootfs)
    output.mkdir(parents=True)
    command = [qemu, "-name", "Armada VR kernel ABI", "-machine", "virt", "-accel", accel,
               "-cpu", "host" if accel in ("hvf", "kvm") else "max", "-smp", "2", "-m", "2048",
               "-nodefaults", "-nographic", "-monitor", "none", "-serial", "stdio", "-no-reboot",
               "-nic", "none", "-kernel", str(kernel / "Image"), "-initrd", str(rootfs / "initramfs.img"),
               "-append", "root=/dev/vda rw console=ttyAMA0 earlycon selinux=0 audit=0 systemd.show_status=error",
               "-drive", f"file={rootfs / 'rootfs.ext4'},if=virtio,format=raw,snapshot=on"]
    report = {"schema_version": 1, "target": "qemu-kernel-abi", "hardware_boot_verified": False,
              "flash_image": False, "sha256": checksums, "command": command, "timeout_seconds": timeout,
              "kernel_build_sha256": digest(kernel / "build.json"), "status": "started"}
    started = time.monotonic()
    try:
        with (output / "boot.log").open("wb") as log:
            process = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT)
            try:
                report["returncode"] = process.wait(timeout=timeout)
            except subprocess.TimeoutExpired:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=5)
                raise TimeoutError("Vendor-kernel VM did not finish before the deadline")
        text = (output / "boot.log").read_text(errors="replace")
        report["acceptance_passed"] = report["returncode"] == 0 and "ARMADA_VR_VM_PASS" in text
        report["status"] = "passed" if report["acceptance_passed"] else "failed"
        if not report["acceptance_passed"]:
            raise RuntimeError(f"Kernel ABI test failed; see {output / 'boot.log'}")
    except (OSError, RuntimeError, TimeoutError) as error:
        report.update(status="failed", error=str(error))
        raise
    finally:
        report["elapsed_seconds"] = round(time.monotonic() - started, 2)
        (output / "result.json").write_text(json.dumps(report, indent=2) + "\n")
    print(f"Vendor-kernel QEMU userspace acceptance passed: {output}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("kernel", type=Path)
    parser.add_argument("--rootfs", type=Path, default=Path("output/vm"))
    parser.add_argument("--output", type=Path, default=Path("output/kernel/quest3/abi-test"))
    parser.add_argument("--qemu", default="qemu-system-aarch64")
    default_accel = "hvf" if platform.system() == "Darwin" and platform.machine() == "arm64" else "tcg"
    parser.add_argument("--accel", choices=("hvf", "kvm", "tcg"), default=default_accel)
    parser.add_argument("--timeout", type=int, default=240)
    args = parser.parse_args()
    if not 1 <= args.timeout <= 600:
        parser.error("Timeout must be 1–600 seconds")
    try:
        run(args.kernel.resolve(), args.rootfs.resolve(), args.output.resolve(), args.qemu, args.accel, args.timeout)
    except (OSError, ValueError, RuntimeError, TimeoutError) as error:
        parser.exit(1, f"{error}\n")
