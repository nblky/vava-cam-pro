#!/usr/bin/env python3
"""Quota/age eviction, durable export holds, pending uploads, and policy ACL."""
import shutil
import struct
import time
import unittest
import zlib
from sdk_exports import ExportTests
from sdk_uploads import sdk

class RetentionTests(ExportTests):
    def policy(self,enabled=True,days=1,pending=72):
        old=sdk.request(self.station_socket,'storage-policy')
        result=sdk.request(self.station_socket,f'storage-policy-set {old["revision"]} {int(enabled)} {days} {pending}')
        self.assertTrue(result['ok'],result);return result

    def seed(self,count,days=5,pending=()):
        row=self.pir_clip();self.cloud(False);self.stop_proc(self.station)
        if self.camera:self.camera.close();self.camera=None
        directory=self.root/'station/recordings';source=directory/f'vrec-{row["id"]}.ts';meta=(directory/f'vrec-{row["id"]}.meta').read_bytes()
        for i in range(1,count+1):
            b=bytearray(meta);struct.pack_into('<Q',b,16,i);struct.pack_into('<Q',b,32,int(time.time())-days*86400);b[60]=1
            if i not in pending:b[192:256]=bytes(64)
            struct.pack_into('<I',b,8,zlib.crc32(b[16:])&0xffffffff)
            p=directory/f'vrec-{i:016x}.meta';p.write_bytes(b);p.chmod(0o600)
            p=directory/f'vrec-{i:016x}.ts';shutil.copyfile(source,p);p.chmod(0o600)
        self.station_extra=['--clock-valid',*getattr(self,'retention_extra',[])]
        self.station=self.start_station();return row

    def test_age_pending_and_export_holds_survive_restart(self):
        self.proxy.drop_all=True
        # Three aged originals. A pending upload younger than its protection
        # window and an explicit export must survive a concurrent age sweep.
        self.seed(3,days=2,pending=(1,));self.proxy.drop_all=False;self.ready_remote()
        self.assertTrue(self.request_export('0000000000000002')['ok']);self.done()
        p=self.policy(days=1,pending=72);self.restart_station();self.ready_remote()
        self.wait(lambda:sdk.request(self.station_socket,'if-serial SIM-EVENT-0001 record-info 0 0000000000000003').get('state')=='missing',25)
        for id in (1,2):self.assertEqual(sdk.request(self.station_socket,f'if-serial SIM-EVENT-0001 record-info 0 {id:016x}')['state'],'committed')
        saved=sdk.request(self.station_socket,'storage-policy');self.assertEqual(saved['revision'],p['revision']);self.assertTrue(saved['enabled'])
        self.assertFalse(sdk.request(self.station_socket,f'storage-policy-set {p["revision"]-1} 0 30 72')['ok'])
        self.policy(days=1,pending=1)
        self.wait(lambda:sdk.request(self.station_socket,'if-serial SIM-EVENT-0001 record-info 0 0000000000000001').get('state')=='missing',25)
        self.wait(lambda:any(r['state']=='source_expired' for r in self.uploads()),25)

    def test_quota_eviction_leaves_newest_and_oldest_pending(self):
        self.retention_extra=['--record-max-mb','1'];self.proxy.drop_all=True
        row=self.seed(200,days=0,pending=(1,));self.proxy.drop_all=False;self.ready_remote()
        before=self.local_status()['storage'];self.assertGreater(before['bytes'],before['quota_bytes'])
        self.policy(days=0,pending=72)
        self.wait(lambda:self.local_status()['storage']['bytes']<=before['quota_bytes']*.8,30)
        self.assertEqual(sdk.request(self.station_socket,'if-serial SIM-EVENT-0001 record-info 0 0000000000000001')['state'],'committed')
        self.assertEqual(sdk.request(self.station_socket,f'if-serial SIM-EVENT-0001 record-info 0 {row["id"]}')['state'],'committed')
        self.assertEqual(sdk.request(self.station_socket,'if-serial SIM-EVENT-0001 record-info 0 0000000000000002')['state'],'missing')

    def test_policy_without_card_and_web_permissions(self):
        token=self.login();self.user(token,'viewer','viewer');viewer=self.login('viewer','Synthetic-User-Password')
        self.assertEqual(self.submit_web(viewer,{'command':'storage-policy-set','args':['1','1','30','72']})[0],403)
        self.assertEqual(self.submit_web(viewer,{'command':'storage-policy','args':[]})[0],200)
        self.stop_proc(self.station);self.allow_missing_storage=True
        self.station_extra=['--record-dir',str(self.root/'absent/recordings')]
        self.station=self.start_station();self.wait(lambda:self.try_request(self.station_socket,'status'));self.ready_remote()
        self.assertTrue(self.policy()['healthy']);self.assertFalse(self.local_status()['storage']['available'])

if __name__=='__main__':
    suite=unittest.TestSuite(RetentionTests(n) for n in sorted(RetentionTests.__dict__) if n.startswith('test_'))
    raise SystemExit(not unittest.TextTestRunner(verbosity=2).run(suite).wasSuccessful())
