#!/usr/bin/env python3
"""Test the pinned compositor's actual display-selection code with Vulkan failures."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess


def function(source, name):
    pattern = r'(?m)^(?:static(?: inline)? )?[^\n;{}]+\n' + re.escape(name) + r'\([^;{}]*\)\n\{'
    match = re.search(pattern, source)
    if not match:
        raise ValueError('Cannot find function definition: ' + name)
    end, depth = match.end(), 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[match.start():end] + '\n'


def run(source, output, baseline=False):
    if output.exists():
        raise ValueError('Choose a new output directory')
    output.mkdir(parents=True)
    common = (source / 'src/xrt/auxiliary/vk/vk_enumerate.c').read_text()
    macros = common[common.index('#define CHECK_FIRST_CALL'):common.index("/*\n *\n * 'Exported' functions.")]
    names = ['vk_enumerate_physical_device_display_properties', 'vk_enumerate_physical_display_plane_properties',
             'vk_enumerate_display_mode_properties']
    if not baseline:
        names.append('vk_enumerate_display_plane_supported_displays')
    (output / 'enumerate-under-test.h').write_text(macros + '\n'.join(function(common, n) for n in names))
    results = []
    for part in ('direct', 'vk_display'):
        path = source / ('src/xrt/compositor/main/comp_window_' + part + '.c')
        text = path.read_text()
        if part == 'direct':
            names = ['get_vk', 'choose_best_vk_mode_auto', 'print_modes', 'get_primary_display_mode', 'choose_alpha_mode']
            if not baseline:
                names.append('choose_display_plane')
            names.append('comp_window_direct_create_surface')
        else:
            names = ['get_vk', 'append_vk_display_entry', 'print_found_displays',
                     'comp_window_vk_display_current_display', 'comp_window_vk_display_init']
        (output / (part + '-under-test.h')).write_text('\n'.join(function(text, n) for n in names))
        executable = output / ('monado-' + part + '-tests')
        command = ['cc', '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                   '-Wno-unused-function', '-Wno-unused-variable', '-fsanitize=address,undefined',
                   '-fno-sanitize-recover=all', '-I', str(output), '-DMONADO_WINDOW_TEST=' + str(int(part == 'vk_display')),
                   '-DMONADO_BASELINE=' + str(int(baseline)), str(Path(__file__).with_suffix('.c')), '-o', str(executable)]
        with (output / (part + '-compile.log')).open('wb') as log:
            subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=60)
        with (output / (part + '-test.log')).open('wb') as log:
            result = subprocess.run([str(executable.resolve())], stdout=log, stderr=subprocess.STDOUT, timeout=20)
        if (result.returncode == 0) == baseline:
            raise RuntimeError('Unexpected ' + part + ' result: ' + str(result.returncode))
        results.append({'part': part, 'returncode': result.returncode, 'command': command})
    report = {'status': 'regressions-reproduced' if baseline else 'passed', 'results': results,
              'scope': 'actual compositor functions with modeled Vulkan; no rendering or hardware timing',
              'source_sha256': {str(p.relative_to(source)): hashlib.sha256(p.read_bytes()).hexdigest() for p in [
                  source / 'src/xrt/auxiliary/vk/vk_enumerate.c',
                  source / 'src/xrt/compositor/main/comp_window_direct.c',
                  source / 'src/xrt/compositor/main/comp_window_vk_display.c']}}
    (output / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
    print(report['status'])


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--baseline', action='store_true')
    args = parser.parse_args()
    run(args.source, args.output, args.baseline)
