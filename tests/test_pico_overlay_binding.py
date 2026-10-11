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
spec = importlib.util.spec_from_file_location("pico_overlay", ROOT / "tools/bind-pico-overlay.py")
overlay = importlib.util.module_from_spec(spec)
spec.loader.exec_module(overlay)


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


@unittest.skipUnless(all(shutil.which(n) for n in ("dtc", "fdtoverlay", "fdtput")), "dtc tools required")
class PicoOverlayTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.base, self.source = self.root / "base.dtb", self.root / "source.dtbo"

    def tearDown(self):
        self.temp.cleanup()

    def compile(self, path, text):
        subprocess.run(["dtc", "-@", "-I", "dts", "-O", "dtb", "-o", str(path)],
                       input=text, text=True, capture_output=True, check=True, timeout=10)

    def fixture(self, handle=100):
        self.compile(self.base, '''/dts-v1/; /memreserve/ 0x1000 0x1000;
            / { #address-cells = <2>; #size-cells = <2>;
                memory@40000000 { device_type = "memory"; reg = <0 0x40000000 0 0x10000000>; };
                soc: soc { mdp: mdp { compatible = "qcom,sde-kms"; phandle = <''' + str(handle) + '''>; };
                    gpio: gpio { gpio-controller; #gpio-cells = <2>; ngpios = <20>; };
                    i2c: i2c {};
                };
            };''')
        gpios = "\n".join(f"{name} = <&gpio {pin} 0>;" for pin, name in enumerate(overlay.panel_binding.GPIO_PROPERTIES))
        self.compile(self.source, '''/dts-v1/; /plugin/;
            &mdp { selected: panel { qcom,mdss-dsi-panel-name = "Selected panel"; };
                   previous: previous { qcom,mdss-dsi-panel-name = "Previous panel"; }; };
            &i2c { bridge { compatible = "qcom,vxr7200"; ''' + gpios + ''' }; };
            &soc { display { compatible = "qcom,dsi-display"; label = "primary";
                   qcom,mdp = <&mdp>; qcom,dsi-default-panel = <&previous>; }; };''')

    def build(self, name="output", base_hash=None):
        return overlay.build(self.base, self.source, self.root / name, base_hash or sha(self.base), sha(self.source),
                             "/soc/mdp/panel", "/soc/i2c/bridge", "/soc/display", "/soc/mdp/previous")

    def test_actual_relocation_and_merge_at_different_base_phandles(self):
        for handle in (16, 100, 0x10000):
            with self.subTest(handle=handle):
                self.fixture(handle)
                before = (self.base.read_bytes(), self.source.read_bytes())
                name = "bound-" + str(handle)
                result = self.build(name)
                self.assertEqual((self.base.read_bytes(), self.source.read_bytes()), before)
                self.assertTrue(result["merged_tree_matches_binding"])
                self.assertFalse(result["flash_ready"])
                self.assertFalse(result["bootloader_selection_verified"])
                local = overlay.read_fdt((self.root / name / "panel-mux.dtbo").read_bytes())
                merged = overlay.read_fdt((self.root / name / "panel-mux.dtb").read_bytes())
                self.assertEqual(merged["/soc/display"]["qcom,dsi-default-panel"], merged["/soc/mdp/panel"]["phandle"])
                self.assertEqual(merged["/soc/mdp/panel"][overlay.panel_binding.PROPERTY], merged["/soc/i2c/bridge"]["phandle"])
                self.assertNotEqual(struct.unpack(">I", merged["/soc/i2c/bridge"]["phandle"])[0], result["local_bridge_phandle"])
                path = "/__local_fixups__" + result["overlay_nodes"]["panel"]
                self.assertEqual(local[path][overlay.panel_binding.PROPERTY], bytes(4))
                with self.assertRaisesRegex(ValueError, "new overlay"):
                    self.build(name)

    def test_wrong_source_hash_or_missing_fixup_cannot_create_output(self):
        self.fixture()
        with self.assertRaisesRegex(ValueError, "SHA-256"):
            self.build(base_hash="0" * 64)
        self.assertFalse((self.root / "output").exists())
        n = overlay.read_fdt(self.source.read_bytes())
        p = overlay.overlay_node(overlay.read_fdt(self.base.read_bytes()), n, "/soc/display")
        subprocess.run(["fdtput", "-t", "x", str(self.source), "/__local_fixups__" + p,
                        "qcom,dsi-default-panel", "4"], capture_output=True, check=True, timeout=10)
        with self.assertRaisesRegex(ValueError, "one local relocation"):
            self.build()
        self.assertFalse((self.root / "output").exists())

    def test_duplicate_fragment_matches_and_missing_symbols_are_rejected(self):
        self.fixture()
        base = overlay.read_fdt(self.base.read_bytes())
        nodes = overlay.read_fdt(self.source.read_bytes())
        path = overlay.overlay_node(base, nodes, "/soc/mdp/panel")
        with self.assertRaisesRegex(ValueError, "exactly one"):
            overlay.overlay_node(base, nodes, "/soc/mdp/missing")
        nodes["/duplicate"] = {"target-path": b"/soc/mdp\0"}
        nodes["/duplicate/__overlay__"] = {}
        nodes["/duplicate/__overlay__/panel"] = dict(nodes[path])
        with self.assertRaisesRegex(ValueError, "exactly one"):
            overlay.overlay_node(base, nodes, "/soc/mdp/panel")
        del nodes["/duplicate"]
        base["/__symbols__"] = {}
        with self.assertRaisesRegex(ValueError, "unique base target"):
            overlay.overlay_node(base, nodes, "/soc/mdp/panel")

    def test_symlink_input_is_rejected(self):
        self.fixture()
        h = sha(self.base)
        self.base.rename(self.root / "original")
        self.base.symlink_to("original")
        with self.assertRaisesRegex(ValueError, "regular DTB"):
            self.build(base_hash=h)
