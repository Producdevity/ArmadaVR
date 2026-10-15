#!/usr/bin/env python3
"""Compare explicit Quest geometry with Basalt's parser, projection and native IMU frames."""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def load(name, file):
    spec = importlib.util.spec_from_file_location(name, ROOT / 'tools' / file)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('--calibration', type=Path, required=True)
    parser.add_argument('--serial', required=True)
    parser.add_argument('--camera', action='append', required=True)
    parser.add_argument('--noise', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--cc', default='cc')
    parser.add_argument('--cxx', default='c++')
    args = parser.parse_args()
    basalt = load('basalt_build', 'build-basalt.py')
    convert = load('quest_slam_calibration', 'quest-slam-calibration.py')
    source = args.source.resolve()
    profile = json.loads((ROOT / 'profiles/basalt.json').read_text())
    basalt.verify(source, profile)
    text = convert.read_private(args.calibration)
    noise_text = convert.read_private(args.noise)
    noise = convert.CAL.DECODER.decode(noise_text)
    for frame in ('original', 'accelerometer'):
        convert.convert(text, args.serial, args.camera, frame, noise)
    output = args.output.resolve()
    output.mkdir(mode=0o700, exist_ok=False)
    report = {'status': 'started', 'device_actions': [], 'physical_tracking_verified': False,
              'source_tree_sha256': profile['source_tree_sha256'], 'commands': []}
    paths = [ROOT / p for p in ('tools/test-quest-slam-calibration.py', 'tools/quest-slam-calibration.py',
                               'tools/extract-quest-calibration.py', 'tests/quest-slam-calibration.cpp',
                               'src/fisheye62.c', 'src/fisheye62.h', 'src/imu-calibration.c', 'src/imu-calibration.h',
                               'patches/basalt/0005-stabilize-fisheye624.patch')]
    report['inputs'] = {str(p.relative_to(ROOT)): basalt.digest(p) for p in paths}
    def run(command, log, timeout=120):
        report['commands'].append(command)
        with (output / log).open('xb') as stream:
            subprocess.run(command, stdout=stream, stderr=subprocess.STDOUT, check=True, timeout=timeout,
                           env=dict(os.environ, UBSAN_OPTIONS='halt_on_error=1', ASAN_OPTIONS='detect_leaks=' + str(int(sys.platform.startswith('linux')))))
    try:
        for frame in ('original', 'accelerometer'):
            convert.export(args.calibration, args.serial, args.camera, frame, args.noise, output / frame)
        unit = convert.CAL.DECODER.decode(text)
        imu = unit['ImuCalibration']
        values = [*imu['Accelerometer']['RectificationMatrix'], *imu['Gyroscope']['RectificationMatrix'],
                  *imu['DeviceFromImu'], len(args.camera)]
        for ident in args.camera:
            camera = next(c for c in unit['CameraCalibration'] if c['Id'] == ident)
            values += [*camera['Projection']['Coefficients'], *camera['Distortion']['Coefficients'],
                       *camera['DeviceFromCamera'], *camera['ImageSize']]
        with os.fdopen(os.open(output / 'reference.txt', os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600), 'w') as stream:
            stream.write('\n'.join(format(v, '.17g') for v in values) + '\n')
        header = Path('thirdparty/basalt-headers/include/basalt/camera/fisheye624_camera.hpp')
        patched = output / 'source'
        (patched / header).parent.mkdir(parents=True)
        shutil.copyfile(source / header, patched / header)
        run(['patch', '--batch', '--fuzz=0', '--forward', '-p1', '-d', str(patched), '-i',
             str(ROOT / 'patches/basalt/0005-stabilize-fisheye624.patch')], 'patch.log')
        headers = source / 'thirdparty/basalt-headers'
        flags = ['-O1', '-g1', '-ffp-contract=off', '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-I', str(ROOT / 'src')]
        for name in ('fisheye62', 'imu-calibration'):
            run([args.cc, '-std=c11', *flags, '-c', str(ROOT / 'src' / (name + '.c')), '-o', str(output / (name + '.o'))], name + '-compile.log')
        command = [args.cxx, '-std=c++17', *flags, '-DSOPHUS_USE_BASIC_LOGGING', '-I', str(patched / 'thirdparty/basalt-headers/include')]
        for include in (headers / 'include', headers / 'thirdparty/eigen', headers / 'thirdparty/Sophus', headers / 'thirdparty/cereal/include'):
            command += ['-isystem', str(include)]
        binary = output / 'geometry-test'
        command += ['-Wall', '-Wextra', '-Werror', str(ROOT / 'tests/quest-slam-calibration.cpp'), str(output / 'fisheye62.o'), str(output / 'imu-calibration.o'), '-o', str(binary)]
        run(command, 'geometry-compile.log')
        for frame in ('original', 'accelerometer'):
            run([str(binary), str(output / frame / 'calibration.json'), str(output / 'reference.txt'), frame], frame + '.log', 60)
            expected = f'QUEST_BASALT_CALIBRATION_PASS cameras={len(args.camera)} projections={2001 * len(args.camera)} frame={frame}\n'
            if (output / (frame + '.log')).read_text() != expected:
                raise ValueError('Incomplete geometry comparison')
        report.update(status='passed', binary_sha256=basalt.digest(binary), projections_per_frame=2001 * len(args.camera))
    except BaseException as error:
        report.update(status='failed', error=str(error))
        raise
    finally:
        with os.fdopen(os.open(output / 'result.json', os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600), 'w') as stream:
            json.dump(report, stream, indent=2)
            stream.write('\n')
    print('Quest/Basalt geometry and native IMU frame comparison passed; physical tracking remains unverified.')


if __name__ == '__main__':
    main()
