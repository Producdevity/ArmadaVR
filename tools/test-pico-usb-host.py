#!/usr/bin/env python3
"""Exercise the Pico USB host lifecycle with modeled hardware and real notifier list logic."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
FIXTURE = ROOT / "tests/fixtures/pico-usb-host"
PATCHES = [ROOT / "patches/kernel/pico-neo3" / name for name in (
    "0042-usb-host-notifier-cleanup.patch", "0043-usb-host-power-cleanup.patch")]
DRIVER = Path("drivers/usb/dwc3/dwc3-msm.c")
SOURCES = {
    str(DRIVER): "f55af56218dbd5964f5de40a47dfb4e90d3e2d68d59537eca1a6d3462af38c3c",
    "kernel/notifier.c": "68ef7db385e0e5939476099793956a2fcc983d81215d47b21f83bcc643e83ca0",
}


def verify_source(source):
    contents = {}
    for name, digest in SOURCES.items():
        data = (source / name).read_bytes()
        if hashlib.sha256(data).hexdigest() != digest:
            raise ValueError("Source does not match pinned Pico kernel: " + name)
        contents[name] = data.decode()
    driver = contents[str(DRIVER)]
    start = driver.index("static int dwc3_otg_start_host(")
    end = driver.index("\nstatic void dwc3_override_vbus_status", start)
    expected = driver[:driver.index("#include")] + driver[start:end]
    if (FIXTURE / DRIVER).read_text() != expected:
        raise ValueError("USB host fixture differs from the actual driver")
    notifier = contents["kernel/notifier.c"]
    expected = "// SPDX-License-Identifier: GPL-2.0\n"
    for name, end_marker in (
        ("notifier_chain_register", "static int notifier_chain_cond_register"),
        ("notifier_chain_unregister", "/**\n * notifier_call_chain"),
    ):
        start = notifier.index("static int " + name + "(")
        expected += notifier[start:notifier.index(end_marker, start)]
    if (FIXTURE / "notifier.inc").read_text() != expected:
        raise ValueError("Notifier fixture differs from the actual kernel")


def run_tests(output, cc="cc"):
    output.mkdir(parents=True, exist_ok=False)
    shutil.copytree(FIXTURE, output, dirs_exist_ok=True)
    harness = ROOT / "tests/pico-usb-host.c"
    shutil.copyfile(harness, output / "test.c")
    report = {"passed": False, "runs": {}}
    env = dict(os.environ, UBSAN_OPTIONS="halt_on_error=1",
               ASAN_OPTIONS=f"detect_leaks={int(sys.platform.startswith('linux'))}")
    for name in ("original", "notifier", "patched"):
        if name != "original":
            patch = PATCHES[0 if name == "notifier" else 1]
            result = subprocess.run(
                ["patch", "--batch", "--fuzz=0", "--forward", "-p1", "-i", str(patch)],
                cwd=output, capture_output=True, text=True, timeout=15, check=True)
            (output / (name + "-patch.log")).write_text(result.stdout + result.stderr)
        command = [cc, "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                   "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                   "-I", str(output), str(output / "test.c"), "-o", str(output / name)]
        result = subprocess.run(command, capture_output=True, text=True, timeout=30)
        (output / (name + "-build.log")).write_text(result.stdout + result.stderr)
        result.check_returncode()
        result = subprocess.run([str(output / name)], env=env, capture_output=True,
                                text=True, timeout=30)
        (output / (name + ".log")).write_text(result.stdout + result.stderr)
        match = re.fullmatch(r"PICO_USB_HOST cases=(\d+) defects=(\d+)\n", result.stdout)
        defects = {"original": 6208, "notifier": 2624, "patched": 0}[name]
        if (not match or int(match[1]) != 608 or int(match[2]) != defects
                or result.returncode != bool(defects) or result.stderr):
            raise ValueError("Unexpected USB host result: " + name)
        report["runs"][name] = {"cases": int(match[1]), "defects": defects,
                                "exit_code": result.returncode}
    files = [harness, *PATCHES, *sorted(p for p in FIXTURE.rglob("*") if p.is_file())]
    report.update(passed=True, inputs={str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest()
                                      for p in files},
                  scope="Actual host start/stop and notifier register/unregister functions; hardware, callbacks and locking modeled")
    (output / "result.json").write_text(json.dumps(report, indent=2) + "\n")
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path, help="Pinned, unmodified kernel source tree")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--cc", default="cc")
    args = parser.parse_args()
    verify_source(args.source)
    report = run_tests(args.output.resolve(), args.cc)
    report["source_sha256"] = SOURCES
    (args.output / "result.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
