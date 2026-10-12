#!/usr/bin/env python3
"""Check Pico DSP relocation bounds against an explicitly selected merged DTB."""
import argparse
import importlib.util
import json
from pathlib import Path
import struct

from devicetree import boot_memory, read_fdt, string_list

spec = importlib.util.spec_from_file_location('pico_firmware', Path(__file__).with_name('prepare-pico-firmware.py'))
firmware = importlib.util.module_from_spec(spec)
spec.loader.exec_module(firmware)
require = firmware.dsp.require


def cell(data):
    require(len(data) == 4, 'Expected a single device-tree cell')
    return int.from_bytes(data, 'big')


def available(nodes, path):
    while True:
        require(nodes[path].get('status', b'okay\0') in (b'okay\0', b'ok\0'), 'Disabled DSP dependency: ' + path)
        if path == '/':
            return
        path = path.rsplit('/', 1)[0] or '/'


def segment_layout(data, name):
    firmware.qcom.required_segments(data, name)
    header = struct.unpack_from('<16sHHIIIIIHHHHHH', data)
    segments = []
    for index in range(header[10]):
        kind, _, _, address, size, memory, flags, alignment = struct.unpack_from('<8I', data, header[5] + index * header[9])
        if kind != 1 or flags & (7 << 24) == 2 << 24 or not memory:
            continue
        require(flags & (1 << 27), 'Non-relocatable DSP segment')
        require(alignment >= 4096 and alignment & (alignment - 1) == 0, 'Invalid DSP alignment')
        segments.append({'index': index, 'address': address, 'file_bytes': size, 'memory_bytes': memory,
                         'alignment': alignment})
    segments.sort(key=lambda item: item['address'])
    require(segments, 'Missing loadable DSP segments')
    require(all(a['address'] + a['memory_bytes'] <= b['address'] for a, b in zip(segments, segments[1:])),
            'Overlapping DSP segments')
    require(any(s['address'] <= header[4] < s['address'] + s['memory_bytes'] for s in segments),
            'DSP entry is outside its loadable segments')
    start = segments[0]['address']
    end = (max(s['address'] + s['memory_bytes'] for s in segments) + 4095) & ~4095
    alignment = segments[0]['alignment']
    allocation_alignment = 4 * 1024**2 if alignment >= 4 * 1024**2 else 1024**2 if alignment >= 1024**2 else 4096
    size = (end - start + allocation_alignment - 1) & ~(allocation_alignment - 1)
    return {'entry': header[4], 'base': start, 'span_bytes': end - start, 'allocation_bytes': size,
            'alignment': alignment, 'segments': segments}


def contract(nodes, memory, files):
    require('qcom,kona' in string_list(nodes['/'].get('compatible', b'')), 'Requires a Kona board tree')
    parent = nodes.get('/reserved-memory', {})
    require(cell(parent.get('#address-cells', b'')) == 2 and cell(parent.get('#size-cells', b'')) == 2 and
            not parent.get('ranges', b''), 'Unsupported DSP reservation addressing')
    handles = {}
    for path, props in nodes.items():
        values = [cell(props[k]) for k in ('phandle', 'linux,phandle') if k in props]
        if values:
            require(values[0] not in (0, 0xffffffff) and all(v == values[0] for v in values) and
                    values[0] not in handles, 'Invalid or duplicate phandle')
            handles[values[0]] = path
    result = []
    for name, pas_id in (('adsp', 1), ('cdsp', 18), ('slpi', 12)):
        matches = [(path, props) for path, props in nodes.items()
                   if props.get('qcom,firmware-name') == (name + '\0').encode() and
                   'qcom,pil-tz-generic' in string_list(props.get('compatible', b''))]
        require(len(matches) == 1, 'Requires one PIL node for ' + name)
        path, props = matches[0]
        available(nodes, path)
        require(cell(props.get('qcom,pas-id', b'')) == pas_id and 'qcom,pil-no-auth' not in props,
                'Unexpected DSP authentication contract')
        reservation = handles.get(cell(props.get('memory-region', b'')))
        require(reservation is not None and reservation.startswith('/reserved-memory/') and
                reservation.count('/') == 2, 'Unresolved DSP reservation')
        require(all(item['reservation'] != reservation for item in result), 'DSPs share one reserved pool')
        available(nodes, reservation)
        region = nodes[reservation]
        require(region.get('compatible') == b'removed-dma-pool\0' and region.get('no-map') == b'' and
                'reusable' not in region and len(region.get('reg', b'')) == 16, 'Unsupported DSP DMA pool')
        start, size = struct.unpack('>QQ', region['reg'])
        require(size > 0 and start > 0 and start + size <= 2**32 and (start | size) & 4095 == 0,
                'Invalid DSP reservation bounds')
        require(any(r['path'] == reservation and r['start'] == start and r['size'] == size
                    for r in memory['reserved']), 'DSP reservation is not enabled')
        for other in memory['reserved']:
            if other['path'] != reservation:
                require(max(start, other['start']) >= min(start + size, other['start'] + other['size']),
                        'DSP reservation overlaps ' + other['path'])
        layout = segment_layout((files / (name + '.mdt')).read_bytes(), name)
        require(start % layout['alignment'] == 0 and layout['allocation_bytes'] <= size,
                'DSP allocation does not fit its reserved pool')
        for segment in layout['segments']:
            segment['relocated_address'] = start + segment['address'] - layout['base']
            require(start <= segment['relocated_address'] and
                    segment['relocated_address'] + segment['memory_bytes'] <= start + size,
                    'Relocated DSP segment exceeds its reserved pool')
        result.append({'processor': name, 'node': path, 'pas_id': pas_id, 'reservation': reservation,
                       'start': start, 'size': size, 'unused_bytes': size - layout['allocation_bytes'],
                       'relocated_entry': start + layout['entry'] - layout['base'], **layout})
    return result


def run(args):
    require(not args.output.exists() and not args.output.is_symlink(), 'Choose a new output file')
    record = firmware.validate(args.firmware)
    require(record['component'] == 'pico-neo3-firmware', 'Requires the combined GPU/DSP firmware bundle')
    dtb_sha = firmware.digest(args.device_tree)
    data = args.device_tree.read_bytes()
    result = contract(read_fdt(data), boot_memory(data), args.firmware / 'files')
    require(firmware.digest(args.device_tree) == dtb_sha and firmware.validate(args.firmware) == record,
            'Inputs changed during verification')
    report = {'status': 'passed', 'scope': 'offline DSP relocation and reserved-memory contract',
              'device_tree_sha256': dtb_sha, 'firmware_manifest_sha256': firmware.digest(args.firmware / 'manifest.json'),
              'reference_build': record['reference_build'], 'processors': result,
              'firmware_executed': False, 'physical_memory_verified': False, 'flash_ready': False}
    args.output.write_text(json.dumps(report, indent=2) + '\n')
    print(args.output)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('firmware', type=Path)
    parser.add_argument('device_tree', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    try:
        run(parser.parse_args())
    except (OSError, ValueError, KeyError, struct.error) as error:
        parser.exit(1, str(error) + '\n')
