#!/usr/bin/env python3
"""Exercise the controller session on an isolated Pico-kernel QRTR test VM."""
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

cases = ['normal', 'immediate', 'no-service', 'clock-failure', 'late', 'silent', 'malformed', 'server-loss']
os.environ['LD_LIBRARY_PATH'] = '/opt/reframed-vr/ssc/lib64:' + os.environ.get('LD_LIBRARY_PATH', '')
assert subprocess.check_output(['/opt/reframed-vr/ssc/bin/ssccli', '--version'], text=True).strip() == 'libssc version 0.4.4'
print('SSC_BUNDLE_CLI_PASS', flush=True)
nameserver = subprocess.Popen(['/qrtr-ns', '-f'])
time.sleep(0.3)
assert nameserver.poll() is None

try:
    for name in cases:
        server = client = None
        with tempfile.TemporaryFile() as mocklog, tempfile.TemporaryFile() as clientlog:
            try:
                if name != 'no-service':
                    command = ['/usr/bin/python3', '/controller-mock.py', '/output/build', '/controller-vectors.c']
                    if name in ('late', 'silent', 'malformed'):
                        command.append(name)
                    server = subprocess.Popen(command, stdout=mocklog, stderr=subprocess.STDOUT, start_new_session=True)
                    deadline = time.monotonic() + 15
                    while True:
                        assert server.poll() is None, 'SSC server exited before registration'
                        lookup = subprocess.run(['/qrtr-lookup', '400'], capture_output=True, text=True, timeout=3)
                        assert lookup.returncode == 0, lookup.stderr
                        if re.search(r'^\s*400\s+1\s+0\s+', lookup.stdout, re.M):
                            break
                        assert time.monotonic() < deadline, 'SSC registration timed out'
                        time.sleep(0.1)
                client = subprocess.Popen(['/ssc-session', name], stdout=clientlog, stderr=subprocess.STDOUT)
                if name == 'server-loss':
                    deadline = time.monotonic() + 10
                    while True:
                        clientlog.seek(0)
                        if b'SSC_SESSION_STREAMING' in clientlog.read():
                            break
                        assert client.poll() is None, 'Client exited before streaming'
                        assert time.monotonic() < deadline, 'Client did not stream'
                        time.sleep(0.05)
                    os.killpg(server.pid, signal.SIGTERM)
                    server.wait(timeout=3)
                code = client.wait(timeout=40)
            finally:
                if client and client.poll() is None:
                    client.terminate()
                    client.wait(timeout=3)
                if server:
                    try:
                        os.killpg(server.pid, signal.SIGTERM)
                    except ProcessLookupError:
                        pass
                    server.wait(timeout=3)
                clientlog.seek(0)
                mocklog.seek(0)
                report = json.dumps({'name': name, 'returncode': client.returncode if client else None,
                                     'test': clientlog.read().decode(errors='replace'),
                                     'mock': mocklog.read().decode(errors='replace')}).encode()
                encoded = base64.b64encode(zlib.compress(report)).decode()
                for offset in range(0, len(encoded), 512):
                    print('SSC_DATA', name, offset // 512, encoded[offset:offset + 512], flush=True)
            print('SSC_CASE', name, 'exit', code, flush=True)
            assert code == 0
    print('SSC_VM_TESTS_PASS suites=8', flush=True)
finally:
    nameserver.terminate()
    nameserver.wait(timeout=3)
