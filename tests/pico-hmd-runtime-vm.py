#!/usr/bin/env python3
"""Exercise the packaged Pico HMD using synthetic calibration and QRTR data."""
import base64
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import tempfile
import time
import zlib

if (Path('/proc/device-tree/compatible').read_bytes().split(b'\0')[0] != b'linux,dummy-virt' or
        'reframed.ssc_test=1' not in Path('/proc/cmdline').read_text().split()):
    raise SystemExit('This test requires the isolated SSC VM')

config = {'schema_version': 1, 'device': 'PICOA7H10', 'serial': 'synthetic-hmd',
          'screen': {'width': 2048, 'height': 1024, 'frame_interval_ns': 11111111},
          'views': [{'viewport': [i * 1024, 0, 1024, 1024], 'display': [1024, 1024],
                     'rotation': [1, 0, 0, 1], 'fov': [-0.7, 0.7, 0.65, -0.65],
                     'eye_pose': [0, 0, 0, 1, 0.032 if i else -0.032, 0, 0],
                     'grid_width': 2, 'grid_height': 2,
                     'distortion': [[u, v, u, v, u, v] for v in (0, 1) for u in (0, 1)]} for i in range(2)],
          'imu': {'suids': [[f'{0xACC10001+i:016x}', f'{0xACC20001+i:016x}'] for i in range(4)],
                  'sample_rates': [100, 100], 'sensor_to_head': [1, 0, 0, 0, 1, 0, 0, 0, 1],
                  'acceleration_scale': 1, 'angular_velocity_scale': 1, 'max_age_ns': 500000000,
                  'max_gap_ns': 350000000, 'minimum_accuracy': 3, 'startup_timeout_ns': 15000000000,
                  'report_timeout_ns': 1000000000, 'bias_max_age_ns': 1000000000}}
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
           XRT_COMPOSITOR_NULL='1', XRT_NO_STDIN='1', PICO_HMD_CONFIG='/synthetic-hmd.json',
           PICO_HMD_COUNTER='1', PICO_SSC_CONFIG='/synthetic-controller.json', PICO_SSC_COUNTER='1', P_LOG='info', IPC_LOG='info', LP_NUM_THREADS='1',
           XRT_SERVICE_AUTO_START='0')
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
        for cycle in range(2):
            service = client = None
            with tempfile.TemporaryFile() as servicelog, tempfile.TemporaryFile() as clientlog:
                try:
                    Path('/hmd-phase').write_text('running')
                    Path('/controller-phase').write_text('release')
                    service = subprocess.Popen(['/opt/reframed-vr/monado/bin/monado-service'],
                                               env=env, stdout=servicelog, stderr=subprocess.STDOUT, preexec_fn=service_user)
                    deadline = time.monotonic() + 15
                    while not Path(env['XDG_RUNTIME_DIR'], 'monado_comp_ipc').exists():
                        assert service.poll() is None, 'Monado exited before IPC readiness'
                        assert time.monotonic() < deadline, 'IPC socket timed out'
                        time.sleep(0.05)
                    for command in (['/pico-monado-openxr', '--retain-stream'], ['/pico-hmd-openxr'], ['/pico-monado-openxr']):
                        client = subprocess.Popen(command, env=env, stdout=clientlog, stderr=subprocess.STDOUT)
                        assert client.wait(timeout=90) == 0, 'OpenXR check failed: ' + command[0]
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
                    report = {'mock': mocklog.read().decode(errors='replace')[-20000:], 'cycle': cycle, 'client_exit': client.returncode if client else None,
                              'service_exit': service.returncode if service else None,
                              'client': clientlog.read().decode(errors='replace'),
                              'service': servicelog.read().decode(errors='replace')}
                    encoded = base64.b64encode(zlib.compress(json.dumps(report).encode())).decode()
                    for offset in range(0, len(encoded), 512):
                        print('PICO_RUNTIME_DATA', cycle, offset // 512, encoded[offset:offset + 512], flush=True)
                print('PICO_RUNTIME_CYCLE_PASS', cycle, flush=True)
        print('PICO_RUNTIME_VM_PASS cycles=2', flush=True)
finally:
    if server:
        try:
            os.killpg(server.pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
        server.wait(timeout=5)
    nameserver.terminate()
    nameserver.wait(timeout=5)
