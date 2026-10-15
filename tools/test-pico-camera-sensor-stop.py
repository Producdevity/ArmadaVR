#!/usr/bin/env python3
"""Test sensor power-down and camera I/O release failures in the retained Pico kernel."""
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
FIXTURE = ROOT / "tests/fixtures/pico-camera-sensor-stop"
PATCH = ROOT / "patches/kernel/pico-neo3/0047-camera-sensor-stop-errors.patch"
POWER = 'techpack/camera/drivers/cam_sensor_module/cam_sensor_utils/cam_sensor_util.c'
CORE = 'techpack/camera/drivers/cam_sensor_module/cam_sensor/cam_sensor_core.c'
SOURCES = {'techpack/camera/drivers/cam_sensor_module/cam_sensor_utils/cam_sensor_util.c': '0c41eaa1487f571698e5b57647ec75cb1a00aad5c6d67a4f7bd16dacdcde0691', 'techpack/camera/drivers/cam_sensor_module/cam_sensor/cam_sensor_core.c': 'cdbbdb752cd256ebdc39271eb046872744bf4eb170a4e720e8f901483a930cda'}
SECTIONS = {POWER: [("int cam_sensor_util_power_down(", None)],
            CORE: [("int cam_sensor_power_down(", "\nint cam_sensor_apply_settings(")]}


def verify_source(source):
    for name, digest in SOURCES.items():
        data = (source / name).read_bytes()
        if hashlib.sha256(data).hexdigest() != digest:
            raise ValueError("Source does not match pinned Pico kernel: " + name)
        text = data.decode()
        expected = text[:text.index("#include")]
        for start, end in SECTIONS[name]:
            offset = text.index(start)
            expected += text[offset:(text.index(end, offset) if end else len(text))] + "\n"
        if (FIXTURE / name).read_text() != expected.rstrip() + "\n":
            raise ValueError("Camera stop fixture differs from the actual kernel: " + name)


def run_tests(output, cc="cc"):
    output.mkdir(parents=True, exist_ok=False)
    shutil.copytree(FIXTURE, output, dirs_exist_ok=True)
    harness = ROOT / "tests/pico-camera-sensor-stop.c"
    shutil.copyfile(harness, output / "test.c")
    report = {"passed": False, "runs": {}}
    env = dict(os.environ, UBSAN_OPTIONS="halt_on_error=1",
               ASAN_OPTIONS=f"detect_leaks={int(sys.platform.startswith('linux'))}")
    for name, defects in (("original", 9856), ("patched", 0)):
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
        match = re.fullmatch(r"PICO_CAMERA_SENSOR_STOP cases=(\d+) defects=(\d+)\n", result.stdout)
        if (not match or int(match[1]) != 6146 or int(match[2]) != defects
                or result.returncode != bool(defects) or result.stderr):
            raise ValueError("Unexpected camera stop result: " + name)
        report["runs"][name] = {"cases": int(match[1]), "defects": defects, "exit_code": result.returncode}
    files = [Path(__file__).resolve(), harness, PATCH, *sorted(p for p in FIXTURE.rglob("*") if p.is_file())]
    report.update(passed=True, inputs={str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest()
                                      for p in files}, device_actions=[], physical_dma_quiescence_verified=False,
                  scope="Actual sensor power-down and I/O release functions; power, pinctrl, GPIO, CCI and delay operations modeled")
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
