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
