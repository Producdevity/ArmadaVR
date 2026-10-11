#!/usr/bin/env python3
"""Bind an explicitly selected Pico panel to its native mux in an offline DTB."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess

from devicetree import boot_memory, read_fdt, string_list


PROPERTY = "pico,native-mipi-mux"
GPIO_PROPERTIES = (
    "qcom,vxr7200-rst", "qcom,vxr7200-3v3en", "qcom,vxr7200-1v0en",
    "qcom,vxr7200-1v8en", "qcom,vxr7200-mipi_switch",
)


def cell(value):
    if len(value) != 4:
        raise ValueError("Expected one device-tree cell")
    return struct.unpack(">I", value)[0]


def available(nodes, path):
    if not path.startswith("/") or path not in nodes:
        raise ValueError(f"Missing node: {path}")
    while True:
        if nodes[path].get("status", b"okay\0") not in (b"okay\0", b"ok\0"):
            raise ValueError(f"Disabled node: {path}")
        if path == "/":
            return
        path = path.rsplit("/", 1)[0] or "/"


def binding(nodes, panel, bridge):
    available(nodes, panel)
    available(nodes, bridge)
    if panel == bridge or PROPERTY in nodes[panel]:
        raise ValueError("Expected a separate panel with no existing mux binding")
    names = string_list(nodes[panel].get("qcom,mdss-dsi-panel-name", b""))
    if len(names) != 1 or not names[0]:
        raise ValueError("Selected node has no DSI panel name")
    if "qcom,vxr7200" not in string_list(nodes[bridge].get("compatible", b"")):
        raise ValueError("Selected bridge is not a VXR7200")

    handles = {}
    for path, props in nodes.items():
        values = [cell(props[key]) for key in ("phandle", "linux,phandle") if key in props]
        if not values:
            continue
        if any(value != values[0] for value in values) or values[0] in (0, 0xffffffff):
            raise ValueError(f"Invalid phandle: {path}")
        if values[0] in handles:
            raise ValueError("Duplicate device-tree phandle")
        handles[values[0]] = path

    gpios = {}
    for name in GPIO_PROPERTIES:
        value = nodes[bridge].get(name, b"")
        if len(value) != 12:
            raise ValueError(f"Expected one two-cell GPIO specifier: {name}")
        controller, pin, flags = struct.unpack(">III", value)
        path = handles.get(controller)
        if path is None:
            raise ValueError(f"Unknown GPIO controller: {name}")
        available(nodes, path)
        props = nodes[path]
        if "gpio-controller" not in props or cell(props.get("#gpio-cells", b"")) != 2:
            raise ValueError(f"Invalid GPIO controller: {name}")
        if flags != 0 or pin == 0xffffffff:
            raise ValueError(f"Unsupported GPIO polarity or pin: {name}")
        if "ngpios" in props and pin >= cell(props["ngpios"]):
            raise ValueError(f"GPIO outside controller range: {name}")
        if any(item["controller"] == path and item["pin"] == pin for item in gpios.values()):
            raise ValueError("Bridge controls share a GPIO")
        gpios[name] = {"controller": path, "pin": pin, "flags": flags}

    bridge_handle = next((value for value, path in handles.items() if path == bridge), None)
    create_handle = bridge_handle is None
    if create_handle:
        bridge_handle = max(handles, default=0) + 1
        if bridge_handle >= 0xffffffff:
            raise ValueError("No free device-tree phandle")
    return {"panel": panel, "bridge": bridge, "bridge_phandle": bridge_handle,
            "create_bridge_phandle": create_handle, "gpios": gpios}


def build(source, output, expected_sha256, panel, bridge):
    data = source.read_bytes()
    digest = hashlib.sha256(data).hexdigest()
    if not re.fullmatch(r"[0-9a-f]{64}", expected_sha256) or digest != expected_sha256:
        raise ValueError("Source DTB does not match the explicit SHA-256")
    nodes = read_fdt(data)
    plan = binding(nodes, panel, bridge)
    memory = boot_memory(data)
    tool = shutil.which("fdtput")
    if not tool:
        raise ValueError("fdtput from dtc is required")
    output.mkdir(parents=True, exist_ok=False)
    target = output / "panel-mux.dtb"
    target.write_bytes(data)
    edits = [(panel, PROPERTY)]
    if plan["create_bridge_phandle"]:
        edits.insert(0, (bridge, "phandle"))
    expected = {path: dict(props) for path, props in nodes.items()}
    for path, key in edits:
        subprocess.run([tool, "-t", "x", str(target), path, key,
                        f"{plan['bridge_phandle']:x}"], check=True, capture_output=True, timeout=15)
        expected[path][key] = struct.pack(">I", plan["bridge_phandle"])
    result = target.read_bytes()
    if read_fdt(result) != expected or boot_memory(result) != memory or result[28:32] != data[28:32]:
        raise ValueError("Unexpected device-tree change outside mux binding")
    if source.read_bytes() != data:
        raise ValueError("Source DTB changed during assembly")
    report = {"schema": 1, "source_sha256": digest,
              "output_sha256": hashlib.sha256(result).hexdigest(), **plan,
              "property_changes": [{"node": path, "property": key} for path, key in edits],
              "other_properties_unchanged": True, "memory_layout_unchanged": True,
              "live_selection_verified": False, "physical_gpio_state_verified": False,
              "boot_verified": False, "recovery_verified": False, "flash_ready": False}
    (output / "binding.json").write_text(json.dumps(report, indent=2) + "\n")
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path, help="new output directory")
    parser.add_argument("--sha256", required=True, help="expected source DTB digest")
    parser.add_argument("--panel", required=True, help="explicit full DSI panel node path")
    parser.add_argument("--bridge", required=True, help="explicit full VXR7200 node path")
    args = parser.parse_args()
    try:
        result = build(args.source, args.output, args.sha256, args.panel, args.bridge)
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        parser.exit(1, f"Cannot bind Pico panel: {error}\n")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
