#!/usr/bin/env python3
"""Validate a native Monado bundle before packaging or testing it."""
import hashlib
import importlib.util
import json
import os
import shutil
import stat
from pathlib import Path
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parent.parent
INSTALL_PREFIX = 'opt/reframed-vr/monado'
PREFIX = 'stage/' + INSTALL_PREFIX
BUILD_RECORD = 'usr/share/reframed-vr/monado-build.json'


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def validate(directory):
    directory = Path(directory)
    report = json.loads((directory / 'build.json').read_text())
    if report.get('status') != 'compiled' or report.get('hardware_verified') is not False:
        raise ValueError('Expected an offline compiled Monado bundle')
    identity = report['identity']
    if identity['profile'] != json.loads((ROOT / 'profiles/monado.json').read_text()):
        raise ValueError('Monado source profile changed since compilation')
    patches = [{'path': str(p.relative_to(ROOT)), 'sha256': digest(p)}
               for p in sorted((ROOT / 'patches/monado').glob('*.patch'))]
    if not patches or identity['patches'] != patches:
        raise ValueError('Monado patches changed since compilation')
    if identity['builder_sha256'] != digest(ROOT / 'tools/monado-build.py'):
        raise ValueError('Monado builder changed since compilation')
    if identity.get('pico_sources') != {name: digest(ROOT / 'src' / name)
                                       for name in identity['profile']['pico_driver_sources']}:
        raise ValueError('Pico driver build sources changed')
    if identity.get('quest_sources') != {name: digest(ROOT / 'src' / name)
                                        for name in identity['profile']['quest_driver_sources']}:
        raise ValueError('Quest tracking build sources changed')
    for name in ('monado-display-tests.py', 'monado-display-tests.c', 'monado-space-tests.c', 'monado-projection-tests.c'):
        if identity['tests'][name] != digest(ROOT / 'tests' / name):
            raise ValueError('Monado regression tests changed since compilation')
    files, links = report['files_sha256'], report['symlinks']
    for name in (*files, *links):
        if not name or Path(name).is_absolute() or '..' in Path(name).parts:
            raise ValueError('Invalid Monado artifact path')
    actual_files = {str(p.relative_to(directory)) for p in directory.rglob('*')
                    if p.is_file() and not p.is_symlink() and p != directory / 'build.json'}
    actual_links = {str(p.relative_to(directory)): os.readlink(p) for p in directory.rglob('*') if p.is_symlink()}
    if actual_files != set(files) or actual_links != links:
        raise ValueError('Monado artifact inventory changed')
    for name, sha in files.items():
        if digest(directory / name) != sha:
            raise ValueError('Monado artifact hash mismatch: ' + name)
    for name in links:
        path = directory / name
        if not path.resolve().is_relative_to((directory / PREFIX).resolve()) or not path.is_file():
            raise ValueError('Monado symlink escapes the install prefix or is broken')
    prefix = directory / PREFIX
    for name in ('bin/monado-service', 'bin/monado-cli', 'lib64/libopenxr_monado.so', 'lib64/libmonado.so'):
        data = (prefix / name).read_bytes()
        if data[:6] != b'\x7fELF\x02\x01' or data[18:20] != b'\xb7\x00':
            raise ValueError('Monado is not a native ARM64 ELF: ' + name)
    if b'Unable to select a compatible display plane' not in (prefix / 'bin/monado-service').read_bytes():
        raise ValueError('Monado service omits the display selection patch')
    if b'XRT_COMPOSITOR_FORCE_DISPLAY_MODE' not in (prefix / 'bin/monado-service').read_bytes():
        raise ValueError('Monado service omits exact display mode selection')
    manifest = json.loads((prefix / 'share/openxr/1/openxr_monado.json').read_text())
    if manifest['runtime']['library_path'] != '/opt/reframed-vr/monado/lib64/libopenxr_monado.so':
        raise ValueError('Unexpected Monado OpenXR runtime path')
    if '#define VK_USE_PLATFORM_DISPLAY_KHR' not in (directory / 'display-preprocessor.txt').read_text():
        raise ValueError('Monado build omitted direct-display support')
    cache = (directory / 'CMakeCache.txt').read_text().splitlines()
    if any(name + ':BOOL=ON' not in cache for name in identity['profile']['required_features']):
        raise ValueError('Monado build omitted required runtime features')
    library = prefix / 'lib64/libquest_tracking.so'
    data = library.read_bytes()
    if data[:6] != b'\x7fELF\x02\x01' or data[18:20] != b'\xb7\x00':
        raise ValueError('Installed Quest runtime is not native ARM64')
    exported = {line.split()[-1] for line in (directory / 'quest-runtime-exports.txt').read_text().splitlines() if line.split()}
    if set(identity['profile']['quest_runtime_exports']) != exported:
        raise ValueError('Installed Quest runtime exports differ')
    dependencies = (directory / 'quest-runtime-dynamic.txt').read_text()
    if any(name in dependencies for name in ('libpico_tracking.so', 'libssc.so')):
        raise ValueError('Quest runtime depends on Pico sensor services')
    if 'QUEST_RUNTIME_LIBRARY_PASS invalid_input_refused=1 device_actions=0' not in (directory / 'quest-runtime-load.txt').read_text():
        raise ValueError('Installed Quest runtime loading was not verified')
    pico_enabled = 'XRT_BUILD_DRIVER_PICO_SSC:BOOL=ON' in cache
    if pico_enabled != ('ssc_dependency' in report):
        raise ValueError('Monado SSC dependency record disagrees with build configuration')
    if pico_enabled:
        library = prefix / 'lib64/libpico_tracking.so'
        data = library.read_bytes()
        if data[:6] != b'\x7fELF\x02\x01' or data[18:20] != b'\xb7\x00':
            raise ValueError('Installed Pico runtime is not native ARM64')
        if any(marker not in data for marker in (b'PICO_SSC_CONFIG', b'PICO_HMD_CONFIG')):
            raise ValueError('Installed Pico runtime omits its probers')
        exported = {line.split()[-1] for line in (directory / 'pico-runtime-exports.txt').read_text().splitlines() if line.split()}
        if not set(identity['profile']['pico_runtime_exports']) <= exported:
            raise ValueError('Installed Pico runtime omits required entry points')
        for name in ('monado-service', 'monado-cli'):
            if 'Shared library: [libpico_tracking.so]' not in (directory / (name + '-dynamic.txt')).read_text():
                raise ValueError('Installed executable omits the Pico runtime dependency')
        if 'PICO_RUNTIME_LIBRARY_PASS invalid_input_refused=1 device_actions=0' not in (directory / 'pico-runtime-load.txt').read_text():
            raise ValueError('Installed Pico runtime loading was not verified')
    tests = ET.parse(directory / 'ctest.xml').getroot()
    if (int(tests.get('tests', '0')) == 0 or int(tests.get('failures', '-1')) != 0 or
            int(tests.get('skipped', '0')) != 0 or any(t.find('failure') is not None for t in tests.findall('testcase'))):
        raise ValueError('Monado upstream tests did not pass')
    result = directory / 'display-tests/result.json'
    if digest(result) != report['display_tests_sha256'] or json.loads(result.read_text())['status'] != 'passed':
        raise ValueError('Monado display regression tests did not pass')
    record = {'build_sha256': digest(directory / 'build.json'), 'prefix': str(prefix),
              'ctest_executables': int(tests.get('tests')), 'hardware_verified': False,
              'default_runtime': False}
    if pico_enabled:
        record['ssc_dependency'] = report['ssc_dependency']
    return record


def contents(prefix):
    prefix = Path(prefix)
    if prefix.is_symlink() or not prefix.is_dir():
        raise ValueError('Expected a regular Monado prefix directory')
    result = {'files': {}, 'directories': {}, 'symlinks': {}, 'prefix_mode': stat.S_IMODE(prefix.stat().st_mode)}
    for path in sorted(prefix.rglob('*')):
        name = str(path.relative_to(prefix))
        if path.is_symlink():
            if (Path(os.readlink(path)).is_absolute() or not path.is_file() or
                    not path.resolve().is_relative_to(prefix.resolve())):
                raise ValueError('Monado library link is broken or escapes its prefix: ' + name)
            result['symlinks'][name] = os.readlink(path)
        elif path.is_file():
            result['files'][name] = {'sha256': digest(path), 'mode': stat.S_IMODE(path.stat().st_mode)}
        elif path.is_dir():
            result['directories'][name] = stat.S_IMODE(path.stat().st_mode)
        else:
            raise ValueError('Unexpected Monado file type: ' + name)
    return result


def package(directory):
    directory = Path(directory)
    record = validate(directory)
    record['contents'] = contents(directory / PREFIX)
    tree = record['contents']
    if (tree['prefix_mode'] != 0o755 or any(mode != 0o755 for mode in tree['directories'].values()) or
            any(item['mode'] not in (0o644, 0o755) for item in tree['files'].values())):
        raise ValueError('Unexpected Monado installation permissions')
    for name, item in tree['files'].items():
        if name.startswith('bin/') and item['mode'] != 0o755:
            raise ValueError('Monado executable lost its executable mode: ' + name)
    return record


def root_path(root, name):
    root = Path(root)
    if not root.is_dir() or root.resolve() == Path('/'):
        raise ValueError('Expected an offline staging root')
    path = root / name
    for parent in (path, *path.parents):
        if parent == root:
            break
        if parent.is_symlink():
            raise ValueError('Monado installation path contains a symlink: ' + str(parent))
    if not path.resolve().is_relative_to(root.resolve()):
        raise ValueError('Monado installation path escapes the staging root')
    return path


def verify_install(root, record):
    if contents(root_path(root, INSTALL_PREFIX)) != record['contents']:
        raise ValueError('Installed Monado inventory, permissions or hashes differ')
    metadata = root_path(root, BUILD_RECORD)
    if not metadata.is_file() or digest(metadata) != record['build_sha256']:
        raise ValueError('Installed Monado build record differs')


def install(directory, root, record):
    directory = Path(directory)
    if (digest(directory / 'build.json') != record['build_sha256'] or
            contents(directory / PREFIX) != record['contents']):
        raise ValueError('Monado bundle changed before installation')
    prefix, metadata = root_path(root, INSTALL_PREFIX), root_path(root, BUILD_RECORD)
    if prefix.exists() or metadata.exists():
        raise ValueError('Staging root already supplies the selected Monado bundle')
    if 'ssc_dependency' in record:
        spec = importlib.util.spec_from_file_location('monado_ssc', Path(__file__).with_name('ssc-artifact.py'))
        ssc = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(ssc)
        ssc.verify_install(root, record['ssc_dependency'])
    prefix.parent.mkdir(parents=True, exist_ok=True)
    metadata.parent.mkdir(parents=True, exist_ok=True)
    shutil.copytree(directory / PREFIX, prefix, symlinks=True)
    shutil.copy2(directory / 'build.json', metadata)
    verify_install(root, record)
