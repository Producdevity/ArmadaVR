#!/usr/bin/env python3
"""Assemble and roundtrip-check an unsigned Pico diagnostic container offline."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import stat
import struct
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
digest = assembly.builder.digest
CMDLINE = 'rdinit=/init armada.pico=diagnostic msm_drm.modeset=0 selinux=1 enforcing=1'


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


def preserve_second_address(reference, output):
    original = reference.read_bytes()[:1660]
    with output.open('r+b') as stream:
        header = stream.read(1660)
        if (len(header) != 1660 or len(original) != 1660 or
                struct.unpack_from('<I', original, 40)[0] != 2 or
                struct.unpack_from('<I', header, 40)[0] != 2 or
                struct.unpack_from('<I', original, 24)[0] != 0 or
                struct.unpack_from('<I', header, 24)[0] != 0 or
                struct.unpack_from('<I', header, 28)[0] != 0):
            raise ValueError('Expected v2 headers with an empty second payload')
        # AOSP drops this inactive address when the second payload is empty.
        stream.seek(28)
        stream.write(original[28:32])


def run(args):
    reference, kernel, initrd = args.reference.absolute(), args.kernel.resolve(), args.diagnostic.resolve()
    tools, output = args.mkbootimg.absolute(), args.output.absolute()
    if output.exists() or output.is_symlink():
        raise ValueError('Choose a new output directory')
    if not re.fullmatch(r'[0-9a-f]{64}', args.reference_sha256):
        raise ValueError('An independently retained reference SHA-256 is required')
    regular(reference, 128 * 1024 * 1024)
    if digest(reference) != args.reference_sha256:
        raise ValueError('Stock reference checksum differs')
    stock = assembly.boot.inspect(reference)
    if (stock['format'] != 'boot' or stock['header_version'] != 2 or stock['page_size'] != 4096 or
            [s['name'] for s in stock['sections']] != ['kernel', 'ramdisk', 'dtb']):
        raise ValueError('Requires the Pico Android v2 reference layout')
    profile = assembly.checked_tools(tools)
    hashes = diagnostic.kernel_input(kernel)
    kernel_sha = digest(kernel / 'build.json')
    arm64 = assembly.checked_kernel_image(kernel / 'Image', hashes['Image'])
    inputs = diagnostic_inputs(initrd, kernel_sha, hashes)
    initrd_sha = digest(initrd / 'build.json')
    output.mkdir(parents=True)
    (output / 'logs').mkdir()
    report = {'status': 'started', 'scope': 'unsigned offline Pico diagnostic assembly',
              'flash_ready': False, 'physical_boot_verified': False, 'recovery_verified': False,
              'dtb_compatibility_verified': False, 'signatures_created': False, 'device_actions': [],
              'reference_sha256': stock['sha256'], 'reference_authentication': 'expected hash supplied by caller',
              'kernel_build_sha256': kernel_sha, 'kernel_image': arm64,
              'diagnostic_build_sha256': initrd_sha, 'aosp_profile': profile,
              'assembler_sha256': digest(Path(__file__)), 'commands': []}

    def call(arguments):
        command = [sys.executable, '-B', *map(str, arguments)]
        index = len(report['commands'])
        command_record = {'command': command}
        report['commands'].append(command_record)
        try:
            result = subprocess.run(command, capture_output=True, timeout=60)
        except subprocess.TimeoutExpired as error:
            command_record['timed_out'] = True
            (output / 'logs' / f'{index}.stdout').write_bytes(error.stdout or b'')
            (output / 'logs' / f'{index}.stderr').write_bytes(error.stderr or b'')
            raise
        command_record['exit'] = result.returncode
        (output / 'logs' / f'{index}.stdout').write_bytes(result.stdout)
        (output / 'logs' / f'{index}.stderr').write_bytes(result.stderr)
        if result.returncode:
            raise RuntimeError(f'AOSP tool failed; inspect logs/{index}.stderr')
        return result.stdout

    try:
        vector = assembly.parse_arguments(call([tools / 'unpack_bootimg.py', '--boot_img', reference,
                                               '--out', output / 'reference-parts', '--format', 'mkbootimg', '-0']))
        roundtrip = output / 'stock-roundtrip-unsigned.img'
        call([tools / 'mkbootimg.py', *vector, '--output', roundtrip])
        preserve_second_address(reference, roundtrip)
        with reference.open('rb') as stream:
            if roundtrip.read_bytes() != stream.read(stock['payload_end_aligned']):
                raise ValueError('Stock payload did not roundtrip byte-for-byte')
        report['stock_roundtrip'] = {'bytes': roundtrip.stat().st_size, 'sha256': digest(roundtrip)}
        for name in ('--os_version', '--os_patch_level'):
            index = vector.index(name)
            del vector[index:index + 2]
        cmdline = CMDLINE + ' panic=10'
        assembly.replace(vector, '--kernel', kernel / 'Image')
        assembly.replace(vector, '--ramdisk', initrd / 'initramfs.cpio')
        assembly.replace(vector, '--cmdline', cmdline)
        image = output / 'boot-diagnostic-unsigned.img'
        call([tools / 'mkbootimg.py', *vector, '--output', image])
        preserve_second_address(reference, image)
        info = assembly.boot.inspect(image)
        if (info['header_version'] != 2 or info['page_size'] != 4096 or info['cmdline'] != cmdline or
                info['avb_footer_magic_present'] or info['trailing_bytes'] or
                info['size'] + 69632 >= stock['size']):
            raise ValueError('Unexpected diagnostic header, signature or insufficient partition headroom')
        call([tools / 'unpack_bootimg.py', '--boot_img', image, '--out', output / 'parts'])
        expected = {'kernel': hashes['Image'], 'ramdisk': digest(initrd / 'initramfs.cpio'),
                    'dtb': next(s['sha256'] for s in stock['sections'] if s['name'] == 'dtb')}
        if any(digest(output / 'parts' / name) != value for name, value in expected.items()):
            raise ValueError('Diagnostic payload roundtrip differs')
        if (digest(reference) != stock['sha256'] or digest(kernel / 'build.json') != kernel_sha or
                diagnostic.kernel_input(kernel) != hashes or digest(initrd / 'build.json') != initrd_sha or
                diagnostic_inputs(initrd, kernel_sha, hashes) != inputs or assembly.checked_tools(tools) != profile):
            raise ValueError('Inputs changed during assembly')
        report.update(status='assembled-and-roundtrip-verified', artifact=info, cmdline=cmdline,
                      source_inputs_unchanged=True, partition_headroom_bytes=stock['size'] - info['size'])
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
