#!/usr/bin/env python3
import secrets
import signal
import time
import unittest
from sdk_integration import SDKTests,request
class CredentialTests(SDKTests):
    def reload(self,text):
        self.private('devices.keys',text);self.server.send_signal(signal.SIGHUP)
    def devices(self):return request(self.server_socket,'devices')['devices']
    def test_revoke_all_and_restore_requires_new_authenticated_session(self):
        self.assertTrue(self.command('status')['ok'])
        self.reload('');self.wait(lambda:not self.devices())
        self.wait(lambda:not self.local_status()['server_connection']['online'],30)
        self.assertTrue(request(self.station_socket,'status')['ok'])
        self.reload('BASE_A '+self.secret+'\nBASE_B '+self.second_secret+'\n')
        self.online(60);self.assertTrue(self.command('status')['ok'])
    def test_bad_reload_preserves_registry_and_rotation_rejects_old_key(self):
        self.reload('BASE_A not-a-key\n');time.sleep(.1)
        self.assertEqual({d['id'] for d in self.devices()},{'BASE_A','BASE_B'})
        self.assertTrue(self.command('status')['ok'])
        new=secrets.token_hex(32);self.reload('BASE_A '+new+'\nBASE_B '+self.second_secret+'\n')
        self.wait(lambda:not next(d for d in self.devices() if d['id']=='BASE_A')['online'])
        self.wait(lambda:not self.local_status()['server_connection']['online'],30)
        self.stop_proc(self.station);self.private('device.key',new+'\n');self.station=self.start_station()
        self.wait(lambda:self.try_request(self.station_socket,'status'));self.online(60)
        self.assertTrue(self.command('status')['ok'])
if __name__=='__main__':
    result=unittest.TextTestRunner(verbosity=2).run(unittest.TestSuite(CredentialTests(n) for n in sorted(CredentialTests.__dict__) if n.startswith('test_')))
    raise SystemExit(not result.wasSuccessful())
