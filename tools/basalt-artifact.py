#!/usr/bin/env python3
"""Validate and stage a native Basalt bundle without enabling physical tracking."""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import stat
import subprocess
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
INSTALL_PREFIX = 'opt/reframed-vr/basalt'
PREFIX = 'stage/' + INSTALL_PREFIX
BUILD_RECORD = 'usr/share/reframed-vr/basalt-build.json'
spec = importlib.util.spec_from_file_location('basalt_builder', ROOT / 'tools/build-basalt.py')
builder = importlib.util.module_from_spec(spec)
spec.loader.exec_module(builder)


def digest(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def contents(prefix):
    prefix = Path(prefix)
    if prefix.is_symlink() or not prefix.is_dir():
        raise ValueError('Expected a regular Basalt prefix directory')
    tree = {'files': {}, 'directories': {}, 'symlinks': {}, 'prefix_mode': stat.S_IMODE(prefix.stat().st_mode)}
    for path in sorted(prefix.rglob('*')):
        name = str(path.relative_to(prefix))
        if path.is_symlink():
            target = os.readlink(path)
            if Path(target).is_absolute() or not path.is_file() or not path.resolve().is_relative_to(prefix.resolve()):
                raise ValueError('Basalt link escapes its prefix or is broken')
            tree['symlinks'][name] = target
        elif path.is_file():
            tree['files'][name] = {'sha256': digest(path), 'mode': stat.S_IMODE(path.stat().st_mode)}
        elif path.is_dir():
            tree['directories'][name] = stat.S_IMODE(path.stat().st_mode)
        else:
            raise ValueError('Unexpected Basalt artifact type: ' + name)
    if (tree['prefix_mode'] != 0o755 or any(mode != 0o755 for mode in tree['directories'].values()) or
            any(item['mode'] not in (0o644, 0o755) for item in tree['files'].values())):
        raise ValueError('Unexpected Basalt installation permissions')
    return tree


def package(directory):
    directory = Path(directory)
    report = json.loads((directory / 'build.json').read_text())
    profile = json.loads((ROOT / 'profiles/basalt.json').read_text())
    builder.verify_patches(profile)
    if report.get('status') != 'passed' or report.get('physical_tracking_verified') is not False:
        raise ValueError('Expected a verified offline Basalt build')
    if report['profile'] != profile or report['builder_sha256'] != digest(ROOT / 'tools/build-basalt.py'):
        raise ValueError('Basalt source profile or builder changed')
    if digest(directory / 'ctest.xml') != report['ctest_sha256']:
        raise ValueError('Basalt test evidence changed')
    tests = ET.parse(directory / 'ctest.xml').getroot()
    if (int(tests.get('tests', '0')) != profile['test_count'] or
            any(int(tests.get(field, '0')) for field in ('failures', 'errors', 'skipped', 'disabled')) or
            any(t.find('failure') is not None for t in tests.findall('testcase'))):
        raise ValueError('Basalt upstream tests did not pass')
    prefix = directory / PREFIX
    tree = contents(prefix)
    if builder.tree_digest(prefix) != report['stage_sha256']:
        raise ValueError('Basalt stage changed')
    library = prefix / 'lib64/libbasalt.so.2.0.1'
    with library.open('rb') as stream:
        header = stream.read(20)
    if header[:6] != b'\x7fELF\x02\x01' or header[18:20] != b'\xb7\x00':
        raise ValueError('Expected a native ARM64 Basalt library')
    if digest(library) != report['library_sha256'] or digest(prefix / 'include/vit/vit_interface.h') != profile['vit_header_sha256']:
        raise ValueError('Basalt library or VIT interface differs')
    return {'build_sha256': digest(directory / 'build.json'), 'contents': tree,
            'hardware_verified': False, 'automatic_tracking': False}


def root_path(root, name):
    root = Path(root)
    if root.is_symlink() or not root.is_dir() or root.resolve() == Path('/'):
        raise ValueError('Expected an offline staging root')
    path = root / name
    for parent in (path, *path.parents):
        if parent == root:
            break
        if parent.is_symlink():
            raise ValueError('Basalt installation path contains a symlink')
    if not path.resolve().is_relative_to(root.resolve()):
        raise ValueError('Basalt installation path escapes the staging root')
    return path


def verify_install(root, record):
    if contents(root_path(root, INSTALL_PREFIX)) != record['contents']:
        raise ValueError('Installed Basalt contents or modes differ')
    if digest(root_path(root, BUILD_RECORD)) != record['build_sha256']:
        raise ValueError('Installed Basalt build record differs')


def install(directory, root, record):
    directory = Path(directory)
    if digest(directory / 'build.json') != record['build_sha256'] or contents(directory / PREFIX) != record['contents']:
        raise ValueError('Basalt bundle changed before installation')
    prefix, metadata = root_path(root, INSTALL_PREFIX), root_path(root, BUILD_RECORD)
    if prefix.exists() or metadata.exists():
        raise ValueError('Staging root already supplies Basalt')
    prefix.parent.mkdir(parents=True, exist_ok=True)
    metadata.parent.mkdir(parents=True, exist_ok=True)
    shutil.copytree(directory / PREFIX, prefix, symlinks=True)
    shutil.copy2(directory / 'build.json', metadata)
    verify_install(root, record)


def check_loader(root):
    root_path(root, INSTALL_PREFIX)
    code = '''import ctypes,os
lib=ctypes.CDLL('/opt/reframed-vr/basalt/lib64/libbasalt.so.2',mode=os.RTLD_NOW)
version=[ctypes.c_uint32() for _ in range(3)]
fn=lib.vit_api_get_version
fn.argtypes=[ctypes.POINTER(ctypes.c_uint32)]*3
fn.restype=ctypes.c_int
assert fn(*[ctypes.byref(v) for v in version])==0
assert [v.value for v in version]==[2,0,1]
print('Basalt ARM64 RTLD_NOW and VIT 2.0.1: passed')
'''
    return subprocess.check_output(['chroot', str(root), '/usr/bin/python3', '-c', code], text=True, stderr=subprocess.STDOUT, timeout=30).strip()


def verify_image(image, record, output):
    output = Path(output)
    if output.exists():
        raise ValueError('Readback directory already exists')
    (output / 'opt/reframed-vr').mkdir(parents=True)
    (output / 'usr/share/reframed-vr').mkdir(parents=True)
    for command in (f'rdump /{INSTALL_PREFIX} {output}/opt/reframed-vr',
                    f'dump /{BUILD_RECORD} {output}/{BUILD_RECORD}'):
        subprocess.run(['debugfs', '-R', command, str(image)], check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=30)
    verify_install(output, record)
