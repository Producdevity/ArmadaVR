import importlib.util
from pathlib import Path
import struct
import sys
import unittest

sys.path.insert(0, str(Path(__file__).parents[1] / "tools"))
spec = importlib.util.spec_from_file_location("thermal", Path(__file__).parents[1] / "tools/inspect-thermal.py")
thermal = importlib.util.module_from_spec(spec)
spec.loader.exec_module(thermal)


def cells(*values):
    return struct.pack(f">{len(values)}I", *values)


class ThermalTests(unittest.TestCase):
    def fixture(self):
        return {
            "/": {},
            "/sensor": {"phandle": cells(1), "#thermal-sensor-cells": cells(0)},
            "/fan": {"phandle": cells(2), "#cooling-cells": cells(2), "cooling-levels": cells(0, 3700, 5500)},
            "/thermal-zones/battery": {"thermal-sensors": cells(1), "polling-delay": cells(1000)},
            "/thermal-zones/battery/trips/warm": {"phandle": cells(3), "temperature": cells(45000),
                                                   "hysteresis": cells(2000), "type": b"active\0"},
            "/thermal-zones/battery/cooling-maps/fan": {"trip": cells(3), "cooling-device": cells(2, 1, 2)},
        }

    def test_resolves_actual_cooling_levels_without_claiming_hardware_proof(self):
        report = thermal.audit(self.fixture())
        self.assertFalse(report["physical_control_tested"])
        zone = report["zones"][0]
        self.assertEqual(zone["cooling_maps"][0]["devices"][0]["levels"], [0, 3700, 5500])
        self.assertEqual(zone["trips"]["/thermal-zones/battery/trips/warm"]["temperature_millicelsius"], 45000)

    def test_rejects_unresolved_trip_and_out_of_range_fan_state(self):
        for field, value, message in (("trip", cells(99), "missing or foreign trip"),
                                      ("cooling-device", cells(2, 1, 3), "exceeds available levels"),
                                      ("cooling-device", cells(2, 2, 1), "Invalid cooling state bounds")):
            with self.subTest(field=field, value=value):
                nodes = self.fixture()
                nodes["/thermal-zones/battery/cooling-maps/fan"][field] = value
                with self.assertRaisesRegex(ValueError, message):
                    thermal.audit(nodes)

    def test_rejects_disabled_or_missing_sensor_and_truncated_reference(self):
        for failure in ("disabled", "missing", "truncated"):
            with self.subTest(failure=failure):
                nodes = self.fixture()
                if failure == "disabled":
                    nodes["/sensor"]["status"] = b"disabled\0"
                elif failure == "missing":
                    del nodes["/sensor"]
                else:
                    nodes["/sensor"]["#thermal-sensor-cells"] = cells(1)
                with self.assertRaises(ValueError):
                    thermal.audit(nodes)

    def test_shutdown_profile_requires_real_critical_trip_and_polling(self):
        nodes = self.fixture()
        profile = {"zones": [{"name": "battery", "temperature_millicelsius": 65000,
                              "hysteresis_millicelsius": 1000}]}
        with self.assertRaisesRegex(ValueError, "Missing stock shutdown limit"):
            thermal.check_shutdown_limits(thermal.audit(nodes), profile)
        nodes["/thermal-zones/battery/trips/shutdown"] = {
            "temperature": cells(65000), "hysteresis": cells(1000), "type": b"critical\0"}
        report = thermal.audit(nodes)
        thermal.check_shutdown_limits(report, profile)
        self.assertEqual(report["stock_shutdown_limits_matched"], 1)
        nodes["/thermal-zones/battery"]["polling-delay"] = cells(0)
        with self.assertRaisesRegex(ValueError, "no polling fallback"):
            thermal.check_shutdown_limits(thermal.audit(nodes), profile)
