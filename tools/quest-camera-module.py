#!/usr/bin/env python3
"""Inspect retained Quest camera module settings without selecting or programming hardware."""
import argparse
from dataclasses import dataclass
import hashlib
import json
import math
from pathlib import Path
import re
import struct

MAX_BYTES = 4 * 1024**2


def read(data, offset, kind='I'):
    size = struct.calcsize('<' + kind)
    if offset < 0 or offset + size > len(data):
        raise ValueError('Truncated camera field')
    return struct.unpack_from('<' + kind, data, offset)[0]


@dataclass(frozen=True)
class Symbol:
    name: str
    version: int
    mode: int
    data: bytes


class Module:
    def __init__(self, data):
        if not 204 <= len(data) <= MAX_BYTES or data[:28] != b'QTI Chromatix Header'.ljust(28, b'\0'):
            raise ValueError('Unsupported camera module header')
        if (read(data, 28) != len(data) or data[32:40] != struct.pack('<HHI', 3, 0, 1) or
                read(data, 152, 'Q') != 1 or struct.unpack_from('<II', data, 160) != (168, 3)):
            raise ValueError('Requires complete uncompressed version 3.0.1 module')
        sections = {}
        for index in range(3):
            kind, start, size = struct.unpack_from('<III', data, 168 + index * 12)
            if kind not in (0, 1, 2) or kind in sections or start < 204 or start + size > len(data):
                raise ValueError('Invalid camera module section')
            sections[kind] = (start, size)
        end = 204
        for start, size in sorted(sections.values()):
            if start != end:
                raise ValueError('Overlapping or incomplete camera sections')
            end = start + size
        if end != len(data):
            raise ValueError('Trailing camera module bytes')
        table, size = sections[0]
        payload, payload_size = sections[1]
        if size % 56 or not 1 <= size // 56 <= 32768:
            raise ValueError('Invalid camera symbol count')
        self.symbols = {}
        ranges = []
        for offset in range(table, table + size, 56):
            identifier, raw_name, version, mode, start, length = struct.unpack_from('<I32sQIII', data, offset)
            name = raw_name.split(b'\0', 1)[0]
            if (not 1 <= identifier <= size // 56 or identifier in self.symbols or
                    not re.fullmatch(rb'(?:[A-Za-z_][A-Za-z0-9_]{0,31}|\[[0-9]{1,10}\])?', name) or
                    raw_name != name.ljust(32, b'\0') or version != 1 or start + length > payload_size):
                raise ValueError('Invalid camera symbol')
            self.symbols[identifier] = Symbol(name.decode('ascii'), version, mode,
                                               data[payload + start:payload + start + length])
            if length:
                ranges.append((start, length))
        end = 0
        for start, length in sorted(ranges):
            if start != end:
                raise ValueError('Overlapping or incomplete symbol payloads')
            end = start + length
        if end != payload_size:
            raise ValueError('Unreferenced symbol payload')

    def reference(self, identifier, name, count, stride):
        symbol = self.symbols.get(identifier)
        if (symbol is None or symbol.mode != 0xffffffff or
                symbol.name not in ((name, '') if count == 0 else (name,)) or
                count > MAX_BYTES // stride or len(symbol.data) != count * stride):
            raise ValueError('Invalid ' + name + ' reference')
        return symbol.data

    def root(self, name, size):
        symbols = [s for s in self.symbols.values() if s.name == name]
        if (len(symbols) != 1 or symbols[0].mode != 0 or len(symbols[0].data) != size or
                symbols[0].data[:12] != struct.pack('<III', 1, 0, 0)):
            raise ValueError('Unsupported ' + name + ' schema')
        return symbols[0].data

    def array(self, row, offset, name, kind='I'):
        count, identifier = read(row, offset), read(row, offset + 4)
        stride = struct.calcsize('<' + kind)
        data = self.reference(identifier, name, count, stride)
        return [read(data, i, kind) for i in range(0, len(data), stride)]

    def optional(self, row, offset, name, kind='I'):
        if read(row, offset) not in (0, 1):
            raise ValueError('Invalid optional ' + name)
        values = self.array(row, offset, name, kind)
        return values[0] if values else None

    def name(self, row, offset):
        count, identifier = read(row, offset), read(row, offset + 4)
        if not 1 <= count <= 256:
            raise ValueError('Invalid sensor name size')
        data = self.reference(identifier, 'sensorName', count, 1)
        if not re.fullmatch(rb'[A-Za-z0-9_.-]+', data):
            raise ValueError('Invalid sensor name')
        return data.decode('ascii')

    def rows(self, row, offset, name, stride):
        data = self.reference(read(row, offset + 4), name, read(row, offset), stride)
        return [data[i:i + stride] for i in range(0, len(data), stride)]

    def configurations(self):
        root = self.root('cameraModuleData', 20)
        return [{'camera_id': read(row, 0, 'H'), 'sensor_name': self.name(row, 18),
                 'slave_address': self.optional(row, 26, 'sensorSlaveAddress', 'H'),
                 'frequency': self.optional(row, 98, 'sensorI2CFrequencyMode'),
                 'lane_assign': self.optional(row, 110, 'laneAssign', 'H'),
                 'combo': self.optional(row, 118, 'isComboMode', 'B')}
                for row in self.rows(root, 12, 'moduleConfiguration', 262)]

    def settings(self, row, offset):
        result = []
        for item in self.rows(row, offset, 'regSetting', 40):
            result.append({'slave_address': self.optional(item, 0, 'slaveAddr', 'H'),
                           'address': read(item, 8), 'values': self.array(item, 12, 'registerData'),
                           'address_type': read(item, 20), 'data_type': read(item, 24),
                           'operation': read(item, 28), 'delay_us': self.optional(item, 32, 'delayUs')})
        return result

    def stream(self, row):
        result = {'virtual_channels': self.array(row, 0, 'vc'), 'virtual_channel_map': self.array(row, 8, 'vcMap')}
        result.update({key: read(row, offset) for key, offset in
                       [('data_type', 16), ('x', 28), ('y', 32), ('width', 36), ('height', 40), ('bits', 52), ('type', 64)]})
        return result

    def resolution(self, row):
        fps = read(row, 64, 'd')
        if not math.isfinite(fps) or fps <= 0:
            raise ValueError('Invalid nominal frame rate')
        result = {key: read(row, offset) for key, offset in
                  [('color_filter_pattern', 16), ('line_length', 36), ('frame_length', 40), ('pixel_clock', 52),
                   ('horizontal_binning', 56), ('vertical_binning', 60), ('lanes', 80), ('settle_count', 100), ('cphy', 104)]}
        result.update(fps=fps, streams=[self.stream(s) for s in self.rows(row, 20, 'streamConfiguration', 116)],
                      settings=self.settings(row, 124))
        return result

    def inspect(self):
        driver = self.root('sensorDriverData', 600)
        groups = []
        for row in self.rows(driver, 240, 'resolutionInfo', 16):
            groups.append({'sensor_version': self.optional(row, 0, 'sensorVersion'),
                           'modes': [self.resolution(mode) for mode in self.rows(row, 8, 'resolutionData', 260)]})
        power = {}
        for name, offset in [('power_up', 80), ('power_down', 88)]:
            power[name] = [dict(zip(('sequence_type', 'value', 'delay_ms'), struct.unpack('<III', row)))
                           for row in self.rows(driver, offset, 'powerSetting', 12)]
        return dict(sensor_name=self.name(driver, 12), configurations=self.configurations(),
                    slave_address=read(driver, 52), address_type=read(driver, 56), data_type=read(driver, 60),
                    sensor_id_register=read(driver, 64), sensor_id=read(driver, 68), sensor_id_mask=read(driver, 72),
                    frequency=read(driver, 76), resolution_groups=groups, **power,
                    stream_on=self.settings(driver, 368), stream_off=self.settings(driver, 376),
                    initialization=[{'selector': read(row, 0), 'settings': self.settings(row, 4)}
                                    for row in self.rows(driver, 448, 'initSettings', 12)])


def run(path, expected):
    if (not re.fullmatch('[0-9a-f]{64}', expected) or path.is_symlink() or not path.is_file() or
            path.stat().st_size > MAX_BYTES):
        raise ValueError('Requires a bounded regular module and its expected SHA-256')
    with path.open('rb') as stream:
        data = stream.read(MAX_BYTES + 1)
    digest = hashlib.sha256(data).hexdigest()
    if digest != expected:
        raise ValueError('Camera module checksum differs')
    return dict(Module(data).inspect(), source_sha256=digest, scope='offline partial sensor module inventory',
                active_sensor_verified=False, calibration_verified=False, capture_configuration_ready=False, device_actions=[])


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('module', type=Path)
    parser.add_argument('--sha256', required=True)
    args = parser.parse_args()
    try:
        print(json.dumps(run(args.module, args.sha256), indent=2, allow_nan=False))
    except (OSError, ValueError, struct.error) as error:
        parser.exit(1, f'{error}\n')
