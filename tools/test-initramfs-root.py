#!/usr/bin/env python3
"""Verify the Linux root handoff in QEMU, including zero writes to a writable disk."""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import time


ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("initramfs_builder", ROOT / "tools/build-headset-initramfs.py")
builder = importlib.util.module_from_spec(spec)
spec.loader.exec_module(builder)


def run(args):
    kernel, initramfs, output = args.kernel.resolve(), args.initramfs.resolve(), args.output.resolve()
    build, hashes, _, _ = builder.inputs(kernel)
    initrd = json.loads((initramfs / "build.json").read_text())
    if build.get("qemu_transports") is not True or initrd.get("qemu_transports") is not True:
        raise ValueError("Requires the QEMU kernel and QEMU initramfs; headset artifacts are refused")
    if initrd.get("status") != "built" or initrd["kernel_build_sha256"] != builder.digest(kernel / "build.json"):
        raise ValueError("Initramfs does not match the kernel build")
    artifact = next(item for item in initrd["artifacts"] if item["path"] == "initramfs.img")
    if builder.digest(initramfs / "initramfs.img") != artifact["sha256"]:
        raise ValueError("Initramfs checksum mismatch")
    if output.exists() or any("," in str(p) or ":" in str(p) for p in (kernel, initramfs, output, ROOT)):
        raise ValueError("Choose a new output directory and paths without commas or colons")
    output.mkdir(parents=True)
    fixture = ROOT / "tests/initramfs-root/init.c"
    report = {"status": "started", "scope": "QEMU initramfs root handoff", "hardware_boot_verified": False,
              "kernel_sha256": hashes["Image"], "initramfs_sha256": artifact["sha256"],
              "fixture_sha256": builder.digest(fixture), "runner_sha256": builder.digest(Path(__file__)),
              "commands": [], "cases": []}
    try:
        boot_initrd = initramfs / "initramfs.img"
        if args.vendor_ramdisk:
            report["vendor_ramdisk_sha256"] = builder.digest(args.vendor_ramdisk)
            if args.vendor_ramdisk.stat().st_size > 1024**2:
                raise ValueError("Vendor ramdisk test prefix exceeds 1 MiB")
            boot_initrd = output / "combined-initramfs.img"
            boot_initrd.write_bytes(args.vendor_ramdisk.read_bytes() + (initramfs / "initramfs.img").read_bytes())
            report["combined_initramfs_sha256"] = builder.digest(boot_initrd)
        compiler_image = subprocess.check_output([args.engine, "image", "inspect", args.compiler_image,
                                                  "--format", "{{.Id}}"], text=True).strip()
        report["compiler_image"] = compiler_image
        command = [args.engine, "run", "--rm", "--init", "--network", "none", "--memory", "256m",
                   "--pids-limit", "64", "--entrypoint", "sh", "-v", f"{fixture}:/init.c:ro",
                   "-v", f"{output}:/output", compiler_image, "-c",
                   "mkdir -p /output/root/sbin /output/root/etc /output/root/dev /output/root/proc /output/root/sys /output/root/run /output/root/tmp; "
                   "gcc -static -O2 -Wall -Wextra -Werror /init.c -o /output/root/sbin/init"]
        report["commands"].append(command)
        subprocess.run(command, check=True, timeout=60)
        (output / "root/etc/os-release").write_text("ID=armada-vr-test\nNAME=Armada\n")
        (output / "root/root-identity").write_text("armada-root-fixture\n")
        command = [args.engine, "run", "--rm", "--init", "--network", "none", "--memory", "256m",
                   "--pids-limit", "64", "--entrypoint", "sh", "-v", f"{output}:/output", initrd["container_image"], "-c",
                   "truncate -s 32M /output/root.ext4 && mkfs.ext4 -q -F -L armada-vr-root -d /output/root /output/root.ext4"]
        report["commands"].append(command)
        subprocess.run(command, check=True, timeout=60)
        report["root_sha256"] = builder.digest(output / "root.ext4")
        for name, label in (("overlay", "armada-vr-root"), ("missing-root", "absent-root")):
            disk = output / (name + ".ext4")
            shutil.copyfile(output / "root.ext4", disk)
            command = [args.qemu, "-machine", "virt", "-accel", args.accel, "-cpu", "host" if args.accel in ("hvf", "kvm") else "max",
                       "-smp", "2", "-m", "1024", "-nodefaults", "-nographic", "-monitor", "none", "-serial", "stdio",
                       "-no-reboot", "-nic", "none", "-kernel", str(kernel / "Image"), "-initrd", str(boot_initrd),
                       "-drive", f"file={disk},if=virtio,format=raw", "-append",
                       f"console=ttyAMA0 earlycon selinux=0 audit=0 root=LABEL={label} ro rootfstype=ext4 rootflags=noload "
                       "systemd.volatile=overlay rd.fstab=0 rd.luks=0 rd.lvm=0 rd.md=0 rd.systemd.gpt_auto=0 "
                       "systemd.gpt_auto=0 rd.shell=0 rd.emergency=poweroff rd.retry=5 rd.timeout=10 "
                       "init=/sbin/init armada.root_test=1"]
            report["commands"].append(command)
            case = {"name": name, "status": "started"}
            report["cases"].append(case)
            started = time.monotonic()
            with (output / (name + ".log")).open("wb") as log:
                process = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT)
                try:
                    case["returncode"] = process.wait(timeout=90)
                except subprocess.TimeoutExpired:
                    process.terminate()
                    try:
                        process.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait(timeout=5)
                    raise TimeoutError(f"Root handoff {name} did not finish; inspect its log")
            case["elapsed_seconds"] = round(time.monotonic() - started, 3)
            case["backing_image_unchanged"] = builder.digest(disk) == report["root_sha256"]
            log = (output / (name + ".log")).read_text(errors="replace")
            passed = "ARMADA_ROOT_OVERLAY_PASS" in log
            expected = passed if name == "overlay" else (
                not passed and "/dev/disk/by-label/absent-root does not exist" in log and "Powering off." in log)
            if (case["returncode"] or not expected or "ARMADA_ROOT_FAIL" in log or
                    "reboot: Power down" not in log or not case["backing_image_unchanged"]):
                raise RuntimeError(f"Root handoff {name} failed; inspect its log")
            case["status"] = "passed"
        report["status"] = "passed"
    except (OSError, ValueError, RuntimeError, TimeoutError, subprocess.SubprocessError) as error:
        report.update(status="failed", error=str(error))
        raise
    finally:
        (output / "result.json").write_text(json.dumps(report, indent=2) + "\n")
    print(output / "result.json")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("kernel", type=Path)
    parser.add_argument("initramfs", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--engine", default=os.environ.get("CONTAINER_ENGINE", "docker"))
    parser.add_argument("--compiler-image", default="localhost/armada-vr:kernel")
    parser.add_argument("--vendor-ramdisk", type=Path)
    parser.add_argument("--qemu", default="qemu-system-aarch64")
    parser.add_argument("--accel", choices=("hvf", "kvm", "tcg"), default="hvf" if platform.system() == "Darwin" else "tcg")
    try:
        run(parser.parse_args())
    except (OSError, ValueError, RuntimeError, TimeoutError, subprocess.SubprocessError) as error:
        parser.exit(1, f"{error}\n")
