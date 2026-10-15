#!/usr/bin/env python3
"""Test reported Pico camera stop errors through retained manager and ISP context functions."""
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
FIXTURE = ROOT / "tests/fixtures/pico-camera-stop"
PATCH = ROOT / "patches/kernel/pico-neo3/0044-camera-stop-errors.patch"
MANAGER = "techpack/camera/drivers/cam_isp/isp_hw_mgr/cam_ife_hw_mgr.c"
CONTEXT = "techpack/camera/drivers/cam_isp/cam_isp_context.c"
CORE = "techpack/camera/drivers/cam_isp/isp_hw_mgr/isp_hw/ife_csid_hw/cam_ife_csid_core.c"
SOURCES = {
    CORE: "87a9ad4abb733c3a1abce98de3e864e712f97e9fc6b2220524106d80c27d1154",
    MANAGER: "e699cf2a64f03adebd9e7e7651bd5bc1945c42be5df431af67067c9797092449",
    CONTEXT: "1f12c25a9634618b4cfa7920659a13a76b0a82553870f54c89aafdb4b5173036",
}
SECTIONS = {
    CORE: [("int cam_ife_csid_stop(", "\nint cam_ife_csid_read(")],
    MANAGER: [
        ("static void cam_ife_hw_mgr_stop_hw_res(", "\nstatic void cam_ife_hw_mgr_deinit_hw_res("),
        ("static int cam_ife_mgr_csid_change_halt_mode(", "\nstatic int cam_ife_hw_mgr_release_hw_for_ctx("),
        ("static int cam_ife_mgr_stop_hw(", "\nstatic int cam_ife_mgr_reset_vfe_hw("),
    ],
    CONTEXT: [("static int __cam_isp_ctx_stop_dev_in_activated_unlock(",
               "\nstatic int __cam_isp_ctx_release_dev_in_activated(")],
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
            expected += text[offset:text.index(end, offset)] + "\n"
        if (FIXTURE / name).read_text() != expected:
            raise ValueError("Camera stop fixture differs from the actual kernel: " + name)


def run_tests(output, cc="cc"):
    output.mkdir(parents=True, exist_ok=False)
    shutil.copytree(FIXTURE, output, dirs_exist_ok=True)
    harness = ROOT / "tests/pico-camera-stop.c"
    shutil.copyfile(harness, output / "test.c")
    report = {"passed": False, "runs": {}}
    env = dict(os.environ, UBSAN_OPTIONS="halt_on_error=1",
               ASAN_OPTIONS=f"detect_leaks={int(sys.platform.startswith('linux'))}")
    for name, defects in (("original", 1794), ("patched", 0)):
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
        match = re.fullmatch(r"PICO_CAMERA_STOP cases=(\d+) defects=(\d+)\n", result.stdout)
        if (not match or int(match[1]) != 1799 or int(match[2]) != defects
                or result.returncode != bool(defects) or result.stderr):
            raise ValueError("Unexpected camera stop result: " + name)
        report["runs"][name] = {"cases": int(match[1]), "defects": defects, "exit_code": result.returncode}
    files = [Path(__file__).resolve(), harness, PATCH, *sorted(p for p in FIXTURE.rglob("*") if p.is_file())]
    report.update(passed=True, inputs={str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest()
                                      for p in files}, device_actions=[], physical_dma_quiescence_verified=False,
                  scope="Actual manager and ISP stop functions; hardware callbacks, waits and locking modeled")
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
