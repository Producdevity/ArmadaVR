"""Shared offline assembly for Pico Android-v2 boot containers."""
import importlib.util
from pathlib import Path
import re
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('boot_assembly', ROOT / 'tools/assemble-quest-boot.py')
assembly = importlib.util.module_from_spec(spec)
spec.loader.exec_module(assembly)
digest = assembly.builder.digest


def reference_input(reference, expected):
    if not re.fullmatch(r'[0-9a-f]{64}', expected):
        raise ValueError('An independently retained reference SHA-256 is required')
    if reference.is_symlink() or not reference.is_file() or not 0 < reference.stat().st_size <= 128 * 1024**2:
        raise ValueError('Expected a bounded regular stock reference')
    if digest(reference) != expected:
        raise ValueError('Stock reference checksum differs')
    stock = assembly.boot.inspect(reference)
    if (stock['format'] != 'boot' or stock['header_version'] != 2 or stock['page_size'] != 4096 or
            [s['name'] for s in stock['sections']] != ['kernel', 'ramdisk', 'dtb']):
        raise ValueError('Requires the Pico Android v2 reference layout')
    return stock


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


def assemble(reference, stock, kernel, ramdisk, expected, cmdline, tools, output, filename, report):
    (output / 'logs').mkdir()

    def call(arguments):
        command = [sys.executable, '-B', *map(str, arguments)]
        index = len(report['commands'])
        record = {'command': command}
        report['commands'].append(record)
        try:
            result = subprocess.run(command, capture_output=True, timeout=60)
        except subprocess.TimeoutExpired as error:
            record['timed_out'] = True
            (output / 'logs' / f'{index}.stdout').write_bytes(error.stdout or b'')
            (output / 'logs' / f'{index}.stderr').write_bytes(error.stderr or b'')
            raise
        record['exit'] = result.returncode
        (output / 'logs' / f'{index}.stdout').write_bytes(result.stdout)
        (output / 'logs' / f'{index}.stderr').write_bytes(result.stderr)
        if result.returncode:
            raise RuntimeError(f'AOSP tool failed; inspect logs/{index}.stderr')
        return result.stdout

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
    assembly.replace(vector, '--kernel', kernel)
    assembly.replace(vector, '--ramdisk', ramdisk)
    assembly.replace(vector, '--cmdline', cmdline)
    image = output / filename
    call([tools / 'mkbootimg.py', *vector, '--output', image])
    preserve_second_address(reference, image)
    info = assembly.boot.inspect(image)
    if (info['header_version'] != 2 or info['page_size'] != 4096 or info['cmdline'] != cmdline or
            info['avb_footer_magic_present'] or info['trailing_bytes'] or
            info['size'] + 69632 >= stock['size']):
        raise ValueError('Unexpected boot header, signature or insufficient partition headroom')
    call([tools / 'unpack_bootimg.py', '--boot_img', image, '--out', output / 'parts'])
    payloads = dict(expected, dtb=next(s['sha256'] for s in stock['sections'] if s['name'] == 'dtb'))
    if any(digest(output / 'parts' / name) != value for name, value in payloads.items()):
        raise ValueError('Boot payload roundtrip differs')
    report.update(artifact=info, cmdline=cmdline, partition_headroom_bytes=stock['size'] - info['size'])
