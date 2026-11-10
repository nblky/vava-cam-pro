#!/usr/bin/env python3
"""One DTLS media lease across all six camera quality transitions."""
import http.client
import json
import struct
import threading
import time
import unittest
from pathlib import Path
from sdk_live import LiveTests
from sdk_web import WebTests
from sdk_clips import sdk
from continuous_media import fixtures,verify,SEQUENCE

class ContinuousLiveTests(LiveTests):
    def setUp(self):
        self.station_extra=['--continuous-quality','1'];super().setUp()

    def test_same_lease_all_transitions_and_owner_guard(self):
        samples=fixtures(self.root/'media')
        original=sdk.camera_sim.first
        def verified_first(serial):
            first=bytearray(sdk.camera_sim.decode(original(serial))[1])
            first[32:41]=b'1.4.0.54\0';first[48:56]=b'VA-HS03\0';struct.pack_into('<I',first,28,8000)
            return sdk.camera_sim.control(5,first)
        sdk.camera_sim.first=verified_first
        self.addCleanup(setattr,sdk.camera_sim,'first',original)
        self.prepare()
        c=self.live(format='mse-v2');self.wait(lambda:self.local_status()['devices'][0]['viewers']==1)
        chunks=[];errors=[];lease=[];done=threading.Event();revoked=threading.Event()
        def reader():
            try:
                r=c.getresponse();self.assertEqual(r.status,200);lease.append(r.getheader('X-Myvava-Lease'))
                while not done.is_set():
                    value=r.read1(32768)
                    if not value:break
                    chunks.append(value)
            except http.client.IncompleteRead as e:
                if e.partial:chunks.append(e.partial)
                if not revoked.is_set():errors.append(e)
            except Exception as e:
                if not done.is_set():errors.append(e)
        thread=threading.Thread(target=reader);thread.start()
        try:
            baseline=None; starts=None
            for phase,q in enumerate(SEQUENCE):
                if phase:
                    self.wait(lambda:lease)
                    settings=self.command('if-serial SIM-EVENT-0001 settings 0');rev=settings['desired']['revision']
                    if phase==1:
                        self.user(self.token,'other','operator');other=self.login('other','Synthetic-User-Password')
                        envelope={'command':'live-quality','args':['0',lease[0],str(q)],'camera_serial':'SIM-EVENT-0001','expected_revision':rev}
                        code,_,_=WebTests.submit_web(self,other,envelope);self.assertEqual(code,403)
                    count=sum(cmd==10 for cmd,_ in self.camera.received_control)
                    result=self.command(f'if-serial SIM-EVENT-0001 if-revision {rev} live-quality 0 {lease[0]} {q}')
                    self.assertTrue(result['ok'],result)
                    if phase==1:
                        for _ in range(10):self.camera.step(.02)
                        self.assertEqual(sum(cmd==10 for cmd,_ in self.camera.received_control),count)
                        self.assertTrue(self.local_status()['devices'][0]['recording']['active'])
                        self.assertTrue(sdk.request(self.station_socket,'if-serial SIM-EVENT-0001 record 0 stop')['ok'])
                    self.wait(lambda:sum(cmd==10 for cmd,_ in self.camera.received_control)>count)
                    payload=[p for cmd,p in self.camera.received_control if cmd==10][-1]
                    self.assertEqual(struct.unpack_from('<4I',payload),(1,q,15,[1000,700,400][q]))
                    self.camera.ctrl.sendall(sdk.camera_sim.control(10,payload))
                    self.wait(lambda:not self.command('if-serial SIM-EVENT-0001 settings 0')['settings']['pending'])
                for ms,kind,payload,epoch in samples:
                    if epoch!=phase+1:continue
                    if phase==0 and ms==1000:
                        self.assertTrue(sdk.request(self.station_socket,'if-serial SIM-EVENT-0001 record 0 start')['ok'])
                    self.send_frames([(ms,kind,payload)]);self.camera.step(.035)
                d=self.local_status()['devices'][0]
                if baseline is None:baseline=d['generation'];starts=sum(cmd==102 for cmd,_ in self.camera.received_control)
                self.assertEqual(d['viewers'],1);self.assertTrue(d['live_requested']);self.assertEqual(d['generation'],baseline)
                self.assertEqual(sum(cmd==102 for cmd,_ in self.camera.received_control),starts)
            self.camera.step(.5);revoked.set();self.web('/api/v1/logout',{},self.token);thread.join(5)
            self.assertFalse(thread.is_alive());self.assertEqual(errors,[])
            self.wait(self.released,5)
            verify(self.root,b''.join(chunks),source_root=self.root/'media')
        finally:
            done.set();c.close();thread.join(5)

if __name__=='__main__':
    suite=unittest.TestSuite(ContinuousLiveTests(n) for n in ContinuousLiveTests.__dict__ if n.startswith('test_'))
    raise SystemExit(not unittest.TextTestRunner(verbosity=2).run(suite).wasSuccessful())
