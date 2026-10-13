import hashlib
import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location('pico_optics', ROOT / 'tools/inspect-pico-optics.py')
OPT = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(OPT)


def fixture():
    lens = {'lensSeparation': .063, 'FOV': 95, 'metersPerTanAngleAtCenter': .04, 'distortionEqnType': 2}
    lens.update({f'distortionCoefficients{i}': i / 100 for i in range(20)})
    lens.update({f'chromaticAberration{i}': i / 1000 for i in range(4)})
    cylinder = {'iCylinderMeshX': 64, 'iCylinderMeshY': 64, 'fCylinderInPosFactor': .8,
                'fCylinderOutPosFactor': 1, 'fCylinderDistortionBoundThreshold': .5,
                'fCylinderDistortionBoundSetValue': 1}
    cylinder.update({f'fCylinderDistortionC10K{i}': i / 1000 for i in range(1, 11)})
    return ''.join('[' + section + ']\n' + ''.join(f'{key}={value}\n' for key, value in values.items())
                   for section, values in [('lens', lens), ('CylinderParameters', cylinder)]).encode()


class PicoOpticsTests(unittest.TestCase):
    def test_lens_metadata_preserves_order_without_assuming_a_polynomial(self):
        parsed = OPT.lens_parameters(fixture())
        self.assertEqual(parsed['distortion_coefficients'], [i / 100 for i in range(20)])
        self.assertEqual(parsed['chromatic_aberration'], [i / 1000 for i in range(4)])
        self.assertEqual(parsed['equation_type'], 2)
        self.assertEqual(parsed['cylinder']['iCylinderMeshX'], 64)

    def test_eye_and_non_eye_row_selection_are_distinct(self):
        rows = b'0.1,0.2,0.3,0.4\n-0.1,-0.2,-0.3,-0.4\n0,0,0,0\n'
        for state, index in ((1, 2), (2, 1), (3, 0)):
            parsed = OPT.axis_offsets(rows, 'neo3-pro', state)
            self.assertEqual(parsed['selected_row'], index)
            self.assertEqual(parsed['selected'], parsed['rows'][index])
            parsed = OPT.axis_offsets(rows.splitlines()[0], 'neo3-pro-eye', state)
            self.assertEqual(parsed['selected_row'], 0)
            self.assertFalse(parsed['property_corrections_applied'])
        for data, model in ((rows, 'neo3-pro-eye'), (b'0,0,0,0', 'neo3-pro')):
            with self.assertRaises(ValueError):
                OPT.axis_offsets(data, model, 2)

    def test_refuses_malformed_or_ambiguous_calibration(self):
        good = fixture()
        for data in [good + b'\0', good.replace(b'FOV=95', b'FOV=95\nFOV=90'),
                     good.replace(b'FOV=95', b'FOV=nan'), good.replace(b'FOV=95', b'FOV=1e999'),
                     good.replace(b'FOV=95', b'FOV=180'), good.replace(b'FOV=95', b'FOV=%(foo)s'),
                     good.replace(b'FOV=95', b'unknown=95'),
                     good.replace(b'distortionEqnType=2', b'distortionEqnType=3'),
                     good.replace(b'iCylinderMeshX=64', b'iCylinderMeshX=2.5'),
                     b'[DEFAULT]\nFOV=95\n' + good, good + b'[extra]\nx=1\n', b' ' * 65537]:
            with self.subTest(data=data[:60]), self.assertRaises(ValueError):
                OPT.lens_parameters(data)
        for data in (b'0,0,0', b'0,0,0,0,0', b'nan,0,0,0', b'0,0,0,1.1', b'0,0,0,0\0',
                     b'0,0,0,0\n\n', b'0'*100+b',0,0,0'):
            with self.subTest(data=data), self.assertRaises(ValueError):
                OPT.axis_offsets(data, 'neo3-pro-eye', 2)

    def test_hash_binding_private_output_and_no_overwrite(self):
        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp)
            config = directory / 'A7H10_0.ini'; config.write_bytes(fixture())
            offsets = directory / 'axisOffset.txt'; offsets.write_bytes(b'0.1,-0.2,0.3,-0.4')
            output = directory / 'metadata.json'
            digest = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
            result = OPT.inspect(config, digest(config), offsets, digest(offsets), 'neo3-pro-eye', 2)
            self.assertFalse(result['runtime_usable'])
            self.assertFalse(result['calibration_applied'])
            self.assertEqual(result['stock_axis_coefficient_indices'], [14, 15, 16, 17])
            command = [sys.executable, '-B', str(ROOT/'tools/inspect-pico-optics.py'), str(config),
                       str(offsets), '--config-sha256', digest(config), '--offsets-sha256', digest(offsets),
                       '--model', 'neo3-pro-eye', '--ipd-state', '2', '--output', str(output)]
            subprocess.run(command, check=True, capture_output=True, timeout=5)
            self.assertEqual(output.stat().st_mode & 0o777, 0o600)
            before = output.read_bytes()
            self.assertNotEqual(subprocess.run(command, capture_output=True, timeout=5).returncode, 0)
            self.assertEqual(output.read_bytes(), before)
            with self.assertRaises(ValueError):
                OPT.inspect(config, '0'*64, offsets, digest(offsets), 'neo3-pro-eye', 2)
            link = directory/'link'; link.symlink_to(offsets)
            with self.assertRaises(OSError):
                OPT.read_file(link, digest(offsets), 1024)
            with self.assertRaises(ValueError):
                OPT.read_file(directory, '0'*64, 1024)
