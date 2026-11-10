const test=require('node:test');
const assert=require('node:assert/strict');
const {cameraName,nameArgument,mergeDraft,editDraft,commitDraft,discardDraft,batteryReading,batteryReportTime,telemetry,cameraOnline,reportedSetting,groupSettingKeys,configurationFieldLocked,reconcilePolicyDraft,recordingTrigger,recordingEnd,collectMedia}=require('../vava-server/web/app.js');
const {createPairingFlow}=require('../vava-server/web/app.js');
function pairingFixture(){
 let time=0,p={id:'0000000000000001',phase:'completed',active:false,remaining_ms:0},failure=null,readHook=null;
 const calls=[],states=[];let dismissed=0;
 const client={stationId:'BASE_A',deviceBoot:()=> 'boot-a',command:async(name,args,options)=>{
  calls.push({name,args,options});
  if(name==='status')return readHook?readHook():{ok:true,pairing:{...p}};
  if(failure)throw Error(failure);
  if(name==='pair-open'){p={id:'0000000000000002',phase:'opening_ap',active:true,remaining_ms:150000};return {ok:true,operation_id:p.id,window_seconds:120};}
  if(name==='pair-close'){p={...p,phase:'cancelled',active:false,remaining_ms:0};return {ok:true};}
  throw Error('unexpected '+name);
 }};
 const flow=createPairingFlow({client,now:()=>time,onChange:d=>states.push(d),onDismiss:()=>dismissed++});
 return {flow,calls,states,get current(){return states.at(-1);},get dismissed(){return dismissed;},set:p0=>p=p0,
  fail:s=>failure=s,hold:fn=>readHook=fn,advance:ms=>{time+=ms;flow.tick();}};
}
test('Server pairing requests 120 seconds and starts a live countdown after AP readiness',async()=>{
 const x=pairingFixture();await x.flow.start();
 assert.deepEqual(x.calls.find(c=>c.name==='pair-open').args,['120']);assert.equal(x.current.remainingSeconds,null);
 x.set({id:x.current.id,phase:'waiting_camera',active:true,remaining_ms:120000});await x.flow.poll();
 assert.equal(x.current.remainingSeconds,120);x.advance(1000);assert.equal(x.current.remainingSeconds,119);
 x.advance(119000);assert.equal(x.current.remainingSeconds,0);assert.equal(x.current.operation.active,true);
 x.set({id:x.current.id,phase:'expired',active:false,remaining_ms:0});await x.flow.poll();assert.equal(x.current.operation.phase,'expired');
});
test('Server pairing resumes the existing window without reopening or extending it',async()=>{
 const x=pairingFixture();x.set({id:'0000000000000005',phase:'waiting_camera',active:true,remaining_ms:45000});
 await x.flow.start();assert.equal(x.current.resumed,true);assert.equal(x.current.remainingSeconds,45);
 assert.ok(!x.calls.some(c=>c.name==='pair-open'));assert.ok(x.calls.every(c=>c.options.stationId==='BASE_A'));
});
test('Server cancellation closes its dialog automatically after the backend confirms cancellation',async()=>{
 const x=pairingFixture();await x.flow.start();await x.flow.close();assert.equal(x.dismissed,1);
 assert.equal(x.calls.filter(c=>c.name==='pair-close').length,1);await x.flow.poll();x.advance(1000);assert.equal(x.dismissed,1);
});
test('Server cancellation failure keeps progress visible and permits an explicit retry',async()=>{
 const x=pairingFixture();await x.flow.start();x.fail('connection lost');await x.flow.close();
 assert.equal(x.dismissed,0);assert.match(x.current.error,/关闭失败/);assert.equal(x.current.closing,false);
 x.fail(null);await x.flow.close();assert.equal(x.dismissed,1);assert.equal(x.calls.filter(c=>c.name==='pair-close').length,2);
});
test('a late status poll cannot revive cancelled pairing or hide a completed camera',async()=>{
 const x=pairingFixture();await x.flow.start();let release,entered;
 const ready=new Promise(r=>entered=r),gate=new Promise(r=>release=r),old={...x.current.operation};
 x.hold(async()=>{entered();await gate;return {ok:true,pairing:old};});
 const poll=x.flow.poll();await ready;const close=x.flow.close();
 x.hold(null);x.set({...old,phase:'completed',active:false,serial:'CAM-NEW'});release();await Promise.all([poll,close]);
 assert.equal(x.current.operation.phase,'completed');assert.equal(x.current.operation.serial,'CAM-NEW');assert.equal(x.dismissed,0);
 assert.ok(!x.calls.some(c=>c.name==='pair-close'));await x.flow.close();assert.equal(x.dismissed,1);
});
test('Server pairing cannot report an old success after open fails or cancel a replacement operation',async()=>{
 const x=pairingFixture();x.fail('network unavailable');await x.flow.start();
 assert.equal(x.current.operation,null);assert.equal(x.current.accepted,false);assert.match(x.current.error,/开启失败/);
 x.fail(null);await x.flow.start();x.set({id:'0000000000000009',phase:'waiting_camera',active:true,remaining_ms:120000});
 await x.flow.close();assert.match(x.current.error,/配对任务已由其他操作更新/);assert.ok(!x.calls.some(c=>c.name==='pair-close'));
});
test('unavailable pairing status does not invent success or expiry and can recover',async()=>{
 const x=pairingFixture();await x.flow.start();x.hold(async()=>{throw Error('offline');});await x.flow.poll();
 assert.equal(x.current.unavailable,true);assert.equal(x.current.operation.active,true);
 x.hold(null);x.set({id:x.current.id,phase:'waiting_camera',active:true,remaining_ms:10000});await x.flow.poll();
 assert.equal(x.current.unavailable,false);assert.equal(x.current.remainingSeconds,10);
});
test('work mode and camera duration remove obsolete policy drafts without losing other edits',()=>{
 const d=mergeDraft(null,{level:2,record:20,idle:15,'camera-duration':'0'});
 editDraft(d,'level',3);editDraft(d,'record',60);editDraft(d,'idle',30);
 editDraft(d,'camera-duration','1');
 const p={level_source:'work_mode',level:1,record_seconds:20};
 reconcilePolicyDraft(d,p);
 assert.equal(d.values.level,'1');assert.equal(d.values.record,'20');
 assert.deepEqual([...d.dirty],['idle','camera-duration']);
 assert.ok(configurationFieldLocked('arming','level',d.values,p));
 assert.ok(configurationFieldLocked('arming','record',d.values,p));
 assert.ok(!configurationFieldLocked('pir','record',d.values,p));
 editDraft(d,'camera-duration','0');editDraft(d,'record',25);reconcilePolicyDraft(d,p);
 assert.equal(d.values.record,'25');assert.ok(d.dirty.has('record'));
 assert.ok(!configurationFieldLocked('arming','record',d.values,p));
 assert.ok(!configurationFieldLocked('arming','level',d.values,{level_source:'legacy_policy'}));
});
test('camera names use UTF-8 without splitting the command and keep identity separate',()=>{
 const name='前门 "花园" \\ Camera';
 assert.equal(Buffer.from(nameArgument(name),'hex').toString('utf8'),name);
 assert.equal(nameArgument(''),'-');assert.equal(cameraName({slot:2,profile:{name:''}}),'摄像头 3');
 assert.equal(cameraName({slot:2,serial:'SIM',profile:{name}}),name);
 assert.equal(Buffer.from(nameArgument('摄'.repeat(32)),'hex').length,96);
 for(const s of ['a'.repeat(33),'🌻'.repeat(25),'name\nforget 0','\ud800','\u202eabc','\u0080'])assert.throws(()=>nameArgument(s));
});
test('configuration reports preserve unknown enums and source ages',()=>{
 const reports={first_connect:{values:{mirror_raw:3,osd_raw:2,antiflicker_wire_raw:1},current_connection:true,age_ms:5000}};
 assert.equal(reportedSetting('image','mirror',reports).value,'3');
 assert.equal(reportedSetting('image','antiflicker',reports).value,'60');
 const unknown=reportedSetting('image','osd',reports);assert.equal(unknown.value,null);assert.equal(unknown.raw,2);assert.equal(unknown.comparable,false);
 assert.equal(reportedSetting('image','mirror',reports,{mirror:{current_connection:true,sent_age_ms:1000}}).olderThanRequest,true);
 assert.equal(reportedSetting('image','mirror',reports,{mirror:{current_connection:true,sent_age_ms:null}}).olderThanRequest,false);
 reports.first_connect.current_connection=false;assert.equal(reportedSetting('image','mirror',reports).current,false);
 assert.equal(reportedSetting('video','quality',reports),null);
});
test('saved PIR sensitivity and current MCU level use different reports',()=>{
 const reports={pir_config_42:{values:{mode_raw:0,sensitivity_raw:2,wait_seconds_raw:20,record_seconds_raw:10,arming_raw:1},current_connection:true,age_ms:30},pir_level_30:{values:{level_raw:0},current_connection:true,age_ms:10}};
 assert.equal(reportedSetting('pir','mode',reports).value,'battery');
 assert.equal(reportedSetting('pir','sensitivity',reports).value,'2');
 assert.equal(reportedSetting('arming','level',reports).value,'0');
 assert.equal(reportedSetting('arming','enabled',reports),null);
});
test('group retry selects only stored keys and PIR uses its protocol setting name',()=>{
 const desired={values:{mirror:3,volume:25,'pir-mode':{mode:0}}};
 assert.deepEqual(groupSettingKeys('image',[{key:'mirror'},{key:'ir'}],desired),['mirror']);
 assert.deepEqual(groupSettingKeys('pir',[{key:'mode'}],desired),['pir-mode']);
 assert.deepEqual(groupSettingKeys('audio',[{key:'volume'},{key:'playback-audio'}],desired),['volume']);
});
test('PIR recordings and disarm completion use the backend event vocabulary',()=>{
 assert.equal(recordingTrigger('pir_first_connect'),'PIR 触发');assert.equal(recordingTrigger('manual'),'手动');
 assert.equal(recordingTrigger('unexpected'),'未知来源');assert.equal(recordingEnd('disarmed'),'策略撤防');
});
test('poll preserves edited values, updates untouched ones',()=>{
 const d=mergeDraft(null,{mirror:'0',ir:'0'}); editDraft(d,'mirror','3');
 mergeDraft(d,{mirror:'0',ir:'1'});assert.equal(d.values.mirror,'3');assert.equal(d.values.ir,'1');assert.deepEqual([...d.dirty],['mirror']);
});
test('a changed remote value reports conflict without discarding draft',()=>{
 const d=mergeDraft(null,{mirror:'0'});editDraft(d,'mirror','3');mergeDraft(d,{mirror:'2'});
 assert.equal(d.values.mirror,'3');assert.equal(d.base.mirror,'0');assert.ok(d.conflicts.has('mirror'));
});
test('save records submitted snapshot and retains a later edit',()=>{
 const d=mergeDraft(null,{mirror:'0'});editDraft(d,'mirror','1');const sent={mirror:d.values.mirror};editDraft(d,'mirror','3');commitDraft(d,sent);
 assert.equal(d.base.mirror,'1');assert.equal(d.values.mirror,'3');assert.ok(d.dirty.has('mirror'));
});
test('independent camera drafts cannot leak edits',()=>{
 const a=mergeDraft(null,{mirror:''}),b=mergeDraft(null,{mirror:''});editDraft(a,'mirror','2');commitDraft(a,{mirror:'2'});
 assert.equal(b.values.mirror,'');assert.equal(b.dirty.size,0);assert.equal(a.dirty.size,0);
});
test('unknown values remain empty and do not imply defaults',()=>{
 const d=mergeDraft(null,{mirror:undefined,volume:null});assert.equal(d.values.mirror,'');assert.equal(d.values.volume,'');assert.equal(d.dirty.size,0);
});
test('reverting edit to original removes dirty and conflict flags',()=>{
 const d=mergeDraft(null,{mirror:'0'});editDraft(d,'mirror','1');mergeDraft(d,{mirror:'2'});editDraft(d,'mirror','0');assert.equal(d.dirty.size,0);assert.equal(d.conflicts.size,0);
});
test('remote reversal clears a stale conflict and discard uses latest station values',()=>{
 const d=mergeDraft(null,{mirror:'0'});editDraft(d,'mirror','1');mergeDraft(d,{mirror:'2'});
 mergeDraft(d,{mirror:'0'});assert.equal(d.conflicts.size,0);assert.ok(d.dirty.has('mirror'));
 discardDraft(d,{mirror:'3'});assert.equal(d.values.mirror,'3');assert.equal(d.base.mirror,'3');assert.equal(d.dirty.size,0);
});
test('battery keeps zero distinct from unknown and replaces report labels with time',()=>{
 assert.equal(telemetry({heartbeat_age_ms:-1,battery_raw:0}),'尚无电量报告');
 assert.equal(telemetry({heartbeat_age_ms:0,heartbeat_current:true,battery_raw:0}),'电量 0% · 报告时间未知');
 assert.equal(telemetry({heartbeat_age_ms:14999,heartbeat_current:true,battery_raw:100}),'电量 100% · 报告时间未知');
 for(const d of [{heartbeat_age_ms:0,heartbeat_current:false},{heartbeat_age_ms:15000,heartbeat_current:true}])
  assert.equal(telemetry({...d,battery_raw:80}),'电量 80% · 报告时间未知');
});
test('absent or malformed battery telemetry never becomes an empty battery or percentage',()=>{
 for(const age of [undefined,null,'0',NaN,Infinity,-1])
  assert.equal(batteryReading({heartbeat_age_ms:age,heartbeat_current:true,battery_raw:0}),null);
 for(const value of [undefined,null,'40',NaN,Infinity,-1,101,255,40.5]) {
  const d={heartbeat_age_ms:0,heartbeat_current:true,battery_raw:value};
  assert.equal(batteryReading(d),null);assert.equal(telemetry(d),'尚无电量报告');
 }
 assert.equal(batteryReading(undefined),null);
 assert.equal(batteryReading({heartbeat_age_ms:0,heartbeat_current:'true',battery_raw:40}).fresh,false);
});
test('charging uses the reported power byte and full charge requires external power',()=>{
 const d={heartbeat_age_ms:0,heartbeat_current:true,battery_raw:80,power_raw:1};
 assert.equal(telemetry(d),'充电中');
 assert.equal(telemetry({...d,battery_raw:100}),'已充满');
 assert.equal(telemetry({...d,battery_raw:100,power_raw:0}),'电量 100% · 报告时间未知');
 assert.equal(telemetry({...d,heartbeat_current:false}),'充电中');
 assert.equal(telemetry({...d,heartbeat_age_ms:15000}),'充电中');
 for(const power of [undefined,null,'1','0',true,false,2,255,-1,NaN]) {
  assert.equal(batteryReading({...d,power_raw:power}).charging,null);
  assert.equal(telemetry({...d,power_raw:power}),'电量 80% · 报告时间未知');
 }
 assert.equal(telemetry({...d,heartbeat_age_ms:-1}),'尚无电量报告');
});
test('battery report time belongs to its station snapshot and does not advance during sleep',()=>{
 const d={heartbeat_age_ms:60000,heartbeat_current:false,battery_raw:40,power_raw:0};
 const snapshot={clock_valid:true,unix_seconds:1794272523};
 assert.equal(batteryReportTime(d,snapshot),1794272463000);
 assert.equal(batteryReportTime({...d,heartbeat_age_ms:90000},{...snapshot,unix_seconds:1794272553}),1794272463000);
 const label=telemetry(d,snapshot);
 assert.match(label,/^电量 40% · \d{2}\/\d{2} \d{2}:\d{2}$/);
 assert.doesNotMatch(label,/上次报告|最新报告|充电中/);
 for(const invalid of [undefined,{}, {...snapshot,clock_valid:false},{...snapshot,unix_seconds:null},{...snapshot,unix_seconds:Infinity}])
  assert.equal(batteryReportTime(d,invalid),null);
 assert.equal(batteryReportTime({...d,heartbeat_age_ms:-1},snapshot),null);
 assert.equal(batteryReportTime({...d,heartbeat_age_ms:1e20},snapshot),null);
 assert.equal(telemetry({...d,power_raw:1},snapshot),'充电中');
});
function mediaReader(...chunks){return new ReadableStream({start(c){for(const bytes of chunks)c.enqueue(new Uint8Array(bytes));c.close();}}).getReader();}
test('media keeps chunk order and reports exact transfer progress',async()=>{
 const progress=[],blob=await collectMedia(mediaReader([1,2],[3]),3,100,(...p)=>progress.push(p),new AbortController().signal);
 assert.deepEqual([...new Uint8Array(await blob.arrayBuffer())],[1,2,3]);assert.equal(blob.type,'video/h264');assert.deepEqual(progress,[[2,3],[3,3]]);
});
test('incomplete, oversized and empty media are rejected without saving',async()=>{
 for(const [chunks,length,limit,pattern] of [
  [[[1,2]],3,100,/不完整/],[[[1,2]],1,100,/长度不符/],[[],0,100,/不完整/],
  [[[1,2]],null,1,/上限/],[[[1]],2,1,/上限/],[[[1]],NaN,100,/长度无效/]
 ]) await assert.rejects(collectMedia(mediaReader(...chunks),length,limit,()=>{},new AbortController().signal),pattern);
});
test('snapshots may use an unknown length but still enforce the size cap',async()=>{
 const blob=await collectMedia(mediaReader([1,2,3]),null,3,()=>{},new AbortController().signal);assert.equal(blob.size,3);
});
test('cancel interrupts a pending read and releases the stream',async()=>{
 let cancelled=false;const ctl=new AbortController(),reader=new ReadableStream({cancel(){cancelled=true;}}).getReader();
 const result=collectMedia(reader,null,100,()=>{},ctl.signal);ctl.abort();
 await assert.rejects(result,{name:'AbortError'});assert.ok(cancelled);
});
test('a transport error is surfaced and an already-cancelled download never reads',async()=>{
 const broken=new ReadableStream({start(c){c.error(Error('connection lost'));}}).getReader();
 await assert.rejects(collectMedia(broken,3,100,()=>{},new AbortController().signal),/connection lost/);
 const ctl=new AbortController();ctl.abort();await assert.rejects(collectMedia(mediaReader([1]),1,100,()=>{},ctl.signal),{name:'AbortError'});
});

test('sleeping camera stays online through ALIVE, pairing and old reports cannot imply online',()=>{
 const camera={paired:true,state:'sleep_observed',control_ready:false,connections:[false,false,true],alive_age_ms:13000};
 assert.equal(cameraOnline(camera),true);
 assert.equal(cameraOnline({...camera,connections:[false,false,false]}),false);
 assert.equal(cameraOnline({...camera,paired:false}),false);
 assert.equal(cameraOnline({...camera,connections:['true',0,0]}),false);
 assert.equal(cameraOnline(camera,false),false);
 assert.equal(cameraOnline(undefined),false);
});

test('charging cover uses reported battery fill and prefixes the charging label with its percentage',()=>{
 const {batteryCell}=require('../vava-server/web/app.js');
 const d={paired:true,heartbeat_reported:true,battery_raw:70,power_raw:1,heartbeat_current:true,heartbeat_age_ms:1000};
 const snapshot={clock_valid:true,unix_seconds:10000};
 const html=batteryCell(d,snapshot);
 assert.match(html,/70% · 充电中/);assert.match(html,/width="11\.9[0-9]*" height="8"/);assert.match(html,/charging-mark/);
 assert.doesNotMatch(html,/opacity="0\.35"/);
 assert.match(batteryCell({...d,battery_raw:100},snapshot),/100% · 已充满/);
});
