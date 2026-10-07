#!/usr/bin/env python3
"""Boot a QEMU-only test or persistent desktop VM, with optional desktop networking."""
import argparse
import hashlib
import json
import platform
import subprocess
import time
from pathlib import Path
from vm_control import request


def abi_kernel(directory):
    directory = directory.resolve()
    build = json.loads((directory / "build.json").read_text())
    image = directory / "Image"
    if build.get("status") != "compiled" or build.get("qemu_transports") is not True:
        raise ValueError("Kernel override requires a compiled QEMU ABI variant")
    if image.is_symlink() or not image.is_file():
        raise ValueError("Expected a regular QEMU ABI kernel Image")
    expected = {entry["path"]: entry["sha256"] for entry in build["artifacts"]}.get("Image")
    with image.open("rb") as stream:
        if hashlib.file_digest(stream, "sha256").hexdigest() != expected:
            raise ValueError("QEMU ABI kernel checksum mismatch")
    return image


def command(directory, qemu, accel, desktop=False, network=False, kernel=None, disable_sme=None, base_kernel=False):
    directory = directory.resolve()
    manifest = json.loads((directory / "manifest.json").read_text())
    if manifest.get("target") != "qemu-arm64" or manifest.get("hardware_flash_image") is not False:
        raise ValueError("Expected a QEMU-only image manifest")
    for name in ("Image", "initramfs.img", "rootfs.ext4"):
        path = directory / name
        if path.is_symlink() or not path.is_file() or "," in str(path):
            raise ValueError(f"Expected a regular image file without commas: {path}")
    cpu = "host" if accel in ("hvf", "kvm") else "max"
    if network and not desktop:
        raise ValueError("Networking is only available for the interactive desktop")
    if desktop and manifest.get("desktop_available") is not True:
        raise ValueError("This image does not contain the interactive desktop")
    if desktop and kernel is None and not base_kernel and manifest.get("desktop_kernel_abi"):
        kernel = Path(manifest["desktop_kernel_abi"])
        if not kernel.is_absolute():
            raise ValueError("Saved desktop kernel directory must be absolute")
    display = ["-display", "cocoa,zoom-to-fit=on,left-command-key=off" if platform.system() == "Darwin" else "gtk",
               "-device", "virtio-gpu-pci,xres=1600,yres=900",
               "-device", "virtio-keyboard-pci", "-device", "virtio-tablet-pci",
               "-device", "virtio-serial-pci",
               "-chardev", f"socket,path={directory / 'agent.sock'},server=on,wait=off,id=agent",
               "-device", "virtserialport,chardev=agent,name=org.qemu.guest_agent.0",
               "-qmp", f"unix:{directory / 'qmp.sock'},server=on,wait=off"] if desktop else ["-nographic"]
    boot = "root=LABEL=armada-vr-vm rw console=ttyAMA0 selinux=0 audit=0 systemd.show_status=error"
    if disable_sme is True or (disable_sme is None and accel == "hvf"):
        boot += " arm64.nosme"
    drive = f"file={directory / 'rootfs.ext4'},if=virtio,format=raw,snapshot=on"
    if desktop:
        boot += " armada.desktop=1"
        drive = f"file={directory / 'desktop.qcow2'},if=virtio,format=qcow2"
    return [qemu, "-name", "Armada VR", "-machine", "virt", "-accel", accel, "-cpu", cpu,
            "-smp", "2", "-m", "6144" if desktop else "2048", "-nodefaults", *display, "-monitor", "none",
            "-serial", "stdio", "-no-reboot", "-nic", "user,model=virtio-net-pci" if network else "none",
            "-kernel", str(abi_kernel(kernel) if kernel else directory / "Image"),
            "-initrd", str(directory / "initramfs.img"),
            "-append", boot, "-drive", drive]


def prepare_desktop(directory):
    directory = directory.resolve()
    monitor = directory / "qmp.sock"
    if monitor.exists():
        try:
            request(directory, "query-status")
        except (ConnectionRefusedError, FileNotFoundError):
            monitor.unlink(missing_ok=True)
        else:
            raise ValueError("The desktop VM is already running")
    overlay = directory / "desktop.qcow2"
    if overlay.is_symlink():
        raise ValueError("Expected a regular desktop overlay")
    if not overlay.exists():
        subprocess.run(["qemu-img", "create", "-f", "qcow2", "-F", "raw", "-b",
                        str(directory / "rootfs.ext4"), str(overlay)], check=True)
    elif not overlay.is_file():
        raise ValueError("Expected a regular desktop overlay")
    info = json.loads(subprocess.check_output(["qemu-img", "info", "--output=json", str(overlay)], text=True))
    if info.get("format") != "qcow2" or info.get("full-backing-filename") != str(directory / "rootfs.ext4"):
        raise ValueError("Desktop overlay has an unexpected backing image")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", nargs="?", type=Path, default=Path("output/vm"))
    parser.add_argument("--qemu", default="qemu-system-aarch64")
    default = "hvf" if platform.system() == "Darwin" and platform.machine() == "arm64" else "tcg"
    parser.add_argument("--accel", choices=("hvf", "kvm", "tcg"), default=default)
    parser.add_argument("--timeout", type=int, default=180)
    parser.add_argument("--desktop", action="store_true", help="Open a persistent desktop VM and return its PID")
    parser.add_argument("--network", action="store_true", help="Enable user-mode networking for the desktop, without forwarded ports")
    kernels = parser.add_mutually_exclusive_group()
    kernels.add_argument("--kernel-abi", type=Path, help="Use a verified QEMU ABI kernel; remember it for desktop launches")
    kernels.add_argument("--base-kernel", action="store_true", help="Use the image's original kernel and clear its saved desktop selection")
    sme = parser.add_mutually_exclusive_group()
    sme.add_argument("--disable-sme", dest="disable_sme", action="store_true", default=None,
                     help="Disable guest kernel SME support (the HVF default)")
    sme.add_argument("--enable-sme", dest="disable_sme", action="store_false",
                     help="Retain guest SME support for controlled regression testing")
    args = parser.parse_args()
    try:
        cmd = command(args.directory, args.qemu, args.accel, args.desktop, args.network,
                      args.kernel_abi, args.disable_sme, args.base_kernel)
        manifest = json.loads((args.directory / "manifest.json").read_text())
        for name in ("Image", "initramfs.img", "rootfs.ext4"):
            with (args.directory / name).open("rb") as stream:
                digest = hashlib.file_digest(stream, "sha256").hexdigest()
            if manifest.get("sha256", {}).get(name) != digest:
                raise ValueError(f"Image checksum mismatch: {name}")
        if args.desktop:
            prepare_desktop(args.directory)
            log = args.directory / "desktop-boot.log"
            with log.open("w") as output:
                process = subprocess.Popen(cmd, stdin=subprocess.DEVNULL, stdout=output,
                                           stderr=subprocess.STDOUT, start_new_session=True)
            time.sleep(1)
            if process.poll() is not None:
                raise ValueError(f"QEMU exited during startup; see {log}")
            if args.kernel_abi or args.base_kernel:
                if args.kernel_abi:
                    manifest["desktop_kernel_abi"] = str(args.kernel_abi.resolve())
                else:
                    manifest.pop("desktop_kernel_abi", None)
                temporary = args.directory / "manifest.json.tmp"
                temporary.write_text(json.dumps(manifest, indent=2) + "\n")
                temporary.replace(args.directory / "manifest.json")
            print(f"Desktop VM started, PID {process.pid}; log: {log}")
            return
        with (args.directory / "boot.log").open("w") as log:
            result = subprocess.run(cmd, timeout=args.timeout, check=False,
                                    stdout=log, stderr=subprocess.STDOUT)
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        parser.exit(1, f"{error}\n")
    output = (args.directory / "boot.log").read_text(errors="replace")
    for line in output.splitlines():
        if "ARMADA_VR_" in line or "samples=" in line:
            print(line)
    if result.returncode or "ARMADA_VR_VM_PASS" not in output:
        parser.exit(1, f"VM acceptance failed; see {args.directory / 'boot.log'}\n")


if __name__ == "__main__":
    main()
