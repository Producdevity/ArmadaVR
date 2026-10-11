import copy
import hashlib
import importlib.util
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
SPEC = importlib.util.spec_from_file_location("pico_panel_binding", ROOT / "tools/bind-pico-panel.py")
binding = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(binding)


def cells(*values):
    return struct.pack(f">{len(values)}I", *values)


def fixture():
    nodes = {
        "/": {}, "/soc": {},
        "/soc/panel": {"qcom,mdss-dsi-panel-name": b"Test panel\0", "phandle": cells(2)},
        "/soc/gpio": {"gpio-controller": b"", "#gpio-cells": cells(2), "phandle": cells(1), "ngpios": cells(20)},
        "/soc/bridge": {"compatible": b"qcom,vxr7200\0"},
    }
    for pin, name in enumerate(binding.GPIO_PROPERTIES):
        nodes["/soc/bridge"][name] = cells(1, pin, 0)
    return nodes


class PicoPanelBindingTests(unittest.TestCase):
    def test_new_and_existing_handle(self):
        nodes = fixture()
        original = copy.deepcopy(nodes)
        result = binding.binding(nodes, "/soc/panel", "/soc/bridge")
        self.assertEqual(nodes, original)
        self.assertEqual(result["bridge_phandle"], 3)
        self.assertTrue(result["create_bridge_phandle"])
        self.assertEqual(len(result["gpios"]), 5)
        for alias in ("phandle", "linux,phandle"):
            with self.subTest(alias=alias):
                nodes = fixture()
                nodes["/soc/bridge"][alias] = cells(7)
                result = binding.binding(nodes, "/soc/panel", "/soc/bridge")
                self.assertEqual(result["bridge_phandle"], 7)
                self.assertFalse(result["create_bridge_phandle"])

    def test_disabled_nodes_and_ancestors(self):
        for path in fixture():
            with self.subTest(path=path):
                nodes = fixture()
                nodes[path]["status"] = b"disabled\0"
                with self.assertRaisesRegex(ValueError, "Disabled"):
                    binding.binding(nodes, "/soc/panel", "/soc/bridge")

    def test_bad_handles(self):
        for values in ({"phandle": cells(1)}, {"phandle": cells(0)}, {"phandle": cells(0xffffffff)},
                       {"phandle": b"x"}, {"phandle": cells(3), "linux,phandle": cells(4)}):
            with self.subTest(values=values):
                nodes = fixture()
                nodes["/soc/bridge"].update(values)
                with self.assertRaises(ValueError):
                    binding.binding(nodes, "/soc/panel", "/soc/bridge")
        nodes = fixture()
        nodes["/unrelated"] = {"phandle": cells(0xfffffffe)}
        with self.assertRaisesRegex(ValueError, "No free"):
            binding.binding(nodes, "/soc/panel", "/soc/bridge")

    def test_bad_gpio_specifications(self):
        for name in binding.GPIO_PROPERTIES:
            for value in (b"", cells(1, 1), cells(1, 1, 0, 1, 2, 0), cells(88, 1, 0),
                          cells(1, 1, 1), cells(1, 20, 0), cells(1, 0xffffffff, 0)):
                with self.subTest(name=name, value=value):
                    nodes = fixture()
                    nodes["/soc/bridge"][name] = value
                    with self.assertRaises(ValueError):
                        binding.binding(nodes, "/soc/panel", "/soc/bridge")
        for property_name, value in (("gpio-controller", None), ("#gpio-cells", cells(3)),
                                     ("#gpio-cells", b"")):
            nodes = fixture()
            if value is None:
                del nodes["/soc/gpio"][property_name]
            else:
                nodes["/soc/gpio"][property_name] = value
            with self.assertRaises(ValueError):
                binding.binding(nodes, "/soc/panel", "/soc/bridge")

    def test_duplicate_pins(self):
        for left in range(5):
            for right in range(left):
                nodes = fixture()
                nodes["/soc/bridge"][binding.GPIO_PROPERTIES[left]] = cells(1, right, 0)
                with self.assertRaisesRegex(ValueError, "share a GPIO"):
                    binding.binding(nodes, "/soc/panel", "/soc/bridge")

    def test_wrong_or_already_bound_nodes(self):
        for path, name, value in (("/soc/panel", "qcom,mdss-dsi-panel-name", b""),
                                  ("/soc/panel", "qcom,mdss-dsi-panel-name", b"\0"),
                                  ("/soc/panel", binding.PROPERTY, cells(3)),
                                  ("/soc/bridge", "compatible", b"unrelated\0")):
            nodes = fixture()
            nodes[path][name] = value
            with self.assertRaises(ValueError):
                binding.binding(nodes, "/soc/panel", "/soc/bridge")
        with self.assertRaises(ValueError):
            binding.binding(fixture(), "/soc/missing", "/soc/bridge")

    @unittest.skipUnless(shutil.which("dtc") and shutil.which("fdtput"), "dtc tools required")
    def test_offline_assembly_preserves_tree_and_source(self):
        gpios = "\n".join(f"{name} = <1 {pin} 0>;" for pin, name in enumerate(binding.GPIO_PROPERTIES))
        dts = '''/dts-v1/;
        /memreserve/ 0x1000 0x1000;
        / { #address-cells = <2>; #size-cells = <2>;
            memory@40000000 { device_type = "memory"; reg = <0 0x40000000 0 0x10000000>; };
            soc {
                panel { qcom,mdss-dsi-panel-name = "Test panel"; phandle = <2>; };
                gpio { gpio-controller; #gpio-cells = <2>; phandle = <1>; ngpios = <20>; };
                bridge { compatible = "qcom,vxr7200"; ''' + gpios + ''' };
            };
        };'''
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            source = root / "original.dtb"
            subprocess.run(["dtc", "-I", "dts", "-O", "dtb", "-o", str(source)],
                           input=dts, text=True, capture_output=True, check=True, timeout=10)
            original = source.read_bytes()
            digest = hashlib.sha256(original).hexdigest()
            with self.assertRaisesRegex(ValueError, "SHA-256"):
                binding.build(source, root / "wrong", "0" * 64, "/soc/panel", "/soc/bridge")
            self.assertFalse((root / "wrong").exists())
            report = binding.build(source, root / "bound", digest, "/soc/panel", "/soc/bridge")
            self.assertEqual(source.read_bytes(), original)
            self.assertFalse(report["flash_ready"])
            self.assertFalse(report["live_selection_verified"])
            self.assertEqual(len(report["property_changes"]), 2)
            nodes = binding.read_fdt((root / "bound/panel-mux.dtb").read_bytes())
            self.assertEqual(nodes["/soc/panel"][binding.PROPERTY], cells(3))
            self.assertEqual(nodes["/soc/bridge"]["phandle"], cells(3))
            with self.assertRaises(FileExistsError):
                binding.build(source, root / "bound", digest, "/soc/panel", "/soc/bridge")


if __name__ == "__main__":
    unittest.main()
