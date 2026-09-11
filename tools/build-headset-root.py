#!/usr/bin/env python3
"""Build an offline ext4 root from pinned ARM64 userspace and matching headset inputs."""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("root_builder", ROOT / "tools/build-headset-initramfs.py")
builder = importlib.util.module_from_spec(spec)
spec.loader.exec_module(builder)
spec = importlib.util.spec_from_file_location("root_assembly", ROOT / "tools/assemble-quest-boot.py")
assembly = importlib.util.module_from_spec(spec)
spec.loader.exec_module(assembly)

SCRIPT = r'''
set -eu
mkdir -p /tmp/root /tmp/modules /tmp/verify/modules /tmp/verify/firmware
tar -xpf /output/rootfs.tar -C /tmp/root
tar -xzf /kernel/modules.tar.gz -C /tmp/modules --no-same-owner
python3 - "$1" <<'PYROOT'
import hashlib,json,shutil,sys
from pathlib import Path
root=Path('/tmp/root'); version=sys.argv[1]
if not (root/'usr/lib/systemd/systemd').is_file() or (root/'etc/initrd-release').exists():
    raise SystemExit('Expected full systemd userspace, not an initramfs')
removed=[]
for name in ['.dockerenv','run/.containerenv','vm-boot','usr/lib/modules',
             'etc/systemd/system/multi-user.target.wants/armada-vr-lab.service',
             'etc/systemd/system/multi-user.target.wants/armada-vr-desktop.service',
             'etc/systemd/system/dev-virtio\\x2dports-org.qemu.guest_agent.0.device.wants',
             'etc/systemd/network/20-qemu.network','etc/X11/xorg.conf.d/20-virtual-display.conf']:
    path=root/name
    if path.is_symlink() or path.is_file():
        path.unlink();removed.append(name)
    elif path.is_dir():
        shutil.rmtree(path);removed.append(name)
(root/'etc/machine-id').write_text('')
(root/'etc/hostname').write_text('armada-vr\n')
(root/'etc/fstab').write_text('# Root is mounted read-only by the initramfs; writable state is in RAM.\n')
default=root/'etc/systemd/system/default.target'
if default.is_symlink() or default.is_file():default.unlink()
default.symlink_to('/usr/lib/systemd/system/multi-user.target')
modules=root/'usr/lib/modules';modules.mkdir()
shutil.copytree(Path('/tmp/modules/lib/modules')/version,modules/version,symlinks=True)
fw=root/'usr/lib/firmware';fw.mkdir(exist_ok=True)
manifest=json.loads(Path('/firmware/manifest.json').read_text())
for item in manifest['files']:
    path=fw/item['path']
    if path.is_symlink() or path.exists():
        raise SystemExit('Base image already supplies selected ADSP firmware: '+str(path))
    shutil.copy2(Path('/firmware/files')/item['path'],path)
unit='usr/lib/systemd/system/armada-quest-boot.service'
shutil.copy2('/quest.service',root/unit)
metadata=root/'usr/share/armada-vr';metadata.mkdir(parents=True,exist_ok=True)
shutil.copy2('/kernel/build.json',metadata/'kernel-build.json')
shutil.copy2('/firmware/manifest.json',metadata/'quest-firmware.json')
Path('/output/root-settings.json').write_text(json.dumps({'removed_virtual_paths':removed,'unit_path':unit,
    'unit_sha256':hashlib.sha256((root/unit).read_bytes()).hexdigest(),'hostname':'armada-vr',
    'default_target':'multi-user.target','automatic_lab_tests':False,'automatic_desktop':False},indent=2)+'\n')
PYROOT
python3 - "$2" <<'PYSIZE'
import sys
with open('/output/rootfs.ext4','xb') as f:f.truncate(int(sys.argv[1])*1024**3)
PYSIZE
mkfs.ext4 -q -F -L "$3" -O ^orphan_file -E lazy_itable_init=0,lazy_journal_init=0 -d /tmp/root /output/rootfs.ext4
e2fsck -fn /output/rootfs.ext4 > /output/fsck.log 2>&1
debugfs -R stats /output/rootfs.ext4 > /output/filesystem.txt 2>&1
debugfs -R 'dump /usr/lib/systemd/system/armada-quest-boot.service /tmp/verify/quest.service' /output/rootfs.ext4
debugfs -R 'rdump /usr/lib/modules /tmp/verify/modules' /output/rootfs.ext4
debugfs -R 'rdump /usr/lib/firmware /tmp/verify/firmware' /output/rootfs.ext4
python3 - <<'PYVERIFY'
import hashlib,json
from pathlib import Path
module_root=Path('/tmp/verify/modules/modules')
expected=json.loads(Path('/kernel/modules.json').read_text())
modules=[]
for item in expected:
    name=item['path'].split('lib/modules/',1)[1];path=module_root/name
    if not path.is_file() or hashlib.sha256(path.read_bytes()).hexdigest()!=item['sha256']:
        raise SystemExit('Root image module mismatch: '+name)
    modules.append({'path':name,'sha256':item['sha256']})
actual={str(p.relative_to(module_root)) for p in module_root.rglob('*.ko')}
if actual!={i['path'] for i in modules}:raise SystemExit('Unexpected root kernel modules')
firmware=json.loads(Path('/firmware/manifest.json').read_text())['files']
for item in firmware:
    p=Path('/tmp/verify/firmware/firmware')/item['path']
    if not p.is_file() or hashlib.sha256(p.read_bytes()).hexdigest()!=item['sha256']:
        raise SystemExit('Root image firmware mismatch: '+item['path'])
if Path('/tmp/verify/quest.service').read_bytes()!=Path('/quest.service').read_bytes():
    raise SystemExit('Root image service mismatch')
Path('/output/contents.json').write_text(json.dumps({'modules':modules,'firmware':firmware,
    'quest_startup_unit_sha256':hashlib.sha256(Path('/tmp/verify/quest.service').read_bytes()).hexdigest()},indent=2)+'\n')
PYVERIFY
rpm -q e2fsprogs systemd > /output/tool-versions.txt
'''


def run(args):
    kernel, initramfs, firmware, output = (p.absolute() for p in (args.kernel, args.initramfs, args.firmware, args.output))
    if output.exists() or output.is_symlink() or any(c in str(p) for p in (kernel, initramfs, firmware, output, ROOT) for c in (",", ":")):
        raise ValueError("Choose a new output directory and paths without commas or colons")
    assembly.root_arguments(args.root_label)
    if not 6 <= args.size_gib <= 32:
        raise ValueError("Root size must be 6–32 GiB")
    build, hashes, version, _ = builder.inputs(kernel)
    fw = builder.firmware.validate(firmware)
    initrd = json.loads((initramfs / "build.json").read_text())
    if (initrd.get("status") != "built" or initrd.get("quest_services") is not True or initrd.get("firmware") != fw or
            initrd.get("kernel_build_sha256") != builder.digest(kernel / "build.json") or
            initrd.get("qemu_transports") is not build["qemu_transports"]):
        raise ValueError("Requires matching kernel, Quest startup initramfs and firmware")
    expected = next(a["sha256"] for a in initrd["artifacts"] if a["path"] == "initramfs.img")
    if builder.digest(initramfs / "initramfs.img") != expected:
        raise ValueError("Initramfs checksum mismatch")
    inputs = {n: builder.digest(ROOT / n) for n in builder.QUEST_INPUTS}
    if initrd.get("quest_inputs_sha256") != inputs:
        raise ValueError("Quest startup sources changed since the initramfs build")
    inputs["tools/build-headset-root.py"] = builder.digest(Path(__file__))
    image = json.loads(subprocess.check_output([args.engine, "image", "inspect", args.image], text=True))[0]
    if image["Architecture"] != "arm64" or image["Os"] != "linux":
        raise ValueError("Root userspace must be Linux ARM64")
    output.mkdir(parents=True)
    report = {"status": "started", "target": "headset-root-offline", "hardware_flash_image": False,
              "hardware_boot_verified": False, "kernel_variant": build["variant"], "qemu_transports": build["qemu_transports"],
              "kernel_build_sha256": builder.digest(kernel / "build.json"), "kernel_inputs_sha256": hashes,
              "initramfs_sha256": expected, "firmware": fw, "container_image": image["Id"], "inputs_sha256": inputs,
              "root_label": args.root_label, "size_gib": args.size_gib, "commands": []}
    container = None; started = time.monotonic()
    try:
        command = [args.engine, "create", image["Id"]]
        report["commands"].append(command)
        container = subprocess.check_output(command, text=True, timeout=30).strip()
        command = [args.engine, "export", container]; report["commands"].append(command)
        with (output / "rootfs.tar").open("xb") as stream:
            subprocess.run(command, stdout=stream, timeout=180, check=True)
        report["export_sha256"] = builder.digest(output / "rootfs.tar")
        command = [args.engine, "run", "--rm", "--init", "--network", "none", "--memory", "2g", "--memory-swap", "2g",
                   "--cpus", "2", "--pids-limit", "256", "-v", f"{output}:/output", "-v", f"{kernel}:/kernel:ro",
                   "-v", f"{firmware}:/firmware:ro", "-v", f"{ROOT / 'system/quest/armada-quest-boot.service'}:/quest.service:ro",
                   "--entrypoint", "timeout", image["Id"], "--kill-after=5", "300", "bash", "-c", SCRIPT,
                   "build-root", version, str(args.size_gib), args.root_label]
        report["commands"].append(command)
        with (output / "build.log").open("wb") as log:
            subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, timeout=320, check=True)
        if builder.inputs(kernel)[1] != hashes or builder.firmware.validate(firmware) != fw:
            raise ValueError("Kernel or firmware changed during root construction")
        if any(builder.digest(ROOT / n) != sha for n, sha in inputs.items()):
            raise ValueError("Root build sources changed during construction")
        report["artifacts"] = [{"path": n, "bytes": (output / n).stat().st_size, "sha256": builder.digest(output / n)}
                               for n in ("rootfs.ext4", "contents.json", "root-settings.json", "filesystem.txt", "fsck.log", "tool-versions.txt")]
        contents = json.loads((output / "contents.json").read_text())
        report["quest_startup_unit_sha256"] = contents["quest_startup_unit_sha256"]
        report.update(status="built", sha256={a["path"]: a["sha256"] for a in report["artifacts"]})
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        report.update(status="failed", error=str(error)); raise
    finally:
        if container:
            subprocess.run([args.engine, "rm", container], check=True, timeout=30)
        report["elapsed_seconds"] = round(time.monotonic() - started, 3)
        (output / "manifest.json").write_text(json.dumps(report, indent=2) + "\n")
    print(output / "manifest.json")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("kernel", type=Path)
    parser.add_argument("initramfs", type=Path)
    parser.add_argument("--firmware", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--image", default="localhost/armada-vr:runtime")
    parser.add_argument("--engine", default=os.environ.get("CONTAINER_ENGINE", "docker"))
    parser.add_argument("--root-label", default="armada-vr-root")
    parser.add_argument("--size-gib", type=int, default=8)
    try:
        run(parser.parse_args())
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        parser.exit(1, f"{error}\n")
