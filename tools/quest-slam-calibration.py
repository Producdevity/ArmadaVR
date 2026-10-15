#!/usr/bin/env python3
"""Convert explicit offline Quest camera/IMU geometry for pre-calibrated VIT input."""
import argparse
import hashlib
import importlib.util
import json
import math
import os
from pathlib import Path
import stat

SPEC = importlib.util.spec_from_file_location('quest_calibration', Path(__file__).with_name('extract-quest-calibration.py'))
CAL = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CAL)
require = CAL.require
NOISE_FIELDS = ('accel_noise_std', 'gyro_noise_std', 'accel_bias_std', 'gyro_bias_std')


def dot(a, b):
    return sum(x * y for x, y in zip(a, b))


def multiply(a, b):
    return [sum(a[3 * i + k] * b[3 * k + j] for k in range(3)) for i in range(3) for j in range(3)]


def transpose(a):
    return [a[3 * j + i] for i in range(3) for j in range(3)]


def proper_rotation(a):
    require(all(math.isfinite(x) for x in a), 'Nonfinite rotation')
    product = multiply(a, transpose(a))
    require(max(abs(x - (1 if i in (0, 4, 8) else 0)) for i, x in enumerate(product)) <= 1e-10
            and abs(CAL.determinant(a) - 1) <= 1e-10, 'Rotation does not meet native IMU precision')


def split_rotation(a):
    scale = max(abs(x) for x in a)
    require(scale > 0, 'Zero rectification')
    columns = [[a[3 * i + j] / scale for i in range(3)] for j in range(3)]
    norm = math.hypot(*columns[0])
    require(norm > 1e-12, 'Degenerate rectification')
    first = [x / norm for x in columns[0]]
    second = [x - y * dot(first, columns[1]) for x, y in zip(columns[1], first)]
    residual = dot(first, second)
    second = [x - y * residual for x, y in zip(second, first)]
    norm = math.hypot(*second)
    require(norm > 1e-12, 'Degenerate rectification')
    second = [x / norm for x in second]
    third = [first[1] * second[2] - first[2] * second[1],
             first[2] * second[0] - first[0] * second[2],
             first[0] * second[1] - first[1] * second[0]]
    norm = math.hypot(*third)
    require(norm > 1e-12, 'Degenerate rectification')
    third = [x / norm for x in third]
    require(dot(third, columns[2]) > 1e-12, 'Reflected rectification')
    rotation = [column[i] for i in range(3) for column in (first, second, third)]
    proper_rotation(rotation)
    return rotation


def quaternion(r):
    proper_rotation(r)
    trace = r[0] + r[4] + r[8]
    if trace > 0:
        s = 2 * math.sqrt(1 + trace)
        q = [(r[7] - r[5]) / s, (r[2] - r[6]) / s, (r[3] - r[1]) / s, s / 4]
    else:
        i = max(range(3), key=lambda j: r[3 * j + j])
        j, k = (i + 1) % 3, (i + 2) % 3
        s = 2 * math.sqrt(1 + r[3 * i + i] - r[3 * j + j] - r[3 * k + k])
        q = [0.0] * 4
        q[i], q[j], q[k], q[3] = s / 4, (r[3 * i + j] + r[3 * j + i]) / s, (r[3 * i + k] + r[3 * k + i]) / s, (r[3 * k + j] - r[3 * j + k]) / s
    norm = math.hypot(*q)
    return [x / norm for x in q]


def convert(text, serial, camera_ids, frame, noise):
    CAL.calibration(text, serial, True)
    unit = CAL.DECODER.decode(text)
    require(frame in ('original', 'accelerometer'), 'Explicit supported IMU frame required')
    require(isinstance(camera_ids, list) and 2 <= len(camera_ids) <= 4 and
            all(isinstance(i, str) and i.isascii() and i.isdecimal() and str(int(i)) == i and 0 <= int(i) <= 255 for i in camera_ids) and
            len(set(camera_ids)) == len(camera_ids), 'Select two to four distinct numeric camera IDs')
    require(isinstance(noise, dict) and set(noise) == {'imu_update_rate', *NOISE_FIELDS}, 'Explicit rate and four noise vectors required')
    rate = CAL.number(noise['imu_update_rate'])
    require(0 < rate <= 10000, 'Invalid IMU rate')
    for key in NOISE_FIELDS:
        require(all(0 < x < 1e6 for x in CAL.vector(noise[key], 3)), 'Invalid noise standard deviation')
    imu = unit['ImuCalibration']
    device = imu['DeviceFromImu']
    rotation = [device[4 * i + j] for i in range(3) for j in range(3)]
    proper_rotation(rotation)
    for name in ('Accelerometer', 'Gyroscope'):
        split_rotation(imu[name]['RectificationMatrix'])
    if frame == 'accelerometer':
        rotation = multiply(rotation, split_rotation(imu['Accelerometer']['RectificationMatrix']))
    proper_rotation(rotation)
    inverse = transpose(rotation)
    cameras = {c['Id']: c for c in unit['CameraCalibration']}
    poses, intrinsics, resolutions, selected = [], [], [], []
    for ident in camera_ids:
        require(ident in cameras, 'Camera ID is absent from calibration')
        camera = cameras[ident]
        require(camera['Shutter']['Type'] == 'Global', 'Rolling shutter is not supported by this VIT path')
        transform = camera['DeviceFromCamera']
        cam_rotation = [transform[4 * i + j] for i in range(3) for j in range(3)]
        r = multiply(inverse, cam_rotation)
        q = quaternion(r)
        delta = [transform[4 * i + 3] - device[4 * i + 3] for i in range(3)]
        translation = [dot(inverse[3 * i:3 * i + 3], delta) for i in range(3)]
        poses.append(dict(zip(('px', 'py', 'pz', 'qx', 'qy', 'qz', 'qw'), translation + q)))
        f, cx, cy = camera['Projection']['Coefficients']
        values = [f, f, cx, cy, *camera['Distortion']['Coefficients'], 0, 0, 0, 0]
        keys = ('fx', 'fy', 'cx', 'cy', 'k1', 'k2', 'k3', 'k4', 'k5', 'k6', 'p1', 'p2', 's1', 's2', 's3', 's4')
        intrinsics.append({'camera_type': 'fisheye624', 'intrinsics': dict(zip(keys, values))})
        resolutions.append(camera['ImageSize'])
        selected.append({'id': ident, 'sensor_type': camera['SensorType'], 'image_size': camera['ImageSize'],
                         'unprojected_fov_unit_plane': camera.get('UnprojectedFovUnitPlane')})
    # Native input already applies sensor calibration and maps exposure time.
    calibration = {'value0': {'T_imu_cam': poses, 'intrinsics': intrinsics, 'resolution': resolutions,
                             'calib_accel_bias': [0.0] * 9, 'calib_gyro_bias': [0.0] * 12,
                             **noise, 'cam_time_offset_ns': 0, 'vignette': []}}
    effective = [rotation[3 * i + j] if j < 3 else device[4 * i + 3] for i in range(3) for j in range(4)] + [0, 0, 0, 1]
    manifest = {'schema_version': 1, 'imu_frame': frame, 'device_from_effective_imu': effective,
                'camera_order': selected, 'input_requirement': 'pre-calibrated SI IMU and exposure-center camera timestamps in one verified clock domain',
                'camera_geometry': 'full calibrated image, no crop, resize, rotation or reflection',
                'source_sha256': hashlib.sha256(text.encode()).hexdigest(),
                'rate_and_noise_verified': False, 'active_calibration_verified': False,
                'camera_identity_verified': False, 'physical_tracking_verified': False,
                'automatic_startup': False, 'device_actions': []}
    return calibration, manifest


def read_private(path):
    with os.fdopen(os.open(path, os.O_RDONLY | os.O_NOFOLLOW), 'rb') as stream:
        require(stat.S_ISREG(os.fstat(stream.fileno()).st_mode), 'Expected a regular input file')
        raw = stream.read(2 * 1024 * 1024 + 1)
    require(len(raw) <= 2 * 1024 * 1024, 'Input exceeds 2 MiB')
    return raw.decode('utf-8')


def export(source, serial, camera_ids, frame, noise_path, output):
    text, noise_text = read_private(source), read_private(noise_path)
    calibration, manifest = convert(text, serial, camera_ids, frame, CAL.DECODER.decode(noise_text))
    payload = (json.dumps(calibration, indent=2, allow_nan=False) + '\n').encode()
    manifest.update(calibration_sha256=hashlib.sha256(payload).hexdigest(), noise_input_sha256=hashlib.sha256(noise_text.encode()).hexdigest())
    output.mkdir(mode=0o700, exist_ok=False)
    for name, data in [('calibration.json', payload), ('manifest.json', (json.dumps(manifest, indent=2) + '\n').encode())]:
        with os.fdopen(os.open(output / name, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600), 'wb') as stream:
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('calibration', type=Path)
    parser.add_argument('--serial', required=True)
    parser.add_argument('--camera', action='append', required=True)
    parser.add_argument('--imu-frame', choices=('original', 'accelerometer'), required=True)
    parser.add_argument('--noise', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    try:
        export(args.calibration, args.serial, args.camera, args.imu_frame, args.noise, args.output)
    except (OSError, ValueError, KeyError, TypeError, OverflowError, RecursionError) as error:
        parser.exit(1, str(error) + '\n')
    print('Quest calibration candidate exported; hardware identity, rate, noise and clock remain unverified.')


if __name__ == '__main__':
    main()
