import hashlib
import importlib.util
import json
import math
import copy
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


def screen_fixture():
    return {'schema_version': 1, 'layout': 'side-by-side', 'width_pixels': 3200,
            'height_pixels': 1600, 'width_m': .16, 'lens_separation_m': .08,
            'horizontal_offset_m': 0, 'meters_per_tan_angle': .04,
            'optical_offsets_mm': [0, 0, 0, 0], 'hwc_reverse': 0,
            'separate_distortion': False, 'fov': [[-math.pi/4, math.pi/4, math.pi/4, -math.pi/4]] * 2}


class PicoOpticsTests(unittest.TestCase):
    def test_lens_metadata_preserves_order_without_assuming_a_polynomial(self):
        parsed = OPT.lens_parameters(fixture())
        self.assertEqual(parsed['distortion_coefficients'], [i / 100 for i in range(20)])
        self.assertEqual(parsed['chromatic_aberration'], [i / 1000 for i in range(4)])
        self.assertEqual(parsed['equation_type'], 2)
        self.assertEqual(parsed['cylinder']['iCylinderMeshX'], 64)

    def test_radial_basis_and_chromatic_record_mapping(self):
        lens = OPT.lens_parameters(fixture())
        lens['distortion_coefficients'] = [1] + [0] * 19
        lens['chromatic_aberration'] = [0] * 4
        self.assertEqual(OPT.sample_tangents(lens, 3, 4),
                         {'input': [3, 4], 'red': [3, 4], 'green': [3, 4], 'blue': [3, 4]})
        for index in range(11):
            coefficients = [int(i == index) for i in range(11)]
            self.assertEqual(OPT.radial_scale(coefficients, 2), 2 ** index)
        lens['chromatic_aberration'] = [.1, .2, .3, .4]
        lens['distortion_coefficients'][18:20] = [.5, .6]
        result = OPT.sample_tangents(lens, 3, 4)
        for channel, expected in [('red', [25.8, 34.4]), ('green', [3, 4]), ('blue', [42.9, 57.2])]:
            for a, b in zip(result[channel], expected):
                self.assertAlmostEqual(a, b)
        lens['distortion_coefficients'][11:18] = [999] * 7
        self.assertEqual(OPT.sample_tangents(lens, 3, 4), result)
        self.assertEqual(OPT.sample_tangents(lens, 0, 0)['red'], [0, 0])

    def test_equation_rejects_nonfinite_and_overflow(self):
        lens = OPT.lens_parameters(fixture())
        for value in [float('nan'), float('inf'), -float('inf'), 1e308]:
            with self.subTest(value=value), self.assertRaises(ValueError):
                OPT.sample_tangents(lens, value, 1)
        for coefficients, radius in [([1] * 10, 1), ([1] * 12, 1), ([1] * 11, -1),
                                      ([float('nan')] * 11, 1), ([1e308] * 11, 2)]:
            with self.subTest(radius=radius), self.assertRaises(ValueError):
                OPT.radial_scale(coefficients, radius)

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
            command += ['--sample-tangent', '0.1', '-0.2']
            subprocess.run(command, check=True, capture_output=True, timeout=5)
            metadata = json.loads(output.read_text())
            self.assertEqual(metadata['tangent_samples'], [OPT.sample_tangents(result['stock_lens'], .1, -.2)])
            self.assertFalse(metadata['runtime_usable'])
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


class PicoScreenTests(unittest.TestCase):
    def setUp(self):
        self.lens = OPT.lens_parameters(fixture())
        self.lens['distortion_coefficients'] = [1] + [0] * 19
        self.lens['chromatic_aberration'] = [0] * 4
        self.screen = OPT.screen_parameters(json.dumps(screen_fixture()).encode())

    def test_screen_texture_axes_and_hwc_reversal(self):
        for reverse in (0, 180):
            self.screen['hwc_reverse'] = reverse
            for eye in (0, 1):
                for u, v in [(0, 0), (1, 0), (0, 1), (1, 1), (.2, .7), (.5, .5)]:
                    sample = OPT.screen_tangents(self.lens, self.screen, eye, u, v)
                    uv = OPT.project_tangents(sample, self.screen['fov'][eye])
                    expected = (1-u, 1-v) if reverse else (u, v)
                    for channel in ('red', 'green', 'blue'):
                        for a, b in zip(uv[channel], expected):
                            self.assertAlmostEqual(a, b)

    def test_analytic_affine_matches_geometry_without_grid_interpolation(self):
        self.screen['optical_offsets_mm'] = [.2, -.3, -.4, .1]
        self.screen['horizontal_offset_m'] = .0005
        for reverse in (0, 180):
            self.screen['hwc_reverse'] = reverse
            for eye in (0, 1):
                model = OPT.analytic_parameters(self.lens, self.screen, eye)
                self.assertEqual(model['radial_coefficients'], self.lens['distortion_coefficients'][:11])
                self.assertEqual(model['chromatic_coefficients'], self.lens['chromatic_aberration'] + self.lens['distortion_coefficients'][18:20])
                for u, v in [(0, 0), (.17, .68), (1, 1)]:
                    expected = OPT.screen_tangents(self.lens, self.screen, eye, u, v)['input']
                    for i, coordinate in enumerate((u, v)):
                        self.assertAlmostEqual(model['tangent_offset'][i] + coordinate * model['tangent_scale'][i], expected[i])

    def test_physical_offsets_aspect_and_distinct_eye_fovs(self):
        self.screen.update(lens_separation_m=.084, horizontal_offset_m=.0005,
                           optical_offsets_mm=[.2, -.3, -.4, .1], height_pixels=800)
        left = OPT.screen_tangents(self.lens, self.screen, 0, .5, .5)
        right = OPT.screen_tangents(self.lens, self.screen, 1, .5, .5)
        for actual, wanted in zip(left['input'], [.0675, .0075]):
            self.assertAlmostEqual(actual, wanted)
        for actual, wanted in zip(right['input'], [-.0475, -.0025]):
            self.assertAlmostEqual(actual, wanted)
        upper = OPT.screen_tangents(self.lens, self.screen, 0, .5, 0)
        self.assertAlmostEqual(upper['input'][1], .5075)
        self.screen['hwc_reverse'] = 180
        reversed_sample = OPT.screen_tangents(self.lens, self.screen, 0, .5, .5)
        for a, b in zip(reversed_sample['input'], left['input']):
            self.assertAlmostEqual(a, -b)
        fov = [-.45, .8, .6, -.4]
        for x, y, expected in [(math.tan(fov[0]), math.tan(fov[2]), [0, 0]),
                                (math.tan(fov[1]), math.tan(fov[3]), [1, 1])]:
            sample = {channel: [x, y] for channel in ('red', 'green', 'blue')}
            self.assertEqual(OPT.project_tangents(sample, fov)['green'], expected)

    def test_grid_keeps_out_of_texture_samples_and_proof_limits(self):
        self.lens['distortion_coefficients'][1] = .3
        self.lens['chromatic_aberration'][0] = .1
        self.screen['fov'][1] = [-.65, .85, .6, -.7]
        result = OPT.screen_grid(self.lens, self.screen, 3, 5)
        self.assertFalse(result['runtime_usable'])
        self.assertFalse(result['calibration_applied'])
        self.assertFalse(result['mesh_clipping_applied'])
        for eye, view in enumerate(result['views']):
            self.assertEqual(len(view['distortion']), 15)
            self.assertEqual(view['distortion'][7],
                             sum(OPT.project_tangents(OPT.screen_tangents(self.lens, self.screen, eye, .5, .5),
                                                      self.screen['fov'][eye]).values(), []))
            self.assertLess(view['distortion'][0][0], 0)
            self.assertLess(view['in_texture_vertices']['green'], 15)
        self.assertNotEqual(result['views'][0]['distortion'], result['views'][1]['distortion'])
        self.lens['distortion_coefficients'][0] = 100
        self.assertFalse(OPT.screen_grid(self.lens, self.screen, 2, 2)['views'][0]['within_driver_coordinate_limit'])

    def test_screen_model_refuses_unsupported_or_ambiguous_inputs(self):
        changes = [('schema_version', True), ('layout', 'stacked'), ('separate_distortion', True),
                   ('separate_distortion', 0), ('width_pixels', 3201), ('height_pixels', 0),
                   ('width_pixels', True), ('width_m', 0), ('width_m', float('nan')),
                   ('meters_per_tan_angle', 1e-300), ('lens_separation_m', '0.06'),
                   ('horizontal_offset_m', .2), ('hwc_reverse', 90), ('hwc_reverse', False),
                   ('optical_offsets_mm', [0, 0, 0]), ('optical_offsets_mm', [0, 0, 0, 2]),
                   ('optical_offsets_mm', [0, 0, 0, True]), ('fov', []),
                   ('fov', [[-.5, .5, 0, -.5]]*2), ('fov', [[-.5, .5, .5, float('inf')]]*2)]
        for key, value in changes:
            model = copy.deepcopy(self.screen); model[key] = value
            with self.subTest(key=key, value=value), self.assertRaises(ValueError):
                OPT.screen_parameters(json.dumps(model).encode())
        for data in [b'{}', b'[]', b'null', b'x', b' '*16385, b'{}\0',
                     json.dumps(self.screen).replace('"schema_version": 1', '"schema_version": 1, "schema_version": 1').encode()]:
            with self.subTest(data=data[:40]), self.assertRaises(ValueError):
                OPT.screen_parameters(data)
        for size in [(1, 2), (2, 66), (2.5, 3), (True, 2)]:
            with self.assertRaises(ValueError):
                OPT.screen_grid(self.lens, self.screen, *size)
        for eye, u, v in [(2, 0, 0), (True, 0, 0), (0, -.1, .5), (0, .5, float('nan'))]:
            with self.assertRaises(ValueError):
                OPT.screen_tangents(self.lens, self.screen, eye, u, v)

    def test_cli_requires_hash_bound_explicit_screen_and_preserves_output(self):
        with tempfile.TemporaryDirectory() as temp:
            d = Path(temp)
            config = d/'A7H10_0.ini'; config.write_bytes(fixture())
            offsets = d/'axisOffset.txt'; offsets.write_text('0,0,0,0')
            screen = d/'screen.json'; screen.write_text(json.dumps(self.screen))
            output = d/'grid.json'
            digest = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
            command = [sys.executable, '-B', str(ROOT/'tools/inspect-pico-optics.py'), str(config), str(offsets),
                       '--config-sha256', digest(config), '--offsets-sha256', digest(offsets),
                       '--model', 'neo3-pro-eye', '--ipd-state', '2', '--output', str(output)]
            extra = ['--screen-model', str(screen), '--screen-sha256', digest(screen), '--grid-size', '3', '5']
            for args in [extra[:2], extra[2:4], extra[4:], extra[:3]+['0'*64]+extra[4:],
                         ['--render-size', '256', '256'], extra+['--max-error-pixels', '1']]:
                self.assertNotEqual(subprocess.run(command+args, capture_output=True, timeout=5).returncode, 0)
                self.assertFalse(output.exists())
            extra += ['--render-size', '256', '256', '--max-error-pixels', '1']
            subprocess.run(command+extra, capture_output=True, timeout=5, check=True)
            result = json.loads(output.read_text())
            self.assertEqual(result['files']['screen_model']['sha256'], digest(screen))
            self.assertEqual(result['grid_audit']['render_size'], [256, 256])
            self.assertFalse(result['grid_audit']['continuous_error_bound_verified'])
            self.assertEqual(len(result['screen_grid']['views'][0]['distortion']), 15)
            self.assertFalse(result['runtime_usable'])
            self.assertEqual(output.stat().st_mode & 0o777, 0o600)
            before = output.read_bytes()
            self.assertNotEqual(subprocess.run(command+extra, capture_output=True, timeout=5).returncode, 0)
            self.assertEqual(output.read_bytes(), before)


class PicoGridAuditTests(unittest.TestCase):
    setUp = PicoScreenTests.setUp

    def test_affine_mapping_is_exact_at_interior_samples(self):
        grid = OPT.screen_grid(self.lens, self.screen, 3, 4)
        audit = OPT.audit_grid(self.lens, self.screen, grid, [1832, 1920], .01)
        self.assertTrue(audit['sampled_error_within_limit'])
        self.assertFalse(audit['continuous_error_bound_verified'])
        self.assertFalse(audit['physical_accuracy_verified'])
        for view in audit['views']:
            self.assertEqual(view['tested_channel_samples'], 6 * 9 * 3)
            self.assertEqual(view['visible_channel_samples'], view['tested_channel_samples'])
            self.assertLess(view['maximum_sampled_error_pixels'], 1e-10)

    def test_refinement_reduces_error_and_each_color_is_checked(self):
        self.lens['distortion_coefficients'][18] = .12
        errors = []
        for size in (3, 9, 33):
            grid = OPT.screen_grid(self.lens, self.screen, size, size)
            audit = OPT.audit_grid(self.lens, self.screen, grid, [1024, 768], .5)
            errors.append(audit['views'][0]['maximum_sampled_error_pixels'])
            self.assertEqual(audit['views'][0]['worst_sample']['channel'], 'red')
            self.assertEqual(audit['sampled_error_within_limit'], size == 33)
        self.assertGreater(errors[0], errors[1])
        self.assertGreater(errors[1], errors[2])

    def test_no_visible_samples_cannot_pass_and_limits_are_checked(self):
        self.screen['horizontal_offset_m'] = .1
        self.screen['fov'] = [[-.01, .01, .01, -.01]]*2
        grid = OPT.screen_grid(self.lens, self.screen, 3, 3)
        audit = OPT.audit_grid(self.lens, self.screen, grid, [128, 128], 1)
        self.assertFalse(audit['sampled_error_within_limit'])
        self.assertEqual(audit['views'][0]['visible_channel_samples'], 0)
        for size, threshold in [([0, 128], 1), ([128], 1), ([True, 128], 1), ([128, 128], 0),
                                 ([128, 128], float('nan')), ([128, 128], 17)]:
            with self.assertRaises(ValueError):
                OPT.audit_grid(self.lens, self.screen, grid, size, threshold)


class PicoRuntimeSettingsTests(unittest.TestCase):
    def setUp(self):
        self.lens = OPT.lens_parameters(fixture())
        self.settings = {'schema_version': 1, 'ipd_enabled': True, 'ipd_correction_enabled': True,
                         'ipd_correction_mm': .25, 'screen_gap_m': 0, 'axis_corrections_mm': [0, 0, 0, 0]}
        self.axes = OPT.axis_offsets(b'0,0,0,0', 'neo3-pro-eye', 2)

    def test_model_and_state_separation_with_explicit_gap(self):
        self.settings['screen_gap_m'] = .003
        # Binary32 results from the saved stock getLensInfo implementation.
        expected = ['0x1.bda512p-5', '0x1.eecbfcp-5', '0x1.0ff972p-4']
        for model in ('neo3-pro', 'neo3-pro-eye'):
            for enabled in (False, True):
                self.settings['ipd_enabled'] = enabled
                for state in (1, 2, 3):
                    result = OPT.resolve_runtime(self.lens, self.axes, self.settings, model, state)
                    value = expected[1] if model == 'neo3-pro-eye' else expected[state-1] if enabled else '0x1.eb852p-5'
                    self.assertEqual(result['lens']['lens_separation_m'], float.fromhex(value))
                    self.assertFalse(result['active_settings_verified'])
                    self.assertFalse(result['fov_override_resolved'])
                    self.assertFalse(result['ipd_lookup_verified'])
                    self.assertFalse(result['runtime_usable'])
                    self.assertFalse(result['calibration_applied'])

    def test_correction_enable_model_state_and_coefficient_ownership(self):
        original = copy.deepcopy(self.lens)
        original_axes = copy.deepcopy(self.axes)
        for model in ('neo3-pro', 'neo3-pro-eye'):
            for state in (1, 2, 3):
                for enabled in (False, True):
                    self.settings['ipd_correction_enabled'] = enabled
                    result = OPT.resolve_runtime(self.lens, self.axes, self.settings, model, state)
                    used = .25 if model == 'neo3-pro' and state != 1 and enabled else 0
                    self.assertEqual(result['ipd_correction_used_mm'], used)
                    self.assertEqual(result['optical_offsets_mm'], [-used, 0, used, 0])
                    self.assertEqual(result['lens']['distortion_coefficients'][14:18], [-used, 0, used, 0])
                    for i in list(range(14)) + [18, 19]:
                        self.assertEqual(result['lens']['distortion_coefficients'][i], original['distortion_coefficients'][i])
        self.assertEqual(self.lens, original)
        self.assertEqual(self.axes, original_axes)
        self.settings['axis_corrections_mm'][0] = 1
        self.assertEqual(result['settings']['axis_corrections_mm'], [0, 0, 0, 0])

    def test_stock_rounding_and_clamping_after_all_adjustments(self):
        self.axes['selected'] = [.37274909076837837, .6888879121751803,
                                 .6631746245604446, -.9295314547405724]
        self.settings.update(ipd_correction_mm=-.5, axis_corrections_mm=[-.25, 0, 2, 0])
        result = OPT.resolve_runtime(self.lens, self.axes, self.settings, 'neo3-pro', 2)
        self.assertEqual(result['optical_offsets_mm'],
                         [float.fromhex(v) for v in ['0x1.3ed8f8p-1', '0x1.60b5eap-1', '0x1p+0', '-0x1.dbeb8cp-1']])
        self.assertEqual(result['clamped_axes'], [2])
        self.assertGreater(result['before_clamping_mm'][2], 1)
        self.settings['axis_corrections_mm'] = [-2, -2, 2, 2]
        result = OPT.resolve_runtime(self.lens, self.axes, self.settings, 'neo3-pro', 2)
        self.assertEqual(result['optical_offsets_mm'], [-1, -1, 1, 1])
        self.assertEqual(result['clamped_axes'], [0, 1, 2, 3])

    def test_rejects_ambiguous_or_unbounded_settings(self):
        changes = [('schema_version', True), ('schema_version', 2), ('ipd_enabled', 1),
                   ('ipd_correction_enabled', None), ('ipd_correction_mm', '0'),
                   ('ipd_correction_mm', True), ('ipd_correction_mm', 11),
                   ('ipd_correction_mm', float('nan')), ('ipd_correction_mm', float('inf')),
                   ('screen_gap_m', -.001), ('screen_gap_m', .051), ('screen_gap_m', 10**400),
                   ('axis_corrections_mm', [0]*3), ('axis_corrections_mm', [0, 0, 0, False]),
                   ('axis_corrections_mm', [0, 0, 0, float('nan')]),
                   ('axis_corrections_mm', [0, 0, 0, 10**400])]
        for key, value in changes:
            settings = dict(self.settings, **{key: value})
            with self.subTest(key=key, value=value), self.assertRaises(ValueError):
                OPT.runtime_parameters(json.dumps(settings).encode())
        for key in self.settings:
            settings = dict(self.settings); del settings[key]
            with self.subTest(missing=key), self.assertRaises(ValueError):
                OPT.runtime_parameters(json.dumps(settings).encode())
        for raw in [b'{}', b'null', b'[]', b' ', b' '*4097, b'{}\0', b'\xff',
                    json.dumps(dict(self.settings, unknown=0)).encode(),
                    json.dumps(self.settings).replace('"schema_version": 1', '"schema_version": 1, "schema_version": 1').encode()]:
            with self.subTest(raw=raw[:40]), self.assertRaises(ValueError):
                OPT.runtime_parameters(raw)
        for model, state in [('unknown', 2), ('neo3-pro', 0), ('neo3-pro', True), ('neo3-pro', 2.0)]:
            with self.assertRaises(ValueError):
                OPT.resolve_runtime(self.lens, self.axes, self.settings, model, state)
        self.lens['lens_separation_m'] = .01
        self.settings.update(ipd_enabled=False, screen_gap_m=.02)
        with self.assertRaises(ValueError):
            OPT.resolve_runtime(self.lens, self.axes, self.settings, 'neo3-pro', 2)

    def test_cli_hash_binding_geometry_agreement_and_private_output(self):
        with tempfile.TemporaryDirectory() as temp:
            d = Path(temp)
            config = d/'A7H10_0.ini'; config.write_bytes(fixture())
            offsets = d/'axisOffset.txt'; offsets.write_text('0,0,0,0')
            settings = d/'runtime.json'; settings.write_text(json.dumps(self.settings))
            screen = d/'screen.json'
            output = d/'resolved.json'
            digest = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
            command = [sys.executable, '-B', str(ROOT/'tools/inspect-pico-optics.py'), str(config), str(offsets),
                       '--config-sha256', digest(config), '--offsets-sha256', digest(offsets),
                       '--model', 'neo3-pro-eye', '--ipd-state', '2', '--output', str(output)]
            extra = ['--runtime-settings', str(settings), '--runtime-sha256', digest(settings)]
            link = d/'link'; link.symlink_to(settings)
            for args in [extra[:2], extra[2:], extra[:3]+['0'*64],
                         ['--runtime-settings', str(link), '--runtime-sha256', digest(settings)]]:
                self.assertNotEqual(subprocess.run(command+args, capture_output=True, timeout=5).returncode, 0)
                self.assertFalse(output.exists())
            geometry = screen_fixture()
            for gap, offsets_value in [(.08, [0]*4), (.0634, [.1, 0, 0, 0])]:
                geometry.update(lens_separation_m=gap, optical_offsets_mm=offsets_value)
                screen.write_text(json.dumps(geometry))
                screen_args = ['--screen-model', str(screen), '--screen-sha256', digest(screen), '--grid-size', '3', '3']
                result = subprocess.run(command+extra+screen_args, capture_output=True, timeout=5)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(b'disagrees', result.stderr)
                self.assertFalse(output.exists())
            geometry['optical_offsets_mm'] = [0]*4
            screen.write_text(json.dumps(geometry))
            screen_args = ['--screen-model', str(screen), '--screen-sha256', digest(screen), '--grid-size', '3', '3']
            subprocess.run(command+extra+screen_args, check=True, capture_output=True, timeout=5)
            result = json.loads(output.read_text())
            self.assertEqual(result['files']['runtime_settings']['sha256'], digest(settings))
            self.assertEqual(result['runtime_adjustment']['lens']['distortion_coefficients'][14:18], [0]*4)
            self.assertEqual(result['screen_grid']['screen_model'], geometry)
            self.assertEqual(len(result['screen_grid']['views'][0]['distortion']), 9)
            self.assertFalse(result['runtime_usable'])
            self.assertEqual(output.stat().st_mode & 0o777, 0o600)
            before = output.read_bytes()
            self.assertNotEqual(subprocess.run(command+extra+screen_args, capture_output=True, timeout=5).returncode, 0)
            self.assertEqual(output.read_bytes(), before)


class PicoFovSettingsTests(unittest.TestCase):
    setUp = PicoRuntimeSettingsTests.setUp

    def test_fov_override_uses_stock_float_boundaries(self):
        self.settings.update(schema_version=2, fov_override_degrees=0)
        cases = [(0, 95, False), (60, 95, False), (60.000001, 95, False),
                 (60.000003, float.fromhex('0x1.e00002p+5'), True),
                 (90, 90, True), (159.99999, float.fromhex('0x1.3ffffep+7'), True),
                 (159.999999, 95, False), (160, 95, False), (180, 95, False)]
        for value, expected, applied in cases:
            self.settings['fov_override_degrees'] = value
            for model in ('neo3-pro', 'neo3-pro-eye'):
                with self.subTest(value=value, model=model):
                    result = OPT.resolve_runtime(self.lens, self.axes, self.settings, model, 2)
                    self.assertEqual(result['lens']['fov_degrees'], expected)
                    self.assertTrue(result['fov_override_resolved'])
                    self.assertEqual(result['fov_override_applied'], applied)
                    self.assertFalse(result['active_settings_verified'])
        self.assertEqual(self.lens['fov_degrees'], 95)

    def test_schema_requires_explicit_bounded_fov_and_preserves_legacy(self):
        self.settings['schema_version'] = 2
        for value in [None, True, '95', -1, 181, float('nan'), float('inf'), 10**400]:
            self.settings['fov_override_degrees'] = value
            with self.subTest(value=value), self.assertRaises(ValueError):
                OPT.runtime_parameters(json.dumps(self.settings).encode())
        del self.settings['fov_override_degrees']
        with self.assertRaises(ValueError):
            OPT.runtime_parameters(json.dumps(self.settings).encode())
        self.settings.update(schema_version=1, fov_override_degrees=95)
        with self.assertRaises(ValueError):
            OPT.runtime_parameters(json.dumps(self.settings).encode())
        del self.settings['fov_override_degrees']
        result = OPT.resolve_runtime(self.lens, self.axes, self.settings, 'neo3-pro-eye', 2)
        self.assertFalse(result['fov_override_resolved'])
        self.assertFalse(result['fov_override_applied'])


class PicoIpdTableTests(unittest.TestCase):
    def setUp(self):
        self.data = b'0,0.2,7,8,0\n1,0.7,9,10,100\n2,-0.3,11,12,200\n'
        self.table = OPT.ipd_offset_table(self.data)

    def test_stock_column_order_interval_delta_and_shared_endpoint(self):
        for sample, expected, bracket in [(0, 0, [0, 1]), (50, .25, [0, 1]), (100, .5, [0, 1]),
                                           (150, -.5, [1, 2]), (200, -1, [1, 2])]:
            result = OPT.lookup_ipd_offset(self.table, 'neo3-pro', sample, 0, 10)
            self.assertEqual(result['value_mm'], expected)
            self.assertEqual(result['bracket_rows'], bracket)
            self.assertFalse(result['table_selection_verified'])
            self.assertFalse(result['active_settings_verified'])
        self.assertEqual(self.table[0][2:4], [7, 8])
        self.assertEqual(self.table[1][2:4], [9, 10])

    def test_eye_model_gate_and_uncovered_sensor_values_return_zero(self):
        for model, sample, offset, reason in [('neo3-pro-eye', 50, 0, 'eye-model'),
                                             ('neo3-pro', 50, -11, 'hardware-offset-out-of-range'),
                                             ('neo3-pro', 50, 11, 'hardware-offset-out-of-range'),
                                             ('neo3-pro', -1, 0, 'sample-out-of-range'),
                                             ('neo3-pro', 201, 0, 'sample-out-of-range')]:
            result = OPT.lookup_ipd_offset(self.table, model, sample, offset, 10)
            self.assertEqual(result['value_mm'], 0)
            self.assertEqual(result['reason'], reason)
            self.assertIsNone(result['bracket_rows'])
        for offset in (-10, 10):
            self.assertEqual(OPT.lookup_ipd_offset(self.table, 'neo3-pro', 50, offset, 10)['value_mm'], .25)

    def test_table_refuses_ambiguous_rounding_and_malformed_rows(self):
        for data in [b'', b' '*20001, b'0,0,0,0,0', self.data+b'\0', self.data+b'\n',
                     b'0,0,0,0,0\n1,0,0,0,0', b'0,0,0,0,1\n1,0,0,0,0',
                     b'0,0,0,0,1000000000\n1,0,0,0,1000000001',
                     self.data.replace(b'0,0.2', b'0,nan'), self.data.replace(b'0,0.2', b'0,11'),
                     self.data.replace(b'0,0.2', b'0,1e100'), self.data.replace(b'0,0.2', b'0.0,0.2'),
                     self.data.replace(b'0,0.2', b'2147483648,0.2'), self.data+b'\xff',
                     b'0,'+b'0'*99+b',0,0,0\n1,0,0,0,1', self.data*101]:
            with self.subTest(data=data[:80]), self.assertRaises(ValueError):
                OPT.ipd_offset_table(data)
        for model, sample, offset, limit in [('unknown', 50, 0, 10), ('neo3-pro', True, 0, 10),
                                            ('neo3-pro', 50, -2147483648, 10), ('neo3-pro', 50, 0, -1),
                                            ('neo3-pro', 50, 0, False)]:
            with self.assertRaises(ValueError):
                OPT.lookup_ipd_offset(self.table, model, sample, offset, limit)

    def test_cli_refuses_partial_inputs_and_conflicting_runtime_correction(self):
        with tempfile.TemporaryDirectory() as temp:
            d = Path(temp)
            config = d/'A7H10_0.ini'; config.write_bytes(fixture())
            offsets = d/'axisOffset.txt'; offsets.write_text('0,0,0,0\n'*3)
            table = d/'IpdOffsetList0.txt'; table.write_bytes(self.data)
            runtime = d/'runtime.json'
            settings = {'schema_version': 1, 'ipd_enabled': True, 'ipd_correction_enabled': True,
                        'ipd_correction_mm': .25, 'screen_gap_m': 0, 'axis_corrections_mm': [0]*4}
            runtime.write_text(json.dumps(settings))
            output = d/'lookup.json'
            digest = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
            command = [sys.executable, '-B', str(ROOT/'tools/inspect-pico-optics.py'), str(config), str(offsets),
                       '--config-sha256', digest(config), '--offsets-sha256', digest(offsets),
                       '--model', 'neo3-pro', '--ipd-state', '2', '--output', str(output)]
            extra = ['--ipd-table', str(table), '--ipd-table-sha256', digest(table),
                     '--ipd-sample', '50', '--ipd-offset', '0', '--ipd-range-limit', '10']
            for i in range(0, len(extra), 2):
                self.assertNotEqual(subprocess.run(command+extra[:i]+extra[i+2:], capture_output=True, timeout=5).returncode, 0)
                self.assertFalse(output.exists())
            settings['ipd_correction_mm'] = .5; runtime.write_text(json.dumps(settings))
            runtime_args = ['--runtime-settings', str(runtime), '--runtime-sha256', digest(runtime)]
            refused = subprocess.run(command+extra+runtime_args, capture_output=True, timeout=5)
            self.assertNotEqual(refused.returncode, 0)
            self.assertIn(b'disagrees', refused.stderr); self.assertFalse(output.exists())
            settings['ipd_correction_mm'] = .25; runtime.write_text(json.dumps(settings))
            runtime_args[-1] = digest(runtime)
            subprocess.run(command+extra+runtime_args, check=True, capture_output=True, timeout=5)
            result = json.loads(output.read_text())
            self.assertEqual(result['ipd_lookup']['value_mm'], .25)
            self.assertTrue(result['runtime_adjustment']['ipd_lookup_verified'])
            self.assertEqual(result['runtime_adjustment']['optical_offsets_mm'], [-.25, 0, .25, 0])
            self.assertFalse(result['runtime_adjustment']['active_settings_verified'])
            self.assertFalse(result['runtime_usable'])
            self.assertEqual(result['files']['ipd_table']['sha256'], digest(table))
            self.assertEqual(output.stat().st_mode & 0o777, 0o600)


class PicoProjectionTests(unittest.TestCase):
    def setUp(self):
        self.model = {'schema_version': 1, 'left': [-.1, .1, .1, -.1, .1, 100],
                      'right': [-.2, .4, .3, -.1, .2, 10]}

    def test_distinct_eye_frustums_preserve_order_and_near_plane(self):
        result = OPT.projection_parameters(json.dumps(self.model).encode())
        for actual, expected in zip(result['fov'][0], [-math.pi/4, math.pi/4, math.pi/4, -math.pi/4]):
            self.assertAlmostEqual(actual, expected, delta=5e-8)
        for actual, expected in zip(result['fov'][1], [-math.pi/4, math.atan(2), math.atan(1.5), -math.atan(.5)]):
            self.assertAlmostEqual(actual, expected, delta=1e-7)
        self.assertFalse(result['runtime_usable'])
        self.assertFalse(result['active_settings_verified'])
        self.assertEqual(result['frustums'], self.model)
        scaled = copy.deepcopy(self.model)
        scaled['right'] = [v*2 for v in scaled['right']]
        self.assertEqual(OPT.projection_parameters(json.dumps(scaled).encode())['fov'], result['fov'])

    def test_malformed_unbounded_and_degenerate_frustums_refused(self):
        for values in [[0, .1, .1, -.1, .1, 100], [-.1, .1, -.1, .1, .1, 100],
                       [-.1, .1, .1, -.1, 0, 100], [-.1, .1, .1, -.1, .1, .1],
                       [-.1, .1, .1, -.1, 1e-100, 100], [-.1, .1, .1, -.1, 1e-20, 100],
                       [-.1, .1, .1, -.1, 1e-40, 100],
                       [-.1, .1, .1, -.1, .1, .1000000001],
                       [-1e-100, .1, .1, -.1, .1, 100], [-.1, .1, .1, -.1, .1],
                       [-.1, .1, .1, -.1, .1, float('inf')],
                       [-.1, .1, .1, -.1, .1, 10**400], [-.1, .1, True, -.1, .1, 100]]:
            model = dict(self.model, left=values)
            with self.subTest(values=values), self.assertRaises(ValueError):
                OPT.projection_parameters(json.dumps(model).encode())
        for data in [b'[]', b'{}', b'null', b' '*4097, b'{}\0', b'\xff',
                     json.dumps(dict(self.model, schema_version=True)).encode(),
                     json.dumps(dict(self.model, unexpected=1)).encode(),
                     json.dumps(self.model).replace('"schema_version": 1', '"schema_version": 1, "schema_version": 1').encode()]:
            with self.subTest(data=data[:40]), self.assertRaises(ValueError):
                OPT.projection_parameters(data)

    def test_cli_requires_hash_and_screen_fov_agreement(self):
        with tempfile.TemporaryDirectory() as temp:
            d = Path(temp)
            config = d/'A7H10_0.ini'; config.write_bytes(fixture())
            offsets = d/'axisOffset.txt'; offsets.write_text('0,0,0,0')
            projection = d/'projection.json'; projection.write_text(json.dumps(self.model))
            screen = d/'screen.json'; screen.write_text(json.dumps(screen_fixture()))
            output = d/'resolved.json'
            digest = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
            command = [sys.executable, '-B', str(ROOT/'tools/inspect-pico-optics.py'), str(config), str(offsets),
                       '--config-sha256', digest(config), '--offsets-sha256', digest(offsets),
                       '--model', 'neo3-pro-eye', '--ipd-state', '2', '--output', str(output)]
            extra = ['--projection-model', str(projection), '--projection-sha256', digest(projection)]
            for partial in [extra[:2], extra[2:], extra[:3]+['0'*64]]:
                self.assertNotEqual(subprocess.run(command+partial, capture_output=True, timeout=5).returncode, 0)
                self.assertFalse(output.exists())
            screen_args = ['--screen-model', str(screen), '--screen-sha256', digest(screen), '--grid-size', '3', '3']
            result = subprocess.run(command+extra+screen_args, capture_output=True, timeout=5)
            self.assertNotEqual(result.returncode, 0); self.assertIn(b'disagrees', result.stderr)
            self.assertFalse(output.exists())
            geometry = screen_fixture()
            geometry['fov'] = OPT.projection_parameters(projection.read_bytes())['fov']
            screen.write_text(json.dumps(geometry)); screen_args[3] = digest(screen)
            subprocess.run(command+extra+screen_args, check=True, capture_output=True, timeout=5)
            result = json.loads(output.read_text())
            self.assertEqual(result['files']['projection_model']['sha256'], digest(projection))
            self.assertEqual(result['screen_grid']['screen_model']['fov'], result['projection']['fov'])
            self.assertEqual(output.stat().st_mode & 0o777, 0o600)
            self.assertFalse(result['runtime_usable'])


class PicoCompositorAxisTests(unittest.TestCase):
    def setUp(self):
        self.settings = {'schema_version': 1, 'hwc_enabled': True, 'hwc_reverse': 180,
                         'sdk_axis_overrides_mm': [None, -.6, .8, None]}

    def test_sdk_replacement_precedes_swap_and_negation_without_mutating_inputs(self):
        service = [.1, .2, .3, .4]
        original = copy.deepcopy(self.settings)
        for enabled in (False, True):
            for reverse in (0, 180):
                settings = dict(self.settings, hwc_enabled=enabled, hwc_reverse=reverse)
                result = OPT.resolve_compositor_axes(service, settings)
                expected = [-.8, -.4, -.1, .6] if enabled and reverse == 180 else [.1, -.6, .8, .4]
                self.assertEqual(result['optical_offsets_mm'], list(map(OPT.binary32, expected)))
                self.assertEqual(result['effective_hwc_reverse'], 180 if enabled and reverse == 180 else 0)
                self.assertEqual(result['before_hwc_mm'], list(map(OPT.binary32, [.1, -.6, .8, .4])))
                self.assertFalse(result['runtime_usable'])
                self.assertFalse(result['active_settings_verified'])
        self.assertEqual(service, [.1, .2, .3, .4])
        self.assertEqual(self.settings, original)

    def test_zero_override_is_not_absent_and_mesh_reversal_is_a_separate_stage(self):
        settings = dict(self.settings, sdk_axis_overrides_mm=[0, None, None, None])
        result = OPT.resolve_compositor_axes([.1, .2, .3, .4], settings)
        self.assertEqual(result['optical_offsets_mm'][2], 0)
        self.assertEqual(math.copysign(1, result['optical_offsets_mm'][2]), -1)
        settings['sdk_axis_overrides_mm'] = [None] * 4
        result = OPT.resolve_compositor_axes([.1, .2, .3, .4], settings)
        screen = screen_fixture()
        screen.update(hwc_reverse=180, optical_offsets_mm=result['optical_offsets_mm'])
        lens = OPT.lens_parameters(fixture())
        expected = [[.0075, -.01], [.0025, -.005]]
        for eye in (0, 1):
            sample = OPT.screen_tangents(lens, screen, eye, .5, .5)
            for actual, wanted in zip(sample['input'], expected[eye]):
                self.assertAlmostEqual(actual, wanted, delta=1e-9)

    def test_ambiguous_invalid_and_out_of_range_settings_refused(self):
        for key, value in [('schema_version', True), ('schema_version', 2), ('hwc_enabled', 1),
                           ('hwc_reverse', True), ('hwc_reverse', 90), ('hwc_reverse', 180.0),
                           ('sdk_axis_overrides_mm', [None]*3), ('sdk_axis_overrides_mm', 'null'),
                           ('sdk_axis_overrides_mm', [0, 0, 0, 1.001]),
                           ('sdk_axis_overrides_mm', [0, 0, 0, float('nan')]),
                           ('sdk_axis_overrides_mm', [0, 0, 0, 10**400]),
                           ('sdk_axis_overrides_mm', [0, 0, 0, True])]:
            with self.subTest(key=key, value=value), self.assertRaises(ValueError):
                OPT.compositor_axis_parameters(json.dumps(dict(self.settings, **{key: value})).encode())
        for key in self.settings:
            invalid = dict(self.settings); del invalid[key]
            with self.subTest(missing=key), self.assertRaises(ValueError):
                OPT.compositor_axis_parameters(json.dumps(invalid).encode())
        for data in [b'[]', b'{}', b'null', b' '*4097, b'{}\0', b'\xff',
                     json.dumps(dict(self.settings, extra=1)).encode(),
                     json.dumps(self.settings).replace('"schema_version": 1', '"schema_version": 1, "schema_version": 1').encode()]:
            with self.subTest(data=data[:40]), self.assertRaises(ValueError):
                OPT.compositor_axis_parameters(data)
        for offsets in [[0]*3, [0, 0, 0, float('inf')], [0, 0, 0, True], [0, 0, 0, 10**400], [0, 0, 0, -1.01]]:
            with self.subTest(offsets=offsets), self.assertRaises(ValueError):
                OPT.resolve_compositor_axes(offsets, self.settings)

    def test_cli_checks_resolved_offsets_hashes_runtime_and_effective_hwc(self):
        with tempfile.TemporaryDirectory() as temp:
            d = Path(temp)
            config = d/'A7H10_0.ini'; config.write_bytes(fixture())
            offsets = d/'axisOffset.txt'; offsets.write_text('.1,.2,.3,.4')
            runtime = d/'runtime.json'
            runtime.write_text(json.dumps({'schema_version': 1, 'ipd_enabled': True,
                               'ipd_correction_enabled': False, 'ipd_correction_mm': 0,
                               'screen_gap_m': .003, 'axis_corrections_mm': [0]*4}))
            compositor = d/'compositor.json'; compositor.write_text(json.dumps(self.settings))
            screen = d/'screen.json'; output = d/'result.json'
            digest = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
            command = [sys.executable, '-B', str(ROOT/'tools/inspect-pico-optics.py'), str(config), str(offsets),
                       '--config-sha256', digest(config), '--offsets-sha256', digest(offsets),
                       '--model', 'neo3-pro-eye', '--ipd-state', '2', '--output', str(output)]
            extra = ['--compositor-axes', str(compositor), '--compositor-axes-sha256', digest(compositor)]
            runtime_args = ['--runtime-settings', str(runtime), '--runtime-sha256', digest(runtime)]
            for args in [extra, runtime_args+extra[:2], runtime_args+extra[2:], runtime_args+extra[:3]+['0'*64]]:
                self.assertNotEqual(subprocess.run(command+args, capture_output=True, timeout=5).returncode, 0)
                self.assertFalse(output.exists())
            geometry = screen_fixture()
            geometry.update(lens_separation_m=OPT.binary32(OPT.binary32(.0634)-OPT.binary32(.003)),
                            optical_offsets_mm=list(map(OPT.binary32, [-.8, -.4, -.1, .6])), hwc_reverse=180)
            for wrong in [dict(geometry, hwc_reverse=0), dict(geometry, optical_offsets_mm=[.1, .2, .3, .4])]:
                screen.write_text(json.dumps(wrong))
                args = ['--screen-model', str(screen), '--screen-sha256', digest(screen), '--grid-size', '3', '3']
                result = subprocess.run(command+runtime_args+extra+args, capture_output=True, timeout=5)
                self.assertNotEqual(result.returncode, 0); self.assertIn(b'disagrees', result.stderr)
                self.assertFalse(output.exists())
            screen.write_text(json.dumps(geometry))
            args = ['--screen-model', str(screen), '--screen-sha256', digest(screen), '--grid-size', '3', '3']
            subprocess.run(command+runtime_args+extra+args, check=True, capture_output=True, timeout=5)
            result = json.loads(output.read_text())
            self.assertEqual(result['files']['compositor_axes']['sha256'], digest(compositor))
            self.assertEqual(result['compositor_axes']['optical_offsets_mm'], geometry['optical_offsets_mm'])
            self.assertEqual(result['runtime_adjustment']['optical_offsets_mm'], list(map(OPT.binary32, [.1, .2, .3, .4])))
            self.assertEqual(result['screen_grid']['screen_model'], geometry)
            self.assertFalse(result['runtime_usable'])
            self.assertEqual(output.stat().st_mode & 0o777, 0o600)
