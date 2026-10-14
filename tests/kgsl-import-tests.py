#!/usr/bin/env python3
"""Test actual KGSL import functions with fault injection and real descriptors."""
import argparse
import importlib.util
from pathlib import Path
import subprocess


def run(source_path, output):
    spec = importlib.util.spec_from_file_location("fence_tests", Path(__file__).with_name("kgsl-fence-tests.py"))
    helpers = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(helpers)
    source = source_path.read_text()
    names = ("kgsl_bo_init_dmabuf", "kgsl_bo_export_dmabuf")
    output.mkdir(parents=True, exist_ok=True)
    (output / "kgsl-import-under-test.h").write_text("\n".join(helpers.function(source, name) for name in names))
    executable = output / "kgsl-import-tests"
    subprocess.run(["clang++", "-std=c++17", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                    "-Wno-unused-parameter", "-Wno-missing-field-initializers",
                    "-fsanitize=address,undefined", "-I", str(output),
                    str(Path(__file__).with_name("kgsl-import-tests.cpp")),
                    "-o", str(executable)], check=True, timeout=60)
    subprocess.run([str(executable.resolve())], check=True, timeout=30)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    run(args.source, args.output)
