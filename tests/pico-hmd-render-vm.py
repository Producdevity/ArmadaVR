#!/usr/bin/env python3
"""Exercise the packaged Pico HMD using synthetic calibration and QRTR data."""
import base64
import json
import math
import os
from pathlib import Path
import re
import signal
import struct
import hashlib
import subprocess
import tempfile
import time
import zlib

if (Path('/proc/device-tree/compatible').read_bytes().split(b'\0')[0] != b'linux,dummy-virt' or
        'reframed.ssc_test=1' not in Path('/proc/cmdline').read_text().split()):
    raise SystemExit('This test requires the isolated SSC VM')

analytic = os.environ.get('PICO_HMD_TEST_ANALYTIC') == '1'
rotated = os.environ.get('PICO_HMD_TEST_ROTATED') == '1'
if rotated and not analytic:
    raise SystemExit('Rotated-view checks require analytic calibration')

config = {'schema_version': 1, 'device': 'PICOA7H10', 'serial': 'synthetic-hmd',
          'screen': {'width': 512, 'height': 256, 'frame_interval_ns': 100000000},
          'views': [{'viewport': [i * 256, 0, 256, 256], 'display': [256, 256],
                     'rotation': [1, 0, 0, 1], 'fov': [-0.7, 0.7, 0.65, -0.65],
                     'eye_pose': [0, 0, 0, 1, 0.032 if i else -0.032, 0, 0],
                     'grid_width': 2, 'grid_height': 2,
                     'distortion': [[u, v, u, v, u, v] for v in (0, 1) for u in (0, 1)]} for i in range(2)],
          'imu': {'suids': [[f'{0xACC10001+i:016x}', f'{0xACC20001+i:016x}'] for i in range(4)],
                  'sample_rates': [1000, 1000], 'sensor_to_head': [1, 0, 0, 0, 1, 0, 0, 0, 1],
                  'acceleration_scale': 1, 'angular_velocity_scale': 1, 'max_age_ns': 500000000,
                  'max_gap_ns': 350000000, 'minimum_accuracy': 3, 'startup_timeout_ns': 15000000000,
                  'report_timeout_ns': 1000000000, 'bias_max_age_ns': 1000000000}}
if rotated:
    config['views'][0]['rotation'] = [0, 1, -1, 0]
    config['views'][1]['rotation'] = [0, -1, 1, 0]
Path('/synthetic-hmd.json').write_text(json.dumps(config))
Path('/hmd-phase').write_text('running')
controller = {'status': 'passed', 'read_only': True, 'device': 'PICOA7H10',
              'fingerprint': 'Pico/A7H10/PICOA7H10:10/5.8.4.0/smartcm.1696861506:user/test-keys',
              'controller': {'mcu_type': 1, 'radio_type': 2, 'touch_supported': True}}
Path('/synthetic-controller.json').write_text(json.dumps(controller))
Path('/controller-phase').write_text('release')
os.environ['SSC_TEST_HMD_RUNTIME'] = '1'
Path('/tmp/pico-runtime').mkdir(mode=0o700, parents=True)
env = dict(os.environ, LD_LIBRARY_PATH='/opt/reframed-vr/monado/lib64:/opt/reframed-vr/ssc/lib64:/usr/lib64',
           XDG_RUNTIME_DIR='/tmp/pico-runtime', XDG_CONFIG_HOME='/tmp/pico-config',
           XR_RUNTIME_JSON='/opt/reframed-vr/monado/share/openxr/1/openxr_monado.json',
           VK_ICD_FILENAMES='/usr/share/vulkan/icd.d/lvp_icd.aarch64.json',
           DISPLAY=':99', XRT_COMPOSITOR_FORCE_XCB='1', XRT_COMPOSITOR_SCALE_PERCENTAGE='100', XRT_NO_STDIN='1', PICO_HMD_CONFIG='/synthetic-hmd.json',
           PICO_HMD_COUNTER='1', PICO_SSC_CONFIG='/synthetic-controller.json', PICO_SSC_COUNTER='1', P_LOG='info', IPC_LOG='info', LP_NUM_THREADS='1',
           XRT_SERVICE_AUTO_START='0')
Path('/var/lib/xkb').mkdir(parents=True, exist_ok=True)
Path('/framebuffer').mkdir()
xvfb_log = tempfile.TemporaryFile()
xvfb = subprocess.Popen(['/usr/bin/Xvfb', ':99', '-screen', '0', '512x256x24', '-nolisten', 'tcp', '-noreset', '-fbdir', '/framebuffer'], stdout=xvfb_log, stderr=subprocess.STDOUT)
for _ in range(100):
    if subprocess.run(['/usr/bin/xdpyinfo'], env=dict(os.environ, DISPLAY=':99'), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 0: break
    if xvfb.poll() is not None:
        xvfb_log.seek(0)
        raise AssertionError('Xvfb exited: ' + xvfb_log.read().decode(errors='replace'))
    time.sleep(0.05)
else: raise AssertionError('Xvfb not ready')
os.chown('/tmp/pico-runtime', 1000, 1000)

def service_user():
    os.setgroups([])
    os.setgid(1000)
    os.setuid(1000)

nameserver = subprocess.Popen(['/qrtr-ns', '-f'])
server = None
try:
    with tempfile.TemporaryFile() as mocklog:
        server = subprocess.Popen(['/usr/bin/python3', '/imu-mock.py', '/output/build', 'bias-normal'],
                                  stdout=mocklog, stderr=subprocess.STDOUT, start_new_session=True)
        deadline = time.monotonic() + 15
        while True:
            assert nameserver.poll() is None and server.poll() is None, 'QRTR service exited'
            lookup = subprocess.run(['/qrtr-lookup', '400'], capture_output=True, text=True, timeout=3)
            assert lookup.returncode == 0, lookup.stderr
            if re.search(r'^\s*400\s+1\s+0\s+', lookup.stdout, re.M):
                break
            assert time.monotonic() < deadline, 'SSC registration timed out'
            time.sleep(0.1)
        for cycle in range(4):
            env['XRT_COMPOSITOR_COMPUTE'] = str(cycle % 2)
            for eye, view in enumerate(config['views']):
                view['distortion'] = [[u * (2 if cycle >= 2 and eye == 0 else 1), v, u, v,
                                       u * (2 if cycle >= 2 and eye == 1 else 1), v]
                                      for v in (0, 1) for u in (0, 1)]
            if analytic:
                config['schema_version'] = 2
                for eye, view in enumerate(config['views']):
                    view.pop('grid_width', None); view.pop('grid_height', None); view.pop('distortion', None)
                    left, right, up, down = map(math.tan, view['fov'])
                    dx, dy = ((-.03, .05) if eye else (.04, -.06)) if cycle >= 2 else (0, 0)
                    view['analytic_distortion'] = {
                        'type': 'pico-radial-2', 'tangent_scale': [right-left, down-up],
                        'tangent_offset': [left+dx, up+dy],
                        'radial_coefficients': [1, .2 if cycle >= 2 else 0] + [0]*9,
                        'chromatic_coefficients': [1 if cycle >= 2 and eye == 0 else 0, 0,
                                                   1 if cycle >= 2 and eye == 1 else 0, 0, 0, 0]}
            Path('/synthetic-hmd.json').write_text(json.dumps(config))
            service = client = None
            with tempfile.TemporaryFile() as servicelog, tempfile.TemporaryFile() as clientlog:
                try:
                    Path('/hmd-phase').write_text('running')
                    Path('/render-next').write_text('waiting')
                    Path('/render-ready').write_text('waiting')
                    frames = []
                    service = subprocess.Popen(['/opt/reframed-vr/monado/bin/monado-service'],
                                               env=env, stdout=servicelog, stderr=subprocess.STDOUT, preexec_fn=service_user)
                    deadline = time.monotonic() + 15
                    while not Path(env['XDG_RUNTIME_DIR'], 'monado_comp_ipc').exists():
                        assert service.poll() is None, 'Monado exited before IPC readiness'
                        assert time.monotonic() < deadline, 'IPC socket timed out'
                        time.sleep(0.05)
                    client = subprocess.Popen(['/pico-hmd-render'], env=env, stdout=clientlog, stderr=subprocess.STDOUT)
                    frames = []
                    for step in range(4):
                        deadline = time.monotonic() + 60
                        while Path('/render-ready').read_text() != str(step):
                            assert client.poll() is None and service.poll() is None, 'Rendering process exited'
                            assert time.monotonic() < deadline, 'Frame readiness timeout'
                            time.sleep(0.05)
                        pixel_deadline = time.monotonic() + 30
                        stable_since = None
                        stable_frames = 0
                        while True:
                            time.sleep(0.2)
                            raw = Path('/framebuffer/Xvfb_screen0').read_bytes()
                            fields = struct.unpack('>25I', raw[:100])
                            size, version, fmt, depth, width, height, xoff, order, unit, bitorder, pad, bpp, stride, visual, red, green, blue, *_ = fields
                            assert (version, fmt, depth, width, height, xoff, order, bpp, visual, red, green, blue) == (7,2,24,512,256,0,0,32,4,0xff0000,0xff00,0xff)
                            assert stride == width * 4
                            start = size + fields[19] * 12
                            pixels = raw[start:start+stride*height]
                            assert len(pixels) == stride * height
                            expected = [(231, 231 if step == 1 else 64, 64), (64, 231 if step == 2 else 64, 231)]
                            checks = []
                            for eye in range(2):
                                matched = sampled = 0
                                for y in range(16, 112):
                                    for x in range(eye * 128 + 16, eye * 128 + 112):
                                        wanted = list(expected[eye])
                                        if analytic:
                                            u, v = (x % 128 + .5) / 128, (y + .5) / 128
                                            if rotated:
                                                u, v = (1-v, u) if eye else (v, 1-u)
                                            left, right, up, down = map(math.tan, config['views'][eye]['fov'])
                                            dx, dy = ((-.03, .05) if eye else (.04, -.06)) if cycle >= 2 else (0, 0)
                                            tx, ty = left + u*(right-left) + dx, up - v*(up-down) + dy
                                            radial = 1 + (.2*(tx*tx+ty*ty) if cycle >= 2 else 0)
                                            near_edge = False
                                            for color in range(3):
                                                factor = radial * (2 if cycle >= 2 and color == (2 if eye else 0) else 1)
                                                su, sv = (tx*factor-left)/(right-left), (up-ty*factor)/(up-down)
                                                near_edge |= min(abs(su), abs(1-su), abs(sv), abs(1-sv)) < .04
                                                if not (0 <= su <= 1 and 0 <= sv <= 1):
                                                    wanted[color] = 0
                                            if near_edge:
                                                continue
                                        else:
                                            if cycle >= 2 and 60 <= x % 128 <= 68:
                                                continue
                                            if cycle >= 2 and x % 128 > 64:
                                                wanted[0 if eye == 0 else 2] = 0
                                        offset = y * stride + x * 4
                                        rgb = tuple(pixels[offset + c] for c in (2, 1, 0))
                                        matched += all(abs(a - b) <= 3 for a, b in zip(rgb, wanted))
                                        sampled += 1
                                checks.append({'eye': eye, 'expected_rgb': expected[eye], 'matching_pixels': matched,
                                               'sampled_pixels': sampled, 'passed': sampled >= 4000 and matched >= 0.95 * sampled})
                            frames.append({'step': step, 'xwd': base64.b64encode(zlib.compress(raw)).decode(),
                                           'sha256': hashlib.sha256(raw).hexdigest(), 'pixel_checks': checks})
                            if all(check['passed'] for check in checks):
                                if stable_since is None:
                                    stable_since = time.monotonic()
                                stable_frames += 1
                                if stable_frames >= 4 and time.monotonic() - stable_since >= 1.5:
                                    frames[-1]['stable_frames'] = stable_frames
                                    frames[-1]['stable_seconds'] = time.monotonic() - stable_since
                                    break
                            else:
                                stable_since = None
                                stable_frames = 0
                            assert time.monotonic() < pixel_deadline, ('Eye framebuffer mismatch', step, checks)
                            frames.pop()
                        Path('/render-next').write_text(str(step))
                    assert client.wait(timeout=15) == 0, 'Rendered OpenXR check failed'
                    assert service.poll() is None, 'Monado exited during input check'
                    service.send_signal(signal.SIGINT)
                    assert service.wait(timeout=30) == 0, 'Monado did not shut down cleanly'
                finally:
                    for process in (client, service):
                        if process and process.poll() is None:
                            process.terminate()
                            try:
                                process.wait(timeout=5)
                            except subprocess.TimeoutExpired:
                                process.kill()
                                process.wait(timeout=5)
                    clientlog.seek(0)
                    servicelog.seek(0)
                    mocklog.seek(0)
                    report = {'mock': mocklog.read().decode(errors='replace')[-20000:], 'frames': frames, 'renderer': 'compute' if cycle % 2 else 'graphics', 'optics': ('analytic-radial-rgb' if cycle >= 2 else 'analytic-identity') if analytic else ('rgb-crop' if cycle >= 2 else 'identity'), 'view_rotations': [view['rotation'] for view in config['views']], 'cycle': cycle, 'client_exit': client.returncode if client else None,
                              'service_exit': service.returncode if service else None,
                              'client': clientlog.read().decode(errors='replace'),
                              'service': servicelog.read().decode(errors='replace')}
                    encoded = base64.b64encode(zlib.compress(json.dumps(report).encode())).decode()
                    for offset in range(0, len(encoded), 512):
                        print('PICO_RUNTIME_DATA', cycle, offset // 512, encoded[offset:offset + 512], flush=True)
                print('PICO_RUNTIME_CYCLE_PASS', cycle, flush=True)
        print('PICO_RUNTIME_VM_PASS cycles=4', flush=True)
finally:
    if server:
        try:
            os.killpg(server.pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
        server.wait(timeout=5)
    xvfb.terminate()
    xvfb.wait(timeout=5)
    xvfb_log.seek(0)
    print('XVFB_LOG', xvfb_log.read().decode(errors='replace'), flush=True)
    nameserver.terminate()
    nameserver.wait(timeout=5)
