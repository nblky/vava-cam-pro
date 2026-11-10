#!/usr/bin/env python3
"""Factory-reset transaction and retained encrypted clip identities; synthetic camera only."""
import hashlib
import socket
import subprocess
import unittest
import zlib
from sdk_archive import ArchiveTests, CAM, sdk

class RemovalTests(ArchiveTests):
    def ready(self, verified=True):
        if not self.camera:self.connect_camera()
        self.camera.auto_media=False
        if verified:
            p=bytearray(sdk.camera_sim.decode(sdk.camera_sim.first(CAM))[1])
            p[32:48]=b'1.4.0.54'.ljust(16,b'\0');p[48:64]=b'VA-HS03'.ljust(16,b'\0')
            self.camera.ctrl.sendall(sdk.camera_sim.control(5,p))
            self.wait(lambda:self.local_status()['devices'][0]['identity_report']['firmware']=='1.4.0.54')
    def op(self):return self.local_status()['removal']
    def start_remove(self):
        r=sdk.request(self.station_socket,f'if-serial {CAM} remove 0');self.assertTrue(r['ok'],r);return r
    def received(self,command):
        return self.wait(lambda:next((p for c,p in reversed(self.camera.received_control) if c==command),None))
    def echo(self,command,payload):self.camera.ctrl.sendall(sdk.camera_sim.control(command,payload))
    def finish_remove(self):
        self.start_remove();defaults=self.received(22)
        self.assertNotIn(int.from_bytes(defaults[:4],'little'),(0,2**32-1));self.assertEqual(defaults[4:8],bytes(4))
        wrong=bytearray(defaults);wrong[0]^=1;self.echo(22,wrong)
        self.wait(lambda:self.op()['phase']=='restoring_defaults');self.assertTrue(self.local_status()['devices'])
        self.echo(22,defaults);clear=self.received(4);self.assertEqual(defaults,clear)
        self.echo(4,clear);self.camera.close();self.camera=None
        self.wait(lambda:self.op()['phase']=='completed');self.assertEqual(self.local_status()['devices'],[])
    def restart(self):
        if self.camera:self.camera.close();self.camera=None
        self.stop_proc(self.station);self.station=self.start_station()
    def test_reset_remove_preserves_encrypted_catalog_indexes_keys_and_slot_reuse(self):
        self.stop_proc(self.server);files=self.root/'remote-files';files.mkdir(mode=0o700)
        self.server_extra=['--file-dir',str(files)];self.server=self.start_server()
        self.stop_proc(self.station);keys=self.root/'storage-keys'
        subprocess.run([sdk.STATION,'--storage-key-create',str(keys)],check=True,capture_output=True)
        self.station_extra=[*self.archive_options(),'--record-keyring',str(keys)];self.station=self.start_station()
        row=self.clip();self.ready();media=self.directory/row['archive_path'];cipher=media.read_bytes()
        key_files={p.name:p.read_bytes() for p in keys.iterdir()};clear=self.download(int(row['id'],16))
        self.finish_remove();self.assertTrue(row['encrypted']);self.assertEqual(media.read_bytes(),cipher)
        self.assertEqual({p.name:p.read_bytes() for p in keys.iterdir()},key_files)
        self.assertEqual(self.rows(),[row]);owners=sdk.request(self.station_socket,'record-cameras')['cameras']
        self.assertEqual(owners,[{'serial':CAM,'slot':0,'records':1}]);self.assertEqual(self.download(int(row['id'],16)),clear)
        self.restart();self.assertEqual(self.op()['phase'],'completed');self.assertEqual(self.rows(),[row])
        sdk.request(self.station_socket,'pair-open 30');ports=self.local_status()['ports']
        self.camera=sdk.camera_sim.Camera('127.0.0.1',ports,'NEW-CAMERA',b'\0\0\0\1\x65test',True);self.camera.auto_media=False
        self.wait(lambda:self.local_status()['devices'][0]['serial']=='NEW-CAMERA')
        self.assertEqual(sdk.request(self.station_socket,f'if-serial {CAM} configure 0 osd 1')['error'],'camera_identity_changed')
        self.assertEqual(sdk.request(self.station_socket,'if-serial NEW-CAMERA record-list 0')['records'],[])
        self.assertEqual(self.rows(),[row]);self.assertEqual(self.download(int(row['id'],16)),clear)
        self.assertEqual(hashlib.sha256(clear).hexdigest(),row['sha256'])
        self.assertTrue(sdk.request(self.station_socket,f'if-serial {CAM} record-export 0 {row["id"]} a000000000000001')['ok'])
        self.wait(lambda:any(e['state']=='committed' for e in sdk.request(self.station_socket,f'if-serial {CAM} export-list 0')['exports']),45)
        self.delete(int(row['id'],16));self.assertEqual(sdk.request(self.station_socket,'record-cameras')['cameras'],[])
        self.assertEqual(self.dates(),[])
    def test_identity_recording_and_firmware_guards_do_not_send_destructive_requests(self):
        self.ready(False)
        self.assertEqual(sdk.request(self.station_socket,f'if-serial {CAM} forget 0')['error'],'factory_reset_required_use_remove')
        self.assertEqual(sdk.request(self.station_socket,'if-serial WRONG remove 0')['error'],'camera_identity_changed')
        self.assertEqual(sdk.request(self.station_socket,'remove 0')['error'],'camera_identity_required')
        self.start_remove();self.wait(lambda:self.op()['phase']=='failed')
        self.assertEqual(self.op()['reason'],'factory_reset_firmware_not_verified')
        self.assertFalse(any(c in (22,4) for c,p in self.camera.received_control))
        self.ready();self.assertTrue(sdk.request(self.station_socket,f'if-serial {CAM} record 0 start')['ok'])
        self.assertEqual(sdk.request(self.station_socket,f'if-serial {CAM} remove 0')['error'],'recording_must_finish_before_remove')
    def test_restart_unknown_never_replays_and_manual_completion_is_identity_operation_guarded(self):
        self.ready();started=self.start_remove();self.received(22);self.restart()
        self.assertEqual(self.op()['phase'],'result_unknown');self.assertTrue(self.local_status()['devices'][0]['paired'])
        self.ready();self.assertFalse(any(c in (22,4) for c,p in self.camera.received_control))
        self.assertEqual(sdk.request(self.station_socket,f'if-serial {CAM} configure 0 osd 1')['error'],'camera_removal_in_progress_or_unconfirmed')
        self.assertEqual(sdk.request(self.station_socket,f'if-serial {CAM} remove-confirm 0 0000000000000000')['error'],'removal_confirmation_conflict')
        self.assertTrue(sdk.request(self.station_socket,f'if-serial {CAM} remove-confirm 0 {started["operation_id"]}')['ok'])
        self.camera.close();self.camera=None;self.wait(lambda:self.op()['phase']=='completed')
        self.assertTrue(self.op()['operator_confirmed']);self.restart();self.assertTrue(self.op()['operator_confirmed'])
    def test_unpair_disconnect_retains_registration_until_physical_reset_confirmation(self):
        self.ready();self.start_remove();self.echo(22,self.received(22));self.received(4)
        self.camera.close();self.camera=None
        self.wait(lambda:self.op()['phase']=='result_unknown');self.assertTrue(self.local_status()['devices'][0]['paired'])
        self.assertFalse(self.op()['operator_confirmed']);self.restart();self.assertEqual(self.op()['phase'],'result_unknown')

    def test_offline_timeout_keeps_pairing_and_never_claims_reset(self):
        self.ready();self.camera.close();self.camera=None
        self.wait(lambda:not self.local_status()['devices'][0]['control_ready'])
        self.start_remove();self.assertEqual(self.op()['phase'],'waiting_camera')
        self.wait(lambda:self.op()['phase']=='failed',50)
        self.assertEqual(self.op()['reason'],'camera_wake_timeout')
        self.assertTrue(self.local_status()['devices'][0]['paired'])
        self.assertFalse(self.op()['operator_confirmed'])

    def test_corrupt_reset_intent_refuses_startup_without_overwriting(self):
        self.ready();self.start_remove();self.received(22)
        self.camera.close();self.camera=None;self.stop_proc(self.station)
        path=self.root/'station/removal.bin';raw=bytearray(path.read_bytes());raw[32]^=1;path.write_bytes(raw)
        proc=self.start_station(wait_ready=False);self.assertNotEqual(proc.wait(timeout=5),0)
        self.assertEqual(path.read_bytes(),raw)

    def test_restart_resumes_only_acknowledged_local_commit(self):
        self.ready();self.start_remove();self.received(22)
        self.camera.close();self.camera=None;self.stop_proc(self.station)
        # Fault injection: the camera's matching echoes have been journalled,
        # power fails before registry_save. No camera command may be replayed.
        path=self.root/'station/removal.bin';raw=bytearray(path.read_bytes());raw[24]=4
        raw[64:112]=b'camera_reset_echoes_observed'.ljust(48,b'\0')
        raw[188:192]=(zlib.crc32(raw[:188])&0xffffffff).to_bytes(4,'little');path.write_bytes(raw)
        self.station=self.start_station();self.wait(lambda:self.op()['phase']=='completed')
        self.assertEqual(self.local_status()['devices'],[]);self.assertFalse(self.op()['operator_confirmed'])

    def test_standalone_defaults_keeps_pairing_policies_name_recordings_and_never_sends_unpair(self):
        row=self.clip();self.ready();clear=self.download(int(row['id'],16))
        self.wait(lambda:not self.local_status()['devices'][0]['recording']['active'])
        sdk.request(self.station_socket,f'if-serial {CAM} name 0 '+b'Entrance'.hex())
        sdk.request(self.station_socket,f'if-serial {CAM} configure 0 osd 1')
        # Echo the ordinary setting before beginning a destructive transaction.
        self.echo(15,self.received(15))
        self.wait(lambda:not sdk.request(self.station_socket,f'if-serial {CAM} settings 0')['settings']['pending'])
        before=self.local_status()['devices'][0]
        desired=sdk.request(self.station_socket,f'if-serial {CAM} settings 0')['desired']
        self.assertEqual(sdk.request(self.station_socket,'reset 0')['error'],'camera_identity_required')
        self.assertEqual(sdk.request(self.station_socket,'if-serial OTHER reset 0')['error'],'camera_identity_changed')
        reply=sdk.request(self.station_socket,f'if-serial {CAM} reset 0');self.assertTrue(reply['ok'],reply)
        self.assertEqual(reply['removal']['action'],'reset');self.assertTrue(reply['removal']['pairing_preserved'])
        payload=self.received(22);wrong=bytearray(payload);wrong[0]^=1;self.echo(22,wrong)
        self.wait(lambda:self.op()['phase']=='restoring_defaults')
        self.assertFalse(any(c==4 for c,p in self.camera.received_control))
        self.echo(22,payload);self.wait(lambda:self.op()['phase']=='completed')
        after=self.local_status()['devices'][0]
        self.assertTrue(after['paired']);self.assertEqual(after['serial'],CAM)
        self.assertEqual(after['profile'],before['profile'])
        self.assertEqual(after['policy']['revision'],before['policy']['revision'])
        current=sdk.request(self.station_socket,f'if-serial {CAM} settings 0')['desired']
        self.assertEqual(current['revision'],desired['revision']+1);self.assertEqual(current['values'],{})
        self.assertEqual(self.rows(),[row]);self.assertEqual(self.download(int(row['id'],16)),clear)
        self.assertFalse(any(c==4 for c,p in self.camera.received_control))
        self.restart();self.assertEqual(self.op()['action'],'reset');self.assertEqual(self.op()['phase'],'completed')
        self.assertEqual(sdk.request(self.station_socket,f'if-serial {CAM} settings 0')['desired']['revision'],current['revision'])
        self.assertTrue(self.local_status()['devices'][0]['paired']);self.assertEqual(self.rows(),[row])

    def test_unconfirmed_standalone_reset_cannot_be_confirmed_as_a_camera_removal(self):
        self.ready();reply=sdk.request(self.station_socket,f'if-serial {CAM} reset 0');self.assertTrue(reply['ok'])
        self.received(22);self.restart();self.assertEqual(self.op()['phase'],'result_unknown')
        self.assertEqual(self.op()['action'],'reset')
        self.assertEqual(sdk.request(self.station_socket,f'if-serial {CAM} remove-confirm 0 {reply["operation_id"]}')['error'],'removal_confirmation_conflict')
        self.assertEqual(sdk.request(self.station_socket,f'if-serial {CAM} remove 0')['error'],'removal_state_unavailable_or_unresolved')
        self.assertTrue(self.local_status()['devices'][0]['paired']);self.ready()
        self.assertFalse(any(c in (22,4) for c,p in self.camera.received_control))
        self.assertTrue(sdk.request(self.station_socket,f'if-serial {CAM} reset 0')['ok'])
        self.echo(22,self.received(22));self.wait(lambda:self.op()['phase']=='completed')
        self.assertFalse(any(c==4 for c,p in self.camera.received_control))

    def test_acknowledged_standalone_reset_resumes_only_idempotent_local_commit(self):
        self.ready();reply=sdk.request(self.station_socket,f'if-serial {CAM} reset 0');self.assertTrue(reply['ok'])
        self.received(22);self.camera.close();self.camera=None;self.stop_proc(self.station)
        path=self.root/'station/removal.bin';raw=bytearray(path.read_bytes());raw[24]=4
        raw[64:112]=b'camera_defaults_echo_observed'.ljust(48,b'\0')
        raw[188:192]=(zlib.crc32(raw[:188])&0xffffffff).to_bytes(4,'little');path.write_bytes(raw)
        self.station=self.start_station();self.wait(lambda:self.op()['phase']=='completed')
        self.assertTrue(self.local_status()['devices'][0]['paired'])
        self.assertEqual(self.op()['action'],'reset')
        revision=sdk.request(self.station_socket,f'if-serial {CAM} settings 0')['desired']['revision']
        self.assertEqual(revision,int.from_bytes(raw[112:116],'little'))
        self.restart();self.ready()
        self.assertEqual(sdk.request(self.station_socket,f'if-serial {CAM} settings 0')['desired']['revision'],revision)
        self.assertFalse(any(c in (22,4) for c,p in self.camera.received_control))

if __name__=='__main__':
    suite=unittest.TestSuite(RemovalTests(n) for n in sorted(RemovalTests.__dict__) if n.startswith('test_'))
    raise SystemExit(not unittest.TextTestRunner(verbosity=2).run(suite).wasSuccessful())
