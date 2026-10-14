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

    def delays(self, count, identifier):
        data = self.reference(identifier, 'delayInfo', count, 80)
        result = []
        for offset in range(0, len(data), 80):
            row = [integer(data, offset + i * 8) for i in range(10)]
            result.append({'type': self.optional(row[1], 'delayType', row[0]),
                           'line_count_delay': self.optional(row[3], 'linecount', row[2]),
                           'gain_delay': self.optional(row[5], 'gain', row[4]),
                           'frame_length_delay': self.optional(row[7], 'frameLengthLines', row[6]),
                           'pipeline_delay': row[8]})
        return result

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

    def optional_settings(self, present, identifier, name):
        if present not in (0, 1):
            raise ValueError('Invalid optional ' + name)
        data = self.reference(identifier, name, present, 16)
        return self.settings(integer(data, 0), integer(data, 8)) if present else None

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
                           'color_filter_pattern': row[0],
                           'camif_sample_pattern': {0: 2, 1: 3, 2: 1, 3: 0, 4: 0, 5: 6}.get(row[0]),
                           'line_length': row[3], 'frame_length': row[4], 'pixel_clock': row[7],
                           'horizontal_binning': row[8], 'vertical_binning': row[9], 'fps': fps,
                           'lanes': row[11], 'settle_count': row[14], 'cphy': row[15],
                           'settings': self.settings(row[18], row[19]),
                           'capabilities': self.array(row[31], 'capability', row[30]),
                           'mipi_flags': flags})
        return result

    def resolution_information(self, symbol):
        if symbol.version != 0 or symbol.mode != 0xffffffff or len(symbol.data) % 32:
            raise ValueError('Invalid resolution-information table')
        result = []
        for offset in range(0, len(symbol.data), 32):
            flag, version_id, count, mode_id = (integer(symbol.data, offset + i * 8) for i in range(4))
            version = self.array(version_id, 'sensorVersion', int(flag != 0))
            self.reference(mode_id, 'resolutionData', count, 288)
            result.append({'index': offset // 32, 'sensor_version_flag': flag,
                           'sensor_version': version[0] if version else None,
                           'modes': {'symbol': mode_id, 'count': count}})
        return result

    def inspect(self):
        drivers = [s for s in self.symbols.values() if s.name == 'sensorDriverData']
        if len(drivers) != 1 or drivers[0].version != 1 or len(drivers[0].data) != 808:
            raise ValueError('Requires one supported sensor driver schema')
        driver = drivers[0].data
        name = self.sensor_name(integer(driver, 24), integer(driver, 32))
        registers = {str(s.identifier): self.registers(s) for s in self.symbols.values() if s.name == 'regSetting'}
        resolutions = {str(s.identifier): self.resolutions(s) for s in self.symbols.values() if s.name == 'resolutionData'}
        resolution_information = {str(s.identifier): self.resolution_information(s)
                                  for s in self.symbols.values() if s.name == 'resolutionInfo'}
        resolution_count, resolution_symbol = integer(driver, 352), integer(driver, 360)
        self.reference(resolution_symbol, 'resolutionInfo', resolution_count, 32)
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
        stream_settings = {
            'stream_on': self.settings(integer(driver, 488), integer(driver, 496)),
            'stream_off': self.settings(integer(driver, 504), integer(driver, 512)),
            'master': self.optional_settings(integer(driver, 552), integer(driver, 560), 'masterSettings'),
            'slave': self.optional_settings(integer(driver, 568), integer(driver, 576), 'slaveSettings'),
        }
        init_count, init_symbol = integer(driver, 584), integer(driver, 592)
        self.reference(init_symbol, 'initSettings', init_count, 24)
        delays = self.delays(integer(driver, 720), integer(driver, 728))
        selected = next((row for row in reversed(delays) if row['type'] == 0),
                        delays[0] if delays else None)
        return {'sensor_name': name,
                'delays': delays,
                'probe_pipeline_delay': selected['pipeline_delay'] & 0xffff if selected else None,
                'address_type': integer(driver, 48), 'data_type': integer(driver, 56),
                'frequency': integer(driver, 88),
                'power_up': self.power(integer(driver, 96), integer(driver, 104)),
                'power_down': self.power(integer(driver, 112), integer(driver, 120)),
                'configurations': self.configurations(),
                'slave_address': integer(driver, 40), 'sensor_id_register': integer(driver, 64),
                'sensor_id': integer(driver, 72), 'sensor_id_mask': integer(driver, 80),
                'symbols': len(self.symbols), 'resolutions': resolutions,
                'resolution_information': resolution_information,
                'resolution_information_reference': {'symbol': resolution_symbol, 'count': resolution_count},
                'stream_settings': stream_settings,
                'initialization_reference': {'symbol': init_symbol, 'count': init_count},
                'initialization': initialization, 'register_tables': registers}



def select_native(data, configuration, group, mode, initialization, synchronization):
    """Resolve explicit module choices into the supported mono8 command subset."""
    def pick(rows, index, name):
        if type(index) is not int or not 0 <= index < len(rows):
            raise ValueError('Invalid explicit ' + name + ' index')
        return rows[index]

    selected = pick(data['configurations'], configuration, 'configuration')
    if selected['sensor_name'] != data['sensor_name']:
        raise ValueError('Configuration names a different sensor')
    groups = data['resolution_information'][str(data['resolution_information_reference']['symbol'])]
    information = pick(groups, group, 'resolution group')
    resolution = pick(data['resolutions'][str(information['modes']['symbol'])], mode, 'resolution')
    initializations = data['initialization'][str(data['initialization_reference']['symbol'])]
    init = pick(initializations, initialization, 'initialization')
    if synchronization not in ('none', 'master', 'slave'):
        raise ValueError('Explicit synchronization table selection required')
    if (resolution['color_filter_pattern'] != 4 or resolution['camif_sample_pattern'] is None or
            len(resolution['streams']) != 1):
        raise ValueError('Native tracking requires one monochrome stream')
    stream = resolution['streams'][0]
    if (stream['type'] != 1 or stream['bits'] != 8 or stream['data_type'] != 0x2a or
            stream['x'] or stream['y'] or stream['virtual_channel'] > 3 or
            not 1 <= stream['width'] <= 4096 or not 1 <= stream['height'] <= 4096):
        raise ValueError('Native tracking requires uncropped RAW8 geometry')
    lane_map = selected['lane_assign']
    lanes, cphy = resolution['lanes'], resolution['cphy']
    if (selected['combo'] != 0 or lane_map is None or cphy not in (0, 1) or
            not 1 <= lanes <= (3 if cphy else 4) or resolution['settle_count'] > 65535):
        raise ValueError('Unsupported native PHY configuration')
    active = [(lane_map >> (i * 4)) & 15 for i in range(lanes)]
    if len(set(active)) != lanes or any(lane >= (3 if cphy else 4) for lane in active):
        raise ValueError('Invalid active PHY lanes')
    slave = selected['slave_address'] if selected['slave_address'] is not None else data['slave_address']
    frequency = selected['frequency'] if selected['frequency'] is not None else data['frequency']
    if (not 0 < slave <= 254 or slave % 2 or frequency > 3 or
            not 1 <= data['address_type'] <= 4 or not 1 <= data['data_type'] <= 4 or
            data['sensor_id_register'] >= 1 << (8 * data['address_type']) or
            data['probe_pipeline_delay'] not in range(4)):
        raise ValueError('Unsupported native probe parameters')
    for direction in ('power_up', 'power_down'):
        steps = data[direction]
        if not 1 <= len(steps) <= 12:
            raise ValueError('Unsupported native power sequence length')
        consecutive = 0
        for step in steps:
            consecutive += 1
            if step['sequence_type'] > 11 or step['delay_ms'] > 32767 or consecutive >= 12:
                raise ValueError('Unsupported native power sequence')
            if step['delay_ms']:
                consecutive = 0

    def table(reference):
        if reference is None:
            raise ValueError('Requested synchronization table is absent')
        rows = data['register_tables'][str(reference['symbol'])]
        if not 1 <= len(rows) <= 4096:
            raise ValueError('Unsupported native register table length')
        address_type, data_type = rows[0]['address_type'], rows[0]['data_type']
        if not 1 <= address_type <= 4 or not 1 <= data_type <= 4:
            raise ValueError('Unsupported native register width')
        include_slave = rows[0]['slave_address'] is not None
        writes = []
        for row in rows:
            delay = row['delay_us'] if row['delay_us'] is not None else 0
            if (row['operation'] != 0 or len(row['values']) != 1 or
                    row['address_type'] != address_type or row['data_type'] != data_type or
                    row['address'] >= 1 << (8 * address_type) or row['values'][0] >= 1 << (8 * data_type) or
                    (row['slave_address'] is not None and (not include_slave or row['slave_address'] != slave)) or
                    delay > 32767000 or (delay >= 2000 and delay % 1000)):
                raise ValueError('Register table is outside the native random-write subset')
            writes.append({'address': row['address'], 'value': row['values'][0], 'delay_us': delay})
        return {'bus': {'address_type': address_type, 'data_type': data_type, 'frequency': frequency,
                        'include_slave': include_slave, 'slave_address': slave}, 'writes': writes}

    tables = {'initialize': table(init['settings']), 'mode': table(resolution['settings']),
              'stream_on': table(data['stream_settings']['stream_on']),
              'stream_off': table(data['stream_settings']['stream_off']),
              'synchronize': None if synchronization == 'none' else table(data['stream_settings'][synchronization])}
    return {'schema_version': 1, 'sensor_name': data['sensor_name'],
            'selection': {'configuration': configuration, 'resolution_group': group, 'resolution': mode,
                          'initialization': initialization, 'synchronization': synchronization},
            'sensor_version': information['sensor_version'], 'initialization_selector': init['selector'],
            'probe': {'bus': {'address_type': data['address_type'], 'data_type': data['data_type'],
                              'frequency': frequency, 'include_slave': True, 'slave_address': slave},
                      'register_address': data['sensor_id_register'], 'expected_id': data['sensor_id'],
                      'id_mask': data['sensor_id_mask'], 'pipeline_delay': data['probe_pipeline_delay'],
                      'power_up': data['power_up'], 'power_down': data['power_down']},
            'stream': stream, 'phy': {'lanes': lanes, 'lane_map': lane_map, 'cphy': cphy, 'combo': 0,
                                     'settle_count': resolution['settle_count'],
                                     'mipi_flags': sum(1 << flag for flag in set(resolution['mipi_flags']))},
            'camif_sample_pattern': resolution['camif_sample_pattern'],
            'timing': {key: resolution[key] for key in ('fps', 'pixel_clock', 'line_length', 'frame_length')},
            'tables': tables}

def run(path, expected, selection=None):
    if not re.fullmatch('[0-9a-f]{64}', expected) or path.is_symlink() or not path.is_file() or path.stat().st_size > MAX_BYTES:
        raise ValueError('Requires a bounded regular module and its expected SHA-256')
    with path.open('rb') as stream:
        data = stream.read(MAX_BYTES + 1)
    digest = hashlib.sha256(data).hexdigest()
    if digest != expected:
        raise ValueError('Camera module checksum differs')
    report = Module(data).inspect()
    if selection is not None:
        report = select_native(report, **selection)
    return dict(report, source_sha256=digest,
                scope='offline native mono8 configuration' if selection is not None else 'offline sensor module inventory; partial schema',
                active_sensor_verified=False, calibration_verified=False, device_actions=[])


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('module', type=Path)
    parser.add_argument('--sha256', required=True)
    parser.add_argument('--configuration', type=int)
    parser.add_argument('--resolution-group', type=int)
    parser.add_argument('--resolution', type=int)
    parser.add_argument('--initialization', type=int)
    parser.add_argument('--synchronization', choices=('none', 'master', 'slave'))
    args = parser.parse_args()
    choices = {'configuration': args.configuration, 'group': args.resolution_group,
               'mode': args.resolution, 'initialization': args.initialization,
               'synchronization': args.synchronization}
    if any(value is not None for value in choices.values()) and any(value is None for value in choices.values()):
        parser.error('Native selection requires all four explicit indices and a synchronization choice')
    try:
        print(json.dumps(run(args.module, args.sha256, choices if args.configuration is not None else None),
                         indent=2, allow_nan=False))
    except (OSError, ValueError, struct.error) as error:
        parser.exit(1, f'{error}\n')
