#!/usr/bin/env python3
"""Native IDX hierarchy, stable identities and crash recovery against a real daemon."""
import hashlib
import json
import os
import shutil
import struct
import subprocess
import sys
import time
import unittest
import zlib
from pathlib import Path
from sdk_clips import ClipTests, sdk

sys.path.insert(0, str(sdk.ROOT / 'tools'))
from vava_recording import inspect_index

BASE='0123456789ABCDEFG12345678'
CAM='SIM-EVENT-0001'
STAMP=1793983490  # 2026-11-07 00:44:50 UTC+8

class ArchiveTests(ClipTests):
    def connect_camera(self):
        if any(d.get("serial")==CAM for d in self.local_status()["devices"]):
            self.camera=sdk.camera_sim.Camera("127.0.0.1",self.local_status()["ports"],CAM,b"\0\0\0\1\x65test",False)
            self.camera.step(.1)
        else:super().connect_camera()

    def stop(self):
        if self.camera:self.camera.close();self.camera=None
        self.stop_proc(self.station)

    def boot(self, extra=None):
        if extra is not None:self.station_extra=extra
        self.station=self.start_station()

    def archive_options(self):return ['--archive-serial',BASE,'--time-offset-seconds','28800','--clock-valid']

    @property
    def directory(self):return self.root/'station/recordings'

    def rows(self):return sdk.request(self.station_socket,f'if-serial {CAM} record-list 0')['records']

    def clip(self):
        self.begin();self.send_frames(self.frames)
        self.wait(lambda:self.local_status()['devices'][0]['recording']['frames']==15)
        return self.finish()['records'][0]

    def seed(self, values):
        row=self.clip();self.stop()
        source=(self.directory/f'vrec-{row["id"]}.ts').read_bytes()
        metadata=(self.directory/f'vrec-{row["id"]}.meta').read_bytes()
        for p in self.directory.glob('vrec-*'):p.unlink()
        for id,stamp,serial,slot,valid in values:
            b=bytearray(metadata);struct.pack_into('<Q',b,16,id);struct.pack_into('<Q',b,32,stamp)
            b[56]=slot;b[60]=valid;b[64:96]=serial.encode().ljust(32,b'\0');b[192:256]=bytes(64)
            struct.pack_into('<I',b,8,zlib.crc32(b[16:])&0xffffffff)
            for ext,data in [('meta',b),('ts',source)]:
                p=self.directory/f'vrec-{id:016x}.{ext}';p.write_bytes(data);p.chmod(0o600)
        return hashlib.sha256(source).hexdigest()

    def dates(self):return [e['name'] for e in inspect_index(self.directory/BASE/'vava.idx','date')['entries'] if e['active']]
    def index(self, path):return inspect_index(path,'file')

    def download(self,id):
        dest=self.root/f'download-{time.monotonic_ns()}.ts'
        p=subprocess.run([sys.executable,str(sdk.ROOT/'tools/clip_download.py'),'--socket',str(self.station_socket),
            '--serial',CAM,'0',f'{id:016x}',str(dest)],capture_output=True,timeout=15)
        self.assertEqual(p.returncode,0,p.stderr.decode());return dest.read_bytes()

    def delete(self,id):
        self.assertTrue(sdk.request(self.station_socket,f'if-serial {CAM} record-delete 0 {id:016x}')['ok'])

    def test_migration_two_cameras_days_and_delete_updates_both_indexes(self):
        digest=self.seed([(1,STAMP,CAM,0,1),(2,STAMP+60,CAM,0,1),(3,STAMP,'OTHER-CAMERA',1,1),(4,STAMP+86400,CAM,0,1)])
        self.boot(self.archive_options());rows=self.rows();self.assertEqual(len(rows),3)
        self.assertEqual(self.dates(),['20261107','20261108'])
        day=self.directory/BASE/'20261107'/CAM
        idx=self.index(day/'vava.idx');self.assertEqual([e['name'] for e in idx['entries']],['004450_U_0','004550_U_0'])
        self.assertTrue(all(e['duration_seconds']==1 and e['event_type']==0 for e in idx['entries']))
        self.assertEqual(hashlib.sha256(self.download(1)).hexdigest(),digest)
        self.delete(1);self.assertEqual([e['name'] for e in self.index(day/'vava.idx')['entries']],['004550_U_0'])
        self.delete(2);self.assertFalse(day.exists());self.assertIn('20261107',self.dates())
        # Other-camera deletion intent simulates a previously authorized operation
        # interrupted after metadata rename; no camera registration is required.
        self.stop();(self.directory/'vrec-0000000000000003.meta').rename(self.directory/'vrec-0000000000000003.deleted')
        self.boot();self.assertEqual(self.dates(),['20261108']);self.assertFalse((self.directory/BASE/'20261107').exists())
        self.delete(4);self.assertEqual(self.dates(),[]);self.assertEqual(self.local_status()['storage']['bytes'],0)

    def test_interrupted_migration_index_corruption_cursor_and_timezone_change(self):
        digest=self.seed([(1,STAMP,CAM,0,1)]);self.boot(self.archive_options());row=self.rows()[0];relative=row['archive_path']
        self.stop();media=self.directory/relative
        # V5 intent durable but the move not yet executed.
        media.rename(self.directory/'vrec-0000000000000001.ts')
        idx=media.parent/'vava.idx';idx.write_bytes(b'broken-index')
        root=self.directory/BASE/'vava.idx';b=bytearray(root.read_bytes());struct.pack_into('<I',b,8,1);root.write_bytes(b)
        self.boot(['--archive-serial',BASE,'--time-offset-seconds','0','--clock-valid'])
        self.assertEqual(self.rows()[0]['archive_path'],relative)
        self.assertEqual(hashlib.sha256(self.download(1)).hexdigest(),digest)
        self.assertEqual(self.index(idx)['first'],0);self.assertEqual(self.dates(),['20261107'])
        self.assertFalse((self.directory/'vrec-0000000000000001.ts').exists())

    def test_collisions_and_unknown_clock_preserve_all_bytes(self):
        digest=self.seed([(1,STAMP,CAM,0,1),(2,STAMP,CAM,0,1),(3,STAMP+1,CAM,0,0)])
        self.boot(self.archive_options());rows=self.rows();self.assertEqual(len(rows),3)
        self.assertEqual(sum(bool(r['archive_path']) for r in rows),1)
        for id in (1,2,3):self.assertEqual(hashlib.sha256(self.download(id)).hexdigest(),digest)
        self.assertEqual(len(self.index(next((self.directory/BASE).rglob('*/'+CAM+'/vava.idx')))['entries']),1)
        # Restart retains all entries, including the two deliberate flat exceptions.
        self.stop();self.boot();self.assertEqual(len(self.rows()),3)

    def test_orphans_are_charged_until_explicit_cleanup_and_index_failure_recovers(self):
        self.seed([(1,STAMP,CAM,0,1)]);self.boot(self.archive_options());row=self.rows()[0];media=self.directory/row['archive_path']
        before=self.local_status()['storage']['bytes'];self.stop()
        orphan=media.with_name('004451_U_0');orphan.write_bytes(b'orphan');orphan.chmod(0o600)
        self.boot();self.assertEqual(self.local_status()['storage']['bytes'],before+6)
        self.assertTrue(sdk.request(self.station_socket,'storage-cleanup')['ok']);self.assertFalse(orphan.exists())
        self.assertEqual(self.local_status()['storage']['bytes'],before)
        # An unwritable index destination cannot produce a successful delete reply.
        idx=media.parent/'vava.idx';idx.unlink();idx.mkdir()
        result=sdk.request(self.station_socket,'if-serial '+CAM+' record-delete 0 0000000000000001')
        self.assertFalse(result['ok']);self.assertFalse(self.local_status()['storage']['available'])
        idx.rmdir();self.wait(lambda:self.local_status()['storage']['available'])
        self.assertEqual(self.rows(),[]);self.assertEqual(self.dates(),[])

    def test_symlink_in_archive_tree_fails_closed(self):
        self.seed([(1,STAMP,CAM,0,1)]);outside=self.root/'outside';outside.mkdir(mode=0o700)
        (self.directory/BASE).symlink_to(outside)
        self.allow_missing_storage=True;self.boot(self.archive_options())
        self.wait(lambda:self.try_request(self.station_socket,'status'));time.sleep(1.2)
        self.assertFalse(self.local_status()['storage']['available']);self.assertEqual(list(outside.iterdir()),[])
        self.assertTrue((self.directory/'vrec-0000000000000001.ts').exists())

    def test_new_encrypted_commit_then_migration_is_byte_identical(self):
        self.stop();keyring=self.root/'keys'
        subprocess.run([sdk.STATION,'--storage-key-create',str(keyring)],check=True,capture_output=True)
        self.boot(['--record-keyring',str(keyring),'--clock-valid']);row=self.clip();self.stop()
        original=(self.directory/f'vrec-{row["id"]}.sealed').read_bytes()
        self.boot(self.archive_options()+['--record-keyring',str(keyring)])
        migrated=self.rows()[0];self.assertEqual(migrated['id'],row['id']);self.assertEqual(migrated['sha256'],row['sha256'])
        self.assertEqual((self.directory/migrated['archive_path']).read_bytes(),original)
        self.assertEqual(hashlib.sha256(self.download(int(row['id'],16))).hexdigest(),row['sha256'])
        self.wait(lambda:int(time.time())>row['unix_seconds'])
        second=self.clip();self.assertTrue(second['archive_path']);self.assertTrue(second['encrypted'])
        self.assertEqual(hashlib.sha256(self.download(int(second['id'],16))).hexdigest(),second['sha256'])
        self.stop();self.boot();self.assertEqual(len(self.rows()),2)
        self.delete(int(row['id'],16));self.delete(int(second['id'],16));self.assertEqual(self.dates(),[])

    def test_retention_removes_archived_date_and_keeps_newer_clip(self):
        self.seed([(1,int(time.time())-5*86400,CAM,0,1),(2,int(time.time()),CAM,0,1)])
        self.boot(self.archive_options());self.assertEqual(len(self.dates()),2)
        p=sdk.request(self.station_socket,'storage-policy')
        self.assertTrue(sdk.request(self.station_socket,f'storage-policy-set {p["revision"]} 1 1 72')['ok'])
        self.wait(lambda:len(self.rows())==1,25)
        self.assertEqual(self.rows()[0]['id'],'0000000000000002');self.assertEqual(len(self.dates()),1)
        self.stop();self.boot();self.assertEqual(len(self.rows()),1)

from sdk_encryption import EncryptionTests

class ArchiveTransferTests(EncryptionTests):
    def start_station(self,*args,**kwargs):
        if '--archive-serial' not in getattr(self,'station_extra',[]):
            self.station_extra=getattr(self,'station_extra',[])+['--archive-serial',BASE,'--time-offset-seconds','28800','--clock-valid']
        return super().start_station(*args,**kwargs)

    def test_archived_encrypted_clip_upload_and_nas_preserve_plaintext(self):
        self.remote_store();self.cloud(True);row=self.pir();self.done();self.committed(60)
        self.assertTrue(row['archive_path']);self.assertTrue(row['encrypted'])
        path=self.root/'station/recordings'/row['archive_path'];self.assertEqual(path.read_bytes()[:8],b'MVSEAL01')
        clear=self.download(row).read_bytes()
        self.assertEqual(next(self.mount.rglob('*.ts')).read_bytes(),clear)
        self.assertEqual((self.files/('BASE_A.'+row['id']+'.ts')).read_bytes(),clear)
        self.restart_station();self.download(row,'restart.ts')
        self.assertTrue(sdk.request(self.station_socket,'if-serial SIM-EVENT-0001 record-delete 0 '+row['id'])['ok'])
        self.assertFalse(path.exists())
        self.assertEqual(inspect_index(self.root/'station/recordings'/BASE/'vava.idx','date')['entries'],[])

if __name__=='__main__':
    suite=unittest.TestSuite(cls(n) for cls in (ArchiveTests,ArchiveTransferTests) for n in sorted(cls.__dict__) if n.startswith('test_'))
    raise SystemExit(not unittest.TextTestRunner(verbosity=2).run(suite).wasSuccessful())
