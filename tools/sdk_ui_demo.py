#!/usr/bin/env python3
"""Disposable loopback SDK/Web/camera fixture. No hardware or vendor services."""
import argparse,json,signal,sys,time,shutil
from pathlib import Path
root=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--build-dir',default='build');p.add_argument('--duration',type=int,default=1800);p.add_argument('--echo-settings',action='store_true',help='Synthetic command echoes for UI flow checks; no camera effects simulated');args=p.parse_args()
sys.path.insert(0,str(root/'tests'));sys.argv=['sdk_ui_demo',str(root/args.build_dir/'vava-station/vava-agent'),str(root/args.build_dir/'vava-server/vava-server')]
from sdk_web_media import WebMediaTests
from sdk_uploads import sdk
class Demo(WebMediaTests):
    def connect_camera(self):
        status=self.local_status()
        if status['devices']:
            self.camera=sdk.camera_sim.Camera('127.0.0.1',status['ports'],'SIM-EVENT-0001',b'\0\0\0\1\x65test',False)
        else:
            return super().connect_camera()
    def start_server(self):
        self.window_seconds=60
        return super().start_server()
Demo.setUpClass();t=Demo();signal.signal(signal.SIGTERM,lambda *_:(_ for _ in ()).throw(KeyboardInterrupt()))
try:
    t.setUp();t.stop_proc(t.station);mount=t.root/'nas';mount.mkdir(mode=0o700)
    t.station_extra=['--nas-host','nas.local','--nas-export','/archive','--nas-mount',str(mount),'--nas-station-id','BASE_A','--nas-lab-local','1']
    t.station=t.start_station();t.ready_remote();row=t.pir_clip(cloud=False)
    assert sdk.request(t.station_socket,'if-serial SIM-EVENT-0001 name 0 '+ '门前摄像头'.encode().hex())['ok']
    sdk.request(t.station_socket,'if-serial SIM-EVENT-0001 policy-config 0 nas=1')
    t.wait(lambda:not t.local_status()['devices'][0]['recording']['finishing']);t.restart_station();t.ready_remote();t.pir_clip(cloud=True);t.committed();t.cloud(False);t.drain_events()
    # Serve fresh source assets, independent of stale configured build copies.
    assets=t.root/'web';assets.mkdir(mode=0o700)
    for folder,names in [('vava-server/web',('index.html','management.js','config.js','style.css','app.js','api-client.js'))]:
        for name in names:shutil.copyfile(root/folder/name,assets/name)
    index=assets/'index.html';index.write_text(index.read_text().replace('<body class="management">', '<body class="management"><div style="padding:7px 18px;background:#edf3fc;color:#2f67ce;text-align:center;font-size:11px">演示环境 · 使用模拟基站与摄像头，所有操作仅影响临时测试数据</div>'))
    t.stop_proc(t.server);t.server_extra[t.server_extra.index('--web-dir')+1]=str(assets);t.server=t.start_server();t.ready_remote()
    print(json.dumps({'url':f'http://127.0.0.1:{t.web_port}','username':'admin','password':t.password,'root':str(t.root),'recording':row['id'],'station_socket':str(t.station_socket)},ensure_ascii=False),flush=True)
    began=time.monotonic();last=-1;last_audio=-1;echo_cursor=len(t.camera.received_control)
    video_frames=[f for f in t.frames if f[1]!=8];audio_frames=[f for f in t.frames if f[1]==8]
    while time.monotonic()-began<args.duration:
        t.camera.step(.01)
        if args.echo_settings:
            for command,payload in t.camera.received_control[echo_cursor:]:
                if command in (10,11,12,15,16,17,18,33,38,43):
                    t.camera.ctrl.sendall(sdk.camera_sim.control(command,payload))
            echo_cursor=len(t.camera.received_control)
        tick=int((time.monotonic()-began)*15)
        if t.camera.live and tick!=last:
            last=tick;frame=video_frames[tick%len(video_frames)]
            t.send_frames([(round(tick*1000/15),frame[1],frame[2])],1)
        audio_tick=int((time.monotonic()-began)/.128)
        if t.camera.live and audio_tick!=last_audio:
            last_audio=audio_tick;frame=audio_frames[audio_tick%len(audio_frames)]
            t.send_frames([(audio_tick*128,8,frame[2])],1)
except KeyboardInterrupt:pass
finally:
    t.doCleanups();Demo.tearDownClass()
