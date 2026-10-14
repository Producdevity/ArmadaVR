#!/usr/bin/env python3
"""Test the pinned compositor's actual display selection and semantic-space ownership."""
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


def run(source, output, baseline=False, mode_baseline=False):
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
            names = ['get_vk', 'choose_best_vk_mode_auto', 'print_modes']
            if not baseline and not mode_baseline:
                names += ['parse_display_mode', 'choose_exact_vk_mode']
            names += ['get_primary_display_mode', 'choose_alpha_mode']
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
        if (result.returncode == 0) == (baseline or (mode_baseline and part == 'direct')):
            raise RuntimeError('Unexpected ' + part + ' result: ' + str(result.returncode))
        results.append({'part': part, 'returncode': result.returncode, 'command': command})
    path = source / 'src/xrt/ipc/client/ipc_client_space_overseer.c'
    text = path.read_text()
    structures = []
    for name in ('ipc_client_space', 'ipc_client_space_overseer'):
        match = re.search(r'struct ' + name + r'\n\{.*?\n\};', text, re.S)
        if not match:
            raise ValueError('Missing IPC space structure: ' + name)
        structures.append(match.group())
    names = ['ipc_client_space', 'ipc_client_space_overseer', 'space_destroy',
             'alloc_space_with_id', 'destroy', 'ipc_client_space_overseer_create']
    (output / 'space-under-test.h').write_text('\n'.join(structures) + '\n' +
        '\n'.join(function(text, name) for name in names))
    executable = output / 'monado-space-tests'
    command = ['cc', '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
               '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
               '-I', str(output), '-isystem', str(source / 'src/xrt/include'),
               str(Path(__file__).with_name('monado-space-tests.c')), '-o', str(executable)]
    with (output / 'space-compile.log').open('wb') as log:
        subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=60)
    with (output / 'space-test.log').open('wb') as log:
        result = subprocess.run([str(executable.resolve())], stdout=log, stderr=subprocess.STDOUT, timeout=30)
    if result.returncode != (1 if baseline else 0) or 'MONADO_SPACE_CASES=117649 ' not in (output / 'space-test.log').read_text():
        raise RuntimeError('Unexpected semantic-space ownership result: ' + str(result.returncode))
    results.append({'part': 'space', 'returncode': result.returncode, 'command': command})
    path = source / 'src/xrt/compositor/main/comp_compositor.c'
    (output / 'projection-under-test.h').write_text(function(path.read_text(), 'can_do_one_projection_layer_fast_path'))
    (output / 'xrt').mkdir()
    (output / 'xrt/xrt_config_have.h').write_text('/* No optional graphics API headers needed by this test. */\n')
    executable = output / 'monado-projection-tests'
    command = ['cc', '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
               '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
               '-I', str(output), '-isystem', str(source / 'src/xrt/include'),
               str(Path(__file__).with_name('monado-projection-tests.c')), '-o', str(executable)]
    with (output / 'projection-compile.log').open('wb') as log:
        subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=60)
    with (output / 'projection-test.log').open('wb') as log:
        result = subprocess.run([str(executable.resolve())], stdout=log, stderr=subprocess.STDOUT, timeout=20)
    if result.returncode != (1 if baseline else 0):
        raise RuntimeError('Unexpected projection path result: ' + str(result.returncode))
    results.append({'part': 'projection', 'returncode': result.returncode, 'command': command})
    report = {'status': 'regressions-reproduced' if baseline or mode_baseline else 'passed', 'results': results,
              'scope': 'actual compositor functions with modeled Vulkan and IPC, real space references; no rendering or hardware timing',
              'source_sha256': {str(p.relative_to(source)): hashlib.sha256(p.read_bytes()).hexdigest() for p in [
                  source / 'src/xrt/auxiliary/vk/vk_enumerate.c',
                  source / 'src/xrt/compositor/main/comp_window_direct.c',
                  source / 'src/xrt/compositor/main/comp_window_vk_display.c',
                  source / 'src/xrt/ipc/client/ipc_client_space_overseer.c',
                  source / 'src/xrt/include/xrt/xrt_space.h',
                  source / 'src/xrt/include/xrt/xrt_compositor.h',
                  source / 'src/xrt/compositor/main/comp_compositor.c',
                  source / 'src/xrt/compositor/util/comp_render_cs.c']}}
    (output / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
    print(report['status'])


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--baseline', action='store_true')
    parser.add_argument('--mode-baseline', action='store_true')
    args = parser.parse_args()
    if args.baseline and args.mode_baseline:
        parser.error('Choose only one baseline')
    run(args.source, args.output, args.baseline, args.mode_baseline)
