#!/usr/bin/env python3
"""Validate and install a matched systemd runtime in offline staging roots."""
import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import shutil
import stat
import struct

PREFIX = 'opt/reframed-vr/systemd'
EXECUTOR = 'usr/lib/systemd/systemd-executor'
MANAGER = 'usr/lib/systemd/systemd'
VALIDATEFS = 'usr/lib/systemd/systemd-validatefs'
NATIVE_ABI = ('usr/lib/ld-linux-aarch64.so.1', 'usr/lib64/libc.so.6')
ROOT_RETAINED = {
    'usr/bin/systemd-mute-console', 'usr/bin/systemd-pty-forward', 'usr/bin/updatectl',
    'usr/lib/systemd/system-generators/systemd-factory-reset-generator',
    'usr/lib/systemd/system-generators/systemd-rc-local-generator',
    'usr/lib/systemd/system-generators/systemd-sysv-generator',
    'usr/lib/systemd/systemd-factory-reset', 'usr/lib/systemd/systemd-ssh-issue',
    'usr/lib/systemd/systemd-sysupdated', 'usr/lib/systemd/systemd-tpm2-clear', VALIDATEFS,
}
ROOT_MASKS = tuple('systemd-' + service + '-varlink.socket'
                   for service in ('udevd', 'logind', 'networkd', 'resolved'))


def metadata_name(scope):
    return 'initramfs-systemd.json' if scope == 'initramfs' else 'root-systemd.json'


def staging_scope(root):
    if (root / 'etc/initrd-release').is_file():
        return 'initramfs'
    if (root / 'usr/lib/os-release').is_file():
        return 'root'
    raise ValueError('Requires an initramfs staging directory or a full OS staging root')


def digest(path):
    if path.is_symlink() or not path.is_file():
        raise ValueError(f'Expected a regular file: {path}')
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def relative(name):
    if not isinstance(name, str) or not name or name.startswith('/') or any(
            part in ('', '.', '..') for part in name.split('/')):
        raise ValueError(f'Unsafe runtime path: {name!r}')
    return name


def needed(data):
    if data[:4] != b'\x7fELF':
        return []
    if len(data) < 64 or data[:6] != b'\x7fELF\x02\x01' or struct.unpack_from('<H', data, 18)[0] != 183:
        raise ValueError('Expected little-endian ARM64 ELF')
    offset = struct.unpack_from('<Q', data, 32)[0]
    size, count = struct.unpack_from('<HH', data, 54)
    if size != 56 or count > 256 or offset + size * count > len(data):
        raise ValueError('Invalid ELF program headers')
    headers = [struct.unpack_from('<IIQQQQQQ', data, offset + i * size) for i in range(count)]
    for header in headers:
        if header[2] + header[5] > len(data):
            raise ValueError('ELF segment extends beyond file')
    dynamic = next((h for h in headers if h[0] == 2), None)
    if dynamic is None:
        return []
    if dynamic[5] % 16:
        raise ValueError('Invalid ELF dynamic table')
    tags = []
    for i in range(dynamic[2], dynamic[2] + dynamic[5], 16):
        tag = struct.unpack_from('<qQ', data, i)
        if tag[0] == 0:
            break
        tags.append(tag)
    strings = next((v for k, v in tags if k == 5), None)
    length = next((v for k, v in tags if k == 10), None)
    segment = next((h for h in headers if h[0] == 1 and strings is not None and
                    h[3] <= strings and length is not None and strings + length <= h[3] + h[5]), None)
    if segment is None:
        raise ValueError('ELF string table is not file-backed')
    start = segment[2] + strings - segment[3]
    result = []
    for key, value in tags:
        if key != 1:
            continue
        if value >= length:
            raise ValueError('ELF dependency outside string table')
        end = data.find(b'\0', start + value, start + length)
        if end < 0:
            raise ValueError('Unterminated ELF dependency')
        name = data[start + value:end].decode('ascii')
        if not name or '/' in name or name in ('.', '..'):
            raise ValueError('Invalid ELF dependency name')
        result.append(name)
    return result


def is_systemd_unit(data):
    return any(line.startswith(b'#') and b'This file is part of systemd.' in line
               for line in data.splitlines())


def commands(root):
    result = []
    for base in ('usr/bin', 'usr/lib/systemd', 'usr/lib/udev'):
        for path in sorted((root / base).rglob('*')):
            if path.is_symlink() or not path.is_file() or '.so' in path.name:
                continue
            name = path.relative_to(root).as_posix()
            with path.open('rb') as stream:
                if stream.read(4) != b'\x7fELF':
                    continue
            if name == EXECUTOR or any(dep.startswith('libsystemd-shared-') for dep in needed(path.read_bytes())):
                result.append(name)
    if MANAGER not in result or EXECUTOR not in result:
        raise ValueError('Systemd manager and standalone executor are required')
    return result


def validate(bundle):
    bundle = Path(bundle).resolve()
    record = json.loads((bundle / 'manifest.json').read_text())
    scope = record.get('scope')
    if record.get('schema_version') != 1 or scope not in ('initramfs', 'root') or record.get('status') != 'prepared':
        raise ValueError('Requires a prepared initramfs or root systemd runtime')
    allowed_retained = {VALIDATEFS} if scope == 'initramfs' else ROOT_RETAINED
    if record.get('prefix') != PREFIX or set(record.get('retained_commands', [])) - allowed_retained:
        raise ValueError('Unsupported systemd runtime layout')
    masks = record.get('masked_units', [])
    abi = record.get('native_abi_sha256', {})
    if (not isinstance(masks, list) or len(masks) != len(set(masks)) or
            set(masks) - (set(ROOT_MASKS) if scope == 'root' else set()) or
            set(abi) != (set(NATIVE_ABI) if scope == 'root' else set())):
        raise ValueError('Unsupported systemd root runtime contract')
    mask_files = ['etc/systemd/system/' + name for name in masks]
    mask_sources = ['usr/lib/systemd/system/' + name for name in masks]
    command_names = record.get('commands', [])
    unit_names = record.get('units', [])
    for names in (command_names, unit_names, record['retained_commands']):
        if not isinstance(names, list) or len(names) != len(set(names)):
            raise ValueError('Invalid runtime command or unit inventory')
        for name in names:
            relative(name)
    if any(not name.startswith('usr/lib/systemd/system/') or '/' in name.removeprefix('usr/lib/systemd/system/')
           for name in unit_names):
        raise ValueError('Unexpected systemd unit path')
    if set(command_names) & set(record['retained_commands']):
        raise ValueError('Systemd command cannot be replaced and retained')
    if set(unit_names) & set(mask_sources):
        raise ValueError('Systemd unit cannot be replaced and masked')
    if set(record.get('template_sha256', {})) != set(command_names + unit_names + record['retained_commands'] + mask_sources):
        raise ValueError('Incomplete systemd template identity')
    files = record.get('files', [])
    if not files or len(files) > 2048:
        raise ValueError('Invalid systemd runtime inventory')
    names = set()
    for item in files:
        name = relative(item['path'])
        if name in names or not name.startswith('files/'):
            raise ValueError('Duplicate or unexpected runtime file')
        installed_name = name.removeprefix('files/')
        if installed_name not in command_names + unit_names + mask_files and not installed_name.startswith(PREFIX + '/usr/'):
            raise ValueError('Unexpected systemd runtime installation path')
        names.add(name)
        path = bundle / name
        if not path.parent.resolve().is_relative_to(bundle.resolve()):
            raise ValueError('Runtime parent symlink escapes bundle')
        if installed_name in mask_files and item.get('link') != '/dev/null':
            raise ValueError('Invalid unsupported systemd socket mask')
        if 'link' in item:
            target = item['link']
            if installed_name in mask_files:
                if not path.is_symlink() or os.readlink(path) != '/dev/null' or target != '/dev/null':
                    raise ValueError('Invalid unsupported systemd socket mask')
                continue
            if not path.is_symlink() or path.readlink().as_posix() != target or target.startswith('/') or not path.resolve().is_relative_to(bundle / 'files' / PREFIX):
                raise ValueError(f'Invalid private runtime link: {name}')
            if not path.is_file():
                raise ValueError(f'Dangling private runtime link: {name}')
        elif (digest(path) != item['sha256'] or path.stat().st_size != item['bytes'] or
              stat.S_IMODE(path.stat().st_mode) != item['mode'] or item['mode'] & 0o7000):
            raise ValueError(f'Systemd runtime checksum or mode mismatch: {name}')
    actual = {p.relative_to(bundle).as_posix() for p in (bundle / 'files').rglob('*') if not p.is_dir() or p.is_symlink()}
    if actual != names:
        raise ValueError('Unexpected systemd runtime files')
    if any('files/' + name not in names for name in unit_names + mask_files):
        raise ValueError('Missing systemd unit')
    for name in record['commands']:
        relative(name)
        if not name.startswith(('usr/bin/', 'usr/lib/systemd/', 'usr/lib/udev/')):
            raise ValueError('Unexpected systemd command path')
        if 'files/' + name not in names or 'files/' + PREFIX + '/' + name not in names:
            raise ValueError('Missing systemd command or its private counterpart')
        if digest(bundle / 'files' / name) != record['launcher_sha256']:
            raise ValueError('Mismatched systemd launcher')
    if MANAGER not in record['commands'] or EXECUTOR not in record['commands']:
        raise ValueError('Missing systemd manager or executor')
    return record


def install(bundle, root):
    bundle, root = Path(bundle).resolve(), Path(root).resolve()
    record = validate(bundle)
    if root == Path('/') or staging_scope(root) != record['scope'] or (root / PREFIX).exists() or (root / PREFIX).is_symlink():
        raise ValueError('Requires an unmodified staging directory matching the runtime scope')
    inventory = commands(root)
    if set(inventory) != set(record['commands'] + record['retained_commands']):
        raise ValueError('Systemd command inventory changed; prepare a matching runtime')
    for name, sha in record['template_sha256'].items():
        if digest(root / relative(name)) != sha:
            raise ValueError(f'Systemd template changed: {name}')
    for name, sha in record.get('native_abi_sha256', {}).items():
        if digest(root / name) != sha:
            raise ValueError(f'Systemd native ABI changed: {name}')
    if record['scope'] == 'root' and set(record.get('masked_units', [])) != {
            name for name in ROOT_MASKS if (root / 'usr/lib/systemd/system' / name).exists()}:
        raise ValueError('Unsupported systemd socket inventory changed')
    for name in record.get('masked_units', []):
        path = root / 'etc/systemd/system' / name
        if path.exists() or path.is_symlink():
            raise ValueError(f'Existing systemd unit override: {name}')
    for item in record['files']:
        name = relative(item['path']).removeprefix('files/')
        path = root / name
        if not path.parent.resolve().is_relative_to(root):
            raise ValueError('Systemd destination escapes staging root')
        path.parent.mkdir(parents=True, exist_ok=True)
        if path.is_symlink() or path.is_file():
            path.unlink()
        if 'link' in item:
            path.symlink_to(item['link'])
        else:
            shutil.copy2(bundle / item['path'], path)
    metadata = root / 'usr/share/reframed-vr'
    metadata.mkdir(parents=True, exist_ok=True)
    shutil.copy2(bundle / 'manifest.json', metadata / metadata_name(record['scope']))
    verify_install(bundle, root)
    return record


def verify_install(bundle, root):
    bundle, root = Path(bundle).resolve(), Path(root).resolve()
    record = validate(bundle)
    for item in record['files']:
        path = root / item['path'].removeprefix('files/')
        if not path.parent.resolve().is_relative_to(root):
            raise ValueError('Installed runtime path escapes root')
        if 'link' in item:
            if not path.is_symlink() or os.readlink(path) != item['link']:
                raise ValueError('Installed runtime link mismatch')
        elif digest(path) != item['sha256'] or stat.S_IMODE(path.stat().st_mode) != item['mode']:
            raise ValueError(f'Installed runtime file mismatch: {path}')
    if digest(root / 'usr/share/reframed-vr' / metadata_name(record['scope'])) != digest(bundle / 'manifest.json'):
        raise ValueError('Installed runtime metadata mismatch')
    for name, sha in record.get('native_abi_sha256', {}).items():
        if digest(root / name) != sha:
            raise ValueError(f'Installed systemd native ABI changed: {name}')
    return record


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('bundle', type=Path)
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument('--install', type=Path)
    group.add_argument('--verify', type=Path)
    args = parser.parse_args()
    install(args.bundle, args.install) if args.install else verify_install(args.bundle, args.verify)
