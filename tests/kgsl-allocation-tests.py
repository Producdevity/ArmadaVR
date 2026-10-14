#!/usr/bin/env python3
"""Exercise actual KGSL allocation lifetimes with real CPU mappings."""
import argparse
import importlib.util
from pathlib import Path
import re
import shutil
import subprocess


def run(source_path, output):
    spec = importlib.util.spec_from_file_location("fence_tests", Path(__file__).with_name("kgsl-fence-tests.py"))
    helpers = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(helpers)
    source = re.sub(r"^static void kgsl_bo_finish[^;]+;\n", "", source_path.read_text(), flags=re.M)
    names = ("kgsl_bo_user_map", "kgsl_sparse_vma_map", "kgsl_bo_init", "kgsl_bo_finish",
             "kgsl_sparse_vma_init", "kgsl_sparse_vma_finish")
    if "kgsl_bo_free(" in source:
        names = ("kgsl_bo_free", *names)
    output.mkdir(parents=True, exist_ok=True)
    functions = []
    for name in names:
        definition = re.search(r"^static [^;{]*\b" + name + r"\([^;{]*\)\n\{", source, re.M)
        if definition is None:
            raise ValueError("Missing KGSL function: " + name)
        functions.append(helpers.function("\n" + source[definition.start():], name))
    (output / "kgsl-allocation-under-test.h").write_text("\n".join(functions))
    shutil.copy2(source_path.with_name("msm_kgsl.h"), output / "msm_kgsl.h")
    executable = output / "kgsl-allocation-tests"
    subprocess.run(["clang++", "-std=c++17", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                    "-Wno-unused-parameter", "-Wno-missing-field-initializers",
                    "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-I", str(output),
                    str(Path(__file__).with_suffix(".cpp")), "-o", str(executable)], check=True, timeout=60)
    subprocess.run([str(executable.resolve())], check=True, timeout=60)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    run(args.source, args.output)
