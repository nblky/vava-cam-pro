#!/usr/bin/env python3
import socket
import struct
import unittest
from sdk_clips import ClipTests,sdk

class LowPowerTests(ClipTests):
    def test_media_before_control_after_sleep_keeps_first_keyframe(self):
        self.stop_proc(self.station)
        neighbors=self.private('neighbors','IP address HW type Flags HW address Mask Device\n127.0.0.1 0x1 0x2 02:00:00:00:00:01 * lo\n')
        self.station_extra=['--neighbor-file',str(neighbors)]
        self.station=self.start_station()
        self.connect_camera();self.camera.auto_media=False
        self.assertTrue(sdk.request(self.station_socket,'policy-config 0 mode=on camera-duration=1 confirm-detection=1')['ok'])
        self.camera.close();self.camera=None
        self.wait(lambda:not any(self.local_status()['devices'][0]['connections']))
        for _ in range(2):
            ports=self.local_status()['ports']
            av=socket.create_connection(('127.0.0.1',ports[1]),timeout=3);self.addCleanup(av.close)
            for ms,kind,payload in self.frames:
                av.sendall(struct.pack('<I8BIQ',0xeb000002,8 if kind==8 else 0,3 if kind==8 else 0,
                    kind,8 if kind==8 else 15,0,1,0,0,len(payload),1000000+ms)+payload)
            self.wait(lambda:self.local_status()['devices'][0]['connections'][1])
            ctrl=socket.create_connection(('127.0.0.1',ports[0]),timeout=3);self.addCleanup(ctrl.close)
            first=bytearray(sdk.camera_sim.decode(sdk.camera_sim.first('SIM-EVENT-0001'))[1]);first[19]=1
            ctrl.sendall(sdk.camera_sim.control(5,first)+sdk.camera_sim.control(42,struct.pack('<5I',0,2,20,10,1))+
                         sdk.camera_sim.control(200,struct.pack('<2I',1,5)))
            self.assertEqual(sdk.camera_sim.decode(sdk.camera_sim.read_exact(ctrl,128))[0],6)
            self.wait(lambda:self.local_status()['devices'][0]['recording']['frames']==15,3)
            record_id=self.local_status()['devices'][0]['recording']['id']
            rows=self.finish()['records']
            row=next(r for r in rows if r['id']==record_id)
            self.verify_clip(row,self.frames)
            ctrl.close();av.close()
            self.wait(lambda:not any(self.local_status()['devices'][0]['connections']))
    def prepare(self,confirm=True):
        self.connect_camera();self.camera.auto_media=False
        self.assertTrue(sdk.request(self.station_socket,f'if-serial SIM-EVENT-0001 policy-config 0 mode=on camera-duration=1 confirm-detection={int(confirm)}')['ok'])
        self.camera.ctrl.sendall(sdk.camera_sim.control(42,struct.pack('<5I',2,2,5,5,1)))
        self.wait(lambda:sdk.request(self.station_socket,'if-serial SIM-EVENT-0001 settings 0')['settings']['reports']['pir_config_42'])
        first=bytearray(sdk.camera_sim.decode(sdk.camera_sim.first('SIM-EVENT-0001'))[1]);first[19]=1
        self.camera.ctrl.sendall(sdk.camera_sim.control(5,first))
        self.wait(lambda:self.local_status()['devices'][0]['recording']['active'])
    def test_disconnect_before_first_frame_preserves_failure_reason(self):
        self.prepare();self.camera.close();self.camera=None
        self.wait(lambda:self.local_status()['devices'][0]['recording']['failed'])
        self.assertEqual(self.local_status()['devices'][0]['recording']['reason'],'control_disconnected')
        self.assertEqual(sdk.request(self.station_socket,'record-list 0')['total'],0)
    def detect(self,result=1,kind=5):
        before=self.local_status()['devices'][0]['detection_reports']
        self.camera.ctrl.sendall(sdk.camera_sim.control(200,struct.pack('<2I',result,kind)))
        self.wait(lambda:self.local_status()['devices'][0]['detection_reports']>before)
    def end_media(self):
        self.send_frames([(5700,1,self.frames[0][2])],stream=0)
        self.wait(lambda:not self.local_status()['devices'][0]['recording']['active'])
        self.wait(lambda:not self.local_status()['devices'][0]['recording']['finishing'])
    def test_no_confirmation_discards_and_report_alone_never_records(self):
        self.prepare();self.send_frames(self.frames,stream=0)
        self.wait(lambda:self.local_status()['devices'][0]['recording']['frames']==15)
        self.detect(0);self.end_media()
        self.assertEqual(sdk.request(self.station_socket,'if-serial SIM-EVENT-0001 record-list 0')['records'],[])
        self.assertEqual(self.local_status()['devices'][0]['recording']['reason'],'detection_not_confirmed')
        self.detect(1);self.assertFalse(self.local_status()['devices'][0]['recording']['active'])
        self.assertFalse(list((self.root/'station/recordings').glob('*.part')))
    def test_late_duplicate_detection_keeps_start_and_media_bound(self):
        self.prepare();self.send_frames(self.frames,stream=0)
        self.wait(lambda:self.local_status()['devices'][0]['recording']['frames']==15)
        self.detect();self.detect();state=self.local_status()['devices'][0]['recording']
        self.assertEqual(state['duration_seconds'],5);self.assertEqual(state['duration_source'],'camera_report');self.assertTrue(state['confirmed'])
        self.end_media();row=sdk.request(self.station_socket,'if-serial SIM-EVENT-0001 record-list 0')['records'][0]
        self.assertEqual(row['frames'],15);self.assertLess(row['last_timestamp_ms']-row['first_timestamp_ms'],5700)
        self.assertEqual(row['end_reason'],'media_duration_reached')
        self.wait(lambda:any(e['event'].get('related_id')==row['id'] and e['event'].get('result_raw')==1 for e in sdk.request(self.server_socket,'events BASE_A 0000000000000000')['events']))
    def test_unconfirmed_retention_is_explicit_and_settings_persist(self):
        self.prepare(confirm=False);self.send_frames(self.frames,stream=0)
        self.wait(lambda:self.local_status()['devices'][0]['recording']['frames']==15);self.end_media()
        self.assertEqual(len(sdk.request(self.station_socket,'if-serial SIM-EVENT-0001 record-list 0')['records']),1)
        self.camera.close();self.camera=None;self.stop_proc(self.station);self.station=self.start_station();self.wait(lambda:self.try_request(self.station_socket,'status'))
        policy=self.local_status()['devices'][0]['policy'];self.assertTrue(policy['camera_duration']);self.assertFalse(policy['confirm_detection'])
    def test_new_camera_daily_defaults_release_idle_connection(self):
        self.stop_proc(self.station);self.station_extra=['--diagnostic-defaults','0'];self.station=self.start_station();self.wait(lambda:self.try_request(self.station_socket,'status'))
        self.connect_camera();self.camera.auto_media=False
        policy=self.local_status()['devices'][0]['policy'];self.assertTrue(policy['managed']);self.assertEqual(policy['mode'],'on');self.assertEqual(policy['idle_seconds'],15);self.assertTrue(policy['confirm_detection']);self.assertTrue(policy['camera_duration'])
        self.wait(lambda:17 in self.camera.events)
        payload=next(p for cmd,p in reversed(self.camera.received_control) if cmd==17)
        self.camera.ctrl.sendall(sdk.camera_sim.control(17,payload))
        self.wait(lambda:403 in self.camera.events,25)

if __name__=='__main__':
    suite=unittest.TestSuite(LowPowerTests(n) for n in sorted(LowPowerTests.__dict__) if n.startswith('test_'))
    result=unittest.TextTestRunner(verbosity=2).run(suite)
    raise SystemExit(not result.wasSuccessful())
