#!/usr/bin/env python3
"""Inspect Quest peripheral camera configuration without applying it to hardware."""
import argparse
import hashlib
import json
import math
from pathlib import Path
import struct

MAX_BYTES = 4 * 1024**2
SCHEMAS = {
    'root': {1: ('device', 'device'), 20: ('cameras', 'camera', True),
             30: ('sets', 'set', True), 31: ('stream_configs', 'stream_config', True)},
    'device': {1: ('model', 'uint'), 2: ('soc', 'text')},
    'camera': {1: ('ers_id', 'uint'), 2: ('name', 'text'), 3: ('template', 'text'),
               4: ('is_template', 'bool'), 6: ('type', 'uint'), 10: ('model', 'uint'),
               12: ('resolution', 'uint'), 13: ('rolling_shutter', 'bool'),
               14: ('has_embedded_metadata', 'bool'), 15: ('dual_gain', 'bool'),
               20: ('calibration_file', 'text'), 40: ('camx', 'camx'),
               51: ('syncboss_set', 'uint'), 60: ('gamma_unit', 'uint'),
               61: ('gamma_curve', 'uint'), 62: ('embedded_metadata_mode', 'uint')},
    'camx': {1: ('metadata', 'metadata'), 2: ('port', 'port'), 3: ('sensor_name', 'text'),
             5: ('physical_sensor_name', 'text'), 6: ('driver_name', 'text'),
             90: ('modes', 'mode', True)},
    'metadata': {1: ('camera_id', 'uint'), 2: ('slot_id', 'uint')},
    'port': {1: ('phy', 'uint'), 10: ('combo', 'bool'), 15: ('lane_assign_hex', 'text'),
             16: ('lane_assign_num', 'uint')},
    'mode': {9: ('line_length', 'uint'), 12: ('frame_length', 'uint'),
             21: ('pixel_clock', 'uint'), 24: ('horizontal_binning', 'uint'),
             27: ('vertical_binning', 'uint'), 30: ('fps', 'double'), 36: ('lanes', 'uint'),
             45: ('settle_count', 'uint'), 48: ('cphy', 'uint'),
             63: ('streams', 'stream', True)},
    'stream': {1: ('type', 'uint'), 5: ('channel', 'uint'), 6: ('tracker', 'uint'),
               11: ('data_type', 'uint'), 12: ('bits', 'uint'),
               20: ('x', 'uint'), 21: ('y', 'uint'), 22: ('width', 'uint'), 23: ('height', 'uint'),
               31: ('isp_scale_x', 'float'), 32: ('isp_scale_y', 'float')},
    'set': {1: ('name', 'text'), 4: ('type', 'uint'), 5: ('tracking', 'bool'),
            20: ('cameras', 'text', True), 30: ('sync_period_us', 'uint'),
            31: ('gain_factor', 'double'), 40: ('gamma_factor', 'double'),
            41: ('gamma_curve', 'uint'), 42: ('system_type', 'uint')},
    'stream_config': {1: ('name', 'text'), 2: ('frame_type', 'uint'), 4: ('cameras', 'text', True),
                      5: ('camera_type', 'uint'), 6: ('default', 'bool'),
                      7: ('relax_timestamp_validation', 'bool'), 20: ('syncboss', 'syncboss'),
                      30: ('gamma_factor', 'double'), 31: ('gamma_curve', 'uint')},
    'syncboss': {1: ('capability', 'uint'), 2: ('exposure_us', 'uint'), 3: ('gain', 'uint')},
}


def varint(data, offset):
    value = 0
    for shift in range(0, 70, 7):
        if offset == len(data):
            raise ValueError('Truncated varint')
        byte = data[offset]; offset += 1
        if shift == 63 and byte > 1:
            raise ValueError('Overflowing varint')
        value |= (byte & 127) << shift
        if not byte & 128:
            if shift and not byte:
                raise ValueError('Noncanonical varint')
            return value, offset
    raise ValueError('Oversized varint')


def decode(data, schema='root'):
    if len(data) > MAX_BYTES:
        raise ValueError('Configuration too large')
    fields, offset, count = {}, 0, 0
    while offset < len(data):
        count += 1
        if count > 4096:
            raise ValueError('Too many fields')
        tag, offset = varint(data, offset)
        number, wire = tag >> 3, tag & 7
        if not 1 <= number < 1 << 29 or wire not in (0, 1, 2, 5):
            raise ValueError('Invalid field tag')
        if wire == 0:
            value, offset = varint(data, offset)
        else:
            size = 8 if wire == 1 else 4
            if wire == 2:
                size, offset = varint(data, offset)
            if size > len(data) - offset:
                raise ValueError('Truncated field')
            value, offset = data[offset:offset + size], offset + size
        definition = SCHEMAS[schema].get(number)
        if not definition:
            continue
        name, kind, *repeated = definition
        expected = {'uint': 0, 'bool': 0, 'float': 5, 'double': 1}.get(kind, 2)
        if wire != expected or (not repeated and name in fields):
            raise ValueError('Wrong wire type or duplicate ' + name)
        if kind in ('uint', 'bool'):
            if value > (1 if kind == 'bool' else 0xffffffff):
                raise ValueError('Out-of-range ' + name)
            if kind == 'bool':
                value = bool(value)
        elif kind in ('float', 'double'):
            value = struct.unpack('<f' if kind == 'float' else '<d', value)[0]
            if not math.isfinite(value):
                raise ValueError('Nonfinite ' + name)
        elif kind == 'text':
            if len(value) > 256 or any(c < 32 or c > 126 for c in value):
                raise ValueError('Invalid ' + name)
            value = value.decode('ascii')
        else:
            value = decode(value, kind)
        if repeated:
            fields.setdefault(name, []).append(value)
        else:
            fields[name] = value
    return fields


def inspect(data):
    result = decode(data)
    if result.get('device', {}).get('model') != 6:
        raise ValueError('Requires Eureka peripheral configuration')
    cameras = result.get('cameras', [])
    names = [c.get('name') for c in cameras]
    ids = [c.get('ers_id', 0) for c in cameras if not c.get('is_template', False)]
    if (not cameras or any(not name for name in names) or len(set(names)) != len(names) or
            len(set(ids)) != len(ids)):
        raise ValueError('Missing or duplicate camera identity')
    for camera in cameras:
        if camera.get('template') and camera['template'] not in names:
            raise ValueError('Missing camera template')
    for item in result.get('sets', []) + result.get('stream_configs', []):
        if any(name not in names for name in item.get('cameras', [])):
            raise ValueError('Unknown camera reference')
    return result


def run(path, expected):
    if path.is_symlink() or not path.is_file() or not 1 <= path.stat().st_size <= MAX_BYTES:
        raise ValueError('Requires bounded regular configuration file')
    data = path.read_bytes()
    actual = hashlib.sha256(data).hexdigest()
    if actual != expected:
        raise ValueError('Configuration checksum mismatch')
    return {'sha256': actual, 'configuration': inspect(data), 'device_actions': [],
            'templates_resolved': False, 'active_modes_verified': False, 'capture_configuration_ready': False}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('configuration', type=Path)
    parser.add_argument('--sha256', required=True)
    args = parser.parse_args()
    try:
        print(json.dumps(run(args.configuration, args.sha256), indent=2))
    except (OSError, ValueError) as error:
        parser.exit(1, str(error) + '\n')
