#!/usr/bin/env python3
"""Own-DTLS file channel: captured policy, persistence, corruption and isolation."""
import hashlib
import json
import time
import unittest
from pathlib import Path
import sdk_clips as clips
sdk = clips.sdk


class UploadTests(clips.ClipTests):
    def setUp(self):
        super().setUp()
        self.files = self.root / 'remote-files'
        self.files.mkdir(mode=0o700)
        self.stop_proc(self.server)
        self.server_extra = ['--file-dir', str(self.files)]
        self.server = self.start_server()
        self.ready_remote()
        self.assertTrue(self.command('capabilities')['cloud_upload'])

    def ready_remote(self):
        self.online(45)
        self.wait(lambda: self.local_status()['server_connection']['online'], 45)

    def cloud(self, enabled):
        reply = sdk.request(self.station_socket, f'if-serial SIM-EVENT-0001 policy-config 0 cloud={int(enabled)}')
        self.assertTrue(reply['ok'], reply)
        return reply

    def pir_clip(self, cloud=True, frames=None, detection=None):
        self.connect_camera()
        self.camera.auto_media = False
        self.cloud(cloud)
        self.assertTrue(sdk.request(self.station_socket, 'if-serial SIM-EVENT-0001 arm 0 on 2')['ok'])
        first = bytearray(sdk.camera_sim.decode(sdk.camera_sim.first('SIM-EVENT-0001'))[1])
        first[19] = 1
        self.camera.ctrl.sendall(sdk.camera_sim.control(5, first))
        self.wait(lambda: self.local_status()['devices'][0]['recording']['active'])
        if detection is not None:
            self.report(*detection)
            self.wait(lambda: self.local_status()['devices'][0]['last_detection'] is not None)
        frames = frames or self.frames
        self.send_frames(frames, stream=0)
        expected = len([x for x in frames if x[1] != 8])
        self.wait(lambda: self.local_status()['devices'][0]['recording']['frames'] == expected)
        self.assertTrue(sdk.request(self.station_socket, 'if-serial SIM-EVENT-0001 arm 0 off')['ok'])
        return self.wait(lambda: sdk.request(self.station_socket, 'if-serial SIM-EVENT-0001 record-list 0')['records'])[0]

    def uploads(self):
        return sdk.request(self.station_socket, 'if-serial SIM-EVENT-0001 upload-list 0')['uploads']

    def remote(self):
        return sdk.request(self.server_socket, 'files BASE_A 0')['files']

    def committed(self, seconds=30):
        return self.wait(lambda: next((r for r in self.uploads() if r['state']=='committed'), None), seconds)

    def restart_station(self):
        if self.camera:
            self.camera.close(); self.camera = None
        self.stop_proc(self.station)
        self.station = self.start_station()
        self.wait(lambda: self.try_request(self.station_socket, 'status'))

    def test_pir_upload_has_complete_av_and_durable_receipt(self):
        row = self.pir_clip()
        done = self.committed()
        self.assertEqual(done['offset'], row['bytes'])
        remote = self.remote()
        self.assertEqual(len(remote), 1)
        self.assertEqual(remote[0]['state'], 'committed')
        path = self.files / ('BASE_A.' + row['id'] + '.ts')
        self.assertEqual(path.read_bytes(), self.verify_clip(row, self.frames).read_bytes())
        self.assertEqual(hashlib.sha256(path.read_bytes()).hexdigest(), row['sha256'])
        self.assertTrue((self.files / ('BASE_A.' + row['id'] + '.meta')).is_file())
        self.assertEqual(sdk.request(self.server_socket, 'files BASE_B 0')['files'], [])
        self.proxy.drop_all = True
        self.restart_station()
        self.committed()
        self.assertTrue(self.local_status()['devices'][0]['policy']['cloud_upload'])
        # A retained completed clip needs no server contact to remain completed.
        self.stop_proc(self.server)
        self.server = self.start_server()
        self.assertEqual(self.remote()[0]['state'], 'committed')

    def test_enable_does_not_retroactively_upload_or_upload_manual_clip(self):
        self.pir_clip(cloud=False)
        self.cloud(True)
        self.assertTrue(sdk.request(self.station_socket, 'if-serial SIM-EVENT-0001 record 0 start')['ok'])
        self.send_frames(self.frames)
        self.wait(lambda: self.local_status()['devices'][0]['recording']['frames']==15)
        self.finish()
        self.restart_station()
        self.wait(lambda: self.local_status()['devices'][0]['policy']['cloud_upload'])
        time.sleep(1.2)
        self.assertEqual(self.uploads(), [])
        self.assertEqual(self.remote(), [])

    def test_offline_queue_recovery_and_pause_resume(self):
        self.proxy.drop_all = True
        row = self.pir_clip()
        self.cloud(False)
        self.restart_station()
        paused = self.wait(lambda: self.uploads())
        self.assertEqual(paused[0]['state'], 'paused')
        self.proxy.drop_all = False
        self.ready_remote()
        self.assertEqual(self.remote(), [])
        self.cloud(True)
        self.committed(40)
        self.assertEqual(self.remote()[0]['sha256'], row['sha256'])

    def test_destination_change_quarantines_old_queue(self):
        self.proxy.drop_all = True
        self.pir_clip()
        self.stop_proc(self.station)
        if self.camera: self.camera.close(); self.camera = None
        # Same server/key but changed configured host spelling is intentionally
        # a different destination; no old file is silently retargeted.
        self.station_extra = ['--server-host', '127.0.0.1']
        self.station = self.start_station()
        self.wait(lambda: self.try_request(self.station_socket, 'status'))
        row = self.wait(lambda: self.uploads())[0]
        self.assertEqual(row['state'], 'destination_changed')
        self.proxy.drop_all = False
        self.ready_remote()
        self.assertEqual(self.remote(), [])

    def test_corrupt_source_never_commits_on_server(self):
        self.proxy.drop_all = True
        row = self.pir_clip()
        self.cloud(False)
        self.restart_station()
        source = self.root / 'station/recordings' / ('vrec-' + row['id'] + '.ts')
        data = bytearray(source.read_bytes()); data[500] ^= 1; source.write_bytes(data)
        self.proxy.drop_all = False
        self.ready_remote()
        self.cloud(True)
        failed = self.wait(lambda: next((r for r in self.uploads() if r['state']=='failed'), None), 40)
        self.assertEqual(failed['error'], 'server_digest_mismatch')
        self.assertFalse(list(self.files.glob('*.meta')))
        self.assertFalse(list(self.files.glob('*.ts')))
        self.assertEqual(self.remote()[0]['state'], 'receiving')
        self.assertTrue(self.command('if-serial SIM-EVENT-0001 settings 0')['ok'])

    def test_server_quota_does_not_break_control_or_local_recording(self):
        self.stop_proc(self.server)
        self.server_extra += ['--file-quota-mb', '1']
        self.server = self.start_server()
        self.ready_remote()
        frames = [(i*70, 1, self.frames[0][2] + b'\x55'*180000) for i in range(8)]
        row = self.pir_clip(frames=frames)
        self.assertGreater(row['bytes'], 1024*1024)
        self.wait(lambda: any(r['error']=='server_quota_full' for r in self.uploads()))
        self.assertEqual(self.remote(), [])
        self.assertTrue(self.command('if-serial SIM-EVENT-0001 configure 0 mirror 3')['ok'])
        self.wait(lambda: 11 in self.camera.events)

    def test_server_restart_mid_upload_resumes_without_duplicate(self):
        frames = [(i*70, 1, self.frames[0][2] + b'\x55'*15000) for i in range(6)]
        row = self.pir_clip(frames=frames)
        progress = self.wait(lambda: next((r for r in self.remote() if 0<r['offset']<r['bytes']), None))
        self.assertLess(progress['offset'], row['bytes'])
        before=time.monotonic()
        self.assertTrue(self.command('if-serial SIM-EVENT-0001 policy 0')['ok'])
        self.assertLess(time.monotonic()-before, 3)
        self.crash_server()
        self.server = self.start_server()
        self.committed(80)
        self.assertEqual(len(self.remote()), 1)
        self.assertEqual(hashlib.sha256((self.files / ('BASE_A.'+row['id']+'.ts')).read_bytes()).hexdigest(), row['sha256'])
        self.assertTrue(self.command('if-serial SIM-EVENT-0001 policy 0')['ok'])


if __name__ == '__main__':
    suite = unittest.TestSuite(UploadTests(name) for name in UploadTests.__dict__ if name.startswith('test_'))
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    raise SystemExit(not result.wasSuccessful())
