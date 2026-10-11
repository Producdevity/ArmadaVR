#!/usr/bin/env python3
"""Fault-test the Pico station notification driver without devices."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
SOURCE_HASHES = {
    "Makefile": "800f7b8cd48ac598bbef9ec8f486d029f6827b5a8b4a69d9668fa0c87930ab6d",
    "Kconfig": "e4744a13d5920ce860f52c76eaf052679143a206afac3d6cfeccfe7008809559",
}


def test_driver(driver, output, compiler):
    (output / "driver.inc").write_text(re.sub(r"^#include <linux/[^>]+>\n", "", driver, flags=re.M))
    shutil.copyfile(ROOT / "tests/pico-station.c", output / "test.c")
    command = [compiler, "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
               "-Wno-unused-parameter", "-fsanitize=address,undefined", "-pthread",
               str(output / "test.c"), "-o", str(output / "test")]
    with (output / "build.log").open("w") as log:
        subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=60)
    with (output / "test.log").open("w") as log:
        subprocess.run([str(output / "test")], stdout=log, stderr=subprocess.STDOUT,
                       check=True, timeout=60)
    return command


def run(source, output, compiler):
    inputs = {}
    for name, expected in SOURCE_HASHES.items():
        data = (source / name).read_bytes()
        if hashlib.sha256(data).hexdigest() != expected:
            raise ValueError("Not the pinned Pico kernel build file: " + name)
        inputs[name] = data
    output.mkdir(parents=True, exist_ok=False)
    destination = output / "drivers/misc"
    destination.mkdir(parents=True)
    for name, data in inputs.items():
        (destination / name).write_bytes(data)
    patch = ROOT / "patches/kernel/pico-neo3/0024-station-notifications.patch"
    with (output / "patch.log").open("w") as log:
        subprocess.run(["patch", "--batch", "--fuzz=0", "--forward", "-p1", "-i", str(patch)],
                       cwd=output, stdout=log, stderr=subprocess.STDOUT, check=True)
    driver = (destination / "pico-station.c").read_text()
    command = test_driver(driver, output, compiler)
    result = {"status": "passed", "source_hashes": SOURCE_HASHES,
              "patch_sha256": hashlib.sha256(patch.read_bytes()).hexdigest(),
              "driver_sha256": hashlib.sha256(driver.encode()).hexdigest(),
              "compile_command": command, "physical_irq_verified": False,
              "flash_ready": False}
    (output / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print((output / "test.log").read_text(), end="")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path, help="Pinned kernel drivers/misc directory")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--cc", default="cc")
    args = parser.parse_args()
    run(args.source, args.output.resolve(), args.cc)


if __name__ == "__main__":
    main()
