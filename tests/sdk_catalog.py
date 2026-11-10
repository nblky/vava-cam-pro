#!/usr/bin/env python3
import shutil
import struct
import unittest
import zlib
from sdk_exports import ExportTests
from sdk_uploads import sdk

class CatalogTests(ExportTests):
    def test_long_catalog_old_source_export_and_background_upload(self):
        self.proxy.drop_all=True;row=self.pir_clip();self.cloud(False)
        self.stop_proc(self.station)
        if self.camera:self.camera.close();self.camera=None
        directory=self.root/'station/recordings'
        source=directory/f'vrec-{row["id"]}.ts';original=(directory/f'vrec-{row["id"]}.meta').read_bytes()
        for i in range(1,301):
            meta=bytearray(original);struct.pack_into('<Q',meta,16,i)
            if i!=1:meta[192:256]=bytes(64)
            struct.pack_into('<I',meta,8,zlib.crc32(meta[16:])&0xffffffff)
            p=directory/f'vrec-{i:016x}.meta';p.write_bytes(meta);p.chmod(0o600)
            shutil.copyfile(source,directory/f'vrec-{i:016x}.ts');(directory/f'vrec-{i:016x}.ts').chmod(0o600)
        self.station=self.start_station();self.wait(lambda:self.try_request(self.station_socket,'status'))
        self.assertEqual(self.local_status()['storage']['records'],301)
        page=sdk.request(self.station_socket,'if-serial SIM-EVENT-0001 record-list 0 18')
        self.assertTrue(page['ok'],page);self.assertEqual(page['total'],301);self.assertEqual(page['records'][-1]['id'],'0000000000000001')
        self.proxy.drop_all=False;self.ready_remote();self.cloud(True)
        self.wait(lambda:any(x['id']=='0000000000000001' and x['state']=='committed' for x in self.uploads()),45)
        self.wait(lambda:any(x['id']==row['id'] and x['state']=='committed' for x in self.uploads()),45)
        self.cloud(False);self.assertTrue(self.request_export('0000000000000002')['ok']);self.done()
        self.assertEqual(len(self.remote()),3)
        self.wait(lambda:len(list(directory.glob('*.evtmark')))==301,60)
        self.assertEqual(self.local_status()['event_spool']['dropped_since_boot'],0)
        self.restart_station();self.assertEqual(self.local_status()['storage']['records'],301);self.ready_remote()
        self.assertTrue(self.command('if-serial SIM-EVENT-0001 settings 0')['ok'])

    def test_upload_cancel_persistence_source_expiry_and_rate(self):
        self.proxy.drop_all=True;row=self.pir_clip();self.wait(lambda:self.uploads())
        self.assertTrue(sdk.request(self.station_socket,'upload-rate 8')['ok'])
        self.assertTrue(sdk.request(self.station_socket,f'if-serial SIM-EVENT-0001 upload-task 0 {row["id"]} cancel')['ok'])
        self.restart_station();self.assertEqual(sdk.request(self.station_socket,'upload-rate')['rate_kib'],8)
        self.assertEqual(self.uploads()[0]['state'],'cancelled')
        self.assertFalse(sdk.request(self.station_socket,f'if-serial SIM-EVENT-0001 upload-task 0 {row["id"]} forget')['ok'])
        self.assertTrue(sdk.request(self.station_socket,f'if-serial SIM-EVENT-0001 upload-task 0 {row["id"]} retry')['ok'])
        self.assertTrue(sdk.request(self.station_socket,f'if-serial SIM-EVENT-0001 record-delete 0 {row["id"]}')['ok'])
        self.proxy.drop_all=False;self.ready_remote()
        self.wait(lambda:any(x['state']=='source_expired' for x in self.uploads()),40)
        self.restart_station();self.assertEqual(self.uploads()[0]['state'],'source_expired')
        self.assertTrue(sdk.request(self.station_socket,f'if-serial SIM-EVENT-0001 upload-task 0 {row["id"]} forget')['ok'])
        self.assertEqual(self.uploads(),[])

if __name__=='__main__':
    suite=unittest.TestSuite(CatalogTests(n) for n in sorted(CatalogTests.__dict__) if n.startswith('test_'))
    result=unittest.TextTestRunner(verbosity=2).run(suite)
    raise SystemExit(not result.wasSuccessful())
