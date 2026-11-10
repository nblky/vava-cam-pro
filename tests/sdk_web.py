#!/usr/bin/env python3
"""Management HTTP(S), durable sessions, ACLs and real SDK command dispatch."""
import http.client
import json
import secrets
import socket
import ssl
import subprocess
import time
import unittest
from sdk_integration import SDKTests, SERVER, STATION, ROOT

class WebTests(SDKTests):
    def start_server(self):
        if not hasattr(self, 'web_port'):
            with socket.socket() as s:
                s.bind(('127.0.0.1',0)); self.web_port=s.getsockname()[1]
            self.password='Synthetic-Admin-Password-1'
            self.bootstrap=self.private('admin.password',self.password+'\n')
        if '--web-port' not in getattr(self, 'server_extra', []):
            self.server_extra=getattr(self,'server_extra',[])+['--web-port',str(self.web_port),'--web-admin-password-file',str(self.bootstrap),
                               '--web-dir',str(__import__('pathlib').Path(SERVER).parent.parent/'server-web')]
        return super().start_server()

    def web(self,path,body=None,token=None,headers=None,method=None,raw=None,tls=None):
        cls=http.client.HTTPSConnection if tls else http.client.HTTPConnection
        kwargs={'context':tls} if tls else {}
        c=cls('127.0.0.1',self.web_port,timeout=10,**kwargs)
        h={'Authorization':'Bearer '+token} if token else {}
        if body is not None or raw is not None: h['Content-Type']='application/json'
        h.update(headers or {})
        c.request(method or ('POST' if body is not None or raw is not None else 'GET'),path,
                  raw if raw is not None else json.dumps(body) if body is not None else None,h)
        r=c.getresponse(); data=r.read(); status=r.status; rh=dict(r.getheaders()); c.close()
        try: data=json.loads(data)
        except (ValueError,UnicodeDecodeError): pass
        return status,data,rh

    def login(self,name='admin',password=None,tls=None):
        time.sleep(.52)
        code,j,_=self.web('/api/v1/login',{'username':name,'password':password or self.password},tls=tls)
        self.assertEqual(code,200,j)
        return j['token']

    def user(self,admin,name,role,devices='BASE_A',disabled=False):
        c,j,_=self.web('/api/v1/users',dict(username=name,password='Synthetic-User-Password',role=role,devices=devices,disabled=disabled),admin)
        self.assertEqual(c,200,j)

    def submit_web(self,token,command,device='BASE_A'):
        c,d,_=self.web('/api/v1/devices',token=token);self.assertEqual(c,200,d)
        base=next(v for v in d['devices'] if v['id']==device)
        body={'boot':base['boot'],'epoch':d['epoch'],'request_key':d['request_prefix']+secrets.token_hex(8),'command':command}
        c,j,_=self.web(f'/api/v1/devices/{device}/commands',body,token)
        return c,j,body

    def test_web_dispatch_dedup_and_guarded_configuration(self):
        token=self.login()
        c,t,b=self.submit_web(token,{'command':'status','args':[]});self.assertEqual(c,200,t);self.assertTrue(t['ok'])
        url=f"/api/v1/devices/BASE_A/tasks/{t['epoch']}/{t['id']}"
        result=self.wait(lambda:(x:=self.web(url,token=token)[1]).get('state')=='completed' and x)
        self.assertIn('devices',json.loads(result['result']))
        c,dup,_=self.web('/api/v1/devices/BASE_A/commands',b,token);self.assertEqual(dup['id'],t['id']);self.assertTrue(dup['duplicate'])
        b['command']={'command':'pair-close','args':[]}
        _,bad,_=self.web('/api/v1/devices/BASE_A/commands',b,token);self.assertFalse(bad['ok'])
        c,j,_=self.web('/api/v1/devices/BASE_A/commands',{'boot':b['boot'],'epoch':b['epoch'],'request_key':b['request_key'],'command':{'command':'configure','args':['0','mirror','1']}},token)
        self.assertEqual(c,400);self.assertEqual(j['error'],'camera_serial_required')
        c,html,h=self.web('/');self.assertEqual(c,200);self.assertIn(b'management.js',html);self.assertIn('nosniff',h['X-Content-Type-Options'])
        self.assertNotIn(self.secret.encode(),html)

    def test_web_roles_scope_revocation_and_last_admin(self):
        admin=self.login();self.user(admin,'reader','viewer')
        reader=self.login('reader','Synthetic-User-Password')
        c,d,_=self.web('/api/v1/devices',token=reader);self.assertEqual([x['id'] for x in d['devices']],['BASE_A'])
        self.assertEqual(self.web('/api/v1/devices/BASE_B/events',token=reader)[0],403)
        self.assertEqual(self.web('/api/v1/users',token=reader)[0],403)
        self.assertEqual(self.submit_web(reader,{'command':'pair-open','args':['120']})[0],403)
        self.assertEqual(self.submit_web(reader,{'command':'nas-config','args':['1','1','nas.local','/archive']})[0],403)
        self.assertEqual(self.submit_web(reader,{'command':'nas-control','args':['1','unmount']})[0],403)
        self.user(admin,'controller','operator');controller=self.login('controller','Synthetic-User-Password')
        for actor in (reader,controller):
            for command,args in [('reset',['0']),('remove',['0']),('remove-confirm',['0','0000000000000001'])]:
                self.assertEqual(self.submit_web(actor,{'command':command,'args':args,'camera_serial':'CAMERA-A'})[0],403)
        c,t,_=self.submit_web(reader,{'command':'nas-status','args':[]});self.assertEqual(c,200,t)
        self.wait(lambda:self.web(f'/api/v1/devices/BASE_A/tasks/{t["epoch"]}/{t["id"]}',token=reader)[1].get('state')=='completed')
        c,t,_=self.submit_web(reader,{'command':'status','args':[]});self.assertEqual(c,200);self.assertTrue(t['ok'])
        self.user(admin,'reader','viewer',disabled=True)
        self.assertEqual(self.web('/api/v1/session',token=reader)[0],401)
        c,j,_=self.web('/api/v1/users',{'username':'admin','role':'viewer','devices':'*','disabled':False},admin)
        self.assertEqual(c,409,j)
        self.assertEqual(self.web('/api/v1/session',token=admin)[0],200)
        self.assertEqual(self.web('/api/v1/logout',{},admin)[0],200)
        self.assertEqual(self.web('/api/v1/session',token=admin)[0],401)

    def test_web_strict_envelopes_origins_and_private_credentials(self):
        token=self.login()
        for raw in ['{"username":"admin","username":"admin","password":"anything"}',
                    '{"username":"admin","password":"bad",}', '{"username":"admin","password":"\\u0000x"}', '{"username":[[[[[[[[[[]]]]]]]]]]}']:
            self.assertEqual(self.web('/api/v1/login',raw=raw)[0],400)
        self.assertEqual(self.web('/api/v1/session',token=token,headers={'Origin':'https://untrusted.test'})[0],403)
        self.assertEqual(self.web('/api/v1/users',{},token,headers={'Content-Type':'text/plain'})[0],400)
        self.assertEqual(self.web('/api/v1/session',token=token+'0')[0],401)
        self.assertEqual(self.web('/api/v1/session',method='DELETE',token=token)[0],405)
        self.assertEqual(self.web('/../devices.keys')[0],404)
        self.assertEqual(self.web('/api/v1/devices/BASE_A/commands',raw='{"command":null}',token=token)[0],400)
        for path in ['/api/v1/devices','/api/v1/users','/api/v1/session']:
            _,j,_=self.web(path,token=token); self.assertNotIn(self.secret,json.dumps(j));self.assertNotIn(self.password,json.dumps(j))

    def test_web_restart_password_and_read_cursor(self):
        token=self.login();self.wait(lambda:self.web('/api/v1/devices/BASE_A/events',token=token)[1].get('events'))
        _,events,_=self.web('/api/v1/devices/BASE_A/events',token=token);latest=events['events'][0]['id']
        self.assertEqual(self.web('/api/v1/devices/BASE_A/events/read',{'through':latest},token)[0],200)
        self.assertEqual(self.web('/api/v1/devices/BASE_A/events/read',{'through':'7fffffffffffffff'},token)[0],400)
        self.server.terminate();self.server.wait(8);self.bootstrap.unlink();self.server=self.start_server();self.online()
        self.assertEqual(self.web('/api/v1/session',token=token)[0],200)
        self.assertEqual(self.web('/api/v1/devices/BASE_A/events',token=token)[1]['read_through'],latest)
        time.sleep(.52)
        self.assertEqual(self.web('/api/v1/password',{'old_password':self.password,'new_password':'Changed-Private-Password'},token)[0],200)
        self.assertEqual(self.web('/api/v1/session',token=token)[0],401)
        self.login(password='Changed-Private-Password')

    def test_web_tls_and_explicit_frontend_origin(self):
        self.server.terminate();self.server.wait(8)
        cert=self.root/'cert.pem';key=self.root/'cert.key'
        subprocess.run(['openssl','req','-x509','-newkey','rsa:2048','-nodes','-keyout',str(key),'-out',str(cert),'-days','1','-subj','/CN=localhost','-addext','subjectAltName=IP:127.0.0.1'],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,check=True)
        self.server_extra+=['--web-cert',str(cert),'--web-key',str(key),'--web-origin','https://frontend.example']
        self.server=self.start_server();ctx=ssl.create_default_context(cafile=str(cert));token=self.login(tls=ctx)
        c,j,h=self.web('/api/v1/devices',token=token,tls=ctx,headers={'Origin':'https://frontend.example'});self.assertEqual(c,200,j);self.assertEqual(h['Access-Control-Allow-Origin'],'https://frontend.example')
        self.assertEqual(self.web('/api/v1/devices',token=token,tls=ctx,headers={'Origin':'https://another.example'})[0],403)
        c,_,_=self.web('/api/v1/devices',method='OPTIONS',tls=ctx,headers={'Origin':'https://frontend.example','Access-Control-Request-Method':'GET','Access-Control-Request-Headers':'authorization'});self.assertEqual(c,204)

if __name__=='__main__':
    suite=unittest.TestSuite(WebTests(n) for n in sorted(WebTests.__dict__) if n.startswith('test_'))
    result=unittest.TextTestRunner(verbosity=2).run(suite)
    raise SystemExit(not result.wasSuccessful())
