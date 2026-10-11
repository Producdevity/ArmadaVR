#!/usr/bin/env python3
"""Bind a Pico panel in a stock DT overlay and verify its complete merged tree."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile

from devicetree import boot_memory, read_fdt, string_list


spec = importlib.util.spec_from_file_location("pico_panel", Path(__file__).with_name("bind-pico-panel.py"))
panel_binding = importlib.util.module_from_spec(spec)
spec.loader.exec_module(panel_binding)


def read_source(path, expected):
    if path.is_symlink() or not path.is_file() or path.stat().st_size > 4 * 1024**2:
        raise ValueError("Requires a regular DTB or overlay of at most 4 MiB")
    data = path.read_bytes()
    if hashlib.sha256(data).hexdigest() != expected:
        raise ValueError("Device-tree source SHA-256 mismatch")
    read_fdt(data)
    return data


def overlay_node(base, overlay, path):
    candidates = []
    for fragment, props in overlay.items():
        if fragment.count("/") != 1 or fragment + "/__overlay__" not in overlay:
            continue
        if "target-path" in props:
            targets = string_list(props["target-path"])
        elif props.get("target") == struct.pack(">I", 0xffffffff):
            labels = [name for name, value in overlay.get("/__fixups__", {}).items()
                      if fragment + ":target:0" in string_list(value)]
            targets = [target for name in labels
                       for target in string_list(base.get("/__symbols__", {}).get(name, b""))]
        else:
            value = props.get("target", b"")
            panel_binding.cell(value)
            targets = [name for name, item in base.items()
                       if item.get("phandle", item.get("linux,phandle")) == value]
        if len(targets) != 1 or targets[0] not in base:
            raise ValueError("Overlay fragment has no unique base target: " + fragment)
        target = targets[0]
        if path != target and not path.startswith(target.rstrip("/") + "/"):
            continue
        relative = path[len(target.rstrip("/")):]
        candidate = fragment + "/__overlay__" + relative
        if candidate in overlay:
            candidates.append(candidate)
    if len(candidates) != 1:
        raise ValueError("Selected node must occur in exactly one overlay fragment: " + path)
    return candidates[0]


def local_handles(nodes):
    handles = {}
    for path, props in nodes.items():
        if path.startswith("/__"):
            continue
        values = [panel_binding.cell(props[k]) for k in ("phandle", "linux,phandle") if k in props]
        if not values:
            continue
        if values[0] in (0, 0xffffffff) or any(v != values[0] for v in values) or values[0] in handles:
            raise ValueError("Invalid or duplicate overlay phandle")
        handles[values[0]] = path
    return handles


def build(base_path, overlay_path, output, base_sha, overlay_sha, panel, bridge, display, expected_default):
    if output.exists() or output.is_symlink():
        raise ValueError("Choose a new overlay output directory")
    base_data, overlay_data = read_source(base_path, base_sha), read_source(overlay_path, overlay_sha)
    base, overlay = read_fdt(base_data), read_fdt(overlay_data)
    apply = shutil.which("fdtoverlay")
    put = shutil.which("fdtput")
    if not apply or not put:
        raise ValueError("fdtoverlay and fdtput from dtc are required")
    paths = {"panel": overlay_node(base, overlay, panel), "bridge": overlay_node(base, overlay, bridge),
             "display": overlay_node(base, overlay, display)}
    handles = local_handles(overlay)
    panel_handle = next((h for h, p in handles.items() if p == paths["panel"]), None)
    if panel_handle is None:
        raise ValueError("Selected overlay panel requires an existing phandle")
    bridge_handle = next((h for h, p in handles.items() if p == paths["bridge"]), None)
    create_bridge = bridge_handle is None
    if create_bridge:
        bridge_handle = max(handles, default=0) + 1
        if bridge_handle >= 0xffffffff:
            raise ValueError("No free overlay phandle")
    default_fixup = "/__local_fixups__" + paths["display"]
    if overlay.get(default_fixup, {}).get("qcom,dsi-default-panel") != struct.pack(">I", 0):
        raise ValueError("Existing default-panel phandle requires one local relocation")

    def call(command):
        subprocess.run(command, check=True, capture_output=True, timeout=15)

    with tempfile.TemporaryDirectory(prefix="pico-panel-overlay-") as temporary:
        temp = Path(temporary)
        merged = temp / "original.dtb"
        call([apply, "-i", str(base_path), "-o", str(merged), str(overlay_path)])
        plan = panel_binding.build(merged, temp / "selected", hashlib.sha256(merged.read_bytes()).hexdigest(),
                                   panel, bridge, display, expected_default)
        desired = (temp / "selected/panel-mux.dtb").read_bytes()
        output.mkdir(parents=True)
        target = output / "panel-mux.dtbo"
        target.write_bytes(overlay_data)
        edits = [(paths["panel"], panel_binding.PROPERTY, bridge_handle),
                 (paths["display"], "qcom,dsi-default-panel", panel_handle),
                 ("/__local_fixups__" + paths["panel"], panel_binding.PROPERTY, 0)]
        if create_bridge:
            edits.insert(0, (paths["bridge"], "phandle", bridge_handle))
        expected = {p: dict(v) for p, v in overlay.items()}
        for path, key, value in edits:
            call([put, "-p", "-t", "x", str(target), path, key, f"{value:x}"])
            parts = path.strip("/").split("/")
            for i in range(1, len(parts) + 1):
                expected.setdefault("/" + "/".join(parts[:i]), {})
            expected[path][key] = struct.pack(">I", value)
        if read_fdt(target.read_bytes()) != expected:
            raise ValueError("Unexpected changes to stock overlay")
        result = output / "panel-mux.dtb"
        call([apply, "-i", str(base_path), "-o", str(result), str(target)])
        data = result.read_bytes()
        if (read_fdt(data) != read_fdt(desired) or boot_memory(data) != boot_memory(desired) or
                data[28:32] != desired[28:32]):
            raise ValueError("Modified overlay does not produce the exact selected panel tree")
        if base_path.read_bytes() != base_data or overlay_path.read_bytes() != overlay_data:
            raise ValueError("Device-tree sources changed during assembly")
        report = {"schema": 1, "base_sha256": base_sha, "source_overlay_sha256": overlay_sha,
                  "overlay_sha256": hashlib.sha256(target.read_bytes()).hexdigest(),
                  "merged_sha256": hashlib.sha256(data).hexdigest(), "binding": plan,
                  "overlay_nodes": paths, "local_bridge_phandle": bridge_handle,
                  "local_panel_phandle": panel_handle, "merged_tree_matches_binding": True,
                  "source_inputs_unchanged": True, "bootloader_selection_verified": False,
                  "boot_verified": False, "recovery_verified": False, "flash_ready": False}
        (output / "binding.json").write_text(json.dumps(report, indent=2) + "\n")
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("base", type=Path)
    parser.add_argument("overlay", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--base-sha256", required=True)
    parser.add_argument("--overlay-sha256", required=True)
    parser.add_argument("--panel", required=True)
    parser.add_argument("--bridge", required=True)
    parser.add_argument("--display", required=True)
    parser.add_argument("--expected-default-panel", required=True)
    args = parser.parse_args()
    try:
        print(json.dumps(build(args.base, args.overlay, args.output, args.base_sha256, args.overlay_sha256,
                               args.panel, args.bridge, args.display, args.expected_default_panel), indent=2))
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        parser.exit(1, "Cannot bind Pico overlay: " + str(error) + "\n")


if __name__ == "__main__":
    main()
