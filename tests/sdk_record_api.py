#!/usr/bin/env python3
"""Stable TF query contracts through local IPC, DTLS and the management API."""
import json
import socket
import unittest
from sdk_archive import ArchiveTests, CAM, STAMP, sdk
from sdk_exports import ExportTests

class RecordApiTests(ArchiveTests):
    def query(self,before='0000000000000000',generation=0,begin=0,end=0,remote=False):
        line=f'if-serial {CAM} record-query 0 {before} {generation} {begin} {end}'
        return self.command(line) if remote else sdk.request(self.station_socket,line)

    def test_cursor_survives_deletion_and_new_clip_with_identity_time_filters(self):
        values=[(i,STAMP+i,CAM,0,1) for i in range(1,35)]+[(35,STAMP+35,CAM,0,0),(36,STAMP+36,'OTHER-CAMERA',1,1)]
        self.seed(values);self.boot(self.archive_options());self.online(45);self.wait(lambda:self.local_status()["server_connection"]["online"],45)
        caps=sdk.request(self.station_socket,'capabilities');self.assertTrue(caps['record_query']);self.assertTrue(self.command('capabilities')['record_info_details'])
        all_rows=self.command('record-query-all 0000000000000000 0 0 0');self.assertTrue(all_rows['ok']);self.assertEqual(all_rows['total'],36);self.assertEqual(all_rows['records'][0]['serial'],'OTHER-CAMERA')
        self.proxy.impair=True
        try:impaired=self.command('record-query-all 0000000000000000 0 0 0',seconds=90)
        finally:self.proxy.impair=False
        self.assertEqual(impaired,all_rows)
        first=self.query(remote=True);self.assertTrue(first['ok']);self.assertEqual(first['total'],35)
        self.assertEqual([int(r['id'],16) for r in first['records']],list(range(35,19,-1)))
        self.delete(30);new=self.clip();self.assertGreater(int(new['id'],16),36)
        second=self.query(first['next_before'],first['storage_generation'],remote=True)
        self.assertEqual([int(r['id'],16) for r in second['records']],list(range(19,3,-1)))
        last=self.query(second['next_before'],second['storage_generation']);self.assertEqual([int(r['id'],16) for r in last['records']],[3,2,1]);self.assertFalse(last['has_more'])
        filtered=self.query(begin=STAMP+10,end=STAMP+20);self.assertEqual(filtered['total'],10)
        self.assertEqual([int(r['id'],16) for r in filtered['records']],list(range(19,9,-1)))
        self.assertTrue(all(r['storage']=='tf' and r['serial']==CAM and r['slot']==0 and r['duration_ms']>0 for r in filtered['records']))
        self.assertEqual(self.query(begin=STAMP+35,end=STAMP+37)['total'],0)
        self.assertEqual(self.query(first['next_before'],first['storage_generation']+1)['error'],'storage_generation_conflict')
        self.assertEqual(self.query(first['next_before'])['error'],'invalid_record_query')
        self.assertEqual(self.query(begin=STAMP,end=STAMP)['error'],'invalid_record_query')
        self.assertEqual(self.command('if-serial WRONG record-query 0 0000000000000000 0 0 0')['error'],'camera_identity_changed')

    def test_details_active_deleted_readers_and_catalog_failure_are_distinct(self):
        self.begin();active=sdk.request(self.station_socket,f'if-serial {CAM} record-info 0 {self.record_id}')
        self.assertEqual(active['state'],'recording');self.assertIsNotNone(active['record'])
        self.assertEqual(sdk.request(self.station_socket,f'if-serial {CAM} record-delete 0 {self.record_id}')['error'],'recording_active')
        large=[(i*70,1,self.frames[0][2]+b'\x55'*180000) for i in range(12)]
        self.send_frames(large);self.wait(lambda:self.local_status()['devices'][0]['recording']['frames']==12)
        row=self.finish()['records'][0]
        info=sdk.request(self.station_socket,f'if-serial {CAM} record-info 0 {row["id"]}')
        self.assertEqual(info['state'],'committed');self.assertEqual(info['record'],row)
        peer=socket.socket(socket.AF_UNIX);peer.settimeout(4);peer.connect(str(self.station_socket))
        try:
            peer.sendall(f'if-serial {CAM} record-get 0 {row["id"]}\n'.encode());header=b''
            while not header.endswith(b'\n'):header+=peer.recv(1)
            self.assertTrue(json.loads(header)['ok'])
            self.assertEqual(sdk.request(self.station_socket,f'if-serial {CAM} record-delete 0 {row["id"]}')['error'],'record_in_use')
        finally:peer.close()
        self.wait(lambda:sdk.request(self.station_socket,f'if-serial {CAM} record-delete 0 {row["id"]}')['ok'])
        self.assertEqual(sdk.request(self.station_socket,f'if-serial {CAM} record-delete 0 {row["id"]}')['error'],'record_not_found')
        missing=sdk.request(self.station_socket,f'if-serial {CAM} record-info 0 {row["id"]}')
        self.assertEqual(missing['state'],'missing');self.assertIsNone(missing['record'])

    def test_unreadable_catalog_never_reports_a_known_clip_as_missing(self):
        self.seed([(i,STAMP+i,CAM,0,1) for i in range(1,65)]);self.boot(self.archive_options())
        # Fill the 32-row cache with high IDs, then damage only the disposable cache.
        self.query();self.query('0000000000000031',self.local_status()['storage']['generation'])
        (self.directory/'.catalog.cache').write_bytes(b'')
        result=sdk.request(self.station_socket,f'if-serial {CAM} record-info 0 0000000000000001')
        self.assertFalse(result['ok']);self.assertNotEqual(result.get('state'),'missing')
        self.wait(lambda:self.local_status()['storage']['available'])
        recovered=sdk.request(self.station_socket,f'if-serial {CAM} record-info 0 0000000000000001')
        self.assertEqual(recovered['state'],'committed')

class RecordWebTests(ExportTests):
    def test_detection_persists_with_clip_and_preview_is_small_independent_and_readable(self):
        row=self.pir_clip(cloud=False,detection=(1,5))
        self.assertEqual(row['file_type'],6);self.assertEqual(row['detection_type_raw'],5)
        self.assertTrue(row['detection_reported']);self.assertEqual(row['detection_result_raw'],1)
        self.restart_station();self.ready_remote()
        restored=sdk.request(self.station_socket,f'if-serial {CAM} record-info 0 {row["id"]}')['record']
        self.assertEqual(restored,row)
        token=self.login();self.user(token,'viewer','viewer');viewer=self.login('viewer','Synthetic-User-Password')
        alias='b000000000000001';root='/api/v1/devices/BASE_A/files/'+alias
        code,task,_=self.submit_web(viewer,{'command':'record-preview','args':['0',row['id'],alias],'camera_serial':CAM});self.assertEqual(code,200,task)
        task=self.wait(lambda:(t:=self.web(f'/api/v1/devices/BASE_A/tasks/{task["epoch"]}/{task["id"]}',token=viewer)[1]).get('state')=='completed' and t)
        self.assertTrue(json.loads(task['result'])['ok'],task)
        info=self.wait(lambda:(r:=self.web(root+'/info',token=viewer))[0]==200 and r[1],45)
        self.assertEqual(info['storage'],'tf_preview');self.assertEqual(info['file_type'],6)
        self.assertLess(info['bytes'],row['bytes']);self.assertEqual(self.exports(),[])
        code,jpeg,_=self.web(root+'/thumbnail',token=viewer);self.assertEqual(code,200);self.assertTrue(jpeg.startswith(bytes([255,216])))
        for kind in ('cloud','tf_export'):
            self.assertEqual(self.web('/api/v1/devices/BASE_A/files?storage='+kind,token=token)[1]['total'],0)
        # Preview history has four separate, recyclable slots, and does not fill
        # the sixteen full-clip export slots after many pages.
        for i in range(2,7):
            alias=f'b00000000000000{i}'
            self.assertTrue(sdk.request(self.station_socket,f'if-serial {CAM} record-preview 0 {row["id"]} {alias}')['ok'])
            self.wait(lambda:self.web('/api/v1/devices/BASE_A/files/'+alias+'/info',token=token)[0]==200,45)
        self.assertTrue(self.request_export(row['id'])['ok']);self.done()
        self.assertEqual(self.web('/api/v1/devices/BASE_A/files/a000000000000001/info',token=token)[1]['file_type'],6)
        self.stop_proc(self.server);self.server=self.start_server();self.ready_remote()
        self.assertEqual(self.web(root+'/info',token=token)[1]['file_type'],6)

    def test_cancelled_preview_can_requeue_but_committed_identity_is_not_reused(self):
        row=self.pir_clip(cloud=False,detection=(1,1));self.stop_proc(self.server)
        alias='b0000000000000ff';line=f'if-serial {CAM} record-preview 0 {row["id"]} {alias}'
        self.assertTrue(sdk.request(self.station_socket,line)['ok'])
        self.assertTrue(sdk.request(self.station_socket,f'if-serial {CAM} export-cancel 0 {alias}')['ok'])
        self.assertTrue(sdk.request(self.station_socket,line)['ok'])
        self.server=self.start_server();self.ready_remote();token=self.login()
        self.wait(lambda:self.web('/api/v1/devices/BASE_A/files/'+alias+'/info',token=token)[0]==200,45)
        self.wait(lambda:sdk.request(self.station_socket,line).get('error')=='export_unavailable_or_conflict',45)
        self.assertTrue(sdk.request(self.station_socket,f'if-serial {CAM} record-preview 0 {row["id"]} b0000000000000fe')['ok'])
        self.wait(lambda:self.web('/api/v1/devices/BASE_A/files/b0000000000000fe/info',token=token)[0]==200,45)
        self.assertEqual(self.exports(),[])

    def test_unknown_raw_type_is_not_mistaken_for_app_face_type(self):
        row=self.pir_clip(cloud=False,detection=(1,7))
        self.assertEqual(row['file_type'],0);self.assertEqual(row['detection_type_raw'],7)
        self.assertTrue(row['detection_reported'])
        self.restart_station();self.assertEqual(sdk.request(self.station_socket,f'if-serial {CAM} record-info 0 {row["id"]}')['record'],row)

    def test_raw_movement_report_maps_to_archive_movement(self):
        row=self.pir_clip(cloud=False,detection=(1,1));self.assertEqual(row['file_type'],5)

    def test_viewer_query_and_details_cross_http_and_dtls_without_write_permission(self):
        row=self.pir_clip(cloud=False);admin=self.login();self.user(admin,'viewer','viewer');viewer=self.login('viewer','Synthetic-User-Password')
        for name,args in [('record-query',['0','0000000000000000','0','0','0']),('record-info',['0',row['id']])]:
            code,task,_=self.submit_web(viewer,{'command':name,'args':args,'camera_serial':CAM});self.assertEqual(code,200,task)
            task=self.wait(lambda:(t:=self.web(f'/api/v1/devices/BASE_A/tasks/{task["epoch"]}/{task["id"]}',token=viewer)[1]).get('state')=='completed' and t)
            result=json.loads(task['result']);self.assertTrue(result['ok'],result)
            record=result['records'][0] if name=='record-query' else result['record'];self.assertEqual(record,row)
        self.assertEqual(self.submit_web(viewer,{'command':'record-delete','args':['0',row['id']],'camera_serial':CAM})[0],403)

if __name__=='__main__':
    suite=unittest.TestSuite(cls(name) for cls in (RecordApiTests,RecordWebTests) for name in sorted(cls.__dict__) if name.startswith('test_'))
    raise SystemExit(not unittest.TextTestRunner(verbosity=2).run(suite).wasSuccessful())
