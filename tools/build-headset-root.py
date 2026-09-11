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
spec = importlib.util.spec_from_file_location("turnip_artifact", ROOT / "tools/turnip-artifact.py")
turnip_artifact = importlib.util.module_from_spec(spec)
spec.loader.exec_module(turnip_artifact)

SCRIPT = r'''
set -eu
mkdir -p /tmp/root /tmp/modules /tmp/verify/modules /tmp/verify/firmware
tar -xpf /rootfs.tar -C /tmp/root
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
gpu=json.loads(Path('/gpu-firmware/manifest.json').read_text()) if Path('/gpu-firmware').exists() else None
for item in manifest['files']+(gpu['files'] if gpu else []):
    path=fw/item['path']
    if path.is_symlink() or path.exists():
        raise SystemExit('Base image already supplies selected Quest firmware: '+str(path))
    source='/gpu-firmware/files' if gpu and item in gpu['files'] else '/firmware/files'
    shutil.copy2(Path(source)/item['path'],path)
unit='usr/lib/systemd/system/armada-quest-boot.service'
shutil.copy2('/quest.service',root/unit)
metadata=root/'usr/share/armada-vr';metadata.mkdir(parents=True,exist_ok=True)
shutil.copy2('/kernel/build.json',metadata/'kernel-build.json')
shutil.copy2('/firmware/manifest.json',metadata/'quest-firmware.json')
if gpu:shutil.copy2('/gpu-firmware/manifest.json',metadata/'quest-gpu-firmware.json')
turnip=None
if Path('/output/turnip.json').is_file():
    turnip=json.loads(Path('/output/turnip.json').read_text())
    prefix=root/'opt/armada-vr/turnip'
    if prefix.exists() or prefix.is_symlink():
        raise SystemExit('Base image already supplies Turnip')
    for item in turnip['files']:
        target=root/item['path'];target.parent.mkdir(parents=True,exist_ok=True)
        shutil.copy2(Path('/turnip')/item['source'],target)
    shutil.copy2('/turnip/build.json',metadata/'turnip-build.json')
Path('/output/root-settings.json').write_text(json.dumps({'removed_virtual_paths':removed,'unit_path':unit,
    'unit_sha256':hashlib.sha256((root/unit).read_bytes()).hexdigest(),'hostname':'armada-vr',
    'default_target':'multi-user.target','automatic_lab_tests':False,'automatic_desktop':False,'turnip':turnip,'gpu_firmware':gpu},indent=2)+'\n')
PYROOT
if test -f /output/turnip.json; then
    chroot /tmp/root /usr/bin/ldd -r /opt/armada-vr/turnip/lib64/libvulkan_freedreno.so > /output/turnip-dependencies.txt 2>&1
    chroot /tmp/root /usr/bin/python3 -c 'import ctypes,os; library=ctypes.CDLL("/opt/armada-vr/turnip/lib64/libvulkan_freedreno.so",mode=os.RTLD_NOW); assert library.vk_icdGetInstanceProcAddr; print("Turnip ARM64 RTLD_NOW and ICD entry: passed")' > /output/turnip-loader.txt 2>&1
fi
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
if test -f /output/turnip.json; then
    mkdir /tmp/verify/turnip
    debugfs -R 'rdump /opt/armada-vr/turnip /tmp/verify/turnip' /output/rootfs.ext4
    debugfs -R 'dump /usr/share/armada-vr/turnip-build.json /tmp/verify/turnip-build.json' /output/rootfs.ext4
fi
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
gpu=json.loads(Path('/gpu-firmware/manifest.json').read_text())['files'] if Path('/gpu-firmware').exists() else []
for item in firmware+gpu:
    p=Path('/tmp/verify/firmware/firmware')/item['path']
    if not p.is_file() or hashlib.sha256(p.read_bytes()).hexdigest()!=item['sha256']:
        raise SystemExit('Root image firmware mismatch: '+item['path'])
if Path('/tmp/verify/quest.service').read_bytes()!=Path('/quest.service').read_bytes():
    raise SystemExit('Root image service mismatch')
turnip=None
if Path('/output/turnip.json').is_file():
    turnip=json.loads(Path('/output/turnip.json').read_text())
    for item in turnip['files']:
        path=Path('/tmp/verify/turnip')/item['path'].removeprefix('opt/armada-vr/')
        if not path.is_file() or hashlib.sha256(path.read_bytes()).hexdigest()!=item['sha256']:
            raise SystemExit('Root image Turnip mismatch: '+item['path'])
    if hashlib.sha256(Path('/tmp/verify/turnip-build.json').read_bytes()).hexdigest()!=turnip['build_sha256']:
        raise SystemExit('Root image Turnip build record mismatch')
Path('/output/contents.json').write_text(json.dumps({'modules':modules,'firmware':firmware,'gpu_firmware':gpu,
    'quest_startup_unit_sha256':hashlib.sha256(Path('/tmp/verify/quest.service').read_bytes()).hexdigest(),'turnip':turnip},indent=2)+'\n')
PYVERIFY
rpm -q e2fsprogs systemd > /output/tool-versions.txt
'''


def run(args):
    kernel, initramfs, firmware, output = (p.absolute() for p in (args.kernel, args.initramfs, args.firmware, args.output))
    if output.exists() or output.is_symlink() or any(c in str(p) for p in (kernel, initramfs, firmware, output, ROOT) for c in (",", ":")):
        raise ValueError("Choose a new output directory and paths without commas or colons")
    turnip_dir = args.turnip.absolute() if args.turnip else None
    if turnip_dir and any(c in str(turnip_dir) for c in (",", ":")):
        raise ValueError("Choose a Turnip path without commas or colons")
    turnip = turnip_artifact.validate(turnip_dir) if turnip_dir else None
    gpu_dir = args.gpu_firmware.absolute() if args.gpu_firmware else None
    export_dir = args.reuse_export.absolute() if args.reuse_export else None
    if any(c in str(p) for p in (gpu_dir, export_dir) if p for c in (",", ":")):
        raise ValueError("Choose GPU firmware and export paths without commas or colons")
    gpu = builder.firmware.validate(gpu_dir, "gpu") if gpu_dir else None
    assembly.root_arguments(args.root_label)
    if not 6 <= args.size_gib <= 32:
        raise ValueError("Root size must be 6–32 GiB")
    build, hashes, version, _ = builder.inputs(kernel)
    fw = builder.firmware.validate(firmware)
    if gpu and (gpu["reference_build"] != fw["reference_build"] or gpu["source_image_sha256"] != fw["source_image_sha256"]):
        raise ValueError("GPU and ADSP firmware must come from the same declared reference image")
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
    inputs["tools/turnip-artifact.py"] = builder.digest(ROOT / "tools/turnip-artifact.py")
    inputs["tools/prepare-quest-firmware.py"] = builder.digest(ROOT / "tools/prepare-quest-firmware.py")
    image = json.loads(subprocess.check_output([args.engine, "image", "inspect", args.image], text=True))[0]
    if image["Architecture"] != "arm64" or image["Os"] != "linux":
        raise ValueError("Root userspace must be Linux ARM64")
    export_source = None
    if export_dir:
        export_manifest = json.loads((export_dir / "manifest.json").read_text())
        archive = export_dir / "rootfs.tar"
        if (export_manifest.get("target") != "headset-root-offline" or export_manifest.get("status") != "built" or
                export_manifest.get("container_image") != image["Id"]):
            raise ValueError("Reused export must come from a successful root build with the same immutable userspace image")
        if archive.is_symlink() or not archive.is_file() or builder.digest(archive) != export_manifest.get("export_sha256"):
            raise ValueError("Reused userspace export checksum mismatch")
        export_source = {"path": str(archive), "manifest_sha256": builder.digest(export_dir / "manifest.json"),
                         "sha256": export_manifest["export_sha256"]}
    output.mkdir(parents=True)
    if turnip:
        (output / "turnip.json").write_text(json.dumps(turnip, indent=2) + "\n")
    report = {"status": "started", "turnip": turnip, "gpu_firmware": gpu, "export_source": export_source, "target": "headset-root-offline", "hardware_flash_image": False,
              "hardware_boot_verified": False, "kernel_variant": build["variant"], "qemu_transports": build["qemu_transports"],
              "kernel_build_sha256": builder.digest(kernel / "build.json"), "kernel_inputs_sha256": hashes,
              "initramfs_sha256": expected, "firmware": fw, "container_image": image["Id"], "inputs_sha256": inputs,
              "root_label": args.root_label, "size_gib": args.size_gib, "commands": []}
    container = None; started = time.monotonic()
    try:
        if export_source:
            archive = Path(export_source["path"])
            report["export_sha256"] = export_source["sha256"]
        else:
            command = [args.engine, "create", image["Id"]]
            report["commands"].append(command)
            container = subprocess.check_output(command, text=True, timeout=30).strip()
            command = [args.engine, "export", container]; report["commands"].append(command)
            archive = output / "rootfs.tar"
            with archive.open("xb") as stream:
                subprocess.run(command, stdout=stream, timeout=180, check=True)
            report["export_sha256"] = builder.digest(archive)
        command = [args.engine, "run", "--rm", "--init", "--network", "none", "--memory", "2g", "--memory-swap", "2g",
                   "--cpus", "2", "--pids-limit", "256", "-v", f"{output}:/output", "-v", f"{archive}:/rootfs.tar:ro", "-v", f"{kernel}:/kernel:ro",
                   "-v", f"{firmware}:/firmware:ro", "-v", f"{ROOT / 'system/quest/armada-quest-boot.service'}:/quest.service:ro",
                   *(["-v", f"{gpu_dir}:/gpu-firmware:ro"] if gpu_dir else []),
                   *(["-v", f"{turnip_dir}:/turnip:ro"] if turnip_dir else []),
                   "--entrypoint", "timeout", image["Id"], "--kill-after=5", "300", "bash", "-c", SCRIPT,
                   "build-root", version, str(args.size_gib), args.root_label]
        report["commands"].append(command)
        with (output / "build.log").open("wb") as log:
            subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, timeout=320, check=True)
        if builder.inputs(kernel)[1] != hashes or builder.firmware.validate(firmware) != fw:
            raise ValueError("Kernel or firmware changed during root construction")
        if builder.digest(archive) != report["export_sha256"]:
            raise ValueError("Userspace export changed during construction")
        if export_source and builder.digest(export_dir / "manifest.json") != export_source["manifest_sha256"]:
            raise ValueError("Source export manifest changed during construction")
        if gpu_dir and builder.firmware.validate(gpu_dir, "gpu") != gpu:
            raise ValueError("GPU firmware changed during root construction")
        if turnip_dir and turnip_artifact.validate(turnip_dir) != turnip:
            raise ValueError("Turnip changed during root construction")
        if any(builder.digest(ROOT / n) != sha for n, sha in inputs.items()):
            raise ValueError("Root build sources changed during construction")
        names = ["rootfs.ext4", "contents.json", "root-settings.json", "filesystem.txt", "fsck.log", "tool-versions.txt"]
        if turnip:
            names += ["turnip.json", "turnip-dependencies.txt", "turnip-loader.txt"]
        report["artifacts"] = [{"path": n, "bytes": (output / n).stat().st_size, "sha256": builder.digest(output / n)}
                               for n in names]
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
    parser.add_argument("--turnip", type=Path, help="Verified Mesa Turnip build directory")
    parser.add_argument("--gpu-firmware", type=Path, help="Verified GPU bundle from the same Quest reference image")
    parser.add_argument("--reuse-export", type=Path, help="Reuse rootfs.tar from a successful root build with the same userspace image")
    parser.add_argument("--engine", default=os.environ.get("CONTAINER_ENGINE", "docker"))
    parser.add_argument("--root-label", default="armada-vr-root")
    parser.add_argument("--size-gib", type=int, default=8)
    try:
        run(parser.parse_args())
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        parser.exit(1, f"{error}\n")
