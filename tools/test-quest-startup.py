#!/usr/bin/env python3
"""Test Quest initramfs startup and root handoff using modeled hardware in QEMU."""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import platform
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]


def load(name, filename):
    spec = importlib.util.spec_from_file_location(name, ROOT / "tools" / filename)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


builder = load("startup_builder", "build-headset-initramfs.py")
sources = load("startup_sources", "fetch-qcom-services.py")
assembly = load("startup_assembly", "assemble-quest-boot.py")

SCRIPT = r'''
set -eu
cmp /profile.json /usr/share/armada-qcom-services/sources.json
cmp /bounds.patch /usr/share/armada-qcom-services/0001-check-qmi-packet-bounds.patch
cmp /path.patch /usr/share/armada-qcom-services/0001-report-actual-firmware-path.patch
mkdir /tmp/pd-mapper
tar -xzf /sources/pd-mapper.tar.gz -C /tmp/pd-mapper --strip-components=1
gcc -O2 -Wall -Wextra -Werror -I/tmp/pd-mapper /probe.c /tmp/pd-mapper/servreg_loc.c -lqrtr -o /output/qmi-probe
mkdir -p /output/extra/etc/systemd/system/armada-quest-boot.service.d
cp /fixture/fixture.sh /output/extra/quest-fixture
cp /output/qmi-probe /output/extra/qmi-probe
chmod 755 /output/extra/quest-fixture
cp /fixture/fixture.service /output/extra/etc/systemd/system/armada-quest-fixture.service
printf '[Unit]\nWants=armada-quest-fixture.service\nAfter=armada-quest-fixture.service\n' > /output/extra/etc/systemd/system/armada-quest-boot.service.d/fixture.conf
python3 - <<'PYLIB'
import re,shutil,subprocess
from pathlib import Path
root=Path('/output/extra')
binaries=['/usr/bin/cp','/usr/bin/ln','/usr/bin/systemctl','/usr/bin/mount','/usr/bin/sleep',
          '/usr/bin/cmp','/usr/bin/chmod','/usr/bin/stat']
for binary in binaries:
    dest=root/binary.lstrip('/');dest.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(binary,dest)
for binary in binaries+['/output/qmi-probe']:
    data=subprocess.check_output(['ldd',binary],text=True)
    if 'not found' in data:
        raise SystemExit('Missing library: '+binary)
    for name in re.findall(r'(/[^\s()]+)',data):
        original=Path(name);path=original.parent.resolve()/original.name
        dest=root/str(path).lstrip('/');dest.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(path,dest)
PYLIB
cp /fixture/handoff-install.sh /output/extra/quest-handoff-install
cp /fixture/root-check.sh /output/extra/quest-root-check
cp /fixture/root-check.service /output/extra/quest-root-check.service
chmod 755 /output/extra/quest-handoff-install /output/extra/quest-root-check
cp /fixture/handoff-install.service /output/extra/etc/systemd/system/quest-handoff-install.service
'''


def run(args):
    kernel, initramfs, source_dir = (p.absolute() for p in (args.kernel, args.initramfs, args.sources))
    rootfs = args.rootfs.absolute() if args.rootfs else None
    needs_root = not args.case or any(name in ("root", "root-fault") for name in args.case)
    if needs_root and rootfs is None:
        raise ValueError("Root handoff cases require --rootfs; diskless cases must be selected explicitly")
    output = args.output.absolute()
    if output.exists() or output.is_symlink() or any(c in str(p) for p in (kernel, initramfs, source_dir, rootfs, output, ROOT) if p for c in (",", ":")):
        raise ValueError("Choose a new output directory and paths without commas or colons")
    build, hashes, _, _ = builder.inputs(kernel)
    initrd = json.loads((initramfs / "build.json").read_text())
    if build["qemu_transports"] is not True or initrd.get("qemu_transports") is not True:
        raise ValueError("Only QEMU variants are accepted; physical headset artifacts are refused")
    if (initrd.get("status") != "built" or initrd.get("quest_services") is not True or not initrd.get("firmware") or
            initrd.get("kernel_build_sha256") != builder.digest(kernel / "build.json")):
        raise ValueError("Requires matching QEMU initramfs with Quest services and firmware")
    expected = next(item["sha256"] for item in initrd["artifacts"] if item["path"] == "initramfs.img")
    if builder.digest(initramfs / "initramfs.img") != expected:
        raise ValueError("Initramfs checksum mismatch")
    current = {name: builder.digest(ROOT / name) for name in builder.QUEST_INPUTS}
    if initrd.get("quest_inputs_sha256") != current:
        raise ValueError("Initramfs Quest sources differ from the current tree")
    manifest, packaged, root_hash = {}, False, None
    if rootfs:
        manifest = json.loads((rootfs / "manifest.json").read_text())
        if manifest.get("target") not in ("qemu-arm64", "headset-root-offline") or manifest.get("hardware_flash_image") is not False:
            raise ValueError("Requires a QEMU-only lab root manifest")
        packaged = manifest["target"] == "headset-root-offline"
        if packaged and (manifest.get("status") != "built" or manifest.get("qemu_transports") is not True or
                         manifest.get("kernel_build_sha256") != builder.digest(kernel / "build.json") or
                         manifest.get("initramfs_sha256") != expected or
                         manifest.get("quest_startup_unit_sha256") != current["system/quest/armada-quest-boot.service"]):
            raise ValueError("Packaged root does not match the QEMU kernel and startup initramfs")
        root_hash = builder.digest(rootfs / "rootfs.ext4")
        if root_hash != manifest["sha256"]["rootfs.ext4"]:
            raise ValueError("Root filesystem checksum mismatch")
    profile = sources.verify(source_dir)
    image = subprocess.check_output([args.engine, "image", "inspect", args.image, "--format", "{{.Id}}"], text=True).strip()
    if image != initrd["container_image"]:
        raise ValueError("Test image must match the initramfs builder image")
    fixtures = ROOT / "tests/quest-startup"
    files = ["tools/test-quest-startup.py", "tools/build-headset-initramfs.py", "tools/assemble-quest-boot.py",
             "tests/qcom-services/probe.c", *builder.QUEST_INPUTS]
    files += [str(p.relative_to(ROOT)) for p in sorted(fixtures.iterdir()) if p.is_file()]
    report = {"status": "started", "scope": "real systemd, vendor modules and QRTR; modeled Quest hardware state",
              "root_handoff_requested": needs_root,
              "firmware_executed": False, "hardware_boot_verified": False, "container_image": image,
              "kernel_sha256": hashes["Image"], "rootfs_sha256": root_hash, "base_initramfs_sha256": expected,
              "sources": profile, "packaged_root": packaged, "inputs_sha256": {n: builder.digest(ROOT / n) for n in files}, "cases": []}
    output.mkdir(parents=True)

    def container(script, logfile):
        command = [args.engine, "run", "--rm", "--init", "--network", "none", "--memory", "512m",
                   "--memory-swap", "512m", "--cpus", "2", "--pids-limit", "64"]
        for host, guest in [(source_dir, "/sources:ro"), (fixtures, "/fixture:ro"),
                            (ROOT / "tests/qcom-services/probe.c", "/probe.c:ro"),
                            (ROOT / "profiles/qcom-services.json", "/profile.json:ro"),
                            (ROOT / "patches/qrtr/0001-check-qmi-packet-bounds.patch", "/bounds.patch:ro"),
                            (ROOT / "patches/pd-mapper/0001-report-actual-firmware-path.patch", "/path.patch:ro"),
                            (output, "/output")]:
            command.extend(["-v", f"{host}:{guest}"])
        command.extend(["--entrypoint", "timeout", image, "--kill-after=5", "60", "bash", "-c", script])
        with logfile.open("wb") as log:
            subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, timeout=80, check=True)

    try:
        container(SCRIPT, output / "build.log")
        for name, required in [("identity", "requires Quest Eureka device tree"), ("ready", "ARMADA_QUEST_MODEL_QMI_PASS"),
                               ("missing-firmware", "firmware checksum mismatch"), ("repeated-request", "ADSP boot was already requested"),
                               ("missing-mapper", "startup failed: mapper: timed out"), ("missing-host-role", "startup failed: usb-role: timed out"),
                               ("root", "ARMADA_QUEST_ROOT_RESTART_REFUSED"),
                               ("root-fault", "ARMADA_QUEST_MAPPER_FAULT_INJECTED")]:
            if args.case and name not in args.case:
                continue
            root_case = name in ("root", "root-fault")
            case = {"name": name, "status": "started"}; report["cases"].append(case)
            directory = output / name; directory.mkdir()
            if name != "identity":
                (output / "extra/etc/systemd/system/armada-quest-fixture.service").write_text(
                    (fixtures / "fixture.service").read_text() + "\nEnvironment=ARMADA_QUEST_CASE=" + name + "\n")
                if root_case:
                    wants = output / "extra/etc/systemd/system/initrd.target.wants"
                    wants.mkdir(exist_ok=True)
                    link = wants / "quest-handoff-install.service"
                    if not link.is_symlink():
                        link.symlink_to("/etc/systemd/system/quest-handoff-install.service")
                    (output / "extra/quest-root-check.service").write_text(
                        (fixtures / "root-check.service").read_text() + "\nEnvironment=ARMADA_QUEST_CASE=" + name + "\n")
                    (output / "extra/etc/systemd/system/quest-handoff-install.service").write_text(
                        (fixtures / "handoff-install.service").read_text() + "\nEnvironment=ARMADA_QUEST_CASE=" + name +
                        "\nEnvironment=ARMADA_QUEST_PACKAGED_ROOT=" + str(int(packaged)) + "\n")
                container("cd /output/extra && find . -print0 | sort -z | cpio --null -o --format=newc --reproducible | gzip -n > /output/" + name + "/extra.cpio.gz", directory / "archive.log")
            boot = directory / "initramfs.img"
            boot.write_bytes(assembly.empty_ramdisk() + (initramfs / "initramfs.img").read_bytes() +
                             ((directory / "extra.cpio.gz").read_bytes() if name != "identity" else b""))
            command = [args.qemu, "-machine", "virt", "-accel", args.accel, "-cpu", "max" if args.accel == "tcg" else "host",
                       "-smp", "2", "-m", "4096" if root_case else "1024", "-nodefaults", "-nographic", "-monitor", "none",
                       "-serial", "stdio", "-no-reboot", "-nic", "none", "-kernel", str(kernel / "Image"), "-initrd", str(boot)]
            cmdline = assembly.root_arguments(manifest.get("root_label", "armada-vr-root"), quest_services=True)
            if root_case:
                command += ["-drive", f"file={rootfs / 'rootfs.ext4'},if=virtio,format=raw,readonly=on"]
                if not packaged:
                    cmdline = cmdline.replace("root=LABEL=armada-vr-root", "root=/dev/vda")
            command += ["-append", "console=ttyAMA0 earlycon selinux=0 audit=0 " + cmdline]
            case.update(command=command, initramfs_sha256=builder.digest(boot))
            started = time.monotonic()
            with (directory / "boot.log").open("wb") as log:
                process = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT)
                try:
                    case["returncode"] = process.wait(timeout=300 if name == "root" else 60)
                except subprocess.TimeoutExpired:
                    process.terminate()
                    try:
                        process.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        process.kill(); process.wait(timeout=5)
                    raise TimeoutError("Quest startup case timed out: " + name)
            case["elapsed_seconds"] = round(time.monotonic() - started, 3)
            log = (directory / "boot.log").read_text(errors="replace")
            markers = [required, "reboot: Power down"]
            if name != "identity" and log.count("ARMADA_QUEST_MODULE_PASS=") != 6:
                raise RuntimeError("Vendor module load failed: " + name)
            if name in ("ready", "root", "root-fault"):
                markers += ["ARMADA_QUEST_MODEL_QMI_PASS", "ARMADA_QMI_PASS requests=96"]
                if log.count("ARMADA_QUEST_MODEL_BOOT_REQUEST") != 1:
                    raise RuntimeError("Expected exactly one modeled boot request: " + name)
            elif "startup ready:" in log:
                raise RuntimeError("Unexpected readiness in failed case: " + name)
            if root_case:
                markers += ["ARMADA_QUEST_ROOT_HANDOFF_PASS", "All filesystems, swaps, loop devices, MD devices and DM devices detached."]
                if packaged:
                    markers += ["ARMADA_QUEST_PACKAGED_ROOT_PASS"]
                markers += ["ARMADA_VR_VM_PASS"] if name == "root" else ["startup failed: mapper-runtime: service mapper exited"]
            if (case["returncode"] or not all(m in log for m in markers) or
                    any(m in log for m in ("ARMADA_QUEST_UNEXPECTED", "ARMADA_QUEST_MAPPER_FAULT_NOT_HANDLED", "QEMU_HANDOFF_FIXTURE_FAIL", "ARMADA_QMI_FAIL", "ARMADA_VR_VM_FAIL"))):
                raise RuntimeError("Quest startup acceptance failed: " + name)
            case["status"] = "passed"
            print(name, case["elapsed_seconds"], flush=True)
        if rootfs and builder.digest(rootfs / "rootfs.ext4") != root_hash:
            raise ValueError("Root image changed during testing")
        if sources.verify(source_dir) != profile or any(builder.digest(ROOT / n) != sha for n, sha in report["inputs_sha256"].items()):
            raise ValueError("Test sources changed during execution")
        report.update(status="passed", rootfs_unchanged=True if rootfs else None)
    except (OSError, ValueError, RuntimeError, TimeoutError, subprocess.SubprocessError) as error:
        report.update(status="failed", error=str(error), rootfs_unchanged=(builder.digest(rootfs / "rootfs.ext4") == root_hash) if rootfs else None)
        raise
    finally:
        (output / "result.json").write_text(json.dumps(report, indent=2) + "\n")
    print(output / "result.json")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("kernel", type=Path)
    parser.add_argument("initramfs", type=Path)
    parser.add_argument("sources", type=Path)
    parser.add_argument("--rootfs", type=Path, help="Required for root handoff cases and the default complete suite")
    parser.add_argument("--case", action="append", choices=("identity", "ready", "missing-firmware", "repeated-request",
                                                          "missing-mapper", "missing-host-role", "root", "root-fault"))
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--image", default="localhost/armada-vr:qcom-services")
    parser.add_argument("--engine", default=os.environ.get("CONTAINER_ENGINE", "docker"))
    parser.add_argument("--qemu", default="qemu-system-aarch64")
    parser.add_argument("--accel", choices=("hvf", "kvm", "tcg"), default="hvf" if platform.system() == "Darwin" else "tcg")
    try:
        run(parser.parse_args())
    except (OSError, ValueError, RuntimeError, TimeoutError, subprocess.SubprocessError) as error:
        parser.exit(1, f"{error}\n")
