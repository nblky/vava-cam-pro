#!/usr/bin/env python3
"""Encrypted TF recordings still export, upload and archive through authenticated readers."""
import hashlib
import json
import subprocess
import sys
import unittest
from pathlib import Path
from sdk_nas import NasTests, sdk

class EncryptionTests(NasTests):
    def start_station(self,*args,**kwargs):
        if not hasattr(self,'keyring'):
            self.keyring=self.root/'record-keys'
            p=subprocess.run([sdk.STATION,'--storage-key-create',str(self.keyring)],capture_output=True,check=True)
            self.first_key=json.loads(p.stdout)['active_key_id']
        if '--record-keyring' not in getattr(self,'station_extra',[]):
            self.station_extra=getattr(self,'station_extra',[])+['--record-keyring',str(self.keyring)]
        return super().start_station(*args,**kwargs)

    def download(self,row,name='clear.ts',good=True):
        dest=self.root/name
        p=subprocess.run([sys.executable,str(sdk.ROOT/'tools/clip_download.py'),'--socket',str(self.station_socket),
            '--serial','SIM-EVENT-0001','0',row['id'],str(dest)],capture_output=True,timeout=15)
        if good:
            self.assertEqual(p.returncode,0,p.stderr.decode())
            self.assertEqual(hashlib.sha256(dest.read_bytes()).hexdigest(),row['sha256'])
        else:
            self.assertNotEqual(p.returncode,0);self.assertFalse(dest.exists())
        return dest

    def remote_store(self):
        self.files=self.root/'remote-files';self.files.mkdir(mode=0o700)
        self.stop_proc(self.server);self.server_extra=['--file-dir',str(self.files)];self.server=self.start_server();self.ready_remote()

    def test_encrypted_tf_download_upload_and_nas_are_identical(self):
        self.remote_store();self.cloud(True);row=self.pir();self.done();self.committed(60)
        self.assertTrue(row['encrypted']);self.assertGreater(row['stored_bytes'],row['bytes'])
        path=self.root/'station/recordings'/('vrec-'+row['id']+'.sealed')
        self.assertEqual(path.stat().st_size,row['stored_bytes']);self.assertEqual(path.read_bytes()[:8],b'MVSEAL01')
        self.assertEqual(self.local_status()['storage']['bytes'],row['stored_bytes'])
        clear=self.download(row).read_bytes();self.assertNotEqual(path.read_bytes()[88:188],clear[:100])
        self.assertEqual(next(self.mount.rglob('*.ts')).read_bytes(),clear)
        self.assertEqual((self.files/('BASE_A.'+row['id']+'.ts')).read_bytes(),clear)
        decoded=self.probe(self.root/'clear.ts');self.assertEqual({s['codec_name'] for s in decoded['streams']},{'h264','aac'})
        self.restart_station();self.wait(lambda:self.try_request(self.station_socket,'status'))
        self.download(row,'after-restart.ts')
        self.assertTrue(self.command('capabilities')['at_rest_encryption'])
        self.assertTrue(sdk.request(self.station_socket,'if-serial SIM-EVENT-0001 record-delete 0 '+row['id'])['ok'])
        self.assertEqual(self.local_status()['storage']['bytes'],0)

    def test_file_format_matches_independent_aes_gcm_implementation(self):
        try:
            from cryptography.hazmat.primitives import hashes, hmac
            from cryptography.hazmat.primitives.kdf.hkdf import HKDF
            from cryptography.hazmat.primitives.ciphers.aead import AESGCM
        except ImportError:
            self.skipTest('cryptography needed for independent format verification')
        row=self.pir();self.done()
        blob=(self.root/'station/recordings'/('vrec-'+row['id']+'.sealed')).read_bytes()
        header=blob[:88];master=bytes.fromhex((self.keyring/(header[8:24].hex()+'.key')).read_text())
        derived=HKDF(algorithm=hashes.SHA256(),length=64,salt=header[24:56],info=b'myvava-storage-aes256gcm-v1').derive(master)
        mac=hmac.HMAC(derived[32:],hashes.SHA256());mac.update(header[:72]);self.assertEqual(mac.finalize()[:16],header[72:88])
        self.assertEqual(int.from_bytes(header[56:64],'big'),int(row['id'],16));self.assertEqual(int.from_bytes(header[64:72],'big'),row['bytes'])
        result=bytearray();index=0;position=88
        while len(result)<row['bytes']:
            n=min(4096,row['bytes']-len(result));sequence=index.to_bytes(8,'big')
            result+=AESGCM(derived[:32]).decrypt(b'\0'*4+sequence,blob[position:position+n+16],header[:64]+sequence+n.to_bytes(4,'big'))
            position+=n+16;index+=1
        self.assertEqual(position,len(blob));self.assertEqual(hashlib.sha256(result).hexdigest(),row['sha256'])
        self.assertEqual(self.download(row).read_bytes(),result)

    def test_rotation_retains_old_keys_and_missing_key_fails_closed(self):
        row=self.pir();self.done();self.restart_station()
        self.stop_proc(self.station)
        result=subprocess.run([sdk.STATION,'--storage-key-rotate',str(self.keyring)],capture_output=True,check=True)
        second=json.loads(result.stdout)['active_key_id'];self.assertNotEqual(second,self.first_key)
        self.station=self.start_station();self.wait(lambda:self.try_request(self.station_socket,'status'));self.camera=sdk.camera_sim.Camera('127.0.0.1',self.local_status()['ports'],'SIM-EVENT-0001',b'\0\0\0\1\x65test',False);self.camera.auto_media=False
        newer=self.pir();self.done();self.download(row,'old.ts');self.download(newer,'new.ts')
        newer_blob=self.root/'station/recordings'/('vrec-'+newer['id']+'.sealed')
        self.assertEqual(newer_blob.read_bytes()[8:24].hex(),second)
        if self.camera:self.camera.close();self.camera=None
        self.stop_proc(self.station);oldkey=self.keyring/(self.first_key+'.key');backup=self.root/'old-key.backup';oldkey.rename(backup)
        self.allow_missing_storage=True;failed=self.start_station();self.wait(lambda:self.try_request(self.station_socket,'status'))
        self.assertIsNone(failed.poll());self.assertFalse(self.local_status()['storage']['available'])
        self.assertTrue(sdk.request(self.station_socket,'if-serial SIM-EVENT-0001 settings 0')['ok'])
        self.assertTrue(newer_blob.exists());self.stop_proc(failed);backup.rename(oldkey);self.allow_missing_storage=False
        self.station=self.start_station();self.wait(lambda:self.try_request(self.station_socket,'status'));self.download(row,'restored.ts')

    def test_ciphertext_tamper_never_commits_plaintext_outputs(self):
        self.remote_store();self.proxy.drop_all=True;self.mount.rmdir();self.cloud(True);row=self.pir()
        self.restart_station();self.stop_proc(self.station)
        path=self.root/'station/recordings'/('vrec-'+row['id']+'.sealed');data=bytearray(path.read_bytes());data[200]^=1;path.write_bytes(data)
        self.mount.mkdir(mode=0o700);self.proxy.drop_all=False;self.station=self.start_station();self.wait(lambda:self.try_request(self.station_socket,'status'));self.ready_remote()
        self.download(row,'tampered.ts',good=False)
        self.assertTrue(sdk.request(self.station_socket,f'if-serial SIM-EVENT-0001 nas-task 0 {row["id"]} retry')['ok'])
        self.assertTrue(sdk.request(self.station_socket,f'if-serial SIM-EVENT-0001 upload-task 0 {row["id"]} retry')['ok'])
        self.wait(lambda:any(r['error_code']==26 for r in self.nas_rows()))
        self.wait(lambda:any(r['error']=='source_read_failed' for r in self.uploads()),60)
        self.assertFalse(list(self.mount.rglob('*.meta')));self.assertFalse(list(self.files.glob('*.meta')))
        self.assertTrue(self.command('status')['ok'])

if __name__=='__main__':
    suite=unittest.TestSuite(EncryptionTests(n) for n in sorted(EncryptionTests.__dict__) if n.startswith('test_'))
    result=unittest.TextTestRunner(verbosity=2).run(suite)
    raise SystemExit(not result.wasSuccessful())
