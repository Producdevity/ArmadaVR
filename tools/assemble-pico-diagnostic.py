#!/usr/bin/env python3
"""Assemble and roundtrip-check an unsigned Pico diagnostic container offline."""
import argparse
import importlib.util
import json
from pathlib import Path
import stat
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def load(name, filename):
    spec = importlib.util.spec_from_file_location(name, ROOT / 'tools' / filename)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


assembly = load('pico_assembly', 'assemble-quest-boot.py')
diagnostic = load('pico_diagnostic', 'build-pico-diagnostic.py')
container = load('pico_boot', 'pico-boot.py')
digest = assembly.builder.digest
preserve_second_address = container.preserve_second_address
CMDLINE = 'rdinit=/init reframed.pico=diagnostic msm_drm.modeset=0 selinux=1 enforcing=1'


def regular(path, limit):
    info = path.lstat()
    if not stat.S_ISREG(info.st_mode) or not 0 < info.st_size <= limit:
        raise ValueError('Expected a bounded regular file: ' + str(path))


def diagnostic_inputs(directory, kernel_build_sha256, kernel_hashes):
    regular(directory / 'build.json', 1024 * 1024)
    report = json.loads((directory / 'build.json').read_text())
    if (report.get('status') != 'built' or report.get('flash_ready') is not False or
            report.get('physical_boot_verified') is not False or
            report.get('kernel_build_sha256') != kernel_build_sha256 or
            report.get('kernel_inputs') != kernel_hashes or
            report.get('source_sha256') != digest(diagnostic.SOURCE) or
            report.get('cmdline') != CMDLINE):
        raise ValueError('Diagnostic identity, kernel or display guard differs')
    serial = diagnostic.serial(report['boot_serial'])
    expected = {'init', 'initramfs.cpio', 'test-initramfs.cpio'}
    entries = report['artifacts']
    if len(entries) != 3 or {item['path'] for item in entries} != expected:
        raise ValueError('Unexpected diagnostic artifact inventory')
    for item in entries:
        path = directory / item['path']
        regular(path, 16 * 1024 * 1024)
        if path.stat().st_size != item['bytes'] or digest(path) != item['sha256']:
            raise ValueError('Diagnostic artifact changed: ' + item['path'])
    binary = (directory / 'init').read_bytes()
    if (directory / 'initramfs.cpio').read_bytes() != diagnostic.archive(binary, serial):
        raise ValueError('Diagnostic ramdisk does not match its executable and physical serial')
    if (directory / 'test-initramfs.cpio').read_bytes() != diagnostic.archive(binary, 'qemu'):
        raise ValueError('Diagnostic virtual ramdisk differs')
    return report


def run(args):
    reference, kernel, initrd = args.reference.absolute(), args.kernel.resolve(), args.diagnostic.resolve()
    tools, output = args.mkbootimg.absolute(), args.output.absolute()
    if output.exists() or output.is_symlink():
        raise ValueError('Choose a new output directory')
    stock = container.reference_input(reference, args.reference_sha256)
    profile = assembly.checked_tools(tools)
    hashes = diagnostic.kernel_input(kernel)
    kernel_sha = digest(kernel / 'build.json')
    arm64 = assembly.checked_kernel_image(kernel / 'Image', hashes['Image'])
    inputs = diagnostic_inputs(initrd, kernel_sha, hashes)
    initrd_sha = digest(initrd / 'build.json')
    output.mkdir(parents=True)
    report = {'status': 'started', 'scope': 'unsigned offline Pico diagnostic assembly',
              'flash_ready': False, 'physical_boot_verified': False, 'recovery_verified': False,
              'dtb_compatibility_verified': False, 'signatures_created': False, 'device_actions': [],
              'reference_sha256': stock['sha256'], 'reference_authentication': 'expected hash supplied by caller',
              'kernel_build_sha256': kernel_sha, 'kernel_image': arm64,
              'diagnostic_build_sha256': initrd_sha, 'aosp_profile': profile,
              'assembler_sha256': digest(Path(__file__)), 'container_helper_sha256': digest(ROOT / 'tools/pico-boot.py'),
              'commands': []}

    try:
        cmdline = CMDLINE + ' panic=10'
        container.assemble(reference, stock, kernel / 'Image', initrd / 'initramfs.cpio',
                           {'kernel': hashes['Image'], 'ramdisk': digest(initrd / 'initramfs.cpio')},
                           cmdline, tools, output, 'boot-diagnostic-unsigned.img', report)
        if (digest(reference) != stock['sha256'] or digest(kernel / 'build.json') != kernel_sha or
                diagnostic.kernel_input(kernel) != hashes or digest(initrd / 'build.json') != initrd_sha or
                diagnostic_inputs(initrd, kernel_sha, hashes) != inputs or assembly.checked_tools(tools) != profile):
            raise ValueError('Inputs changed during assembly')
        report.update(status='assembled-and-roundtrip-verified', source_inputs_unchanged=True)
    except Exception as error:
        report.update(status='failed', error=str(error))
        raise
    finally:
        (output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(output / 'report.json')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('reference', type=Path, help='authenticated full stock boot partition backup')
    parser.add_argument('kernel', type=Path)
    parser.add_argument('diagnostic', type=Path)
    parser.add_argument('--reference-sha256', required=True)
    parser.add_argument('--mkbootimg', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    try:
        run(parser.parse_args())
    except (OSError, ValueError, RuntimeError, KeyError, TypeError, subprocess.SubprocessError) as error:
        parser.exit(1, f'{error}\n')
