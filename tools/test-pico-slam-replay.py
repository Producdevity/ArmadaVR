#!/usr/bin/env python3
"""Replay pinned reference camera/IMU data through Pico input and native Basalt."""
import argparse
import importlib.util
import json
from pathlib import Path, PurePosixPath
import re
import subprocess
import tarfile
import uuid
import zipfile

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('basalt_build', ROOT / 'tools/build-basalt.py')
basalt = importlib.util.module_from_spec(spec)
spec.loader.exec_module(basalt)
digest = basalt.digest


def trajectory_metrics(poses, reference):
    import numpy as np
    actual = np.genfromtxt(poses, delimiter=',', skip_header=1)
    target = np.genfromtxt(reference, delimiter=',', comments='#')
    if actual.ndim != 2 or actual.shape[1] != 8 or not np.isfinite(actual).all():
        raise ValueError('Invalid pose output')
    if not np.all(np.diff(actual[:, 0]) > 0) or not np.all(np.diff(target[:, 0]) > 0):
        raise ValueError('Nonmonotonic trajectory')
    if not np.allclose(np.linalg.norm(actual[:, 4:8], axis=1), 1, atol=0.001):
        raise ValueError('Invalid pose rotations')
    selected = actual[(actual[:, 0] >= target[0, 0]) & (actual[:, 0] <= target[-1, 0])]
    if len(selected) != len(actual):
        raise ValueError('Reference does not cover every output pose')
    positions = selected[:, 1:4]
    reference_positions = np.stack([np.interp(selected[:, 0], target[:, 0], target[:, i])
                                    for i in range(1, 4)], axis=1)
    centered = positions - positions.mean(axis=0)
    reference_centered = reference_positions - reference_positions.mean(axis=0)
    u, _, vt = np.linalg.svd(centered.T @ reference_centered)
    reflection = np.eye(3)
    reflection[2, 2] = np.linalg.det(u @ vt)
    residual = np.linalg.norm(centered @ (u @ reflection @ vt) - reference_centered, axis=1)
    return {'poses': len(actual), 'rigid_rmse_m': float(np.sqrt(np.mean(residual ** 2))),
            'max_error_m': float(residual.max()), 'scale_fitted': False}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('monado-archive', 'dataset-archive', 'basalt-source', 'basalt-build', 'output'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--image', required=True)
    parser.add_argument('--engine', default='docker')
    args = parser.parse_args()
    args.output = args.output.resolve()
    monado = json.loads((ROOT / 'profiles/monado.json').read_text())
    profile = json.loads((ROOT / 'profiles/basalt.json').read_text())
    dataset = json.loads((ROOT / 'profiles/slam-replay.json').read_text())
    basalt.verify(args.basalt_source, profile)
    basalt.verify_patches(profile)
    build = json.loads((args.basalt_build / 'build.json').read_text())
    prefix = args.basalt_build / basalt.PREFIX
    if (build['status'] != 'passed' or build['profile'] != profile or
            build['builder_sha256'] != digest(ROOT / 'tools/build-basalt.py') or
            build['stage_sha256'] != basalt.tree_digest(prefix)):
        raise ValueError('Basalt bundle differs from verified build')
    if digest(args.monado_archive) != monado['full_source']['sha256']:
        raise ValueError('Unexpected Monado archive')
    if args.dataset_archive.stat().st_size != dataset['size'] or digest(args.dataset_archive) != dataset['sha256']:
        raise ValueError('Unexpected reference dataset')
    if not re.fullmatch(r'sha256:[0-9a-f]{64}', args.image):
        raise ValueError('Use an immutable local build image ID')
    image = json.loads(subprocess.check_output([args.engine, 'image', 'inspect', args.image], text=True))[0]
    if image['Architecture'] != 'arm64':
        raise ValueError('Replay image must be ARM64')
    args.output.mkdir(parents=True, exist_ok=False)
    with tarfile.open(args.monado_archive) as stream:
        stream.extractall(args.output / 'monado', filter='data')
    monado_source = args.output / 'monado' / monado['full_source']['archive_root']
    include = monado_source / 'src/xrt/include'
    generated = args.output / 'include/xrt'
    generated.mkdir(parents=True)
    for name in ('xrt_config_build.h', 'xrt_config_have.h'):
        template = (include / ('xrt/' + name + '.cmake_in')).read_text()
        (generated / name).write_text(re.sub(r'^#cmakedefine .+$', '', template, flags=re.MULTILINE))
    with zipfile.ZipFile(args.dataset_archive) as stream:
        if sum(item.file_size for item in stream.infolist()) != dataset['uncompressed_size']:
            raise ValueError('Unexpected dataset contents')
        for item in stream.infolist():
            path = PurePosixPath(item.filename)
            if path.is_absolute() or '..' in path.parts or (item.external_attr >> 16) & 0o170000 == 0o120000:
                raise ValueError('Unsafe dataset entry')
        stream.extractall(args.output / 'dataset')
    reference = args.output / 'dataset' / dataset['archive_root']
    (args.output / 'reference.toml').write_text(
        'show-gui=0\ncam-calib="/source/' + dataset['calibration'] + '"\nconfig-path="/source/' +
        dataset['configuration'] + '"\nmarg-data=""\nprint-queue=0\nuse-double=0\ndeterministic=0\nnum-threads=1\n')
    (args.output / 'bounded.toml').write_text((args.output / 'reference.toml').read_text() + 'nonblocking-input=1\n')
    source_paths = ['tools/build-basalt.py', 'profiles/basalt.json', 'profiles/monado.json', 'src/pico-camera.c', 'src/pico-camera.h', 'src/pico-camera-capture.c', 'src/pico-camera-capture.h', 'src/pico-camera-stream.c', 'src/pico-camera-stream.h', 'src/pico-camera-events.c', 'src/pico-camera-events.h', 'src/pico-camera-control.h', 'src/pico-slam-imu.c', 'src/pico-slam-imu.h', 'src/ssc-calibration.h', 'src/ssc-imu.h', 'src/ssc-clock.h', 'src/pico-slam-input.c', 'src/pico-slam-input.h', 'src/pico-slam-tracking.c', 'src/pico-slam-tracking.h', 'src/pico-slam-session.c', 'src/pico-slam-session.h',
                    'src/pico-camera-transport.c', 'src/pico-camera-transport.h', 'src/pico-camera-request.c', 'src/pico-camera-request.h',
                    'src/pico-camera-control.c', 'src/pico-camera-isp.c', 'src/pico-camera-isp.h', 'src/pico-camera-buffer.h', 'src/ion-buffer.h',
                    'tests/pico-camera-replay.c', 'tests/pico-camera-replay.h', 'tests/pico-slam-replay.cpp', 'tests/basalt-lifecycle.cpp', 'tests/basalt-input.cpp', 'tools/test-pico-slam-replay.py', 'profiles/slam-replay.json']
    for part in ('sensor', 'sensor-setup', 'session', 'rig', 'ife', 'front-end', 'pipeline'):
        source_paths += ['src/pico-camera-' + part + suffix for suffix in ('.c', '.h')]
    for part in ('runtime', 'live'):
        source_paths += ['src/pico-slam-' + part + suffix for suffix in ('.c', '.h')]
    source_paths += ['src/ssc-imu-session.h', 'tests/pico-slam-live-replay.cpp']
    inputs = {name: digest(ROOT / name) for name in source_paths}
    script = '''set -e
flags="-O1 -g1 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all -I/repo/src -I/source/thirdparty -I/out/include -isystem /monado/src/xrt/include"
cc -std=c11 $flags -c /repo/src/pico-camera.c -o /out/camera.o
cc -std=c11 $flags -c /repo/src/pico-camera-capture.c -o /out/capture.o
cc -std=c11 $flags -c /repo/src/pico-camera-stream.c -o /out/stream.o
cc -std=c11 $flags -c /repo/src/pico-camera-events.c -o /out/events.o
for part in transport request control isp sensor sensor-setup session rig ife front-end pipeline; do
    cc -std=c11 $flags -c /repo/src/pico-camera-$part.c -o /out/camera-$part.o
done
cc -std=c11 $flags -c /repo/tests/pico-camera-replay.c -o /out/camera-replay.o
for part in runtime live; do
    cc -std=gnu11 $flags -pthread -c /repo/src/pico-slam-$part.c -o /out/slam-$part.o
done
cc -std=c11 $flags -pthread -c /repo/src/pico-slam-imu.c -o /out/imu-queue.o
cc -std=c11 $flags -c /repo/src/pico-slam-input.c -o /out/input.o
cc -std=c11 $flags -I/source/thirdparty -c /repo/src/pico-slam-session.c -o /out/session.o
mathflags="$flags -pthread -ffunction-sections -fdata-sections -isystem /monado/src/xrt/auxiliary -isystem /monado/src/external/cjson $(pkg-config --cflags eigen3)"
cc -std=gnu11 $mathflags -c /repo/src/pico-slam-tracking.c -o /out/tracking.o
for path in math/m_predict.c util/u_logging.c util/u_debug.c; do
    cc -std=gnu11 $mathflags -w -c /monado/src/xrt/auxiliary/$path -o /out/$(basename $path).o
done
for path in math/m_base.cpp math/m_relation_history.cpp math/m_quatexpmap.cpp; do
    c++ -std=c++17 $mathflags -w -c /monado/src/xrt/auxiliary/$path -o /out/$(basename $path).o
done
cc -std=gnu11 $mathflags -w -c /monado/src/external/cjson/cjson/cJSON.c -o /out/cJSON.o
c++ -std=c++17 $flags -pthread -Wl,--gc-sections -I/source/thirdparty/vit /repo/tests/pico-slam-replay.cpp /out/*.o $(pkg-config --cflags --libs opencv4) -L/opt/reframed-vr/basalt/lib64 -lbasalt -o /out/replay
c++ -std=c++17 $flags -pthread -Wl,--gc-sections -I/source/thirdparty/vit /repo/tests/pico-slam-live-replay.cpp /out/*.o $(pkg-config --cflags --libs opencv4) -L/opt/reframed-vr/basalt/lib64 -lbasalt -o /out/live-replay
c++ -std=c++17 $flags -I/opt/reframed-vr/basalt/include /repo/tests/basalt-lifecycle.cpp -L/opt/reframed-vr/basalt/lib64 -lbasalt -o /out/lifecycle
c++ -std=c++17 $flags -I/opt/reframed-vr/basalt/include /repo/tests/basalt-input.cpp -L/opt/reframed-vr/basalt/lib64 -lbasalt -o /out/input-test
export LD_LIBRARY_PATH=/opt/reframed-vr/basalt/lib64
export ASAN_OPTIONS=detect_leaks=1
/out/lifecycle /out/reference.toml > /out/lifecycle.log 2>&1
/out/input-test /out/bounded.toml > /out/input-test.log 2>&1
for n in 1 2; do
    /out/replay /dataset /out/reference.toml /out/poses-$n.csv $n > /out/replay-$n.log 2>&1
done
/out/replay /dataset /out/bounded.toml /out/poses-session.csv 0 > /out/session-replay.log 2>&1
/out/replay /dataset /out/bounded.toml /out/poses-native.csv 3 > /out/native-replay.log 2>&1
/out/replay /dataset /out/bounded.toml /out/poses-capture.csv 4 > /out/capture-replay.log 2>&1
/out/replay /dataset /out/bounded.toml /out/poses-stream.csv 5 > /out/stream-replay.log 2>&1
/out/replay /dataset /out/bounded.toml /out/poses-transport.csv 6 > /out/transport-replay.log 2>&1
/out/replay /dataset /out/bounded.toml /out/poses-queued-imu.csv 7 > /out/queued-imu-replay.log 2>&1
/out/live-replay /dataset /out/bounded.toml /out/poses-live.csv > /out/live-replay.log 2>&1
'''
    name = 'reframed-vio-' + uuid.uuid4().hex[:12]
    command = [args.engine, 'run', '--rm', '--name', name, '--pull', 'never', '--network', 'none', '--read-only',
               '--cap-drop', 'ALL', '--security-opt', 'no-new-privileges', '--memory', '4g', '--memory-swap', '4g',
               '--cpus', '2', '--pids-limit', '128', '--tmpfs', '/tmp:rw,exec,size=128m']
    for source, target, readonly in [(ROOT, '/repo', True), (args.basalt_source, '/source', True),
                                     (prefix, '/opt/reframed-vr/basalt', True), (monado_source, '/monado', True),
                                     (reference, '/dataset', True), (args.output, '/out', False)]:
        if ',' in str(source):
            raise ValueError('Container bind paths cannot contain commas')
        command += ['--mount', f'type=bind,src={source.resolve()},dst={target}' + (',readonly' if readonly else '')]
    command += ['--entrypoint', 'sh', args.image, '-c', script]
    report = {'status': 'started', 'command': command, 'sources': inputs, 'dataset': dataset,
              'basalt_build_sha256': digest(args.basalt_build / 'build.json'), 'image': image['Id'],
              'physical_tracking_verified': False}
    try:
        with (args.output / 'run.log').open('wb') as log:
            subprocess.run(command, check=True, stdout=log, stderr=subprocess.STDOUT, timeout=300)
        if 'BASALT_LIFECYCLE_PASS stopped=70' not in (args.output / 'lifecycle.log').read_text():
            raise ValueError('Estimator lifecycle checks did not finish')
        if 'BASALT_INPUT_PASS exhausted=6 malformed=24 stopped=30' not in (args.output / 'input-test.log').read_text():
            raise ValueError('Bounded input checks did not finish')
        results = []
        for index in (1, 2):
            log = (args.output / f'replay-{index}.log').read_text()
            if 'PICO_SLAM_REPLAY_PASS' not in log or 'PICO_SLAM_HEAD_POSES_PASS poses=184 stopped_invalid=1' not in log:
                raise ValueError('Replay did not finish')
            if f'PICO_SLAM_IMAGE_FORMAT_PASS bytes={index} padded=1 unaligned=1' not in log:
                raise ValueError('Image format replay did not finish')
            metrics = trajectory_metrics(args.output / f'poses-{index}.csv', reference / 'mav0/gt/data.csv')
            if (metrics['poses'] != dataset['poses'] or metrics['rigid_rmse_m'] > dataset['max_rigid_rmse_m'] or
                    metrics['max_error_m'] > dataset['max_rigid_error_m']):
                raise ValueError('Reference trajectory acceptance failed')
            results.append(metrics)
        if 'PICO_SLAM_SESSION_REPLAY_PASS poses=185 images=370 stopped_invalid=1' not in (args.output / 'session-replay.log').read_text():
            raise ValueError('Owned session replay did not finish')
        session_metrics = trajectory_metrics(args.output / 'poses-session.csv', reference / 'mav0/gt/data.csv')
        if (session_metrics['poses'] != dataset['poses'] or
                session_metrics['rigid_rmse_m'] > dataset['max_rigid_rmse_m'] or
                session_metrics['max_error_m'] > dataset['max_rigid_error_m']):
            raise ValueError('Owned session trajectory acceptance failed')
        native_log = (args.output / 'native-replay.log').read_text()
        if ('PICO_SLAM_SESSION_REPLAY_PASS poses=185 images=370 stopped_invalid=1' not in native_log or
                'PICO_SLAM_NATIVE_INPUT_PASS padded=1 unaligned=1 poisoned=1' not in native_log):
            raise ValueError('Native frame session replay did not finish')
        native_metrics = trajectory_metrics(args.output / 'poses-native.csv', reference / 'mav0/gt/data.csv')
        if (native_metrics['poses'] != dataset['poses'] or
                native_metrics['rigid_rmse_m'] > dataset['max_rigid_rmse_m'] or
                native_metrics['max_error_m'] > dataset['max_rigid_error_m']):
            raise ValueError('Native frame trajectory acceptance failed')
        capture_log = (args.output / 'capture-replay.log').read_text()
        if ('PICO_SLAM_SESSION_REPLAY_PASS poses=185 images=370 stopped_invalid=1' not in capture_log or
                'PICO_SLAM_CAPTURE_PASS buffers=372 simulated_fences=1 simulated_cache=1 poisoned=1 stopped_invalid=1' not in capture_log):
            raise ValueError('Capture-owner replay did not finish')
        capture_metrics = trajectory_metrics(args.output / 'poses-capture.csv', reference / 'mav0/gt/data.csv')
        if (capture_metrics['poses'] != dataset['poses'] or
                capture_metrics['rigid_rmse_m'] > dataset['max_rigid_rmse_m'] or
                capture_metrics['max_error_m'] > dataset['max_rigid_error_m']):
            raise ValueError('Capture-owner trajectory acceptance failed')
        stream_log = (args.output / 'stream-replay.log').read_text()
        if ('PICO_SLAM_SESSION_REPLAY_PASS poses=185 images=370 stopped_invalid=1' not in stream_log or
                'PICO_SLAM_STREAM_PASS simulated_events=1 sequence_wrap=1 per_request_timing=1' not in stream_log or
                'PICO_SLAM_CAPTURE_PASS buffers=372 simulated_fences=1 simulated_cache=1 poisoned=1 stopped_invalid=1' not in stream_log):
            raise ValueError('Event stream replay did not finish')
        stream_metrics = trajectory_metrics(args.output / 'poses-stream.csv', reference / 'mav0/gt/data.csv')
        if (stream_metrics['poses'] != dataset['poses'] or
                stream_metrics['rigid_rmse_m'] > dataset['max_rigid_rmse_m'] or
                stream_metrics['max_error_m'] > dataset['max_rigid_error_m']):
            raise ValueError('Event stream trajectory acceptance failed')
        transport_log = (args.output / 'transport-replay.log').read_text()
        if ('PICO_SLAM_SESSION_REPLAY_PASS poses=185 images=370 stopped_invalid=1' not in transport_log or
                'PICO_SLAM_TRANSPORT_PASS buffers=372 simulated_ioctls=1 simulated_dma=1 poisoned=1' not in transport_log):
            raise ValueError('Owned camera transport replay did not finish')
        transport_metrics = trajectory_metrics(args.output / 'poses-transport.csv', reference / 'mav0/gt/data.csv')
        if (transport_metrics['poses'] != dataset['poses'] or
                transport_metrics['rigid_rmse_m'] > dataset['max_rigid_rmse_m'] or
                transport_metrics['max_error_m'] > dataset['max_rigid_error_m']):
            raise ValueError('Owned camera transport trajectory acceptance failed')
        queued_log = (args.output / 'queued-imu-replay.log').read_text()
        if ('PICO_SLAM_QUEUED_IMU_PASS pairs=3467 synthetic_ssc=1 stopped_invalid=1' not in queued_log or
                'PICO_SLAM_TRANSPORT_PASS buffers=372 simulated_ioctls=1 simulated_dma=1 poisoned=1' not in queued_log):
            raise ValueError('SSC queue and camera transport replay did not finish')
        queued_metrics = trajectory_metrics(args.output / 'poses-queued-imu.csv', reference / 'mav0/gt/data.csv')
        if (queued_metrics['poses'] != dataset['poses'] or
                queued_metrics['rigid_rmse_m'] > dataset['max_rigid_rmse_m'] or
                queued_metrics['max_error_m'] > dataset['max_rigid_error_m']):
            raise ValueError('SSC queue and camera transport trajectory acceptance failed')
        live_log = (args.output / 'live-replay.log').read_text()
        if ('PICO_SLAM_LIVE_REPLAY_PASS poses=185 images=370 imu=3467' not in live_log or
                'buffers=372 startup_skipped=1 simulated_ioctls=1 simulated_ssc=1 stopped_invalid=1' not in live_log):
            raise ValueError('Continuous camera/SSC/SLAM replay did not finish')
        live_metrics = trajectory_metrics(args.output / 'poses-live.csv', reference / 'mav0/gt/data.csv')
        if (live_metrics['poses'] != dataset['poses'] or
                live_metrics['rigid_rmse_m'] > dataset['max_rigid_rmse_m'] or
                live_metrics['max_error_m'] > dataset['max_rigid_error_m']):
            raise ValueError('Continuous camera/SSC/SLAM trajectory acceptance failed')
        if inputs != {name: digest(ROOT / name) for name in source_paths}:
            raise ValueError('Replay sources changed during testing')
        if build['stage_sha256'] != basalt.tree_digest(prefix):
            raise ValueError('Basalt bundle changed during replay')
        basalt.verify(args.basalt_source, profile)
        report.update(status='passed', live_binary_sha256=digest(args.output / 'live-replay'), trajectories=results, session_trajectory=session_metrics, native_trajectory=native_metrics, capture_trajectory=capture_metrics, stream_trajectory=stream_metrics, transport_trajectory=transport_metrics, queued_imu_trajectory=queued_metrics, live_trajectory=live_metrics, lifecycle_cycles=70, bounded_input_cycles=30,
                      input_test_binary_sha256=digest(args.output / 'input-test'),
                      lifecycle_binary_sha256=digest(args.output / 'lifecycle'),
                      binary_sha256=digest(args.output / 'replay'),
                      output_sha256={path.name: digest(path) for path in args.output.glob('poses-*.csv')})
    except BaseException as error:
        report.update(status='failed', error=str(error))
        with (args.output / 'cleanup.log').open('wb') as log:
            try:
                subprocess.run([args.engine, 'rm', '-f', name], stdout=log, stderr=subprocess.STDOUT, timeout=30)
            except (OSError, subprocess.TimeoutExpired):
                pass
        raise
    finally:
        (args.output / 'acceptance.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(results, indent=2))


if __name__ == '__main__':
    main()
