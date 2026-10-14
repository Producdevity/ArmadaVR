#!/usr/bin/env python3
"""Inspect a retained Pico b1977 camera module without selecting or programming a sensor."""
import argparse
from dataclasses import dataclass
import hashlib
import json
import math
from pathlib import Path
import re
import struct

MAX_BYTES = 4 * 1024**2
MAX_SYMBOLS = 32768


@dataclass(frozen=True)
class Symbol:
    identifier: int
    name: str
    version: int
    mode: int
    data: bytes


def integer(data, offset, bits=32):
    if offset < 0 or offset + 8 > len(data):
        raise ValueError('Truncated padded integer')
    value = struct.unpack_from('<Q', data, offset)[0]
    if value >= 1 << bits:
        raise ValueError('Integer or padding exceeds field width')
    return value


class Module:
    def __init__(self, data):
        if not 232 <= len(data) <= MAX_BYTES or data[:28] != b'QTI Chromatix Header'.ljust(28, b'\0'):
            raise ValueError('Unsupported camera module header')
        if (struct.unpack_from('<I', data, 28)[0] != len(data) or
                data[32:40] != struct.pack('<HHI', 2, 0, 0) or
                struct.unpack_from('<II', data, 152) != (160, 3)):
            raise ValueError('Requires the complete version 2 camera module')
        sections = {}
        for i in range(3):
            offset = 160 + i * 24
            kind, start, size = (integer(data, offset + j * 8) for j in range(3))
            if kind not in (0, 1, 2) or kind in sections or start < 232 or start % 8 or size % 8 or start + size > len(data):
                raise ValueError('Invalid camera module section')
            sections[kind] = (start, size)
        end = 232
        for start, size in sorted(sections.values()):
            if start != end:
                raise ValueError('Overlapping or incomplete module sections')
            end = start + size
        if end != len(data):
            raise ValueError('Trailing module data')
        table, table_size = sections[0]
        payload, payload_size = sections[1]
        if table_size % 72 or not 1 <= table_size // 72 <= MAX_SYMBOLS:
            raise ValueError('Invalid module symbol count')
        self.symbols = {}
        for offset in range(table, table + table_size, 72):
            identifier = integer(data, offset)
            name_bytes = data[offset + 8:offset + 40]
            name = name_bytes.split(b'\0', 1)[0]
            if (not identifier or identifier in self.symbols or not re.fullmatch(rb'(?:[A-Za-z_][A-Za-z0-9_]{0,31}|\[[0-9]{1,10}\])', name) or
                    name_bytes != name.ljust(32, b'\0')):
                raise ValueError('Invalid or repeated module symbol')
            version = integer(data, offset + 40, 64)
            mode, start, size = (integer(data, offset + j) for j in (48, 56, 64))
            if start % 8 or size % 8 or start + size > payload_size:
                raise ValueError('Symbol outside module payload')
            self.symbols[identifier] = Symbol(identifier, name.decode('ascii'), version, mode,
                                              data[payload + start:payload + start + size])

    def reference(self, identifier, name, count, stride):
        symbol = self.symbols.get(identifier)
        if (symbol is None or symbol.name != name or symbol.version != 0 or symbol.mode != 0xffffffff or
                count > MAX_BYTES // stride or len(symbol.data) != count * stride):
            raise ValueError('Invalid ' + name + ' reference')
        return symbol.data

    def array(self, identifier, name, count, bits=32):
        data = self.reference(identifier, name, count, 8)
        return [integer(data, i * 8, bits) for i in range(count)]

    def optional(self, identifier, name, present, bits=32):
        if present not in (0, 1):
            raise ValueError('Invalid optional ' + name)
        values = self.array(identifier, name, present, bits)
        return values[0] if present else None

    def sensor_name(self, length, identifier):
        if not 1 <= length <= 256:
            raise ValueError('Invalid sensor name length')
        name = self.reference(identifier, 'sensorName', (length + 7) // 8, 8)
        if name[length - 1] != 0 or any(name[length:]) or not re.fullmatch(rb'[A-Za-z0-9_.-]+\0', name[:length]):
            raise ValueError('Invalid sensor name')
        return name[:length - 1].decode('ascii')

    def power(self, count, identifier):
        data = self.reference(identifier, 'powerSetting', count, 24)
        return [dict(zip(('sequence_type', 'value', 'delay_ms'),
                         [integer(data, offset + i * 8) for i in range(3)]))
                for offset in range(0, len(data), 24)]

    def configurations(self):
        roots = [s for s in self.symbols.values() if s.name == 'cameraModuleData']
        if len(roots) != 1 or roots[0].version != 1 or len(roots[0].data) != 40:
            raise ValueError('Requires one supported camera module schema')
        root = roots[0].data
        count, identifier = integer(root, 24), integer(root, 32)
        data = self.reference(identifier, 'moduleConfiguration', count, 360)
        result = []
        for offset in range(0, len(data), 360):
            row = [integer(data, offset + i * 8) for i in range(31)]
            result.append({'index': offset // 360, 'sensor_name': self.sensor_name(row[3], row[4]),
                           'slave_address': self.optional(row[6], 'sensorSlaveAddress', row[5], 16),
                           'frequency': self.optional(row[8], 'sensorI2CFrequencyMode', row[7]),
                           'lane_assign': self.optional(row[28], 'laneAssign', row[27], 16),
                           'combo': self.optional(row[30], 'isComboMode', row[29], 8)})
        return result

    def registers(self, symbol):
        if symbol.version != 0 or symbol.mode != 0xffffffff or len(symbol.data) % 80:
            raise ValueError('Invalid register-setting table')
        result = []
        for offset in range(0, len(symbol.data), 80):
            row = [integer(symbol.data, offset + i * 8) for i in range(10)]
            result.append({'slave_address': self.optional(row[1], 'slaveAddr', row[0], 16),
                           'address': row[2], 'values': self.array(row[4], 'registerData', row[3]),
                           'address_type': row[5], 'data_type': row[6], 'operation': row[7],
                           'delay_us': self.optional(row[9], 'delayUs', row[8])})
        return result

    def settings(self, count, identifier):
        data = self.reference(identifier, 'regSetting', count, 80)
        return {'symbol': identifier, 'count': len(data) // 80}

    def resolutions(self, symbol):
        if symbol.version != 0 or symbol.mode != 0xffffffff or len(symbol.data) % 288:
            raise ValueError('Invalid resolution table')
        result = []
        for offset in range(0, len(symbol.data), 288):
            row = [integer(symbol.data, offset + i * 8) if i != 10 else 0 for i in range(36)]
            fps = struct.unpack_from('<d', symbol.data, offset + 80)[0]
            if not math.isfinite(fps) or fps <= 0:
                raise ValueError('Invalid sensor frame rate')
            streams = self.reference(row[2], 'streamConfiguration', row[1], 64)
            stream_keys = ('virtual_channel', 'data_type', 'x', 'y', 'width', 'height', 'bits', 'type')
            decoded = [dict(zip(stream_keys, [integer(streams, j + i * 8) for i in range(8)]))
                       for j in range(0, len(streams), 64)]
            flags = self.array(row[35], 'mipiFlags', row[34])
            if any(flag > 31 for flag in flags):
                raise ValueError('Invalid MIPI flag bit')
            result.append({'index': offset // 288, 'streams': decoded,
                           'line_length': row[3], 'frame_length': row[4], 'pixel_clock': row[7],
                           'horizontal_binning': row[8], 'vertical_binning': row[9], 'fps': fps,
                           'lanes': row[11], 'settle_count': row[14], 'cphy': row[15],
                           'settings': self.settings(row[18], row[19]),
                           'capabilities': self.array(row[31], 'capability', row[30]),
                           'mipi_flags': flags})
        return result

    def inspect(self):
        drivers = [s for s in self.symbols.values() if s.name == 'sensorDriverData']
        if len(drivers) != 1 or drivers[0].version != 1 or len(drivers[0].data) != 808:
            raise ValueError('Requires one supported sensor driver schema')
        driver = drivers[0].data
        name = self.sensor_name(integer(driver, 24), integer(driver, 32))
        registers = {str(s.identifier): self.registers(s) for s in self.symbols.values() if s.name == 'regSetting'}
        resolutions = {str(s.identifier): self.resolutions(s) for s in self.symbols.values() if s.name == 'resolutionData'}
        initialization = {}
        for symbol in self.symbols.values():
            if symbol.name != 'initSettings':
                continue
            if symbol.version != 0 or symbol.mode != 0xffffffff or len(symbol.data) % 24:
                raise ValueError('Invalid initialization table')
            initialization[str(symbol.identifier)] = [
                {'selector': integer(symbol.data, i),
                 'settings': self.settings(integer(symbol.data, i + 8), integer(symbol.data, i + 16))}
                for i in range(0, len(symbol.data), 24)]
        return {'sensor_name': name,
                'address_type': integer(driver, 48), 'data_type': integer(driver, 56),
                'frequency': integer(driver, 88),
                'power_up': self.power(integer(driver, 96), integer(driver, 104)),
                'power_down': self.power(integer(driver, 112), integer(driver, 120)),
                'configurations': self.configurations(),
                'slave_address': integer(driver, 40), 'sensor_id_register': integer(driver, 64),
                'sensor_id': integer(driver, 72), 'sensor_id_mask': integer(driver, 80),
                'symbols': len(self.symbols), 'resolutions': resolutions,
                'initialization': initialization, 'register_tables': registers}


def run(path, expected):
    if not re.fullmatch('[0-9a-f]{64}', expected) or path.is_symlink() or not path.is_file() or path.stat().st_size > MAX_BYTES:
        raise ValueError('Requires a bounded regular module and its expected SHA-256')
    with path.open('rb') as stream:
        data = stream.read(MAX_BYTES + 1)
    digest = hashlib.sha256(data).hexdigest()
    if digest != expected:
        raise ValueError('Camera module checksum differs')
    return dict(Module(data).inspect(), source_sha256=digest,
                scope='offline sensor module inventory; partial schema',
                active_sensor_verified=False, calibration_verified=False, device_actions=[])


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('module', type=Path)
    parser.add_argument('--sha256', required=True)
    args = parser.parse_args()
    try:
        print(json.dumps(run(args.module, args.sha256), indent=2, allow_nan=False))
    except (OSError, ValueError, struct.error) as error:
        parser.exit(1, f'{error}\n')
