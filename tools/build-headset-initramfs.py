#!/usr/bin/env python3
"""Build a Linux initramfs using only the selected vendor build's kernel modules."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import subprocess
import time


spec = importlib.util.spec_from_file_location("quest_firmware", Path(__file__).with_name("prepare-quest-firmware.py"))
firmware = importlib.util.module_from_spec(spec)
spec.loader.exec_module(firmware)


def digest(path):
    if path.is_symlink() or not path.is_file():
        raise ValueError(f"Expected a regular file: {path}")
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def inputs(kernel):
    report = json.loads((kernel / "build.json").read_text())
    if report.get("status") != "compiled" or report.get("variant") not in ("linux-userspace", "qemu-abi"):
        raise ValueError("Requires a compiled Linux-userspace or QEMU ABI vendor kernel")
    if report.get("qemu_transports") is not (report["variant"] == "qemu-abi"):
        raise ValueError("Kernel variant and QEMU transport flag disagree")
    expected = {item["path"]: item for item in report["artifacts"]}
    hashes = {}
    for name in ("Image", "modules.tar.gz", "modules.json", "resolved.config"):
        hashes[name] = digest(kernel / name)
        if hashes[name] != expected[name]["sha256"]:
            raise ValueError(f"Kernel build checksum mismatch: {name}")
    modules = json.loads((kernel / "modules.json").read_text())
    versions = {item["vermagic"][0].split()[0] for item in modules}
    if len(versions) != 1 or len(modules) != report["module_count"]:
        raise ValueError("Inconsistent kernel module inventory")
    version = versions.pop()
    if not re.fullmatch(r"[0-9A-Za-z_.+-]+", version):
        raise ValueError("Invalid kernel module release")
    names = [item["name"][0] for item in modules]
    if not names or any(not re.fullmatch(r"[0-9A-Za-z_-]+", name) for name in names):
        raise ValueError("Invalid kernel module name")
    return report, hashes, version, names


SCRIPT = r'''
set -eu
mkdir /tmp/modules /tmp/dracut-conf /tmp/unpacked
tar -xzf /kernel/modules.tar.gz -C /tmp/modules --no-same-owner
test ! -e "/lib/modules/$1"
cp -a "/tmp/modules/lib/modules/$1" "/lib/modules/$1"
if [ "$3" = yes ]; then
    set -- "$@" --include /firmware/files /usr/lib/firmware
fi
dracut --kver "$1" --kmoddir "/lib/modules/$1" \
    --kernel-image /kernel/Image --no-hostonly --no-hostonly-cmdline \
    --no-hostonly-default-device --no-early-microcode --reproducible \
    --nostrip --gzip --nofscks --nomdadmconf --nolvmconf \
    --conf /dev/null --confdir /tmp/dracut-conf \
    --modules 'systemd systemd-initrd systemd-udevd systemd-modules-load kernel-modules rootfs-block fs-lib base shutdown dracut-systemd initqueue' \
    --add-drivers "$2" "${@:4}" /output/initramfs.img
cd /tmp/unpacked
lsinitrd --unpack /output/initramfs.img
python3 - /kernel/modules.json /output/contents.json "$3" <<'PY'
import hashlib,json,sys
from pathlib import Path
expected=json.loads(Path(sys.argv[1]).read_text())
records=[]
for item in expected:
    path=Path(item['path'])
    if not path.is_file() or hashlib.sha256(path.read_bytes()).hexdigest()!=item['sha256']:
        raise SystemExit('Missing or changed kernel module: '+str(path))
    records.append({'path':str(path),'sha256':item['sha256']})
actual={p.resolve() for p in Path('usr/lib/modules').rglob('*.ko*') if p.is_file()}
if actual != {Path(item['path']).resolve() for item in expected}:
    raise SystemExit('Initramfs contains an unexpected kernel module')
firmware_files=[]
if sys.argv[3]=='yes':
    firmware_files=json.loads(Path('/firmware/manifest.json').read_text())['files']
    for item in firmware_files:
        path=Path('usr/lib/firmware')/item['path']
        if not path.is_file() or path.is_symlink() or hashlib.sha256(path.read_bytes()).hexdigest()!=item['sha256']:
            raise SystemExit('Missing or changed firmware: '+item['path'])
actual={str(p.relative_to('usr/lib/firmware')) for p in Path('usr/lib/firmware').rglob('*') if p.is_file()}
if actual!={item['path'] for item in firmware_files}:
    raise SystemExit('Initramfs contains unexpected firmware')
required=['init','usr/lib/systemd/systemd','usr/lib/systemd/systemd-volatile-root',
          'usr/lib/systemd/system/systemd-volatile-root.service','usr/bin/mount','usr/bin/kmod',
          'usr/bin/dracut-emergency','usr/lib/systemd/system/dracut-initqueue.service']
for name in required:
    path=Path(name)
    if path.is_symlink() and path.readlink().is_absolute():
        path=Path(str(path.readlink()).lstrip('/'))
    if not path.is_file() or not path.resolve().is_relative_to(Path.cwd()):
        raise SystemExit('Missing initramfs component: '+name)
Path(sys.argv[2]).write_text(json.dumps({'module_count':len(records),'modules':records,'required_files':required,'firmware':firmware_files},indent=2)+'\n')
PY
lsinitrd -m /output/initramfs.img > /output/dracut-modules.txt
rpm -q dracut systemd kmod > /output/tool-versions.txt
'''


def run(args):
    kernel, output = args.kernel.resolve(), args.output.resolve()
    report, hashes, version, names = inputs(kernel)
    firmware_dir = args.firmware.absolute() if args.firmware else None
    firmware_manifest = firmware.validate(firmware_dir) if firmware_dir else None
    if firmware_dir and any(c in str(firmware_dir) for c in (",", ":")):
        raise ValueError("Firmware path must not contain commas or colons")
    if output.exists() or any("," in str(path) or ":" in str(path) for path in (kernel, output)):
        raise ValueError("Choose a new output directory and paths without commas or colons")
    image = subprocess.check_output([args.engine, "image", "inspect", args.image, "--format", "{{.Id}}"], text=True).strip()
    output.mkdir(parents=True)
    result = {"status": "started", "purpose": "Linux vendor-kernel initramfs", "kernel_release": version,
              "kernel_variant": report["variant"], "qemu_transports": report["qemu_transports"],
              "kernel_build_sha256": digest(kernel / "build.json"), "kernel_inputs_sha256": hashes,
              "hardware_boot_verified": False, "hardware_flash_image": False,
              "container_image": image, "builder_sha256": digest(Path(__file__)),
              "firmware": firmware_manifest,
              "firmware_validator_sha256": digest(Path(firmware.__file__))}
    command = [args.engine, "run", "--rm", "--init", "--network", "none", "--memory", "2g", "--memory-swap", "2g",
               "--cpus", "2", "--pids-limit", "256", "-e", "SOURCE_DATE_EPOCH=0", "-e", "PYTHONDONTWRITEBYTECODE=1",
               "-v", f"{kernel}:/kernel:ro", "-v", f"{output}:/output"]
    if firmware_dir:
        command.extend(["-v", f"{firmware_dir}:/firmware:ro"])
    command.extend(["--entrypoint", "timeout", image, "--kill-after=5", "240", "bash", "-c", SCRIPT,
                    "build-initramfs", version, " ".join(names), "yes" if firmware_dir else "no"])
    result["command"] = command
    started = time.monotonic()
    try:
        with (output / "build.log").open("wb") as log:
            completed = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, timeout=260)
            if completed.returncode:
                raise RuntimeError(f"Initramfs container exited {completed.returncode}; inspect {output / 'build.log'}")
        if (digest(kernel / "build.json") != result["kernel_build_sha256"] or
                any(digest(kernel / name) != value for name, value in hashes.items())):
            raise ValueError("Kernel inputs changed during initramfs construction")
        if firmware_dir and firmware.validate(firmware_dir) != firmware_manifest:
            raise ValueError("Firmware inputs changed during initramfs construction")
        result["artifacts"] = [{"path": name, "bytes": (output / name).stat().st_size,
                                "sha256": digest(output / name)} for name in
                               ("initramfs.img", "contents.json", "dracut-modules.txt", "tool-versions.txt")]
        result["status"] = "built"
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        result.update(status="failed", error=str(error))
        raise
    finally:
        result["elapsed_seconds"] = round(time.monotonic() - started, 3)
        (output / "build.json").write_text(json.dumps(result, indent=2) + "\n")
    print(output / "initramfs.img")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("kernel", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--firmware", type=Path, help="optional prepared Quest ADSP firmware directory")
    parser.add_argument("--image", default="localhost/armada-vr:vm")
    parser.add_argument("--engine", default=os.environ.get("CONTAINER_ENGINE", "docker"))
    try:
        run(parser.parse_args())
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        parser.exit(1, f"{error}\n")
