#!/usr/bin/env python3
"""Inspect a selected Pico IMU's saved SSC registry; never apply calibration."""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import re
import stat


def require(condition, message):
    if not condition:
        raise ValueError(message)


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        require(key not in result, 'Duplicate registry key: ' + key)
        result[key] = value
    return result


def read_record(path):
    fd = os.open(path, os.O_RDONLY | os.O_NONBLOCK | os.O_NOFOLLOW)
    with os.fdopen(fd, 'rb') as stream:
        info = os.fstat(stream.fileno())
        require(stat.S_ISREG(info.st_mode) and 0 < info.st_size <= 65536,
                'Expected a bounded regular registry file')
        data = stream.read(65537)
        require(len(data) == info.st_size, 'Registry size changed during inspection')
    return data


def group(data, name, owner, fields, kind):
    require(0 < len(data) <= 65536, 'Registry record exceeds size limit')
    document = json.loads(data, object_pairs_hook=unique_object)
    require(isinstance(document, dict) and set(document) == {name}, 'Registry group identity differs')
    record = document[name]
    require(isinstance(record, dict) and set(record) == {'owner', *fields} and
            record['owner'] == owner, 'Registry owner or fields differ')
    values, versions = [], []
    for field in fields:
        item = record[field]
        require(isinstance(item, dict) and set(item) == {'type', 'ver', 'data'} and
                item['type'] == kind, 'Registry value type differs: ' + field)
        version, value = item['ver'], item['data']
        require(isinstance(version, str) and re.fullmatch(r'[0-9]{1,10}', version) and
                int(version) <= 2**32 - 1, 'Invalid registry version')
        require(isinstance(value, str) and len(value) <= 128, 'Invalid registry value')
        if kind == 'flt':
            require(re.fullmatch(r'[+-]?(?:[0-9]+(?:\.[0-9]*)?|\.[0-9]+)(?:[eE][+-]?[0-9]+)?', value),
                    'Expected a decimal calibration coefficient')
            value = float(value)
            require(math.isfinite(value), 'Non-finite calibration coefficient')
        values.append(value)
        versions.append(int(version))
    return {'values': values, 'versions': versions}


def determinant(values):
    a, b, c, d, e, f, g, h, i = values
    return a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g)


def inspect(directory, sensor):
    require(sensor in ('icm4x6xx_0', 'lsm6dst_0', 'lsm6dst_1'), 'Unsupported explicit sensor selection')
    directory = Path(directory)
    require(directory.is_dir() and not directory.is_symlink(), 'Expected an offline registry directory')
    owner = sensor.rsplit('_', 1)[0]
    hashes = {}

    def load(suffix, fields, kind='flt'):
        name = sensor + '_platform.' + suffix
        data = read_record(directory / name)
        result = group(data, name, owner, fields, kind)
        hashes[name] = hashlib.sha256(data).hexdigest()
        return result

    sensors = {}
    for kind in ('accel', 'gyro'):
        bias = load(kind + '.fac_cal.bias', ['x', 'y', 'z'])
        correction = load(kind + '.fac_cal.corr_mat', [f'{i}_{j}' for i in range(3) for j in range(3)])
        det = determinant(correction['values'])
        require(math.isfinite(det) and abs(det) > 1e-8, 'Singular or unbounded IMU correction matrix')
        sensors[kind] = {'bias': bias, 'correction_matrix_row_major': correction, 'determinant': det}
    orient = load('orient', ['x', 'y', 'z'], 'str')
    require(all(re.fullmatch(r'[+-][xyz]', v) for v in orient['values']) and
            len({v[1] for v in orient['values']}) == 3, 'Invalid signed axis permutation')
    placement = load('placement', [str(i) for i in range(12)])
    for name, digest in hashes.items():
        require(hashlib.sha256(read_record(directory / name)).hexdigest() == digest,
                'Registry changed during inspection: ' + name)
    return {'schema': 1, 'status': 'inspected', 'sensor': sensor, 'owner': owner,
            'files_sha256': hashes, 'sensors': sensors, 'orientation_fields': orient,
            'placement_fields': placement, 'placement_all_zero': not any(placement['values']),
            'calibration_applied': False, 'runtime_usable': False,
            'unverified': ['live SUID association and unit identity',
                           'whether SSC output already includes factory or temperature correction',
                           'bias/matrix application order and units',
                           'orientation direction, placement layout and headset coordinate frame',
                           'camera extrinsics and camera/IMU time alignment']}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('registry', type=Path)
    parser.add_argument('--sensor', choices=('icm4x6xx_0', 'lsm6dst_0', 'lsm6dst_1'), required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    result = inspect(args.registry, args.sensor)
    fd = os.open(args.output, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, 'w') as stream:
        json.dump(result, stream, indent=2, allow_nan=False)
        stream.write('\n')
    print('Inspected saved registry; calibration remains unapplied: ' + str(args.output))


if __name__ == '__main__':
    main()
