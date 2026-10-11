#!/usr/bin/env python3
"""Reconstruct one full BLOCK OTA partition into a new regular host file."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import stat
import subprocess
import threading
import zipfile


BLOCK = 4096
LIMIT = 8 * 1024**3


def digest(stream):
    stream.seek(0)
    result = hashlib.sha256()
    while data := stream.read(1024**2):
        result.update(data)
    return result.hexdigest()


def layout(data, partition):
    lines = [line.strip() for line in data.decode('ascii').splitlines()
             if line.strip() and not line.lstrip().startswith('#')]
    if not lines or lines[0] != 'remove_all_groups':
        raise ValueError('Requires a full dynamic-partition OTA')
    sizes = [line.split()[2:] for line in lines if line.split()[:2] == ['resize', partition]]
    additions = [line for line in lines if line.split()[:2] == ['add', partition]]
    if len(sizes) != 1 or len(additions) != 1 or len(sizes[0]) != 1 or not sizes[0][0].isdigit():
        raise ValueError('Expected one partition declaration and resize')
    size = int(sizes[0][0])
    if not 0 < size <= LIMIT or size % BLOCK:
        raise ValueError('Partition size must be block aligned and at most 8 GiB')
    return size


def transfers(data, image_bytes):
    lines = data.decode('ascii').splitlines()
    if len(lines) < 5 or lines[0] != '4' or lines[2:4] != ['0', '0'] or not lines[1].isdigit():
        raise ValueError('Requires version 4 full transfers without stashes')
    commands, extents, counts = [], [], {'new': 0, 'zero': 0, 'erase': 0}
    for line in lines[4:]:
        fields = line.split()
        if len(fields) != 2 or fields[0] not in counts:
            raise ValueError('Only new, zero and erase transfers are supported')
        numbers = fields[1].split(',')
        if len(numbers) > 4097 or any(not re.fullmatch(r'[0-9]{1,10}', n) for n in numbers):
            raise ValueError('Invalid or excessive transfer ranges')
        numbers = list(map(int, numbers))
        if numbers[0] != len(numbers) - 1 or numbers[0] < 2 or numbers[0] % 2:
            raise ValueError('Transfer range count mismatch')
        ranges = list(zip(numbers[1::2], numbers[2::2]))
        previous = 0
        for start, end in ranges:
            if not previous <= start < end <= image_bytes // BLOCK:
                raise ValueError('Invalid, unordered or out-of-bounds transfer range')
            previous = end
            extents.append((start, end))
            counts[fields[0]] += end - start
        commands.append((fields[0], ranges))
    previous = 0
    for start, end in sorted(extents):
        if start != previous:
            raise ValueError('Overlapping or uncovered full-image blocks')
        previous = end
    if previous * BLOCK != image_bytes or int(lines[1]) != counts['new'] + counts['zero']:
        raise ValueError('Image extent or declared block count mismatch')
    return commands, counts


def reconstruct(stream, target, commands, image_bytes):
    target.truncate(image_bytes)
    for operation, ranges in commands:
        if operation != 'new':
            continue
        for start, end in ranges:
            target.seek(start * BLOCK)
            remaining = (end - start) * BLOCK
            while remaining:
                data = stream.read(min(remaining, 1024**2))
                if not data:
                    raise ValueError('Truncated decompressed new-data stream')
                target.write(data)
                remaining -= len(data)
    if stream.read(1):
        raise ValueError('Trailing decompressed new data')


def run(args):
    if not re.fullmatch(r'[a-z][a-z0-9_]{0,63}', args.partition):
        raise ValueError('Invalid partition name')
    if not re.fullmatch(r'[0-9a-f]{64}', args.expected_sha256):
        raise ValueError('Requires the independently authenticated OTA SHA-256')
    if args.output.exists() or args.output.is_symlink():
        raise ValueError('Output directory must not exist')
    fd = os.open(args.ota, os.O_RDONLY | os.O_NONBLOCK | os.O_NOFOLLOW)
    with os.fdopen(fd, 'rb') as source:
        before = os.fstat(source.fileno())
        if not stat.S_ISREG(before.st_mode) or not 0 < before.st_size <= 16 * 1024**3:
            raise ValueError('Expected a bounded regular OTA file')
        if digest(source) != args.expected_sha256:
            raise ValueError('OTA SHA-256 mismatch')
        with zipfile.ZipFile(source) as archive:
            entries = archive.infolist()
            if len(entries) > 256 or len({i.filename for i in entries}) != len(entries):
                raise ValueError('Excessive or duplicate ZIP entries')
            if any(i.flag_bits & 1 for i in entries):
                raise ValueError('Encrypted ZIP entries are unsupported')

            def small(name):
                entry = archive.getinfo(name)
                if not 0 < entry.file_size <= 1024**2:
                    raise ValueError('OTA metadata size limit')
                return archive.read(entry)

            metadata = {}
            for line in small('META-INF/com/android/metadata').decode().splitlines():
                key, value = line.split('=', 1)
                if key in metadata:
                    raise ValueError('Duplicate OTA metadata key')
                metadata[key] = value
            if (metadata.get('ota-type') != 'BLOCK' or metadata.get('pre-device') != args.expected_device or
                    metadata.get('post-build-incremental') != args.expected_build):
                raise ValueError('OTA type, device or build mismatch')
            image_bytes = layout(small('dynamic_partitions_op_list'), args.partition)
            transfer = small(args.partition + '.transfer.list')
            commands, counts = transfers(transfer, image_bytes)
            if archive.getinfo(args.partition + '.patch.dat').file_size != 0:
                raise ValueError('Incremental patch data is unsupported')
            compressed = archive.getinfo(args.partition + '.new.dat.br')
            if not 0 < compressed.file_size <= LIMIT:
                raise ValueError('Compressed new-data size limit')
            if shutil.disk_usage(args.output.parent).free < image_bytes + 2 * 1024**3:
                raise ValueError('Need partition size plus 2 GiB free before extraction')
            args.output.mkdir(mode=0o700)
            partial = args.output / (args.partition + '.img.partial')
            errors = []
            with archive.open(compressed) as packed, partial.open('x+b') as target, \
                    (args.output / 'brotli.log').open('xb') as log:
                process = subprocess.Popen([args.brotli, '--decompress', '--stdout'],
                                           stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=log)

                def feed():
                    try:
                        while data := packed.read(1024**2):
                            process.stdin.write(data)
                    except (OSError, EOFError, zipfile.BadZipFile) as error:
                        errors.append(error)
                    finally:
                        try:
                            process.stdin.close()
                        except OSError as error:
                            errors.append(error)

                thread = threading.Thread(target=feed, daemon=True)
                thread.start()
                try:
                    reconstruct(process.stdout, target, commands, image_bytes)
                    if process.wait(timeout=30):
                        raise ValueError('Brotli decompression failed; see brotli.log')
                    thread.join(timeout=30)
                    if thread.is_alive() or errors:
                        raise ValueError('Compressed OTA stream read failed')
                    target.flush()
                    os.fsync(target.fileno())
                    image_hash = digest(target)
                finally:
                    if process.poll() is None:
                        process.kill()
                    process.wait()
                    thread.join(timeout=30)
                    process.stdout.close()
            after = os.fstat(source.fileno())
            if ((before.st_size, before.st_mtime_ns, before.st_ctime_ns) !=
                    (after.st_size, after.st_mtime_ns, after.st_ctime_ns) or digest(source) != args.expected_sha256):
                raise ValueError('OTA changed during extraction')
    image = partial.with_suffix('')
    partial.rename(image)
    report = {'status': 'reconstructed', 'partition': args.partition, 'image_bytes': image_bytes,
              'image_sha256': image_hash, 'ota_sha256': args.expected_sha256, 'metadata': metadata,
              'transfer_sha256': hashlib.sha256(transfer).hexdigest(), 'blocks': counts,
              'erase_representation': 'zero-filled host holes; device discard contents are unspecified',
              'authentication': 'caller-supplied OTA hash; authenticate separately with verify-ota.py',
              'avb_verified': False, 'restore_verified': False, 'flash_ready': False}
    (args.output / 'reconstruction.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('ota', type=Path)
    parser.add_argument('partition')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--expected-sha256', required=True)
    parser.add_argument('--expected-device', required=True)
    parser.add_argument('--expected-build', required=True)
    parser.add_argument('--brotli', default='brotli')
    try:
        run(parser.parse_args())
    except (OSError, ValueError, KeyError, zipfile.BadZipFile, subprocess.SubprocessError) as error:
        parser.exit(1, f'{error}\n')
