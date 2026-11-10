const test=require('node:test');
const assert=require('node:assert/strict');
const management=require('../vava-server/web/management.js');
const flush=()=>new Promise(resolve=>setImmediate(resolve));

// Exercise the rendered control handlers against an abortable pending media
// request. A stopped connection must stay stopped, including after late errors.
async function fixture(t,{rejectFirst=false}={}) {
 const previous=globalThis.document,previousMedia=globalThis.MediaSource;
 class Element extends EventTarget {
  constructor(id=''){super();this.id=id;this.hidden=false;this.disabled=false;this.value='1';this.muted=true;this.volume=1;this.readyState=0;this.attrs=new Map();this.classList={toggle(){}};}
  setAttribute(k,v){this.attrs.set(k,v);} removeAttribute(k){this.attrs.delete(k);}
  load(){} focus(){} closest(){return null;} querySelectorAll(){return [];}
 }
 const elements=new Map(),doc=new EventTarget();doc.getElementById=id=>elements.get(id);globalThis.document=doc;globalThis.MediaSource=class {};
 const content={isConnected:true,set innerHTML(html){this.html=html;for(const [,id] of html.matchAll(/id="([^"]+)"/g))elements.set(id,new Element(id));}};
 const attempts=[],notices=[];
 const client={baseUrl:'http://test',stationId:'BASE',user:{role:3},endpoint:()=>'/devices/BASE',deviceBoot:()=> 'boot',devices:async()=>({devices:[{id:'BASE',online:true,boot:'boot'}]}),raw:async(path,body,signal)=>{
  const attempt={path,body,signal};attempts.push(attempt);
  if(rejectFirst&&attempts.length===1)throw Error('simulated media unavailable');
  return new Promise((resolve,reject)=>{signal.addEventListener('abort',()=>reject(signal.reason),{once:true});if(signal.aborted)reject(signal.reason);});
 }};
 t.after(async()=>{await management.invalidate();await flush();globalThis.document=previous;globalThis.MediaSource=previousMedia;});
 await management.render('live',{client,content,notice:(...args)=>notices.push(args),camera:{slot:0,serial:'CAM'},autoStart:true,embedded:true});await flush();
 return {get:id=>elements.get(id),attempts,content,notices};
}
test('live stop cancels the media request and does not resume on later work',async t=>{
 const x=await fixture(t);assert.equal(x.attempts.length,1);assert.equal(x.get('liveToggle').attrs.get('aria-label'),'停止观看');
 assert.equal(management.hasLocalRecording(),false);
 x.get('liveToggle').onclick();await flush();
 assert.equal(x.attempts[0].signal.aborted,true);assert.equal(x.get('liveToggle').attrs.get('aria-label'),'重新观看');assert.equal(x.get('liveRetry').hidden,false);
 await flush();assert.equal(x.attempts.length,1);assert.equal(x.get('liveProgress').textContent,'观看已停止');
 assert.equal(management.hasLocalRecording(),false);
 assert.doesNotMatch(x.content.html,/<video[^>]*\scontrols\b/);assert.doesNotMatch(x.content.html,/id="liveStart"/);
});
test('only explicit play reconnects and double clicks create one viewing session',async t=>{
 const x=await fixture(t);x.get('liveToggle').onclick();await flush();
 x.get('liveRetry').onclick();x.get('liveRetry').onclick();await flush();
 assert.equal(x.attempts.length,2);assert.equal(x.attempts[0].signal.aborted,true);assert.equal(x.attempts[1].signal.aborted,false);
 assert.equal(x.attempts[1].body.serial,'CAM');assert.equal(x.get('liveToggle').attrs.get('aria-label'),'停止观看');
});
test('connection errors offer in-picture retry without automatic reconnection',async t=>{
 const x=await fixture(t,{rejectFirst:true});assert.equal(x.attempts.length,1);assert.equal(x.get('liveRetry').hidden,false);
 assert.match(x.get('liveProgress').textContent,/simulated media unavailable/);assert.equal(x.get('liveToggle').disabled,false);
 x.get('liveRetry').onclick();await flush();assert.equal(x.attempts.length,2);
});
test('closing the player aborts its connection and blocks stale play handlers',async t=>{
 const x=await fixture(t);await management.invalidate();await flush();assert.equal(x.attempts[0].signal.aborted,true);
 x.get('liveRetry').onclick();await flush();assert.equal(x.attempts.length,1);
});

function liveRecord(kind,epoch,data=Buffer.from('data')){const header=Buffer.alloc(16);header.write('MVL2');header[4]=kind;header.writeUInt32BE(epoch,8);header.writeUInt32BE(data.length,12);return Buffer.concat([header,data]);}
test('continuous parser preserves boundaries across every split and coalesced epochs',async()=>{
 const input=Buffer.concat([liveRecord(1,1),liveRecord(2,1),liveRecord(3,1),liveRecord(1,2),liveRecord(2,2),liveRecord(3,2)]);
 for(let size=1;size<=input.length;size++){const seen=[],parser=management.createLiveRecordParser(async r=>seen.push([r.kind,r.epoch,Buffer.from(r.data).toString()]));for(let at=0;at<input.length;at+=size)await parser.push(input.subarray(at,at+size));parser.finish();assert.deepEqual(seen.map(r=>r.slice(0,2)),[[1,1],[2,1],[3,1],[1,2],[2,2],[3,2]]);}
});
test('continuous parser rejects unbounded, stale, uninitialized and truncated media',async()=>{
 for(const raw of [liveRecord(3,1),Buffer.concat([liveRecord(1,1),liveRecord(3,1)]),Buffer.concat([liveRecord(1,2),liveRecord(1,1)]),Buffer.concat([liveRecord(1,1),liveRecord(2,1),liveRecord(2,1)])]){
  const parser=management.createLiveRecordParser(()=>{});await assert.rejects(parser.push(raw));
 }
 const huge=liveRecord(1,1);huge.writeUInt32BE(4097,12);await assert.rejects(management.createLiveRecordParser(()=>{}).push(huge));
 const truncated=management.createLiveRecordParser(()=>{});await truncated.push(liveRecord(1,1).subarray(0,18));assert.throws(()=>truncated.finish(),/完整/);
});

test('picture confirmation requires presented frames at the target size and cancels on stop',async()=>{
 let callback,id=0,cancelled=[];const v={paused:false,readyState:4,videoWidth:640,videoHeight:360,requestVideoFrameCallback:f=>(callback=f,++id),cancelVideoFrameCallback:i=>cancelled.push(i)};
 const ctl=new AbortController();let resolved=false;const waiting=management.waitLivePicture({video:v,quality:1,signal:ctl.signal,timeout:2000}).then(q=>(resolved=true,q));
 callback(0,{width:640,height:360,mediaTime:1});await flush();assert.equal(resolved,false);
 callback(1,{width:1280,height:720,mediaTime:1.1});await flush();assert.equal(resolved,false);
 callback(2,{width:1280,height:720,mediaTime:1.3});assert.equal(await waiting,1);assert.ok(cancelled.length);
 const stop=new AbortController();const pending=management.waitLivePicture({video:v,quality:0,signal:stop.signal});stop.abort();await assert.rejects(pending,{name:'AbortError'});
});
