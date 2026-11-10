#!/usr/bin/env python3
"""Read-only legacy AES import through the actual server file store and Web."""
import hashlib
import json
import struct
import subprocess
import unittest
from sdk_exports import ExportTests
from sdk_uploads import sdk
from cryptography.hazmat.primitives.ciphers import Cipher,algorithms,modes

class ImportTests(ExportTests):
    def fixture(self):
        def encrypt(p):
            n=len(p)//16*16;ctx=Cipher(algorithms.AES(b'vavalic2'+bytes(8)),modes.ECB()).encryptor()
            return ctx.update(p[:n])+ctx.finalize()+p[n:]
        frames=[];total=0;video=0
        for ms,kind,payload in self.frames:
            total+=len(payload);video+=kind!=8
            body=encrypt(payload) if kind==1 else b'\xbb'+encrypt(payload[1:]) if kind==8 else payload
            frames.append(struct.pack('<6I',0xeb0000aa,len(body),kind,8 if kind==8 else 15,ms//1000,ms%1000)+body)
        raw=struct.pack('<6BHII',5,0,3,0,15,26,video+1,total,1)+b''.join(frames)
        source=self.root/'original';source.write_bytes(raw);source.chmod(0o400);return source,raw
    def import_file(self,source,keys='-',success=True):
        p=subprocess.run([str(sdk.SERVER),'--import-original',str(source),str(self.files),'BASE_A','ORIGINAL-CAM','0','0',str(keys),'64','1024'],capture_output=True,timeout=40)
        self.assertEqual(p.returncode,0 if success else 1,p.stderr.decode());return json.loads(p.stdout) if success else p
    def test_original_import_preserves_bytes_timestamps_and_uses_authenticated_web(self):
        source,raw=self.fixture();self.import_file(source,success=False) # running store lock
        self.stop_proc(self.server);r=self.import_file(source);self.assertEqual(source.read_bytes(),raw)
        self.assertEqual(self.import_file(source)['id'],r['id']);self.assertEqual(len(list(self.files.glob('*.ts'))),1)
        marker=next(self.files.glob('*.import'));descriptor=marker.read_bytes();marker.unlink()
        partial=marker.with_name(marker.name+'-tmp');partial.write_bytes(descriptor[:17]);partial.chmod(0o600)
        self.assertEqual(self.import_file(source)['id'],r['id']);self.assertEqual(marker.read_bytes(),descriptor);self.assertFalse(partial.exists())
        marker.write_bytes(b'corrupt');self.import_file(source,success=False);self.assertEqual(marker.read_bytes(),b'corrupt');marker.write_bytes(descriptor)
        self.server=self.start_server();self.ready_remote();token=self.login();rows=self.remote()
        self.assertEqual(rows[0]['source'],'legacy_import');self.assertEqual(rows[0]['serial'],'ORIGINAL-CAM')
        code,data,_=self.web('/api/v1/devices/BASE_A/files/'+r['id']+'/download',token=token)
        self.assertEqual(code,200);target=self.root/'import.ts';target.write_bytes(data);probe=self.probe(target)
        for stream,kind in [(0,False),(1,True)]:
            expected=[ms/1000 for ms,k,_ in self.frames if (k==8)==kind]
            actual=[float(p['pts_time'])-1 for p in probe['packets'] if p['stream_index']==stream]
            self.assertEqual(len(expected),len(actual))
            for a,b in zip(expected,actual):self.assertAlmostEqual(a,b,places=3)
        self.assertEqual(self.web('/api/v1/devices/BASE_A/files/'+r['id']+'/playback',token=token)[0],200)
        self.assertEqual(source.read_bytes(),raw)
    def test_corrupt_input_does_not_publish_and_encrypted_destination_never_downgrades(self):
        source,raw=self.fixture();self.stop_proc(self.server)
        # Empty new archive, entirely encrypted output; no decrypted temp file.
        keys=self.root/'archive-keys';subprocess.run([str(sdk.SERVER),'--storage-key-create',str(keys)],check=True,capture_output=True)
        r=self.import_file(source,keys);self.assertFalse(list(self.files.glob('*.ts')));self.assertTrue(list(self.files.glob('*.sealed')))
        self.import_file(source,success=False)
        bad=self.root/'truncated';bad.write_bytes(raw[:-1]);self.import_file(bad,keys,False)
        self.assertEqual(len(list(self.files.glob('*.meta'))),1);self.assertEqual(source.read_bytes(),raw)
        self.server_extra+=['--file-keyring',str(keys)];self.server=self.start_server();self.ready_remote();token=self.login()
        code,data,_=self.web('/api/v1/devices/BASE_A/files/'+r['id']+'/download',token=token);self.assertEqual(code,200)
        self.assertEqual(hashlib.sha256(data).hexdigest(),self.remote()[0]['sha256'])

if __name__=='__main__':
    suite=unittest.TestSuite(ImportTests(n) for n in sorted(ImportTests.__dict__) if n.startswith('test_'))
    raise SystemExit(not unittest.TextTestRunner(verbosity=2).run(suite).wasSuccessful())
