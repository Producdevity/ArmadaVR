#!/usr/bin/env python3
"""Test reported camera regulator and IRQ shutdown failures in the retained Quest kernel."""
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
FIXTURE = ROOT / "tests/fixtures/quest-camera-power-stop"
PATCH = ROOT / "patches/kernel/linux-userspace/0005-quest-camera-power-stop-errors.patch"
CORE = "techpack/camera/drivers/cam_utils/cam_soc_util.c"
SOURCES = {CORE: 'fa7603e03e0f1bb27b215a43d667449ea7757e2dc263ee71f3705963f93be7be'}
SECTIONS = {CORE: [
    ("int cam_soc_util_regulator_disable(", "\nint cam_soc_util_regulator_enable("),
    ("static void cam_soc_util_regulator_disable_default(", "\nstatic int cam_soc_util_regulator_enable_default("),
    ("int cam_soc_util_disable_platform_resource(", "\nint cam_soc_util_reg_dump("),
]}


def verify_source(source):
    for name, digest in SOURCES.items():
        data = (source / name).read_bytes()
        if hashlib.sha256(data).hexdigest() != digest:
            raise ValueError("Source does not match pinned Quest kernel: " + name)
        text = data.decode()
        expected = text[:text.index("#include")]
        for start, end in SECTIONS[name]:
            offset = text.index(start)
            expected += text[offset:text.index(end, offset)] + "\n"
        if (FIXTURE / name).read_text() != expected.rstrip() + "\n":
            raise ValueError("Camera stop fixture differs from the actual kernel: " + name)


def run_tests(output, cc="cc"):
    output.mkdir(parents=True, exist_ok=False)
    shutil.copytree(FIXTURE, output, dirs_exist_ok=True)
    harness = ROOT / "tests/quest-camera-power-stop.c"
    shutil.copyfile(harness, output / "test.c")
    report = {"passed": False, "runs": {}}
    env = dict(os.environ, UBSAN_OPTIONS="halt_on_error=1",
               ASAN_OPTIONS=f"detect_leaks={int(sys.platform.startswith('linux'))}")
    for name, defects in (("original", 925), ("patched", 0)):
        if name == "patched":
            result = subprocess.run(
                ["patch", "--batch", "--fuzz=0", "--forward", "-p1", "-i", str(PATCH)],
                cwd=output, capture_output=True, text=True, timeout=15)
            (output / "patch.log").write_text(result.stdout + result.stderr)
            result.check_returncode()
        command = [cc, "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                   "-Wno-unused-parameter", "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                   "-I", str(output), str(output / "test.c"), "-o", str(output / name)]
        result = subprocess.run(command, capture_output=True, text=True, timeout=30)
        (output / (name + "-build.log")).write_text(result.stdout + result.stderr)
        result.check_returncode()
        result = subprocess.run([str(output / name)], env=env, capture_output=True, text=True, timeout=30)
        (output / (name + ".log")).write_text(result.stdout + result.stderr)
        match = re.fullmatch(r"QUEST_CAMERA_POWER_STOP cases=(\d+) defects=(\d+)\n", result.stdout)
        if (not match or int(match[1]) != 2372 or int(match[2]) != defects
                or result.returncode != bool(defects) or result.stderr):
            raise ValueError("Unexpected camera stop result: " + name)
        report["runs"][name] = {"cases": int(match[1]), "defects": defects, "exit_code": result.returncode}
    files = [Path(__file__).resolve(), harness, PATCH, *sorted(p for p in FIXTURE.rglob("*") if p.is_file())]
    report.update(passed=True, inputs={str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest()
                                      for p in files}, device_actions=[], physical_dma_quiescence_verified=False,
                  scope="Actual regulator and platform shutdown helpers; regulator, IRQ, clock and delay operations modeled")
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
