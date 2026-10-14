#!/usr/bin/env python3
"""Exercise the actual display format lookup with modeled libdrm queries."""
import argparse
import importlib.util
from pathlib import Path
import subprocess


def run(source, output):
    spec = importlib.util.spec_from_file_location("fence_tests", Path(__file__).with_name("kgsl-fence-tests.py"))
    helpers = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(helpers)
    output.mkdir(parents=True, exist_ok=True)
    (output / "display-format-under-test.h").write_text(helpers.function(source.read_text(), "wsi_get_modifiers_for_format"))
    flags = subprocess.check_output(["pkg-config", "--cflags", "libdrm"], text=True).split()
    executable = output / "mesa-display-format-tests"
    subprocess.run(["clang", "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                    "-I", str(output), *flags, str(Path(__file__).with_suffix(".c")),
                    "-o", str(executable)], check=True, timeout=60)
    results = [subprocess.run([str(executable.resolve()), mode], timeout=30).returncode
               for mode in ("formats", "missing-property")]
    if results != [0, 0]:
        raise RuntimeError("Display format regressions: " + str(results))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    run(args.source, args.output)
