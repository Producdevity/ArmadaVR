#!/usr/bin/env python3
"""Prepare private Adreno 650 firmware from a verified Neo3 vendor image."""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import re
import struct
import subprocess


spec = importlib.util.spec_from_file_location("qcom_firmware", Path(__file__).with_name("prepare-quest-firmware.py"))
qcom = importlib.util.module_from_spec(spec)
spec.loader.exec_module(qcom)
digest = qcom.digest
GMU_RANGES = ((0, 0x4000), (0x4000, 0x1000000), (0x10004000, 0x10008000),
              (0x60000000, 0x80000000), (0x80000000, 0xc0000000))


def inspect_files(directory):
    mdt = directory / "a650_zap.mdt"
    digest(mdt)
    if mdt.stat().st_size > 1024**2:
        raise ValueError("MDT exceeds size limit")
    required = qcom.required_segments(mdt.read_bytes(), "a650_zap")
    names = {"a650_zap.mdt", *required, "a650_sqe.fw", "a650_gmu.bin"}
    if {p.name for p in directory.iterdir()} != names:
        raise ValueError("Pico GPU directory has missing or unexpected files")
    records = []
    for name in sorted(names):
        path = directory / name
        sha = digest(path)
        size = path.stat().st_size
        if not 0 < size <= 32 * 1024**2 or name in required and size != required[name]:
            raise ValueError("Invalid Pico GPU file size: " + name)
        records.append({"path": name, "bytes": size, "sha256": sha})
    sqe = (directory / "a650_sqe.fw").read_bytes()
    if len(sqe) < 16 or len(sqe) % 4:
        raise ValueError("SQE firmware must contain aligned header and instruction words")
    blocks = qcom.gmu_blocks((directory / "a650_gmu.bin").read_bytes(), GMU_RANGES)
    return records, {"gpu": "adreno-650", "sqe_version": struct.unpack_from("<I", sqe, 4)[0],
                     "sqe_words": (len(sqe) - 4) // 4, "gmu_blocks": blocks}


def validate(directory):
    if directory.is_symlink() or (directory / "files").is_symlink():
        raise ValueError("Firmware directories must not be symlinks")
    digest(directory / "manifest.json")
    manifest = json.loads((directory / "manifest.json").read_text())
    if (manifest.get("schema_version") != 1 or manifest.get("component") != "pico-neo3-gpu" or
            manifest.get("status") != "prepared" or
            not re.fullmatch(r"smartcm\.[0-9]{10}", manifest.get("reference_build", "")) or
            not re.fullmatch(r"[0-9a-f]{64}", manifest.get("source_image_sha256", ""))):
        raise ValueError("Invalid Pico GPU firmware manifest")
    records, details = inspect_files(directory / "files")
    if manifest.get("files") != records or manifest.get("gpu") != details:
        raise ValueError("Pico GPU manifest does not match its files")
    return manifest


def extract(source, output, expected, build):
    if digest(source) != expected:
        raise ValueError("Vendor image checksum mismatch")
    files = output / "files"
    files.mkdir()

    def dump(name):
        path = files / name
        result = subprocess.run(["debugfs", "-R", f"dump /firmware/{name} {path}", str(source)],
                                capture_output=True, timeout=15)
        with (output / "debugfs.log").open("ab") as log:
            log.write(name.encode() + b"\n" + result.stdout + result.stderr)
        if result.returncode or not path.is_file() or path.is_symlink():
            raise ValueError("Unable to extract firmware: " + name)

    dump("a650_zap.mdt")
    if (files / "a650_zap.mdt").stat().st_size > 1024**2:
        raise ValueError("MDT exceeds size limit")
    for name in sorted([*qcom.required_segments((files / "a650_zap.mdt").read_bytes(), "a650_zap"),
                        "a650_sqe.fw", "a650_gmu.bin"]):
        dump(name)
    records, details = inspect_files(files)
    if digest(source) != expected:
        raise ValueError("Vendor image changed during extraction")
    manifest = {"schema_version": 1, "component": "pico-neo3-gpu", "status": "prepared",
                "reference_build": build, "source_image_sha256": expected,
                "manufacturer_signature_verified": False, "device_compatibility_verified": False,
                "firmware_executed": False, "files": records, "gpu": details}
    (output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    validate(output)


def run(args):
    if (not re.fullmatch(r"[0-9a-f]{64}", args.sha256) or
            not re.fullmatch(r"smartcm\.[0-9]{10}", args.reference_build)):
        raise ValueError("Requires a vendor SHA-256 and Neo3 reference build")
    source, output = args.vendor_image.absolute(), args.output.absolute()
    if args.inside_container:
        extract(source, output, args.sha256, args.reference_build)
        return
    if any(c in str(p) for p in (source, output, Path(__file__).parent) for c in (",", ":")):
        raise ValueError("Container paths must not contain commas or colons")
    if output.exists() or output.is_symlink():
        raise ValueError("Output exists; choose a new directory")
    if digest(source) != args.sha256:
        raise ValueError("Vendor image checksum mismatch")
    image = subprocess.check_output([args.engine, "image", "inspect", args.image, "--format", "{{.Id}}"],
                                    text=True, timeout=30).strip()
    output.mkdir(parents=True)
    helpers = {p.name: digest(p) for p in (Path(__file__), Path(qcom.__file__))}
    command = [args.engine, "run", "--rm", "--init", "--network", "none", "--read-only",
               "--memory", "256m", "--memory-swap", "256m", "--cpus", "1", "--pids-limit", "64",
               "--tmpfs", "/tmp:rw,noexec,nosuid,size=16m", "-v", f"{source}:/vendor.img:ro",
               "-v", f"{output}:/output", "-v", f"{Path(__file__).resolve().parent}:/tools:ro",
               "--entrypoint", "timeout", image, "--kill-after=5", "120", "python3", "-B",
               "/tools/prepare-pico-firmware.py", "/vendor.img", "--output", "/output",
               "--sha256", args.sha256, "--reference-build", args.reference_build, "--inside-container"]
    report = {"status": "started", "command": command, "container_image": image,
              "helpers_sha256": helpers, "source_image_sha256": args.sha256}
    try:
        with (output / "prepare.log").open("wb") as log:
            result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, timeout=140)
        if result.returncode:
            raise RuntimeError("Pico firmware extraction failed; inspect " + str(output / "prepare.log"))
        validate(output)
        if digest(source) != args.sha256 or any(digest(Path(__file__).parent / n) != h for n, h in helpers.items()):
            raise ValueError("Firmware inputs changed during preparation")
        report.update(status="prepared", manifest_sha256=digest(output / "manifest.json"))
    except (OSError, ValueError, RuntimeError, KeyError, subprocess.SubprocessError) as error:
        report.update(status="failed", error=str(error))
        raise
    finally:
        (output / "prepare.json").write_text(json.dumps(report, indent=2) + "\n")
    print(output / "manifest.json")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("vendor_image", type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--sha256", required=True)
    parser.add_argument("--reference-build", required=True)
    parser.add_argument("--engine", default=os.environ.get("CONTAINER_ENGINE", "docker"))
    parser.add_argument("--image", default="localhost/armada-vr:vm")
    parser.add_argument("--inside-container", action="store_true", help=argparse.SUPPRESS)
    try:
        run(parser.parse_args())
    except (OSError, ValueError, RuntimeError, KeyError, subprocess.SubprocessError) as error:
        parser.exit(1, str(error) + "\n")
