#!/usr/bin/env python3
"""Compare compiled Qualcomm WMI TLV schemas without loading either module."""
import argparse
import hashlib
import json
from pathlib import Path
import struct

TABLES = ('cmd_attr_list', 'evt_attr_list')


def require(condition, message):
    if not condition:
        raise ValueError(message)


def elf_tables(data):
    require(len(data) >= 64 and data[:7] == b'\x7fELF\x02\x01\x01', 'Expected little-endian ELF64')
    header = struct.unpack_from('<16sHHIQQQIHHHHHH', data)
    require(header[1:4] == (1, 183, 1) and header[11] == 64 and 0 < header[12] <= 4096,
            'Expected an ARM64 relocatable module with section headers')
    require(header[6] >= 64 and header[6] + 64 * header[12] <= len(data), 'Truncated ELF section headers')
    sections = [struct.unpack_from('<IIQQQQIIQQ', data, header[6] + i * 64) for i in range(header[12])]

    def block(section):
        require(section[1] != 8 and section[4] + section[5] <= len(data), 'Invalid ELF section bounds')
        return data[section[4]:section[4] + section[5]]

    result = {}
    for section in sections:
        if section[1] != 2:
            continue
        require(section[9] == 24 and section[5] % 24 == 0 and section[6] < len(sections), 'Invalid ELF symbol table')
        symbols = block(section)
        strings = block(sections[section[6]])
        for offset in range(0, len(symbols), 24):
            name, info, _, index, value, size = struct.unpack_from('<IBBHQQ', symbols, offset)
            end = strings.find(b'\0', name)
            require(end >= name, 'Unterminated ELF symbol name')
            name = strings[name:end]
            if name not in (b'cmd_attr_list', b'evt_attr_list'):
                continue
            name = name.decode('ascii')
            require(name not in result and info & 15 == 1 and 0 < index < len(sections), 'Invalid schema symbol')
            target = sections[index]
            require(size > 0 and size % 4 == 0 and value + size <= target[5], 'Invalid schema array bounds')
            payload = block(target)[value:value + size]
            for relocation in sections:
                if relocation[1] not in (4, 9) or relocation[7] != index:
                    continue
                width = 24 if relocation[1] == 4 else 16
                require(relocation[9] == width and relocation[5] % width == 0, 'Invalid relocation table')
                raw = block(relocation)
                for location in range(0, len(raw), width):
                    address = struct.unpack_from('<Q', raw, location)[0]
                    require(not value <= address < value + size, 'Schema array requires runtime relocation')
            result[name] = payload
    require(set(result) == set(TABLES), 'Module lacks complete command and event schema arrays')
    return result


def decode_table(data):
    require(data and len(data) % 4 == 0, 'Invalid schema word array')
    words = struct.unpack('<' + 'I' * (len(data) // 4), data)
    result, offset = {}, 0
    while offset < len(words):
        header = words[offset]
        count, ident = header >> 24, header & 0xffffff
        require(ident not in result and count > 0 and offset + count < len(words), 'Duplicate or truncated message schema')
        tags = []
        for value in words[offset + 1:offset + count + 1]:
            require(value >> 31 == 0, 'Unsupported schema attribute bits')
            tags.append({'tag': value & 0xfff, 'struct_bytes': (value >> 12) & 0x1ff,
                         'array_size': (value >> 21) & 0x1ff, 'variable': bool((value >> 30) & 1)})
        result[ident] = tags
        offset += count + 1
    return result


def compare(reference, candidate):
    result = {}
    for name in TABLES:
        first, second = decode_table(reference[name]), decode_table(candidate[name])
        shared = first.keys() & second.keys()
        result[name] = {
            'reference_messages': len(first), 'candidate_messages': len(second),
            'reference_sha256': hashlib.sha256(reference[name]).hexdigest(),
            'candidate_sha256': hashlib.sha256(candidate[name]).hexdigest(),
            'equal_messages': sum(first[i] == second[i] for i in shared),
            'changed': [{'id': i, 'reference': first[i], 'candidate': second[i]}
                        for i in sorted(shared) if first[i] != second[i]],
            'missing': sorted(first.keys() - second.keys()), 'added': sorted(second.keys() - first.keys())}
    return result


def run(args):
    require(not args.output.exists() and not args.output.is_symlink(), 'Choose a new output file')
    tables, inputs = {}, {}
    for name, path in (('reference', args.reference), ('candidate', args.candidate)):
        require(not path.is_symlink() and path.is_file() and path.stat().st_size <= 512 * 1024**2,
                'Expected a regular module no larger than 512 MiB')
        data = path.read_bytes()
        tables[name] = elf_tables(data)
        inputs[name] = {'path': str(path.absolute()), 'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest()}
    result = compare(tables['reference'], tables['candidate'])
    matches = all(not item[key] for item in result.values() for key in ('changed', 'missing', 'added'))
    report = {'status': 'matching' if matches else 'different', 'inputs': inputs, 'tables': result,
              'scope': 'compiled WMI message IDs, TLV order, tags, sizes and variable-array flags',
              'field_semantics_verified': False, 'hardware_descriptors_verified': False,
              'hardware_verified': False, 'flash_ready': False}
    args.output.write_text(json.dumps(report, indent=2) + '\n')
    print(args.output)
    return 0 if matches else 1


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('reference', type=Path)
    parser.add_argument('candidate', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    try:
        raise SystemExit(run(parser.parse_args()))
    except (OSError, ValueError, struct.error) as error:
        parser.exit(1, str(error) + '\n')
