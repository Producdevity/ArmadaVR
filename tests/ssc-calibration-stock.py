#!/usr/bin/env python3
"""Compare calibration decoding with the exact saved b1977 protobuf descriptors."""
import argparse
import hashlib
import json
import math
from pathlib import Path
import random
import struct
import subprocess

from google.protobuf import descriptor_pb2, descriptor_pool, message_factory


def digest(data):
    return hashlib.sha256(data).hexdigest()


def run(args):
    library = args.stock_library.read_bytes()
    expected = '9adc9c3c6aee833e3566bf3574844c038098385abd7c94c81aeedfd07edc0fa8'
    if digest(library) != expected:
        raise ValueError('Expected the exact b1977 libssc.so')
    descriptors = [
        ('nanopb.proto', 575989, 836, '72f91e0c896327e9d85e1269ea60655662407490c4cb0ee484368cf4aba5ec32'),
        ('sns_std_sensor.proto', 609204, 2417, '6868756f1e95aea4024c113cde31dc3839354714c24f93a7657028f78d9bc51b'),
        ('sns_cal.proto', 581014, 322, '5b072435bae6353d2656804a4763b39abcd26c05ab94e5a2d9908d8a9ed888c5'),
    ]
    pool = descriptor_pool.DescriptorPool()
    pool.AddSerializedFile(descriptor_pb2.DESCRIPTOR.serialized_pb)
    for name, offset, size, sha in descriptors:
        raw = library[offset:offset + size]
        if digest(raw) != sha:
            raise ValueError('Descriptor changed: ' + name)
        pool.AddSerializedFile(raw)
    message = message_factory.GetMessageClass(pool.FindMessageTypeByName('sns_cal_event'))
    args.output.mkdir(parents=True, exist_ok=False)
    corpus = bytearray()
    rng = random.Random(0x43414c)
    accepted = refused = 0

    def add(payload):
        nonlocal accepted, refused
        value = message.FromString(payload)
        valid = (value.IsInitialized() and len(value.bias) == 3 and
                 len(value.scale_factor) in (0, 3) and len(value.comp_matrix) in (0, 9) and
                 all(math.isfinite(x) for x in [*value.bias, *value.scale_factor, *value.comp_matrix]))
        data = bytes(72)
        if valid:
            flags = bool(value.scale_factor) | bool(value.comp_matrix) << 1 | value.HasField('cal_id') << 2
            data = struct.pack('<15fIII', *value.bias, *(value.scale_factor or [0] * 3),
                               *(value.comp_matrix or [0] * 9), value.status, value.cal_id, flags)
        corpus.extend(struct.pack('<II', len(payload), valid) + data + payload)
        accepted += bool(valid)
        refused += not valid

    for i in range(4096):
        values = struct.unpack('<15f', struct.pack('<15I', *(rng.getrandbits(32) for _ in range(15))))
        fields = {'bias': values[:3], 'status': i % 4}
        if i & 1:
            fields['scale_factor'] = values[3:6]
        if i & 2:
            fields['comp_matrix'] = values[6:]
        if i & 4:
            fields['cal_id'] = rng.getrandbits(32)
        add(message(**fields).SerializeToString())
        parts = [b'\x20' + bytes([i % 4])]
        for field, key in enumerate(('bias', 'scale_factor', 'comp_matrix'), 1):
            data = struct.pack('<' + 'f' * len(fields.get(key, [])), *fields.get(key, []))
            if i & 8:
                parts.append(bytes([field * 8 + 2, len(data)]) + data)
            else:
                parts.extend(bytes([field * 8 + 5]) + data[j:j + 4] for j in range(0, len(data), 4))
        if 'cal_id' in fields:
            parts.append(b'\x2d' + struct.pack('<I', fields['cal_id']))
        parts.append(b'\x9a\x06\x03abc')
        rng.shuffle(parts)
        add(b''.join(parts))
    for bias_count in range(5):
        for scale_count in range(5):
            for matrix_count in range(11):
                add(message(bias=[1] * bias_count, scale_factor=[1] * scale_count,
                            comp_matrix=[1] * matrix_count, status=3).SerializeToString())
    add(message(bias=[1, 2, 3]).SerializePartialToString())
    for number in (float('nan'), float('inf'), -float('inf'), -0.0, 0.0, 2 ** -149):
        add(message(bias=[number, 1, -1], status=0).SerializeToString())
    payload = message(bias=[1, 2, 3], status=0, cal_id=1).SerializeToString()
    add(payload + b'\x20\x03\x2d\xff\xff\xff\xff')
    (args.output / 'corpus.bin').write_bytes(corpus)
    result = subprocess.run([str(args.test_binary.resolve()), str((args.output / 'corpus.bin').resolve())],
                            capture_output=True, text=True, timeout=30)
    (args.output / 'comparison.log').write_text(result.stdout + result.stderr)
    passed = result.returncode == 0 and f'SSC_CALIBRATION_CORPUS_PASS {accepted + refused}' in result.stdout
    report = {'status': 'passed' if passed else 'failed', 'cases': accepted + refused,
              'accepted': accepted, 'refused': refused, 'corpus_sha256': digest(corpus),
              'test_binary_sha256': digest(args.test_binary.read_bytes()), 'stock_library_sha256': expected,
              'descriptors': descriptors, 'physical_calibration_verified': False}
    (args.output / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
    if not passed:
        raise ValueError('Calibration comparison failed: ' + result.stdout + result.stderr)
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--stock-library', type=Path, required=True)
    parser.add_argument('--test-binary', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    run(parser.parse_args())
