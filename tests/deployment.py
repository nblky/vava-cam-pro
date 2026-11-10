#!/usr/bin/env python3
"""Exercise the real server launcher and private provisioning, without hardware."""
import http.client
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
SERVER = Path(sys.argv[1]).resolve()
sys.argv = sys.argv[:1]

def port(kind):
    with socket.socket(type=kind) as sock:
        sock.bind(('127.0.0.1', 0))
        return sock.getsockname()[1]

class DeploymentTests(unittest.TestCase):
    def test_private_bundle_real_launcher_login_and_no_overwrite(self):
        with tempfile.TemporaryDirectory(prefix='vl-deploy-', dir='/tmp') as temp:
            bundle = Path(temp)/'private bundle'
            command = [sys.executable, str(ROOT/'tools/provision_server.py'), str(bundle), '--device-id', 'TEST_BASE', '--server-binary', str(SERVER)]
            result = subprocess.run(command, capture_output=True, text=True, check=True)
            metadata = json.loads(result.stdout)
            self.assertFalse(metadata['services_changed'])
            secret = (bundle/'agent/server-key').read_text().strip()
            self.assertNotIn(secret, result.stdout)
            for path in ('config/devices.keys', 'config/initial-admin-password', 'agent/server-key'):
                self.assertEqual((bundle/path).stat().st_mode & 0o777, 0o600)
            self.assertNotEqual(subprocess.run(command, capture_output=True).returncode, 0)
            web = port(socket.SOCK_STREAM)
            env = dict(os.environ, VAVA_SERVER_BIN=str(SERVER), VAVA_DATA_DIR=str(bundle/'data'),
                       VAVA_DEVICE_KEYS=str(bundle/'config/devices.keys'), VAVA_ADMIN_PASSWORD_FILE=str(bundle/'config/initial-admin-password'),
                       VAVA_WEB_DIR=str(ROOT/'vava-server/web'), VAVA_WEB_BIND='127.0.0.1', VAVA_WEB_PORT=str(web),
                       VAVA_FFMPEG='/nonexistent-vava-test-ffmpeg',
                       VAVA_UDP_BIND='127.0.0.1', VAVA_UDP_PORT=str(port(socket.SOCK_DGRAM)))
            with open(Path(temp)/'server.log', 'wb') as log:
                process = subprocess.Popen([str(ROOT/'vava-server/deploy/run-server.sh')], env=env, stdout=log, stderr=log)
                try:
                    deadline = time.monotonic()+10
                    while True:
                        try:
                            conn = http.client.HTTPConnection('127.0.0.1', web, timeout=1)
                            conn.request('GET', '/')
                            response = conn.getresponse(); page = response.read(); conn.close()
                            self.assertEqual(response.status, 200)
                            self.assertIn(b'VAVA Home', page)
                            break
                        except OSError:
                            self.assertIsNone(process.poll(), (Path(temp)/'server.log').read_text())
                            if time.monotonic()>deadline:
                                raise
                            time.sleep(.1)
                    conn = http.client.HTTPConnection('127.0.0.1', web, timeout=5)
                    conn.request('POST', '/api/v1/login', json.dumps({'username':'admin','password':(bundle/'config/initial-admin-password').read_text().strip()}), {'Content-Type':'application/json'})
                    response = conn.getresponse(); payload = json.loads(response.read()); conn.close()
                    self.assertEqual(response.status, 200, payload)
                    self.assertTrue(payload['ok'])
                finally:
                    process.terminate();process.wait(timeout=10)
            env['VAVA_WEB_BIND']='0.0.0.0'
            denied = subprocess.run([str(ROOT/'vava-server/deploy/run-server.sh')], env=env, capture_output=True, timeout=3)
            self.assertNotEqual(denied.returncode,0)
            self.assertIn(b'TLS',denied.stderr)

if __name__ == '__main__':
    unittest.main()
