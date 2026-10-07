#!/usr/bin/env python3
"""Audit thermal references in a merged headset DTB without accessing hardware."""
import argparse
import hashlib
import json
from pathlib import Path
import struct

from devicetree import read_fdt, string_list


def cells(value):
    if len(value) % 4:
        raise ValueError("Malformed thermal cell array")
    return list(struct.unpack(f">{len(value) // 4}I", value))


def audit(nodes):
    handles = {}
    for path, props in nodes.items():
        if "phandle" in props:
            values = cells(props["phandle"])
            if len(values) != 1 or values[0] in handles or values[0] in (0, 0xffffffff):
                raise ValueError(f"Invalid or duplicate phandle: {path}")
            handles[values[0]] = path

    def enabled(path):
        return all(nodes.get(str(parent), {}).get("status", b"okay\0") in (b"okay\0", b"ok\0")
                   for parent in (Path(path), *Path(path).parents))

    def references(value, count_property):
        values = cells(value)
        while values:
            handle = values.pop(0)
            if handle not in handles:
                raise ValueError(f"Unresolved thermal phandle: {handle}")
            target = handles[handle]
            count = cells(nodes[target].get(count_property, b""))
            if len(count) != 1 or count[0] > len(values):
                raise ValueError(f"Invalid {count_property} at {target}")
            args, values = values[:count[0]], values[count[0]:]
            if not enabled(target):
                raise ValueError(f"Active thermal zone references disabled provider: {target}")
            yield target, args

    zones = []
    for path, props in nodes.items():
        if "thermal-sensors" not in props or not enabled(path):
            continue
        sensors = [{"path": target, "arguments": args}
                   for target, args in references(props["thermal-sensors"], "#thermal-sensor-cells")]
        if not sensors:
            raise ValueError(f"Empty thermal sensor reference: {path}")
        trips = {}
        for child, values in nodes.items():
            if str(Path(child).parent) != path + "/trips":
                continue
            temperature = cells(values.get("temperature", b""))
            hysteresis = cells(values.get("hysteresis", b""))
            kind = string_list(values.get("type", b""))
            if len(temperature) != 1 or len(hysteresis) != 1 or kind not in (["active"], ["passive"], ["hot"], ["critical"]):
                raise ValueError(f"Incomplete thermal trip: {child}")
            trips[child] = {"temperature_millicelsius": struct.unpack(">i", values["temperature"])[0],
                            "hysteresis_millicelsius": hysteresis[0], "type": kind[0]}
        maps = []
        for child, values in nodes.items():
            if str(Path(child).parent) != path + "/cooling-maps":
                continue
            trip = cells(values.get("trip", b""))
            if len(trip) != 1 or handles.get(trip[0]) not in trips:
                raise ValueError(f"Cooling map references a missing or foreign trip: {child}")
            devices = []
            for target, limits in references(values.get("cooling-device", b""), "#cooling-cells"):
                if len(limits) != 2 or (0xffffffff not in limits and limits[0] > limits[1]):
                    raise ValueError(f"Invalid cooling state bounds: {child}")
                levels = cells(nodes[target].get("cooling-levels", b""))
                if levels and any(limit != 0xffffffff and limit >= len(levels) for limit in limits):
                    raise ValueError(f"Cooling state exceeds available levels: {child}")
                devices.append({"path": target, "states": limits, "levels": levels})
            if not devices:
                raise ValueError(f"Cooling map has no device: {child}")
            maps.append({"path": child, "trip": handles[trip[0]], "devices": devices})
        polling = cells(props.get("polling-delay", b"\0\0\0\0"))
        if len(polling) != 1:
            raise ValueError(f"Invalid thermal polling interval: {path}")
        zones.append({"path": path, "sensors": sensors, "trips": trips, "cooling_maps": maps,
                      "polling_milliseconds": polling[0]})
    if not zones:
        raise ValueError("DTB has no active thermal zones")
    return {"status": "references-valid", "physical_control_tested": False, "zones": zones}


def check_shutdown_limits(report, profile):
    for limit in profile["zones"]:
        matches = [zone for zone in report["zones"] if Path(zone["path"]).name == limit["name"]]
        if len(matches) != 1:
            raise ValueError(f"Missing or ambiguous shutdown sensor: {limit['name']}")
        zone = matches[0]
        if not zone["polling_milliseconds"]:
            raise ValueError(f"Shutdown sensor has no polling fallback: {limit['name']}")
        if not any(trip["type"] == "critical" and
                   trip["temperature_millicelsius"] == limit["temperature_millicelsius"] and
                   trip["hysteresis_millicelsius"] == limit["hysteresis_millicelsius"]
                   for trip in zone["trips"].values()):
            raise ValueError(f"Missing stock shutdown limit: {limit['name']}")
    report["stock_shutdown_limits_matched"] = len(profile["zones"])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("dtb", type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--shutdown-profile", type=Path, help="Require the stock shutdown trips and polling fallback")
    args = parser.parse_args()
    try:
        data = args.dtb.read_bytes()
        report = audit(read_fdt(data))
        if args.shutdown_profile:
            profile_data = args.shutdown_profile.read_bytes()
            check_shutdown_limits(report, json.loads(profile_data))
            report["shutdown_profile_sha256"] = hashlib.sha256(profile_data).hexdigest()
        report.update({"dtb": str(args.dtb), "sha256": hashlib.sha256(data).hexdigest()})
        args.output.parent.mkdir(parents=True, exist_ok=True)
        with args.output.open("x") as stream:
            json.dump(report, stream, indent=2)
            stream.write("\n")
        print(f"Validated {len(report['zones'])} thermal-zone reference graphs; physical control remains untested")
    except (OSError, ValueError) as error:
        parser.exit(1, f"{error}\n")


if __name__ == "__main__":
    main()
