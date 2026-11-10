#!/usr/bin/env python3
"""Explicit TF requests with cloud disabled, restart, cancellation and tombstones."""
import hashlib
import json
import os
import time
import struct
import zlib
import unittest
from sdk_web_media import WebMediaTests
from sdk_web import WebTests
from sdk_uploads import sdk

class ExportTests(WebMediaTests):
    submit_web=WebTests.submit_web
    def exports(self):
        return sdk.request(self.station_socket,'if-serial SIM-EVENT-0001 export-list 0')['exports']
    def request_export(self,source,id='a000000000000001'):
        return sdk.request(self.station_socket,f'if-serial SIM-EVENT-0001 record-export 0 {source} {id}')
    def done(self,id='a000000000000001'):
        return self.wait(lambda:next((r for r in self.exports() if r['id']==id and r['state']=='committed'),None),45)
    def test_cloud_off_request_and_browser_api_acl(self):
        row=self.pir_clip(cloud=False);self.assertEqual(self.remote(),[])
        token=self.login();self.user(token,'viewer','viewer');viewer=self.login('viewer','Synthetic-User-Password')
        code,task,_=self.submit_web(viewer,{'command':'record-export','args':['0',row['id'],'a000000000000001'],'camera_serial':'SIM-EVENT-0001'})
        self.assertEqual(code,200,task)
        task=self.wait(lambda:(t:=self.web(f"/api/v1/devices/BASE_A/tasks/{task['epoch']}/{task['id']}",token=viewer)[1]).get('state')=='completed' and t)
        self.assertTrue(json.loads(task['result'])['ok'],task)
        self.done();self.assertEqual(self.uploads(),[])
        self.assertFalse(self.local_status()['devices'][0]['policy']['cloud_upload'])
        code,data,_=self.web('/api/v1/devices/BASE_A/files/a000000000000001/download',token=viewer)
        self.assertEqual(code,200);self.assertEqual(hashlib.sha256(data).hexdigest(),row['sha256'])
        self.assertTrue(self.request_export(row['id'])['ok']);self.assertEqual(len(self.exports()),1)
        self.assertFalse(self.request_export(row['id'],'1000000000000001')['ok'])
        self.assertFalse(self.command(f'if-serial DIFFERENT record-export 0 {row["id"]} a000000000000002')['ok'])
        self.assertEqual(self.submit_web(viewer,{'command':'export-cancel','args':['0','a000000000000001'],'camera_serial':'SIM-EVENT-0001'})[0],403)
    def test_half_megabyte_export_is_independent_of_background_upload_limit(self):
        # Valid independently decodable IDRs make a substantial real TS clip.
        # A 1 KiB/s cloud cap must not turn interactive TF retrieval into a
        # many-minute job. The old idle polls alone also exceeded 30 seconds.
        keyframe=next(payload for _,kind,payload in self.frames if kind==1)
        frames=[(i*10,1,keyframe) for i in range(600)]
        row=self.pir_clip(cloud=False,frames=frames)
        self.assertGreater(row['bytes'],512*1024)
        rate=sdk.request(self.station_socket,'upload-rate 1');self.assertTrue(rate['ok'])
        self.assertEqual(rate['scope'],'cloud_upload')
        started=time.monotonic();self.assertTrue(self.request_export(row['id'])['ok'])
        self.done();elapsed=time.monotonic()-started
        self.assertLess(elapsed,20,'file transfer is accumulating idle loop delays')
        path=self.files/'BASE_A.a000000000000001.ts'
        self.assertEqual(hashlib.sha256(path.read_bytes()).hexdigest(),row['sha256'])
        self.assertEqual(self.remote()[0]['state'],'committed')
        self.assertEqual(self.uploads(),[]) # explicit retrieval still is not cloud upload
    def test_slow_background_upload_yields_to_explicit_retrieval(self):
        self.assertTrue(sdk.request(self.station_socket,'upload-rate 1')['ok'])
        keyframe=next(payload for _,kind,payload in self.frames if kind==1)
        row=self.pir_clip(cloud=True,frames=[(i*10,1,keyframe) for i in range(50)])
        self.assertGreater(row['bytes'],16*1024)
        self.wait(lambda:any(0<j['offset']<j['bytes'] for j in self.uploads()))
        time.sleep(1)
        self.assertLess(self.uploads()[0]['offset'],4096) # cloud pacing still applies
        started=time.monotonic();self.assertTrue(self.request_export(row['id'])['ok'])
        self.done();self.assertLess(time.monotonic()-started,10)
        self.assertEqual(hashlib.sha256((self.files/'BASE_A.a000000000000001.ts').read_bytes()).hexdigest(),row['sha256'])
    def test_storage_sources_are_independent_and_api_filters_are_strict(self):
        row=self.pir_clip(cloud=False);token=self.login();endpoint='/api/v1/devices/BASE_A/files'
        self.assertTrue(self.request_export(row['id'])['ok']);self.done()
        def listing(query):
            code,data,_=self.web(endpoint+query,token=token);self.assertEqual(code,200,data);return data
        self.assertEqual(listing('?storage=cloud')['total'],0)
        cached=listing('?storage=tf_export&serial=SIM-EVENT-0001&page=0');self.assertEqual(cached['total'],1);self.assertEqual(cached['files'][0]['storage'],'tf_export')
        self.assertEqual(listing('?storage=tf_export&serial=OTHER')['total'],0)
        self.assertEqual(self.web(endpoint+'/a000000000000001/info',token=token)[1]['storage'],'tf_export')
        for q in ('?storage=unknown','?storage=cloud&storage=tf_export','?serial=%00','?serial=a&serial=b','?page=-1','?page=0&unknown=x','?page=0&&storage=cloud','?'):
            self.assertEqual(self.web(endpoint+q,token=token)[0],400,q)
        # Policy updates are field-specific; neither destination controls the other.
        self.assertTrue(sdk.request(self.station_socket,'if-serial SIM-EVENT-0001 policy-config 0 nas=1')['ok'])
        self.cloud(True);policy=self.local_status()['devices'][0]['policy'];self.assertTrue(policy['nas_backup']);self.assertTrue(policy['cloud_upload'])
        self.assertTrue(sdk.request(self.station_socket,'if-serial SIM-EVENT-0001 policy-config 0 nas=0')['ok'])
        self.assertTrue(self.local_status()['devices'][0]['policy']['cloud_upload'])
        self.cloud(False);self.assertFalse(self.local_status()['devices'][0]['policy']['nas_backup'])
        self.assertEqual(listing('?storage=cloud')['total'],0) # no retroactive upload
        # Cache expiry is independent of the cloud retention switch and preserves TF.
        self.stop_proc(self.server);meta=self.files/'BASE_A.a000000000000001.meta';os.utime(meta,(time.time()-3700,)*2)
        self.server=self.start_server();self.ready_remote();self.wait(lambda:not self.remote(),10)
        self.assertFalse(self.web('/api/v1/archive-policy',token=token)[1]['enabled'])
        self.assertEqual(sdk.request(self.station_socket,f'if-serial SIM-EVENT-0001 record-info 0 {row["id"]}')['state'],'committed')
        self.assertTrue(self.request_export(row['id'],'a000000000000002')['ok']);self.done('a000000000000002')
        self.assertEqual(listing('?storage=cloud')['total'],0)

    def test_cancel_retry_restart_and_deleted_server_object(self):
        row=self.pir_clip(cloud=False);self.proxy.drop_all=True
        self.assertTrue(self.request_export(row['id'])['ok'])
        self.assertTrue(sdk.request(self.station_socket,'if-serial SIM-EVENT-0001 export-cancel 0 a000000000000001')['ok'])
        self.restart_station();self.assertEqual(self.exports()[0]['state'],'cancelled')
        self.proxy.drop_all=False;self.ready_remote();self.assertEqual(self.remote(),[])
        self.assertTrue(sdk.request(self.station_socket,'if-serial SIM-EVENT-0001 export-retry 0 a000000000000001')['ok']);self.done()
        token=self.login();self.assertEqual(self.web('/api/v1/devices/BASE_A/files/a000000000000001/delete',{},token)[0],200)
        self.stop_proc(self.station)
        if self.camera:self.camera.close();self.camera=None
        (self.root/'station/recordings/.remote/a000000000000001.done').unlink()
        # A committed durable export is never silently sent again after deletion.
        self.station=self.start_station();self.wait(lambda:self.try_request(self.station_socket,'status'));self.ready_remote()
        self.assertEqual(self.exports()[0]['state'],'committed');self.assertEqual(self.remote(),[])
        self.assertTrue(self.request_export(row['id'],'a000000000000002')['ok']);self.done('a000000000000002')
        self.assertEqual([r['id'] for r in self.remote()],['a000000000000002'])
    def test_expired_terminal_export_history_is_reclaimed_without_losing_active_jobs(self):
        row=self.pir_clip(cloud=False);self.proxy.drop_all=True
        ids=[f'a00000000000{i:04x}' for i in range(1,17)]
        for id in ids:self.assertTrue(self.request_export(row['id'],id)['ok'])
        self.assertFalse(self.request_export(row['id'],'a00000000000ffff')['ok'])
        self.assertTrue(sdk.request(self.station_socket,f'if-serial SIM-EVENT-0001 export-cancel 0 {ids[0]}')['ok'])
        self.stop_proc(self.station)
        if self.camera:self.camera.close();self.camera=None
        path=self.root/'station/recordings/.remote'/f'{ids[0]}.export';data=bytearray(path.read_bytes());struct.pack_into('>Q',data,24,int(time.time())-1);struct.pack_into('<I',data,8,zlib.crc32(data[16:]));path.write_bytes(data)
        self.station=self.start_station();self.wait(lambda:self.try_request(self.station_socket,'status'))
        self.assertTrue(self.request_export(row['id'],'a00000000000ffff')['ok'])
        current={r['id'] for r in self.exports()};self.assertEqual(current,set(ids[1:])|{'a00000000000ffff'})
        self.assertFalse(path.exists());self.assertFalse(self.request_export(row['id'],'a00000000000fffe')['ok'])

    def test_source_expired_history_and_forget(self):
        row=self.pir_clip(cloud=False);self.proxy.drop_all=True
        self.assertTrue(self.request_export(row['id'])['ok'])
        self.assertTrue(sdk.request(self.station_socket,f'if-serial SIM-EVENT-0001 record-delete 0 {row["id"]}')['ok'])
        self.proxy.drop_all=False;self.ready_remote()
        failed=self.wait(lambda:next((r for r in self.exports() if r['state']=='source_expired'),None),40)
        self.assertEqual(failed['error'],'source_unavailable')
        self.restart_station();self.assertEqual(self.exports()[0]['state'],'source_expired')
        self.assertFalse(sdk.request(self.station_socket,'if-serial SIM-EVENT-0001 export-retry 0 a000000000000001')['ok'])
        self.assertTrue(sdk.request(self.station_socket,'if-serial SIM-EVENT-0001 export-forget 0 a000000000000001')['ok']);self.assertEqual(self.exports(),[])
        self.restart_station();self.assertEqual(self.exports(),[])

if __name__=='__main__':
    suite=unittest.TestSuite(ExportTests(n) for n in sorted(ExportTests.__dict__) if n.startswith('test_'))
    result=unittest.TextTestRunner(verbosity=2).run(suite)
    raise SystemExit(not result.wasSuccessful())
