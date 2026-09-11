#!/usr/bin/env python3
"""Test extracted KGSL wait logic with real poll and pipes, without GPU access."""
import argparse
import importlib.util
from pathlib import Path
import subprocess


def run(source_path, output):
    spec = importlib.util.spec_from_file_location("fences", Path(__file__).with_name("kgsl-fence-tests.py"))
    fences = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(fences)
    source = source_path.read_text()
    helpers = source[source.index("enum kgsl_syncobj_state"):
                     source.index("static void\nkgsl_syncobj_init")]
    old = "#define kgsl_syncobj_foreach_state" in source
    for name in (("timestamp_cmp",) if old else ("timestamp_cmp", "min_ts")):
        helpers += fences.function(source, name)
    if old:
        start = source.index("#define kgsl_syncobj_foreach_state")
        helpers += source[start:source.index("\nstatic VkResult", start)] + "\n"
    helpers += fences.function(source, "kgsl_syncobj_wait_any")
    output.mkdir(parents=True, exist_ok=True)
    (output / "kgsl-wait-under-test.h").write_text(helpers)
    executable = output / "kgsl-wait-tests"
    subprocess.run(["clang++", "-std=c++17", "-O1", "-g", "-Wall", "-Wextra",
                    "-Werror", "-DNDEBUG", "-fsanitize=address,undefined", "-pthread",
                    *(["-Wno-sign-compare", "-Wno-unused-function",
                       "-Wno-missing-field-initializers"] if old else []),
                    f"-DOLD_KGSL_WAIT={int(old)}", "-I", str(output),
                    str(Path(__file__).with_name("kgsl-wait-tests.cpp")),
                    "-o", str(executable)], check=True, timeout=60)
    subprocess.run([str(executable.resolve())], check=True, timeout=20)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    run(args.source, args.output)
