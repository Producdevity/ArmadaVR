#!/usr/bin/env python3
"""Verify a Pico boot image and panel overlay together without accessing a device."""
import argparse
import hashlib
import importlib.util
import json
import re
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile

from devicetree import read_fdt


def load(name, filename):
    spec = importlib.util.spec_from_file_location(name, Path(__file__).with_name(filename))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


backup = load('pico_backup', 'verify-pico-backup.py')
boot = load('pico_image', 'inspect-boot.py')
overlay = load('pico_overlay', 'bind-pico-overlay.py')
dtbo = load('pico_dtbo', 'repack-dtbo.py')


def base_pack(image):
    layout = boot.layout(image[:4096], len(image))
    if (layout['format'] != 'boot' or layout['header_version'] != 2 or layout['page_size'] != 4096 or
            [s['name'] for s in layout['sections']] != ['kernel', 'ramdisk', 'dtb']):
        raise ValueError('Requires the Pico Android-v2 boot layout')
    part = layout['sections'][-1]
    data = image[part['offset']:part['offset'] + part['size']]
    entries, cursor = [], 0
    while cursor < len(data):
        if len(entries) >= 128 or len(data) - cursor < 40:
            raise ValueError('Invalid base DTB pack')
        magic, size = struct.unpack_from('>II', data, cursor)
        if magic != 0xd00dfeed or not 40 <= size <= min(4 * 1024**2, len(data) - cursor):
            raise ValueError('Invalid base DTB extent')
        entry = data[cursor:cursor + size]
        read_fdt(entry)
        entries.append(entry)
        cursor += size
    if not entries:
        raise ValueError('Empty base DTB pack')
    return data, entries


def verify(paths, hashes, base_index, overlay_index, panel, bridge, display, previous, output):
    if output.exists() or output.is_symlink():
        raise ValueError('Choose a new verification directory')
    names = {'stock_boot', 'boot', 'stock_dtbo', 'dtbo'}
    if set(paths) != names or set(hashes) != names:
        raise ValueError('Requires four independently hashed container inputs')
    data, records = {}, {}
    for name in sorted(names):
        data[name], records[name] = backup.regular(paths[name], 128 * 1024**2, True)
        if records[name]['sha256'] != hashes[name]:
            raise ValueError('Container SHA-256 mismatch: ' + name)
    stock_pack, bases = base_pack(data['stock_boot'])
    if base_pack(data['boot'])[0] != stock_pack:
        raise ValueError('Boot image changed the stock base DTB pack')
    cmdline = boot.layout(data['boot'][:4096], len(data['boot']))['cmdline']
    for parameter in re.split(r'[\s"]+', cmdline):
        name = parameter.split('=', 1)[0].replace('-', '_')
        if name in ('msm_drm.dsi_display0', 'msm_drm.dsi_display1'):
            raise ValueError('Boot command-line panel override bypasses the verified DTBO selection')
    _, stock_entries = dtbo.table(data['stock_dtbo'])
    total, entries = dtbo.table(data['dtbo'])
    if total != len(data['dtbo']):
        raise ValueError('Expected an unsigned DTBO table without a trailer')
    if (type(base_index) is not int or not 0 <= base_index < len(bases) or
            type(overlay_index) is not int or not 0 <= overlay_index < len(stock_entries)):
        raise ValueError('Explicit base or overlay index outside container')
    original = stock_entries[overlay_index]
    with tempfile.TemporaryDirectory(prefix='pico-display-set-') as directory:
        temporary = Path(directory)
        base, source = temporary / 'base.dtb', temporary / 'stock.dtbo'
        base.write_bytes(bases[base_index])
        source.write_bytes(data['stock_dtbo'][original['offset']:original['offset'] + original['bytes']])
        bound = temporary / 'bound'
        binding = overlay.build(base, source, bound, hashlib.sha256(base.read_bytes()).hexdigest(),
                                original['sha256'], panel, bridge, display, previous)
        replacement = (bound / 'panel-mux.dtbo').read_bytes()
        expected, _, _ = dtbo.replace(data['stock_dtbo'], replacement, overlay_index, original['sha256'])
        if data['dtbo'] != expected:
            raise ValueError('DTBO does not contain exactly the requested panel binding and original other entries')
        for name in sorted(names):
            if backup.regular(paths[name], 128 * 1024**2)[1] != records[name]:
                raise ValueError('Input changed during verification: ' + name)
        output.mkdir(parents=True)
        shutil.copyfile(bound / 'panel-mux.dtb', output / 'selected.dtb')
    report = {'status': 'verified', 'scope': 'offline Pico boot/DTBO display binding correspondence',
              'inputs': records, 'base_index': base_index, 'overlay_index': overlay_index,
              'boot_cmdline': cmdline, 'bootloader_cmdline_verified': False,
              'base_count': len(bases), 'overlay_count': len(entries), 'binding': binding,
              'stock_base_pack_unchanged': True, 'other_overlays_unchanged': True,
              'selected_tree_sha256': hashlib.sha256((output / 'selected.dtb').read_bytes()).hexdigest(),
              'device_accessed': False, 'live_selection_verified': False, 'display_verified': False,
              'boot_verified': False, 'recovery_verified': False, 'flash_ready': False,
              'source_sha256': {name: hashlib.sha256(Path(__file__).with_name(name).read_bytes()).hexdigest()
                                for name in ('verify-pico-display-set.py', 'bind-pico-overlay.py',
                                             'bind-pico-panel.py', 'inspect-boot.py', 'devicetree.py',
                                             'verify-pico-backup.py', 'repack-dtbo.py')}}
    (output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('stock-boot', 'boot', 'stock-dtbo', 'dtbo'):
        parser.add_argument('--' + name, type=Path, required=True)
        parser.add_argument('--' + name + '-sha256', required=True)
    for name in ('base-index', 'overlay-index'):
        parser.add_argument('--' + name, type=int, required=True)
    for name in ('panel', 'bridge', 'display', 'expected-default-panel'):
        parser.add_argument('--' + name, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    names = ('stock_boot', 'boot', 'stock_dtbo', 'dtbo')
    try:
        verify({n: getattr(args, n) for n in names}, {n: getattr(args, n + '_sha256') for n in names},
               args.base_index, args.overlay_index, args.panel, args.bridge, args.display,
               args.expected_default_panel, args.output)
    except (OSError, ValueError, KeyError, TypeError, subprocess.SubprocessError) as error:
        parser.exit(1, str(error) + '\n')
    print(args.output / 'report.json')


if __name__ == '__main__':
    main()
