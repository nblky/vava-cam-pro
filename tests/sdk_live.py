#!/usr/bin/env python3
"""Real DTLS live media, bounded viewer ownership, privacy and PIR independence."""
import http.client
import json
import socket
import struct
import time
import threading
import unittest
from sdk_web_media import WebMediaTests
from sdk_clips import sdk

class LiveTests(WebMediaTests):
    def prepare(self):
        self.connect_camera();self.camera.auto_media=False
        self.token=self.login()
        self.boot=self.web('/api/v1/devices',token=self.token)[1]['devices'][0]['boot']

    def live(self,format='ts',talk=False,token=None,serial='SIM-EVENT-0001',boot=None):
        c=http.client.HTTPConnection('127.0.0.1',self.web_port,timeout=20)
        c.request('POST','/api/v1/devices/BASE_A/live',json.dumps(dict(slot=0,serial=serial,boot=boot or self.boot,talk=talk,format=format)),
                  {'Authorization':'Bearer '+(token or self.token),'Content-Type':'application/json'})
        self.addCleanup(c.close)
        return c

    def send_live(self,cycles=2):
        for cycle in range(cycles):
            for ms,kind,payload in self.frames:
                self.send_frames([(cycle*1280+ms,kind,payload)])
                # Give media worker time to send complete access units before
                # the next frame, as a physical 15 fps / AAC camera would.
                time.sleep(.045)

    def released(self):
        return not self.local_status()['devices'][0]['viewers'] and not self.local_status()['devices'][0]['live_requested']

    def test_live_ts_and_mp4_and_disconnect_release(self):
        self.prepare()
        for format in ['ts','mp4']:
            c=self.live(format);self.wait(lambda:self.local_status()['devices'][0]['viewers']==1)
            self.send_live(3)
            r=c.getresponse();self.assertEqual(r.status,200);self.assertRegex(r.getheader('X-Myvava-Lease'),r'^[a-f0-9]{16}$')
            data=r.read1(32768);self.assertTrue(data)
            if format=='ts':self.assertEqual(data[0],0x47)
            else:self.assertIn(b'ftyp',data)
            # Incremental fragments are available while camera is still live.
            data+=r.read1(32768);self.assertTrue(self.command('status')['ok'])
            r.close();c.close();self.wait(self.released,5)
            target=self.root/('live.'+format);target.write_bytes(data)
            if format=='ts':
                decoded=self.probe(target);self.assertEqual({s['codec_name'] for s in decoded['streams']},{'h264','aac'})

    def test_batched_camera_audio_video_preserves_every_frame(self):
        self.prepare();c=self.live();self.wait(lambda:self.local_status()['devices'][0]['viewers']==1)
        received=[];failures=[];revoked=threading.Event()
        def read_stream():
            try:
                r=c.getresponse();self.assertEqual(r.status,200)
                while True:
                    data=r.read1(32768)
                    if not data:break
                    received.append(data)
            except http.client.IncompleteRead as e:
                # Revoking authorization deliberately closes the HTTP stream
                # without a successful terminating chunk.
                if e.partial:received.append(e.partial)
                if not revoked.is_set():failures.append(e)
            except Exception as e:failures.append(e)
        reader=threading.Thread(target=read_stream);reader.start()
        try:
            expected=[]
            for cycle in range(3):
                for start in range(0,len(self.frames),4):
                    batch=[]
                    for ms,kind,payload in self.frames[start:start+4]:
                        at=cycle*1280+ms;audio=kind==8;expected.append((at,audio))
                        batch.append(struct.pack('<I8BIQ',0xEB000002,9 if audio else 1,3 if audio else 0,
                            kind,8 if audio else 15,0,1,0,0,len(payload),1000000+at)+payload)
                    # Real TCP reads may contain several audio/video frames.
                    self.camera.av.sendall(b''.join(batch));self.camera.step(.1)
            self.camera.step(.5);revoked.set();self.web('/api/v1/logout',{},self.token);reader.join(5)
            self.assertFalse(reader.is_alive());self.assertEqual(failures,[])
            target=self.root/'batched-live.ts';target.write_bytes(b''.join(received));decoded=self.probe(target)
            for audio in (False,True):
                times=[ms for ms,a in expected if a==audio]
                packets=[p for p in decoded['packets'] if p['codec_type']==('audio' if audio else 'video')]
                self.assertEqual(len(packets),len(times))
                for packet,ms in zip(packets,times):self.assertAlmostEqual(float(packet['pts_time']),1+ms/1000,places=5)
        finally:
            c.close();reader.join(5)

    def test_corrupt_h264_waits_for_clean_keyframe(self):
        self.prepare();c=self.live();self.wait(lambda:self.local_status()['devices'][0]['viewers']==1)
        self.send_live(2);r=c.getresponse();self.assertEqual(r.status,200)
        before=self.local_status()['devices'][0]['live_media']
        bad=b'\x00\x00\x00\x01\x65\x80\x00\x00\x00\x22'
        dependent=b'\x00\x00\x00\x01\x41\x80'
        self.send_frames([(2700,1,bad),(2800,0,dependent)],stream=1)
        self.wait(lambda:self.local_status()['devices'][0]['live_media']['rejected_frames']==before['rejected_frames']+1)
        guarded=self.local_status()['devices'][0]['live_media']
        self.assertEqual(guarded['dependent_frames_skipped'],before['dependent_frames_skipped']+1)
        self.assertEqual(self.local_status()['devices'][0]['error'],'corrupt_h264_dropped')
        self.send_frames([(3000+ms,kind,payload) for ms,kind,payload in self.frames],stream=1)
        self.wait(lambda:self.local_status()['devices'][0]['error']=='')
        self.assertGreater(self.local_status()['devices'][0]['live_media']['width'],0)
        r.close();c.close();self.wait(self.released,5)

    def test_lost_server_releases_viewer_without_ending_pir(self):
        self.prepare();c=self.live();self.wait(lambda:self.local_status()['devices'][0]['viewers']==1)
        self.assertTrue(sdk.request(self.station_socket,'if-serial SIM-EVENT-0001 arm 0 on 2')['ok'])
        first=bytearray(sdk.camera_sim.decode(sdk.camera_sim.first('SIM-EVENT-0001'))[1]);first[19]=1
        self.camera.ctrl.sendall(sdk.camera_sim.control(5,first))
        self.wait(lambda:self.local_status()['devices'][0]['recording']['active'])
        self.send_frames(self.frames,stream=0)
        self.proxy.drop_all=True
        self.wait(self.released,6)
        recording=self.local_status()['devices'][0]['recording'];self.assertTrue(recording['active']);self.assertGreater(recording['frames'],0)
        self.assertTrue(sdk.request(self.station_socket,'if-serial SIM-EVENT-0001 arm 0 off')['ok'])
        row=self.wait(lambda:sdk.request(self.station_socket,'if-serial SIM-EVENT-0001 record-list 0')['records'])[0]
        self.verify_clip(row,self.frames);c.close()

    def test_stale_boot_wrong_serial_acl_and_exclusive_viewer(self):
        self.prepare();c=self.live(boot='f'*32);self.assertEqual(c.getresponse().status,409);c.close()
        c=self.live(serial='OTHER-CAMERA');time.sleep(.4);self.assertTrue(self.released());c.close()
        self.wait(self.released);time.sleep(.3)
        c=self.live();self.wait(lambda:self.local_status()['devices'][0]['viewers']==1)
        other=self.live();self.assertEqual(other.getresponse().status,409);other.close()
        self.user(self.token,'viewer','viewer');viewer=self.login('viewer','Synthetic-User-Password')
        other=self.live(talk=True,token=viewer);self.assertEqual(other.getresponse().status,403);other.close()
        self.web('/api/v1/logout',{},self.token);self.wait(self.released,5);c.close()

    def test_talk_authenticated_owner_aac_and_deduplicated_delivery(self):
        self.prepare()
        first=bytearray(sdk.camera_sim.decode(sdk.camera_sim.first('SIM-EVENT-0001'))[1])
        struct.pack_into('<I',first,28,8000)
        self.camera.ctrl.sendall(sdk.camera_sim.control(5,first))
        c=self.live(talk=True);self.wait(lambda:self.local_status()['devices'][0]['viewers']==1)
        self.send_live(1);r=c.getresponse();self.assertEqual(r.status,200);lease=r.getheader('X-Myvava-Lease')
        path='/api/v1/devices/BASE_A/live/'+lease+'/talk'
        aac=next(p for _,kind,p in self.frames if kind==8)
        self.assertEqual(self.web(path,{'aac':aac.hex()})[0],401)
        self.user(self.token,'operator','operator');operator=self.login('operator','Synthetic-User-Password')
        self.assertEqual(self.web(path,{'aac':aac.hex()},operator)[0],403)
        self.assertEqual(self.web(path,{'aac':'00'*40},self.token)[0],400)
        self.assertEqual(self.web(path,{'aac':aac.hex()},self.token)[0],200)
        self.wait(lambda:any(p[24:]==aac for p in self.camera.talk_packets),4)
        for _ in range(20):self.camera.step(.02)
        matches=[p for p in self.camera.talk_packets if p[24:]==aac]
        self.assertEqual(len(matches),1);self.assertEqual(matches[0][4:7],bytes([2,3,8]))
        r.close();c.close();self.wait(self.released,5)
        self.wait(lambda:self.web(path,{'aac':aac.hex()},self.token)[0]==404,3)

    def test_quality_waits_for_pir_commit_without_disarming(self):
        self.prepare()
        self.assertTrue(sdk.request(self.station_socket,'if-serial SIM-EVENT-0001 policy-config 0 mode=on level=2 camera-duration=0 record=5')['ok'])
        # Drain the arming request before introducing the independent quality job.
        self.wait(lambda:any(c==17 for c,_ in self.camera.received_control))
        payload=next(p for c,p in self.camera.received_control if c==17)
        self.camera.ctrl.sendall(sdk.camera_sim.control(17,payload))
        self.wait(lambda:not sdk.request(self.station_socket,'settings 0')['settings']['pending'])
        first=bytearray(sdk.camera_sim.decode(sdk.camera_sim.first('SIM-EVENT-0001'))[1]);first[19]=1
        self.camera.ctrl.sendall(sdk.camera_sim.control(5,first))
        self.wait(lambda:self.local_status()['devices'][0]['recording']['active'])
        self.send_frames(self.frames,stream=0)
        self.assertTrue(self.command('if-serial SIM-EVENT-0001 configure 0 quality 1')['ok'])
        for _ in range(10):self.camera.step(.02)
        self.assertFalse(any(c==10 for c,_ in self.camera.received_control))
        self.wait(lambda:not self.local_status()['devices'][0]['recording']['active'],8)
        self.wait(lambda:any(c==10 for c,_ in self.camera.received_control))
        self.assertFalse(self.local_status()['devices'][0]['recording']['finishing'])
        payload=next(p for c,p in self.camera.received_control if c==10)
        self.assertEqual(struct.unpack_from('<4I',payload),(1,1,15,700))
        self.camera.ctrl.sendall(sdk.camera_sim.control(10,payload))
        self.wait(lambda:sdk.request(self.station_socket,'settings 0')['desired']['items']['quality']['phase']=='echo_observed')
        self.assertTrue(self.local_status()['devices'][0]['policy']['desired_armed'])
        row=sdk.request(self.station_socket,'record-list 0')['records'][0]
        self.verify_clip(row,self.frames)

if __name__=='__main__':
    suite=unittest.TestSuite(LiveTests(n) for n in sorted(LiveTests.__dict__) if n.startswith('test_'))
    result=unittest.TextTestRunner(verbosity=2).run(suite)
    raise SystemExit(not result.wasSuccessful())
