#!/usr/bin/env python3
"""Test Pico camera PHY stop failures through the retained kernel handler and hardware-disable helper."""
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
FIXTURE = ROOT / "tests/fixtures/pico-csiphy-stop"
PATCH = ROOT / "patches/kernel/pico-neo3/0045-csiphy-stop-errors.patch"
CORE = "techpack/camera/drivers/cam_sensor_module/cam_csiphy/cam_csiphy_core.c"
SOC = "techpack/camera/drivers/cam_sensor_module/cam_csiphy/cam_csiphy_soc.c"
SOURCES = {
    CORE: "5f8c440f30d224604730a81fe34f1db35c31d8c17bb96a80c30c0620b8dae842",
    SOC: "f068d9203260d5d561c4430793409105dd8ed16135cedd4ee11a8f3a71bc5846",
}
SECTIONS = {
    CORE: [
        ("int32_t cam_csiphy_get_instance_offset(", "\nvoid cam_csiphy_query_cap("),
        ("int32_t cam_csiphy_core_cfg(", "\tcase CAM_ACQUIRE_DEV: {"),
        ("\tcase CAM_STOP_DEV: {", "\tcase CAM_RELEASE_DEV: {"),
    ],
    SOC: [("int32_t cam_csiphy_disable_hw(", "\nint32_t cam_csiphy_parse_dt_info(")],
}


def verify_source(source):
    for name, digest in SOURCES.items():
        data = (source / name).read_bytes()
        if hashlib.sha256(data).hexdigest() != digest:
            raise ValueError("Source does not match pinned Pico kernel: " + name)
        text = data.decode()
        expected = text[:text.index("#include")]
        for start, end in SECTIONS[name]:
            offset = text.index(start)
            expected += text[offset:text.index(end, offset)] + ("\n" if name == CORE else "")
        if name == CORE:
            offset = text.index("\n\tdefault:", text.index("int32_t cam_csiphy_core_cfg("))
            expected += text[offset:]
        if (FIXTURE / name).read_text() != expected.rstrip() + "\n":
            raise ValueError("Camera stop fixture differs from the actual kernel: " + name)


def run_tests(output, cc="cc"):
    output.mkdir(parents=True, exist_ok=False)
    shutil.copytree(FIXTURE, output, dirs_exist_ok=True)
    harness = ROOT / "tests/pico-csiphy-stop.c"
    shutil.copyfile(harness, output / "test.c")
    report = {"passed": False, "runs": {}}
    env = dict(os.environ, UBSAN_OPTIONS="halt_on_error=1",
               ASAN_OPTIONS=f"detect_leaks={int(sys.platform.startswith('linux'))}")
    for name, defects in (("original", 175), ("patched", 0)):
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
        match = re.fullmatch(r"PICO_CSIPHY_STOP cases=(\d+) defects=(\d+)\n", result.stdout)
        if (not match or int(match[1]) != 184 or int(match[2]) != defects
                or result.returncode != bool(defects) or result.stderr):
            raise ValueError("Unexpected camera stop result: " + name)
        report["runs"][name] = {"cases": int(match[1]), "defects": defects, "exit_code": result.returncode}
    files = [Path(__file__).resolve(), harness, PATCH, *sorted(p for p in FIXTURE.rglob("*") if p.is_file())]
    report.update(passed=True, inputs={str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest()
                                      for p in files}, device_actions=[], physical_dma_quiescence_verified=False,
                  scope="Actual PHY stop dispatch, handle lookup and disable helper; user copy, hardware and locking modeled")
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
