#!/usr/bin/env python3
"""Package and install the metadata-only SSC collector into an offline root."""
import argparse
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('ssc', Path(__file__).with_name('ssc-artifact.py'))
ssc = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ssc)
PREFIX = 'opt/reframed-vr/ssc-inventory'
BUILD_RECORD = 'usr/share/reframed-vr/ssc-inventory-build.json'
INPUTS = ('tools/ssc-inventory.c', 'tools/build-ssc-inventory.py', 'tests/ssc-inventory.c')
LAUNCHER = '''#!/bin/sh
exec /lib/ld-linux-aarch64.so.1 --library-path /opt/reframed-vr/ssc/lib64:/usr/lib64 /opt/reframed-vr/ssc-inventory/libexec/ssc-inventory "$@"
'''


def inputs():
    return {name: ssc.digest(ROOT / name) for name in (*INPUTS, 'tools/ssc-inventory-artifact.py')}


def build(directory, dependency, output):
    directory, dependency, output = Path(directory), Path(dependency), Path(output)
    record = ssc.package(dependency)
    source = directory / 'result.json'
    result = json.loads(source.read_text())
    current = inputs()
    binary = directory / 'ssc-inventory-release'
    if (result.get('status') != 'passed' or result.get('hardware_verified') is not False
            or result.get('ssc_bundle') != record
            or result.get('inputs_sha256') != {n: current[n] for n in INPUTS}
            or result.get('binaries_sha256', {}).get(binary.name) != ssc.digest(binary)
            or binary.is_symlink() or source.is_symlink()):
        raise ValueError('Expected an unchanged verified inventory build and SSC dependency')
    output.mkdir(parents=True, exist_ok=False)
    prefix = output / PREFIX
    for name in ('bin', 'libexec', 'share/source/tools', 'share/source/tests'):
        (prefix / name).mkdir(parents=True, exist_ok=True)
    shutil.copyfile(binary, prefix / 'libexec/ssc-inventory')
    (prefix / 'libexec/ssc-inventory').chmod(0o755)
    (prefix / 'bin/ssc-inventory').write_text(LAUNCHER)
    (prefix / 'bin/ssc-inventory').chmod(0o755)
    for name in current:
        shutil.copyfile(ROOT / name, prefix / 'share/source' / name)
        (prefix / 'share/source' / name).chmod(0o644)
    shutil.copyfile(source, prefix / 'share/source/result.json')
    (prefix / 'share/source/result.json').chmod(0o644)
    report = {'status': 'packaged', 'inputs_sha256': current, 'ssc_dependency': record,
              'collector_sha256': ssc.digest(binary), 'source_result_sha256': ssc.digest(source),
              'contents': ssc.contents(prefix), 'hardware_verified': False,
              'flash_ready': False, 'automatic_startup': False}
    if (inputs() != current or ssc.package(dependency) != record
            or ssc.digest(binary) != report['collector_sha256']
            or ssc.digest(source) != report['source_result_sha256']):
        raise ValueError('Inventory build inputs changed during packaging')
    (output / 'build.json').write_text(json.dumps(report, indent=2) + '\n')
    return package(output, record)


def package(directory, dependency):
    directory = Path(directory)
    report = json.loads((directory / 'build.json').read_text())
    if (report.get('status') != 'packaged' or report.get('hardware_verified') is not False
            or report.get('flash_ready') is not False or report.get('automatic_startup') is not False
            or report.get('ssc_dependency') != dependency or report.get('inputs_sha256') != inputs()):
        raise ValueError('Inventory package or SSC dependency differs')
    prefix = directory / PREFIX
    tree = ssc.contents(prefix)
    expected = {'bin/ssc-inventory', 'libexec/ssc-inventory', 'share/source/result.json',
                *('share/source/' + name for name in inputs())}
    if tree != report.get('contents') or set(tree['files']) != expected or tree['links']:
        raise ValueError('Unexpected inventory package contents')
    for name, item in tree['files'].items():
        if item['mode'] != (0o755 if name.startswith(('bin/', 'libexec/')) else 0o644):
            raise ValueError('Unexpected inventory package mode')
    for name, value in inputs().items():
        if ssc.digest(prefix / 'share/source' / name) != value:
            raise ValueError('Inventory packaged source differs')
    binary = prefix / 'libexec/ssc-inventory'
    with binary.open('rb') as stream:
        header = stream.read(20)
    if header[:7] != b'\x7fELF\x02\x01\x01' or header[18:20] != b'\xb7\x00':
        raise ValueError('Expected an ARM64 inventory collector')
    result_path = prefix / 'share/source/result.json'
    result = json.loads(result_path.read_text())
    if (ssc.digest(binary) != report['collector_sha256']
            or ssc.digest(result_path) != report['source_result_sha256']
            or result.get('status') != 'passed' or result.get('hardware_verified') is not False
            or result.get('ssc_bundle') != dependency
            or result.get('inputs_sha256') != {n: inputs()[n] for n in INPUTS}
            or result.get('binaries_sha256', {}).get('ssc-inventory-release') != ssc.digest(binary)
            or (prefix / 'bin/ssc-inventory').read_text() != LAUNCHER):
        raise ValueError('Collector or verified build result differs')
    return {'build_sha256': ssc.digest(directory / 'build.json'), 'contents': tree,
            'ssc_dependency': dependency, 'automatic_startup': False,
            'hardware_verified': False, 'flash_ready': False}


def verify_install(root, record):
    ssc.verify_install(root, record['ssc_dependency'])
    if (ssc.contents(ssc.root_path(root, PREFIX)) != record['contents']
            or ssc.digest(ssc.root_path(root, BUILD_RECORD)) != record['build_sha256']):
        raise ValueError('Installed inventory files or build record differ')


def install(directory, root, record):
    directory = Path(directory)
    ssc.verify_install(root, record['ssc_dependency'])
    if (ssc.digest(directory / 'build.json') != record['build_sha256']
            or ssc.contents(directory / PREFIX) != record['contents']):
        raise ValueError('Inventory bundle changed before installation')
    prefix, metadata = ssc.root_path(root, PREFIX), ssc.root_path(root, BUILD_RECORD)
    if prefix.exists() or metadata.exists():
        raise ValueError('Root already supplies the inventory collector')
    prefix.parent.mkdir(parents=True, exist_ok=True)
    metadata.parent.mkdir(parents=True, exist_ok=True)
    shutil.copytree(directory / PREFIX, prefix)
    shutil.copyfile(directory / 'build.json', metadata)
    metadata.chmod(0o644)
    verify_install(root, record)


def check_loader(root):
    root = Path(root).absolute()
    ssc.root_path(root, PREFIX)

    def enter_root():
        import os
        os.chroot(root)
        os.chdir('/')

    result = subprocess.run(['/lib/ld-linux-aarch64.so.1', '--library-path',
                             '/opt/reframed-vr/ssc/lib64:/usr/lib64', '/' + PREFIX + '/libexec/ssc-inventory'],
                            preexec_fn=enter_root, env={'LC_ALL': 'C', 'LD_TRACE_LOADED_OBJECTS': '1',
                            'LD_WARN': 'yes', 'LD_BIND_NOW': 'yes', 'LD_VERBOSE': 'yes'},
                            capture_output=True, text=True, timeout=15)
    trace = result.stdout + result.stderr
    if (result.returncode or 'not found' in trace or 'undefined symbol' in trace
            or '/opt/reframed-vr/ssc/lib64/libqmi-glib.so.' not in trace):
        raise ValueError('Inventory root loader or relocations failed: ' + trace)
    return {'status': 'passed', 'trace': trace, 'physical_sensor_access': False}


def verify_image(image, record, destination):
    destination = Path(destination)
    if any(c in str(destination) for c in ('"', '\\', '\n', '\r')):
        raise ValueError('Invalid inventory readback directory')
    destination.mkdir(parents=True, exist_ok=False)
    for name in (PREFIX, BUILD_RECORD, ssc.INSTALL_PREFIX, ssc.BUILD_RECORD):
        parent = destination / Path(name).parent
        parent.mkdir(parents=True, exist_ok=True)
        subprocess.run(['debugfs', '-R', f'rdump /{name} "{parent}"', str(image)],
                       check=True, timeout=30, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    verify_install(destination, record)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('build', type=Path)
    parser.add_argument('--ssc-bundle', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    print(json.dumps(build(args.build, args.ssc_bundle, args.output), indent=2))
