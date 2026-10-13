#!/usr/bin/env python3
"""Verify inventory over actual QRTR without allowing sensor enable messages."""
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
        'armada.ssc_test=1' not in Path('/proc/cmdline').read_text().split()):
    raise SystemExit('This test requires the isolated SSC VM')
os.environ['LD_LIBRARY_PATH'] = '/opt/armada-vr/ssc/lib64'
os.environ['UBSAN_OPTIONS'] = 'halt_on_error=1'
taint = Path('/proc/sys/kernel/tainted').read_text()
cases = ['normal', 'empty', 'duplicate', 'zero', 'too-many', 'wrong-type', 'registry-empty',
         'malformed-suid', 'malformed-attr', 'duplicate-attr', 'nan', 'missing-attr',
         'wrong-uid', 'late-ack', 'no-ack', 'server-loss', 'error-ack', 'no-service']
nameserver = subprocess.Popen(['/qrtr-ns', '-f'])
time.sleep(0.3)
assert nameserver.poll() is None
try:
    for case in cases:
        server = None
        runs = []
        with tempfile.TemporaryFile() as log:
            try:
                if case != 'no-service':
                    server = subprocess.Popen(['/usr/bin/python3', '-u', '/inventory-mock.py', '/output/build', case],
                                              stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
                    deadline = time.monotonic() + 10
                    while True:
                        assert server.poll() is None
                        found = subprocess.run(['/qrtr-lookup', '400'], capture_output=True, text=True, timeout=3)
                        assert found.returncode == 0
                        if re.search(r'^\s*400\s+1\s+0\s+', found.stdout, re.M):
                            break
                        assert time.monotonic() < deadline
                        time.sleep(0.05)
                for datatype in (['accel', 'gyro', 'accel_cal', 'gyro_cal', 'ble_mcu', 'ndi_fpga']
                                 if case == 'normal' else ['accel']):
                    start = time.monotonic()
                    result = subprocess.run(['/ssc-inventory', '--collect', datatype],
                                            capture_output=True, text=True, timeout=35)
                    runs.append({'type': datatype, 'code': result.returncode, 'stdout': result.stdout,
                                 'stderr': result.stderr, 'seconds': time.monotonic() - start})
            finally:
                if server:
                    try:
                        os.killpg(server.pid, signal.SIGTERM)
                    except ProcessLookupError:
                        pass
                    server.wait(timeout=3)
                log.seek(0)
                mock = log.read().decode(errors='replace')
                report = {'case': case, 'runs': runs, 'mock': mock,
                          'server_code': server.returncode if server else None}
                encoded = base64.b64encode(zlib.compress(json.dumps(report).encode())).decode()
                for i in range(0, len(encoded), 512):
                    print('SSC_DATA', case, i // 512, encoded[i:i + 512], flush=True)
            assert 'Traceback' not in mock, mock
            requests = [json.loads(line.split(' ', 1)[1]) for line in mock.splitlines()
                        if line.startswith('SSC_INVENTORY_REQUEST ')]
            assert all(r['id'] in (512, 1) for r in requests)
            for run in runs:
                success = case in ('normal', 'empty', 'late-ack')
                assert run['code'] == (0 if success else 1), run
                if not success:
                    assert run['stdout'] == '', run
                    if case == 'error-ack':
                        assert 'QMI protocol error' in run['stderr'], run
                    continue
                data = json.loads(run['stdout'])
                assert data['schema'] == 1 and data['data_type'] == run['type']
                assert len(data['sensors']) == (0 if case == 'empty' else 2)
                for i, sensor in enumerate(data['sensors']):
                    assert int(sensor['suid_high'], 16) == 0xACC10001 + i
                    assert int(sensor['suid_low'], 16) == 0xACC20001 + i
                    attrs = {a['id']: a['values'] for a in sensor['attributes']}
                    assert attrs == {0: ['candidate\n"é'], 1: ['fixture'], 3: [True],
                                     6: [5, 1000, 2000], 16: [{'fixed64': '0'}],
                                     19: [{'fixed64': '0'}], 20: [[1, 0, -1]],
                                     999: [{'fixed64': str(2**64 - 1)}]}
            if case == 'normal':
                assert [r['id'] for r in requests] == [512, 512, 1, 1] * 6
            if case == 'late-ack':
                assert [r['id'] for r in requests] == [512, 512, 1, 1]
            print('SSC_CASE', case, 'exit 0', flush=True)
    assert Path('/proc/sys/kernel/tainted').read_text() == taint
    print('SSC_INVENTORY_TAINT_UNCHANGED', taint.strip(), flush=True)
    print('SSC_VM_TESTS_PASS suites=18', flush=True)
finally:
    nameserver.terminate()
    nameserver.wait(timeout=3)
