#!/usr/bin/env python3
"""Check Pico FastRPC's initialization guard before any transport operation."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
SOURCE_SHA256 = "02881153c10412441976514779d19688dd613021c6c6e2736165c6b27eadc239"
FIXTURE = ROOT / "tests/fixtures/pico-fastrpc-init/drivers/char/adsprpc.c"
PATCH = ROOT / "patches/kernel/pico-neo3/0033-fastrpc-init-channel.patch"


def fragment(source):
    start = source.index("static int fastrpc_init_process(")
    end = source.index("\tVERIFY(err, 0 == (err = fastrpc_channel_open(fl)));", start)
    return source[start:end]


def run_tests(output, cc="cc"):
    output.mkdir(parents=True, exist_ok=False)
    shutil.copytree(FIXTURE.parents[2], output, dirs_exist_ok=True)
    source = output / "test.c"
    shutil.copyfile(ROOT / "tests/pico-fastrpc-init.c", source)
    report = {"passed": False, "negative_controls": {}}
    env = dict(
        os.environ,
        UBSAN_OPTIONS="halt_on_error=1",
        ASAN_OPTIONS=f"detect_leaks={int(sys.platform.startswith('linux'))}:abort_on_error=0",
    )
    for old in (True, False):
        name = "original" if old else "patched"
        if not old:
            result = subprocess.run(
                ["patch", "--batch", "--fuzz=0", "--forward", "-p1", "-i", str(PATCH)],
                cwd=output,
                check=True,
                capture_output=True,
                text=True,
                timeout=15,
            )
            (output / "patch.log").write_text(result.stdout + result.stderr)
        binary = output / name
        command = [
            cc,
            "-std=c11",
            "-O1",
            "-g",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-Wno-unused-variable",
            "-fsanitize=address,undefined",
            "-fno-sanitize-recover=all",
            "-I",
            str(output),
            *(["-DEXPECT_OLD"] if old else []),
            str(source),
            "-o",
            str(binary),
        ]
        compiled = subprocess.run(command, capture_output=True, text=True, timeout=30)
        (output / f"{name}-build.log").write_text(compiled.stdout + compiled.stderr)
        compiled.check_returncode()
        result = subprocess.run(
            [str(binary)], capture_output=True, text=True, env=env, timeout=15
        )
        (output / f"{name}.log").write_text(result.stdout + result.stderr)
        result.check_returncode()
        if old:
            for cid in (-1, 4):
                result = subprocess.run(
                    [str(binary), str(cid)],
                    capture_output=True,
                    text=True,
                    env=env,
                    timeout=15,
                )
                (output / f"original-cid-{cid}.log").write_text(
                    result.stdout + result.stderr
                )
                if (
                    not result.returncode
                    or "AddressSanitizer: heap-buffer-overflow" not in result.stderr
                ):
                    raise ValueError(
                        f"Original bounds failure not reproduced for channel {cid}"
                    )
                report["negative_controls"][str(cid)] = result.returncode
    report.update(
        passed=True,
        fixture_sha256=hashlib.sha256(FIXTURE.read_bytes()).hexdigest(),
        harness_sha256=hashlib.sha256(source.read_bytes()).hexdigest(),
        patch_sha256=hashlib.sha256(PATCH.read_bytes()).hexdigest(),
        scope="Actual initialization prefix with channel-open boundary stubbed; no DSP or device access",
    )
    (output / "result.json").write_text(json.dumps(report, indent=2) + "\n")
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "source", type=Path, help="Pinned, unmodified drivers/char/adsprpc.c"
    )
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--cc", default="cc")
    args = parser.parse_args()
    data = args.source.read_bytes()
    if hashlib.sha256(data).hexdigest() != SOURCE_SHA256:
        raise ValueError("FastRPC source does not match the pinned Pico kernel")
    expected = fragment(data.decode())
    fixture = FIXTURE.read_text()
    start = fixture.index("static int fastrpc_init_process(")
    end = fixture.index("\treturn fastrpc_channel_open(fl);", start)
    if fixture[start:end] != expected:
        raise ValueError(
            "Fixture does not match the actual driver initialization prefix"
        )
    output = args.output.resolve()
    report = run_tests(output, args.cc)
    report["source_sha256"] = SOURCE_SHA256
    (output / "result.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report))


if __name__ == "__main__":
    main()
