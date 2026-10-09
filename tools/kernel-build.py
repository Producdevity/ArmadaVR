#!/usr/bin/env python3
"""Compile a vendor kernel in Linux; export diagnostics and unassembled artifacts."""
import argparse
import hashlib
import importlib.util
import json
import mmap
import os
from pathlib import Path
import shutil
import subprocess
import struct
import sys
import tarfile
import tempfile
import time

spec = importlib.util.spec_from_file_location("kernel_source", Path(__file__).with_name("kernel-source.py"))
sources = importlib.util.module_from_spec(spec)
spec.loader.exec_module(sources)


def config_values(path):
    result = {}
    for line in path.read_text().splitlines():
        if line.startswith("CONFIG_") and "=" in line:
            name, value = line.split("=", 1)
            result[name] = value
        elif line.startswith("# CONFIG_") and line.endswith(" is not set"):
            result[line[2:-11]] = "n"
    return result


def config_changes(requested, resolved):
    actual = config_values(resolved)
    return [{"symbol": key, "requested": value, "resolved": actual.get(key, "n")}
            for key, value in sorted(config_values(requested).items())
            if actual.get(key, "n") != value]


def file_record(path, root):
    with path.open("rb") as stream:
        digest = hashlib.file_digest(stream, "sha256").hexdigest()
    return {"path": path.relative_to(root).as_posix(), "bytes": path.stat().st_size, "sha256": digest}


def module_metadata(data):
    entries = data.split("\0")
    result = {field: [entry.split("=", 1)[1] for entry in entries if entry.startswith(field + "=")]
              for field in ("name", "vermagic", "depends", "softdep", "firmware", "alias")}
    if len(result["name"]) != 1 or len(result["vermagic"]) != 1:
        raise ValueError("modinfo did not return a module name and vermagic")
    return result


def validate_image_layout(vmlinux, image):
    """Check the ELF's allocated memory against the raw ARM64 Image extent."""
    with vmlinux.open("rb") as stream, mmap.mmap(stream.fileno(), 0, access=mmap.ACCESS_READ) as elf:
        def bounded(offset, size):
            if offset < 0 or size < 0 or offset + size > len(elf):
                raise ValueError("Truncated kernel ELF")
            return elf[offset:offset + size]

        header = struct.unpack("<16sHHIQQQIHHHHHH", bounded(0, 64))
        if (header[0][:7] != b"\x7fELF\x02\x01\x01" or header[1] not in (2, 3)
                or header[2:4] != (183, 1)):
            raise ValueError("Expected a little-endian AArch64 executable ELF")
        if header[11] != 64 or not 0 < header[13] < header[12]:
            raise ValueError("Unsupported kernel ELF section table")
        sections = [struct.unpack("<IIQQQQIIQQ", bounded(header[6] + i * 64, 64))
                    for i in range(header[12])]

        def strings(section):
            if section[1] != 3:
                raise ValueError("Expected ELF string table")
            return bounded(section[4], section[5])

        def name(table, offset):
            if not 0 <= offset < len(table) or (end := table.find(b"\0", offset)) < 0:
                raise ValueError("Invalid ELF string offset")
            return table[offset:end].decode("ascii")

        section_names = strings(sections[header[13]])
        tables = [section for section in sections if section[1] == 2]
        if len(tables) != 1 or tables[0][9] != 24 or tables[0][5] % 24:
            raise ValueError("Expected one complete kernel symbol table")
        table = tables[0]
        if not 0 < table[6] < len(sections):
            raise ValueError("Invalid kernel symbol string table")
        symbol_names = strings(sections[table[6]])
        symbols = {}
        required = {"_text", "_end", "__bss_start", "__bss_stop"}
        for offset in range(table[4], table[4] + table[5], 24):
            entry = struct.unpack("<IBBHQQ", bounded(offset, 24))
            label = name(symbol_names, entry[0])
            if label in required:
                if label in symbols or entry[3] == 0:
                    raise ValueError("Ambiguous or undefined kernel boundary: " + label)
                symbols[label] = entry[4]
        if set(symbols) != required:
            raise ValueError("Missing kernel memory boundaries")
        start, end = symbols["_text"], symbols["_end"]
        if not start < symbols["__bss_start"] <= symbols["__bss_stop"] <= end < 2**64:
            raise ValueError("Invalid kernel memory boundaries")
        with image.open("rb") as raw:
            image_header = raw.read(64)
        if len(image_header) != 64 or image_header[56:60] != b"ARM\x64":
            raise ValueError("Expected raw ARM64 Image")
        if struct.unpack_from("<Q", image_header, 16)[0] != end - start or image.stat().st_size > end - start:
            raise ValueError("Image extent disagrees with linked kernel boundaries")
        allocated = []
        for section in sections:
            label = name(section_names, section[0])
            if not section[2] & 2 or not section[5]:
                continue
            address, size = section[3], section[5]
            if not start <= address < address + size <= end:
                raise ValueError("Allocated ELF section outside Image extent: " + label)
            if section[1] == 8 and not symbols["__bss_start"] <= address < address + size <= symbols["__bss_stop"]:
                raise ValueError("Uninitialized ELF section outside kernel BSS: " + label)
            if section[1] != 8:
                bounded(section[4], size)
            allocated.append({"name": label, "address": address, "bytes": size, "type": section[1]})
        return {"boundaries": symbols, "image_size": end - start, "allocated_sections": allocated}


def tree_digest(root):
    digest = hashlib.sha256()
    for path in sorted(root.rglob("*")):
        relative = path.relative_to(root).as_posix()
        if path.is_symlink():
            value = [relative, "symlink", os.readlink(path)]
        elif path.is_file():
            value = [relative, path.stat().st_mode & 0o777, file_record(path, root)["sha256"]]
        elif path.is_dir():
            value = [relative, "directory", path.stat().st_mode & 0o777]
        else:
            raise ValueError(f"Unexpected source entry: {path}")
        digest.update(json.dumps(value, separators=(",", ":")).encode() + b"\n")
    return digest.hexdigest()


def patched_source(source, cache, patches):
    target = cache / "source"
    stamp = cache / "source-sha256.txt"
    if target.exists():
        if not stamp.is_file() or tree_digest(target) != stamp.read_text().strip():
            raise ValueError("Patched kernel source changed; use a new build cache")
        return target, stamp.read_text().strip()
    shutil.copytree(source, target, symlinks=True)
    for patch in patches:
        subprocess.run(["patch", "--batch", "--fuzz=0", "--forward", "-p1", "-i", str(patch)],
                       cwd=target, check=True, timeout=30)
    digest = tree_digest(target)
    stamp.write_text(digest + "\n")
    return target, digest


def prepare_pico_audio(source, cache):
    links = {"include/soc/internal.h": "drivers/base/regmap/internal.h",
             "soc/core.h": "drivers/pinctrl/core.h",
             "soc/pinctrl-utils.h": "drivers/pinctrl/pinctrl-utils.h"}
    original = source / "audio-kernel"
    for name, target in links.items():
        link = original / name
        header = source / target
        if not link.is_symlink() or not header.is_file() or link.resolve() != header.resolve():
            raise ValueError(f"Unexpected Pico audio header link: {name}")
    directory = Path(tempfile.mkdtemp(prefix="audio-", dir=cache)) / "source"
    shutil.copytree(original, directory, symlinks=True)
    for name, target in links.items():
        link = directory / name
        link.unlink()
        link.symlink_to((source / target).resolve())
    shutil.copyfile(sources.ROOT / "profiles/kernel/pico-audio.Kbuild", directory / "Kbuild")
    return directory


def build(profile, workspace, output, archive, linux_userspace=False, configure_only=False, qemu_abi=False):
    if sys.platform != "linux":
        raise ValueError("Use the Linux container workflow: just build-vendor-kernel")
    if profile["repository"] == "bytedance/neo3-kernel" and qemu_abi:
        raise ValueError("Pico QEMU transports have not been ported")
    targets = profile.get("build_targets", ["Image", "modules", "dtbs"])
    if targets not in (["Image", "modules"], ["Image", "modules", "dtbs"]):
        raise ValueError("Unsupported kernel build targets")
    if output.exists() and any(output.iterdir()):
        raise ValueError("Output exists; choose a new report directory")
    output.mkdir(parents=True, exist_ok=True)
    workspace.mkdir(parents=True, exist_ok=True)
    sources.require_case_sensitive(workspace)
    source = workspace / "source"
    cache = workspace / "linux-userspace" if linux_userspace else workspace
    cache.mkdir(exist_ok=True)
    build_dir = cache / "build"
    report = {"schema_version": 1, "purpose": "vendor kernel compilation baseline",
              "repository": profile["repository"], "commit": profile["commit"],
              "hardware_boot_verified": False, "flash_image": False,
              "stock_toolchain_reproduction": False, "status": "started", "commands": [], "artifacts": [],
              "variant": "qemu-abi" if qemu_abi else "linux-userspace" if linux_userspace else "vendor",
              "qemu_transports": qemu_abi,
              "build_targets": targets, "device_trees_compiled": False,
              "limits": {"memory": os.environ.get("KERNEL_BUILD_MEMORY", "external"), "make_jobs": 2,
                         "linker_threads": 1, "pahole_jobs": 1, "compile_seconds": 2700}}
    started = time.monotonic()

    def save():
        report["elapsed_seconds"] = round(time.monotonic() - started, 2)
        (output / "build.json").write_text(json.dumps(report, indent=2) + "\n")

    env = {**os.environ, "ARCH": "arm64", "LLVM": "1", "LLVM_IAS": "1", "REAL_CC": "clang",
           "CROSS_COMPILE": "aarch64-linux-gnu-", "CROSS_COMPILE_COMPAT": "arm-linux-gnueabi-",
           "CROSS_COMPILE_ARM32": "arm-linux-gnueabi-", "KBUILD_BUILD_USER": "armada",
           "KBUILD_BUILD_HOST": "kernel-builder", "KBUILD_BUILD_TIMESTAMP": "2026-09-10 00:00:00 UTC"}

    def run(command, log, timeout=120):
        report["commands"].append(command)
        save()
        print(f"Running {log}", flush=True)
        with (output / log).open("wb") as stream:
            subprocess.run(command, cwd=build_dir, env=env, stdout=stream, stderr=subprocess.STDOUT,
                           check=True, timeout=timeout)

    try:
        if not source.exists():
            sources.full_source(profile, archive, source)
        report["source"] = sources.full_source(profile, archive, source, verify_only=True)
        report["compiler"] = subprocess.check_output(["clang", "--version"], text=True).strip()
        report["container_image"] = os.environ.get("KERNEL_BUILD_IMAGE_ID", "unrecorded")
        (output / "compiler-packages.txt").write_text(subprocess.check_output(["dpkg-query", "-W"], text=True))
        identity = {"profile_sha256": hashlib.sha256(json.dumps(profile, sort_keys=True).encode()).hexdigest(),
                    "compiler": report["compiler"], "container_image": report["container_image"],
                    "config_fragments": profile["config_fragments"]}
        pico_make_options = []
        if profile["repository"] == "bytedance/neo3-kernel":
            pico_make_options = ["REAL_CC=clang", "CLANG_TRIPLE=aarch64-linux-gnu-",
                                 "KCFLAGS=-Wno-unused-but-set-variable",
                                 "CC_COMPAT=clang --target=arm-linux-gnueabi --prefix=/usr/bin/arm-linux-gnueabi- --gcc-toolchain=/usr -no-integrated-as"]
            identity["make_overrides"] = pico_make_options
            identity["audio_kbuild_sha256"] = hashlib.sha256(
                (sources.ROOT / "profiles/kernel/pico-audio.Kbuild").read_bytes()).hexdigest()
            report["compiler_compatibility"] = {
                "disabled_diagnostic": "unused-but-set-variable",
                "reason": "Clang 13 added this diagnostic; the stock kernel used Clang 8",
                "vendor_warning_guard": "retained",
            }
        extensions = [sources.ROOT / "profiles/kernel" / name for name in
                      ("linux-userspace.config", "pico-neo3-linux.config"
                       if profile["repository"] == "bytedance/neo3-kernel" else "quest3-build.config")] if linux_userspace else []
        if qemu_abi:
            extensions.append(sources.ROOT / "profiles/kernel/qemu-abi.config")
        patches = sorted((sources.ROOT / "patches/kernel/linux-userspace").glob("*.patch")) if linux_userspace else []
        if profile["repository"] == "bytedance/neo3-kernel":
            patches = sorted((sources.ROOT / "patches/kernel/pico-neo3").glob("*.patch"))
            if not patches:
                raise ValueError("Missing Pico compiler portability patches")
            if linux_userspace:
                patches += sorted((sources.ROOT / "patches/kernel/pico-neo3-linux").glob("*.patch"))
        if qemu_abi:
            qemu_patches = sorted((sources.ROOT / "patches/kernel/qemu-abi").glob("*.patch"))
            if not qemu_patches:
                raise ValueError("Missing QEMU ABI source patches")
            patches += qemu_patches
        if patches:
            identity["patches"] = {str(path.relative_to(sources.ROOT)): hashlib.sha256(path.read_bytes()).hexdigest() for path in patches}
        if linux_userspace:
            identity["config_extensions"] = {path.name: hashlib.sha256(path.read_bytes()).hexdigest() for path in extensions}
        if linux_userspace or patches:
            if not linux_userspace:
                cache = workspace / "vendor"
                cache.mkdir(exist_ok=True)
            cache = cache / hashlib.sha256(json.dumps(identity, sort_keys=True).encode()).hexdigest()[:16]
            cache.mkdir(exist_ok=True)
            build_dir = cache / "build"
        if patches:
            source, report["patched_source_sha256"] = patched_source(source, cache, patches)
        stamp = cache / "build-identity.json"
        if build_dir.exists() and (not stamp.is_file() or json.loads(stamp.read_text()) != identity):
            raise ValueError("Cached build identity differs; use a new Linux volume")
        build_dir.mkdir(exist_ok=True)
        stamp.write_text(json.dumps(identity, indent=2) + "\n")
        fragments = [str(sources.source_path(source, name)) for name in profile["config_fragments"]]
        fragments.extend(str(path) for path in extensions)
        report["identity"] = identity
        config_dir = cache / "config"
        config_dir.mkdir(exist_ok=True)
        run(["bash", str(source / "scripts/kconfig/merge_config.sh"), "-m", "-O", str(config_dir), *fragments],
            "config-merge.log")
        shutil.copyfile(config_dir / ".config", output / "requested.config")
        config = build_dir / ".config"
        requested_stamp = cache / "requested.config"
        resolved_stamp = cache / "resolved.config"
        if not config.exists():
            shutil.copyfile(config_dir / ".config", config)
        elif not requested_stamp.is_file() or requested_stamp.read_bytes() != (config_dir / ".config").read_bytes():
            raise ValueError("Cached requested configuration differs; use a new Linux volume")
        elif not resolved_stamp.is_file() or resolved_stamp.read_bytes() != config.read_bytes():
            raise ValueError("Cached resolved configuration differs; use a new Linux volume")
        shutil.copyfile(config_dir / ".config", requested_stamp)
        make = ["make", "-C", str(source), f"O={build_dir}", "PYTHON=python3",
                "LD=ld.lld --threads=1 --lto-partitions=1",
                "PAHOLE_FLAGS=--skip_encoding_btf_enum64 --jobs=1"]
        make.extend(pico_make_options)
        run([*make, "olddefconfig"], "config-resolve.log")
        shutil.copyfile(config, output / "resolved.config")
        shutil.copyfile(config, resolved_stamp)
        report["config_changes"] = config_changes(output / "requested.config", config)
        if linux_userspace:
            report["unresolved_userspace_options"] = [change for path in extensions for change in config_changes(path, config)]
            if report["unresolved_userspace_options"]:
                raise ValueError("Kconfig rejected Linux userspace options; see build.json")
        values = config_values(config)
        report["optimization"] = {name: values.get(name, "n") for name in
                                  ("CONFIG_LTO_CLANG_FULL", "CONFIG_LTO_CLANG_THIN", "CONFIG_CFI_CLANG", "CONFIG_META_SAMPLE_PGO")}
        if configure_only:
            report["status"] = "configured"
            return report
        report["status"] = "compiling"
        # GNU timeout terminates make's complete process group, including compiler children.
        run(["timeout", "--kill-after=15", "2700", *make, "-j2", *targets], "compile.log", 2760)
        report["image_layout"] = validate_image_layout(build_dir / "vmlinux", build_dir / "arch/arm64/boot/Image")
        for relative in ("arch/arm64/boot/Image", "System.map", "Module.symvers", ".config", "modules.order", "modules.builtin"):
            shutil.copyfile(build_dir / relative, output / Path(relative).name)
        dt_dir = output / "device-trees"
        dt_dir.mkdir()
        for path in sorted((build_dir / "arch/arm64/boot/dts").rglob("*")):
            if path.suffix in (".dtb", ".dtbo"):
                destination = dt_dir / path.relative_to(build_dir / "arch/arm64/boot/dts")
                destination.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(path, destination)
        report["device_trees_compiled"] = "dtbs" in targets
        audio_make = None
        if profile["repository"] == "bytedance/neo3-kernel":
            if values.get("CONFIG_ARCH_KONA") != "y":
                raise ValueError("Pico audio requires the Kona configuration")
            audio = prepare_pico_audio(source, cache)
            report["audio"] = {"source": str(audio), "stock_manufacturing_flags_verified": False,
                               "PICOVR_US_EURO_HEADSET": "unset"}
            # One modpost invocation resolves the vendor modules' circular imports.
            audio_make = [*make, f"M={audio}", "MODNAME=audio", f"AUDIO_ROOT={audio}",
                          "BOARD_PLATFORM=kona", "KBUILD_EXTRA_SYMBOLS=", "PICOVR_US_EURO_HEADSET="]
            run(["timeout", "--kill-after=15", "900", *audio_make, "-j2", "modules"],
                "audio-compile.log", 930)
            shutil.copyfile(audio / "Module.symvers", output / "audio-Module.symvers")
        staging = cache / "modules"
        if staging.exists():
            shutil.rmtree(staging)
        run([*make, f"INSTALL_MOD_PATH={staging}", "INSTALL_MOD_STRIP=1", "modules_install"], "modules-install.log", 300)
        if audio_make is not None:
            run([*audio_make, f"INSTALL_MOD_PATH={staging}", "INSTALL_MOD_DIR=extra/pico-audio",
                 "INSTALL_MOD_STRIP=1", "modules_install"], "audio-install.log", 300)
        for path in staging.glob("lib/modules/*/*"):
            if path.name in ("build", "source") and path.is_symlink():
                path.unlink()
        modules = []
        for path in sorted(staging.rglob("*.ko")):
            metadata = subprocess.check_output(["modinfo", "-0", str(path)]).decode()
            item = file_record(path, staging)
            item.update(module_metadata(metadata))
            modules.append(item)
        (output / "modules.json").write_text(json.dumps(modules, indent=2) + "\n")
        with tarfile.open(output / "modules.tar.gz", "w:gz") as stream:
            stream.add(staging / "lib", arcname="lib")
        report["module_count"] = len(modules)
        if patches:
            report["patched_source_after_build_sha256"] = tree_digest(source)
            if report["patched_source_after_build_sha256"] != report["patched_source_sha256"]:
                raise ValueError("Kernel build changed its patched source tree")
        report["source_after_build"] = sources.full_source(profile, archive, workspace / "source", verify_only=True)
        report["artifacts"] = [file_record(path, output) for path in sorted(output.rglob("*"))
                               if path.is_file() and path.name != "build.json"]
        report["status"] = "compiled"
    except (OSError, ValueError, subprocess.SubprocessError, tarfile.TarError) as error:
        report.update(status="failed", error=str(error))
        raise
    finally:
        report["resources"] = {}
        for name in ("memory.peak", "memory.events", "pids.peak"):
            path = Path("/sys/fs/cgroup") / name
            if path.is_file():
                report["resources"][name] = path.read_text().strip()
        save()
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("profile", choices=("quest3", "pico-neo3"))
    parser.add_argument("--workspace", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--archive", type=Path, required=True)
    parser.add_argument("--linux-userspace", action="store_true")
    parser.add_argument("--configure-only", action="store_true")
    parser.add_argument("--qemu-abi", action="store_true", help="Add virtual transports for userspace ABI tests")
    args = parser.parse_args()
    try:
        result = build(sources.read_profile(args.profile), args.workspace.resolve(), args.output.resolve(), args.archive.resolve(),
                       args.linux_userspace or args.qemu_abi, args.configure_only, args.qemu_abi)
        print(f"Kernel baseline {result['status']}; no headset boot verified")
    except (OSError, ValueError, subprocess.SubprocessError, tarfile.TarError) as error:
        parser.exit(1, f"{error}\n")


if __name__ == "__main__":
    main()
