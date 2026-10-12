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


ROOT = Path(__file__).resolve().parents[1]
VOLATILE_ROOT = ROOT / "system/systemd/volatile-root.conf"
PICO_INPUTS = ["system/pico/armada-pico-qrtr", "system/pico/armada-pico-qrtr.service",
               "system/pico/armada-pico-pd-mapper", "system/pico/armada-pico-pd-mapper.service",
               "tools/pico-services.py",
               "system/pico/module-setup.sh", "profiles/qcom-services.json",
               "patches/qrtr/0001-check-qmi-packet-bounds.patch",
               "patches/pd-mapper/0001-report-actual-firmware-path.patch",
               "patches/pd-mapper/0002-explicit-service-maps.patch",
               "patches/pd-mapper/0003-check-firmware-attribute-length.patch"]
QUEST_INPUTS = ["system/quest/armada-quest-boot", "system/quest/armada-quest-boot.service",
                "system/quest/module-setup.sh", "profiles/qcom-services.json",
                "patches/qrtr/0001-check-qmi-packet-bounds.patch",
                "patches/pd-mapper/0001-report-actual-firmware-path.patch",
                "patches/pd-mapper/0002-explicit-service-maps.patch",
                "patches/pd-mapper/0003-check-firmware-attribute-length.patch"]

spec = importlib.util.spec_from_file_location("quest_firmware", Path(__file__).with_name("prepare-quest-firmware.py"))
firmware = importlib.util.module_from_spec(spec)
spec.loader.exec_module(firmware)

spec = importlib.util.spec_from_file_location("pico_firmware", Path(__file__).with_name("prepare-pico-firmware.py"))
pico_firmware = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pico_firmware)

spec = importlib.util.spec_from_file_location("systemd_runtime", Path(__file__).with_name("systemd-runtime.py"))
systemd_runtime = importlib.util.module_from_spec(spec)
spec.loader.exec_module(systemd_runtime)


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


def firmware_input(kernel, quest_directory, pico_directory, quest_services, pico_services=False):
    if pico_services and (quest_services or not pico_directory):
        raise ValueError("Pico QRTR services require Pico firmware without Quest services")
    if quest_directory and pico_directory:
        raise ValueError("Choose one target's firmware bundle")
    if pico_directory:
        profile = json.loads((ROOT / "profiles/kernel/pico-neo3.json").read_text())
        if (quest_services or kernel.get("repository") != profile["repository"] or
                kernel.get("commit") != profile["commit"]):
            raise ValueError("Pico firmware requires the pinned Neo3 kernel without Quest services")
        validator, directory = pico_firmware, pico_directory
    else:
        validator, directory = firmware, quest_directory
        if quest_directory or quest_services:
            profile = json.loads((ROOT / "profiles/kernel/quest3.json").read_text())
            if kernel.get("repository") != profile["repository"] or kernel.get("commit") != profile["commit"]:
                raise ValueError("Quest firmware and services require the pinned Quest kernel")
    manifest = validator.validate(directory) if directory else None
    if quest_services and not manifest:
        raise ValueError("Quest startup services require a verified firmware bundle")
    return directory, manifest, validator


SCRIPT = r'''
set -eu
mkdir /tmp/modules /tmp/dracut-conf /tmp/unpacked
tar -xzf /kernel/modules.tar.gz -C /tmp/modules --no-same-owner
test ! -e "/lib/modules/$1"
cp -a "/tmp/modules/lib/modules/$1" "/lib/modules/$1"
if [ "$3" = yes ]; then
    set -- "$@" --include /firmware/files /usr/lib/firmware
fi
if [ "$4" = yes ] || [ "$5" = yes ]; then
    cmp /qcom-profile.json /usr/share/armada-qcom-services/sources.json
    cmp /qmi-bounds.patch /usr/share/armada-qcom-services/0001-check-qmi-packet-bounds.patch
fi
if [ "$5" = yes ]; then
    python3 - <<'PYPICO'
import importlib.util,json
from pathlib import Path
spec=importlib.util.spec_from_file_location('pico_services','/repo/tools/pico-services.py')
pico=importlib.util.module_from_spec(spec);spec.loader.exec_module(pico)
checksums=pico.service_maps(json.loads(Path('/firmware/manifest.json').read_text()))
if checksums is not None:
    for name,reference in pico.MAPPER_PROVENANCE.items():
        if Path('/'+name).read_bytes()!=Path('/repo',reference).read_bytes():
            raise SystemExit('Pico PD mapper source identity mismatch: '+name)
    Path('/tmp/pico-service-maps.sha256').write_bytes(checksums)
PYPICO
    cp -a /pico /usr/lib/dracut/modules.d/95armada-pico
    set -- "$@" --add armada-pico
fi
if [ "$4" = yes ]; then
    cmp /firmware-path.patch /usr/share/armada-qcom-services/0001-report-actual-firmware-path.patch
    cmp /service-maps.patch /usr/share/armada-qcom-services/0002-explicit-service-maps.patch
    cmp /firmware-read.patch /usr/share/armada-qcom-services/0003-check-firmware-attribute-length.patch
    cp -a /quest /usr/lib/dracut/modules.d/95armada-quest
    python3 - <<'PYFW'
import json
from pathlib import Path
manifest=json.loads(Path('/firmware/manifest.json').read_text())
Path('/tmp/quest-firmware.sha256').write_text(''.join(item['sha256']+'  /usr/lib/firmware/'+item['path']+'\n' for item in manifest['files']))
PYFW
    set -- "$@" --add armada-quest
fi
dracut --kver "$1" --kmoddir "/lib/modules/$1" \
    --kernel-image /kernel/Image --no-hostonly --no-hostonly-cmdline \
    --no-hostonly-default-device --no-early-microcode --reproducible \
    --nostrip --gzip --nofscks --nomdadmconf --nolvmconf \
    --conf /dev/null --confdir /tmp/dracut-conf \
    --include /volatile-root.conf /etc/systemd/system/systemd-volatile-root.service.d/root.conf \
    --modules 'systemd systemd-initrd systemd-udevd systemd-modules-load kernel-modules rootfs-block fs-lib base shutdown dracut-systemd initqueue' \
    --add-drivers "$2" "${@:6}" /output/initramfs.img
cd /tmp/unpacked
lsinitrd --unpack /output/initramfs.img
if test -d /systemd-runtime; then
    python3 /systemd-runtime.py /systemd-runtime --install /tmp/unpacked
    find . -print0 | LC_ALL=C sort -z | cpio --null -o -H newc --reproducible --owner=0:0 | gzip -n -9 > /output/initramfs.img
    mkdir /tmp/verified
    cd /tmp/verified
    lsinitrd --unpack /output/initramfs.img
    python3 /systemd-runtime.py /systemd-runtime --verify /tmp/verified
fi
python3 - /kernel/modules.json /output/contents.json "$3" "$4" "$5" <<'PY'
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
dropin=Path('etc/systemd/system/systemd-volatile-root.service.d/root.conf')
if dropin.is_symlink() or dropin.read_bytes()!=Path('/volatile-root.conf').read_bytes():
    raise SystemExit('Volatile-root mount dependency changed')
required.append(str(dropin))
quest_files=[]
if sys.argv[4]=='yes':
    pairs=[('/quest/armada-quest-boot','usr/libexec/armada-quest-boot'),
           ('/quest/armada-quest-boot.service','usr/lib/systemd/system/armada-quest-boot.service'),
           ('/tmp/quest-firmware.sha256','etc/armada-vr/quest-firmware.sha256'),
           ('/usr/bin/pd-mapper','usr/bin/pd-mapper'),('/usr/bin/qrtr-lookup','usr/bin/qrtr-lookup')]
    for source,name in pairs:
        path=Path(name)
        sha=hashlib.sha256(Path(source).read_bytes()).hexdigest()
        if not path.is_file() or path.is_symlink() or hashlib.sha256(path.read_bytes()).hexdigest()!=sha:
            raise SystemExit('Missing or changed Quest startup file: '+name)
        if name == 'usr/libexec/armada-quest-boot' and not path.stat().st_mode & 0o111:
            raise SystemExit('Quest startup helper is not executable')
        quest_files.append({'path':name,'sha256':sha})
    required += [item['path'] for item in quest_files]
    required += ['usr/bin/systemd-notify','etc/systemd/system/initrd.target.wants/armada-quest-boot.service']
pico_files=[]
if sys.argv[5]=='yes':
    pairs=[('/pico/armada-pico-qrtr','usr/libexec/armada-pico-qrtr'),
           ('/pico/armada-pico-qrtr.service','usr/lib/systemd/system/armada-pico-qrtr.service'),
           ('/usr/bin/qrtr-ns','usr/bin/qrtr-ns'),('/usr/bin/qrtr-lookup','usr/bin/qrtr-lookup')]
    if Path('/tmp/pico-service-maps.sha256').is_file():
        pairs += [('/pico/armada-pico-pd-mapper','usr/libexec/armada-pico-pd-mapper'),
                  ('/pico/armada-pico-pd-mapper.service','usr/lib/systemd/system/armada-pico-pd-mapper.service'),
                  ('/tmp/pico-service-maps.sha256','etc/armada-vr/pico-service-maps.sha256'),
                  ('/usr/bin/pd-mapper','usr/bin/pd-mapper')]
        required += ['usr/bin/sha256sum','etc/systemd/system/initrd.target.wants/armada-pico-pd-mapper.service']
    for source,name in pairs:
        path=Path(name)
        sha=hashlib.sha256(Path(source).read_bytes()).hexdigest()
        if not path.is_file() or path.is_symlink() or hashlib.sha256(path.read_bytes()).hexdigest()!=sha:
            raise SystemExit('Missing or changed Pico startup file: '+name)
        if name.startswith(('usr/bin/','usr/libexec/')) and not path.stat().st_mode & 0o111:
            raise SystemExit('Pico startup executable has no execute permission: '+name)
        pico_files.append({'path':name,'sha256':sha})
    required += [item['path'] for item in pico_files]
    required += ['usr/bin/systemd-notify','etc/systemd/system/initrd.target.wants/armada-pico-qrtr.service']
for name in required:
    path=Path(name)
    if path.is_symlink() and path.readlink().is_absolute():
        path=Path(str(path.readlink()).lstrip('/'))
    if not path.is_file() or not path.resolve().is_relative_to(Path.cwd()):
        raise SystemExit('Missing initramfs component: '+name)
Path(sys.argv[2]).write_text(json.dumps({'module_count':len(records),'modules':records,'required_files':required,'firmware':firmware_files,'quest_services':quest_files,'pico_services':pico_files},indent=2)+'\n')
PY
lsinitrd -m /output/initramfs.img > /output/dracut-modules.txt
rpm -q dracut systemd kmod > /output/tool-versions.txt
'''


def run(args):
    kernel, output = args.kernel.resolve(), args.output.resolve()
    report, hashes, version, names = inputs(kernel)
    runtime_dir = args.systemd_runtime.absolute() if args.systemd_runtime else None
    runtime = systemd_runtime.validate(runtime_dir) if runtime_dir else None
    runtime_sha = digest(runtime_dir / "manifest.json") if runtime_dir else None
    runtime_helper_sha = digest(Path(systemd_runtime.__file__)) if runtime_dir else None
    if runtime_dir and any(c in str(runtime_dir) for c in (",", ":")):
        raise ValueError("Systemd runtime path must not contain commas or colons")
    firmware_dir, firmware_manifest, validator = firmware_input(
        report, args.firmware.absolute() if args.firmware else None,
        args.pico_firmware.absolute() if args.pico_firmware else None, args.quest_services, args.pico_services)
    if args.pico_services and not runtime_dir:
        raise ValueError("Pico QRTR services require the compatible initramfs systemd runtime")
    pico_hashes = {name: digest(ROOT / name) for name in PICO_INPUTS} if args.pico_services else {}
    quest_hashes = {name: digest(ROOT / name) for name in QUEST_INPUTS} if args.quest_services else {}
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
              "volatile_root_dependency_sha256": digest(VOLATILE_ROOT),
              "firmware": firmware_manifest, "quest_services": args.quest_services, "quest_inputs_sha256": quest_hashes,
              "pico_services": args.pico_services, "pico_inputs_sha256": pico_hashes,
              "firmware_validator_sha256": digest(Path(validator.__file__)),
              "qcom_firmware_helper_sha256": digest(Path(firmware.__file__)),
              "pico_dsp_helper_sha256": digest(Path(pico_firmware.dsp.__file__)) if args.pico_firmware else None,
              "systemd_runtime_sha256": runtime_sha, "systemd_runtime_helper_sha256": runtime_helper_sha}
    command = [args.engine, "run", "--rm", "--init", "--network", "none", "--memory", "2g", "--memory-swap", "2g",
               "--cpus", "2", "--pids-limit", "256",
               "--tmpfs", "/tmp:rw,exec,size=512m", "--tmpfs", "/var/tmp:rw,exec,size=512m",
               "--tmpfs", "/usr/lib/modules:rw,nosuid,size=128m",
               "-e", "SOURCE_DATE_EPOCH=0", "-e", "PYTHONDONTWRITEBYTECODE=1",
               "-v", f"{kernel}:/kernel:ro", "-v", f"{output}:/output",
               "-v", f"{VOLATILE_ROOT}:/volatile-root.conf:ro"]
    if runtime_dir:
        command.extend(["-v", f"{runtime_dir}:/systemd-runtime:ro",
                        "-v", f"{Path(systemd_runtime.__file__)}:/systemd-runtime.py:ro"])
    if firmware_dir:
        command.extend(["-v", f"{firmware_dir}:/firmware:ro"])
    if args.pico_services:
        for source, target in [("system/pico", "/pico"), ("profiles/qcom-services.json", "/qcom-profile.json"),
                               ("tools/pico-services.py", "/repo/tools/pico-services.py"),
                               ("patches/pd-mapper", "/repo/patches/pd-mapper"),
                               ("patches/qrtr/0001-check-qmi-packet-bounds.patch", "/qmi-bounds.patch")]:
            command.extend(["-v", f"{ROOT / source}:{target}:ro"])
    if args.quest_services:
        for source, target in [("system/quest", "/quest"), ("profiles/qcom-services.json", "/qcom-profile.json"),
                               ("patches/qrtr/0001-check-qmi-packet-bounds.patch", "/qmi-bounds.patch"),
                                ("patches/pd-mapper/0001-report-actual-firmware-path.patch", "/firmware-path.patch"),
                                ("patches/pd-mapper/0002-explicit-service-maps.patch", "/service-maps.patch"),
                                ("patches/pd-mapper/0003-check-firmware-attribute-length.patch", "/firmware-read.patch")]:
            command.extend(["-v", f"{ROOT / source}:{target}:ro"])
    command.extend(["--entrypoint", "timeout", image, "--kill-after=5", "240", "bash", "-c", SCRIPT,
                    "build-initramfs", version, " ".join(names), "yes" if firmware_dir else "no", "yes" if args.quest_services else "no", "yes" if args.pico_services else "no"])
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
        if firmware_dir and validator.validate(firmware_dir) != firmware_manifest:
            raise ValueError("Firmware inputs changed during initramfs construction")
        if (digest(Path(validator.__file__)) != result["firmware_validator_sha256"] or
                digest(Path(firmware.__file__)) != result["qcom_firmware_helper_sha256"]):
            raise ValueError("Firmware validators changed during initramfs construction")
        if args.pico_firmware and digest(Path(pico_firmware.dsp.__file__)) != result["pico_dsp_helper_sha256"]:
            raise ValueError("Pico DSP validator changed during initramfs construction")
        if any(digest(ROOT / name) != sha for name, sha in pico_hashes.items()):
            raise ValueError("Pico startup inputs changed during construction")
        if any(digest(ROOT / name) != sha for name, sha in quest_hashes.items()):
            raise ValueError("Quest startup inputs changed during construction")
        if digest(VOLATILE_ROOT) != result["volatile_root_dependency_sha256"]:
            raise ValueError("Volatile-root dependency changed during construction")
        if runtime_dir and (systemd_runtime.validate(runtime_dir) != runtime or
                            digest(runtime_dir / "manifest.json") != runtime_sha or
                            digest(Path(systemd_runtime.__file__)) != runtime_helper_sha):
            raise ValueError("Systemd runtime inputs changed during construction")
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
    parser.add_argument("--systemd-runtime", type=Path, help="prepared private systemd runtime matching this initramfs template")
    group = parser.add_mutually_exclusive_group()
    group.add_argument("--firmware", type=Path, help="optional prepared Quest ADSP firmware directory")
    group.add_argument("--pico-firmware", type=Path, help="prepared Neo3 GPU or GPU/DSP firmware; requires the Pico kernel")
    parser.add_argument("--pico-services", action="store_true", help="include bounded Pico QRTR startup; requires Pico firmware, compatible systemd and the QCOM service image")
    parser.add_argument("--quest-services", action="store_true", help="include bounded Quest ADSP/USB startup; requires --firmware and the QCOM service image")
    parser.add_argument("--image", default="localhost/armada-vr:vm")
    parser.add_argument("--engine", default=os.environ.get("CONTAINER_ENGINE", "docker"))
    try:
        run(parser.parse_args())
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        parser.exit(1, f"{error}\n")
