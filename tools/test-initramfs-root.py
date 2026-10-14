#!/usr/bin/env python3
"""Verify the Linux root handoff in QEMU, including zero writes to a writable disk."""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import struct
import time
import tempfile


ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("initramfs_builder", ROOT / "tools/build-headset-initramfs.py")
builder = importlib.util.module_from_spec(spec)
spec.loader.exec_module(builder)
spec = importlib.util.spec_from_file_location("vm_control", ROOT / "tools/vm_control.py")
vm = importlib.util.module_from_spec(spec)
spec.loader.exec_module(vm)


def machine_options(build, accel):
    if build.get("repository") == "bytedance/neo3-kernel":
        if accel not in (None, "tcg"):
            raise ValueError("The Pico kernel fixture requires TCG with the virtual secure monitor")
        return ["-machine", "virt,secure=on,virtualization=on", "-accel", "tcg",
                "-cpu", "cortex-a76", "-smp", "1", "-m", "2048",
                "-object", "rng-random,id=rng,filename=/dev/urandom", "-device", "virtio-rng-device,rng=rng"]
    accel = accel or ("hvf" if platform.system() == "Darwin" else "tcg")
    return ["-machine", "virt", "-accel", accel,
            "-cpu", "host" if accel in ("hvf", "kvm") else "max", "-smp", "2", "-m", "1024"]


def disk_options(disk, transport, *, pico=False, readonly=False):
    drive = f"file={disk},if=none,id=root,format=raw" + (",readonly=on" if readonly else "")
    if transport == "usb-storage":
        return ["-device", "qemu-xhci,id=xhci", "-drive", drive,
                "-device", "usb-storage,bus=xhci.0,drive=root"]
    if transport != "virtio":
        raise ValueError("Unsupported root transport")
    return ["-drive", drive, "-device", ("virtio-blk-device" if pico else "virtio-blk-pci") + ",drive=root"]


def partition_fixture(source, target):
    size = source.stat().st_size
    if size % 512 or size > 64 * 1024**2:
        raise ValueError("Expected a sector-aligned root fixture at most 64 MiB")
    first = 2048
    mbr = bytearray(512)
    struct.pack_into("<B3sB3sII", mbr, 446, 0, b"\xfe\xff\xff", 0x83, b"\xfe\xff\xff", first, size // 512)
    mbr[510:] = b"\x55\xaa"
    with target.open("xb") as disk, source.open("rb") as root:
        disk.write(mbr)
        disk.seek(first * 512)
        shutil.copyfileobj(root, disk)
    return first


def unsupported_filesystem_fixture(disk):
    def crc32c(data):
        value = 0xffffffff
        for byte in data:
            value ^= byte
            for _ in range(8):
                value = (value >> 1) ^ (0x82f63b78 if value & 1 else 0)
        return value

    with disk.open("r+b") as stream:
        stream.seek(1024)
        superblock = bytearray(stream.read(1024))
        if (len(superblock) != 1024 or struct.unpack_from("<H", superblock, 56)[0] != 0xef53 or
                struct.unpack_from("<I", superblock, 1020)[0] != crc32c(superblock[:1020])):
            raise ValueError("Expected an ext4 fixture with a valid superblock checksum")
        features = struct.unpack_from("<I", superblock, 96)[0]
        struct.pack_into("<I", superblock, 96, features | 0x80000000)
        struct.pack_into("<I", superblock, 1020, crc32c(superblock[:1020]))
        stream.seek(1024)
        stream.write(superblock)


def service_failures(log):
    log = re.sub(r"\x1b\[[0-9;]*m", "", log)
    failures = re.findall(r"([\w@.:-]+\.service): Failed (?:at step|with result)", log)
    failures += re.findall(r"\[FAILED\]\s+([^\r\n]+)", log)
    return sorted(set(failures))


def missing_root_failure(log):
    log = re.sub(r"\x1b\[[0-9;]*m", "", log)
    return ("/dev/disk/by-label/absent-root does not exist" in log or
            re.search(r"Timed out waiting for device (?:[^\r\n]+ - )?"
                      r"/dev/disk/by-label/absent-root\.", log) is not None)


def run(args):
    kernel, initramfs, output = args.kernel.resolve(), args.initramfs.resolve(), args.output.resolve()
    build, hashes, _, _ = builder.inputs(kernel)
    initrd = json.loads((initramfs / "build.json").read_text())
    if build.get("qemu_transports") is not True or initrd.get("qemu_transports") is not True:
        raise ValueError("Requires the QEMU kernel and QEMU initramfs; headset artifacts are refused")
    machine = machine_options(build, args.accel)
    pico = build.get("repository") == "bytedance/neo3-kernel"
    if initrd.get("status") != "built" or initrd["kernel_build_sha256"] != builder.digest(kernel / "build.json"):
        raise ValueError("Initramfs does not match the kernel build")
    artifact = next(item for item in initrd["artifacts"] if item["path"] == "initramfs.img")
    if builder.digest(initramfs / "initramfs.img") != artifact["sha256"]:
        raise ValueError("Initramfs checksum mismatch")
    if output.exists() or any("," in str(p) or ":" in str(p) for p in (kernel, initramfs, output, ROOT)):
        raise ValueError("Choose a new output directory and paths without commas or colons")
    output.mkdir(parents=True)
    fixture = ROOT / "tests/initramfs-root/init.c"
    console = ROOT / "tests/initramfs-root/console.conf"
    report = {"status": "started", "scope": "QEMU initramfs root handoff", "hardware_boot_verified": False,
              "kernel_sha256": hashes["Image"], "initramfs_sha256": artifact["sha256"],
              "fixture_sha256": builder.digest(fixture), "runner_sha256": builder.digest(Path(__file__)),
              "console_fixture_sha256": builder.digest(console), "transport": args.transport,
              "vm_control_sha256": builder.digest(ROOT / "tools/vm_control.py"),
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
        (output / "root/etc/os-release").write_text("ID=reframed-vr-test\nNAME=Reframed\n")
        (output / "root/root-identity").write_text("reframed-root-fixture\n")
        command = [args.engine, "run", "--rm", "--init", "--network", "none", "--memory", "256m",
                   "--pids-limit", "64", "--entrypoint", "sh", "-v", f"{output}:/output", initrd["container_image"], "-c",
                   "set -eu; truncate -s 32M /output/root.ext4; "
                   "mkfs.ext4 -q -F -L reframed-vr-root -d /output/root /output/root.ext4; "
                   "for unit in dracut-initqueue dracut-emergency emergency rescue; do "
                   "directory=/output/console/etc/systemd/system/$unit.service.d; mkdir -p \"$directory\"; "
                   "cp /output/console.conf \"$directory/console.conf\"; done; "
                   "cd /output/console; find . -print0 | LC_ALL=C sort -z | "
                   "cpio --null -o -H newc --reproducible --owner=0:0 | gzip -n > /output/console.cpio.gz"]
        report["commands"].append(command)
        shutil.copyfile(console, output / "console.conf")
        subprocess.run(command, check=True, timeout=60)
        test_initrd = output / "test-initramfs.img"
        test_initrd.write_bytes(boot_initrd.read_bytes() + (output / "console.cpio.gz").read_bytes())
        boot_initrd = test_initrd
        report["test_initramfs_sha256"] = builder.digest(boot_initrd)
        report["root_sha256"] = builder.digest(output / "root.ext4")
        cases = [("overlay", "reframed-vr-root", 10), ("partitioned-overlay", "reframed-vr-root", 10),
                 ("missing-root", "absent-root", 30), ("missing-root-device", "absent-root", 2),
                 ("unsupported-filesystem", "reframed-vr-root", 10)]
        if args.transport == "usb-storage":
            cases.append(("delayed-overlay", "reframed-vr-root", 30))
        for name, label, device_timeout in cases:
            disk = output / (name + ".ext4")
            if name == "partitioned-overlay":
                partition_fixture(output / "root.ext4", disk)
            else:
                shutil.copyfile(output / "root.ext4", disk)
            if name == "unsupported-filesystem":
                unsupported_filesystem_fixture(disk)
            before = builder.digest(disk)
            devices = disk_options(disk, args.transport, pico=pico)
            if name == "missing-root-device":
                devices = devices[:2] if args.transport == "usb-storage" else []
            if name == "delayed-overlay":
                devices = devices[:-2]
            command = [args.qemu, *machine, "-nodefaults", "-nographic", "-monitor", "none", "-serial", "stdio",
                       "-no-reboot", "-nic", "none", "-kernel", str(kernel / "Image"), "-initrd", str(boot_initrd),
                       *devices, "-append",
                       f"console=ttyAMA0 earlycon selinux=0 audit=0 root=LABEL={label} ro rootfstype=ext4 rootflags=noload "
                       "systemd.volatile=overlay rd.fstab=0 rd.luks=0 rd.lvm=0 rd.md=0 rd.systemd.gpt_auto=0 "
                       f"systemd.gpt_auto=0 rd.shell=0 rd.emergency=poweroff rd.retry=5 rd.timeout={device_timeout} "
                       f"init=/sbin/init reframed.root_test=1 reframed.root_transport={args.transport}" + (" initcall_blacklist=hwinfo_init" if pico else "")]
            report["commands"].append(command)
            case = {"name": name, "status": "started", "root_sha256": before,
                    "disk_attached_at_start": name not in ("missing-root-device", "delayed-overlay")}
            report["cases"].append(case)
            started = time.monotonic()
            with tempfile.TemporaryDirectory(prefix="root-qmp-", dir="/tmp") as sockets:
                command.extend(["-qmp", f"unix:{sockets}/qmp.sock,server=on,wait=off"])
                log_path = output / (name + ".log")
                with log_path.open("wb") as log:
                    process = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT)
                    try:
                        if name == "delayed-overlay":
                            deadline = time.monotonic() + 30
                            while time.monotonic() < deadline:
                                if process.poll() is not None:
                                    raise RuntimeError("Guest exited before delayed USB attachment")
                                if "Expecting device /dev/disk/by-label/reframed-vr-root" in log_path.read_text(errors="replace"):
                                    break
                                time.sleep(0.1)
                            else:
                                raise TimeoutError("Guest never waited for the USB root")
                            time.sleep(2)
                            case["usb_attach_after_seconds"] = round(time.monotonic() - started, 3)
                            case["usb_attach_result"] = vm.request(sockets, "device_add", {
                                "driver": "usb-storage", "bus": "xhci.0", "drive": "root", "id": "rootusb"})
                        case["returncode"] = process.wait(timeout=90)
                    finally:
                        if process.poll() is None:
                            process.terminate()
                            try:
                                process.wait(timeout=5)
                            except subprocess.TimeoutExpired:
                                process.kill()
                                process.wait(timeout=5)
            case["elapsed_seconds"] = round(time.monotonic() - started, 3)
            case["backing_image_unchanged"] = builder.digest(disk) == before
            log = (output / (name + ".log")).read_text(errors="replace")
            case["service_failures"] = service_failures(log)
            expected_failures = ["Failed to mount /sysroot."] if name == "unsupported-filesystem" else []
            if case["service_failures"] != expected_failures:
                raise RuntimeError(f"Root handoff {name} has failed services: {', '.join(case['service_failures'])}")
            passed = "REFRAMED_ROOT_OVERLAY_PASS" in log
            expected = passed if name in ("overlay", "partitioned-overlay", "delayed-overlay") else (
                not passed and missing_root_failure(log) and "Powering off." in log)
            if name == "unsupported-filesystem":
                expected = (not passed and "Couldn't mount because of unsupported optional features (80000000)" in log
                            and "Powering off." in log)
            if (case["returncode"] or not expected or "REFRAMED_ROOT_FAIL" in log or
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
    parser.add_argument("--compiler-image", default="localhost/reframed-vr:kernel")
    parser.add_argument("--vendor-ramdisk", type=Path)
    parser.add_argument("--qemu", default="qemu-system-aarch64")
    parser.add_argument("--transport", choices=("virtio", "usb-storage"), default="virtio")
    parser.add_argument("--accel", choices=("hvf", "kvm", "tcg"), help="Defaults to TCG for Pico, otherwise the host default")
    try:
        run(parser.parse_args())
    except (OSError, ValueError, RuntimeError, TimeoutError, subprocess.SubprocessError) as error:
        parser.exit(1, f"{error}\n")
