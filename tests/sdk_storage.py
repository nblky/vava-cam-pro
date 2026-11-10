#!/usr/bin/env python3
"""Removable-storage recovery without restarting pairing or the UDP session."""
import time
import os
import platform
import socket
import subprocess
import json
import unittest
from sdk_exports import ExportTests
from sdk_uploads import sdk


class StorageTests(ExportTests):
    def spawn(self,args):
        if not getattr(self,'storage_test_env',None):return super().spawn(args)
        log=__import__('tempfile').TemporaryFile()
        proc=subprocess.Popen(args,stdout=subprocess.DEVNULL,stderr=log,env={**os.environ,**self.storage_test_env})
        self.procs.append(proc);self.logs.append(log);return proc
    def change_storage(self, mount, extra=()):
        if self.camera:self.camera.close();self.camera=None
        self.stop_proc(self.station)
        self.allow_missing_storage=True
        self.station_extra=['--record-dir',str(mount/'recordings'),'--event-dir',str(mount/'events'),*extra]
        self.station=self.start_station()
        self.wait(lambda:self.try_request(self.station_socket,'status'))
        self.ready_remote()

    def storage_ready(self):
        def ready():
            s=self.local_status()
            return s['storage']['available'] and s['storage_workers']['upload_ready'] and s['event_spool']['ready']
        return self.wait(ready)

    def test_missing_card_keeps_pairing_control_then_recovers_upload(self):
        mount=self.root/'card';self.change_storage(mount)
        self.assertFalse(self.local_status()['storage']['available'])
        self.connect_camera();self.camera.auto_media=False
        self.assertTrue(self.command('if-serial SIM-EVENT-0001 settings 0')['ok'])
        self.assertFalse(sdk.request(self.station_socket,'if-serial SIM-EVENT-0001 record 0 start')['ok'])
        self.assertFalse(sdk.request(self.station_socket,'storage-cleanup')['ok'])
        self.assertFalse(mount.exists())
        mount.mkdir(mode=0o700);self.storage_ready()
        self.assertTrue(sdk.request(self.station_socket,'if-serial SIM-EVENT-0001 record 0 start')['ok'])
        self.send_frames(self.frames);self.wait(lambda:self.local_status()['devices'][0]['recording']['frames']==15)
        row=self.finish()['records'][0]
        self.assertTrue(self.request_export(row['id'])['ok']);self.done()
        self.assertEqual((mount/'recordings'/f'vrec-{row["id"]}.ts').read_bytes(),
                         (self.files/'BASE_A.a000000000000001.ts').read_bytes())
        self.drain_events()
        self.assertEqual(self.local_status()['event_spool']['dropped_since_boot'],0)

    def test_detached_card_pauses_tasks_and_restored_card_resumes(self):
        mount=self.root/'card';mount.mkdir(mode=0o700);self.change_storage(mount);self.storage_ready()
        self.proxy.drop_all=True;row=self.pir_clip();self.wait(lambda:self.uploads())
        generation=self.local_status()['storage']['generation']
        original=self.root/'removed-card';mount.rename(original)
        self.wait(lambda:not self.local_status()['storage']['available'])
        self.wait(lambda:not self.local_status()['event_spool']['ready'])
        self.assertFalse(mount.exists())
        self.assertTrue(sdk.request(self.station_socket,'if-serial SIM-EVENT-0001 settings 0')['ok'])
        mount.mkdir(mode=0o700);self.storage_ready()
        self.assertGreater(self.local_status()['storage']['generation'],generation)
        self.assertEqual(self.uploads(),[])
        self.assertEqual(sdk.request(self.station_socket,'if-serial SIM-EVENT-0001 record-list 0')['total'],0)
        replacement=self.root/'replacement-card';mount.rename(replacement);original.rename(mount)
        next_generation=self.local_status()['storage']['generation']
        self.wait(lambda:self.local_status()['storage']['generation']>next_generation)
        self.storage_ready();self.assertEqual(sdk.request(self.station_socket,'if-serial SIM-EVENT-0001 record-list 0')['total'],1)
        self.proxy.drop_all=False;self.ready_remote();self.committed(45)
        self.assertEqual(len(self.remote()),1)
        self.assertFalse(list(replacement.rglob('*.ts')))

    def test_corrupt_catalog_preserves_files_and_recovers_after_repair(self):
        mount=self.root/'card';mount.mkdir(mode=0o700);recordings=mount/'recordings';recordings.mkdir(mode=0o700)
        bad=recordings/'vrec-0000000000000001.meta';bad.write_bytes(b'corrupted fixture');bad.chmod(0o600)
        self.change_storage(mount)
        self.assertFalse(self.local_status()['storage']['available'])
        self.assertTrue(self.command('pair-open 30')['ok'])
        self.assertFalse(self.command('storage-cleanup')['ok'])
        self.assertEqual(bad.read_bytes(),b'corrupted fixture')
        bad.unlink();self.storage_ready()
        self.assertEqual(self.local_status()['storage']['records'],0)

    def test_mount_guard_never_uses_unmounted_directory_or_symlink(self):
        mount=self.root/'not-a-mount';mount.mkdir(mode=0o700)
        self.change_storage(mount,['--storage-mount',str(mount)])
        time.sleep(1.2)
        self.assertFalse(self.local_status()['storage']['available'])
        self.assertFalse(self.local_status()['event_spool']['ready'])
        self.assertEqual(list(mount.iterdir()),[])
        self.assertTrue(self.command('pair-open 30')['ok'])
        self.change_storage(mount)
        self.storage_ready();self.stop_proc(self.station)
        recordings=mount/'recordings';recordings.rename(mount/'original')
        target=self.root/'symlink-target';target.mkdir(mode=0o700);recordings.symlink_to(target)
        self.station=self.start_station();self.wait(lambda:self.try_request(self.station_socket,'status'))
        time.sleep(1.2)
        self.assertFalse(self.local_status()['storage']['available'])
        self.assertEqual(list(target.iterdir()),[])

    def test_slow_storage_does_not_hold_camera_event_loop(self):
        self.stop_proc(self.station)
        library=self.root/('slow.dylib' if platform.system()=='Darwin' else 'slow.so')
        subprocess.run(['cc','-dynamiclib' if platform.system()=='Darwin' else '-shared','-fPIC',
                        str(sdk.ROOT/'tests/slow_storage.c'),'-o',str(library)],check=True,timeout=30)
        marker=self.root/'block-fsync';directory=(self.root/'station/recordings').resolve()
        self.storage_test_env={('DYLD_INSERT_LIBRARIES' if platform.system()=='Darwin' else 'LD_PRELOAD'):str(library),
                               'MYVAVA_TEST_BLOCK_FSYNC':str(marker),'MYVAVA_TEST_BLOCK_DIRECTORY':str(directory)}
        self.station=self.start_station();self.ready_remote();self.connect_camera();self.drain_events()
        marker.write_text('test only')
        task=self.submit('storage-cleanup')
        try:
            self.wait(lambda:marker.with_suffix('.entered').exists())
            for _ in range(8):
                began=time.monotonic();self.assertTrue(sdk.request(self.station_socket,'status')['ok'])
                self.assertLess(time.monotonic()-began,.5)
                self.assertTrue(sdk.request(self.station_socket,'if-serial SIM-EVENT-0001 set 0 mirror 1')['ok'])
                self.camera.step(.05)
            self.assertNotEqual(self.task_state(task)['state'],'completed')
        finally:marker.unlink(missing_ok=True)
        self.assertTrue(self.result(task['id'])['ok'])
        self.assertTrue(self.command('if-serial SIM-EVENT-0001 settings 0')['ok'])


if __name__=='__main__':
    suite=unittest.TestSuite(StorageTests(n) for n in sorted(StorageTests.__dict__) if n.startswith('test_'))
    raise SystemExit(not unittest.TextTestRunner(verbosity=2).run(suite).wasSuccessful())
