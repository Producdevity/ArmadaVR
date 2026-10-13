#!/usr/bin/env python3
"""Inspect saved Neo3 optical parameters without producing runtime calibration."""
import argparse
import configparser
import hashlib
import json
import math
import os
from pathlib import Path
import re
import stat


DECIMAL = r'[+-]?(?:[0-9]+(?:\.[0-9]*)?|\.[0-9]+)(?:[eE][+-]?[0-9]+)?'


def read_file(path, expected, limit):
    if not re.fullmatch(r'[0-9a-f]{64}', expected):
        raise ValueError('Expected an independently retained SHA-256')
    fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
    info = os.fstat(fd)
    if not stat.S_ISREG(info.st_mode) or not 0 < info.st_size <= limit:
        os.close(fd)
        raise ValueError('Expected a bounded regular calibration file')
    with os.fdopen(fd, 'rb') as stream:
        data = stream.read(limit + 1)
        if len(data) != info.st_size or hashlib.sha256(data).hexdigest() != expected:
            raise ValueError('Calibration file size or SHA-256 mismatch')
    return data


def number(text):
    text = text.strip()
    if len(text) > 64 or not re.fullmatch(DECIMAL, text):
        raise ValueError('Expected a decimal optical parameter')
    value = float(text)
    if not math.isfinite(value):
        raise ValueError('Non-finite optical parameter')
    return value


def lens_parameters(data):
    if not 0 < len(data) <= 65536 or b'\0' in data:
        raise ValueError('Invalid display configuration size or NUL')
    parser = configparser.ConfigParser(interpolation=None, strict=True, empty_lines_in_values=False)
    parser.optionxform = str
    try:
        parser.read_string(data.decode('ascii'))
    except (configparser.Error, UnicodeError) as error:
        raise ValueError('Malformed display configuration') from error
    if parser.defaults() or set(parser.sections()) != {'lens', 'CylinderParameters'}:
        raise ValueError('Unexpected display configuration sections')
    expected = {'lensSeparation', 'FOV', 'metersPerTanAngleAtCenter', 'distortionEqnType'}
    expected.update(f'distortionCoefficients{i}' for i in range(20))
    expected.update(f'chromaticAberration{i}' for i in range(4))
    if set(parser['lens']) != expected:
        raise ValueError('Missing or unexpected lens parameter')
    lens = {key: number(value) for key, value in parser['lens'].items()}
    if (not 0 < lens['lensSeparation'] < .2 or not 0 < lens['FOV'] < 180 or
            not 0 < lens['metersPerTanAngleAtCenter'] < 1 or lens['distortionEqnType'] != 2):
        raise ValueError('Unsupported or out-of-range lens model')
    expected = {'iCylinderMeshX', 'iCylinderMeshY', 'fCylinderInPosFactor', 'fCylinderOutPosFactor',
                'fCylinderDistortionBoundThreshold', 'fCylinderDistortionBoundSetValue'}
    expected.update(f'fCylinderDistortionC10K{i}' for i in range(1, 11))
    if set(parser['CylinderParameters']) != expected:
        raise ValueError('Missing or unexpected cylinder parameter')
    cylinder = {key: number(value) for key, value in parser['CylinderParameters'].items()}
    for key in ('iCylinderMeshX', 'iCylinderMeshY'):
        if not cylinder[key].is_integer() or not 1 <= cylinder[key] <= 256:
            raise ValueError('Invalid cylinder mesh size')
        cylinder[key] = int(cylinder[key])
    return {'lens_separation_m': lens['lensSeparation'], 'fov_degrees': lens['FOV'],
            'meters_per_tan_angle_at_center': lens['metersPerTanAngleAtCenter'],
            'equation_type': int(lens['distortionEqnType']),
            'distortion_coefficients': [lens[f'distortionCoefficients{i}'] for i in range(20)],
            'chromatic_aberration': [lens[f'chromaticAberration{i}'] for i in range(4)],
            'cylinder': cylinder}



def radial_scale(coefficients, radius_squared):
    if (len(coefficients) != 11 or not all(math.isfinite(c) for c in coefficients) or
            not math.isfinite(radius_squared) or radius_squared < 0):
        raise ValueError('Expected eleven finite coefficients and a nonnegative squared radius')
    scale = 0.0
    for coefficient in reversed(coefficients):
        scale = scale * radius_squared + coefficient
    if not math.isfinite(scale):
        raise ValueError('Radial distortion overflow')
    return scale


def sample_tangents(lens, x, y):
    if not math.isfinite(x) or not math.isfinite(y) or lens['equation_type'] != 2:
        raise ValueError('Expected finite lens coordinates and equation type 2')
    coefficients, chromatic = lens['distortion_coefficients'], lens['chromatic_aberration']
    if (len(coefficients) != 20 or len(chromatic) != 4 or
            not all(math.isfinite(c) for c in coefficients + chromatic)):
        raise ValueError('Invalid stock lens record')
    radius_squared = x * x + y * y
    radius = math.sqrt(radius_squared)
    scale = radial_scale(coefficients[:11], radius_squared)
    red = scale * (1 + chromatic[0] + radius_squared * chromatic[1] + radius * coefficients[18])
    blue = scale * (1 + chromatic[2] + radius_squared * chromatic[3] + radius * coefficients[19])
    result = {'input': [x, y], 'red': [x * red, y * red], 'green': [x * scale, y * scale],
              'blue': [x * blue, y * blue]}
    if not all(math.isfinite(value) for channel in result.values() for value in channel):
        raise ValueError('Chromatic distortion overflow')
    return result


def screen_parameters(data):
    def unique(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise ValueError('Duplicate screen parameter')
            result[key] = value
        return result

    if not 0 < len(data) <= 16384 or b'\0' in data:
        raise ValueError('Invalid screen model size or NUL')
    try:
        screen = json.loads(data, object_pairs_hook=unique)
    except (ValueError, UnicodeError) as error:
        raise ValueError('Malformed screen model') from error
    expected = {'schema_version', 'layout', 'width_pixels', 'height_pixels', 'width_m',
                'lens_separation_m', 'horizontal_offset_m', 'meters_per_tan_angle',
                'optical_offsets_mm', 'hwc_reverse', 'separate_distortion', 'fov'}
    if not isinstance(screen, dict) or set(screen) != expected:
        raise ValueError('Missing or unexpected screen parameter')
    if type(screen['schema_version']) is not int or screen['schema_version'] != 1:
        raise ValueError('Unsupported screen schema')
    if screen['layout'] != 'side-by-side' or screen['separate_distortion'] is not False:
        raise ValueError('Only the shared-coefficient side-by-side lens model is supported')
    for key in ('width_pixels', 'height_pixels'):
        if type(screen[key]) is not int or not 2 <= screen[key] <= 16384:
            raise ValueError('Invalid screen dimensions')
    if screen['width_pixels'] % 2:
        raise ValueError('Side-by-side screen width must be even')
    if type(screen['hwc_reverse']) is not int or screen['hwc_reverse'] not in (0, 180):
        raise ValueError('Expected effective HWC reversal of 0 or 180 degrees')
    for key, low, high in [('width_m', .001, 1), ('lens_separation_m', .001, .2),
                           ('meters_per_tan_angle', .001, 1), ('horizontal_offset_m', -.1, .1)]:
        value = screen[key]
        if type(value) not in (int, float) or not math.isfinite(value) or not low <= value <= high:
            raise ValueError('Invalid physical screen parameter')
    offsets = screen['optical_offsets_mm']
    if (not isinstance(offsets, list) or len(offsets) != 4 or
            any(type(v) not in (int, float) or not math.isfinite(v) or abs(v) > 1 for v in offsets)):
        raise ValueError('Expected four finite, adjusted optical offsets in millimetres')
    fovs = screen['fov']
    if not isinstance(fovs, list) or len(fovs) != 2:
        raise ValueError('Expected explicit left and right render FOVs')
    for fov in fovs:
        if (not isinstance(fov, list) or len(fov) != 4 or
                any(type(v) not in (int, float) or not math.isfinite(v) for v in fov) or
                not (-1.57 < fov[0] < 0 < fov[1] < 1.57 and -1.57 < fov[3] < 0 < fov[2] < 1.57)):
            raise ValueError('Expected finite left, right, up, down FOV angles in radians')
    return screen


def screen_tangents(lens, screen, eye, u, v):
    if type(eye) is not int or eye not in (0, 1):
        raise ValueError('Expected left or right eye index')
    if not all(math.isfinite(c) and 0 <= c <= 1 for c in (u, v)):
        raise ValueError('Expected normalized per-eye screen coordinates')
    v = 1 - v
    quarter_width = screen['width_m'] * .25
    shift = screen['lens_separation_m'] * .5 - quarter_width
    offsets = screen['optical_offsets_mm']
    dx = (shift if eye == 0 else -shift) + screen['horizontal_offset_m'] + offsets[eye * 2] * .001
    dy = -offsets[eye * 2 + 1] * .001
    # Stock HWC reversal changes both mesh placement and optical offsets.
    if screen['hwc_reverse'] == 180:
        u, v, dx, dy = 1 - u, 1 - v, -dx, -dy
    aspect = screen['width_pixels'] * .5 / screen['height_pixels']
    x = (dx + (2 * u - 1) * quarter_width) / screen['meters_per_tan_angle']
    y = (dy + (2 * v - 1) * quarter_width / aspect) / screen['meters_per_tan_angle']
    return sample_tangents(lens, x, y)


def project_tangents(tangents, fov):
    left, right, up, down = map(math.tan, fov)
    result = {color: [(tangents[color][0] - left) / (right - left),
                      (up - tangents[color][1]) / (up - down)]
              for color in ('red', 'green', 'blue')}
    if not all(math.isfinite(value) for pair in result.values() for value in pair):
        raise ValueError('Texture projection overflow')
    return result


def screen_grid(lens, screen, width, height):
    if any(type(n) is not int or not 2 <= n <= 65 for n in (width, height)):
        raise ValueError('Grid dimensions must be integers from 2 to 65')
    views = []
    for eye in (0, 1):
        grid = []
        inside = {color: 0 for color in ('red', 'green', 'blue')}
        for row in range(height):
            for col in range(width):
                sample = screen_tangents(lens, screen, eye, col / (width - 1), row / (height - 1))
                uv = project_tangents(sample, screen['fov'][eye])
                grid.append([value for color in ('red', 'green', 'blue') for value in uv[color]])
                for color in inside:
                    inside[color] += all(0 <= value <= 1 for value in uv[color])
        views.append({'eye': eye, 'grid_width': width, 'grid_height': height,
                      'distortion': grid, 'in_texture_vertices': inside,
                      'within_driver_coordinate_limit': all(abs(v) <= 4 for point in grid for v in point)})
    return {'screen_model': screen, 'views': views,
            'coordinates': 'per-eye screen and texture UVs, top-left origin, before panel rotation',
            'runtime_usable': False, 'calibration_applied': False,
            'mesh_clipping_applied': False,
            'remaining': ['Verify exact-unit active geometry, FOV, coefficients and offsets',
                          'Verify panel/view rotation and physical optical alignment',
                          'Verify grid interpolation error across the visible field']}


def audit_grid(lens, screen, grid, render_size, max_error_pixels):
    if (len(render_size) != 2 or any(type(n) is not int or not 1 <= n <= 16384 for n in render_size) or
            type(max_error_pixels) not in (int, float) or not math.isfinite(max_error_pixels) or
            not 0 < max_error_pixels <= 16):
        raise ValueError('Expected bounded render dimensions and a positive pixel-error limit')
    views = []
    for eye, view in enumerate(grid['views']):
        width, height = view['grid_width'], view['grid_height']
        tested = visible = 0
        largest = 0.0
        worst = None
        for row in range(height - 1):
            for col in range(width - 1):
                a = view['distortion'][row * width + col]
                b = view['distortion'][row * width + col + 1]
                c = view['distortion'][(row + 1) * width + col]
                d = view['distortion'][(row + 1) * width + col + 1]
                for fy in (.25, .5, .75):
                    for fx in (.25, .5, .75):
                        u, v = (col + fx) / (width - 1), (row + fy) / (height - 1)
                        exact = project_tangents(screen_tangents(lens, screen, eye, u, v), screen['fov'][eye])
                        interpolated = []
                        for p, q, r, s in zip(a, b, c, d):
                            top, bottom = p + fx * (q - p), r + fx * (s - r)
                            interpolated.append(top + fy * (bottom - top))
                        for channel, name in enumerate(('red', 'green', 'blue')):
                            approximation = interpolated[channel * 2:channel * 2 + 2]
                            tested += 1
                            if not (all(0 <= n <= 1 for n in exact[name]) or
                                    all(0 <= n <= 1 for n in approximation)):
                                continue
                            visible += 1
                            error = max(abs(a - b) * pixels for a, b, pixels in zip(exact[name], approximation, render_size))
                            if error > largest:
                                largest = error
                                worst = {'screen_uv': [u, v], 'channel': name,
                                         'exact_uv': exact[name], 'interpolated_uv': approximation}
        views.append({'eye': eye, 'tested_channel_samples': tested, 'visible_channel_samples': visible,
                      'maximum_sampled_error_pixels': largest, 'worst_sample': worst,
                      'sampled_error_within_limit': visible > 0 and largest <= max_error_pixels})
    return {'render_size': list(render_size), 'max_error_pixels': max_error_pixels,
            'cell_sample_offsets': [.25, .5, .75], 'views': views,
            'sampled_error_within_limit': all(v['sampled_error_within_limit'] for v in views),
            'continuous_error_bound_verified': False, 'physical_accuracy_verified': False}


def axis_offsets(data, model, ipd_state):
    if model not in ('neo3-pro', 'neo3-pro-eye') or ipd_state not in (1, 2, 3):
        raise ValueError('Explicit Neo3 model and IPD state required')
    if not 0 < len(data) <= 1024 or b'\0' in data:
        raise ValueError('Invalid axis-offset size or NUL')
    try:
        lines = data.decode('ascii').splitlines()
    except UnicodeError as error:
        raise ValueError('Non-ASCII axis offsets') from error
    if len(lines) != (1 if model == 'neo3-pro-eye' else 3):
        raise ValueError('Offset row count does not match the selected headset model')
    rows = []
    for line in lines:
        if len(line) >= 99 or len(line.split(',')) != 4:
            raise ValueError('Expected four offsets within the stock 100-byte line buffer')
        row = [number(value) for value in line.split(',')]
        if any(abs(value) > 1 for value in row):
            raise ValueError('Offsets require stock clamping; measurement must be checked')
        rows.append(row)
    index = 0 if model == 'neo3-pro-eye' else {3: 0, 2: 1, 1: 2}[ipd_state]
    return {'rows': rows, 'selected_row': index, 'selected': rows[index],
            'order': ['left_x', 'left_y', 'right_x', 'right_y'],
            'property_corrections_applied': False}


def inspect(config, config_sha256, offsets, offsets_sha256, model, ipd_state):
    config, offsets = Path(config), Path(offsets)
    if not re.fullmatch(r'A7H10_[01]\.ini', config.name):
        raise ValueError('Requires an explicitly selected Neo3 A7H10 display configuration')
    lens = lens_parameters(read_file(config, config_sha256, 65536))
    axes = axis_offsets(read_file(offsets, offsets_sha256, 1024), model, ipd_state)
    return {'schema_version': 1, 'model': model, 'ipd_state': ipd_state,
            'files': {'display': {'name': config.name, 'sha256': config_sha256},
                      'axis_offsets': {'name': offsets.name, 'sha256': offsets_sha256}},
            'stock_lens': lens, 'axis_offsets': axes,
            'stock_axis_coefficient_indices': [14, 15, 16, 17],
            'stock_equation': {'radial_coefficient_indices': list(range(11)),
                               'red_radius_coefficient_index': 18, 'blue_radius_coefficient_index': 19,
                               'sample_coordinates': 'lens-relative tangents, not texture UVs'},
            'configuration_selection_verified': False, 'runtime_usable': False,
            'calibration_applied': False,
            'remaining': ['Verify active product and glasses-holder configuration selection',
                          'Resolve IPD, screen-gap, FOV and per-axis property overrides',
                          'Verify screen-to-texture mapping, rotations and separate-eye overrides',
                          'Validate physical optical alignment at the selected IPD']}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('config', type=Path)
    parser.add_argument('offsets', type=Path)
    parser.add_argument('--config-sha256', required=True)
    parser.add_argument('--offsets-sha256', required=True)
    parser.add_argument('--model', choices=('neo3-pro', 'neo3-pro-eye'), required=True)
    parser.add_argument('--ipd-state', type=int, choices=(1, 2, 3), required=True)
    parser.add_argument('--sample-tangent', type=float, nargs=2, action='append', default=[],
                        metavar=('X', 'Y'), help='Inspect an equation sample; does not apply screen/IPD transforms')
    parser.add_argument('--screen-model', type=Path, help='Explicit resolved geometry JSON; no device defaults')
    parser.add_argument('--screen-sha256', help='Independently retained screen-model SHA-256')
    parser.add_argument('--grid-size', type=int, nargs=2, metavar=('WIDTH', 'HEIGHT'))
    parser.add_argument('--render-size', type=int, nargs=2, metavar=('WIDTH', 'HEIGHT'),
                        help='Render texture dimensions for a sampled grid interpolation audit')
    parser.add_argument('--max-error-pixels', type=float, help='Maximum sampled per-axis interpolation error')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    try:
        result = inspect(args.config, args.config_sha256, args.offsets, args.offsets_sha256,
                         args.model, args.ipd_state)
        if len(args.sample_tangent) > 256:
            raise ValueError('At most 256 tangent samples are allowed')
        result['tangent_samples'] = [sample_tangents(result['stock_lens'], x, y) for x, y in args.sample_tangent]
        if any(value is not None for value in (args.screen_model, args.screen_sha256, args.grid_size)):
            if any(value is None for value in (args.screen_model, args.screen_sha256, args.grid_size)):
                raise ValueError('Screen model, SHA-256 and grid size are required together')
            screen = screen_parameters(read_file(args.screen_model, args.screen_sha256, 16384))
            result['screen_grid'] = screen_grid(result['stock_lens'], screen, *args.grid_size)
            result['files']['screen_model'] = {'name': args.screen_model.name, 'sha256': args.screen_sha256}
        if args.render_size is not None or args.max_error_pixels is not None:
            if args.render_size is None or args.max_error_pixels is None or 'screen_grid' not in result:
                raise ValueError('Grid, render size and pixel-error limit are required for the audit')
            result['grid_audit'] = audit_grid(result['stock_lens'], screen, result['screen_grid'],
                                              args.render_size, args.max_error_pixels)
        fd = os.open(args.output, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(fd, 'w') as stream:
            json.dump(result, stream, indent=2)
            stream.write('\n')
    except (OSError, ValueError) as error:
        parser.exit(1, f'{error}\n')


if __name__ == '__main__':
    main()
