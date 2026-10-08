#!/usr/bin/env python3
"""Build an offline ext4 root from pinned ARM64 userspace and matching headset inputs."""
import argparse
import hashlib
import importlib.util
import json
import os
import re
import shutil
from pathlib import Path
import subprocess
import tempfile
import threading
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
spec = importlib.util.spec_from_file_location("monado_artifact", ROOT / "tools/monado-artifact.py")
monado_artifact = importlib.util.module_from_spec(spec)
spec.loader.exec_module(monado_artifact)

SCRIPT = r'''
set -eu
mkdir -p /tmp/root /tmp/modules /tmp/verify/modules /tmp/verify/firmware
tar -xpf /rootfs.tar -C /tmp/root
if test -d /runtime-rpms; then
    test -x /tmp/root/usr/lib/systemd/systemd
    test ! -e /tmp/root/tmp/armada-vr-rpms
    mkdir /tmp/root/tmp/armada-vr-rpms
    cp /runtime-rpms/*.rpm /tmp/root/tmp/armada-vr-rpms/
    chroot /tmp/root /bin/sh -ec '
        export LC_ALL=C
        for package in /tmp/armada-vr-rpms/*.rpm; do
            rpmkeys --checksig --verbose "$package" > /tmp/armada-vr-rpms/signature.txt
            cat /tmp/armada-vr-rpms/signature.txt
            grep -Ei "signature.*: OK$" /tmp/armada-vr-rpms/signature.txt >/dev/null
        done
        rpm --upgrade --test /tmp/armada-vr-rpms/*.rpm
        rpm --upgrade /tmp/armada-vr-rpms/*.rpm
        ldconfig
        rpm -qp --qf "%{NAME}-%{VERSION}-%{RELEASE}.%{ARCH}\n" /tmp/armada-vr-rpms/*.rpm
    ' > /output/runtime-rpms-install.txt 2>&1
    rm -rf /tmp/root/tmp/armada-vr-rpms
fi
tar -xzf /kernel/modules.tar.gz -C /tmp/modules --no-same-owner
python3 - "$1" <<'PYROOT'
import hashlib,importlib.util,json,shutil,sys
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
if Path('/output/runtime-rpms.json').exists():
    shutil.copy2('/output/runtime-rpms.json',metadata/'runtime-rpms.json')
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
monado=None
if Path('/output/monado.json').is_file():
    monado=json.loads(Path('/output/monado.json').read_text())
    spec=importlib.util.spec_from_file_location('monado','/monado-artifact.py')
    artifact=importlib.util.module_from_spec(spec);spec.loader.exec_module(artifact)
    artifact.install('/monado',root,monado)
Path('/output/root-settings.json').write_text(json.dumps({'removed_virtual_paths':removed,'unit_path':unit,
    'unit_sha256':hashlib.sha256((root/unit).read_bytes()).hexdigest(),'hostname':'armada-vr',
    'default_target':'multi-user.target','automatic_lab_tests':False,'automatic_desktop':False,'turnip':turnip,'monado':monado,'gpu_firmware':gpu},indent=2)+'\n')
PYROOT
if test -f /output/turnip.json; then
    chroot /tmp/root /usr/bin/ldd -r /opt/armada-vr/turnip/lib64/libvulkan_freedreno.so > /output/turnip-dependencies.txt 2>&1
    chroot /tmp/root /usr/bin/python3 -c 'import ctypes,os; library=ctypes.CDLL("/opt/armada-vr/turnip/lib64/libvulkan_freedreno.so",mode=os.RTLD_NOW); assert library.vk_icdGetInstanceProcAddr; print("Turnip ARM64 RTLD_NOW and ICD entry: passed")' > /output/turnip-loader.txt 2>&1
fi
if test -f /output/monado.json; then
    chroot /tmp/root /usr/bin/env LD_LIBRARY_PATH=/opt/armada-vr/monado/lib64 /usr/bin/ldd -r /opt/armada-vr/monado/bin/monado-service > /output/monado-dependencies.txt 2>&1
    python3 -c 'from pathlib import Path; data=Path("/output/monado-dependencies.txt").read_text(); assert "not found" not in data and "undefined symbol" not in data, data'
    chroot /tmp/root /usr/bin/env LD_LIBRARY_PATH=/opt/armada-vr/monado/lib64 /usr/bin/python3 -c 'import ctypes,os; [ctypes.CDLL("/opt/armada-vr/monado/lib64/"+name,mode=os.RTLD_NOW) for name in ("libopenxr_monado.so","libmonado.so")]; print("Monado ARM64 client libraries RTLD_NOW: passed")' > /output/monado-loader.txt 2>&1
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
if test -f /output/monado.json; then
    mkdir -p /tmp/verify/monado-root/opt/armada-vr /tmp/verify/monado-root/usr/share/armada-vr
    debugfs -R 'rdump /opt/armada-vr/monado /tmp/verify/monado-root/opt/armada-vr' /output/rootfs.ext4
    debugfs -R 'dump /usr/share/armada-vr/monado-build.json /tmp/verify/monado-root/usr/share/armada-vr/monado-build.json' /output/rootfs.ext4
fi
python3 - <<'PYVERIFY'
import hashlib,importlib.util,json
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
monado=None
if Path('/output/monado.json').is_file():
    monado=json.loads(Path('/output/monado.json').read_text())
    spec=importlib.util.spec_from_file_location('monado','/monado-artifact.py')
    artifact=importlib.util.module_from_spec(spec);spec.loader.exec_module(artifact)
    artifact.verify_install('/tmp/verify/monado-root',monado)
Path('/output/contents.json').write_text(json.dumps({'modules':modules,'firmware':firmware,'gpu_firmware':gpu,
    'quest_startup_unit_sha256':hashlib.sha256(Path('/tmp/verify/quest.service').read_bytes()).hexdigest(),'turnip':turnip,'monado':monado},indent=2)+'\n')
PYVERIFY
rpm -q e2fsprogs systemd > /output/tool-versions.txt
'''


def export_digest(path, compressed=False, limit=32 * 1024**3):
    if path.is_symlink() or not path.is_file():
        raise ValueError("Expected a regular file: " + str(path))
    process = watchdog = None
    with tempfile.TemporaryFile() as errors:
        if compressed:
            process = subprocess.Popen(["zstd", "-dc", "--memory=128MB", "--", str(path)],
                                       stdout=subprocess.PIPE, stderr=errors)
            watchdog = threading.Timer(120, process.kill)
            watchdog.daemon = True
            watchdog.start()
            stream = process.stdout
        else:
            stream = path.open("rb")
        try:
            digest, size = hashlib.sha256(), 0
            while block := stream.read(1024**2):
                size += len(block)
                if size > limit:
                    raise ValueError("Userspace export exceeds the 32 GiB expanded-size limit")
                digest.update(block)
            if process and process.wait(timeout=5):
                errors.seek(0)
                raise ValueError("Cannot decompress userspace export: " + errors.read(4096).decode(errors="replace"))
            return digest.hexdigest(), size
        finally:
            stream.close()
            if watchdog:
                watchdog.cancel()
            if process and process.poll() is None:
                process.kill()
                process.wait(timeout=5)


def read_export(directory, expected_image=None):
    manifest_path = directory / "manifest.json"
    manifest_sha = builder.digest(manifest_path)
    manifest = json.loads(manifest_path.read_text())
    if not isinstance(manifest, dict):
        raise ValueError("Requires a root export manifest object")
    image = manifest.get("container_image", "")
    expected = manifest.get("export_sha256", "")
    if (manifest.get("target") != "headset-root-offline" or manifest.get("status") != "built" or
            not isinstance(image, str) or not re.fullmatch(r"sha256:[0-9a-f]{64}", image) or
            not isinstance(expected, str) or not re.fullmatch(r"[0-9a-f]{64}", expected)):
        raise ValueError("Requires a successful root build with immutable userspace and export identities")
    if expected_image is not None and image != expected_image:
        raise ValueError("Reused export must have the same immutable userspace image")
    archive = directory / "rootfs.tar"
    if not archive.exists() and not archive.is_symlink():
        archive = directory / "rootfs.tar.zst"
    archive_sha = builder.digest(archive)
    compressed = archive.name.endswith(".zst")
    digest, size = export_digest(archive, compressed)
    if digest != expected or not size:
        raise ValueError("Reused userspace export checksum mismatch")
    if builder.digest(archive) != archive_sha or builder.digest(manifest_path) != manifest_sha:
        raise ValueError("Userspace export changed during validation")
    return {"path": str(archive), "manifest_sha256": manifest_sha, "sha256": digest,
            "archive_sha256": archive_sha, "expanded_bytes": size, "compressed": compressed,
            "container_image": image}


def read_rpms(directory):
    manifest_path = directory / "manifest.json"
    manifest_sha = builder.digest(manifest_path)
    manifest = json.loads(manifest_path.read_text())
    if not isinstance(manifest, dict):
        raise ValueError("Requires a runtime RPM manifest object")
    packages = manifest.get("packages")
    if manifest.get("schema_version") != 1 or not isinstance(packages, list) or not 1 <= len(packages) <= 128:
        raise ValueError("Requires a version 1 runtime RPM manifest with 1–128 packages")
    names, size = set(), 0
    for item in packages:
        if not isinstance(item, dict):
            raise ValueError("Invalid runtime RPM identity")
        name, sha = item.get("path", ""), item.get("sha256", "")
        if (not isinstance(name, str) or not re.fullmatch(r"[A-Za-z0-9_+.-]+\.rpm", name) or
                name in names or not isinstance(sha, str) or not re.fullmatch(r"[0-9a-f]{64}", sha)):
            raise ValueError("Invalid or duplicate runtime RPM identity")
        path = directory / name
        if builder.digest(path) != sha:
            raise ValueError("Runtime RPM checksum mismatch: " + name)
        size += path.stat().st_size
        names.add(name)
    if size > 1024**3 or {p.name for p in directory.iterdir()} != names | {"manifest.json"}:
        raise ValueError("Runtime RPM directory has extra files or exceeds 1 GiB")
    return {"manifest_sha256": manifest_sha, "packages": packages, "bytes": size}


def storage_required(size_gib, userspace_bytes, new_export, rpm_bytes=0):
    return {"root_bytes": size_gib * 1024**3,
            "export_bytes": userspace_bytes if new_export else 0,
            "scratch_bytes": userspace_bytes + 4 * rpm_bytes + 1024**3,
            "headroom_bytes": 1024**3}


def run(args):
    kernel, initramfs, firmware, output = (p.absolute() for p in (args.kernel, args.initramfs, args.firmware, args.output))
    if output.exists() or output.is_symlink() or any(c in str(p) for p in (kernel, initramfs, firmware, output, ROOT) for c in (",", ":")):
        raise ValueError("Choose a new output directory and paths without commas or colons")
    turnip_dir = args.turnip.absolute() if args.turnip else None
    if turnip_dir and any(c in str(turnip_dir) for c in (",", ":")):
        raise ValueError("Choose a Turnip path without commas or colons")
    turnip = turnip_artifact.validate(turnip_dir) if turnip_dir else None
    monado_dir = args.monado.absolute() if args.monado else None
    if monado_dir and any(c in str(monado_dir) for c in (",", ":")):
        raise ValueError("Choose a Monado path without commas or colons")
    monado = monado_artifact.package(monado_dir) if monado_dir else None
    gpu_dir = args.gpu_firmware.absolute() if args.gpu_firmware else None
    if args.reuse_export and args.userspace_export:
        raise ValueError("Choose one export source")
    export_dir = (args.reuse_export or args.userspace_export)
    export_dir = export_dir.absolute() if export_dir else None
    rpm_dir = args.runtime_rpms.absolute() if args.runtime_rpms else None
    if any(c in str(p) for p in (gpu_dir, export_dir, rpm_dir) if p for c in (",", ":")):
        raise ValueError("Choose firmware, export and RPM paths without commas or colons")
    rpms = read_rpms(rpm_dir) if rpm_dir else None
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
    inputs["tools/monado-artifact.py"] = builder.digest(ROOT / "tools/monado-artifact.py")
    inputs["tools/turnip-artifact.py"] = builder.digest(ROOT / "tools/turnip-artifact.py")
    inputs["tools/prepare-quest-firmware.py"] = builder.digest(ROOT / "tools/prepare-quest-firmware.py")
    image = json.loads(subprocess.check_output([args.engine, "image", "inspect", args.image], text=True))[0]
    if image["Architecture"] != "arm64" or image["Os"] != "linux":
        raise ValueError("Root build image must be Linux ARM64")
    export_source = None
    if export_dir:
        export_source = read_export(export_dir, image["Id"] if args.reuse_export else None)
    output.mkdir(parents=True)
    if turnip:
        (output / "turnip.json").write_text(json.dumps(turnip, indent=2) + "\n")
    if monado:
        (output / "monado.json").write_text(json.dumps(monado, indent=2) + "\n")
    if rpms:
        (output / "runtime-rpms.json").write_text(json.dumps(rpms, indent=2) + "\n")
    report = {"status": "started", "turnip": turnip, "monado": monado, "gpu_firmware": gpu, "export_source": export_source, "target": "headset-root-offline", "hardware_flash_image": False,
              "hardware_boot_verified": False, "kernel_variant": build["variant"], "qemu_transports": build["qemu_transports"],
              "kernel_build_sha256": builder.digest(kernel / "build.json"), "kernel_inputs_sha256": hashes,
              "initramfs_sha256": expected, "firmware": fw,
              "container_image": export_source["container_image"] if export_source else image["Id"],
              "build_container_image": image["Id"], "runtime_rpms": rpms, "inputs_sha256": inputs,
              "root_label": args.root_label, "size_gib": args.size_gib, "commands": []}
    container = None; started = time.monotonic()
    try:
        if monado_dir and not args.userspace_export and not rpms:
            probe = """import ctypes, os, subprocess
prefix='/opt/armada-vr/monado'
data=subprocess.check_output(['ldd','-r',prefix+'/bin/monado-service'],text=True,stderr=subprocess.STDOUT)
print(data,flush=True)
if 'not found' in data or 'undefined symbol' in data:
    raise SystemExit('Monado runtime image has unresolved dependencies')
for name in ('libopenxr_monado.so','libmonado.so'):
    ctypes.CDLL(prefix+'/lib64/'+name,mode=os.RTLD_NOW)
print('Monado runtime image preflight: passed')
"""
            command = [args.engine, "run", "--rm", "--read-only", "--network", "none",
                       "--memory", "256m", "--memory-swap", "256m", "--cpus", "1", "--pids-limit", "64",
                       "-v", f"{monado_dir / monado_artifact.PREFIX}:/opt/armada-vr/monado:ro",
                       "-e", "LD_LIBRARY_PATH=/opt/armada-vr/monado/lib64",
                       "--entrypoint", "python3", image["Id"], "-B", "-c", probe]
            report["commands"].append(command)
            with (output / "monado-preflight.txt").open("wb") as log:
                subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=30)
        storage = storage_required(args.size_gib, export_source["expanded_bytes"] if export_source else image["Size"],
                                   not export_source, rpms["bytes"] if rpms else 0)
        required = sum(storage.values())
        available = shutil.disk_usage(output).free
        report["storage_preflight"] = {"available_bytes": available, "required_working_bytes": required,
                                       "container_scratch_included": True, "estimates": storage}
        if available < required:
            raise ValueError(f"Root assembly requires at least {required / 1024**3:.2f} GiB free including estimated scratch; "
                             f"only {available / 1024**3:.2f} GiB is available")
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
                   *(["-v", f"{rpm_dir}:/runtime-rpms:ro"] if rpm_dir else []),
                   *(["-v", f"{turnip_dir}:/turnip:ro"] if turnip_dir else []),
                   *(["-v", f"{monado_dir}:/monado:ro", "-v", f"{ROOT / 'tools/monado-artifact.py'}:/monado-artifact.py:ro"] if monado_dir else []),
                   "--entrypoint", "timeout", image["Id"], "--kill-after=5", "300", "bash", "-c", SCRIPT,
                   "build-root", version, str(args.size_gib), args.root_label]
        report["commands"].append(command)
        with (output / "build.log").open("wb") as log:
            subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, timeout=320, check=True)
        if builder.inputs(kernel)[1] != hashes or builder.firmware.validate(firmware) != fw:
            raise ValueError("Kernel or firmware changed during root construction")
        if builder.digest(archive) != (export_source["archive_sha256"] if export_source else report["export_sha256"]):
            raise ValueError("Userspace export changed during construction")
        if export_source and builder.digest(export_dir / "manifest.json") != export_source["manifest_sha256"]:
            raise ValueError("Source export manifest changed during construction")
        if gpu_dir and builder.firmware.validate(gpu_dir, "gpu") != gpu:
            raise ValueError("GPU firmware changed during root construction")
        if turnip_dir and turnip_artifact.validate(turnip_dir) != turnip:
            raise ValueError("Turnip changed during root construction")
        if monado_dir and monado_artifact.package(monado_dir) != monado:
            raise ValueError("Monado changed during root construction")
        if rpm_dir and read_rpms(rpm_dir) != rpms:
            raise ValueError("Runtime RPMs changed during root construction")
        if any(builder.digest(ROOT / n) != sha for n, sha in inputs.items()):
            raise ValueError("Root build sources changed during construction")
        names = ["rootfs.ext4", "contents.json", "root-settings.json", "filesystem.txt", "fsck.log", "tool-versions.txt"]
        if turnip:
            names += ["turnip.json", "turnip-dependencies.txt", "turnip-loader.txt"]
        if monado:
            names += ["monado.json", "monado-dependencies.txt", "monado-loader.txt"]
            if (output / "monado-preflight.txt").exists():
                names.append("monado-preflight.txt")
        if rpms:
            names += ["runtime-rpms.json", "runtime-rpms-install.txt"]
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
    parser.add_argument("--monado", type=Path, help="Verified native Monado build directory")
    parser.add_argument("--gpu-firmware", type=Path, help="Verified GPU bundle from the same Quest reference image")
    parser.add_argument("--reuse-export", type=Path, help="Reuse rootfs.tar from a successful root build with the same userspace image")
    parser.add_argument("--userspace-export", type=Path, help="Use a verified prior root export (tar or tar.zst); --image supplies only build tools")
    parser.add_argument("--runtime-rpms", type=Path, help="Manifest-pinned RPMs; signatures and dependencies must validate inside the selected userspace")
    parser.add_argument("--engine", default=os.environ.get("CONTAINER_ENGINE", "docker"))
    parser.add_argument("--root-label", default="armada-vr-root")
    parser.add_argument("--size-gib", type=int, default=8)
    try:
        run(parser.parse_args())
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        parser.exit(1, f"{error}\n")
