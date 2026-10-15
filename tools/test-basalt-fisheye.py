#!/usr/bin/env python3
"""Check native Fisheye624 projection, derivatives and inverse failures against the pinned Basalt source."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
HEADER = Path("thirdparty/basalt-headers/include/basalt/camera/fisheye624_camera.hpp")
PATCH = ROOT / "patches/basalt/0005-stabilize-fisheye624.patch"


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--cxx", default="c++")
    args = parser.parse_args()
    spec = importlib.util.spec_from_file_location("basalt_build", ROOT / "tools/build-basalt.py")
    build = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(build)
    source = args.source.resolve()
    profile = json.loads((ROOT / "profiles/basalt.json").read_text())
    build.verify(source, profile)
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    patched = output / "source"
    (patched / HEADER).parent.mkdir(parents=True)
    shutil.copyfile(source / HEADER, patched / HEADER)
    result = subprocess.run(["patch", "--batch", "--fuzz=0", "--forward", "-p1", "-i", str(PATCH)],
                            cwd=patched, capture_output=True, text=True, timeout=15)
    (output / "patch.log").write_text(result.stdout + result.stderr)
    result.check_returncode()
    headers = source / "thirdparty/basalt-headers"
    includes = [headers / "include", headers / "thirdparty/eigen", headers / "thirdparty/Sophus"]
    test = ROOT / "tests/basalt-fisheye624.cpp"
    report = {"passed": False, "source_tree_sha256": profile["source_tree_sha256"],
              "original_header_sha256": digest(source / HEADER), "patched_header_sha256": digest(patched / HEADER),
              "inputs": {str(p.relative_to(ROOT)): digest(p) for p in (PATCH, test, Path(__file__).resolve())},
              "runs": {}, "commands": [], "device_actions": [], "physical_camera_verified": False}
    for name, defects in (("original", 982), ("patched", 0)):
        binary = output / name
        command = [args.cxx, "-std=c++17", "-O1", "-g", "-DSOPHUS_USE_BASIC_LOGGING",
                   "-fsanitize=address,undefined", "-fno-sanitize-recover=all"]
        if name == "patched":
            command += ["-I", str(patched / "thirdparty/basalt-headers/include")]
        for include in includes:
            command += ["-isystem", str(include)]
        command += ["-Wall", "-Wextra", "-Werror", str(test), "-o", str(binary)]
        report["commands"].append(command)
        with (output / (name + "-build.log")).open("x") as log:
            subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=120)
        env = dict(os.environ, UBSAN_OPTIONS="halt_on_error=1",
                   ASAN_OPTIONS=f"detect_leaks={int(sys.platform.startswith('linux'))}")
        result = subprocess.run([str(binary)], capture_output=True, text=True, env=env, timeout=60)
        (output / (name + ".log")).write_text(result.stdout + result.stderr)
        match = re.fullmatch(r"BASALT_FISHEYE624 cases=(\d+) defects=(\d+)\n", result.stdout)
        if (not match or int(match[1]) != 10334 or int(match[2]) != defects or
                result.returncode != bool(defects) or result.stderr):
            raise ValueError("Unexpected Fisheye624 regression result: " + name)
        report["runs"][name] = {"cases": int(match[1]), "defects": defects, "binary_sha256": digest(binary)}
    report["passed"] = True
    (output / "result.json").write_text(json.dumps(report, indent=2) + "\n")
    print("Native Fisheye624: 10334 checks passed; original 982 failures reproduced")


if __name__ == "__main__":
    main()
