#!/usr/bin/env python3
"""Exercise the actual KGSL display helper with injected libdrm failures."""
import argparse
from pathlib import Path
import shutil
import subprocess


def run(header, output):
    output.mkdir(parents=True, exist_ok=True)
    shutil.copy2(header, output / 'tu_kgsl_display.h')
    executable = output / 'kgsl-display-tests'
    flags = subprocess.check_output(['pkg-config', '--cflags', 'libdrm'], text=True).split()
    subprocess.run(['clang++', '-std=gnu++17', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-I', str(output), *flags,
                    str(Path(__file__).with_suffix('.cpp')), '-o', str(executable)], check=True, timeout=60)
    subprocess.run([str(executable.resolve())], check=True, timeout=20)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('header', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    run(args.header, args.output)
