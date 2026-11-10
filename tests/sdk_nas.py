#!/usr/bin/env python3
"""Base-owned NAS archive process; local filesystem mode for host validation."""
import hashlib
import subprocess
import time
import unittest
import sdk_uploads as uploads
sdk = uploads.sdk


class NasTests(uploads.UploadTests):
    def setUp(self):
        # NAS must work without any configured management/cloud connection.
        sdk.SDKTests.setUp(self)
        self.mount = self.root / 'nas-mount'
        self.mount.mkdir(mode=0o700)
        self.stop_proc(self.station)
        self.station_extra = ['--nas-host', 'nas.local', '--nas-export', '/archive',
            '--nas-mount', str(self.mount), '--nas-station-id', 'BASE_A', '--nas-lab-local', '1']
        self.station = self.start_station()
        self.wait(lambda: self.try_request(self.station_socket, 'status'))
        self.connect_camera()
        self.camera.auto_media = False
        self.assertTrue(sdk.request(self.station_socket, 'if-serial SIM-EVENT-0001 policy-config 0 nas=1')['ok'])

    def pir(self, nas=True):
        self.assertTrue(sdk.request(self.station_socket, f'if-serial SIM-EVENT-0001 policy-config 0 nas={int(nas)} mode=on')['ok'])
        first = bytearray(sdk.camera_sim.decode(sdk.camera_sim.first('SIM-EVENT-0001'))[1]); first[19]=1
        self.camera.ctrl.sendall(sdk.camera_sim.control(5, first))
        self.wait(lambda: self.local_status()['devices'][0]['recording']['active'])
        self.send_frames(self.frames, stream=0)
        self.wait(lambda: self.local_status()['devices'][0]['recording']['frames']==15)
        self.assertTrue(sdk.request(self.station_socket, 'if-serial SIM-EVENT-0001 arm 0 off')['ok'])
        return self.wait(lambda: sdk.request(self.station_socket, 'if-serial SIM-EVENT-0001 record-list 0')['records'])[0]

    def nas_rows(self):
        return sdk.request(self.station_socket, 'if-serial SIM-EVENT-0001 nas-list 0')['records']

    def done(self):
        return self.wait(lambda: next((r for r in self.nas_rows() if r['state']=='committed'), None),45)

    def test_nas_works_offline_and_commits_av_in_identity_directory(self):
        self.proxy.drop_all = True
        row = self.pir()
        self.done()
        path = next(self.mount.glob('BASE_A/unsynced/SIM-EVENT-0001/*_motion.ts'))
        self.assertIn(row['id'], path.name)
        self.assertEqual(path.read_bytes(), self.verify_clip(row, self.frames).read_bytes())
        self.assertEqual(path.with_suffix('.meta').stat().st_size, 128)
        mp4=path.with_suffix('.mp4');probe=self.probe(mp4)
        self.assertEqual({s['codec_name'] for s in probe['streams']},{'h264','aac'})
        self.assertEqual(next(s['sample_rate'] for s in probe['streams'] if s['codec_name']=='aac'),'8000')
        original=self.probe(path)
        for stream in (0,1):
            source=[float(p['pts_time'])-1 for p in original['packets'] if p['stream_index']==stream]
            target=[float(p['pts_time']) for p in probe['packets'] if p['stream_index']==stream]
            self.assertEqual(len(source),len(target))
            for a,b in zip(source,target):self.assertAlmostEqual(a,b,places=3)
        decoded=subprocess.run(['ffmpeg','-v','error','-xerror','-i',str(mp4),'-f','null','-'],capture_output=True,timeout=10)
        self.assertEqual(decoded.returncode,0,decoded.stderr)

        self.restart_station()
        self.done()
        self.assertEqual(len(list(self.mount.rglob('*.ts'))), 1)
        self.assertTrue(self.local_status()['devices'][0]['policy']['nas_backup'])

    def test_disabled_nas_does_not_import_history(self):
        self.pir(nas=False)
        self.assertTrue(sdk.request(self.station_socket, 'if-serial SIM-EVENT-0001 policy-config 0 nas=1')['ok'])
        self.restart_station()
        time.sleep(1.2)
        self.assertEqual(self.nas_rows(), [])
        self.assertEqual(list(self.mount.rglob('*.ts')), [])

    def test_missing_mount_fails_without_blocking_controls_and_recovers(self):
        self.mount.rmdir()
        row = self.pir()
        failed = self.wait(lambda: next((r for r in self.nas_rows() if r['state']=='retry_wait'), None))
        self.assertEqual(failed['error_code'], 21)
        self.assertTrue(sdk.request(self.station_socket, 'if-serial SIM-EVENT-0001 configure 0 mirror 3')['ok'])
        self.wait(lambda: 11 in self.camera.events)
        self.mount.mkdir(mode=0o700)
        self.restart_station()
        # Restoring a usable mount resumes automatically. A mount-health
        # probe must not make the copy wait for another 30-second cycle.
        self.wait(lambda: any(r['state']=='committed' for r in self.nas_rows()),10)
        self.assertEqual(hashlib.sha256(next(self.mount.rglob('*.ts')).read_bytes()).hexdigest(), row['sha256'])

    def test_symlink_destination_does_not_escape_mount(self):
        outside = self.root / 'outside'; outside.mkdir(mode=0o700)
        (self.mount / 'BASE_A').symlink_to(outside, target_is_directory=True)
        self.pir()
        self.wait(lambda: any(r['error_code']==22 for r in self.nas_rows()))
        self.assertEqual(list(outside.iterdir()), [])
        self.assertTrue(sdk.request(self.station_socket, 'status')['ok'])

    def test_committed_destination_conflict_is_not_overwritten(self):
        row = self.pir(); self.done()
        dest = next(self.mount.rglob('*.ts')); damaged=bytearray(dest.read_bytes()); damaged[500]^=1; dest.write_bytes(damaged)
        self.camera.close(); self.camera=None
        self.stop_proc(self.station)
        for path in (self.root/'station/recordings/.nas').glob('*.done'): path.unlink()
        for path in (self.root/'station/recordings/.nas/jobs').glob('*.job'): path.unlink()
        self.station = self.start_station()
        self.wait(lambda: self.try_request(self.station_socket, 'status'))
        self.wait(lambda: any(r['error_code']==23 for r in self.nas_rows()))
        self.assertEqual(dest.read_bytes(), damaged)
        source=self.root/'station/recordings'/('vrec-'+row['id']+'.ts')
        self.assertEqual(hashlib.sha256(source.read_bytes()).hexdigest(), row['sha256'])

    def test_mp4_conflict_is_preserved_and_missing_manifest_recovers(self):
        row=self.pir();self.done();dest=next(self.mount.rglob('*.mp4'));original=dest.read_bytes()
        marker=dest.with_name(dest.name+'.meta');self.assertTrue(marker.exists())
        def retry_from_source():
            if self.camera:self.camera.close();self.camera=None
            self.stop_proc(self.station)
            for ext in ('*.done','jobs/*.job'):
                for path in (self.root/'station/recordings/.nas').glob(ext):path.unlink()
            self.station=self.start_station()
        marker.unlink();retry_from_source();self.done();self.assertEqual(dest.read_bytes(),original);self.assertTrue(marker.exists())
        damaged=bytearray(original);damaged[-10]^=1;dest.write_bytes(damaged);retry_from_source()
        self.wait(lambda:any(r['state'] in ('failed','retry_wait') for r in self.nas_rows()))
        self.assertEqual(dest.read_bytes(),damaged)
        self.assertTrue(sdk.request(self.station_socket,'status')['ok'])

    def test_changed_nas_destination_does_not_retarget_pending_clip(self):
        self.mount.rmdir(); self.pir()
        self.wait(lambda: any(r['state']=='retry_wait' for r in self.nas_rows()))
        self.camera.close(); self.camera=None; self.stop_proc(self.station)
        self.mount.mkdir(mode=0o700)
        self.station_extra += ['--nas-export', '/different']
        self.station = self.start_station()
        self.wait(lambda: self.try_request(self.station_socket, 'status'))
        self.wait(lambda: any(r['state']=='destination_changed' for r in self.nas_rows()))
        self.assertEqual(list(self.mount.iterdir()), [])

    def test_cloud_upload_and_nas_backup_complete_independently(self):
        self.files=self.root/'remote-files'; self.files.mkdir(mode=0o700)
        self.stop_proc(self.server)
        self.server_extra=['--file-dir',str(self.files)]
        self.server=self.start_server(); self.ready_remote()
        self.cloud(True)
        row=self.pir(); self.done(); self.committed()
        cloud=self.files/('BASE_A.'+row['id']+'.ts')
        self.assertEqual(cloud.read_bytes(),next(self.mount.rglob('*.ts')).read_bytes())
        self.assertEqual(self.remote()[0]['sha256'],row['sha256'])
        self.assertTrue(sdk.request(self.station_socket,'if-serial SIM-EVENT-0001 record-delete 0 '+row['id'])['ok'])
        # Source deletion retains transfer outcomes. Explicit forgetting is
        # permitted only once the source has gone, preventing accidental resend.
        self.assertEqual(self.uploads()[0]['state'],'committed')
        self.assertEqual(self.nas_rows()[0]['state'],'committed')
        for channel in ['upload-task','nas-task']:
            self.assertTrue(sdk.request(self.station_socket,f'if-serial SIM-EVENT-0001 {channel} 0 {row["id"]} forget')['ok'])
        self.wait(lambda:not list((self.root/'station/recordings/.remote').glob('*.done')) and
                  not list((self.root/'station/recordings/.nas').glob('*.done')))
        self.assertTrue(cloud.exists())
        self.assertEqual(len(list(self.mount.rglob('*.ts'))),1)

    def test_pending_nas_recovers_without_sdk_server_configuration(self):
        self.mount.rmdir(); row=self.pir()
        self.wait(lambda:any(r['state']=='retry_wait' for r in self.nas_rows()))
        self.camera.close(); self.camera=None; self.stop_proc(self.station)
        self.mount.mkdir(mode=0o700)
        self.station=self.spawn([sdk.STATION,'--state-dir',str(self.root/'station'),
            '--control-port','0','--media-port','0','--alive-port','0',
            '--password-file',str(self.root/'wifi.key'),'--lab-ip-association',*self.station_extra])
        self.wait(lambda:self.try_request(self.station_socket,'status'))
        self.assertFalse(self.local_status()['server_connection']['enabled'])
        self.wait(lambda:self.local_status()['storage_workers']['nas_ready'])
        self.done()
        self.assertEqual(hashlib.sha256(next(self.mount.rglob('*.ts')).read_bytes()).hexdigest(),row['sha256'])

    def test_camera_serial_cannot_become_a_parent_directory(self):
        self.camera.close(); self.camera=None
        state=self.local_status()
        self.assertTrue(sdk.request(self.station_socket,'pair-open 30')['ok'])
        self.camera=sdk.camera_sim.Camera('127.0.0.1',state['ports'],'..',b'\0\0\0\1\x65test',True)
        self.camera.auto_media=False
        self.assertTrue(sdk.request(self.station_socket,'if-serial .. policy-config 1 nas=1 mode=on')['ok'])
        first=bytearray(sdk.camera_sim.decode(sdk.camera_sim.first('..'))[1]); first[19]=1
        self.camera.ctrl.sendall(sdk.camera_sim.control(5,first))
        self.wait(lambda:self.local_status()['devices'][1]['recording']['active'])
        self.send_frames(self.frames,stream=0)
        self.wait(lambda:self.local_status()['devices'][1]['recording']['frames']==15)
        self.assertTrue(sdk.request(self.station_socket,'if-serial .. arm 1 off')['ok'])
        self.wait(lambda:any(r['state']=='committed' for r in sdk.request(self.station_socket,'if-serial .. nas-list 1')['records']))
        paths=list(self.mount.rglob('*.ts')); self.assertEqual(len(paths),1)
        self.assertEqual(paths[0].relative_to(self.mount).parts[:3],('BASE_A','unsynced','%2e%2e'))

    def nas_status(self):
        return sdk.request(self.station_socket,'nas-status')

    def test_runtime_configuration_revision_capacity_and_restart(self):
        self.wait(lambda:self.nas_status()['ready'])
        status=self.command('nas-status');self.assertGreater(status['total_bytes'],0)
        self.assertGreater(status['available_bytes'],0)
        self.assertFalse(self.command('nas-config 0 1 nas.local /archive')['ok'])
        self.assertEqual(self.command(f'nas-config {status["revision"]} 1 different.local /new')['error'],
                         'disable_and_unmount_nas_before_target_change')
        self.assertTrue(self.command(f'nas-control {status["revision"]} unmount')['ok'])
        self.wait(lambda:self.nas_status()['phase']=='disabled')
        revision=self.nas_status()['revision']
        self.assertTrue(self.command(f'nas-config {revision} 1 different.local /new')['ok'])
        self.wait(lambda:self.nas_status()['ready'])
        saved=self.nas_status();self.restart_station()
        self.wait(lambda:self.nas_status()['ready'])
        actual=self.nas_status()
        self.assertEqual((actual['host'],actual['export'],actual['revision']),
                         (saved['host'],saved['export'],saved['revision']))
        self.assertEqual((self.root/'station/nas-config.bin').stat().st_size,1024)

    def test_failed_probe_and_retry_do_not_block_control(self):
        self.wait(lambda:self.nas_status()['ready'])
        self.mount.chmod(0o500);self.addCleanup(lambda:self.mount.chmod(0o700))
        revision=self.nas_status()['revision']
        self.assertTrue(self.command(f'nas-control {revision} probe')['ok'])
        self.wait(lambda:self.nas_status()['phase']=='retry_wait')
        self.assertFalse(self.nas_status()['ready'])
        self.assertTrue(self.command('if-serial SIM-EVENT-0001 settings 0')['ok'])
        self.mount.chmod(0o700)
        self.assertTrue(self.command(f'nas-control {revision} retry')['ok'])
        self.wait(lambda:self.nas_status()['ready'])
        self.assertEqual(self.nas_status()['error'],'')
        self.assertFalse(list(self.mount.glob('.myvava-probe-*')))

    def test_configuration_survives_absent_tf_and_pending_jobs_stay_at_old_target(self):
        self.mount.chmod(0o500);self.addCleanup(lambda:self.mount.chmod(0o700))
        row=self.pir();self.wait(lambda:any(r['state']=='retry_wait' for r in self.nas_rows()))
        self.assertTrue(self.command(f'nas-control {self.nas_status()["revision"]} unmount')['ok'])
        self.wait(lambda:self.nas_status()['phase']=='disabled')
        self.assertTrue(self.command(f'nas-config {self.nas_status()["revision"]} 1 another.local /new')['ok'])
        self.wait(lambda:any(r['state']=='destination_changed' for r in self.nas_rows()))
        self.mount.chmod(0o700)
        self.assertTrue(self.command(f'nas-control {self.nas_status()["revision"]} retry')['ok'])
        self.wait(lambda:self.nas_status()['ready'])
        self.assertFalse(sdk.request(self.station_socket,f'if-serial SIM-EVENT-0001 nas-task 0 {row["id"]} retry')['ok'])
        directory=self.root/'station/recordings';directory.rename(self.root/'removed-tf')
        directory.symlink_to(self.root/'missing-card')
        self.wait(lambda:not self.local_status()['storage']['available'])
        self.assertEqual(self.command('nas-status')['host'],'another.local')
        self.assertTrue(self.command('if-serial SIM-EVENT-0001 settings 0')['ok'])
        self.assertFalse(list(self.mount.rglob('*.ts')))

    def test_corrupt_nas_configuration_keeps_device_control_available(self):
        self.wait(lambda:self.nas_status()['ready'])
        self.assertTrue(self.command(f'nas-control {self.nas_status()["revision"]} unmount')['ok'])
        self.wait(lambda:self.nas_status()['phase']=='disabled')
        self.camera.close();self.camera=None;self.stop_proc(self.station)
        path=self.root/'station/nas-config.bin';original=path.read_bytes();bad=bytearray(original);bad[40]^=1;path.write_bytes(bad)
        self.station=self.start_station();self.ready_remote()
        status=self.command('nas-status');self.assertEqual(status['error'],'nas_configuration_corrupt')
        self.assertFalse(status['ready']);self.assertEqual(path.read_bytes(),bad)
        self.assertTrue(self.command('pair-open 30')['ok'])
        self.assertEqual(self.command(f'nas-config {status["revision"]} 1 nas.local /archive')['error'],'nas_configuration_corrupt')
        self.assertEqual(path.read_bytes(),bad)
        self.stop_proc(self.station);path.write_bytes(original);self.station=self.start_station()
        self.wait(lambda:self.nas_status()['phase']=='disabled')


if __name__ == '__main__':
    suite=unittest.TestSuite(NasTests(name) for name in NasTests.__dict__ if name.startswith('test_'))
    result=unittest.TextTestRunner(verbosity=2).run(suite)
    raise SystemExit(not result.wasSuccessful())
