const test=require('node:test');
const assert=require('node:assert/strict');
const {createClient,normalizeBase,fromCommandLine,envelope}=require('../vava-server/web/api-client.js');
test('API endpoints can be remote or proxied; credentials never belong in URLs',()=>{
 assert.equal(normalizeBase('http://192.168.1.20:8080/'),'http://192.168.1.20:8080');
 assert.equal(normalizeBase('/station/'),'/station');
 for(const base of ['file:///tmp/ui','javascript:foo','https://user:secret@station','http://station/?token=bad','//station','/station/../other'])assert.throws(()=>normalizeBase(base));
});
test('command adapter preserves device and revision guards in JSON',()=>{
 assert.deepEqual(fromCommandLine('if-serial SIM-1 if-revision 7 configure-many 0 mirror=3 ir=1'),
  {command:'configure-many',args:['0','mirror=3','ir=1'],camera_serial:'SIM-1',expected_revision:7});
 for(const line of ['status\nforget 0','if-serial A if-serial B settings 0','if-revision x name 0 61'])assert.throws(()=>fromCommandLine(line));
 assert.throws(()=>envelope('name',['0','hello world']));
 assert.throws(()=>envelope('name',[{}]));
});
test('one API client covers standalone settings and streaming downloads without cookies or redirects',async()=>{
 const calls=[];
 const client=createClient({baseUrl:'https://station.example/proxy/',token:'test-token-12345678901234567890123',
  fetchImpl:async(url,options)=>{calls.push([url,options]);return new Response('{"ok":true}',{headers:{'Content-Type':'application/json'}});}});
 await client.status();await client.capabilities();
 await client.camera({slot:2,serial:'CAM-2'},'configure',['mirror',3],{expectedRevision:8});
 const signal=new AbortController().signal;
 const response=await client.commandRaw('record-get',['2','0000000000000001'],{cameraSerial:'CAM-2',signal});
 assert.ok(response instanceof Response);
 assert.equal(calls[0][0],'https://station.example/proxy/api/v1/status');
 assert.deepEqual(JSON.parse(calls[2][1].body),{command:'configure',args:['2','mirror','3'],camera_serial:'CAM-2',expected_revision:8});
 assert.equal(calls[3][1].signal,signal);
 for(const [,request] of calls){assert.equal(request.credentials,'omit');assert.equal(request.redirect,'error');assert.match(request.headers.Authorization,/^Bearer /);}
});
test('API client exposes stable backend errors including HTTP status',async()=>{
 const client=createClient({token:'test-token-12345678901234567890123',fetchImpl:async()=>new Response('{"ok":false,"error":"configuration_conflict"}',{status:422})});
 await assert.rejects(client.command('status',[]),e=>e.code==='configuration_conflict'&&e.status===422);
});
