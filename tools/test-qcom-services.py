#!/usr/bin/env python3
"""Test the pinned PD mapper over real QRTR/QMI in diskless vendor-kernel QEMU."""
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


builder = load("initramfs_builder", "build-headset-initramfs.py")
sources = load("qcom_sources", "fetch-qcom-services.py")

SCRIPT = r'''
set -eu
cmp /profile.json /usr/share/armada-qcom-services/sources.json
cmp /bounds.patch /usr/share/armada-qcom-services/0001-check-qmi-packet-bounds.patch
cmp /path.patch /usr/share/armada-qcom-services/0001-report-actual-firmware-path.patch
mkdir /tmp/pd-mapper
tar -xzf /sources/pd-mapper.tar.gz -C /tmp/pd-mapper --strip-components=1
gcc -O2 -Wall -Wextra -Werror -I/tmp/pd-mapper /tests/probe.c /tmp/pd-mapper/servreg_loc.c -lqrtr -o /output/qmi-probe
mkdir -p /output/extra
cp /tests/init.sh /output/extra/qmi-init
chmod 755 /output/extra/qmi-init
cp /output/qmi-probe /output/extra/qmi-probe
cp /usr/share/armada-qcom-services/sanitizers.log /output/sanitizers.log
python3 - <<'PY'
import re,shutil,subprocess
from pathlib import Path
root=Path('/output/extra')
for name in ['/usr/bin/pd-mapper','/usr/bin/systemctl']:
    path=Path(name); dest=root/name.lstrip('/')
    dest.parent.mkdir(parents=True,exist_ok=True); shutil.copy2(path,dest)
for binary in ['/output/qmi-probe','/usr/bin/pd-mapper','/usr/bin/systemctl']:
    result=subprocess.check_output(['ldd',binary],text=True)
    if 'not found' in result:
        raise SystemExit('Unresolved runtime library: '+binary)
    for name in re.findall(r'(/[^\s()]+)',result):
        original=Path(name)
        # Keep the requested SONAME while resolving Fedora's /lib directory alias.
        path=original.parent.resolve()/original.name
        dest=root/str(path).lstrip('/')
        dest.parent.mkdir(parents=True,exist_ok=True); shutil.copy2(path,dest)
PY
cd /output/extra
find . -print0 | sort -z | cpio --null -o --format=newc --reproducible | gzip -n > /output/extra.cpio.gz
'''


def run(args):
    kernel, initramfs = args.kernel.resolve(), args.initramfs.resolve()
    source_dir, output = args.sources.resolve(), args.output.resolve()
    profile = sources.verify(source_dir)
    build, hashes, _, _ = builder.inputs(kernel)
    initrd = json.loads((initramfs / "build.json").read_text())
    if build.get("qemu_transports") is not True or initrd.get("qemu_transports") is not True:
        raise ValueError("Requires QEMU kernel and initramfs variants; physical headset artifacts are refused")
    if initrd.get("status") != "built" or initrd["kernel_build_sha256"] != builder.digest(kernel / "build.json"):
        raise ValueError("Initramfs does not match the verified kernel build")
    artifact = next(item for item in initrd["artifacts"] if item["path"] == "initramfs.img")
    if builder.digest(initramfs / "initramfs.img") != artifact["sha256"]:
        raise ValueError("Initramfs checksum mismatch")
    if not initrd.get("firmware"):
        raise ValueError("Requires a firmware-bearing initramfs with the Quest ADSP service maps")
    if output.exists() or any(c in str(p) for p in (kernel, initramfs, source_dir, output, ROOT) for c in (",", ":")):
        raise ValueError("Choose a new output directory and paths without commas or colons")
    image = subprocess.check_output([args.engine, "image", "inspect", args.image, "--format", "{{.Id}}"], text=True).strip()
    output.mkdir(parents=True)
    files = ["tools/test-qcom-services.py", "tests/qcom-services/init.sh", "tests/qcom-services/probe.c",
             "patches/qrtr/0001-check-qmi-packet-bounds.patch", "profiles/qcom-services.json",
             "patches/pd-mapper/0001-report-actual-firmware-path.patch"]
    report = {"status": "started", "scope": "real vendor-kernel QRTR/QMI with modeled remoteproc enumeration",
              "firmware_executed": False, "adsp_boot_verified": False, "hardware_boot_verified": False,
              "container_image": image, "sources": profile, "kernel_sha256": hashes["Image"],
              "base_initramfs_sha256": artifact["sha256"], "commands": [],
              "inputs_sha256": {name: builder.digest(ROOT / name) for name in files}}
    try:
        mounts = [(source_dir, "/sources:ro"), (ROOT / "tests/qcom-services", "/tests:ro"),
                  (ROOT / "profiles/qcom-services.json", "/profile.json:ro"),
                  (ROOT / "patches/qrtr/0001-check-qmi-packet-bounds.patch", "/bounds.patch:ro"),
                  (ROOT / "patches/pd-mapper/0001-report-actual-firmware-path.patch", "/path.patch:ro"), (output, "/output")]
        command = [args.engine, "run", "--rm", "--init", "--network", "none", "--memory", "512m",
                   "--memory-swap", "512m", "--cpus", "2", "--pids-limit", "64"]
        for host, guest in mounts:
            command.extend(["-v", f"{host}:{guest}"])
        command.extend(["--entrypoint", "timeout", image, "--kill-after=5", "60", "bash", "-c", SCRIPT])
        report["commands"].append(command)
        with (output / "build.log").open("wb") as log:
            subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, timeout=80, check=True)
        boot_initrd = output / "initramfs.img"
        boot_initrd.write_bytes((initramfs / "initramfs.img").read_bytes() + (output / "extra.cpio.gz").read_bytes())
        report["initramfs_sha256"] = builder.digest(boot_initrd)
        report["probe_sha256"] = builder.digest(output / "qmi-probe")
        command = [args.qemu, "-machine", "virt", "-accel", args.accel, "-cpu", "max" if args.accel == "tcg" else "host",
                   "-smp", "2", "-m", "1024", "-nodefaults", "-nographic", "-monitor", "none", "-serial", "stdio",
                   "-no-reboot", "-nic", "none", "-kernel", str(kernel / "Image"), "-initrd", str(boot_initrd),
                   "-append", "rdinit=/qmi-init console=ttyAMA0 earlycon selinux=0 audit=0 panic=-1"]
        report["commands"].append(command)
        started = time.monotonic()
        with (output / "boot.log").open("wb") as log:
            process = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT)
            try:
                report["returncode"] = process.wait(timeout=45)
            except subprocess.TimeoutExpired:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill(); process.wait(timeout=5)
                raise TimeoutError("QRTR/QMI test did not finish; inspect boot.log")
        report["elapsed_seconds"] = round(time.monotonic() - started, 3)
        log = (output / "boot.log").read_text(errors="replace")
        markers = ["ARMADA_QMI_CYCLE_PASS=1", "ARMADA_QMI_CYCLE_PASS=2", "ARMADA_QMI_NO_MAPS_PASS",
                   "ARMADA_QMI_INIT_PASS", "no pd maps available", "reboot: Power down",
                   "Cannot open firmware path: /lib/firmware//missing"]
        if (report["returncode"] or not all(marker in log for marker in markers) or
                log.count("ARMADA_QMI_PASS requests=96") != 2 or "ARMADA_QMI_FAIL" in log):
            raise RuntimeError("QRTR/QMI acceptance failed; inspect boot.log")
        if sources.verify(source_dir) != profile or any(builder.digest(ROOT / name) != sha for name, sha in report["inputs_sha256"].items()):
            raise ValueError("Test sources changed during execution")
        report.update(status="passed", requests=192, mapper_starts=2, missing_maps_exit=1)
    except (OSError, ValueError, RuntimeError, TimeoutError, subprocess.SubprocessError) as error:
        report.update(status="failed", error=str(error))
        raise
    finally:
        (output / "result.json").write_text(json.dumps(report, indent=2) + "\n")
    print(output / "result.json")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("kernel", type=Path)
    parser.add_argument("initramfs", type=Path)
    parser.add_argument("sources", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--image", default="localhost/armada-vr:qcom-services")
    parser.add_argument("--engine", default=os.environ.get("CONTAINER_ENGINE", "docker"))
    parser.add_argument("--qemu", default="qemu-system-aarch64")
    parser.add_argument("--accel", choices=("hvf", "kvm", "tcg"), default="hvf" if platform.system() == "Darwin" else "tcg")
    try:
        run(parser.parse_args())
    except (OSError, ValueError, RuntimeError, TimeoutError, subprocess.SubprocessError) as error:
        parser.exit(1, f"{error}\n")
