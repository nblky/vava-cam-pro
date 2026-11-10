#!/usr/bin/env python3
import hashlib
import shutil
import subprocess
import unittest
from sdk_uploads import UploadTests
from sdk_web import WebTests

class WebMediaTests(UploadTests,WebTests):
    web=WebTests.web
    login=WebTests.login
    user=WebTests.user
    def start_server(self):
        if '--media-ffmpeg' not in getattr(self,'server_extra',[]):
            self.server_extra=getattr(self,'server_extra',[])+['--media-ffmpeg',shutil.which('ffmpeg')]
        return WebTests.start_server(self)

    def test_authorized_download_mp4_playback_delete_and_tombstone(self):
        row=self.pir_clip();self.committed();token=self.login()
        root='/api/v1/devices/BASE_A/files/'+row['id']
        self.assertEqual(self.web(root+'/download')[0],401)
        code,data,headers=self.web(root+'/download',token=token)
        self.assertEqual(code,200);self.assertEqual(hashlib.sha256(data).hexdigest(),row['sha256'])
        self.assertEqual(headers['X-Content-SHA256'],row['sha256'])
        code,jpeg,headers=self.web(root+'/thumbnail',token=token)
        self.assertEqual(code,200);self.assertEqual(headers['Content-Type'],'image/jpeg');self.assertTrue(jpeg.startswith(b'\xff\xd8') and jpeg.endswith(b'\xff\xd9'))
        code,mp4,headers=self.web(root+'/playback',token=token)
        self.assertEqual(code,200,mp4);self.assertEqual(headers['Content-Type'],'video/mp4')
        target=self.root/'playback.mp4';target.write_bytes(mp4)
        probe=self.probe(target);self.assertEqual({s['codec_name'] for s in probe['streams']},{'h264','aac'})
        audio=next(s for s in probe['streams'] if s['codec_name']=='aac');self.assertEqual(audio['sample_rate'],'48000')
        result=subprocess.run(['ffmpeg','-v','error','-xerror','-i',str(target),'-f','null','-'],capture_output=True,timeout=10)
        self.assertEqual(result.returncode,0,result.stderr)
        self.user(token,'viewer','viewer');reader=self.login('viewer','Synthetic-User-Password')
        self.assertEqual(self.web(root+'/delete',{},reader)[0],403)
        self.assertEqual(self.web(root.replace('BASE_A','BASE_B')+'/download',token=reader)[0],403)
        self.assertEqual(self.web(root+'/delete',{},token)[0],200)
        self.assertEqual(self.web(root+'/delete',{},token)[0],200)
        self.assertEqual(self.web(root+'/download',token=token)[0],404)
        self.assertEqual(self.remote(),[])
        self.stop_proc(self.server);self.server=self.start_server();self.ready_remote()
        self.assertEqual(self.web(root+'/download',token=token)[0],404)
        self.assertEqual(self.remote(),[])
        self.assertTrue((self.files/('BASE_A.'+row['id']+'.deleted')).exists())

    def test_event_categories_filters_and_clip_projection_survive_restart(self):
        row=self.pir_clip();self.committed();token=self.login();self.drain_events()
        endpoint='/api/v1/devices/BASE_A/events'
        def events(query=''):
            status,data,_=self.web(endpoint+query,token=token);self.assertEqual(status,200,data);return data['events']
        related=[e for e in events('?category=recording&serial=SIM-EVENT-0001') if e['event']['related_id']==row['id']]
        self.assertTrue(related);self.assertTrue(all(e['clip_state_reported']=='committed' for e in related))
        self.assertTrue(all(e['event']['category']=='recording' for e in related));self.assertEqual(events('?serial=NO-SUCH-CAMERA'),[])
        system=events('?category=system');self.assertTrue(system);self.assertTrue(all(e['event']['category']=='system' for e in system))
        for query in ('?category=person','?before=bad','?serial=%00','?serial=a&serial=b','?unknown=a'):
            self.assertEqual(self.web(endpoint+query,token=token)[0],400)
        self.stop_proc(self.server);self.server=self.start_server();self.ready_remote()
        restored=[e for e in events('?category=recording') if e['event']['related_id']==row['id']]
        self.assertEqual([e['id'] for e in restored],[e['id'] for e in related]);self.assertTrue(all(e['clip_state_reported']=='committed' for e in restored))

    def test_server_retention_is_opt_in_durable_and_does_not_delete_tf(self):
        row=self.pir_clip();self.committed();token=self.login()
        self.stop_proc(self.server);self.server_extra+=['--file-limit','1'];self.server=self.start_server();self.ready_remote()
        endpoint='/api/v1/archive-policy';code,policy,_=self.web(endpoint,token=token);self.assertEqual(code,200);self.assertFalse(policy['enabled'])
        self.assertEqual(len(self.remote()),1)
        self.user(token,'viewer','viewer');viewer=self.login('viewer','Synthetic-User-Password')
        self.assertEqual(self.web(endpoint,token=viewer)[0],403)
        self.assertEqual(self.web(endpoint,{'revision':policy['revision']+1,'enabled':True,'max_days':0},token)[0],409)
        self.assertEqual(self.web(endpoint,{'revision':policy['revision'],'enabled':True,'max_days':0},token)[0],200)
        self.wait(lambda:not self.remote(),10)
        self.assertEqual(len(__import__('sdk_uploads').sdk.request(self.station_socket,'if-serial SIM-EVENT-0001 record-list 0')['records']),1)
        self.stop_proc(self.server);self.server=self.start_server();self.ready_remote()
        restored=self.web(endpoint,token=token)[1];self.assertTrue(restored['enabled']);self.assertEqual(restored['revision'],policy['revision']+1)
        self.assertTrue((self.files/('BASE_A.'+row['id']+'.deleted')).exists())

    def test_corrupted_archive_is_not_exposed_as_valid_media(self):
        row=self.pir_clip();self.committed();token=self.login()
        path=self.files/('BASE_A.'+row['id']+'.ts');data=bytearray(path.read_bytes());data[256]^=1;path.write_bytes(data)
        for action in ['download','playback']:
            self.assertEqual(self.web('/api/v1/devices/BASE_A/files/'+row['id']+'/'+action,token=token)[0],404)
        self.assertTrue(self.command('status')['ok'])

    def test_delayed_upload_cannot_resurrect_deleted_archive(self):
        row=self.pir_clip();self.committed();token=self.login()
        self.stop_proc(self.station)
        if self.camera:self.camera.close();self.camera=None
        # Model a committed upload whose receipt was lost before durable local
        # publication. The next process must offer the same immutable object.
        (self.root/'station/recordings/.remote'/(row['id']+'.done')).unlink()
        (self.root/'station/recordings/.remote/jobs'/(row['id']+'.job')).unlink()
        self.assertEqual(self.web('/api/v1/devices/BASE_A/files/'+row['id']+'/delete',{},token)[0],200)
        self.station=self.start_station();self.wait(lambda:self.try_request(self.station_socket,'status'));self.ready_remote()
        failed=self.wait(lambda: next((r for r in self.uploads() if r['state']=='failed'),None),40)
        self.assertEqual(failed['error'],'server_deleted_file')
        self.assertEqual(self.remote(),[])
        self.assertFalse((self.files/('BASE_A.'+row['id']+'.ts')).exists())
        self.assertTrue(self.command('if-serial SIM-EVENT-0001 settings 0')['ok'])

if __name__=='__main__':
    suite=unittest.TestSuite(WebMediaTests(n) for n in sorted(WebMediaTests.__dict__) if n.startswith('test_'))
    result=unittest.TextTestRunner(verbosity=2).run(suite)
    raise SystemExit(not result.wasSuccessful())
