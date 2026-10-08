"""Read compiled FDT properties for offline board inventories."""
import struct


def read_fdt(data):
    if len(data) < 40:
        raise ValueError("Truncated FDT header")
    magic, size, off_struct, off_strings, _, version, _, _, strings_size, struct_size = struct.unpack_from(">10I", data)
    if magic != 0xd00dfeed or version != 17 or not 40 <= size <= len(data):
        raise ValueError("Expected a complete version-17 FDT")
    if any(offset < 40 or offset + length > size for offset, length in
           ((off_struct, struct_size), (off_strings, strings_size))):
        raise ValueError("FDT block is out of bounds")
    block = data[off_struct:off_struct + struct_size]
    strings = data[off_strings:off_strings + strings_size]
    nodes, stack, cursor = {}, [], 0
    while cursor + 4 <= len(block):
        token, = struct.unpack_from(">I", block, cursor)
        cursor += 4
        if token == 1:
            end = block.find(b"\0", cursor)
            if end < 0:
                raise ValueError("Unterminated FDT node")
            name = block[cursor:end].decode("ascii")
            stack.append(name)
            path = "/".join(stack) or "/"
            if path in nodes:
                raise ValueError("Duplicate FDT node")
            nodes[path] = {}
            cursor = (end + 4) & ~3
        elif token == 2:
            if not stack:
                raise ValueError("Unbalanced FDT node")
            stack.pop()
        elif token == 3:
            if not stack or cursor + 8 > len(block):
                raise ValueError("Truncated FDT property")
            length, name_offset = struct.unpack_from(">II", block, cursor)
            cursor += 8
            end = strings.find(b"\0", name_offset)
            if end < 0 or cursor + length > len(block):
                raise ValueError("FDT property is out of bounds")
            name = strings[name_offset:end].decode("ascii")
            nodes["/".join(stack) or "/"][name] = block[cursor:cursor + length]
            cursor = (cursor + length + 3) & ~3
        elif token == 4:
            continue
        elif token == 9 and not stack and "/" in nodes:
            return nodes
        else:
            raise ValueError(f"Invalid FDT token/state: {token}")
    raise ValueError("FDT has no end marker")


def string_list(data):
    return data.rstrip(b"\0").decode("utf-8").split("\0") if data else []


def boot_memory(data):
    nodes = read_fdt(data)

    def count(properties, key, default):
        value = properties.get(key, struct.pack(">I", default))
        if len(value) != 4 or (result := struct.unpack(">I", value)[0]) not in (1, 2):
            raise ValueError(f"Unsupported memory cell count: {key}")
        return result

    def ranges(value, address_cells, size_cells):
        width = 4 * (address_cells + size_cells)
        if not value or len(value) % width:
            raise ValueError("Invalid memory range geometry")
        result = []
        for offset in range(0, len(value), width):
            split = offset + 4 * address_cells
            start = int.from_bytes(value[offset:split], "big")
            size = int.from_bytes(value[split:offset + width], "big")
            if start + size > 1 << 64:
                raise ValueError("Memory range overflows 64 bits")
            result.append({"start": start, "size": size})
        return result

    root = nodes["/"]
    address_cells, size_cells = count(root, "#address-cells", 2), count(root, "#size-cells", 1)
    ram, reserved, dynamic, unresolved = [], [], [], []
    for path, properties in nodes.items():
        if properties.get("device_type") != b"memory\0" or properties.get("status", b"okay\0") not in (b"okay\0", b"ok\0"):
            continue
        if path.count("/") != 1:
            raise ValueError("Non-root RAM address translation is unsupported")
        entries = ranges(properties.get("reg", b""), address_cells, size_cells)
        ram.extend({"path": path, **entry} for entry in entries if entry["size"])
        if any(not entry["size"] for entry in entries):
            unresolved.append(path)
    parent = nodes.get("/reserved-memory", {})
    address_cells, size_cells = count(parent, "#address-cells", address_cells), count(parent, "#size-cells", size_cells)
    if parent.get("ranges", b""):
        raise ValueError("Translated reserved-memory addresses are unsupported")
    for path, properties in nodes.items():
        if not path.startswith("/reserved-memory/") or path.count("/") != 2:
            continue
        if properties.get("status", b"okay\0") not in (b"okay\0", b"ok\0"):
            continue
        if "reg" in properties:
            entries = ranges(properties["reg"], address_cells, size_cells)
            reserved.extend({"path": path, **entry} for entry in entries if entry["size"])
            if any(not entry["size"] for entry in entries):
                unresolved.append(path)
        elif "size" in properties:
            if len(properties["size"]) != 4 * size_cells:
                raise ValueError("Invalid dynamic reservation size")
            size = int.from_bytes(properties["size"], "big")
            if not size:
                raise ValueError("Empty dynamic reservation")
            dynamic.append({"path": path, "size": size, "allocation_ranges":
                            ranges(properties["alloc-ranges"], address_cells, size_cells)
                            if "alloc-ranges" in properties else []})
        else:
            unresolved.append(path)
    total, structure, strings, reserve_offset = struct.unpack_from(">4I", data, 4)
    strings_size, structure_size = struct.unpack_from(">2I", data, 32)
    if reserve_offset < 40 or reserve_offset % 8 or any(
            start <= reserve_offset < start + size for start, size in
            ((structure, structure_size), (strings, strings_size))):
        raise ValueError("Invalid FDT reservation map offset")
    limit = min(offset for offset in (structure, strings, total) if offset > reserve_offset)
    while True:
        if reserve_offset + 16 > limit:
            raise ValueError("Unterminated FDT reservation map")
        start, size = struct.unpack_from(">QQ", data, reserve_offset)
        reserve_offset += 16
        if not start and not size:
            break
        if not size or start + size > 1 << 64:
            raise ValueError("Invalid FDT reservation range")
        reserved.append({"path": "FDT reservation map", "start": start, "size": size})
    ordered = sorted(ram, key=lambda entry: entry["start"])
    if any(left["start"] + left["size"] > right["start"] for left, right in zip(ordered, ordered[1:])):
        raise ValueError("Overlapping RAM banks")
    return {"ram": ram, "reserved": reserved, "dynamic_reservations": dynamic,
            "unresolved_nodes": unresolved, "fixed_memory_map_complete": bool(ram) and not dynamic and not unresolved}


def inventory(data):
    nodes = read_fdt(data)
    root = nodes["/"]
    result = {key: string_list(root.get(key, b"")) for key in ("model", "compatible")}
    for key in ("qcom,msm-id", "qcom,board-id", "qcom,pmic-id"):
        value = root.get(key, b"")
        if len(value) % 4:
            raise ValueError(f"Invalid cell array: {key}")
        result[key] = list(struct.unpack(f">{len(value) // 4}I", value))
    result["nodes"] = []
    for path, props in nodes.items():
        if "compatible" not in props:
            continue
        ancestors = ["/"] + [path.rsplit("/", n)[0] for n in range(path.count("/"))]
        enabled = all(nodes.get(parent, {}).get("status", b"okay\0") in (b"okay\0", b"ok\0") for parent in ancestors)
        result["nodes"].append({"path": path, "compatible": string_list(props["compatible"]),
                                "status": string_list(props.get("status", b"okay\0"))[0],
                                "enabled_in_tree": enabled})
    return result
