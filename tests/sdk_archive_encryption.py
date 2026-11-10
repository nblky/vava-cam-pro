#!/usr/bin/env python3
"""Ciphertext-only server staging/archives, independent decrypt and recovery."""
import hashlib
import json
import subprocess
import unittest
from sdk_web_media import WebMediaTests
from sdk_clips import sdk
class ArchiveEncryptionTests(WebMediaTests):
    def setUp(self):
        super().setUp();self.server_keys=self.root/'server-record-keys'
        subprocess.run([sdk.SERVER,'--storage-key-create',str(self.server_keys)],check=True,capture_output=True,timeout=10)
        self.stop_proc(self.server);self.server_extra+=['--file-keyring',str(self.server_keys)];self.server=self.start_server();self.ready_remote()
    def independent_decode(self,path,row):
        from cryptography.hazmat.primitives import hashes,hmac
        from cryptography.hazmat.primitives.kdf.hkdf import HKDF
        from cryptography.hazmat.primitives.ciphers.aead import AESGCM
        desc=(self.files/('BASE_A.'+row['id']+'.offer')).read_bytes();raw=path.read_bytes();clear=bytearray();at=0
        while at<len(raw):
            n=min(736,row['bytes']-len(clear));block=raw[at:at+88+n+16];self.assertEqual(block[:8],b'MVSEAL01')
            expected=int.from_bytes(hashlib.sha256(desc+len(clear).to_bytes(4,'big')).digest()[:8],'big') or (2**64-1)
            self.assertEqual(int.from_bytes(block[56:64],'big'),expected);self.assertEqual(int.from_bytes(block[64:72],'big'),n)
            key=bytes.fromhex((self.server_keys/(block[8:24].hex()+'.key')).read_text())
            derived=HKDF(algorithm=hashes.SHA256(),length=64,salt=block[24:56],info=b'myvava-storage-aes256gcm-v1').derive(key)
            mac=hmac.HMAC(derived[32:],hashes.SHA256());mac.update(block[:72]);self.assertEqual(mac.finalize()[:16],block[72:88])
            plain=AESGCM(derived[:32]).decrypt(b'\0'*12,block[88:],block[:64]+b'\0'*8+n.to_bytes(4,'big'));clear.extend(plain);at+=len(block)
        self.assertEqual(len(clear),row['bytes']);self.assertEqual(hashlib.sha256(clear).hexdigest(),row['sha256']);return bytes(clear)
    def test_preview_reads_encrypted_tf_and_keeps_detection_in_encrypted_server_archive(self):
        keys=self.root/'tf-preview-keys'
        subprocess.run([sdk.STATION,'--storage-key-create',str(keys)],check=True,capture_output=True,timeout=10)
        self.stop_proc(self.station);self.station_extra=getattr(self,'station_extra',[])+['--record-keyring',str(keys)];self.station=self.start_station();self.wait(lambda:self.try_request(self.station_socket,'status'));self.ready_remote()
        row=self.pir_clip(cloud=False,detection=(1,5));self.assertTrue(row['encrypted'])
        alias='d000000000000001';self.assertTrue(sdk.request(self.station_socket,f'if-serial SIM-EVENT-0001 record-preview 0 {row["id"]} {alias}')['ok'])
        token=self.login();endpoint='/api/v1/devices/BASE_A/files/'+alias
        info=self.wait(lambda:(r:=self.web(endpoint+'/info',token=token))[0]==200 and r[1],45)
        self.assertEqual(info['storage'],'tf_preview');self.assertEqual(info['file_type'],6);self.assertTrue(info['encrypted'])
        clear=self.independent_decode(self.files/('BASE_A.'+alias+'.sealed'),info)
        self.assertLess(len(clear),row['bytes']);self.assertEqual(clear[0],0x47)
        code,jpeg,_=self.web(endpoint+'/thumbnail',token=token);self.assertEqual(code,200);self.assertTrue(jpeg.startswith(bytes([255,216])))
        self.assertEqual(self.web('/api/v1/devices/BASE_A/files?storage=cloud',token=token)[1]['total'],0)
        self.stop_proc(self.server);self.server=self.start_server();self.ready_remote()
        self.assertEqual(self.web(endpoint+'/info',token=token)[1]['file_type'],6)
        self.assertEqual(self.web(endpoint+'/thumbnail',token=token)[0],200)

    def test_ciphertext_download_playback_and_independent_format(self):
        row=self.pir_clip();self.committed();token=self.login();path=self.files/('BASE_A.'+row['id']+'.sealed')
        self.assertTrue(path.exists());self.assertFalse(list(self.files.glob('*.ts')));self.assertFalse(list(self.files.glob('*.part')))
        clear=self.independent_decode(path,row);root='/api/v1/devices/BASE_A/files/'+row['id']
        self.assertTrue(self.web(root+'/info',token=token)[1]['encrypted'])
        self.assertEqual(self.web(root+'/download',token=token)[1],clear)
        code,mp4,_=self.web(root+'/playback',token=token);self.assertEqual(code,200);target=self.root/'cipher-playback.mp4';target.write_bytes(mp4)
        self.assertEqual({s['codec_name'] for s in self.probe(target)['streams']},{'h264','aac'})
        subprocess.run([sdk.SERVER,'--storage-key-rotate',str(self.server_keys)],check=True,capture_output=True,timeout=10)
        self.stop_proc(self.server);self.server=self.start_server();self.ready_remote()
        self.assertEqual(self.web(root+'/download',token=token)[1],clear)
        self.assertEqual(self.web(root+'/delete',{},token)[0],200);self.assertFalse(path.exists());self.assertEqual(self.remote(),[])
    def test_tf_batch_is_encrypted_mtu_bounded_and_retransmits_a_lost_block(self):
        keyframe=next(payload for _,kind,payload in self.frames if kind==1)
        row=self.pir_clip(cloud=False,frames=[(i*10,1,keyframe) for i in range(100)])
        self.assertGreater(row['bytes'],32*736)
        alias='a000000000000001';dropped=[]
        def lose_one_block(data):
            if len(data)>1000 and not dropped:
                dropped.append(len(data));return True
            return False
        self.proxy.frames.clear();self.proxy.drop_client_when=lose_one_block
        self.assertTrue(sdk.request(self.station_socket,f'if-serial SIM-EVENT-0001 record-export 0 {row["id"]} {alias}')['ok'])
        self.wait(lambda:any(r['id']==alias and r['state']=='committed' for r in self.remote()),45)
        self.assertEqual(len(dropped),1) # exercises Block1, not just the legacy small channel
        self.assertLessEqual(max(map(len,self.proxy.frames)),1200) # no giant UDP datagrams
        remote={**row,'id':alias};path=self.files/('BASE_A.'+alias+'.sealed')
        clear=self.independent_decode(path,remote)
        token=self.login();root='/api/v1/devices/BASE_A/files/'+alias
        self.assertEqual(self.web(root+'/download',token=token)[1],clear)
        # The exact 736-byte capsule layout stays readable across restart.
        before=path.read_bytes();self.stop_proc(self.server);self.server=self.start_server();self.ready_remote()
        self.assertEqual(path.read_bytes(),before)
        self.assertEqual(self.web(root+'/download',token=token)[1],clear)
        self.assertEqual(self.uploads(),[]) # TF cache never turns into cloud upload
    def test_partial_upload_encrypted_and_incomplete_ciphertext_is_retried(self):
        frames=[(i*70,1,self.frames[0][2]+b'\x55'*15000) for i in range(6)]
        row=self.pir_clip(frames=frames);progress=self.wait(lambda:next((r for r in self.remote() if 0<r['offset']<r['bytes']),None))
        self.proxy.drop_all=True;self.stop_proc(self.server)
        path=self.files/('BASE_A.'+row['id']+'.sealed-part');data=path.read_bytes();self.assertNotIn(b'\x55'*100,data);self.assertEqual(data[:8],b'MVSEAL01')
        # Emulate interruption in the next unacknowledged ciphertext capsule.
        self.assertLess(progress['offset'],row['bytes']);path.write_bytes(data+b'MVSEAL01'+b'\0'*23)
        self.server=self.start_server();self.proxy.drop_all=False;self.ready_remote();self.committed(80)
        self.independent_decode(self.files/('BASE_A.'+row['id']+'.sealed'),row)
    def test_tamper_missing_key_and_plaintext_fallback_refused(self):
        row=self.pir_clip();self.committed();token=self.login();path=self.files/('BASE_A.'+row['id']+'.sealed');raw=path.read_bytes()
        corrupt=bytearray(raw);corrupt[110]^=1;path.write_bytes(corrupt)
        self.assertEqual(self.web('/api/v1/devices/BASE_A/files/'+row['id']+'/download',token=token)[0],404)
        self.stop_proc(self.server);proc=self.spawn(self.server.args);self.assertNotEqual(proc.wait(timeout=10),0);self.assertEqual(path.read_bytes(),bytes(corrupt))
        path.write_bytes(raw);key=next(self.server_keys.glob('*.key'));key.rename(key.with_suffix('.missing'))
        proc=self.spawn(self.server.args);self.assertNotEqual(proc.wait(timeout=10),0)
        args=list(self.server.args);index=args.index('--file-keyring');del args[index:index+2]
        proc=self.spawn(args);self.assertNotEqual(proc.wait(timeout=10),0);self.assertEqual(path.read_bytes(),raw)
if __name__=='__main__':
    result=unittest.TextTestRunner(verbosity=2).run(unittest.TestSuite(ArchiveEncryptionTests(n) for n in sorted(ArchiveEncryptionTests.__dict__) if n.startswith('test_')))
    raise SystemExit(not result.wasSuccessful())
