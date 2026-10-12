#!/usr/bin/env python3
"""Verify timestamp and payload ownership in the pinned native SSC dispatcher."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path, help="Unmodified libssc source from profiles/ssc.json")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--cc", default="gcc")
    args = parser.parse_args()
    profile = json.loads((ROOT / "profiles/ssc.json").read_text())
    source = args.source.resolve()
    for name, expected in profile["test_source_sha256"].items():
        if digest(source / name) != expected:
            raise ValueError("Source differs from the pinned libssc revision: " + name)
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    patched = output / "patched"
    shutil.copytree(source, patched)
    patch = ROOT / profile["patches"][0]
    with (output / "patch.log").open("w") as log:
        subprocess.run(["patch", "--batch", "--fuzz=0", "--forward", "-p1", "-i", str(patch)],
                       cwd=patched, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=10)
    if digest(patched / "src/libssc-client.c") != profile["patched_report_source_sha256"]:
        raise ValueError("Patched dispatcher differs from the expected source")
    generated = output / "generated"
    generated.mkdir()
    subprocess.run(["protoc-c", "--proto_path=" + str(source / "data"),
                    "--c_out=" + str(generated), str(source / "data/ssc-common.proto")],
                   check=True, timeout=10)
    flags = shlex.split(subprocess.check_output(
        ["pkg-config", "--cflags", "--libs", "gio-2.0", "qmi-glib", "libprotobuf-c"], text=True))
    commands = {}
    for name, tree in (("original", source), ("patched", patched)):
        binary = output / ("test-" + name)
        command = [args.cc, "-std=gnu11", "-g", "-O1", "-Wall", "-Wextra", "-Werror",
                   "-Wno-unused-parameter", "-fsanitize=address,undefined", '-DG_LOG_DOMAIN="libssc"']
        if name == "original":
            command.append("-DEXPECT_OLD")
        command += ["-I" + str(tree / "src"), "-I" + str(generated), "-I" + str(ROOT / "src"),
                    str(ROOT / "tests/ssc-reports.c"), str(tree / "src/libssc-common.c"),
                    str(generated / "ssc-common.pb-c.c"), str(ROOT / "src/pico-controller.c"),
                    *flags, "-o", str(binary)]
        with (output / ("build-" + name + ".log")).open("w") as log:
            subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=60)
        env = dict(os.environ, SSC_TEST_WIRE_OUTPUT=str(output / ("wire-" + name + ".pb")),
                   ASAN_OPTIONS="detect_leaks=1", UBSAN_OPTIONS="halt_on_error=1")
        with (output / ("test-" + name + ".log")).open("w") as log:
            subprocess.run([str(binary)], env=env, stdout=log, stderr=subprocess.STDOUT,
                           check=True, timeout=15)
        commands[name] = command
    if (output / "wire-original.pb").read_bytes() != (output / "wire-patched.pb").read_bytes():
        raise ValueError("The test inputs differ between source revisions")
    report = {"passed": True, "source_commit": profile["commit"],
              "patch_sha256": digest(patch), "test_sha256": digest(ROOT / "tests/ssc-reports.c"),
              "wire_sha256": digest(output / "wire-patched.pb"), "compile_commands": commands,
              "scope": "Actual protobuf dispatcher; no QRTR, DSP, radio or physical stream acceptance"}
    (output / "result.json").write_text(json.dumps(report, indent=2) + "\n")
    print((output / "test-patched.log").read_text(), end="")


if __name__ == "__main__":
    main()
