#!/usr/bin/env python3
"""Inspect saved Neo3 optical parameters without producing runtime calibration."""
import argparse
import configparser
import hashlib
import json
import math
import os
from pathlib import Path
import re
import stat


DECIMAL = r'[+-]?(?:[0-9]+(?:\.[0-9]*)?|\.[0-9]+)(?:[eE][+-]?[0-9]+)?'


def read_file(path, expected, limit):
    if not re.fullmatch(r'[0-9a-f]{64}', expected):
        raise ValueError('Expected an independently retained SHA-256')
    fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
    info = os.fstat(fd)
    if not stat.S_ISREG(info.st_mode) or not 0 < info.st_size <= limit:
        os.close(fd)
        raise ValueError('Expected a bounded regular calibration file')
    with os.fdopen(fd, 'rb') as stream:
        data = stream.read(limit + 1)
        if len(data) != info.st_size or hashlib.sha256(data).hexdigest() != expected:
            raise ValueError('Calibration file size or SHA-256 mismatch')
    return data


def number(text):
    text = text.strip()
    if len(text) > 64 or not re.fullmatch(DECIMAL, text):
        raise ValueError('Expected a decimal optical parameter')
    value = float(text)
    if not math.isfinite(value):
        raise ValueError('Non-finite optical parameter')
    return value


def lens_parameters(data):
    if not 0 < len(data) <= 65536 or b'\0' in data:
        raise ValueError('Invalid display configuration size or NUL')
    parser = configparser.ConfigParser(interpolation=None, strict=True, empty_lines_in_values=False)
    parser.optionxform = str
    try:
        parser.read_string(data.decode('ascii'))
    except (configparser.Error, UnicodeError) as error:
        raise ValueError('Malformed display configuration') from error
    if parser.defaults() or set(parser.sections()) != {'lens', 'CylinderParameters'}:
        raise ValueError('Unexpected display configuration sections')
    expected = {'lensSeparation', 'FOV', 'metersPerTanAngleAtCenter', 'distortionEqnType'}
    expected.update(f'distortionCoefficients{i}' for i in range(20))
    expected.update(f'chromaticAberration{i}' for i in range(4))
    if set(parser['lens']) != expected:
        raise ValueError('Missing or unexpected lens parameter')
    lens = {key: number(value) for key, value in parser['lens'].items()}
    if (not 0 < lens['lensSeparation'] < .2 or not 0 < lens['FOV'] < 180 or
            not 0 < lens['metersPerTanAngleAtCenter'] < 1 or lens['distortionEqnType'] != 2):
        raise ValueError('Unsupported or out-of-range lens model')
    expected = {'iCylinderMeshX', 'iCylinderMeshY', 'fCylinderInPosFactor', 'fCylinderOutPosFactor',
                'fCylinderDistortionBoundThreshold', 'fCylinderDistortionBoundSetValue'}
    expected.update(f'fCylinderDistortionC10K{i}' for i in range(1, 11))
    if set(parser['CylinderParameters']) != expected:
        raise ValueError('Missing or unexpected cylinder parameter')
    cylinder = {key: number(value) for key, value in parser['CylinderParameters'].items()}
    for key in ('iCylinderMeshX', 'iCylinderMeshY'):
        if not cylinder[key].is_integer() or not 1 <= cylinder[key] <= 256:
            raise ValueError('Invalid cylinder mesh size')
        cylinder[key] = int(cylinder[key])
    return {'lens_separation_m': lens['lensSeparation'], 'fov_degrees': lens['FOV'],
            'meters_per_tan_angle_at_center': lens['metersPerTanAngleAtCenter'],
            'equation_type': int(lens['distortionEqnType']),
            'distortion_coefficients': [lens[f'distortionCoefficients{i}'] for i in range(20)],
            'chromatic_aberration': [lens[f'chromaticAberration{i}'] for i in range(4)],
            'cylinder': cylinder}


def axis_offsets(data, model, ipd_state):
    if model not in ('neo3-pro', 'neo3-pro-eye') or ipd_state not in (1, 2, 3):
        raise ValueError('Explicit Neo3 model and IPD state required')
    if not 0 < len(data) <= 1024 or b'\0' in data:
        raise ValueError('Invalid axis-offset size or NUL')
    try:
        lines = data.decode('ascii').splitlines()
    except UnicodeError as error:
        raise ValueError('Non-ASCII axis offsets') from error
    if len(lines) != (1 if model == 'neo3-pro-eye' else 3):
        raise ValueError('Offset row count does not match the selected headset model')
    rows = []
    for line in lines:
        if len(line) >= 99 or len(line.split(',')) != 4:
            raise ValueError('Expected four offsets within the stock 100-byte line buffer')
        row = [number(value) for value in line.split(',')]
        if any(abs(value) > 1 for value in row):
            raise ValueError('Offsets require stock clamping; measurement must be checked')
        rows.append(row)
    index = 0 if model == 'neo3-pro-eye' else {3: 0, 2: 1, 1: 2}[ipd_state]
    return {'rows': rows, 'selected_row': index, 'selected': rows[index],
            'order': ['left_x', 'left_y', 'right_x', 'right_y'],
            'property_corrections_applied': False}


def inspect(config, config_sha256, offsets, offsets_sha256, model, ipd_state):
    config, offsets = Path(config), Path(offsets)
    if not re.fullmatch(r'A7H10_[01]\.ini', config.name):
        raise ValueError('Requires an explicitly selected Neo3 A7H10 display configuration')
    lens = lens_parameters(read_file(config, config_sha256, 65536))
    axes = axis_offsets(read_file(offsets, offsets_sha256, 1024), model, ipd_state)
    return {'schema_version': 1, 'model': model, 'ipd_state': ipd_state,
            'files': {'display': {'name': config.name, 'sha256': config_sha256},
                      'axis_offsets': {'name': offsets.name, 'sha256': offsets_sha256}},
            'stock_lens': lens, 'axis_offsets': axes,
            'stock_axis_coefficient_indices': [14, 15, 16, 17],
            'configuration_selection_verified': False, 'runtime_usable': False,
            'calibration_applied': False,
            'remaining': ['Verify active product and glasses-holder configuration selection',
                          'Resolve IPD, screen-gap, FOV and per-axis property overrides',
                          'Verify the stock equation and screen-to-texture coordinate mapping',
                          'Validate physical optical alignment at the selected IPD']}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('config', type=Path)
    parser.add_argument('offsets', type=Path)
    parser.add_argument('--config-sha256', required=True)
    parser.add_argument('--offsets-sha256', required=True)
    parser.add_argument('--model', choices=('neo3-pro', 'neo3-pro-eye'), required=True)
    parser.add_argument('--ipd-state', type=int, choices=(1, 2, 3), required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    try:
        result = inspect(args.config, args.config_sha256, args.offsets, args.offsets_sha256,
                         args.model, args.ipd_state)
        fd = os.open(args.output, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(fd, 'w') as stream:
            json.dump(result, stream, indent=2)
            stream.write('\n')
    except (OSError, ValueError) as error:
        parser.exit(1, f'{error}\n')


if __name__ == '__main__':
    main()
