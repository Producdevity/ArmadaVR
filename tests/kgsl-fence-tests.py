#!/usr/bin/env python3
"""Exercise pinned Turnip fence helpers with a deterministic descriptor model."""
import argparse
from pathlib import Path
import subprocess


def function(source, name):
    position = source.index(name + "(")
    start = source.rfind("\nstatic ", 0, position) + 1
    body = source.index("{", position)
    depth = 1
    end = body + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end] + "\n"


def run(source_path, output):
    source = source_path.read_text()
    names = ("kgsl_syncobj_init", "kgsl_syncobj_reset", "kgsl_syncobj_destroy",
             "kgsl_syncobj_dup", "timestamp_cmp", "max_ts",
             "kgsl_syncobj_export", "kgsl_syncobj_merge")
    patched = "struct kgsl_syncobj *out)" in function(source, "kgsl_syncobj_merge")
    if not patched:
        names = (*names[:-1], "sync_merge_close", names[-1])
    definitions = source[source.index("enum kgsl_syncobj_state"):
                         source.index("static void\nkgsl_syncobj_init")]
    helpers = definitions + "\n".join(function(source, name) for name in names)
    output.mkdir(parents=True, exist_ok=True)
    (output / "kgsl-sync-under-test.h").write_text(helpers)
    executable = output / "kgsl-fence-tests"
    subprocess.run(["clang++", "-std=c++17", "-O1", "-g", "-Wall", "-Wextra",
                    "-Werror", "-DNDEBUG", "-fsanitize=address,undefined",
                    f"-DPATCHED_KGSL={int(patched)}", "-I", str(output),
                    str(Path(__file__).with_name("kgsl-fence-tests.cpp")),
                    "-o", str(executable)], check=True, timeout=60)
    subprocess.run([str(executable.resolve())], check=True, timeout=20)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    run(args.source, args.output)
