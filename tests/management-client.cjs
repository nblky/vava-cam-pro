const test=require('node:test');
const assert=require('node:assert/strict');
globalThis.crypto ??= require('node:crypto').webcrypto;
globalThis.VavaAPI=require('../vava-server/web/api-client.js');
const {createClient}=require('../vava-server/web/management.js');
function setup(options={}) {
 const calls=[],store=new Map();let state='completed',fail=false;
 const memory={getItem:k=>store.get(k)||null,setItem:(k,v)=>store.set(k,v),removeItem:k=>store.delete(k)};
 const fetchImpl=async(url,request)=>{
  calls.push({url,request});await options.beforeFetch?.(url,request);request.signal?.throwIfAborted();let j;
  if(url.endsWith('/session'))j={ok:true,user:{name:'owner',role:3,devices:'*'}};
  else if(url.endsWith('/devices'))j={ok:true,epoch:'a'.repeat(32),request_prefix:'b'.repeat(16),devices:[{id:'BASE_A',boot:'c'.repeat(32),online:true},{id:'BASE_B',boot:'d'.repeat(32),online:true}]};
  else if(url.endsWith('/commands')){if(fail)throw Error('connection lost');j=await options.commandReply?.()||{ok:true,id:'1'.repeat(16),epoch:'a'.repeat(32)};}
  else if(url.includes('/tasks/')||url.includes('/lookup/'))j={ok:true,state,id:'1'.repeat(16),epoch:'a'.repeat(32),result:'{"ok":true,"station_saved":true}'};
  else throw Error('unexpected request '+url);
  return new Response(JSON.stringify(j),{headers:{'Content-Type':'application/json'}});
 };
 return {client:createClient({baseUrl:'https://server.example',token:'session-secret',fetchImpl,storage:memory}),calls,store,setState:v=>state=v,setFailure:v=>fail=v,...options};
}
test('management bridge preserves SN/revision guards and waits for durable result',async()=>{
 const {client,calls}=setup();await client.capabilities();
 const r=await client.commandLineRaw('if-serial CAM1 if-revision 12 configure-many 0 mirror=1');assert.equal((await r.json()).station_saved,true);
 const post=calls.find(c=>c.url.endsWith('/commands')),body=JSON.parse(post.request.body);
 assert.deepEqual(body.command,{command:'configure-many',args:['0','mirror=1'],camera_serial:'CAM1',expected_revision:12});
 assert.match(body.request_key,/^b{16}[a-f0-9]{16}$/);assert.equal(body.boot,'c'.repeat(32));assert.equal(client.getPending(),null);
 assert.ok(calls.every(c=>!c.url.includes('session-secret')&&c.request.credentials==='omit'&&c.request.redirect==='error'));
});
test('unknown operation survives refresh and blocks a new mutation without replay',async()=>{
 const x=setup();await x.client.capabilities();x.setState('unknown');
 await assert.rejects(x.client.command('pair-open',['120']),e=>e.code==='unknown');assert.ok(x.client.getPending());
 const count=x.calls.filter(c=>c.url.endsWith('/commands')).length;
 await assert.rejects(x.client.command('pair-open',['120']),e=>e.code==='pending_result_unknown');
 assert.equal(x.calls.filter(c=>c.url.endsWith('/commands')).length,count);
 x.setState('completed');await x.client.command('status');assert.ok(x.client.getPending());
 await x.client.recover();assert.equal(x.client.getPending(),null);
});
test('a lost submission keeps exact key and asks lookup before any next command',async()=>{
 const x=setup();await x.client.capabilities();x.setFailure(true);
 await assert.rejects(x.client.command('pair-close'));const p=x.client.getPending();assert.ok(p&&!p.id);
 x.setFailure(false);await x.client.recover();assert.ok(x.calls.some(c=>c.url.endsWith('/lookup/'+p.request_key)));
 assert.equal(x.calls.filter(c=>c.url.endsWith('/commands')).length,1);
});

test('retry after a lost response returns the recovered result without executing twice',async()=>{
 const x=setup();await x.client.capabilities();x.setFailure(true);
 await assert.rejects(x.client.command('pair-close'));x.setFailure(false);
 const result=await x.client.command('pair-close');assert.equal(result.station_saved,true);
 assert.equal(x.calls.filter(c=>c.url.endsWith('/commands')).length,1);
});

test('concurrent queue rejection waits and retries the same request until accepted once',async()=>{
 let rejected=2;
 const x=setup({commandReply:()=>rejected-->0?{ok:false,error:'device_busy'}:null});await x.client.capabilities();
 const result=await x.client.command('pair-close');assert.equal(result.station_saved,true);
 const submissions=x.calls.filter(c=>c.url.endsWith('/commands')).map(c=>JSON.parse(c.request.body));
 assert.equal(submissions.length,3);assert.deepEqual(submissions[0],submissions[1]);assert.deepEqual(submissions[1],submissions[2]);
 assert.equal(x.calls.filter(c=>c.url.includes('/tasks/')).length,1);assert.equal(x.client.getPending(),null);
});

test('cancelling a rejected busy request leaves no uncertain mutation or later submission',async()=>{
 const ctl=new AbortController();
 const x=setup({commandReply:()=>{setTimeout(()=>ctl.abort(),0);return {ok:false,error:'device_busy'};}});await x.client.capabilities();
 await assert.rejects(x.client.command('pair-close',[],{signal:ctl.signal}),e=>e.name==='AbortError');
 assert.equal(x.calls.filter(c=>c.url.endsWith('/commands')).length,1);assert.equal(x.client.getPending(),null);
});

test('cloud clip opens cloud storage without waking the base',async()=>{
 const {resolveClip}=require('../vava-server/web/management.js');let commands=0;
 const client={endpoint:()=>'/devices/BASE_A',request:async()=>({serial:'CAM1',storage:'cloud'}),command:async()=>{commands++;}};
 assert.equal(await resolveClip(client,{id:'0000000000000001',slot:0,serial:'CAM1',storage:'cloud'}),'0000000000000001');assert.equal(commands,0);
});
test('cloud-off event clip exports TF once and preserves identity on every command',async()=>{
 const {resolveClip}=require('../vava-server/web/management.js');let exported='',commands=[];
 const client={endpoint:()=>'/devices/BASE_A',request:async path=>{if(exported&&path.includes(exported))return {serial:'CAM1',storage:'tf_export'};throw Object.assign(Error('missing'),{status:404});},command:async(name,args,opts)=>{
  commands.push({name,args,opts});if(name==='export-list')return {ok:true,exports:exported?[{id:exported,source_id:'0000000000000001',state:'committed'}]:[]};
  if(name==='record-info')return {ok:true,state:'committed'};if(name==='record-export'){exported=args[2];return {ok:true};}throw Error(name);
 }};
 assert.equal(await resolveClip(client,{id:'0000000000000001',slot:0,serial:'CAM1'}),exported);
 assert.match(exported,/^[89a-f][a-f0-9]{15}$/);assert.equal(commands.filter(c=>c.name==='record-export').length,1);
 assert.ok(commands.every(c=>c.opts.cameraSerial==='CAM1'&&c.args[0]===0));
});
test('verified TF cache reopens without station commands and keeps server identity checks',async()=>{
 const {resolveClip}=require('../vava-server/web/management.js');let commands=0,valid=true;
 const client={baseUrl:'http://cache-test',endpoint:()=>'/devices/CACHE_TEST',request:async()=>valid?{serial:'CACHE_CAM',storage:'tf_export'}:{serial:'OTHER_CAM',storage:'tf_export'},command:async()=>{commands++;return {ok:true,exports:[{id:'a000000000000099',source_id:'0000000000000099',state:'committed'}]};}};
 const clip={id:'0000000000000099',slot:0,serial:'CACHE_CAM'};
 assert.equal(await resolveClip(client,clip),'a000000000000099');assert.equal(commands,1);
 assert.equal(await resolveClip(client,clip),'a000000000000099');assert.equal(commands,1);
 valid=false;await assert.rejects(resolveClip(client,clip),/身份/);assert.equal(commands,1);
});
test('missing event source reports its state without a new export',async()=>{
 const {resolveClip}=require('../vava-server/web/management.js');let exports=0;
 const client={endpoint:()=>'',request:async()=>{throw Object.assign(Error('missing'),{status:404});},command:async name=>{if(name==='export-list')return {ok:true,exports:[]};if(name==='record-info')return {ok:true,state:'missing'};exports++;return {ok:false,error:'export_unavailable_or_conflict'};}};
 await assert.rejects(resolveClip(client,{id:'0000000000000001',slot:0,serial:'CAM1'}),/已清理/);assert.equal(exports,1);
});

test('missing cloud never starts a TF export; wrong source is rejected',async()=>{
 const {resolveClip}=require('../vava-server/web/management.js');let commands=0;
 const client={endpoint:()=>'',request:async()=>{throw Object.assign(Error('missing'),{status:404});},command:async()=>commands++};
 const clip={id:'0000000000000001',serial:'CAM1',slot:0,storage:'cloud'};
 await assert.rejects(resolveClip(client,clip),/云存储中没有/);assert.equal(commands,0);
 client.request=async()=>({serial:'CAM1',storage:'tf_export'});await assert.rejects(resolveClip(client,clip),/来源/);assert.equal(commands,0);
});
test('TF playback never chooses a cloud object and replaces a deleted cache',async()=>{
 const {resolveClip}=require('../vava-server/web/management.js');let exported='',requests=[];
 const source='0000000000000001',old='a000000000000001';
 const client={endpoint:()=>'',request:async path=>{requests.push(path);if(exported&&path.includes(exported))return {serial:'CAM1',storage:'tf_export'};if(path.includes(source))throw Error('must not query cloud ID');throw Object.assign(Error('missing'),{status:404});},command:async(name,args)=>{
  if(name==='export-list')return {ok:true,exports:[{id:old,source_id:source,state:'committed'},...(exported?[{id:exported,source_id:source,state:'committed'}]:[])]};
  if(name==='record-info')return {ok:true,state:'committed'};if(name==='record-export'){exported=args[2];return {ok:true};}throw Error(name);
 }};
 assert.equal(await resolveClip(client,{id:source,serial:'CAM1',slot:0}),exported);assert.notEqual(exported,old);assert.ok(requests.some(p=>p.includes(old)));
});
test('cloud and NAS policy writes each preserve the other setting',()=>{
 const {storagePolicyCommand}=require('../vava-server/web/management.js'),camera={serial:'CAM1',slot:2,policy:{revision:17,cloud_upload:true,nas_backup:true}};
 assert.equal(storagePolicyCommand(camera,'cloud',false),'if-serial CAM1 if-revision 17 policy-config 2 cloud=0');
 assert.equal(storagePolicyCommand(camera,'nas',false),'if-serial CAM1 if-revision 17 policy-config 2 nas=0');
 assert.throws(()=>storagePolicyCommand(camera,'both',true));
});
const fs=require('node:fs'),os=require('node:os'),path=require('node:path'),cp=require('node:child_process');
let recordingMedia;
function mp4Fixture(){
 if(recordingMedia)return recordingMedia;
 const r=cp.spawnSync('ffmpeg',['-v','error','-f','lavfi','-i','testsrc2=s=160x96:r=10','-f','lavfi','-i','sine=frequency=440:sample_rate=48000','-t','20','-c:v','libx264','-preset','ultrafast','-g','20','-bf','0','-c:a','aac','-movflags','frag_keyframe+empty_moov+default_base_moof','-frag_duration','250000','-f','mp4','pipe:1'],{maxBuffer:8*1024*1024});
 assert.equal(r.status,0,r.stderr?.toString());const b=r.stdout,boxes=[];
 for(let p=0;p<b.length;){const n=b.readUInt32BE(p);boxes.push({type:b.toString('ascii',p+4,p+8),data:b.subarray(p,p+n)});p+=n;}
 const init=Buffer.concat(boxes.filter(b=>['ftyp','moov'].includes(b.type)).map(b=>b.data)),fragments=[];
 for(let i=0;i<boxes.length;i++)if(boxes[i].type==='moof'){assert.equal(boxes[i+1].type,'mdat');fragments.push(Buffer.concat([boxes[i].data,boxes[++i].data]));}
 return recordingMedia={init,fragments};
}
function recordingFixture(options={}){
 const saves=[],errors=[],states=[],{createLocalRecording}=require('../vava-server/web/management.js');
 return {saves,errors,states,recorder:createLocalRecording({...options,save:(blob,ext)=>saves.push({blob,ext}),onError:e=>errors.push(e.message),onState:s=>states.push(s)})};
}
function chopped(recorder,data){for(let i=0;i<data.length;i+=137)recorder.push(data.subarray(i,i+137));}
async function probeRecording(blob){
 const dir=fs.mkdtempSync(path.join(os.tmpdir(),'myvava-recording-'));try{
  const file=path.join(dir,'recording.mp4');fs.writeFileSync(file,Buffer.from(await blob.arrayBuffer()));
  const decode=cp.spawnSync('ffmpeg',['-v','error','-xerror','-i',file,'-fps_mode','passthrough','-enc_time_base:v','demux','-f','null','-']);assert.equal(decode.status,0,decode.stderr.toString());assert.equal(decode.stderr.length,0);
  return JSON.parse(cp.execFileSync('ffprobe',['-v','error','-count_packets','-show_entries','format=duration,start_time:stream=codec_name,duration,start_time,nb_read_packets','-of','json',file]));
 }finally{fs.rmSync(dir,{recursive:true,force:true});}
}
test('local recording preserves encoded audio/video from a mid-stream keyframe with split HTTP boxes',async()=>{
 const {init,fragments}=mp4Fixture(),f=recordingFixture();assert.throws(()=>f.recorder.start(),/尚未/);chopped(f.recorder,init);
 fragments.slice(0,17).forEach(b=>chopped(f.recorder,b));f.recorder.start();fragments.slice(17,60).forEach(b=>chopped(f.recorder,b));
 const done=f.recorder.stop();assert.throws(()=>f.recorder.start(),/收尾/);assert.throws(()=>f.recorder.reset(),/尚未保存/);await done;
 assert.equal(f.saves.length,1);assert.equal(f.saves[0].ext,'.mp4');const result=await probeRecording(f.saves[0].blob);
 const v=result.streams.find(s=>s.codec_name==='h264'),a=result.streams.find(s=>s.codec_name==='aac');
 assert.ok(Number(v.duration)>8&&Number(v.duration)<13);assert.ok(Math.abs(Number(v.duration)-Number(a.duration))<.3);assert.ok(Number(v.nb_read_packets)>80);assert.ok(Number(result.format.start_time)<.1);assert.deepEqual(f.errors,[]);
 f.recorder.start();fragments.slice(60).forEach(b=>f.recorder.push(b));await f.recorder.stop();assert.equal(f.saves.length,2);await probeRecording(f.saves[1].blob);f.recorder.reset();assert.throws(()=>f.recorder.start(),/尚未/);
});
test('local recording caps memory at complete playable fragments and stops without a keyframe',async()=>{
 const {init,fragments}=mp4Fixture(),limit=init.length+fragments.slice(0,9).reduce((n,b)=>n+b.length,0),f=recordingFixture({maxBytes:limit});f.recorder.push(init);f.recorder.start();fragments.forEach(b=>f.recorder.push(b));await f.recorder.stop();
 assert.equal(f.saves.length,1);assert.ok(f.saves[0].blob.size<=limit);assert.equal(f.errors.length,1);await probeRecording(f.saves[0].blob);
 const empty=recordingFixture();empty.recorder.push(init);empty.recorder.start();await empty.recorder.stop();assert.equal(empty.saves.length,0);assert.match(empty.errors[0],/关键帧/);
});
test('local recording saves a valid prefix on truncation and rejects malformed container lengths',async()=>{
 const {init,fragments}=mp4Fixture(),f=recordingFixture();f.recorder.push(init);f.recorder.start();fragments.slice(0,8).forEach(b=>f.recorder.push(b));f.recorder.push(fragments[8].subarray(0,100));await f.recorder.stop();await probeRecording(f.saves[0].blob);
 f.recorder.reset();assert.throws(()=>f.recorder.push(Buffer.from([0,0,0,1,109,111,111,102])),/格式/);
});

function gate(){let resolve;const promise=new Promise(r=>resolve=r);return {promise,resolve};}
test('inflight and queued commands stay on the originating base after selection changes',async()=>{
 const entered=gate(),release=gate();let block=true;
 const x=setup({beforeFetch:async url=>{if(block&&url.includes('/tasks/')){entered.resolve();await release.promise;block=false;}}});
 await x.client.capabilities();const first=x.client.command('pair-open',['120']);await entered.promise;
 const queued=x.client.command('pair-close');x.client.select('BASE_B');
 x.store.set('myvavaPending:https://server.example:BASE_B','{"sentinel":true}');
 release.resolve();await Promise.all([first,queued]);
 assert.deepEqual(x.calls.filter(c=>c.url.endsWith('/commands')).map(c=>c.url),Array(2).fill('https://server.example/api/v1/devices/BASE_A/commands'));
 assert.ok(x.calls.filter(c=>c.url.includes('/tasks/')).every(c=>c.url.includes('/BASE_A/')));
 assert.equal(x.client.getPending('BASE_A'),null);assert.deepEqual(x.client.getPending(),{sentinel:true});
});
test('aborted queued mutations never submit or leave pending intent',async()=>{
 const entered=gate(),release=gate();let block=true;
 const x=setup({beforeFetch:async url=>{if(block&&url.includes('/tasks/')){entered.resolve();await release.promise;block=false;}}});
 await x.client.capabilities();const first=x.client.command('status');await entered.promise;
 const ctl=new AbortController(),queued=x.client.command('pair-close',[],{signal:ctl.signal});
 const rejected=assert.rejects(queued,e=>e.name==='AbortError');ctl.abort();release.resolve();await first;await rejected;
 assert.equal(x.calls.filter(c=>c.url.endsWith('/commands')).length,1);assert.equal(x.client.getPending(),null);
});
test('page scope pins endpoint and command base and cancels subsequent work',async()=>{
 const {scopedClient}=require('../vava-server/web/management.js');const x=setup();await x.client.capabilities();
 const ctl=new AbortController(),page=scopedClient(x.client,ctl.signal);x.client.select('BASE_B');
 assert.equal(page.endpoint(),'/devices/BASE_A');assert.equal(page.stationId,'BASE_A');await page.command('pair-close');
 assert.ok(x.calls.find(c=>c.url.endsWith('/commands')).url.includes('/BASE_A/'));
 ctl.abort();const count=x.calls.length;await assert.rejects(page.commandLineRaw('pair-open 120'),e=>e.name==='AbortError');assert.equal(x.calls.length,count);
 const other=new AbortController();other.abort();const livePage=scopedClient(x.client,new AbortController().signal);
 await assert.rejects(livePage.command('status',[],{signal:other.signal}),e=>e.name==='AbortError');
});

test('TF cursor paging carries medium generation and commits only successful pages',async()=>{
 const {createRecordingPager}=require('../vava-server/web/management.js');const calls=[];let fail=false;
 const client={command:async(name,args,guard)=>{calls.push({name,args,guard});if(name==='capabilities')return {ok:true,record_query:true};if(fail)return {ok:false,error:'storage_generation_conflict'};return {ok:true,records:[],total:40,storage_generation:7,has_more:true,next_before:args[1]==='0000000000000000'?'0000000000000019':'0000000000000009'};}};
 const pager=createRecordingPager(client,{slot:2,serial:'CAM2'});
 assert.equal((await pager.load('refresh',[100,200])).page,0);
 fail=true;await assert.rejects(pager.load('next'),e=>e.code==='storage_generation_conflict');fail=false;
 assert.equal((await pager.load('next')).page,1);assert.deepEqual(calls.at(-1).args,[2,'0000000000000019',7,100,200]);assert.equal(calls.at(-1).guard.cameraSerial,'CAM2');
 assert.equal((await pager.load('previous')).page,0);assert.deepEqual(calls.at(-1).args,[2,'0000000000000000',7,100,200]);
 await pager.load('refresh',[0,0]);assert.deepEqual(calls.at(-1).args,[2,'0000000000000000',0,0,0]);
});
test('TF pager supports old stations without pretending to filter a single page',async()=>{
 const {createRecordingPager,recordingRange}=require('../vava-server/web/management.js');const calls=[];
 const pager=createRecordingPager({command:async(name,args)=>{calls.push({name,args});return name==='capabilities'?{ok:true}:{ok:true,records:[],total:17};}},{slot:0,serial:'CAM'});
 assert.equal((await pager.load()).supports_query,false);await pager.load('next');assert.deepEqual(calls.at(-1),{name:'record-list',args:[0,1]});
 await assert.rejects(pager.load('refresh',[100,200]),/不支持时间范围/);
 assert.deepEqual(recordingRange('',''),[0,0]);assert.throws(()=>recordingRange('2026-11-07T00:00',''),/同时填写/);
 assert.throws(()=>recordingRange('2026-11-07T00:00','2026-11-06T00:00'),/结束时间/);
});
test('TF query remains read-only when an unrelated mutation result is pending',async()=>{
 const x=setup();await x.client.capabilities();x.setFailure(true);await assert.rejects(x.client.command('pair-close'));x.setFailure(false);
 await x.client.command('record-query',['0','0000000000000000','0','0','0'],{cameraSerial:'CAM'});
 assert.equal(x.client.getPending().command.command,'pair-close');
});
test('TF cursors cannot silently cross a base reboot even if mount generation repeats',async()=>{
 const {createRecordingPager}=require('../vava-server/web/management.js');let boot='old';
 const client={deviceBoot:()=>boot,command:async name=>name==='capabilities'?{ok:true,record_query:true}:{ok:true,records:[],total:17,storage_generation:1,has_more:true,next_before:'0000000000000002'}};
 const pager=createRecordingPager(client,{slot:0,serial:'CAM'});await pager.load();boot='new';
 await assert.rejects(pager.load('next'),e=>e.code==='storage_generation_conflict');assert.equal((await pager.load()).page,0);
});

const {createAdaptiveQuality,observedLiveQuality,switchLiveQuality}=require('../vava-server/web/management.js');
test('automatic quality has warmup, hysteresis, bounds and rolling budget',()=>{
 let time=0;const p=createAdaptiveQuality({now:()=>time});const sample=(q,poor=false,extra={})=>p.observe({quality:q,buffered:poor?0:1,stalled:poor,...extra});
 assert.equal(sample(0,true),null);time=7999;assert.equal(sample(0,true),null);time=8000;assert.equal(sample(0,true),1);
 for(let i=0;i<20;i++)assert.equal(sample(1),null);time=16000;assert.equal(sample(1),0);
 time=24000;assert.equal(sample(0,true),null);assert.equal(sample(0,true),1);time=32000;
 for(let i=0;i<20;i++)assert.equal(sample(1,true),null);assert.equal(p.changes,3);
});
test('automatic policy suspends for recording/paused playback and never sends auto enum',()=>{
 let time=30000;const p=createAdaptiveQuality({now:()=>time});time=60000;
 for(let i=0;i<10;i++)assert.equal(p.observe({quality:0,buffered:0,progressRatio:0,suspended:true}),null);
 assert.equal(p.observe({quality:0,buffered:0,progressRatio:0}),null);assert.equal(p.observe({quality:0,buffered:0,progressRatio:0}),1);
 time=90000;for(let i=0;i<5;i++)assert.equal(p.observe({quality:2,buffered:0,progressRatio:0}),null);
 assert.equal(p.observe({quality:3,buffered:0,progressRatio:0}),null);assert.equal(p.observe({quality:null,buffered:0,progressRatio:0}),null);
 p.reset();assert.equal(p.observe({quality:1,buffered:0,progressRatio:0}),null);
});
function qualityFixture({busy=0,other=0,same=false,timeout=false,conflict=false}={}){
 const calls=[];let time=0,paused=false,revision=4,written=false,boot='boot1',q=same?1:0;
 const signal=new AbortController();const settings=()=>({ok:true,desired:{revision,values:{quality:q},pending_mask:0,items:{quality:{phase:written?(timeout?'echo_timeout':'echo_observed'):'explicit_apply_required',current_connection:true,operation_id:written?7:0}}},settings:{pending:false,operations:{quality:written&&!timeout?{value:q,phase:'echo_observed',current_connection:true}:null},reports:{first_connect:{current_connection:true,values:{live_quality_raw:0}}}}});
 const client={deviceBoot:()=>boot,command:async(name,args,opts)=>{calls.push({name,args,opts});opts.signal?.throwIfAborted();
   if(name==='status'){assert.equal(opts.cameraSerial,undefined);return {ok:true,devices:[{slot:0,serial:'CAM1',paired:true,policy:{enabled:true},recording:{active:busy-->0,finishing:false},viewers:paused?(other-->0?1:0):1,live_requested:!paused}]};}
   if(name==='settings'){const j=settings();if(written&&conflict)j.desired.revision++;return j;}
   assert.ok(paused);assert.equal(opts.expectedRevision,4);assert.equal(opts.expectedBoot,'boot1');written=true;q=1;revision++;return {ok:true,desired:{revision}};
 }};
 const options={client,camera:{slot:0,serial:'CAM1'},quality:1,signal:signal.signal,deadline:10000,pause:async()=>{calls.push({name:'pause'});paused=true;},now:()=>time,wait:async n=>{time+=n;}};
 return {options,calls,signal,setBoot:v=>boot=v,setTime:v=>time=v};
}
test('quality switching waits for clip and viewers then guards revision and waits echo',async()=>{
 const x=qualityFixture({busy:2,other:2});assert.equal((await switchLiveQuality(x.options)).quality,1);
 const pause=x.calls.findIndex(c=>c.name==='pause'),write=x.calls.findIndex(c=>c.name==='configure');assert.equal(pause,4);assert.ok(write>pause+2);
 assert.ok(x.calls.findIndex(c=>c.name==='settings')<pause);
 assert.equal(x.calls.filter(c=>c.name==='configure').length,1);assert.ok(x.calls.filter(c=>c.opts&&c.name!=='status').every(c=>c.opts.cameraSerial==='CAM1'));
 assert.ok(!x.calls.some(c=>['arm','stop','record','enable'].includes(c.name)));
});
test('unchanged desired value without current echo explicitly applies once',async()=>{
 const x=qualityFixture({same:true});await switchLiveQuality(x.options);assert.equal(x.calls.filter(c=>c.name==='apply-settings').length,1);assert.equal(x.calls.filter(c=>c.name==='configure').length,0);
});
test('quality timeout/conflict/cancellation never automatically replays writes',async()=>{
 for(const mode of [{timeout:true},{conflict:true}]){const x=qualityFixture(mode);await assert.rejects(switchLiveQuality(x.options));assert.equal(x.calls.filter(c=>c.name==='configure').length,1);}
 const x=qualityFixture({busy:20});x.options.wait=async()=>x.signal.abort();await assert.rejects(switchLiveQuality(x.options));assert.ok(!x.calls.some(c=>c.name==='pause'||c.name==='configure'));
 const y=qualityFixture();y.setTime(10000);await assert.rejects(switchLiveQuality(y.options),/超时/);assert.equal(y.calls.length,0);
 const z=qualityFixture({other:40});await assert.rejects(switchLiveQuality(z.options),/超时/);assert.ok(!z.calls.some(c=>c.name==='configure'));
});
test('station reboot between reads and submission cannot redirect a quality write',async()=>{
 const x=setup();await x.client.capabilities();await assert.rejects(x.client.command('configure',[0,'quality',1],{expectedBoot:'old-boot'}),e=>e.code==='device_boot_changed');
 assert.ok(!x.calls.some(c=>c.url.endsWith('/commands')));
});
test('quality observation prefers current matched echo over startup snapshot',()=>{
 const j={settings:{operations:{quality:{phase:'echo_observed',current_connection:true,value:2}},reports:{first_connect:{current_connection:true,values:{live_quality_raw:0}}}}};
 assert.equal(observedLiveQuality(j),2);j.settings.operations.quality.current_connection=false;assert.equal(observedLiveQuality(j),0);
 j.settings.reports.first_connect.current_connection=false;assert.equal(observedLiveQuality(j),null);
});

test('automatic quality resumes after rolling window without ending live viewing',()=>{
 let now=0;const p=createAdaptiveQuality({now:()=>now,maxChanges:1});now=30000;
 assert.equal(p.observe({quality:0,buffered:0,progressRatio:0}),null);assert.equal(p.observe({quality:0,buffered:0,progressRatio:0}),1);
 now=200000;for(let i=0;i<10;i++)assert.equal(p.observe({quality:1,buffered:0,progressRatio:0}),null);
 now=330001;assert.equal(p.observe({quality:1,buffered:0,progressRatio:0}),2);assert.equal(p.changes,2);
});

const {waitLivePicture,pictureQuality}=require('../vava-server/web/management.js');
test('quality confirmation requires matching decoded dimensions and advancing playback',async()=>{
 let time=0,ticks=0;const video={videoWidth:640,videoHeight:360,readyState:4,paused:false,currentTime:0};
 const q=await waitLivePicture({video,quality:0,now:()=>time,wait:async ms=>{time+=ms;ticks++;if(ticks===3){video.videoWidth=1920;video.videoHeight=1080;}if(ticks>=5)video.currentTime+=.1;}});
 assert.equal(q,0);assert.ok(ticks>=5);assert.equal(pictureQuality(video),0);
});
test('quality confirmation rejects frozen, mismatched, failed and cancelled playback',async()=>{
 for(const mode of ['frozen','mismatch','failed','cancelled']){
  let time=0;const ctl=new AbortController(),video={videoWidth:640,videoHeight:360,readyState:4,paused:false,currentTime:0};
  await assert.rejects(waitLivePicture({video,quality:mode==='mismatch'?0:2,signal:ctl.signal,timeout:500,now:()=>time,wait:async ms=>{time+=ms;if(mode==='failed')video.error={code:3};if(mode==='cancelled')ctl.abort();if(mode==='mismatch')video.currentTime+=.2;}}));
 }
});

test('live picture accepts unclassified dimensions but waits for two decoded frames when available',async()=>{
 let now=0,frames=1,ticks=0;const video={videoWidth:128,videoHeight:96,readyState:4,paused:false,currentTime:0,getVideoPlaybackQuality:()=>({totalVideoFrames:frames})};
 const quality=await waitLivePicture({video,now:()=>now,wait:async ms=>{now+=ms;video.currentTime+=.2;if(++ticks>=3)frames++;}});
 assert.equal(quality,null);assert.ok(ticks>=4);
});

test('auto distinguishes healthy low-latency playback from a stalled clock',()=>{
 let now=0;const p=createAdaptiveQuality({now:()=>now});let next=null;
 for(let i=0;i<2;i++){now+=4000;next=p.observe({quality:2,buffered:.2,progressRatio:1});if(i===0)assert.equal(next,null);}
 assert.equal(next,1);now+=8000;
 assert.equal(p.observe({quality:1,buffered:.05,progressRatio:.1}),null);
 assert.equal(p.observe({quality:1,buffered:.05,progressRatio:.1}),2);
});


test('recording archives remain selectable with no paired camera and after the live slot is reused',async()=>{
  const {loadRecordingCameras}=require('../vava-server/web/management.js');
  const old={slot:0,serial:'OLD-CAMERA',records:3};
  const client={command:async name=>name==='capabilities'?{ok:true,record_cameras:true}:{ok:true,total:1,storage_generation:7,cameras:[old]}};
  assert.equal((await loadRecordingCameras(client,null)).camera,null);assert.deepEqual((await loadRecordingCameras(client,null)).owners,[old]);
  const selected=await loadRecordingCameras(client,{slot:0,serial:'NEW-CAMERA'},'#recordings/OLD-CAMERA/0');
  assert.equal(selected.camera.serial,'OLD-CAMERA');assert.equal(selected.owners.length,2);
  await assert.rejects(loadRecordingCameras(client,null,'#recordings/OTHER-CAMERA/0'),/所选录像归档/);
});

test('recording archive pagination rejects a medium change instead of mixing identities',async()=>{
  const {loadRecordingCameras}=require('../vava-server/web/management.js');
  let n=0;const client={command:async name=>name==='capabilities'?{ok:true,record_cameras:true}:
    {ok:true,total:17,storage_generation:++n,cameras:Array.from({length:n===1?16:1},(_,i)=>({slot:0,serial:'CAM'+i,records:1}))}};
  await assert.rejects(loadRecordingCameras(client,null),/存储介质/);
});

test('all-camera TF query has a single global cursor without a live camera guard',async()=>{
 const {createRecordingPager}=require('../vava-server/web/management.js');const calls=[];
 const client={command:async(name,args,guard)=>{calls.push({name,args,guard});return name==='capabilities'?{ok:true,record_query_all:true}:{ok:true,records:[],storage_generation:9,total:20,has_more:true,next_before:'000000000000000a'};}};
 const pager=createRecordingPager(client,null);await pager.load();await pager.load('next');
 assert.equal(calls.at(-1).name,'record-query-all');assert.deepEqual(calls.at(-1).args,['000000000000000a',9,0,0]);assert.deepEqual(calls.at(-1).guard,{});
});
test('recording page shares one capability reply and cancels all of its directory requests',async()=>{
 const {loadRecordingCameras,createRecordingPager}=require('../vava-server/web/management.js');const calls=[],ctl=new AbortController();
 const client={command:async(name,args,options)=>{
  options.signal.throwIfAborted();calls.push(name);assert.equal(options.signal,ctl.signal);
  if(name==='capabilities')return {ok:true,record_cameras:true,record_query_all:true};
  if(name==='record-cameras')return {ok:true,total:1,storage_generation:7,cameras:[{serial:'OLD',slot:0,records:2}]};
  return {ok:true,records:[],total:2,storage_generation:7,has_more:false};
 }};
 const archives=await loadRecordingCameras(client,null,'#recordings',{signal:ctl.signal});
 const pager=createRecordingPager(client,archives.camera,{capabilities:archives.capabilities,signal:ctl.signal});
 await pager.load();assert.deepEqual(calls,['capabilities','record-cameras','record-query-all']);
 assert.equal(archives.owners[0].serial,'OLD');ctl.abort();await assert.rejects(pager.load(),e=>e.name==='AbortError');
 assert.equal(calls.length,3);
});
test('archive labels support movement, human, face and human-face without inventing a missing type',()=>{
 const {detectionKind,clipCameraName}=require('../vava-server/web/management.js');
 assert.deepEqual([5,6,7,8].map(file_type=>detectionKind({file_type}).english),['Movement detected','Human detected','Face detected','Human face detected']);
 assert.equal(detectionKind({detection_type_raw:5}).english,'Detection type unavailable');
 assert.equal(clipCameraName({serial:'OLD',slot:0},[{serial:'NEW',slot:0,profile:{name:'前门'}}]),'OLD');
});
test('thumbnail transfer uses a stable clip-specific identity and only reads a dedicated preview',async()=>{
 const {previewId,loadThumbnail}=require('../vava-server/web/management.js'),calls=[];
 const row={id:'0000000000001234',serial:'CAM_PREVIEW',slot:1,sha256:'11'.repeat(32)};
 const client={baseUrl:'https://thumbnail-test',endpoint:()=>'/devices/BASE_A',request:async path=>{calls.push(path);return {ok:true,serial:row.serial,slot:1,storage:'tf_preview'};},raw:async path=>{calls.push(path);return new Response(new Blob([new Uint8Array([255,216,255,217])],{type:'image/jpeg'}));},command:()=>{throw Error('cached preview should not be requested again');}};
 const id=await previewId(client,row);assert.equal(id.length,16);assert.ok(parseInt(id[0],16)>=8);assert.notEqual(id,await previewId(client,{...row,serial:'OTHER'}));
 const blob=await loadThumbnail(client,row);assert.equal(blob.type,'image/jpeg');assert.ok(calls[0].includes(id));assert.ok(calls.at(-1).endsWith('/thumbnail'));
 const count=calls.length;await loadThumbnail(client,row);assert.equal(calls.length,count);
});

test('viewing covers are bounded and isolated by station and camera, including reused slots',()=>{
 const {createViewingFrameCache}=require('../vava-server/web/management.js');
 const cache=createViewingFrameCache({maxEntries:2,maxBytes:180}),base={baseUrl:'https://example.test',endpoint:()=>'/devices/A'},other={...base,endpoint:()=>'/devices/B'},cam={serial:'CAM_A',slot:0},replacement={serial:'CAM_B',slot:0};
 const frame=at=>({url:'data:image/jpeg;base64,'+'A'.repeat(40),at});
 assert.equal(cache.set(base,cam,frame(10)),true);assert.equal(cache.get(other,cam),undefined);assert.equal(cache.get(base,replacement),undefined);
 assert.equal(cache.set(base,cam,frame(9)),false);assert.equal(cache.get(base,cam).at,10);
 cache.set(other,cam,frame(11));cache.set(base,replacement,frame(12));assert.equal(cache.get(base,cam),undefined);
 assert.equal(cache.set(base,cam,{url:'https://untrusted.test/image',at:13}),false);
 assert.equal(cache.set(base,cam,{url:'data:image/jpeg;base64,'+'A'.repeat(180),at:13}),false);
 assert.ok(cache.get(base,replacement));
});
test('stopping preserves only a decoded video frame and never replaces it with a blank frame',()=>{
 const {captureViewingFrame,viewingFrame}=require('../vava-server/web/management.js');
 const client={baseUrl:'https://frame.test',endpoint:()=>'/devices/A'},camera={serial:'CAM_A',slot:0};
 let draws=0;const canvas={getContext:()=>({drawImage:()=>draws++}),toDataURL:()=> 'data:image/jpeg;base64,AAAA'};
 const video={readyState:2,videoWidth:1920,videoHeight:1080};
 assert.equal(captureViewingFrame(client,camera,video,()=>canvas),true);assert.equal(canvas.width,640);assert.equal(canvas.height,360);assert.equal(draws,1);
 const frame=viewingFrame(client,camera);
 assert.equal(captureViewingFrame(client,camera,{...video,readyState:0},()=>{throw Error('must not draw')}),false);assert.deepEqual(viewingFrame(client,camera),frame);
 assert.equal(captureViewingFrame(client,camera,video,()=>({getContext:()=>{throw Error('tainted')}})),false);assert.deepEqual(viewingFrame(client,camera),frame);
});

test('expired preview cache retries with a fresh identity without downloading the full clip',async()=>{
 const {loadThumbnail}=require('../vava-server/web/management.js'),row={serial:'EXPIRING_CAM',slot:2,id:'0000000000000007',sha256:'b'.repeat(64)},requests=[];let ready=false;
 const client={baseUrl:'https://expired-preview.test',endpoint:()=>'/devices/A',request:async()=>{if(!ready)throw Object.assign(Error('missing'),{status:404});return {serial:row.serial,slot:2,storage:'tf_preview'};},command:async(name,args,guard)=>{requests.push({name,args,guard});if(requests.length===1)return {ok:false,error:'export_unavailable_or_conflict'};ready=true;return {ok:true};},raw:async()=>new Response(new Blob([new Uint8Array([255,216,255,217])],{type:'image/jpeg'}))};
 const blob=await loadThumbnail(client,row);assert.equal(blob.type,'image/jpeg');assert.equal(requests.length,2);assert.ok(requests.every(r=>r.name==='record-preview'&&r.guard.cameraSerial===row.serial));assert.notEqual(requests[0].args[2],requests[1].args[2]);
});

test('event feed distinguishes raw camera detections from the archive enum',()=>{
 const {eventLabel}=require('../vava-server/web/management.js');
 assert.equal(eventLabel('camera_report_200',{result_raw:1,type_raw:1}),'检测到移动');
 assert.equal(eventLabel('camera_report_200',{result_raw:1,type_raw:5}),'检测到人形');
 assert.equal(eventLabel('camera_report_200',{result_raw:1,type_raw:7}),'检测类型未报告');
 assert.equal(eventLabel('camera_report_200',{result_raw:0,type_raw:5}),'未确认的检测报告');
});

test('camera cover invalidation ignores telemetry and tracks archive identity and new clips',()=>{
 const {cameraPreviewStamp}=require('../vava-server/web/management.js');
 const client={baseUrl:'https://home.test',endpoint:()=>'/devices/BASE'},camera={slot:0,serial:'CAM',battery_raw:42,heartbeat_age_ms:10,state:'awake',recording:{id:'one'}},storage={generation:1,records:4,bytes:100,available:true};
 const stamp=cameraPreviewStamp(client,[camera],storage);
 assert.equal(cameraPreviewStamp(client,[{...camera,battery_raw:43,heartbeat_age_ms:5000,state:'sleep_observed'}],{...storage,active:1,queued_bytes:50}),stamp);
 for(const changed of [{records:5},{bytes:200},{generation:2}])assert.notEqual(cameraPreviewStamp(client,[camera],{...storage,...changed}),stamp);
 assert.notEqual(cameraPreviewStamp(client,[{...camera,serial:'REPLACEMENT'}],storage),stamp);
 assert.notEqual(cameraPreviewStamp(client,[{...camera,recording:{id:'two'}}],storage),stamp);
});
test('new events replace an older viewing cover while a newer viewing frame wins',()=>{
 const {preferViewingFrame}=require('../vava-server/web/management.js'),row={clock_valid:true,unix_seconds:1000,duration_ms:10000};
 assert.equal(preferViewingFrame(null,row),false);assert.equal(preferViewingFrame({at:999000},row),false);
 assert.equal(preferViewingFrame({at:1011000},row),true);assert.equal(preferViewingFrame({at:1001000},null),true);
 assert.equal(preferViewingFrame({at:1001000},{...row,clock_valid:false}),true);
});

test('codec initialization changes keep idle recording idle and save only an active prefix',async()=>{
 const {init,fragments}=mp4Fixture(),f=recordingFixture();f.recorder.push(init);f.recorder.push(init);assert.equal(f.recorder.busy,false);assert.equal(f.saves.length,0);
 f.recorder.start();fragments.slice(0,10).forEach(b=>f.recorder.push(b));f.recorder.push(init);fragments.slice(10,20).forEach(b=>f.recorder.push(b));await f.recorder.stop();
 assert.equal(f.saves.length,1);assert.match(f.errors[0],/编码已变化/);assert.equal(f.recorder.busy,false);await probeRecording(f.saves[0].blob);
 f.recorder.start();fragments.slice(0,10).forEach(b=>f.recorder.push(b));await f.recorder.stop();assert.equal(f.saves.length,2);await probeRecording(f.saves[1].blob);
});
