#!/usr/bin/env python3
"""Assemble unsigned Quest reference-format Linux boot containers, offline only."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import struct
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]


def load(name, filename):
    spec = importlib.util.spec_from_file_location(name, ROOT / "tools" / filename)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


verify = load("boot_set", "verify-boot-set.py")
boot = load("boot_image", "inspect-boot.py")
builder = load("initramfs_builder", "build-headset-initramfs.py")


def checked_tools(directory):
    profile = json.loads((ROOT / "profiles/mkbootimg.json").read_text())
    expected = {item["path"]: item for item in profile["files"]}
    paths = list(directory.rglob("*"))
    if directory.is_symlink() or any(path.is_symlink() for path in paths):
        raise ValueError("AOSP tool tree must not contain symlinks")
    actual = {str(path.relative_to(directory)) for path in paths if path.is_file()}
    if actual - {"source.json"} != set(expected):
        raise ValueError("AOSP tool tree has missing or unexpected files")
    for name, item in expected.items():
        path = directory / name
        if path.stat().st_size != item["bytes"] or builder.digest(path) != item["sha256"]:
            raise ValueError(f"Pinned AOSP tool mismatch: {name}")
    return profile


def parse_arguments(data):
    if not data.endswith(b"\0"):
        raise ValueError("Expected NUL-terminated AOSP arguments")
    args = data[:-1].decode().split("\0")
    if len(args) % 2 or len(set(args[::2])) != len(args[::2]):
        raise ValueError("Expected unique AOSP option/value pairs")
    if not all(option.startswith("--") for option in args[::2]):
        raise ValueError("Invalid AOSP option vector")
    return args


def replace(args, name, value):
    if args.count(name) != 1 or args.index(name) % 2:
        raise ValueError(f"Missing or ambiguous AOSP option: {name}")
    args[args.index(name) + 1] = str(value)


def empty_ramdisk():
    fields = (0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 11, 0)
    data = b"070701" + "".join(f"{value:08x}" for value in fields).encode() + b"TRAILER!!!\0"
    return data + bytes(-len(data) % 512)


def overlay_table(data, compiled):
    if len(data) < 32:
        raise ValueError("Truncated Android DTBO table")
    magic, total, header, entry_size, count, offset, page, version = struct.unpack_from(">8I", data)
    if ((magic, total, header, entry_size, offset, page, version) !=
            (0xd7b7ab1e, len(data), 32, 32, 32, 4096, 0) or
            not 1 <= count <= 128 or offset + count * entry_size > len(data)):
        raise ValueError("Unsupported Android DTBO table geometry")
    records, ranges = [], []
    for index in range(count):
        size, start, *identifiers = struct.unpack_from(">8I", data, offset + entry_size * index)
        if size < 40 or start < offset + count * entry_size or start + size > len(data):
            raise ValueError("DTBO payload lies outside the image")
        payload = data[start:start + size]
        if struct.unpack_from(">II", payload) != (0xd00dfeed, size):
            raise ValueError("Invalid DTBO FDT header")
        digest = hashlib.sha256(payload).hexdigest()
        if digest not in compiled:
            raise ValueError(f"Reference overlay {index} does not match a compiled overlay")
        records.append({"index": index, "sha256": digest, "compiled_path": compiled[digest],
                        "identifiers": identifiers})
        ranges.append((start, start + size))
    ranges.sort()
    if any(left[1] > right[0] for left, right in zip(ranges, ranges[1:])):
        raise ValueError("Overlapping DTBO payloads")
    if len(records) != len(compiled) or len({item["compiled_path"] for item in records}) != len(compiled):
        raise ValueError("Reference table does not cover each compiled overlay exactly once")
    return records


def root_arguments(label, *, quest_services=False):
    if not re.fullmatch(r"[A-Za-z0-9_-]{1,16}", label):
        raise ValueError("Root label must be 1–16 ASCII letters, digits, hyphens or underscores")
    return (f"root=LABEL={label} ro rootfstype=ext4 rootflags=noload systemd.volatile=overlay "
            "rd.fstab=0 rd.luks=0 rd.lvm=0 rd.md=0 rd.systemd.gpt_auto=0 systemd.gpt_auto=0 "
            "rd.shell=0 rd.emergency=poweroff rd.retry=15 rd.timeout=30"
            + (" armada.quest=usb-root" if quest_services else ""))


def run(args):
    reference, kernel, initramfs = args.reference.resolve(), args.kernel.resolve(), args.initramfs.resolve()
    tool, output = args.mkbootimg.absolute(), args.output.absolute()
    if output.exists() or output.is_symlink():
        raise ValueError("Output exists; choose a new directory")
    root_arguments(args.root_label)
    profile = checked_tools(tool)
    integrity = verify.verify_set(reference, args.reference_build, verify.load_avb(args.avbtool))
    kernel_report, kernel_hashes, _, _ = builder.inputs(kernel)
    if kernel_report["qemu_transports"] is not False:
        raise ValueError("Quest container assembly refuses QEMU transport kernels")
    initrd_report = json.loads((initramfs / "build.json").read_text())
    if (initrd_report.get("status") != "built" or initrd_report.get("qemu_transports") is not False or
            initrd_report.get("kernel_build_sha256") != builder.digest(kernel / "build.json")):
        raise ValueError("Initramfs does not match the selected headset kernel build")
    firmware_manifest = initrd_report.get("firmware")
    if firmware_manifest and firmware_manifest.get("reference_build") != args.reference_build:
        raise ValueError("Initramfs firmware does not match the selected reference build")
    quest_services = initrd_report.get("quest_services", False)
    if type(quest_services) is not bool or (quest_services and not firmware_manifest):
        raise ValueError("Quest startup requires a boolean service flag and verified firmware")
    cmdline = root_arguments(args.root_label, quest_services=quest_services)
    initrd_artifacts = {item["path"]: item for item in initrd_report["artifacts"]}
    if builder.digest(initramfs / "initramfs.img") != initrd_artifacts["initramfs.img"]["sha256"]:
        raise ValueError("Initramfs checksum mismatch")
    expected = {item["path"]: item["sha256"] for item in kernel_report["artifacts"]}
    dtb_name = "device-trees/oculus/eureka/eureka.dtb"
    dtb = kernel / dtb_name
    if builder.digest(dtb) != expected[dtb_name]:
        raise ValueError("Base DTB checksum mismatch")
    compiled = {}
    for name, digest in expected.items():
        if name.startswith("device-trees/oculus/eureka/") and name.endswith(".dtbo"):
            if builder.digest(kernel / name) != digest or digest in compiled:
                raise ValueError("Changed or ambiguous compiled overlay")
            compiled[digest] = name
    with (reference / "dtbo.img").open("rb") as stream:
        dtbo = stream.read(integrity["images"]["dtbo"]["original_image_size"])
    entries = overlay_table(dtbo, compiled)
    for name in ("boot", "vendor_boot"):
        info = boot.inspect(reference / (name + ".img"))
        if info["format"] != name or info["header_version"] != 4:
            raise ValueError("Requires Android v4 boot and vendor_boot reference images")
        if name == "vendor_boot":
            fragments = info["vendor_ramdisk_fragments"]
            if len(fragments) != 1 or fragments[0]["type"] != 1 or any(fragments[0]["board_id"]):
                raise ValueError("Requires one platform vendor ramdisk with no board filter")
    output.mkdir(parents=True)
    (output / "logs").mkdir()
    report = {"status": "started", "scope": "unsigned offline Quest reference-format assembly",
              "hardware_flash_image": False, "hardware_boot_verified": False,
              "device_compatibility_verified": False, "signatures_created": False,
              "reference_build": args.reference_build, "reference_integrity": integrity,
              "kernel_build_sha256": builder.digest(kernel / "build.json"),
              "initramfs_build_sha256": builder.digest(initramfs / "build.json"),
              "firmware": firmware_manifest, "quest_services": quest_services, "cmdline": cmdline,
              "assembler_sha256": builder.digest(Path(__file__)), "aosp_profile": profile,
              "dtbo_entries": entries, "commands": [], "reference_roundtrip": {}, "artifacts": {}}

    def call(arguments):
        command = [sys.executable, "-B", *map(str, arguments)]
        number = len(report["commands"])
        record = {"command": command}
        report["commands"].append(record)
        try:
            result = subprocess.run(command, capture_output=True, timeout=60)
        except subprocess.TimeoutExpired as error:
            record["timed_out"] = True
            (output / "logs" / f"{number:02d}.stdout").write_bytes(error.stdout or b"")
            (output / "logs" / f"{number:02d}.stderr").write_bytes(error.stderr or b"")
            raise
        record["returncode"] = result.returncode
        for suffix, data in (("stdout", result.stdout), ("stderr", result.stderr)):
            (output / "logs" / f"{number:02d}.{suffix}").write_bytes(data)
        if result.returncode:
            raise RuntimeError(f"AOSP command failed; inspect logs/{number:02d}.stderr")
        return result.stdout

    try:
        vectors = {}
        for name, flag in (("boot", "--output"), ("vendor_boot", "--vendor_boot")):
            vector = parse_arguments(call([tool / "unpack_bootimg.py", "--boot_img", reference / (name + ".img"),
                                           "--out", output / "reference-parts" / name, "--format", "mkbootimg", "-0"]))
            vectors[name] = vector
            path = output / (name + "-reference-unsigned.img")
            call([tool / "mkbootimg.py", *vector, flag, path])
            size = integrity["images"][name]["original_image_size"]
            with (reference / (name + ".img")).open("rb") as stream:
                if path.read_bytes() != stream.read(size):
                    raise ValueError(f"Reference {name} payload did not roundtrip byte-for-byte")
            report["reference_roundtrip"][name] = {"bytes": size, "sha256": builder.digest(path)}
        (output / "vendor-ramdisk.cpio").write_bytes(empty_ramdisk())
        vector = vectors["boot"].copy()
        for name in ("--os_version", "--os_patch_level"):
            index = vector.index(name)
            del vector[index:index + 2]
        replace(vector, "--kernel", kernel / "Image")
        replace(vector, "--ramdisk", initramfs / "initramfs.img")
        replace(vector, "--cmdline", cmdline)
        call([tool / "mkbootimg.py", *vector, "--output", output / "boot-linux-unsigned.img"])
        vector = vectors["vendor_boot"].copy()
        replace(vector, "--vendor_ramdisk_fragment", output / "vendor-ramdisk.cpio")
        replace(vector, "--dtb", dtb)
        call([tool / "mkbootimg.py", *vector, "--vendor_boot", output / "vendor_boot-linux-unsigned.img"])
        (output / "dtbo-unsigned.img").write_bytes(dtbo)
        for name in ("boot", "vendor_boot"):
            path = output / (name + "-linux-unsigned.img")
            info = boot.inspect(path)
            if (info["avb_footer_magic_present"] or info["trailing_bytes"] or
                    any(item["name"] == "boot_signature" for item in info["sections"]) or
                    info["size"] + 69632 >= integrity["images"][name]["bytes"]):
                raise ValueError(f"Unexpected signature, trailing bytes or insufficient partition headroom: {name}")
            if name == "boot" and info["cmdline"] != cmdline:
                raise ValueError("Linux command line roundtrip mismatch")
            report["artifacts"][path.name] = info
            call([tool / "unpack_bootimg.py", "--boot_img", path, "--out", output / "linux-parts" / name])
        report["artifacts"]["dtbo-unsigned.img"] = {"bytes": len(dtbo), "sha256": builder.digest(output / "dtbo-unsigned.img")}
        for extracted, wanted in (("boot/kernel", kernel_hashes["Image"]),
                                  ("boot/ramdisk", initrd_artifacts["initramfs.img"]["sha256"]),
                                  ("vendor_boot/dtb", expected[dtb_name]),
                                  ("vendor_boot/vendor_ramdisk00", hashlib.sha256(empty_ramdisk()).hexdigest())):
            if builder.digest(output / "linux-parts" / extracted) != wanted:
                raise ValueError(f"Linux payload roundtrip mismatch: {extracted}")
        for name, item in integrity["images"].items():
            if builder.digest(reference / (name + ".img")) != item["sha256"]:
                raise ValueError("Reference inputs changed during assembly")
        if (builder.digest(kernel / "build.json") != report["kernel_build_sha256"] or
                builder.digest(initramfs / "build.json") != report["initramfs_build_sha256"]):
            raise ValueError("Build inputs changed during assembly")
        report.update(status="assembled-and-roundtrip-verified", linux_payload_roundtrip_verified=True,
                      reference_inputs_unchanged=True)
    except (OSError, ValueError, RuntimeError, KeyError, subprocess.SubprocessError) as error:
        report.update(status="failed", error=str(error))
        raise
    finally:
        (output / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(output / "report.json")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("reference", type=Path)
    parser.add_argument("kernel", type=Path)
    parser.add_argument("initramfs", type=Path)
    parser.add_argument("--reference-build", required=True)
    parser.add_argument("--avbtool", type=Path, required=True)
    parser.add_argument("--mkbootimg", type=Path, required=True, help="directory containing pinned AOSP tools")
    parser.add_argument("--root-label", default="armada-vr-root")
    parser.add_argument("--output", type=Path, required=True)
    try:
        run(parser.parse_args())
    except (OSError, ValueError, RuntimeError, KeyError, subprocess.SubprocessError) as error:
        parser.exit(1, f"{error}\n")
