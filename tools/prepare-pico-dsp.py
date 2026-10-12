#!/usr/bin/env python3
"""Prepare private Pico ADSP, CDSP and SLPI firmware from a pinned FAT16 image."""
import argparse
import hashlib
import importlib.util
import json
import math
from pathlib import Path
import re
import struct

spec = importlib.util.spec_from_file_location('qcom_firmware', Path(__file__).with_name('prepare-quest-firmware.py'))
qcom = importlib.util.module_from_spec(spec)
spec.loader.exec_module(qcom)
DOMAIN_FILES = {'adspr.jsn': ('adsp', 'root_pd', 74), 'adspua.jsn': ('adsp', 'audio_pd', 74),
                'cdspr.jsn': ('cdsp', 'root_pd', 76), 'slpir.jsn': ('slpi', 'root_pd', 90),
                'slpius.jsn': ('slpi', 'sensor_pd', 90)}


def require(condition, message):
    if not condition:
        raise ValueError(message)


class FirmwareVolume:
    def __init__(self, data):
        require(512 <= len(data) <= 512 * 1024**2 and data[510:512] == b'\x55\xaa', 'Invalid FAT boot sector')
        self.data = data
        u16 = lambda offset: struct.unpack_from('<H', data, offset)[0]
        sector, per_cluster, reserved, fats = u16(11), data[13], u16(14), data[16]
        root_entries, fat_sectors = u16(17), u16(22)
        total = u16(19) or struct.unpack_from('<I', data, 32)[0]
        require(sector == 4096 and per_cluster == 4 and fats == 2 and reserved > 0 and
                fat_sectors > 0 and root_entries > 0, 'Unsupported Pico FAT16 geometry')
        fat_start, fat_bytes = reserved * sector, fat_sectors * sector
        root_start = (reserved + fats * fat_sectors) * sector
        self.start = root_start + math.ceil(root_entries * 32 / sector) * sector
        self.cluster_bytes, self.declared_bytes = sector * per_cluster, total * sector
        self.cluster_count = (self.declared_bytes - self.start) // self.cluster_bytes
        require(self.start <= len(data) <= self.declared_bytes <= 512 * 1024**2 and
                4085 <= self.cluster_count < 65525, 'Invalid FAT16 bounds')
        fat = data[fat_start:fat_start + fat_bytes]
        require(fat == data[fat_start + fat_bytes:fat_start + 2 * fat_bytes], 'FAT copies disagree')
        self.entries = struct.unpack('<' + str(len(fat) // 2) + 'H', fat)
        require(len(self.entries) >= self.cluster_count + 2 and self.entries[0] >= 0xfff8 and
                self.entries[1] >= 0xfff8, 'Invalid FAT16 allocation table')
        self.owners, self.files, self.paths = {}, {}, set()
        self.walk(data[root_start:root_start + root_entries * 32], '')
        allocated = {i for i in range(2, self.cluster_count + 2) if self.entries[i] != 0}
        require(allocated == set(self.owners), 'Unreferenced allocated FAT clusters')

    def chain(self, first, name):
        blocks = []
        while first < 0xfff8:
            require(2 <= first < self.cluster_count + 2 and first not in self.owners,
                    'Invalid, cyclic or shared FAT chain: ' + name)
            self.owners[first] = name
            blocks.append(first)
            first = self.entries[first]
        return blocks

    def content(self, blocks, size):
        result = bytearray()
        for cluster in blocks:
            count = min(self.cluster_bytes, size - len(result))
            offset = self.start + (cluster - 2) * self.cluster_bytes
            require(count >= 0 and offset + count <= len(self.data), 'Truncated FAT file content')
            result += self.data[offset:offset + count]
        require(len(result) == size, 'FAT file size disagrees with its chain')
        return bytes(result)

    @staticmethod
    def checksum(short):
        result = 0
        for value in short:
            result = (((result & 1) << 7) + (result >> 1) + value) & 255
        return result

    def walk(self, raw, parent, depth=0):
        require(depth < 16, 'FAT directory depth exceeds limit')
        pending = []
        for offset in range(0, len(raw), 32):
            entry = raw[offset:offset + 32]
            require(len(entry) == 32, 'Truncated FAT directory entry')
            if entry[0] == 0:
                break
            if entry[0] == 0xe5:
                pending = []
                continue
            attr, short = entry[11], entry[:11]
            if attr == 15:
                require(entry[12] == 0 and entry[26:28] == b'\0\0', 'Invalid FAT long-name entry')
                pending.append(entry)
                require(len(pending) <= 20, 'FAT long name exceeds limit')
                continue
            if pending:
                count = pending[0][0] & 31
                require(pending[0][0] & 64 and count == len(pending) and
                        [p[0] & 31 for p in pending] == list(range(count, 0, -1)) and
                        all(p[13] == self.checksum(short) for p in pending), 'FAT long-name sequence mismatch')
                text = b''.join(p[1:11] + p[14:26] + p[28:32] for p in reversed(pending))
                units = list(struct.unpack('<' + str(len(text) // 2) + 'H', text))
                if 0 in units:
                    end = units.index(0)
                    require(all(v == 65535 for v in units[end + 1:]), 'Invalid FAT name padding')
                    units = units[:end]
                require(65535 not in units, 'Invalid FAT long name')
                name = struct.pack('<' + str(len(units)) + 'H', *units).decode('utf-16le')
            else:
                name = short[:8].decode('ascii').rstrip()
                if short[8:].strip():
                    name += '.' + short[8:].decode('ascii').rstrip()
            pending = []
            if name in ('.', '..') or attr & 8:
                continue
            require(name and all(c not in name for c in ('/', '\\', '\0')), 'Unsafe FAT name')
            path = parent + '/' + name if parent else name
            require(path not in self.paths and len(self.paths) < 4096, 'Duplicate or excessive FAT entries')
            self.paths.add(path)
            first, size = struct.unpack_from('<H', entry, 26)[0], struct.unpack_from('<I', entry, 28)[0]
            require(struct.unpack_from('<H', entry, 20)[0] == 0, 'Unexpected FAT32 cluster field')
            if attr & 16:
                require(size == 0 and first >= 2, 'Invalid FAT directory chain')
                blocks = self.chain(first, path)
                self.walk(self.content(blocks, len(blocks) * self.cluster_bytes), path, depth + 1)
            else:
                require(path not in self.files, 'Duplicate FAT file')
                blocks = self.chain(first, path) if first else []
                require(len(blocks) == math.ceil(size / self.cluster_bytes), 'FAT file allocation mismatch')
                payload = self.content(blocks, size)
                self.files[path] = {'bytes': size, 'sha256': hashlib.sha256(payload).hexdigest(), 'clusters': blocks}

    def read(self, path):
        item = self.files[path]
        return self.content(item['clusters'], item['bytes'])


def inspect_files(directory, extra_files=()):
    records, domains, expected = [], [], set(DOMAIN_FILES)
    for processor in ('adsp', 'cdsp', 'slpi'):
        name = processor + '.mdt'
        qcom.digest(directory / name)
        require((directory / name).stat().st_size <= 1024**2, 'DSP MDT exceeds size limit')
        data = (directory / name).read_bytes()
        required = qcom.required_segments(data, processor)
        expected.update([name, *required])
        for segment, size in required.items():
            qcom.digest(directory / segment)
            require((directory / segment).stat().st_size == size, 'DSP segment length mismatch: ' + segment)
        require(data.endswith((directory / (processor + '.b01')).read_bytes()), 'DSP signing metadata differs')
    require({p.name for p in directory.iterdir()} == expected | set(extra_files),
            'Missing or unexpected DSP firmware files')
    for name, (processor, subdomain, instance) in DOMAIN_FILES.items():
        path = directory / name
        qcom.digest(path)
        require(0 < path.stat().st_size <= 65536, 'DSP service JSON exceeds limit')
        value = json.loads(path.read_text())
        require(value.get('sr_domain') == {'soc': 'msm', 'domain': processor, 'subdomain': subdomain,
                                         'qmi_instance_id': instance}, 'Unexpected DSP service domain: ' + name)
        services = value.get('sr_service')
        require(isinstance(services, list) and 1 <= len(services) <= 32, 'Invalid DSP services')
        for service in services:
            require(all(isinstance(service.get(k), str) and re.fullmatch(r'[a-z0-9_]+', service[k])
                        for k in ('provider', 'service')), 'Invalid DSP service name')
        require(any(s['provider'] == 'tms' and s['service'] == 'servreg' for s in services), 'Missing DSP service registry')
        domains.append({'file': name, 'domain': value['sr_domain'], 'services': services})
    for name in sorted(expected):
        path = directory / name
        records.append({'path': name, 'bytes': path.stat().st_size, 'sha256': qcom.digest(path)})
    return records, domains


def validate_record(record, files_directory, extra_files=()):
    require(record.get('schema_version') == 1 and record.get('status') == 'prepared' and
            record.get('component') == 'pico-neo3-dsp' and
            re.fullmatch(r'smartcm\.[0-9]{10}', record.get('reference_build', '')) and
            re.fullmatch(r'[a-f0-9]{64}', record.get('source_image_sha256', '')), 'Invalid Pico DSP manifest')
    files, domains = inspect_files(files_directory, extra_files)
    require(record.get('files') == files and record.get('service_domains') == domains, 'DSP bundle changed')
    return record


def validate(directory):
    directory = Path(directory)
    require(not directory.is_symlink() and not (directory / 'files').is_symlink(), 'DSP directories must not be symlinks')
    qcom.digest(directory / 'manifest.json')
    return validate_record(json.loads((directory / 'manifest.json').read_text()), directory / 'files')


def prepare(source, expected_sha, reference_build, output):
    require(not output.exists() and not output.is_symlink(), 'Choose a new output directory')
    require(re.fullmatch(r'[a-f0-9]{64}', expected_sha) and re.fullmatch(r'smartcm\.[0-9]{10}', reference_build),
            'Requires a firmware image SHA-256 and Neo3 reference build')
    require(source.stat().st_size <= 512 * 1024**2, 'Firmware image exceeds size limit')
    require(qcom.digest(source) == expected_sha, 'Firmware image checksum mismatch')
    volume = FirmwareVolume(source.read_bytes())
    selected = {name: volume.read('image/' + name) for name in DOMAIN_FILES}
    for processor in ('adsp', 'cdsp', 'slpi'):
        name = processor + '.mdt'
        data = volume.read('image/' + name)
        require(len(data) <= 1024**2, 'DSP MDT exceeds size limit')
        required = qcom.required_segments(data, processor)
        require(data == volume.read('image/' + processor + '.b00') + volume.read('image/' + processor + '.b01'),
                'DSP MDT does not match its split metadata')
        selected[name] = data
        selected.update({n: volume.read('image/' + n) for n in required})
    require(qcom.digest(source) == expected_sha, 'Firmware image changed during extraction')
    (output / 'files').mkdir(parents=True)
    for name, data in selected.items():
        (output / 'files' / name).write_bytes(data)
    files, domains = inspect_files(output / 'files')
    record = {'schema_version': 1, 'status': 'prepared', 'component': 'pico-neo3-dsp',
              'source_image_sha256': expected_sha, 'reference_build': reference_build,
              'files': files, 'service_domains': domains, 'firmware_executed': False,
              'manufacturer_signature_verified': False, 'device_compatibility_verified': False,
              'fat': {'source_bytes': len(volume.data), 'declared_bytes': volume.declared_bytes,
                      'verified_files': len(volume.files), 'allocated_clusters': len(volume.owners),
                      'omitted_tail_contains_file_data': False}}
    (output / 'manifest.json').write_text(json.dumps(record, indent=2) + '\n')
    validate(output)
    print(output / 'manifest.json')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('firmware_image', type=Path)
    parser.add_argument('--sha256', required=True)
    parser.add_argument('--reference-build', required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    try:
        prepare(args.firmware_image, args.sha256, args.reference_build, args.output)
    except (OSError, ValueError, KeyError, struct.error) as error:
        parser.exit(1, f'{error}\n')
