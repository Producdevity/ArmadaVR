#!/usr/bin/env python3
"""Test actual WSI image import/cleanup with modeled DRM and real descriptors."""
import argparse
import importlib.util
from pathlib import Path
import subprocess


def run(source, output):
    spec = importlib.util.spec_from_file_location("fence_tests", Path(__file__).with_name("kgsl-fence-tests.py"))
    helpers = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(helpers)
    output.mkdir(parents=True, exist_ok=True)
    names = ("wsi_display_destroy_buffer", "wsi_display_image_init", "wsi_display_image_finish")
    (output / "display-image-under-test.h").write_text("\n".join(helpers.function(source.read_text(), name) for name in names))
    flags = subprocess.check_output(["pkg-config", "--cflags", "libdrm", "vulkan"], text=True).split()
    executable = output / "mesa-display-image-tests"
    subprocess.run(["clang", "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                    "-I", str(output), *flags, str(Path(__file__).with_suffix(".c")),
                    "-o", str(executable)], check=True, timeout=60)
    subprocess.run([str(executable.resolve())], check=True, timeout=60)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    run(args.source, args.output)
