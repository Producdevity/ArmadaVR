#!/usr/bin/env python3
"""Read Pico media-controller metadata without opening camera or sync controls."""
import argparse
import ctypes
import errno
import fcntl
import json
import os
from pathlib import Path
import platform
import re
import stat
import struct
import sys

DEVICE_INFO = 0xc1007c00
GET_TOPOLOGY = 0xc0487c04
TOPOLOGY = struct.Struct('<QIIQIIQIIQIIQ')
RECORDS = (
    ('entities', struct.Struct('<I64sII20x'), 256),
    ('interfaces', struct.Struct('<III36xII56x'), 256),
    ('pads', struct.Struct('<IIII16x'), 1024),
    ('links', struct.Struct('<IIII24x'), 2048),
)
MODELS = {'cam_req_mgr': 'cam-req-mgr-devnode', 'cam_sync': 'cam_sync_device'}
ROLES = ('request-manager', 'sensor', 'ife', 'icp', 'lrme', 'jpeg', 'face-detection',
         'cpas', 'csiphy', 'actuator', 'cci', 'flash', 'eeprom', 'ois', 'custom')


def string(data):
    end = data.find(b'\0')
    if end < 0:
        raise ValueError('Unterminated media name')
    return data[:end].decode('utf-8')


def device_info(fd):
    data = bytearray(256)
    fcntl.ioctl(fd, DEVICE_INFO, data, True)
    driver, model, serial, bus, version, hardware, driver_version = struct.unpack('<16s32s40s32sIII124x', data)
    info = dict(zip(('driver', 'model', 'serial', 'bus_info'), map(string, (driver, model, serial, bus))))
    if info['driver'] not in MODELS or info['model'] != MODELS[info['driver']]:
        raise ValueError('Expected the Pico camera request-manager or sync media controller')
    if version < 0x041300:
        raise ValueError('Media topology requires Linux 4.19 or newer')
    return dict(info, media_version=version, hardware_revision=hardware, driver_version=driver_version)


def topology(fd):
    for _ in range(3):
        query = bytearray(TOPOLOGY.size)
        fcntl.ioctl(fd, GET_TOPOLOGY, query, True)
        initial = TOPOLOGY.unpack(query)
        counts = initial[1::3]
        if any(count > limit for count, (_, _, limit) in zip(counts, RECORDS)):
            raise ValueError('Media topology exceeds inventory bounds')
        buffers = [ctypes.create_string_buffer(max(1, count * record.size))
                   for count, (_, record, _) in zip(counts, RECORDS)]
        values = [initial[0]]
        for count, buffer in zip(counts, buffers):
            values.extend((count, 0, ctypes.addressof(buffer)))
        query = bytearray(TOPOLOGY.pack(*values))
        try:
            fcntl.ioctl(fd, GET_TOPOLOGY, query, True)
        except OSError as error:
            if error.errno == errno.ENOSPC:
                continue
            raise
        current = TOPOLOGY.unpack(query)
        if current[0] != initial[0] or current[1::3] != counts:
            continue
        return current[0], {name: list(record.iter_unpack(buffer.raw[:count * record.size]))
                            for count, (name, record, _), buffer in zip(counts, RECORDS, buffers)}
    raise ValueError('Media topology changed during all three collection attempts')


def describe(info, version, records):
    objects = {}
    for kind, rows in records.items():
        for row in rows:
            if not row[0] or row[0] in objects:
                raise ValueError('Missing or repeated media object ID')
            objects[row[0]] = kind
    entities = []
    for identifier, name, function, flags in records['entities']:
        role = None
        if info['driver'] == 'cam_req_mgr' and 0x10000 <= function < 0x10000 + len(ROLES):
            role = ROLES[function - 0x10000]
        elif info['driver'] == 'cam_sync' and function == 0x10000:
            role = 'sync'
        entities.append(dict(id=identifier, name=string(name), function=function, flags=flags, role=role))
    interfaces = [dict(zip(('id', 'type', 'flags', 'major', 'minor'), row)) for row in records['interfaces']]
    pads = []
    for identifier, entity, flags, index in records['pads']:
        if objects.get(entity) != 'entities':
            raise ValueError('Media pad references a missing entity')
        pads.append(dict(id=identifier, entity_id=entity, flags=flags, index=index))
    links = []
    for identifier, source, sink, flags in records['links']:
        kind = flags & 0xf0000000
        expected = ('interfaces', 'entities') if kind == 0x10000000 else ('pads', 'pads') if kind == 0 else None
        if expected is None or (objects.get(source), objects.get(sink)) != expected:
            raise ValueError('Invalid media link endpoints or type')
        links.append(dict(id=identifier, source_id=source, sink_id=sink, flags=flags))
    for entity in entities:
        entity['interfaces'] = [link['source_id'] for link in links
                                if link['sink_id'] == entity['id'] and link['flags'] & 0x10000000]
    return dict(schema_version=1, device=info, topology_version=version, entities=entities,
                interfaces=interfaces, pads=pads, links=links,
                sensor_identity_verified=False, active_configuration_verified=False,
                hardware_capture_verified=False)


def collect(path):
    if sys.platform != 'linux' or platform.machine() != 'aarch64' or sys.byteorder != 'little' or ctypes.sizeof(ctypes.c_void_p) != 8:
        raise ValueError('Live collection requires little-endian ARM64 Linux')
    if not re.fullmatch(r'/dev/media(?:0|[1-9][0-9]{0,2})', path):
        raise ValueError('Select one explicit /dev/mediaN node')
    expected = (Path('/sys/class/media') / Path(path).name / 'dev').read_text().strip()
    if not re.fullmatch(r'[0-9]+:[0-9]+', expected):
        raise ValueError('Invalid media sysfs device number')
    numbers = tuple(map(int, expected.split(':')))
    before = os.lstat(path)
    if not stat.S_ISCHR(before.st_mode) or (os.major(before.st_rdev), os.minor(before.st_rdev)) != numbers:
        raise ValueError('Media node and sysfs identity differ')
    fd = os.open(path, os.O_RDONLY | os.O_CLOEXEC | os.O_NONBLOCK | os.O_NOFOLLOW)
    try:
        current = os.fstat(fd)
        if (current.st_mode, current.st_dev, current.st_ino, current.st_rdev) != (before.st_mode, before.st_dev, before.st_ino, before.st_rdev):
            raise ValueError('Media node changed while opening')
        info = device_info(fd)
        version, records = topology(fd)
        return dict(describe(info, version, records), path=path, device_number=list(numbers))
    finally:
        os.close(fd)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--media', required=True, help='One explicit /dev/mediaN node; no subdevice is opened')
    args = parser.parse_args()
    try:
        report = collect(args.media)
    except (OSError, ValueError) as error:
        parser.exit(1, f'Camera inventory failed: {error}\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
