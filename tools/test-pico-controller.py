#!/usr/bin/env python3
"""Fault-test the pinned Pico SPI controller source on the host."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
SOURCE_SHA256 = "a71f126a7e1466f5b58373c36f0ea79fd1689d6ef08f243216cbc802562bc39b"


def function(source, name):
    match = re.search(r"^(?:static\s+)?(?:inline\s+)?(?:int|void|ssize_t|unsigned int)\s+"
                      + re.escape(name) + r"\([^;{]*\)\n\{", source, re.M)
    if not match:
        raise ValueError("Missing source function: " + name)
    end = match.end()
    depth = 1
    while depth and end < len(source):
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    if depth:
        raise ValueError("Incomplete source function: " + name)
    return source[match.start():end] + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path, help="Unmodified drivers/spi/spidev.c from the pinned Neo3 source")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--cc", default="clang")
    args = parser.parse_args()
    data = args.source.read_bytes()
    if hashlib.sha256(data).hexdigest() != SOURCE_SHA256:
        raise ValueError("Source does not match the pinned Pico driver")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    target = output / "drivers/spi/spidev.c"
    target.parent.mkdir(parents=True)
    target.write_bytes(data)
    patch = ROOT / "patches/kernel/pico-neo3/0023-controller-resource-lifetime.patch"
    with (output / "patch.log").open("w") as log:
        subprocess.run(["patch", "--batch", "--fuzz=0", "--forward", "-p1", "-i", str(patch)],
                       cwd=output, stdout=log, stderr=subprocess.STDOUT, check=True)
    source = target.read_text()
    names = ("spidev_sync", "spidev_sync_write", "spidev_sync_read", "spidev_read",
             "spidev_write", "cam_spidev_write", "spidev_release", "spi_poll",
             "spidev_controller_release", "spidev_controller_setup", "spidev_probe", "spidev_remove")
    fragments = "\n".join(function(source, name) for name in names)
    start = source.index("\tcase SPI_IOC_SET_RFIC_IO2_STATUS:")
    end = source.index("\tcase SPI_IOC_GET_POWER_COUNTER:", start)
    fragments += "\nstatic int controls(unsigned cmd, unsigned long arg) {\n"
    fragments += "struct spidev_data *spidev = pvr_dev; struct spi_device *spi = spidev->spi;\n"
    fragments += "unsigned tmp = 0; int retval = 0, result = 0; (void)spi; switch (cmd) {\n"
    fragments += source[start:end] + "}\n(void)result; return retval;\n}\n"
    (output / "driver-functions.inc").write_text(fragments)
    shutil.copy2(ROOT / "tests/pico-controller-lifecycle.c", output / "test.c")
    command = [args.cc, "-std=c11", "-g", "-O1", "-Wall", "-Wextra", "-Werror",
               "-Wno-unused-parameter", "-Wno-sign-compare", "-fsanitize=address,undefined", "-pthread",
               str(output / "test.c"), "-o", str(output / "test")]
    with (output / "build.log").open("w") as log:
        subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True)
    with (output / "test.log").open("w") as log:
        subprocess.run([str(output / "test")], stdout=log, stderr=subprocess.STDOUT, check=True, timeout=30)
    baseline = fragments.replace(function(source, "cam_spidev_write"),
                                 function(data.decode(), "cam_spidev_write"))
    (output / "original-camera.inc").write_text(baseline)
    harness = (output / "test.c").read_text()
    harness = harness.replace('#include "driver-functions.inc"', '#include "original-camera.inc"')
    prefix = harness[:harness.index("int main(void) {")]
    cases = {
        "missing-device": ("unsigned char data[8] = {0}; cam_spidev_write(data, 8);",
                           "runtime error: member access within null pointer"),
        "buffer-ownership": ("""struct device_node node = { .present = 1 };
struct spi_master master = { .bus_num = 0 };
struct spi_device spi = { .dev.of_node = &node, .master = &master, .max_speed_hz = 1000000 };
reset(); assert(!spidev_probe(&spi));
struct spidev_data *state = pvr_dev;
unsigned char owned[8] = {0}, data[8] = {1};
state->tx_buffer = owned;
assert(cam_spidev_write(data, 8) == 8);
assert(state->tx_buffer == owned);""", "state->tx_buffer == owned"),
    }
    negative = {}
    for name, (body, diagnostic) in cases.items():
        test_source = output / (name + ".c")
        executable = output / name
        test_source.write_text(prefix + "int main(void) {\n" + body + "\n}\n")
        build = command[:-3] + ["-Wno-unused-function", "-Wno-unused-variable",
                                str(test_source), "-o", str(executable)]
        with (output / (name + "-build.log")).open("w") as log:
            subprocess.run(build, stdout=log, stderr=subprocess.STDOUT, check=True)
        result = subprocess.run([str(executable)], capture_output=True, text=True, timeout=30)
        (output / (name + ".log")).write_text(result.stdout + result.stderr)
        if result.returncode == 0 or diagnostic not in result.stderr:
            raise ValueError("Original camera defect was not reproduced: " + name)
        negative[name] = {"returncode": result.returncode, "expected_diagnostic": diagnostic}
    report = {"passed": True, "source_sha256": SOURCE_SHA256,
              "patch_sha256": hashlib.sha256(patch.read_bytes()).hexdigest(),
              "patched_sha256": hashlib.sha256(target.read_bytes()).hexdigest(),
              "compile_command": command, "extracted_functions": names, "negative_controls": negative,
              "scope": "Host fault injection of actual driver functions; no physical hardware acceptance"}
    (output / "result.json").write_text(json.dumps(report, indent=2) + "\n")
    print((output / "test.log").read_text(), end="")


if __name__ == "__main__":
    main()
