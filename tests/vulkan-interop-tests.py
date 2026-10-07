#!/usr/bin/env python3
"""Check Vulkan capability refusal paths using modeled driver responses."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess


def run(output):
    output.mkdir(parents=True, exist_ok=False)
    source = Path(__file__).with_suffix('.c').resolve()
    executable = output.resolve() / 'vulkan-interop-tests'
    command = ['cc', '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
               '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
               str(source), '-o', str(executable)]
    with (output / 'compile.log').open('wb') as log:
        subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=60)
    with (output / 'test.log').open('wb') as log:
        subprocess.run([str(executable)], stdout=log, stderr=subprocess.STDOUT, check=True, timeout=20)
    (output / 'result.json').write_text(json.dumps({
        'status': 'passed', 'command': command,
        'scope': 'modeled Vulkan queries; no driver execution, rendering or hardware proof',
        'sources_sha256': {str(p): hashlib.sha256(p.read_bytes()).hexdigest()
                           for p in (source, source.parents[1] / 'src/vulkan-interop.c')},
    }, indent=2) + '\n')
    print('Vulkan capability refusal tests passed')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    run(parser.parse_args().output)
