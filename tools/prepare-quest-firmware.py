#!/usr/bin/env python3
"""Extract and validate Quest ADSP or GPU firmware as data from a local vendor image."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import sys


SERVICE_FILES = ("adspr.jsn", "adsps.jsn", "adspua.jsn", "battmgr.jsn")


def digest(path):
    if path.is_symlink() or not path.is_file():
        raise ValueError(f"Expected a regular file: {path}")
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def required_segments(data, prefix="adsp"):
    if len(data) < 52 or data[:7] != b"\x7fELF\x01\x01\x01":
        raise ValueError("Requires a little-endian ELF32 MDT")
    header = struct.unpack_from("<16sHHIIIIIHHHHHH", data)
    _, _, machine, version, _, phoff, shoff, _, ehsize, phsize, count, shsize, shcount, _ = header
    if ((machine, version, ehsize, phoff, phsize) != (164, 1, 52, 52, 32) or
            not 2 <= count <= 100 or phoff + count * phsize > len(data) or
            ((shsize or shcount) and (shsize != 40 or shoff + shcount * shsize > len(data)))):
        raise ValueError("Unsupported or truncated Qualcomm MDT geometry")
    headers = [struct.unpack_from("<8I", data, phoff + i * phsize) for i in range(count)]
    if headers[0][0] == 1 or not phoff + count * phsize <= headers[0][4] <= len(data):
        raise ValueError("Invalid MDT metadata header segment")
    hash_indices = [i for i, h in enumerate(headers[1:], 1) if h[0] != 1 and h[6] & (7 << 24) == 2 << 24]
    if len(hash_indices) != 1:
        raise ValueError("Requires one MDT signing-metadata segment")
    if not any(h[1] + h[4] > len(data) for h in headers):
        raise ValueError("Requires split Qualcomm firmware")
    required = {}
    for i, h in enumerate(headers):
        kind, _, _, physical, filesz, memsz, flags, _ = h
        loadable = kind == 1 and flags & (7 << 24) != 2 << 24 and memsz > 0
        if loadable and (filesz > memsz or physical + memsz > 2**32):
            raise ValueError("Invalid MDT loadable segment bounds")
        if (loadable and filesz) or i == hash_indices[0]:
            if not 0 < filesz <= 32 * 1024**2:
                raise ValueError("MDT segment size exceeds limit")
            required[f"{prefix}.b{i:02d}"] = filesz
    if len(required) < 2 or sum(required.values()) > 128 * 1024**2:
        raise ValueError("Invalid MDT payload size")
    return required


def inspect_adsp_files(directory):
    mdt = directory / "adsp.mdt"
    digest(mdt)
    if mdt.stat().st_size > 1024**2:
        raise ValueError("MDT exceeds size limit")
    required = required_segments(mdt.read_bytes())
    records, services = [], []
    for name in sorted(["adsp.mdt", *required, *SERVICE_FILES]):
        path = directory / name
        sha = digest(path)
        size = path.stat().st_size
        if name in required and size != required[name]:
            raise ValueError(f"Truncated or oversized MDT segment: {name}")
        if name in SERVICE_FILES:
            if not 0 < size <= 65536:
                raise ValueError(f"Service JSON exceeds limit: {name}")
            value = json.loads(path.read_text())
            domain = value["sr_domain"]
            if ((domain["soc"], domain["domain"]) != ("msm", "adsp") or
                    not re.fullmatch(r"[a-z0-9_]+", domain["subdomain"]) or
                    type(domain["qmi_instance_id"]) is not int or not 0 <= domain["qmi_instance_id"] <= 65535):
                raise ValueError(f"Invalid ADSP service domain: {name}")
            for service in value["sr_service"]:
                if not all(re.fullmatch(r"[a-z0-9_]+", service[k]) for k in ("provider", "service")):
                    raise ValueError(f"Invalid ADSP service name: {name}")
            services.append({"file": name, "domain": domain, "services": value["sr_service"]})
        records.append({"path": name, "bytes": size, "sha256": sha})
    charger = [v for v in services if v["domain"]["subdomain"] == "charger_pd"]
    if len(charger) != 1 or not any((s["provider"], s["service"]) == ("tms", "servreg") for s in charger[0]["services"]):
        raise ValueError("Missing unique charger_pd service-registry declaration")
    actual = {p.name for p in directory.iterdir()}
    if actual != {item["path"] for item in records}:
        raise ValueError("Firmware directory has unexpected files")
    return records, services


def gmu_blocks(data):
    if not 0 < len(data) <= 32 * 1024**2:
        raise ValueError("GMU firmware exceeds size limit")
    ranges = ((0, 0x4000), (0x4000, 0x1000000), (0x10004000, 0x10008000),
              (0x60000000, 0x80000000), (0xc0000000, 0xe0000000))
    blocks, allocated = [], []
    offset, payloads = 0, 0
    while offset < len(data):
        if len(data) - offset < 16:
            raise ValueError("Truncated GMU block header")
        address, size, kind, value = struct.unpack_from("<4I", data, offset)
        if size > len(data) - offset - 16 or size % 4:
            raise ValueError("Truncated or unaligned GMU block payload")
        extent = size if size else value if kind in (1, 7) else 0
        region = next((i for i, (start, end) in enumerate(ranges)
                       if start <= address and address + extent <= end), None)
        if extent and (address % 4 or region is None):
            raise ValueError("GMU block is outside the Gen7 virtual address ranges")
        if not size:
            if payloads:
                raise ValueError("GMU metadata must precede payload blocks")
            if kind in (1, 7):
                if not value:
                    raise ValueError("Empty GMU preallocation")
                allocated.append((address, address + value))
        else:
            payloads += 1
            if region not in (0, 2) and not any(a <= address and address + size <= b for a, b in allocated):
                raise ValueError("GMU payload has no matching preallocation")
        blocks.append({"offset": offset, "address": address, "bytes": size, "type": kind, "value": value})
        offset += 16 + size
    if not payloads:
        raise ValueError("GMU firmware has no payload")
    return blocks


def inspect_gpu_files(directory):
    mdt = directory / "a740v3_zap.mdt"
    digest(mdt)
    if mdt.stat().st_size > 1024**2:
        raise ValueError("MDT exceeds size limit")
    required = required_segments(mdt.read_bytes(), "a740v3_zap")
    names = ["a740v3_zap.mdt", *required, "a740v3_sqe.fw", "gmu_gen70200.bin"]
    records = []
    for name in sorted(names):
        path = directory / name
        sha = digest(path)
        size = path.stat().st_size
        if not 0 < size <= 32 * 1024**2:
            raise ValueError("GPU firmware exceeds size limit: " + name)
        if name in required and size != required[name]:
            raise ValueError("Truncated or oversized MDT segment: " + name)
        records.append({"path": name, "bytes": size, "sha256": sha})
    sqe = (directory / "a740v3_sqe.fw").read_bytes()
    if len(sqe) < 16 or len(sqe) % 4:
        raise ValueError("SQE firmware must contain aligned header and instruction words")
    words = struct.unpack_from("<4I", sqe)
    version = words[1] if words[1] & 15 != 10 else (words[1] & ~4095) | ((words[3] & 0xfff000) >> 12)
    blocks = gmu_blocks((directory / "gmu_gen70200.bin").read_bytes())
    if {p.name for p in directory.iterdir()} != set(names):
        raise ValueError("Firmware directory has unexpected files")
    return records, {"compatible": "qcom,adreno-gpu-gen7-6-0", "sqe_version": version,
                     "sqe_words": (len(sqe) - 4) // 4, "gmu_blocks": blocks}


def inspect_files(directory, component="adsp"):
    if component == "adsp":
        return inspect_adsp_files(directory)
    if component == "gpu":
        return inspect_gpu_files(directory)
    raise ValueError("Unknown Quest firmware component")


def validate(directory, component="adsp"):
    if directory.is_symlink():
        raise ValueError("Firmware directory must not be a symlink")
    digest(directory / "manifest.json")
    manifest = json.loads((directory / "manifest.json").read_text())
    if (manifest.get("schema_version") != 1 or manifest.get("component") != "quest3-" + component or
            manifest.get("status") != "prepared" or
            not re.fullmatch(r"[0-9]{10,20}", manifest.get("reference_build", "")) or
            not re.fullmatch(r"[0-9a-f]{64}", manifest.get("source_image_sha256", ""))):
        raise ValueError("Invalid Quest firmware manifest")
    files = directory / "files"
    if files.is_symlink():
        raise ValueError("Firmware files directory must not be a symlink")
    records, details = inspect_files(files, component)
    if records != manifest["files"] or details != manifest["service_domains" if component == "adsp" else "gpu"]:
        raise ValueError("Firmware manifest does not match its files")
    return manifest


def extract(source, output, expected, build, component="adsp"):
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
        if result.returncode or not path.is_file():
            raise ValueError(f"Unable to extract firmware file: {name}")

    prefix = "adsp" if component == "adsp" else "a740v3_zap"
    dump(prefix + ".mdt")
    extra = SERVICE_FILES if component == "adsp" else ("a740v3_sqe.fw", "gmu_gen70200.bin")
    for name in sorted([*required_segments((files / (prefix + ".mdt")).read_bytes(), prefix), *extra]):
        dump(name)
    records, details = inspect_files(files, component)
    if digest(source) != expected:
        raise ValueError("Vendor image changed during extraction")
    manifest = {"schema_version": 1, "component": "quest3-" + component, "status": "prepared",
                "reference_build": build, "source_image_sha256": expected,
                "manufacturer_signature_verified": False, "device_compatibility_verified": False,
                "firmware_executed": False, "files": records,
                "service_domains" if component == "adsp" else "gpu": details}
    (output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    validate(output, component)


def run(args):
    if not re.fullmatch(r"[0-9a-f]{64}", args.sha256) or not re.fullmatch(r"[0-9]{10,20}", args.reference_build):
        raise ValueError("Requires a SHA-256 and numeric reference build")
    source, output = args.vendor_image.absolute(), args.output.absolute()
    if args.inside_container:
        extract(source, output, args.sha256, args.reference_build, args.component)
        return
    if any("," in str(p) or ":" in str(p) for p in (source, output)):
        raise ValueError("Container paths must not contain commas or colons")
    if output.exists() or output.is_symlink():
        raise ValueError("Output exists; choose a new directory")
    if digest(source) != args.sha256:
        raise ValueError("Vendor image checksum mismatch")
    image = subprocess.check_output([args.engine, "image", "inspect", args.image, "--format", "{{.Id}}"], text=True).strip()
    output.mkdir(parents=True)
    command = [args.engine, "run", "--rm", "--init", "--network", "none", "--read-only",
               "--memory", "256m", "--memory-swap", "256m", "--cpus", "1", "--pids-limit", "64",
               "--tmpfs", "/tmp:rw,noexec,nosuid,size=16m", "-v", f"{source}:/vendor.img:ro",
               "-v", f"{output}:/output", "-v", f"{Path(__file__).resolve()}:/prepare.py:ro",
               "--entrypoint", "timeout", image, "--kill-after=5", "120", "python3", "-B", "/prepare.py",
               "/vendor.img", "--output", "/output", "--sha256", args.sha256,
               "--reference-build", args.reference_build, "--component", args.component, "--inside-container"]
    report = {"status": "started", "command": command, "container_image": image,
              "preparer_sha256": digest(Path(__file__)), "source_image_sha256": args.sha256}
    try:
        with (output / "prepare.log").open("wb") as log:
            result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, timeout=140)
        if result.returncode:
            raise RuntimeError(f"Firmware preparation failed; inspect {output / 'prepare.log'}")
        validate(output, args.component)
        if digest(source) != args.sha256:
            raise ValueError("Vendor image changed during preparation")
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
    parser.add_argument("--component", choices=("adsp", "gpu"), default="adsp")
    parser.add_argument("--sha256", required=True)
    parser.add_argument("--reference-build", required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--engine", default=os.environ.get("CONTAINER_ENGINE", "docker"))
    parser.add_argument("--image", default="localhost/armada-vr:vm")
    parser.add_argument("--inside-container", action="store_true", help=argparse.SUPPRESS)
    try:
        run(parser.parse_args())
    except (OSError, ValueError, RuntimeError, KeyError, subprocess.SubprocessError) as error:
        parser.exit(1, f"{error}\n")
