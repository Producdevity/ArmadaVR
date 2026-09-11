#!/usr/bin/env python3
"""Exercise the actual KGSL bridge submission wrapper with injected failures."""
import argparse
import importlib.util
from pathlib import Path
import subprocess


def run(source_path, output):
    spec = importlib.util.spec_from_file_location("fences", Path(__file__).with_name("kgsl-fence-tests.py"))
    fences = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(fences)
    output.mkdir(parents=True, exist_ok=True)
    (output / "kgsl-submit-under-test.h").write_text(
        fences.function(source_path.read_text(), "kgsl_queue_submit_drm"))
    executable = output / "kgsl-submit-tests"
    subprocess.run(["clang++", "-std=gnu++17", "-O1", "-g", "-Wall", "-Wextra",
                    "-Werror", "-fsanitize=address,undefined", "-I", str(output),
                    str(Path(__file__).with_name("kgsl-submit-tests.cpp")),
                    "-o", str(executable)], check=True, timeout=60)
    subprocess.run([str(executable.resolve())], check=True, timeout=20)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    run(args.source, args.output)
