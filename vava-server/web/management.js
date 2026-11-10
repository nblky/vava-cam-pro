/* Server-only adapter: browser sessions never contain a device PSK. */
(() => {
'use strict';
const readOnly=new Set(['status','list','settings','policy','record-info','record-query','record-list','record-cameras','record-query-all','record-preview','export-list','upload-list','nas-list','nas-status','storage-policy','capabilities','events']);
const esc=s=>String(s??'').replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
const delay=(ms,signal)=>new Promise((resolve,reject)=>{if(signal?.aborted){reject(signal.reason);return;}const stop=()=>{clearTimeout(t);reject(signal.reason);};const t=setTimeout(()=>{signal?.removeEventListener('abort',stop);resolve();},ms);signal?.addEventListener('abort',stop,{once:true});});
function createClient({baseUrl='',token,fetchImpl=globalThis.fetch,storage=globalThis.sessionStorage}={}) {
  const base=VavaAPI.normalizeBase(baseUrl);let selected='',directory=null,chain=Promise.resolve(),user=null;
  const pendingKey=id=>'myvavaPending:'+base+':'+id;
  const raw=(path,body,signal)=>fetchImpl(base+'/api/v1'+path,{method:body===undefined?'GET':'POST',credentials:'omit',redirect:'error',cache:'no-store',headers:{...(token?{Authorization:'Bearer '+token}:{}),...(body===undefined?{}:{'Content-Type':'application/json'})},...(body===undefined?{}:{body:JSON.stringify(body)}),signal});
  const request=async(path,body,signal)=>{const r=await raw(path,body,signal),j=await r.json();if(!r.ok||!j.ok){const e=Error(j.error||'request_failed');e.code=j.error;e.status=r.status;throw e;}return j;};
  async function devices(signal){directory=await request('/devices',undefined,signal);return directory;}
  async function capabilities(options={}) {
    user=(await request('/session',undefined,options.signal)).user;await devices(options.signal);
    const saved=storage?.getItem('myvavaSelected:'+base);selected=directory.devices.some(d=>d.id===saved)?saved:directory.devices[0]?.id||'';
    if(typeof document!=='undefined'){const el=document.getElementById('stationSelect');if(el){el.innerHTML=directory.devices.map(d=>`<option value="${esc(d.id)}">${esc(d.id)} · ${d.online?'在线':'离线'}</option>`).join('');el.value=selected;}}
    return {ok:true,api_version:'1',user};
  }
  const root=(id=selected)=>'/devices/'+encodeURIComponent(id);
  function savePending(value,id=selected){if(storage)storage.setItem(pendingKey(id),JSON.stringify(value));}
  function getPending(id=selected){try{return JSON.parse(storage?.getItem(pendingKey(id))||'null');}catch{return null;}}
  function clearPending(id=selected){storage?.removeItem(pendingKey(id));}
  async function pollTask(t,signal,id){for(;;){const result=await request(root(id)+'/tasks/'+t.epoch+'/'+t.id,undefined,signal);if(['completed','expired','unknown'].includes(result.state))return result;await delay(150,signal);}}
  async function recover(signal,id=selected){const p=getPending(id);if(!p)return null;let t;
    try{t=p.id?await request(root(id)+'/tasks/'+p.epoch+'/'+p.id,undefined,signal):await request(root(id)+'/lookup/'+p.request_key,undefined,signal);}catch(e){if(e.code==='task_not_retained'){e.message='上次请求未能确定结果。请在任务页核对基站状态后解除挂起；不会自动重发。';e.code='pending_result_unknown';}throw e;}
    if(t.state==='queued'||t.state==='dispatched')t=await pollTask(t,signal,id);
    if(t.state==='completed'||t.state==='expired')clearPending(id);return t;
  }
  async function execute(envelope,signal,id,expectedBoot) {
    signal?.throwIfAborted();
    if(!id)throw Error('当前账户尚未获准访问任何基站。');
    const mutating=!readOnly.has(envelope.command);
    if(mutating&&getPending(id)){const pending=getPending(id),old=await recover(signal,id);if(old?.state==='completed'&&JSON.stringify(pending.command)===JSON.stringify(envelope))return JSON.parse(old.result);if(old?.state==='unknown'){const e=Error('上次操作结果未知。请在任务页核对设备状态；不会自动重发。');e.code='pending_result_unknown';throw e;}}
    const current=await devices(signal);signal?.throwIfAborted();const device=current.devices.find(d=>d.id===id);if(!device?.online)throw Object.assign(Error('基站离线；云存储视频和事件仍可查看。'),{code:'device_offline'});
    if(expectedBoot!==undefined&&device.boot!==expectedBoot)throw Object.assign(Error('基站已重启，请重新开始观看。'),{code:'device_boot_changed'});
    const random=new Uint8Array(8);crypto.getRandomValues(random);
    const body={boot:device.boot,epoch:current.epoch,request_key:current.request_prefix+Array.from(random,n=>n.toString(16).padStart(2,'0')).join(''),command:envelope};
    if(mutating)savePending(body,id);
    let accepted;
    for(let attempt=0;;++attempt){
      try{accepted=await request(root(id)+'/commands',body,signal);break;}
      catch(e){
        if(mutating&&((e.status>=400&&e.status<500)||['device_busy','device_offline','unknown_device','server_epoch_changed','device_boot_changed','request_key_expired','request_key_conflict','history_full_with_protected_requests'].includes(e.code)))clearPending(id);
        // Only an explicit queue rejection is safe to retry. Lost responses
        // retain the exact request key for lookup, without resubmitting.
        if(e.code!=='device_busy')throw e;
        if(attempt>=24){e.message='基站正在处理其他请求，请稍后重试。';throw e;}
        await delay(200,signal);signal?.throwIfAborted();
        if(mutating)savePending(body,id);
      }
    }
    if(mutating)savePending({...body,id:accepted.id},id);const result=await pollTask(accepted,signal,id);
    if(mutating&&result.state!=='unknown')clearPending(id);
    if(result.state!=='completed'){const e=Error(result.state==='unknown'?'基站操作结果未知，请核对任务记录。':'任务已过期，未执行。');e.code=result.state;throw e;}
    return JSON.parse(result.result);
  }
  const command=async(envelope,signal,id=selected,expectedBoot)=>{const timeout=AbortSignal.timeout(45000),bounded=signal?AbortSignal.any([signal,timeout]):timeout;const p=chain.catch(()=>{}).then(()=>execute(envelope,bounded,id,expectedBoot));chain=p;return p;};
  return {baseUrl:base,get stationId(){return selected;},get user(){return user;},deviceBoot:(id=selected)=>directory?.devices.find(d=>d.id===id)?.boot,request,raw,devices,capabilities,getPending,clearPending,recover,
    select(id){if(!directory?.devices.some(d=>d.id===id))throw Error('未知基站');selected=id;storage?.setItem('myvavaSelected:'+base,id);},
    commandLineRaw:async(line,options={})=>{const j=await command(VavaAPI.fromCommandLine(line),options.signal,options.stationId);return new Response(JSON.stringify(j),{status:200,headers:{'Content-Type':'application/json'}});},
    command:(name,args=[],options={})=>command(VavaAPI.envelope(name,args,options),options.signal,options.stationId,options.expectedBoot),
    endpoint:root,
    logout:()=>request('/logout',{}),
    status:options=>command({command:'status',args:[]},options?.signal)
  };
}
function storageError(code){
  const message=({record_in_use:'片段正在回放或传输，请稍后重试删除。',recording_active:'片段还在录制或收尾，请稍后操作。',record_not_found:'片段已删除或不在当前存储中，请刷新目录。',storage_generation_conflict:'存储介质已重新挂载或更换，请刷新目录后继续。',storage_changed_or_unavailable:'基站存储暂不可用或已更换，请检查存储状态。',storage_catalog_unavailable:'录像目录暂不可读取，请稍后刷新。',archive_index_failed:'录像索引尚未完成更新，存储正在恢复；请刷新核对操作结果。',record_delete_io_failed:'删除时写盘失败，请检查存储状态并核对片段是否仍存在。',invalid_record_query:'录像查询范围或翻页标识无效，请重新查询。'})[code]||code||'基站存储请求失败';
  return Object.assign(Error(message),{code});
}
function recordingRange(start,end){
  if(!start&&!end)return [0,0];
  if(!start||!end)throw Error('请同时填写开始和结束时间，或清空两项以查询全部。');
  const from=Math.floor(new Date(start).getTime()/1000),to=Math.floor(new Date(end).getTime()/1000);
  if(!Number.isSafeInteger(from)||!Number.isSafeInteger(to)||from<0||to<=from)throw Error('结束时间必须晚于开始时间。');
  return [from,to];
}
// Commit pager state only after a successful response; a failed/aborted request
// cannot skip a page or retain a cursor from another storage generation.
async function loadRecordingCameras(client,current,hash='#recordings',{signal}={}) {
  // A recording keeps its original identity when the live slot is reused.
  const options=signal?{signal}:{};
  const caps=await client.command('capabilities',[],options);if(!caps.ok)throw storageError(caps.error);
  const owners=[];let generation=null;
  if(caps.record_cameras)for(let page=0;page<64;++page){
    const reply=await client.command('record-cameras',[page],options);if(!reply.ok)throw storageError(reply.error);
    if(generation!==null&&generation!==reply.storage_generation)throw storageError('storage_generation_conflict');generation=reply.storage_generation;
    owners.push(...reply.cameras);if(owners.length>=reply.total)break;
  }
  if(current&&!owners.some(o=>o.serial===current.serial&&o.slot===current.slot))owners.unshift({...current,records:0});
  const [,serial,slot]=hash.split('/');
  const selected=serial?owners.find(o=>o.serial===serial&&String(o.slot)===slot):
    null;
  if(serial&&!selected)throw Error('所选录像归档不在当前存储中，请重新选择。');
  return {owners,camera:selected,capabilities:caps};
}
function createRecordingPager(client,camera,{capabilities,signal}={}){
  const zero='0000000000000000';let supported=null,page=0,cursors=[zero],generation=0,range=[0,0],last=null,busy=false,boot=null;
  async function load(action='refresh',filter=range){
    if(busy)throw Error('正在读取录像目录，请稍候。');busy=true;
    try{
      if(supported===null){const caps=capabilities||await client.command('capabilities',[],signal?{signal}:{});if(!caps.ok)throw storageError(caps.error);supported=camera?caps.record_query===true:caps.record_query_all===true;if(!camera&&!supported)throw Error('此基站版本尚不支持全部摄像头查询，请选择单台摄像头或更新 Agent。');}
      const fresh=action==='refresh',nextPage=fresh?0:action==='next'?page+1:action==='previous'?Math.max(0,page-1):page;
      const nextCursors=fresh?[zero]:cursors.slice();if(action==='next'){if(!last?.has_more)throw Error('已是最后一页。');nextCursors[nextPage]=last.next_before;}
      const queryRange=fresh?filter:range;if(!supported&&queryRange.some(Boolean))throw Error('当前基站版本不支持时间范围查询，请先更新基站程序。');
      const args=supported?[...(camera?[camera.slot]:[]),nextCursors[nextPage],fresh?0:generation,...queryRange]:[camera.slot,nextPage];
      const reply=await client.command(camera?(supported?'record-query':'record-list'):'record-query-all',args,{...(camera?{cameraSerial:camera.serial}:{}),...(signal?{signal}:{})});
      if(!reply.ok)throw storageError(reply.error);
      const currentBoot=client.deviceBoot?.();if(!fresh&&boot&&currentBoot!==boot)throw storageError("storage_generation_conflict");
      const result=supported?reply:{...reply,has_more:(nextPage+1)*16<reply.total,next_before:zero};
      page=nextPage;cursors=nextCursors;generation=reply.storage_generation||0;range=queryRange.slice();last=result;boot=currentBoot;
      return {...result,page,supports_query:supported};
    }finally{busy=false;}
  }
  return {load};
}
const detectionKinds={5:{label:'检测到移动',english:'Movement detected',icon:'movement'},6:{label:'检测到人形',english:'Human detected',icon:'human'},7:{label:'检测到人脸',english:'Face detected',icon:'face'},8:{label:'检测到人形与人脸',english:'Human face detected',icon:'face'}};
function detectionKind(row){return detectionKinds[row.file_type]||{label:row.trigger==='manual'?'诊断片段':'检测类型未报告',english:'Detection type unavailable',icon:'unknown'};}
function detectionBadge(row){const k=detectionKind(row),paths={movement:'M4 8h6m-8 4h7m-5 4h6m5-11 3 4-3 4m3-4h3M13 15l-2 5m5-5 3 5',human:'M12 3a3 3 0 1 0 0 6 3 3 0 0 0 0-6M8 21v-7l-3 3m11 4v-7l3 3M8 14v-1a4 4 0 0 1 8 0v1m-4 2v5',face:'M8 3H4v4m12-4h4v4M4 17v4h4m12-4v4h-4M8 10h.01M16 10h.01M8 15q4 4 8 0',unknown:'M12 8v5m0 3h.01M12 3a9 9 0 1 0 0 18 9 9 0 0 0 0-18'};
  return `<span class="detection-badge ${k.icon}" title="${k.english}"><svg viewBox="0 0 24 24" aria-hidden="true"><path d="${paths[k.icon]}"></path></svg>${esc(k.label)}</span>`;
}
function clipCameraName(row,cameras=[]){const current=cameras.find(c=>c.serial===row.serial&&c.slot===row.slot);return current?(current.profile?.name||`摄像头 ${current.slot+1}`):row.serial;}
const thumbnailBlobs=new Map(),previewAliases=new Map();let thumbnailBytes=0;
const thumbnailKey=(client,row,storage)=>[client.baseUrl,client.endpoint(),storage,row.serial,row.slot,row.id,row.sha256||''].join('|');
async function previewId(client,row){const bytes=new TextEncoder().encode(['myvava-preview-v1',client.endpoint(),row.serial,row.slot,row.id,row.sha256||'',Math.floor(Date.now()/3600000)].join('|'));const hash=new Uint8Array(await crypto.subtle.digest('SHA-256',bytes));hash[0]|=128;return [...hash.slice(0,8)].map(b=>b.toString(16).padStart(2,'0')).join('');}
async function loadThumbnail(client,row,storage='tf',signal){
  const key=thumbnailKey(client,row,storage);if(thumbnailBlobs.has(key))return thumbnailBlobs.get(key);
  let id=row.id;
  if(storage==='tf'){
    id=previewAliases.get(key)||await previewId(client,row);
    const info=async()=>{try{const j=await client.request(client.endpoint()+'/files/'+id+'/info',undefined,signal);if(j.serial!==row.serial||j.slot!==row.slot||j.storage!=='tf_preview')throw Error('封面来源不匹配');return true;}catch(e){if(e.status===404)return false;throw e;}};
    if(!await info()){
      let j=await client.command('record-preview',[row.slot,row.id,id],{cameraSerial:row.serial,signal});
      if(!j.ok&&j.error==='export_unavailable_or_conflict'){const random=new Uint8Array(8);crypto.getRandomValues(random);random[0]|=128;id=[...random].map(b=>b.toString(16).padStart(2,'0')).join('');j=await client.command('record-preview',[row.slot,row.id,id],{cameraSerial:row.serial,signal});}
      if(!j.ok)throw storageError(j.error);
      if(previewAliases.size>=256)previewAliases.delete(previewAliases.keys().next().value);previewAliases.set(key,id);
      const until=Date.now()+20000;while(!await info()){if(Date.now()>until)throw Error('封面读取超时');await delay(150,signal);}
    }
  }
  const r=await client.raw(client.endpoint()+'/files/'+id+'/thumbnail',undefined,signal);if(!r.ok)throw Error('封面暂不可用');
  const blob=await r.blob();if(!blob.type.startsWith('image/jpeg')||blob.size>1048576||!blob.size)throw Error('封面格式无效');
  while(thumbnailBlobs.size>=64||thumbnailBytes+blob.size>8388608){const k=thumbnailBlobs.keys().next().value;if(k===undefined)break;thumbnailBytes-=thumbnailBlobs.get(k).size;thumbnailBlobs.delete(k);}
  thumbnailBlobs.set(key,blob);thumbnailBytes+=blob.size;return blob;
}
function createThumbnailLoader(client,{signal,active=()=>true}={}){
  let cleanupBatch=()=>{};
  return {attach(container,rows,storage='tf'){
    cleanupBatch();const ctl=new AbortController(),combined=signal?AbortSignal.any([signal,ctl.signal]):ctl.signal,urls=[];let queue=[],running=false,observer;
    const alive=()=>active()&&!combined.aborted;
    const run=async()=>{if(running)return;running=true;try{while(alive()&&queue.length){const {el,row}=queue.shift();if(!el.isConnected)continue;
      el.setAttribute('aria-busy','true');el.textContent='正在读取封面…';
      try{const blob=await loadThumbnail(client,row,storage,combined);if(!alive()||!el.isConnected)continue;const url=URL.createObjectURL(blob);urls.push(url);const img=document.createElement('img');img.src=url;img.alt=clipCameraName(row)+' · 录像封面';img.decoding='async';el.replaceChildren(img);el.classList.add('has-preview');}
      catch(e){if(alive()&&el.isConnected){el.textContent='封面暂不可用';el.title=e.message;el.classList.add('preview-unavailable');}}
      finally{el.removeAttribute('aria-busy');}
    }}finally{running=false;}};
    const enqueue=el=>{const row=rows[Number(el.dataset.previewIndex)];if(!row||el.dataset.previewQueued)return;el.dataset.previewQueued='1';queue.push({el,row});void run();};
    const elements=[...container.querySelectorAll('[data-preview-index]')];
    if(typeof IntersectionObserver!=='undefined'){observer=new IntersectionObserver(entries=>{for(const entry of entries)if(entry.isIntersecting){observer.unobserve(entry.target);enqueue(entry.target);}},{rootMargin:'200px'});elements.forEach(el=>observer.observe(el));}else elements.forEach(enqueue);
    cleanupBatch=()=>{ctl.abort();observer?.disconnect();queue=[];for(const url of urls)URL.revokeObjectURL(url);};
  },cleanup(){cleanupBatch();}};
}

// Viewing covers belong to this browser session, not the station's flash or
// its event archive. Keep camera and station identities when a slot is reused.
function createViewingFrameCache({maxEntries=32,maxBytes=8*1024*1024}={}) {
  const frames=new Map();let bytes=0;
  const key=(client,camera)=>[client.baseUrl,client.endpoint(),camera.serial,camera.slot].join('|');
  return {get:(client,camera)=>frames.get(key(client,camera)),
    set(client,camera,frame){
      if(!frame||!/^data:image\/jpeg;base64,/.test(frame.url)||!Number.isFinite(frame.at)||frame.url.length>maxBytes)return false;
      const k=key(client,camera),old=frames.get(k);if(old&&old.at>frame.at)return false;
      if(old){bytes-=old.url.length;frames.delete(k);}
      while(frames.size>=maxEntries||bytes+frame.url.length>maxBytes){const oldest=frames.keys().next().value;if(oldest===undefined)break;bytes-=frames.get(oldest).url.length;frames.delete(oldest);}
      frames.set(k,{...frame});bytes+=frame.url.length;return true;
    }};
}
const viewingFrames=createViewingFrameCache();
function captureViewingFrame(client,camera,video,makeCanvas=()=>document.createElement('canvas')) {
  if(video.readyState<2||!video.videoWidth||!video.videoHeight)return false;
  try{const canvas=makeCanvas();canvas.width=Math.min(640,video.videoWidth);canvas.height=Math.max(1,Math.round(video.videoHeight*canvas.width/video.videoWidth));
    canvas.getContext('2d').drawImage(video,0,0,canvas.width,canvas.height);
    return viewingFrames.set(client,camera,{url:canvas.toDataURL('image/jpeg',.8),at:Date.now()});
  }catch{return false;}
}
const latestPreviewRows=new Map();
function cameraPreviewStamp(client,cameras,storage={}) {
  return [client.baseUrl,client.endpoint(),storage.generation,storage.available,storage.records,storage.bytes,cameras.map(c=>c.serial+'/'+c.slot+'/'+(c.recording?.id||'')).join(',')].join('|');
}
function preferViewingFrame(frame,row) {
  if(!frame)return false;if(!row||!row.clock_valid)return true;
  return frame.at>=(row.unix_seconds*1000+(row.duration_ms||0));
}
function fillCameraPreviews(client,container,cameras,storage={}){
  let ctl=new AbortController(),version=0,lastStamp='',closed=false;
  const urls=new Map(),paints=new WeakMap();
  const cleanup=()=>{closed=true;ctl.abort();for(const url of urls.values())URL.revokeObjectURL(url);urls.clear();};
  const update=(cameras,storage={})=>{
    const catalogStamp=cameraPreviewStamp(client,cameras,storage),stamp=catalogStamp+'|'+cameras.map(c=>viewingFrames.get(client,c)?.at||0).join(',');
    if(closed||stamp===lastStamp)return;lastStamp=stamp;ctl.abort();ctl=new AbortController();const signal=ctl.signal,current=++version;
    const active=()=>!closed&&!signal.aborted&&current===version&&container.isConnected;
    const elements=[...container.querySelectorAll('[data-camera-preview]')];
    for(const [el,url] of urls)if(!el.isConnected){URL.revokeObjectURL(url);urls.delete(el);}
    const paint=async(el,key,source,caption,blob=false)=>{
      if(!active()||!el.isConnected||el.dataset.previewKey===key){if(blob)URL.revokeObjectURL(source);return;}
      const paintVersion=(paints.get(el)||0)+1;paints.set(el,paintVersion);
      const img=document.createElement('img');img.src=source;img.alt=(el.getAttribute('aria-label')||'摄像头')+' · '+(blob?'事件封面':'上次观看');img.decoding='async';
      try{await img.decode();if(!active()||!el.isConnected||paints.get(el)!==paintVersion||el.dataset.previewKey===key)return;
        const old=urls.get(el);el.replaceChildren(img);el.classList.add('has-preview');el.classList.remove('preview-unavailable');el.dataset.previewKey=key;el.removeAttribute('title');
        const label=el.closest('.camera-card')?.querySelector('.camera-preview-time');if(label)label.textContent=caption;
        urls.delete(el);if(blob)urls.set(el,source);if(old)URL.revokeObjectURL(old);
      }finally{if(blob&&urls.get(el)!==source)URL.revokeObjectURL(source);}
    };
    const cached=latestPreviewRows.get(catalogStamp);
    // Show a newly captured viewing frame immediately. Fetching the event
    // catalog never blanks an image which is already on screen.
    for(const camera of cameras){const frame=viewingFrames.get(client,camera),row=cached?.find(r=>r.serial===camera.serial&&r.slot===camera.slot),el=elements.find(el=>el.dataset.cameraPreview===camera.serial&&Number(el.dataset.slot)===camera.slot);
      if(el&&frame&&preferViewingFrame(frame,row))void paint(el,'view:'+frame.at,frame.url,'上次观看 · '+new Date(frame.at).toLocaleString()).catch(()=>{});
    }
    (async()=>{let rows=cached;
      if(!rows){
        const j=await client.command('record-query-all',['0000000000000000',0,0,0],{signal});if(!j.ok)throw storageError(j.error);
        rows=[];for(const camera of cameras){let row=j.records.find(r=>r.serial===camera.serial&&r.slot===camera.slot);
          if(!row&&j.has_more){const q=await client.command('record-query',[camera.slot,'0000000000000000',0,0,0],{cameraSerial:camera.serial,signal});if(q.ok)row=q.records[0];}
          if(row)rows.push(row);
        }
        if(!active())return;if(latestPreviewRows.size>=8)latestPreviewRows.delete(latestPreviewRows.keys().next().value);latestPreviewRows.set(catalogStamp,rows);
      }
      for(const camera of cameras){if(!active())return;
        const el=elements.find(el=>el.dataset.cameraPreview===camera.serial&&Number(el.dataset.slot)===camera.slot);if(!el)continue;
        const row=rows.find(r=>r.serial===camera.serial&&r.slot===camera.slot),frame=viewingFrames.get(client,camera);
        if(preferViewingFrame(frame,row)){await paint(el,'view:'+frame.at,frame.url,'上次观看 · '+new Date(frame.at).toLocaleString());continue;}
        if(!row){if(!el.querySelector('img'))el.textContent='尚无事件画面';continue;}
        const key='event:'+row.id+':'+(row.sha256||'');if(el.dataset.previewKey===key)continue;
        try{const blob=await loadThumbnail(client,row,'tf',signal);if(!active())return;
          await paint(el,key,URL.createObjectURL(blob),'最近事件 · '+(row.clock_valid?new Date(row.unix_seconds*1000).toLocaleString():'时间未确认'),true);
        }catch(e){if(active()&&!el.querySelector('img')){el.textContent='封面暂不可用';el.title=e.message;}}
      }
    })().catch(e=>{if(active())for(const el of elements)if(!el.querySelector('img')){el.textContent='暂无历史预览';el.title=e.message;}});
  };
  cleanup.update=update;update(cameras,storage);return cleanup;
}
const resolvedTfClips=new Map();
async function resolveClip(client,{id,serial,slot,storage='tf'},{signal,progress=()=>{}}={}) {
  if(!/^[a-f0-9]{16}$/.test(id)||!id||!/^[A-Za-z0-9_.:-]{1,31}$/.test(serial)||!Number.isInteger(slot)||slot<0||slot>3)throw Error('无效的事件短片标识。');
  const cacheKey=client.baseUrl+'|'+client.endpoint()+'/'+serial+'/'+slot+'/'+id;
  const archive=async target=>{try{const f=await client.request(client.endpoint()+'/files/'+target+'/info',undefined,signal);if(f.serial!==serial||f.storage!==(storage==='cloud'?'cloud':'tf_export'))throw Error('短片来源或摄像头身份不匹配。');if(storage==='tf'){if(resolvedTfClips.size>=256)resolvedTfClips.delete(resolvedTfClips.keys().next().value);resolvedTfClips.set(cacheKey,target);}return target;}catch(e){if(e.status===404)return null;throw e;}};
  if(storage==='cloud'){if(await archive(id))return id;throw Error('云存储中没有这段短片；可能未开启上传、尚未上传完成或已被删除。');}
  if(storage!=='tf')throw Error('无效的视频来源。');
  const cached=resolvedTfClips.get(cacheKey);if(cached){if(await archive(cached))return cached;resolvedTfClips.delete(cacheKey);}
  const command=async(name,args=[])=>{const r=await client.command(name,[slot,...args],{cameraSerial:serial,signal});if(!r.ok)throw Error(r.error||'基站存储请求失败');return r;};
  let jobs=(await command('export-list')).exports.filter(j=>j.source_id===id),job=null;
  for(const j of jobs)if(j.state==='committed'&&await archive(j.id))return j.id;
  job=jobs.find(j=>['queued','uploading','retry_wait'].includes(j.state));
  if(!job){
    const random=crypto.getRandomValues(new Uint8Array(8));random[0]|=128;const alias=Array.from(random,n=>n.toString(16).padStart(2,'0')).join('');
    try{await command('record-export',[id,alias]);}catch(e){
      // Admission already validates the immutable source. Query its state only
      // on rejection, avoiding a serial remote round trip on every successful play.
      const source=await command('record-info',[id]);
      const reason=({recording:'短片正在录制，请稍后重试。',finishing:'短片正在保存，请稍后重试。',failed:'短片保存失败，原因可在事件详情查看。',missing:'TF 原片已清理或不在当前存储卡中。'})[source.state];
      if(reason)throw Error(reason);throw e;
    }
    job={id:alias,state:'queued',offset:0,bytes:0};progress(job);
  }
  let nextStatus=Date.now()+2000;
  for(let n=0;n<900;++n){
    // Server commit is authoritative and durable. A local metadata check does
    // not wait for the station's next command poll or receipt publication.
    if(await archive(job.id))return job.id;
    if(Date.now()>=nextStatus){
      const current=(await command('export-list')).exports.find(j=>j.id===job.id);if(!current)throw Error('调取任务已不在当前存储卡中。');
      progress(current);if(current.state==='committed')throw Error('已完成的服务器副本已删除或不可用。');
      if(['failed','destination_changed','cancelled','source_expired'].includes(current.state))throw Error(current.error||current.state);
      nextStatus=Date.now()+2000;
    }
    await delay(100,signal);
  }
  throw Error('调取仍在后台进行，请在 TF 卡视频的调取任务中查看。');
}
// Save the received fMP4 on the viewing device. No captureStream re-encoding:
// background rendering and muted audio must not alter the recorded timeline.
function createLocalRecording({save,onState=()=>{},onError=()=>{},maxBytes=64*1024*1024}) {
  let session=null,pending=new Uint8Array(),ftyp=null,moov=null,fragment=null,tracks=new Map();
  const invalid=()=>Error('直播 MP4 分片格式不受支持，无法完整保存本机录像。');
  const view=a=>new DataView(a.buffer,a.byteOffset,a.byteLength);
  const type=(a,p)=>String.fromCharCode(...a.subarray(p+4,p+8));
  const boxes=(a,start=0,end=a.length)=>{
    const out=[];for(let p=start;p<end;){if(p+8>end)throw invalid();const n=view(a).getUint32(p);if(n<8||p+n>end)throw invalid();out.push({type:type(a,p),p,n});p+=n;}return out;
  };
  const child=(a,b,name)=>boxes(a,b.p+8,b.p+b.n).find(x=>x.type===name);
  const readTracks=a=>{
    const result=new Map(),root={p:0,n:a.length};
    for(const trak of boxes(a,8).filter(b=>b.type==='trak')){
      const tkhd=child(a,trak,'tkhd'),mdia=child(a,trak,'mdia');if(!tkhd||!mdia)throw invalid();
      const mdhd=child(a,mdia,'mdhd'),hdlr=child(a,mdia,'hdlr');if(!mdhd||!hdlr)throw invalid();
      const id=view(a).getUint32(tkhd.p+(a[tkhd.p+8]===1?28:20)),scale=view(a).getUint32(mdhd.p+(a[mdhd.p+8]===1?28:20));
      if(!scale||result.has(id))throw invalid();result.set(id,{scale,video:String.fromCharCode(...a.subarray(hdlr.p+16,hdlr.p+20))==='vide',flags:0});
    }
    const mvex=child(a,root,'mvex');if(!mvex||![...result.values()].some(t=>t.video))throw invalid();
    for(const trex of boxes(a,mvex.p+8,mvex.p+mvex.n).filter(b=>b.type==='trex')){const t=result.get(view(a).getUint32(trex.p+12));if(!t||trex.n<32)throw invalid();t.flags=view(a).getUint32(trex.p+28);}
    return result;
  };
  const inspect=a=>{
    const entries=[];let key=false;const v=view(a);
    for(const traf of boxes(a,8).filter(b=>b.type==='traf')){
      const tfhd=child(a,traf,'tfhd'),tfdt=child(a,traf,'tfdt'),trun=child(a,traf,'trun');if(!tfhd||!tfdt||!trun)throw invalid();
      const flags=v.getUint32(tfhd.p+8)&0xffffff,t=tracks.get(v.getUint32(tfhd.p+12));
      // The server emits default-base-is-moof. Absolute file offsets cannot be
      // reused after selecting a range of fragments.
      if(!t||!(flags&0x20000)||(flags&1))throw invalid();
      let p=tfhd.p+16;if(flags&2)p+=4;if(flags&8)p+=4;if(flags&16)p+=4;
      let sampleFlags=flags&32?v.getUint32(p):t.flags;
      const runFlags=v.getUint32(trun.p+8)&0xffffff,count=v.getUint32(trun.p+12);p=trun.p+16;if(runFlags&1)p+=4;
      if(runFlags&4)sampleFlags=v.getUint32(p);
      else if(runFlags&0x400){if(runFlags&0x100)p+=4;if(runFlags&0x200)p+=4;sampleFlags=v.getUint32(p);}
      const version=a[tfdt.p+8];if(version>1||tfdt.n<(version?20:16))throw invalid();
      const time=version?v.getBigUint64(tfdt.p+12):BigInt(v.getUint32(tfdt.p+12));
      entries.push({track:t,offset:tfdt.p+12,version,time});
      if(t.video&&count&&!(sampleFlags&0x10000)&&((sampleFlags>>>24)&3)!==1)key=true;
    }
    if(!entries.length)throw invalid();return {entries,key};
  };
  const stop=()=>{
    if(!session)return Promise.resolve();const current=session;if(current.done)return current.done;
    current.stopping=true;onState('saving');
    current.done=Promise.resolve().then(()=>{if(current.fragments)return save(new Blob(current.chunks,{type:'video/mp4'}),'.mp4');onError(Error('尚未收到完整关键帧，未生成录像。'));}).catch(onError).finally(()=>{current.chunks=[];if(session===current)session=null;onState('idle');});
    return current.done;
  };
  const accept=(head,data)=>{
    const current=session;if(!current||current.stopping)return;
    const meta=inspect(head);if(!current.origin){if(!meta.key)return;
      // One shared origin preserves A/V offset across different timescales.
      const first=meta.entries.reduce((a,b)=>a.time*BigInt(b.track.scale)<=b.time*BigInt(a.track.scale)?a:b);
      current.origin={time:first.time,scale:BigInt(first.track.scale)};
    }
    if(current.bytes+head.length+data.length>maxBytes){onError(Error('本次录像达到容量上限，已停止并保存完整分片。'));void stop();return;}
    const copy=head.slice(),v=view(copy),origin=current.origin;
    for(const e of meta.entries){const base=origin.time*BigInt(e.track.scale)/origin.scale;if(e.time<base)throw invalid();const adjusted=e.time-base;
      if(e.version)v.setBigUint64(e.offset,adjusted);else {if(adjusted>0xffffffffn)throw invalid();v.setUint32(e.offset,Number(adjusted));}}
    const mfhd=boxes(copy,8).find(b=>b.type==='mfhd');if(mfhd)v.setUint32(mfhd.p+12,current.fragments+1);
    current.chunks.push(copy,data);current.bytes+=copy.length+data.length;current.fragments++;
  };
  const push=value=>{
    const merged=new Uint8Array(pending.length+value.length);merged.set(pending);merged.set(value,pending.length);let p=0;
    try{while(p+8<=merged.length){const n=view(merged).getUint32(p);if(n<8||n>8*1024*1024)throw invalid();if(p+n>merged.length)break;
        const kind=type(merged,p),box=merged.slice(p,p+n);p+=n;
        if(fragment&&kind!=='mdat')throw invalid();
        if(kind==='ftyp'){ftyp=box;}
        else if(kind==='moov'){if(moov&&session){onError(Error('摄像头编码已变化，本机录像已结束并保存；再次点击可录制新文件。'));void stop();}tracks=readTracks(box);moov=box;}
        else if(kind==='moof'){if(!ftyp||!moov)throw invalid();fragment=box;}
        else if(kind==='mdat'){if(!fragment)throw invalid();accept(fragment,box);fragment=null;}
      }pending=merged.slice(p);
    }catch(e){void stop();throw e;}
  };
  const start=()=>{
    if(session)throw Error('上一段录像正在收尾，请等待保存完成。');if(!ftyp||!moov)throw Error('尚未收到可录制的音视频。');
    if(ftyp.length+moov.length>=maxBytes)throw invalid();
    session={chunks:[ftyp,moov],bytes:ftyp.length+moov.length,origin:null,fragments:0,stopping:false};onState('recording');
  };
  const reset=()=>{if(session)throw Error('录像尚未保存完成。');pending=new Uint8Array();ftyp=moov=fragment=null;tracks=new Map();};
  return {start,stop,push,reset,get busy(){return !!session;}};
}
// Auto is a viewer policy, never camera quality enum 3. A proposal consumes its
// rolling budget even on failure: uncertain Flash writes must not be retried implicitly.
function createAdaptiveQuality({now=()=>performance.now(),cooldown=8000,maxChanges=3,windowMs=300000}={}) {
  let last=now(),bad=0,good=0,changes=0,recent=[];
  return {reset(){last=now();bad=good=0;},get changes(){return changes;},observe({quality,buffered,stalled=false,dropRatio=0,progressRatio=1,suspended=false}) {
    if(suspended||!Number.isInteger(quality)||quality<0||quality>2||!Number.isFinite(buffered)){bad=good=0;return null;}
    // Low latency deliberately keeps less than one second buffered. A small
    // buffer alone is healthy while the playback clock advances normally.
    const poor=stalled||dropRatio>.12||progressRatio<.5||(buffered<.1&&progressRatio<.85);
    bad=poor?bad+1:0;good=!poor&&buffered>=.15&&progressRatio>=.85&&dropRatio<.02?good+1:0;
    recent=recent.filter(t=>now()-t<windowMs);if(recent.length>=maxChanges||now()-last<cooldown)return null;
    const next=bad>=2&&quality<2?quality+1:good>=2&&quality>0?quality-1:null;
    if(next!==null){changes++;last=now();recent.push(last);bad=good=0;}return next;
  }};
}
function observedLiveQuality(reply) {
  const op=reply.settings?.operations?.quality;
  if(op?.current_connection&&op.phase==='echo_observed'&&Number.isInteger(op.value)&&op.value>=0&&op.value<=2)return op.value;
  const r=reply.settings?.reports?.first_connect,q=r?.values?.live_quality_raw;
  return r?.current_connection&&Number.isInteger(q)&&q>=0&&q<=2?q:null;
}
const liveSizes=[[1920,1080],[1280,720],[640,360]];
function pictureQuality(video) {return liveSizes.findIndex(([w,h])=>video.videoWidth===w&&video.videoHeight===h);}
async function waitLivePicture({video,quality=null,signal,now=()=>performance.now(),wait=delay,timeout=20000}) {
  if(typeof video.requestVideoFrameCallback==='function')return new Promise((resolve,reject)=>{
    let callback=0,first=null,frames=0;
    const cleanup=()=>{clearTimeout(timer);signal?.removeEventListener('abort',abort);if(callback)video.cancelVideoFrameCallback(callback);};
    const abort=()=>{cleanup();reject(signal.reason);};
    const timer=setTimeout(()=>{cleanup();reject(Error(quality===null?'等待可播放画面超时。':'尚未看到目标分辨率的连续画面，请核对画质设置。'));},timeout);
    const frame=(_,meta)=>{
      if(signal?.aborted){abort();return;}
      const actual=pictureQuality({videoWidth:meta.width||video.videoWidth,videoHeight:meta.height||video.videoHeight});
      if(!video.paused&&video.readyState>=2&&(quality===null||actual===quality)){
        if(first===null){first=meta.mediaTime;frames=1;}else if(++frames>=2&&meta.mediaTime-first>=.1){cleanup();resolve(actual<0?null:actual);return;}
      }else{first=null;frames=0;}
      callback=video.requestVideoFrameCallback(frame);
    };
    signal?.addEventListener('abort',abort,{once:true});if(signal?.aborted)abort();else callback=video.requestVideoFrameCallback(frame);
  });
  const until=now()+timeout;let first=null,firstFrames=null;
  for(;;){signal?.throwIfAborted();if(video.error)throw Error('播放器解码失败，请重新开始观看。');
    const actual=pictureQuality(video);
    if(video.readyState>=2&&!video.paused&&video.videoWidth>0&&video.videoHeight>0&&(quality===null||quality===actual)){
      const frames=video.getVideoPlaybackQuality?.().totalVideoFrames;
      if(first===null){first=video.currentTime;firstFrames=frames;}
      else if(video.currentTime-first>=.1&&(!Number.isFinite(firstFrames)||frames-firstFrames>=2))return actual<0?null:actual;
    }else first=null;
    if(now()>=until)throw Error(quality===null?'等待可播放画面超时。':'尚未看到目标分辨率的连续画面，请核对画质设置。');
    await wait(100,signal);
  }
}
// Protect clips, other viewers, station boot, camera identity and configuration
// revision. The quality operation has its own timeout; viewing has no fixed limit.
// The caller starts a fresh MP4 pipeline
// only after a matched echo; this is not proof of image effect or Flash durability.
async function switchLiveQuality({client,camera,quality,signal,deadline,pause,lease=null,progress=()=>{},now=()=>performance.now(),wait=delay}) {
  if(!Number.isInteger(quality)||quality<0||quality>2)throw Error('画质只能是高清、标清或流畅。');
  const boot=client.deviceBoot();if(!boot)throw Error('基站连接标识不可用，请重新打开观看页面。');
  const expires=deadline??now()+90000;
  const check=()=>{signal?.throwIfAborted();if(now()>=expires)throw Error('画质切换等待超时，请核对当前设置。');};
  const cmd=async(name,args=[],extra={})=>{check();const j=await client.command(name,args,{...(name==='status'?{}:{cameraSerial:camera.serial}),signal,expectedBoot:boot,...extra});check();if(client.deviceBoot()!==boot)throw Error('基站已重启，请重新开始观看。');if(!j.ok)throw Error(j.error||'画质设置请求失败');return j;};
  const idle=async includeViewers=>{for(;;){const j=await cmd('status'),d=j.devices.find(d=>d.slot===camera.slot&&d.serial===camera.serial);if(!d?.paired)throw Error('摄像头配对身份已改变。');if(!d.policy.enabled)throw Error('摄像头已停用。');if(!d.recording.active&&!d.recording.finishing&&(!includeViewers||(!d.viewers&&!d.live_requested)))return;
    progress(d.recording.active||d.recording.finishing?'等待事件短片保存完成…':'等待其他观看连接释放…');await wait(500,signal);}};
  // Read and reject conflicting configuration while the current picture is
  // still playing. The revision guard below detects changes during release.
  await idle(false);check();
  let before=await cmd('settings',[camera.slot]);
  if(before.settings.pending||before.desired.pending_mask)throw Error('有其他设置正在下发，请完成后再切换画质。');
  const revision=before.desired.revision;
  let result;
  if(lease){
    if(!/^[a-f0-9]{16}$/.test(lease))throw Error('观看连接标识无效。');
    result=await cmd('live-quality',[camera.slot,lease,quality],{expectedRevision:revision});
  }else{
  await pause();check();await idle(true);
  if(before.desired.values.quality===quality){
    if(observedLiveQuality(before)===quality)return {quality,echoOnly:true};
    result=await cmd('apply-settings',[camera.slot,'quality'],{expectedRevision:revision});
  }else result=await cmd('configure',[camera.slot,'quality',quality],{expectedRevision:revision});
  }
  const saved=result.desired.revision;progress('画质已保存，等待摄像头回显…');
  for(;;){const j=await cmd('settings',[camera.slot]),item=j.desired.items?.quality;
    if(j.desired.revision!==saved||j.desired.values.quality!==quality)throw Error('设置已被其他操作修改，请刷新后核对。');
    if(item?.phase==='echo_observed'&&item.current_connection&&item.operation_id&&observedLiveQuality(j)===quality)return {quality,echoOnly:true};
    if(['echo_timeout','disconnected','queue_timeout','apply_deadline_expired','explicit_retry_required','explicit_apply_required'].includes(item?.phase))throw Error('画质未得到确认；请核对设置后手动重试，不会自动重发。');
    await wait(350,signal);
  }
}

// Network chunks are arbitrary. Only validated, bounded complete MVL2 records
// reach MSE or the local recording parser.
function createLiveRecordParser(onRecord){
  let pending=new Uint8Array(),epoch=0,configured=false,initialized=false;
  return {async push(value){
    if(value.length>1024*1024)throw Error('直播数据块超过限制。');
    const merged=new Uint8Array(pending.length+value.length);merged.set(pending);merged.set(value,pending.length);let at=0;
    while(merged.length-at>=16){const h=merged.subarray(at,at+16),v=new DataView(h.buffer,h.byteOffset,h.byteLength),kind=h[4],next=v.getUint32(8),n=v.getUint32(12);
      if(String.fromCharCode(...h.subarray(0,4))!=='MVL2'||h[5]||h[6]||h[7]||kind<1||kind>4||!next||n>512*1024||((kind===1||kind===4)&&n>4096)||(kind===2&&n>65536))throw Error('直播记录格式无效。');
      if(merged.length-at<16+n)break;const data=merged.slice(at+16,at+16+n);at+=16+n;
      if(kind===1){if(next<=epoch)throw Error('直播编码代次无效。');epoch=next;configured=true;initialized=false;}
      else if(next!==epoch||!configured||(kind===3&&!initialized))throw Error('直播初始化顺序无效。');
      if(kind===2){if(initialized)throw Error('直播初始化重复。');initialized=true;}
      await onRecord({kind,epoch:next,data});
    }
    pending=merged.slice(at);if(pending.length>512*1024+16)throw Error('直播记录超过限制。');
  },finish(){if(pending.length)throw Error('直播记录未完整接收。');}};
}

function storagePolicyCommand(camera,kind,enabled) {
  if(!['cloud','nas'].includes(kind))throw Error('无效的存储设置');
  return `if-serial ${camera.serial} if-revision ${camera.policy.revision} policy-config ${camera.slot} ${kind}=${enabled?1:0}`;
}
const playerHTML='<section id="archivePlayer" class="card" hidden><h2 id="archiveTitle">短片播放</h2><video id="archiveVideo" controls playsinline style="width:100%;max-height:60vh"></video><p id="archiveProgress"></p><button id="archiveCancel">取消加载</button></section>';
function archivePlayer(client,{active,storage,serial}) {
  let controller=null,objectURL=null;
  const cleanup=()=>{controller?.abort();controller=null;const video=document.getElementById('archiveVideo');if(video){video.removeAttribute('src');video.load();}if(objectURL)URL.revokeObjectURL(objectURL);objectURL=null;};
  document.getElementById('archiveCancel').onclick=()=>controller?.abort();
  const open=async(id,action='playback')=>{
    cleanup();const ctl=new AbortController();controller=ctl;const timer=setTimeout(()=>ctl.abort(),70000);
    document.getElementById('archivePlayer').hidden=false;document.getElementById('archiveTitle').textContent=(action==='download'?'下载':'播放')+' · '+id;document.getElementById('archiveProgress').textContent='正在读取短片…';
    try{
      const meta=await client.request(client.endpoint()+'/files/'+id+'/info',undefined,ctl.signal).catch(e=>{if(e.status===404)throw Error(storage==='cloud'?'云存储中没有这段视频；可能未开启上传、尚未上传完成或已被删除。':storage==='tf_export'?'TF 回放缓存已到期或删除，请返回 TF 卡视频重新调取。':'找不到这段历史导入视频。');throw e;});
      if(meta.storage!==storage||(serial&&meta.serial!==serial))throw Error('短片来源或摄像头身份不匹配。');
      const r=await client.raw(client.endpoint()+'/files/'+id+'/'+action,undefined,ctl.signal);if(!r.ok){const j=await r.json();throw Error(j.error||'媒体读取失败');}
      const chunks=[];let total=0;const reader=r.body.getReader();try{for(;;){const {done,value}=await reader.read();if(done)break;total+=value.byteLength;if(total>160*1024*1024)throw Error('短片超过浏览器 160 MiB 接收上限。');chunks.push(value);if(active())document.getElementById('archiveProgress').textContent=`已接收 ${(total/1048576).toFixed(2)} MiB`;}}finally{await reader.cancel().catch(()=>{});}
      if(!active()||ctl.signal.aborted)return;const blob=new Blob(chunks,{type:action==='playback'?'video/mp4':'video/mp2t'});if(!blob.size)throw Error('媒体为空。');
      if(action==='download'){if(blob.size!==meta.bytes)throw Error('短片长度不一致，未保存。');const digest=await crypto.subtle.digest('SHA-256',await blob.arrayBuffer());const sha=Array.from(new Uint8Array(digest),x=>x.toString(16).padStart(2,'0')).join('');if(sha!==meta.sha256)throw Error('短片校验失败，未保存。');}
      if(!active()||ctl.signal.aborted)return;objectURL=URL.createObjectURL(blob);if(action==='playback'){const video=document.getElementById('archiveVideo');video.src=objectURL;video.play().catch(()=>{});}else{const a=document.createElement('a');a.href=objectURL;a.download=id+'.ts';a.click();}
      document.getElementById('archiveProgress').textContent=action==='playback'?'短片已加载；可播放和拖动进度。':'长度与 SHA-256 校验通过，已交给浏览器保存。';
    }finally{clearTimeout(timer);}
  };
  return {open,cleanup};
}
async function login(base,username,password){const client=createClient({baseUrl:base,token:''});return client.request('/login',{username,password});}
let renderVersion=0,mediaCleanup=()=>{},pageController=null;
const recordings=new Set();
const requiresStation=page=>['overview','camera','station','live','recordings','event-clip','tf-clip','storage-policy','nas-settings','nas-policy','cloud-settings','storage-services'].includes(page);
function scopedClient(client,signal) {
  const endpoint=client.endpoint(),stationId=client.stationId;
  const combined=other=>other?AbortSignal.any([signal,other]):signal;
  return {...client,stationId,endpoint:()=>endpoint,deviceBoot:()=>client.deviceBoot?.(stationId),
    request:(path,body,other)=>client.request(path,body,combined(other)),
    raw:(path,body,other)=>client.raw(path,body,combined(other)),
    devices:other=>client.devices(combined(other)),
    getPending:()=>client.getPending(stationId),clearPending:()=>client.clearPending(stationId),
    recover:other=>client.recover(combined(other),stationId),
    command:(name,args=[],options={})=>client.command(name,args,{...options,stationId,signal:combined(options.signal)}),
    commandLineRaw:(line,options={})=>client.commandLineRaw(line,{...options,stationId,signal:combined(options.signal)})};
}
async function render(page,{client,content,notice,camera,cameras=[],api,autoStart=false,embedded=false}) {
  pageController?.abort();pageController=new AbortController();
  const version=++renderVersion,active=()=>version===renderVersion&&content.isConnected;
  client=scopedClient(client,pageController.signal);
  api=async line=>{const j=await (await client.commandLineRaw(line,{signal:AbortSignal.timeout(45000)})).json();if(!j.ok){const e=Error(j.error||'操作失败');e.code=j.error;throw e;}return j;};
  const showError=e=>{if(active()&&e.name!=='AbortError')notice(e.message,true);};
  if(page==='live') {
    if(!camera){content.innerHTML='<section class="card">先选择已配对摄像头。</section>';return true;}
    const icon=name=>`<svg viewBox="0 0 24 24" aria-hidden="true">${({play:'<path d="M8 5.8Q8 4.6 9.1 5.3l10 5.8q1.1.7 0 1.4l-10 5.8Q8 19 8 17.8Z" fill="currentColor" stroke-linejoin="round"/>',stop:'<path d="M8 5v14M16 5v14" stroke-width="3.5"/>',sound:'<path d="M11 5 6 9H3v6h3l5 4V5Zm4 3a6 6 0 0 1 0 8m3-11a10 10 0 0 1 0 14"/>',muted:'<path d="M11 5 6 9H3v6h3l5 4V5Zm5 4 5 6m0-6-5 6"/>',fullscreen:'<path d="M8 3H3v5m13-5h5v5M3 16v5h5m13-5v5h-5"/>',exit:'<path d="M3 8h5V3m8 0v5h5M8 21v-5H3m18 0h-5v5"/>',quality:'<path d="m9 3-1 3-3 1-2 5 2 5 3 1 1 3h6l1-3 3-1 2-5-2-5-3-1-1-3H9Z"/><circle cx="12" cy="12" r="3"/>'})[name]}</svg>`;
    content.innerHTML=`<section class="card live-section">${embedded?'':`<h2>实时观看 · ${esc(clipCameraName(camera,[camera]))}</h2>`}<div id="livePlayer" class="live-player" role="group" aria-label="实时视频播放器" tabindex="0"><video id="liveVideo" playsinline autoplay muted disablepictureinpicture></video><div id="liveOverlay" class="live-overlay"><span id="liveSpinner" class="live-spinner" hidden></span><p id="liveProgress" role="status">准备观看</p><button id="liveRetry" class="live-retry" aria-label="开始观看">${icon('play')}</button></div><span id="liveRecording" class="live-recording" role="status" hidden>本机录制中 <time id="liveRecordElapsed">00:00</time></span><div class="live-controls"><button id="liveToggle" class="player-button" aria-label="开始观看" title="开始观看">${icon('play')}</button><span id="liveBadge" class="live-badge">LIVE</span><time id="liveElapsed" class="live-elapsed" aria-label="本次观看时长">00:00</time><div class="live-controls-spacer"></div><div class="live-audio"><button id="liveMute" class="player-button" aria-label="开启声音" title="开启声音">${icon('muted')}</button><input id="liveVolume" type="range" min="0" max="1" step="0.05" value="1" aria-label="音量"></div><div class="live-quality"><button id="liveQuality" class="player-button live-quality-button" aria-label="画质" aria-expanded="false" aria-controls="liveQualityMenu" ${client.user.role>1?'':'disabled'}>${icon('quality')}<span id="liveQualityLabel">自动</span></button><div id="liveQualityMenu" class="live-quality-menu" role="group" aria-label="选择直播画质" hidden><p>画质</p>${[['0','1080P'],['1','720P'],['2','360P'],['auto','自动']].map(([value,label])=>`<button type="button" data-quality="${value}" aria-pressed="${value==='auto'}"><span>${label}</span><span class="quality-check" aria-hidden="true">✓</span></button>`).join('')}</div></div><button id="liveFullscreen" class="player-button" aria-label="全屏" title="全屏">${icon('fullscreen')}</button></div></div><p id="liveQualityState" class="live-quality-state" role="status"></p><div class="row live-actions"><button id="liveSnapshot" disabled>保存画面</button><button id="liveRecord" disabled>录像到本机</button>${client.user.role>1?'<label class="live-talk-mode"><input id="liveTalkMode" type="checkbox">启用对讲</label><button id="liveTalk" disabled>开启麦克风</button>':''}</div><p class="hint live-record-hint">点击“录像到本机”后才开始录制，文件保存到当前观看设备。</p></section>`;
    let ctl=null,url=null,stream=null,encoder=null,processor=null,audioContext=null,stopGeneration=0,talkLease='',talkQueue=Promise.resolve(),sending=0;
    let video=document.getElementById('liveVideo');const player=document.getElementById('livePlayer'),qualityButton=document.getElementById('liveQuality'),qualityMenu=document.getElementById('liveQualityMenu'),uiSignal=pageController.signal;
    let session=null,autoTimer=0,switching=false,starting=false,stopping=null,ready=false,quality=null,qualityMode='auto',policy=null,stalled=false,lastFrames=0,lastDropped=0,lastTime=0,lastSampleAt=0,startedAt=null,recordingStartedAt=null,continuous=false,qualityChanging=false,internalReset=false;
    const qualityName=q=>['高清 1080P','标清 720P','流畅 360P'][q]||'待确认';
    const qualityState=t=>{if(active())document.getElementById('liveQualityState').textContent=t;};
    const closeQuality=()=>{qualityMenu.hidden=true;qualityButton.setAttribute('aria-expanded','false');};
    const controls=()=>{if(!active())return;const playing=!!session,toggle=document.getElementById('liveToggle');
      toggle.disabled=!!stopping||starting;toggle.innerHTML=icon(playing?'stop':'play');toggle.title=playing?'停止观看':'重新观看';toggle.setAttribute('aria-label',toggle.title);
      document.getElementById('liveOverlay').hidden=ready;
      document.getElementById('liveSpinner').hidden=!playing;
      const retry=document.getElementById('liveRetry');retry.hidden=playing;retry.disabled=!!stopping||starting;retry.setAttribute('aria-label',startedAt===null?'开始观看':'重新观看');
      document.getElementById('liveBadge').classList.toggle('active',ready&&(!switching||continuous));
      document.getElementById('liveSnapshot').disabled=!ready||switching;document.getElementById('liveRecord').disabled=!ready||switching;
      qualityButton.disabled=client.user.role<2||!ready||switching||recording.busy;
      document.getElementById('liveQualityLabel').textContent=qualityMode==='auto'?'自动'+(quality!==null?' · '+['1080P','720P','360P'][quality]:''):['1080P','720P','360P'][Number(qualityMode)];
      qualityMenu.querySelectorAll('[data-quality]').forEach(b=>b.setAttribute('aria-pressed',String(b.dataset.quality===qualityMode)));
      if(qualityButton.disabled)closeQuality();
      const talk=document.getElementById('liveTalk');if(talk)talk.disabled=!ready||switching||!document.getElementById('liveTalkMode').checked;
      const mode=document.getElementById('liveTalkMode');if(mode)mode.disabled=!!session;};
    const status=t=>{if(active()){document.getElementById('liveProgress').textContent=t;if(switching&&ready)qualityState(t);}};
    const stopMic=()=>{stream?.getTracks().forEach(t=>t.stop());stream=null;processor?.disconnect();processor=null;if(encoder&&encoder.state!=='closed')encoder.close();encoder=null;audioContext?.close().catch(()=>{});audioContext=null;if(active()&&document.getElementById('liveTalk'))document.getElementById('liveTalk').textContent='开启麦克风';};
    const save=(blob,name)=>{const u=URL.createObjectURL(blob),a=document.createElement('a');a.href=u;a.download=name;a.click();setTimeout(()=>URL.revokeObjectURL(u),10000);};
    const recording=createLocalRecording({save:(blob,extension)=>save(blob,camera.serial+'-'+Date.now()+extension),onError:showError,onState:state=>{if(state==='idle'){recordings.delete(recording);recordingStartedAt=null;}if(state==='recording')recordingStartedAt=performance.now();if(active()){document.getElementById('liveRecording').hidden=state!=='recording';if(state==='recording')document.getElementById('liveRecordElapsed').textContent='00:00';const button=document.getElementById('liveRecord');button.textContent=state==='recording'?'停止录像并保存到本机':state==='saving'?'正在保存…':'录像到本机';controls();button.disabled=state==='saving'||!ready||switching;}}});
    const stopMedia=async()=>{const generation=++stopGeneration;captureViewingFrame(client,camera,video);ctl?.abort();ctl=null;ready=false;stopMic();controls();await recording.stop();if(generation!==stopGeneration)return;if(url)URL.revokeObjectURL(url);url=null;video.removeAttribute('src');const cover=viewingFrames.get(client,camera);if(cover)video.poster=cover.url;video.load();controls();};
    const stop=()=>{if(stopping)return stopping;session?.abort();session=null;clearInterval(autoTimer);switching=false;stopping=stopMedia().finally(()=>{stopping=null;controls();});controls();return stopping;};
    const userStop=()=>{status('观看已停止');qualityState('点击播放按钮重新连接摄像头');closeQuality();void stop();};
    const updateSound=()=>{if(!active())return;const muted=video.muted||video.volume===0,b=document.getElementById('liveMute');b.innerHTML=icon(muted?'muted':'sound');b.title=muted?'开启声音':'静音';b.setAttribute('aria-label',b.title);document.getElementById('liveVolume').value=video.volume;};
    const watchVideo=()=>{for(const event of ['waiting','stalled'])video.addEventListener(event,()=>{if(ready)stalled=true;});video.addEventListener('volumechange',updateSound);video.addEventListener('pause',()=>{if(ready&&session&&!switching&&!internalReset&&!video.ended)userStop();});};
    watchVideo();
    const changeQuality=async next=>{
      if(!session||switching||recording.busy)return;const watching=session;qualityChanging=true;switching=true;controls();
      status('正在切换画质…');
      try{await switchLiveQuality({client,camera,quality:next,signal:AbortSignal.any([watching.signal,AbortSignal.timeout(90000)]),pause:stopMedia,lease:continuous?talkLease:null,progress:status});
        if(session!==watching||watching.signal.aborted)return;policy.reset();qualityState(qualityName(next)+' · 等待实际画面确认');if(continuous){const actual=await waitLivePicture({video,quality:next,signal:watching.signal});if(session!==watching||watching.signal.aborted)return;quality=actual;status('正在观看');qualityState((qualityMode==='auto'?'自动 · ':'')+qualityName(actual)+' · '+video.videoWidth+' × '+video.videoHeight);}else void play(watching,next);
      }catch(e){if(session===watching){if(!continuous)await stop();if(e.name!=='AbortError'){status('画质切换未完成：'+e.message);qualityState(continuous?'画质结果未确认；当前观看继续，不会自动重发。':'观看已停止，可点击画面重新观看');}}}
      finally{qualityChanging=false;if(continuous)switching=false;if(session===watching)controls();}
    };
    qualityButton.onclick=()=>{qualityMenu.hidden=!qualityMenu.hidden;qualityButton.setAttribute('aria-expanded',String(!qualityMenu.hidden));};
    qualityMenu.onclick=e=>{const b=e.target.closest('[data-quality]');if(!b||qualityButton.disabled)return;qualityMode=b.dataset.quality;closeQuality();qualityButton.focus();controls();if(qualityMode==='auto'){policy.reset();qualityState(qualityName(quality)+' · 自动调整');}else if(Number(qualityMode)!==quality)void changeQuality(Number(qualityMode));else qualityState(qualityName(quality)+' · 当前画质');};
    const sampleQuality=()=>{
      if(!session||!policy||!ready||switching||client.user.role<2)return;
      let buffered=0;for(let i=0;i<video.buffered.length;i++)if(video.buffered.start(i)<=video.currentTime+.05&&video.buffered.end(i)>video.currentTime)buffered=video.buffered.end(i)-video.currentTime;
      const metric=video.getVideoPlaybackQuality?.(),frames=metric?.totalVideoFrames||0,dropped=metric?.droppedVideoFrames||0;
      const delta=Math.max(0,frames-lastFrames),dropRatio=delta?Math.max(0,dropped-lastDropped)/delta:0;lastFrames=frames;lastDropped=dropped;
      const at=performance.now(),elapsed=(at-lastSampleAt)/1000,progressRatio=elapsed>0?Math.max(0,video.currentTime-lastTime)/elapsed:1;lastTime=video.currentTime;lastSampleAt=at;
      const next=policy.observe({quality,buffered,stalled:stalled&&progressRatio<.85,dropRatio,progressRatio,suspended:qualityMode!=='auto'||recording.busy||video.paused||document.hidden});stalled=false;
      if(next!==null)void changeQuality(next);
    };
    document.addEventListener('click',e=>{if(!qualityMenu.hidden&&!e.target.closest('.live-quality'))closeQuality();},{signal:uiSignal});
    player.addEventListener('keydown',e=>{if(e.key==='Escape'&&!qualityMenu.hidden){e.preventDefault();e.stopPropagation();closeQuality();qualityButton.focus();}else if((e.key===' '||e.key==='k')&&e.target===player){e.preventDefault();document.getElementById('liveToggle').click();}});
    document.getElementById('liveMute').onclick=()=>{const silent=video.muted||video.volume===0;if(silent&&video.volume===0)video.volume=1;video.muted=!silent;updateSound();};
    document.getElementById('liveVolume').oninput=e=>{video.volume=Number(e.target.value);video.muted=video.volume===0;updateSound();};
    const fullscreen=document.getElementById('liveFullscreen');
    let expanded=false;
    const expand=value=>{expanded=value;player.classList.toggle('is-expanded',value);player.closest('.live-dialog')?.classList.toggle('player-expanded',value);};
    const isFullscreen=()=>expanded||document.fullscreenElement===player||document.webkitFullscreenElement===player;
    const fullscreenState=()=>{if(!active())return;fullscreen.innerHTML=icon(isFullscreen()?'exit':'fullscreen');fullscreen.title=isFullscreen()?'退出全屏':'全屏';fullscreen.setAttribute('aria-label',fullscreen.title);};
    fullscreen.onclick=async()=>{try{if(expanded)expand(false);else if(isFullscreen())await (document.exitFullscreen||document.webkitExitFullscreen).call(document);else if(player.requestFullscreen||player.webkitRequestFullscreen)await (player.requestFullscreen||player.webkitRequestFullscreen).call(player);else expand(true);}catch{expand(true);}fullscreenState();};
    player.addEventListener('keydown',e=>{if(e.key==='Escape'&&expanded&&!e.defaultPrevented){e.preventDefault();e.stopPropagation();expand(false);fullscreenState();}});
    for(const event of ['fullscreenchange','webkitfullscreenchange'])document.addEventListener(event,fullscreenState,{signal:uiSignal});
    const elapsed=since=>{const s=Math.floor((performance.now()-since)/1000);return (s>=3600?Math.floor(s/3600)+':':'')+String(Math.floor(s/60)%60).padStart(2,'0')+':'+String(s%60).padStart(2,'0');};
    const clockTimer=setInterval(()=>{if(!active())return;if(session&&ready&&startedAt!==null)document.getElementById('liveElapsed').textContent=elapsed(startedAt);if(recordingStartedAt!==null)document.getElementById('liveRecordElapsed').textContent=elapsed(recordingStartedAt);},1000);
    mediaCleanup=async()=>{clearInterval(clockTimer);expand(false);try{if(isFullscreen())await (document.exitFullscreen||document.webkitExitFullscreen).call(document);}catch{}await stop();};
    document.getElementById('liveSnapshot').onclick=()=>{if(!video.videoWidth)return;const canvas=document.createElement('canvas');canvas.width=video.videoWidth;canvas.height=video.videoHeight;canvas.getContext('2d').drawImage(video,0,0);canvas.toBlob(blob=>{if(blob)save(blob,camera.serial+'-'+Date.now()+'.jpg');},'image/jpeg',.92);};
    document.getElementById('liveRecord').onclick=()=>{try{if(recording.busy)void recording.stop();else {recording.start();recordings.add(recording);}}catch(e){showError(e);}};
    if(document.getElementById('liveTalk'))document.getElementById('liveTalk').onclick=async()=>{
      if(stream){stopMic();return;}
      try{if(!globalThis.AudioEncoder||!globalThis.AudioData)throw Error('当前浏览器缺少 AAC 编码能力。');const config={codec:'mp4a.40.2',sampleRate:8000,numberOfChannels:1,bitrate:16000};if(!(await AudioEncoder.isConfigSupported(config)).supported)throw Error('当前浏览器不支持 8 kHz AAC 对讲编码。');
        stream=await navigator.mediaDevices.getUserMedia({audio:{channelCount:1,echoCancellation:true,noiseSuppression:true},video:false});if(!active()||!ctl||ctl.signal.aborted){stopMic();return;}
        audioContext=new AudioContext({sampleRate:8000});if(audioContext.sampleRate!==8000)throw Error('浏览器未提供 8 kHz 音频采样。');let timestamp=0;
        encoder=new AudioEncoder({error:e=>{showError(e);stopMic();},output:chunk=>{if(!ctl||ctl.signal.aborted||sending>=2)return;const raw=new Uint8Array(chunk.byteLength);chunk.copyTo(raw);const length=raw.length+7;if(length>512)return;const adts=new Uint8Array(length);adts.set([255,241,108,64|(length>>11),(length>>3)&255,((length&7)<<5)|31,252]);adts.set(raw,7);const aac=Array.from(adts,n=>n.toString(16).padStart(2,'0')).join('');sending++;const signal=ctl.signal;talkQueue=talkQueue.catch(()=>{}).then(()=>client.request(client.endpoint()+'/live/'+talkLease+'/talk',{aac},signal)).catch(e=>{if(e.name!=='AbortError'&&e.status!==429)showError(e);}).finally(()=>sending--);}});encoder.configure(config);
        const source=audioContext.createMediaStreamSource(stream);processor=audioContext.createScriptProcessor(1024,1,1);processor.onaudioprocess=e=>{if(!encoder||encoder.state!=='configured'||encoder.encodeQueueSize>2)return;const data=new Float32Array(e.inputBuffer.getChannelData(0));const frame=new AudioData({format:'f32-planar',sampleRate:8000,numberOfFrames:data.length,numberOfChannels:1,timestamp,data});timestamp+=data.length*1000000/8000;encoder.encode(frame);frame.close();};source.connect(processor);processor.connect(audioContext.destination);document.getElementById('liveTalk').textContent='关闭麦克风';
      }catch(e){stopMic();showError(e);}
    };
    const play=async (watching,expectedQuality=null)=>{
      if(session!==watching||watching.signal.aborted||!active())return;
      recording.reset();ready=false;lastFrames=lastDropped=0;stalled=false;ctl=new AbortController();const current=ctl;
      let playbackFailure=null,confirming=null;
      const failed=e=>{if(!current.signal.aborted){playbackFailure=e;current.abort();}};
      const timer=setTimeout(()=>failed(Error('等待直播画面超时，请重新开始观看。')),60000);
      const end=()=>current.abort();watching.signal.addEventListener('abort',end,{once:true});controls();status('正在请求摄像头并等待关键帧…');
      try{if(!globalThis.MediaSource)throw Error('当前浏览器不支持 MediaSource 实时播放。');const directory=await client.devices(current.signal),base=directory.devices.find(d=>d.id===client.stationId);if(!base?.online)throw Error('基站离线。');const talking=!!document.getElementById('liveTalkMode')?.checked;
        let mediaCaps={};if(client.request)mediaCaps=await Promise.resolve().then(()=>client.request('/media-capabilities',undefined,current.signal)).catch(()=>({}));
        const stationCaps=mediaCaps.continuous_live?await client.command('capabilities',[],{signal:current.signal}):{};continuous=!!stationCaps.continuous_quality_switch;
        const r=await client.raw(client.endpoint()+'/live',{slot:camera.slot,serial:camera.serial,boot:base.boot,talk:talking,format:continuous?'mse-v2':'mp4'},current.signal);if(!r.ok){const j=await r.json();throw Error(j.error||'实时连接失败');}talkLease=r.headers.get('X-Myvava-Lease');
        const reader=r.body.getReader();let ms=new MediaSource();
        // A new MSE stream must not inherit a previous decoder/error state,
        // particularly across resolution changes in WebKit.
        const replacement=video.cloneNode(false);replacement.removeAttribute('src');replacement.muted=video.muted;replacement.volume=video.volume;
        video.replaceWith(replacement);video=replacement;watchVideo();video.addEventListener('error',()=>failed(Error('播放器解码失败，请重新开始观看。')),{signal:current.signal});url=URL.createObjectURL(ms);video.src=url;await new Promise((resolve,reject)=>{ms.addEventListener('sourceopen',resolve,{once:true});current.signal.addEventListener('abort',()=>reject(current.signal.reason),{once:true});});
        let initial=new Uint8Array(),buffer=null;const waitUpdate=()=>new Promise((resolve,reject)=>{const cleanup=()=>{buffer.removeEventListener('updateend',done);buffer.removeEventListener('error',error);current.signal.removeEventListener('abort',abort);};const done=()=>{cleanup();resolve();},error=()=>{cleanup();reject(Error('浏览器媒体解码失败。'));},abort=()=>{cleanup();reject(current.signal.reason);};buffer.addEventListener('updateend',done,{once:true});buffer.addEventListener('error',error,{once:true});current.signal.addEventListener('abort',abort,{once:true});if(current.signal.aborted)abort();});
        // Start a new decoder once. Appending media must never undo a user's
        // stop/pause action or silently create a replacement viewing session.
        const append=async data=>{if(buffer.updating)await waitUpdate();if(video.currentTime>40&&buffer.buffered.length&&buffer.buffered.start(0)<video.currentTime-40){buffer.remove(0,video.currentTime-30);await waitUpdate();}buffer.appendBuffer(data);await waitUpdate();if(buffer.buffered.length){const end=buffer.buffered.end(buffer.buffered.length-1);if(end-video.currentTime>3)video.currentTime=Math.max(0,end-.8);}};
        let mime='',config=null,configSignal=null;
        const confirm=expected=>{configSignal?.abort();const confirmation=new AbortController();configSignal=confirmation;
          confirming=waitLivePicture({video,quality:expected,signal:AbortSignal.any([current.signal,confirmation.signal])}).then(actual=>{
            if(ctl!==current||!active()||confirmation.signal.aborted)return;clearTimeout(timer);quality=actual;ready=true;if(!qualityChanging)switching=false;lastTime=video.currentTime;lastSampleAt=performance.now();policy.reset();controls();status('正在观看');qualityState((qualityMode==='auto'?'自动 · ':'')+qualityName(actual)+' · '+video.videoWidth+' × '+video.videoHeight);
          }).catch(e=>{if(!confirmation.signal.aborted)failed(e);});};
        const resetDecoder=async()=>{
          internalReset=true;
          try{captureViewingFrame(client,camera,video);if(url)URL.revokeObjectURL(url);ms=new MediaSource();url=URL.createObjectURL(ms);video.src=url;
            await new Promise((resolve,reject)=>{const cleanup=()=>{ms.removeEventListener('sourceopen',opened);current.signal.removeEventListener('abort',aborted);};const opened=()=>{cleanup();resolve();},aborted=()=>{cleanup();reject(current.signal.reason);};ms.addEventListener('sourceopen',opened,{once:true});current.signal.addEventListener('abort',aborted,{once:true});if(current.signal.aborted)aborted();});buffer=null;
          }finally{internalReset=false;}
        };
        const records=continuous?createLiveRecordParser(async record=>{
          current.signal.throwIfAborted();
          if(record.kind===1){config=JSON.parse(new TextDecoder().decode(record.data));if(typeof config.mime!=='string'||!Number.isInteger(config.width)||!Number.isInteger(config.height)||config.width<1||config.height<1||config.width>4096||config.height>2160||!MediaSource.isTypeSupported(config.mime))throw Error('当前浏览器不支持目标视频编码。');}
          else if(record.kind===2){
            let reused=!!buffer;
            try{
              if(buffer){if(buffer.updating)await waitUpdate();if(config.mime!==mime){if(!buffer.changeType)throw Error('changeType unavailable');buffer.changeType(config.mime);}}
              if(!buffer)buffer=ms.addSourceBuffer(config.mime);await append(record.data);
            }catch(e){
              current.signal.throwIfAborted();if(!reused)throw e;
              // Keep the fetch, authenticated lease and camera viewer. Only
              // replace browser decoding state when in-place reconfiguration fails.
              await resetDecoder();buffer=ms.addSourceBuffer(config.mime);await append(record.data);
            }
            mime=config.mime;recording.push(record.data);confirm(pictureQuality({videoWidth:config.width,videoHeight:config.height}));
            current.signal.throwIfAborted();if(video.paused)void video.play().catch(failed);
          }else if(record.kind===3){recording.push(record.data);await append(record.data);}
          else if(record.kind===4)qualityState(new TextDecoder().decode(record.data));
        }):null;
        try{for(;;){const {done,value}=await reader.read();if(done){records?.finish();break;}if(current.signal.aborted)break;if(records){await records.push(value);continue;}recording.push(value);
            if(!buffer){const next=new Uint8Array(initial.length+value.length);next.set(initial);next.set(value,initial.length);initial=next;if(initial.length>1024*1024)throw Error('媒体初始化头超过限制。');let pos=0,moov=false;while(pos+8<=initial.length){const n=new DataView(initial.buffer).getUint32(pos),type=String.fromCharCode(...initial.slice(pos+4,pos+8));if(n<8)throw Error('媒体容器无效。');if(pos+n>initial.length)break;if(type==='moov')moov=true;pos+=n;}if(!moov)continue;
                let avc='',audio=false;for(let i=4;i+8<initial.length;i++){const tag=String.fromCharCode(...initial.slice(i,i+4));if(tag==='avcC')avc=Array.from(initial.slice(i+5,i+8),n=>n.toString(16).padStart(2,'0')).join('');if(tag==='mp4a')audio=true;}const mime=`video/mp4; codecs="avc1.${avc}${audio?',mp4a.40.2':''}"`;if(!avc||!MediaSource.isTypeSupported(mime))throw Error('当前浏览器不支持摄像头的视频编码。');buffer=ms.addSourceBuffer(mime);await append(initial);initial=new Uint8Array();
                current.signal.throwIfAborted();void video.play().catch(failed);
                confirming=waitLivePicture({video,quality:expectedQuality,signal:current.signal}).then(actual=>{
                  if(ctl!==current||!active())return;clearTimeout(timer);quality=actual;if(qualityMode!=='auto')qualityMode=actual===null?'auto':String(actual);ready=true;if(!continuous||!qualityChanging)switching=false;lastTime=video.currentTime;lastSampleAt=performance.now();policy.reset();controls();
                  status('正在观看');
                  qualityState((qualityMode==='auto'?'自动 · ':'')+qualityName(actual)+' · '+video.videoWidth+' × '+video.videoHeight);
                }).catch(failed);
            }else await append(value);
        }}finally{await reader.cancel().catch(()=>{});}
        if(ms.readyState==='open'&&!buffer?.updating)ms.endOfStream();if(ctl===current)status('观看已结束，可重新开始。');
      }catch(e){if(ctl===current&&(playbackFailure||(!current.signal.aborted&&e.name!=='AbortError'))){status((playbackFailure||e).message);qualityState('连接已关闭，点击画面重试');}}finally{clearTimeout(timer);watching.signal.removeEventListener('abort',end);if(ctl===current)await stop();current.abort();await confirming;}

    };
    const start=async()=>{
      if(session||starting||stopping||!active())return;starting=true;controls();await stop();if(!active())return;notice('');
      const watching=new AbortController();session=watching;quality=null;policy=createAdaptiveQuality();
      startedAt=performance.now();document.getElementById('liveElapsed').textContent='00:00';starting=false;controls();qualityState('正在连接摄像头…');
      autoTimer=setInterval(sampleQuality,4000);void play(watching);
    };
    document.getElementById('liveToggle').onclick=()=>{if(session)userStop();else void start();};
    document.getElementById('liveRetry').onclick=()=>{void start();};
    controls();updateSound();
    if(autoStart)await start();
  } else if(page==='recordings') {
    const ctl=new AbortController();mediaCleanup=()=>ctl.abort();
    content.innerHTML='<section class="card" role="status">正在读取 TF 卡视频目录…</section>';
    const archives=await loadRecordingCameras(client,camera,location.hash,{signal:ctl.signal});
    camera=archives.camera;const owners=archives.owners;
    if(!active())return true;
    content.innerHTML=`<section class="card"><div class="section-heading"><div><h2>事件短片</h2><p class="hint">来源：当前基站 TF / USB 存储，与云存储独立。</p></div><a href="#storage-policy">留存设置</a></div><div class="clip-filters"><label>摄像头<select id="tfCamera"><option value=""${!camera?' selected':''}>全部摄像头</option>${owners.map(o=>`<option value="${esc(o.serial)}/${o.slot}"${o.serial===camera?.serial&&o.slot===camera?.slot?' selected':''}>${esc(clipCameraName(o,cameras))} · ${o.records} 段</option>`).join('')}</select></label><label>开始时间<input id="tfFrom" type="datetime-local"></label><label>结束时间<input id="tfTo" type="datetime-local"></label><button id="tfRefresh">查询 / 刷新</button></div><p id="tfRangeHint" class="hint">按观看设备的时区查询。已删除摄像头的历史片段仍保留；封面来自已有录像。</p><div id="tfRows" class="clip-list"></div><div class="row pagination"><button id="tfPrev">上一页</button><span id="tfPage"></span><button id="tfNext">下一页</button></div></section><details class="card" id="tfTransfer"><summary>视频调取任务</summary><p class="hint">播放或下载时调取完整视频。封面只传输首个关键帧，与云存储开关独立。</p><div id="tfJobs"></div></details>`;
    let rows=[],jobs=[],running=false;const pager=createRecordingPager(client,camera,{capabilities:archives.capabilities,signal:ctl.signal}),thumbs=createThumbnailLoader(client,{signal:ctl.signal,active});
    mediaCleanup=()=>{ctl.abort();thumbs.cleanup();};
    const command=(owner,name,args=[])=>client.command(name,[owner.slot,...args],{cameraSerial:owner.serial,signal:ctl.signal}).then(j=>{if(!j.ok)throw storageError(j.error);return j;});
    const names={queued:'等待传输',uploading:'正在调取',committed:'已完成',retry_wait:'等待重试',failed:'失败',destination_changed:'服务器已变更',cancelled:'已取消',source_expired:'源已失效或任务到期'};
    const drawJobs=()=>{document.getElementById('tfJobs').innerHTML=jobs.map(j=>`<article class="info"><strong>${esc(clipCameraName(j,cameras))} · ${esc(names[j.state]||j.state)}</strong><p>${(j.offset/1048576).toFixed(2)} / ${(j.bytes/1048576).toFixed(2)} MiB${j.error?' · '+esc(j.error):''}</p><div class="row">${j.state==='committed'?`<a href="#tf-clip/${esc(j.serial)}/${j.slot}/${esc(j.source_id)}">查看 TF 视频</a>`:''}${client.user.role>1?`${['queued','uploading','retry_wait'].includes(j.state)?`<button data-tf-job="export-cancel" data-id="${esc(j.id)}">取消</button>`:''}${['failed','cancelled','source_expired','retry_wait'].includes(j.state)?`<button data-tf-job="export-retry" data-id="${esc(j.id)}">重试</button>`:''}${['committed','failed','cancelled','source_expired'].includes(j.state)?`<button data-tf-job="export-forget" data-id="${esc(j.id)}">清除任务记录</button>`:''}`:''}</div></article>`).join('')||'<p class="empty">当前所选摄像头暂无调取任务。</p>';};
    let loadingJobs=false;
    const loadJobs=async()=>{if(loadingJobs)return;loadingJobs=true;try{const identities=camera?[camera]:[...new Map(rows.map(r=>[r.serial+'/'+r.slot,r])).values()],result=[];for(const owner of identities){const j=await command(owner,'export-list');result.push(...j.exports.map(r=>({...r,serial:owner.serial,slot:owner.slot})));}if(active()){jobs=result;drawJobs();}}finally{loadingJobs=false;}};
    document.getElementById('tfTransfer').ontoggle=()=>{if(document.getElementById('tfTransfer').open)loadJobs().catch(showError);};
    const load=async(action='refresh')=>{
      for(const id of ['tfRefresh','tfPrev','tfNext'])document.getElementById(id).disabled=true;
      try{
        const range=action==='refresh'?recordingRange(document.getElementById('tfFrom').value,document.getElementById('tfTo').value):undefined;
        const j=await pager.load(action,range);if(!active())return;rows=j.records;let day='';
        document.getElementById('tfRows').innerHTML=rows.map((r,i)=>{const date=r.clock_valid?new Date(r.unix_seconds*1000).toLocaleDateString():'时间未确认',heading=date!==day?`<h3 class="clip-date">${esc(date)}</h3>`:'';day=date;return heading+`<article class="clip-row"><button class="clip-preview" data-tf="playback" data-id="${esc(r.id)}" data-preview-index="${i}" aria-label="播放 ${esc(clipCameraName(r,cameras))} 的事件短片">录像封面</button><div class="clip-body"><div class="clip-heading"><strong>${esc(clipCameraName(r,cameras))}</strong><time>${r.clock_valid?esc(new Date(r.unix_seconds*1000).toLocaleTimeString()):'时间未确认'}</time></div>${detectionBadge(r)}<p class="clip-meta">${Number.isFinite(r.duration_ms)?(r.duration_ms/1000).toFixed(1)+' 秒 · ':''}${(r.bytes/1048576).toFixed(2)} MiB<span class="clip-source">TF 卡${r.encrypted?' · 已加密':''}</span></p><div class="row clip-actions"><button data-tf="playback" data-id="${esc(r.id)}">播放</button><button data-tf="download" data-id="${esc(r.id)}">下载</button>${client.user.role>1?`<button class="danger" data-tf="delete" data-id="${esc(r.id)}">删除</button>`:''}<details class="clip-detail"><summary>详情</summary><p>摄像头：${esc(r.serial)}<br>片段：${esc(r.id)}<br>${esc(r.end_reason)}</p>${r.detection_reported?`<p>检测报告：${r.detection_result_raw} / ${r.detection_type_raw}</p>`:''}</details></div></div></article>`;}).join('')||'<p class="empty">当前查询没有录像。</p>';
        thumbs.attach(document.getElementById('tfRows'),rows,'tf');
        document.getElementById('tfPage').textContent=`第 ${j.page+1} 页 · 共 ${j.total} 段`;
        document.getElementById('tfPrev').disabled=j.page===0;document.getElementById('tfNext').disabled=!j.has_more;
        for(const id of ['tfFrom','tfTo'])document.getElementById(id).disabled=!j.supports_query;
        if(!j.supports_query)document.getElementById('tfRangeHint').textContent='当前基站使用旧目录接口，更新程序后可按时间范围查询。';
        if(document.getElementById('tfTransfer').open)await loadJobs();
      }finally{if(active())document.getElementById('tfRefresh').disabled=false;}
    };
    document.getElementById('tfCamera').onchange=e=>{location.hash='recordings'+(e.target.value?'/'+e.target.value:'');};
    document.getElementById('tfRefresh').onclick=()=>load().catch(showError);
    document.getElementById('tfPrev').onclick=()=>load('previous').catch(showError);
    document.getElementById('tfNext').onclick=()=>load('next').catch(showError);
    document.getElementById('tfRows').onclick=async e=>{const b=e.target.closest('[data-tf]');if(!b||running)return;const row=rows.find(r=>r.id===b.dataset.id);if(!row)return;running=true;try{
        if(b.dataset.tf==='delete'){if(confirm('永久删除 TF 上这段录像？云存储和 NAS 中的副本不受影响。')){await command(row,'record-delete',[row.id]);await load();}return;}
        location.hash=`tf-clip/${row.serial}/${row.slot}/${row.id}/${b.dataset.tf}`;
    }catch(err){if(err.name!=='AbortError')showError(err);}finally{running=false;}};
    document.getElementById('tfJobs').onclick=async e=>{const b=e.target.closest('[data-tf-job]');if(!b)return;const job=jobs.find(j=>j.id===b.dataset.id);if(!job)return;try{await command(job,b.dataset.tfJob,[job.id]);await loadJobs();}catch(err){showError(err);}};
    await load();
  } else if(page==='event-clip'||page==='tf-clip'||page==='tf-cache') {
    const [,serial,slot,id,action]=location.hash.split('/');const ctl=new AbortController();
    content.innerHTML='<section class="card"><h2>TF 卡视频回放</h2><p id="eventClipProgress">正在检查 TF 视频调取状态…</p><p class="hint">首次播放需从基站调取，缓存有效时直接播放。临时缓存与云存储开关和云端视频列表无关。</p><a href="#recordings">返回 TF 卡视频</a></section>'+playerHTML;
    const player=archivePlayer(client,{active,storage:'tf_export',serial:page==='tf-cache'?null:serial});mediaCleanup=()=>{ctl.abort();player.cleanup();};
    try{const target=page==='tf-cache'?serial:await resolveClip(client,{serial,slot:Number(slot),id},{signal:ctl.signal,progress:j=>{if(active())document.getElementById('eventClipProgress').textContent=`${j.state==='retry_wait'?'等待重试':j.state==='queued'?'等待调取':'基站 → 管理服务器'} · ${(j.offset/1048576).toFixed(2)} / ${(j.bytes/1048576).toFixed(2)} MiB`;}});if(active()){document.getElementById('eventClipProgress').textContent='来源：基站 TF 卡。';await player.open(target,action==='download'?'download':'playback');}}catch(e){if(active()&&e.name!=='AbortError')document.getElementById('eventClipProgress').textContent=e.message;}
  } else if(page==='storage-policy') {
    const ctl=new AbortController();mediaCleanup=()=>ctl.abort();let revision=0;
    content.innerHTML='<section class="card"><h2>TF 循环留存</h2><form id="retentionForm"><label><input name="enabled" type="checkbox">启用自动循环清理</label><label>最多保留天数（0 为不按天数清理）<input name="days" type="number" min="0" max="3650" required></label><label>未完成同步的保护时长（小时）<input name="pending" type="number" min="1" max="720" required></label><button>保存策略</button></form><p class="hint">达到配额 90% 后从最早短片开始清理，降至 80%。按天数清理需要可信时钟。正在读写或刚申请调取的原片会受保护；同步超出保护时间后，可能被清理并记为源已过期。</p><button id="retentionRefresh">刷新</button><pre id="retentionStatus"></pre></section>';
    const form=document.getElementById('retentionForm'),load=async()=>{const j=await client.command('storage-policy',[],{signal:ctl.signal});if(!j.ok)throw Error(j.error);if(!active())return;revision=j.revision;form.enabled.checked=j.enabled;form.days.value=j.max_days;form.pending.value=j.pending_hours;document.getElementById('retentionStatus').textContent=`已自动清理 ${j.removed_since_boot} 段 · ${(j.removed_bytes_since_boot/1048576).toFixed(2)} MiB\n${j.error||'策略正常'} · ${j.clock_valid?'时钟已确认':'时钟未确认：不按日龄清理'}`;};
    form.onsubmit=async e=>{e.preventDefault();try{const j=await client.command('storage-policy-set',[revision,form.enabled.checked?1:0,form.days.value,form.pending.value],{signal:ctl.signal});if(!j.ok)throw Error(j.error);await load();notice('留存策略已保存。');}catch(e){showError(e);}};
    document.getElementById('retentionRefresh').onclick=()=>load().catch(showError);if(client.user.role!==3)form.querySelectorAll('input,button').forEach(e=>e.disabled=true);await load();
  } else if(page==='archive-policy') {
    content.innerHTML='<section class="card"><h2>云存储留存</h2><p class="hint">全局策略由管理员配置。启用后按天数与容量清理云存储短片；TF 回放缓存和历史导入文件不参与此策略。正在读取和未完成上传的文件受保护。默认关闭自动清理。</p><form id="archivePolicy"><label><input name="enabled" type="checkbox">启用云存储自动清理</label><label>最长保留天数（0 表示仅按容量）<input name="days" type="number" min="0" max="3650" required></label><button>保存策略</button></form><pre id="archivePolicyStatus"></pre></section>';
    if(client.user.role!==3){content.textContent='服务器留存策略需要管理员权限。';return;}
    let revision=0;const form=document.getElementById('archivePolicy'),load=async()=>{const j=await client.request('/archive-policy');if(!active())return;revision=j.revision;form.enabled.checked=j.enabled;form.days.value=j.max_days;document.getElementById('archivePolicyStatus').textContent=`${j.healthy?'策略正常':'策略或清理失败，自动清理已停用'} · 已清理 ${j.removed_since_boot} 段\n共享归档空间（含回放缓存和历史导入） ${(j.reserved_bytes/1073741824).toFixed(2)} / ${(j.quota_bytes/1073741824).toFixed(2)} GiB · 文件上限 ${j.file_limit}`;};
    form.onsubmit=async e=>{e.preventDefault();try{await client.request('/archive-policy',{revision,enabled:form.enabled.checked,max_days:Number(form.days.value)});await load();notice('服务器留存策略已保存。');}catch(err){showError(err);}};await load();
  } else if(page==='server-recordings') {
    const id=location.hash.split('/')[1];if(!id){location.hash='cloud-storage';return true;}
    const meta=await client.request(client.endpoint()+'/files/'+id+'/info');if(active())location.hash=(meta.storage==='tf_export'?'tf-cache':meta.storage==='legacy_import'?'legacy-recordings':'cloud-storage')+'/'+id;
  } else if(page==='cloud-storage'||page==='legacy-recordings') {
    const imported=page==='legacy-recordings',storage=imported?'legacy_import':'cloud',label=imported?'原厂历史导入':'云存储视频';
    content.innerHTML=`<section class="card"><h2>${label}</h2><p class="hint">${imported?'使用服务器离线导入工具接入的历史文件，单独保留。':'开启云存储后，基站会把新产生的事件短片上传到自己的服务器。这里仅显示云上传内容；基站离线时仍可查看已上传短片。'}</p><p><a href="#cloud-settings">云存储设置与上传任务</a> · <a href="#archive-policy">云存储留存</a> · <a href="#${imported?'cloud-storage':'legacy-recordings'}">${imported?'返回云存储':'原厂历史文件导入'}</a></p><div class="row"><label>摄像头序列号<input id="archiveFilter" placeholder="留空表示全部" maxlength="31" value=""></label><button id="archiveRefresh">查询</button></div><div id="archiveRows"></div><div class="row"><button id="archivePrev">上一页</button><span id="archivePage"></span><button id="archiveNext">下一页</button></div></section>`+playerHTML;
    let pageNumber=0,listVersion=0,rows=[];const ctl=new AbortController(),player=archivePlayer(client,{active,storage}),thumbs=createThumbnailLoader(client,{signal:ctl.signal,active});
    mediaCleanup=()=>{ctl.abort();player.cleanup();thumbs.cleanup();};
    const draw=()=>{document.getElementById('archiveRows').className='clip-list';document.getElementById('archiveRows').innerHTML=rows.map((r,i)=>`<article class="clip-row"><button class="clip-preview" data-archive="playback" data-id="${esc(r.id)}" ${r.state==='committed'?`data-preview-index="${i}"`:'disabled'} aria-label="播放 ${esc(clipCameraName(r,cameras))} 的短片">${r.state==='committed'?'录像封面':'正在上传…'}</button><div class="clip-body"><div class="clip-heading"><strong>${esc(clipCameraName(r,cameras))}</strong><time>${r.clock_valid?esc(new Date(r.unix_seconds*1000).toLocaleString()):'基站时钟未确认'}</time></div>${detectionBadge(r)}<p class="clip-meta">${Number.isFinite(r.duration_ms)?(r.duration_ms/1000).toFixed(1)+' 秒 · ':''}${(r.bytes/1048576).toFixed(2)} MiB<span class="clip-source">${label}</span></p><div class="row clip-actions"><button data-archive="playback" data-id="${esc(r.id)}" ${r.state!=='committed'?'disabled':''}>播放</button><button data-archive="download" data-id="${esc(r.id)}" ${r.state!=='committed'?'disabled':''}>下载视频</button>${client.user.role>1?`<button class="danger" data-archive="delete" data-id="${esc(r.id)}">删除</button>`:''}<details class="clip-detail"><summary>详情</summary><p>${esc(r.serial)}<br>#${esc(r.id)}</p></details></div></div></article>`).join('')||'<p class="empty">没有匹配的视频。</p>';thumbs.attach(document.getElementById('archiveRows'),rows,storage);};
    const load=async()=>{const version=++listVersion,params=new URLSearchParams({page:pageNumber,storage});const serial=document.getElementById('archiveFilter').value.trim();if(serial)params.set('serial',serial);const j=await client.request(client.endpoint()+'/files?'+params,undefined,ctl.signal);if(!active()||version!==listVersion)return;rows=j.files;draw();document.getElementById('archivePage').textContent=`第 ${pageNumber+1} 页 · 共 ${j.total} 段`;document.getElementById('archivePrev').disabled=pageNumber===0;document.getElementById('archiveNext').disabled=(pageNumber+1)*16>=j.total;};
    document.getElementById('archiveRefresh').onclick=()=>{pageNumber=0;load().catch(showError);};document.getElementById('archivePrev').onclick=()=>{pageNumber--;load().catch(showError);};document.getElementById('archiveNext').onclick=()=>{pageNumber++;load().catch(showError);};
    document.getElementById('archiveRows').onclick=async e=>{const b=e.target.closest('[data-archive]');if(!b||b.disabled)return;try{if(b.dataset.archive==='delete'){if(confirm('永久删除这段'+label+'？TF 卡与 NAS 中的文件不受影响。')){await client.request(client.endpoint()+'/files/'+b.dataset.id+'/delete',{});await load();}}else await player.open(b.dataset.id,b.dataset.archive);}catch(err){if(err.name!=='AbortError')showError(err);}};
    await load();const requested=location.hash.split('/')[1];if(active()&&/^[a-f0-9]{16}$/.test(requested||''))await player.open(requested,location.hash.split('/')[2]==='download'?'download':'playback');
  } else if(page==='events') {
    content.innerHTML='<section class="card"><h2>事件中心</h2><div class="row"><label>本页搜索<input id="eventFilter" placeholder="搜索详情"></label><label>类别<select id="eventCategory"><option value="">全部</option><option value="detection">检测报告</option><option value="recording">录像</option><option value="pairing">配对</option><option value="storage">存储日志</option><option value="system">系统</option></select></label><label>摄像头序列号<input id="eventSerial" maxlength="31" placeholder="留空表示全部"></label><button id="eventReload">查询</button><button id="eventRead">本页标为已读</button></div><div id="eventRows"></div><button id="eventOlder">更早事件</button></section>';
    let before='',rows=[],latest='0000000000000000',cursor='0000000000000000';
    const draw=()=>{const filter=document.getElementById('eventFilter').value.toLowerCase();document.getElementById('eventRows').innerHTML=rows.filter(e=>JSON.stringify(e).toLowerCase().includes(filter)).map(e=>`<article class="info"><strong>${e.id>cursor?'● 未读 · ':''}${esc(eventLabel(e.event.kind,e.event))}</strong> · ${esc(new Date(e.received_at*1000).toLocaleString())}<p>${esc(e.event.serial||'基站')} · #${esc(e.id)}</p><p>${esc(clipLabel(e.clip_state_reported))}</p>${e.event.related_id&&e.event.related_id!=='0000000000000000'?`<div class="row"><a href="#tf-clip/${esc(e.event.serial)}/${e.event.slot}/${esc(e.event.related_id)}">查看 TF 卡视频</a><a href="#cloud-storage/${esc(e.event.related_id)}">查看云存储视频</a></div>`:''}<details><summary>事件详情</summary><pre>${esc(JSON.stringify(e,null,2))}</pre></details></article>`).join('')||'<p class="empty">没有匹配事件。</p>';};
    const load=async(reset=false)=>{if(reset){before='';rows=[];}const params=new URLSearchParams();if(before)params.set('before',before);const category=document.getElementById('eventCategory').value,serial=document.getElementById('eventSerial').value.trim();if(category)params.set('category',category);if(serial)params.set('serial',serial);const j=await client.request(client.endpoint()+'/events'+(params.size?'?'+params:''));if(!active())return;rows=j.events;before=j.next_before;latest=rows[0]?.id||'0000000000000000';cursor=j.read_through;draw();document.getElementById('eventOlder').disabled=!rows.length;};
    document.getElementById('eventCategory').onchange=()=>load(true).catch(showError);document.getElementById('eventFilter').oninput=draw;document.getElementById('eventReload').onclick=()=>load(true).catch(showError);document.getElementById('eventOlder').onclick=()=>load().catch(showError);
    document.getElementById('eventRead').onclick=async()=>{try{await client.request(client.endpoint()+'/events/read',{through:latest});if(active()){cursor=latest;draw();}}catch(e){showError(e);}};
    await load(true);
  } else if(page==='tasks') {
    content.innerHTML='<section class="card"><h2>操作记录</h2><p class="hint">结果未知的操作不会自动重试。请先检查摄像头或基站状态，再决定是否重新操作。</p><div id="pendingTask"></div><div id="taskRows"></div><button id="taskOlder">更早任务</button></section>';
    const pending=client.getPending();if(pending){document.getElementById('pendingTask').innerHTML='<div class="info warning">有待核对操作。<button id="taskRecover">查询结果</button><button id="taskClear">已核对，解除挂起</button><pre id="pendingJson"></pre></div>';document.getElementById('pendingJson').textContent=JSON.stringify(pending,null,2);document.getElementById('taskRecover').onclick=async()=>{try{const t=await client.recover(AbortSignal.timeout(35000));if(active())document.getElementById('pendingJson').textContent=JSON.stringify(t,null,2);}catch(e){showError(e);}};document.getElementById('taskClear').onclick=()=>{if(confirm('仅清除网页中的挂起标记，不取消设备操作。确认已核对实际状态？')){client.clearPending();document.getElementById('pendingTask').textContent='挂起标记已解除。';}};}
    let before='';const load=async()=>{const j=await client.request(client.endpoint()+'/history'+(before?'?before='+before:''));if(!active())return;before=j.next_before;document.getElementById('taskRows').innerHTML=j.tasks.map(t=>`<details><summary>${esc(t.state)} · ${esc(t.command)} · #${esc(t.id)}</summary><button data-result="${esc(t.epoch+'/'+t.id)}">读取结果</button><pre>${esc(JSON.stringify(t,null,2))}</pre></details>`).join('')||'<p>暂无任务。</p>';document.getElementById('taskOlder').disabled=!j.tasks.length;};
    document.getElementById('taskOlder').onclick=()=>load().catch(showError);document.getElementById('taskRows').onclick=async e=>{if(!e.target.dataset.result)return;try{const t=await client.request(client.endpoint()+'/tasks/'+e.target.dataset.result);if(active())e.target.nextElementSibling.textContent=JSON.stringify(t,null,2);}catch(err){showError(err);}};await load();
  } else if(page==='account') {
    content.innerHTML=`<section class="card"><h2>${esc(client.user.name)}</h2><p>权限：${esc({1:'只读',2:'操作员',3:'管理员'}[client.user.role])}</p><form id="passwordForm"><label>当前密码<input name="old" type="password" autocomplete="current-password" required maxlength="128"></label><label>新密码<input name="password" type="password" autocomplete="new-password" required minlength="12" maxlength="128"></label><p class="hint">12–128 个 ASCII 字符。修改后所有会话失效。</p><button>修改密码</button></form></section>${client.user.role===3?'<section class="card"><h2>用户与授权</h2><div id="userList"></div><form id="userForm"><label>用户名<input name="username" required pattern="[A-Za-z0-9_-]{1,63}"></label><label>角色<select name="role"><option value="viewer">只读</option><option value="operator">操作员</option><option value="admin">管理员</option></select></label><label>授权基站<input name="devices" placeholder="BASE_A,BASE_B 或 *" required></label><label>设置密码<input name="password" type="password" autocomplete="new-password" minlength="12" maxlength="128"></label><label><input name="disabled" type="checkbox">停用账户</label><button>创建 / 更新用户</button><p class="hint">已有用户密码留空则保留；更新会撤销该用户的所有会话。管理员需要授权 *。</p></form></section>':''}`;
    document.getElementById('passwordForm').onsubmit=async e=>{e.preventDefault();try{await client.request('/password',{old_password:e.target.old.value,new_password:e.target.password.value});sessionStorage.removeItem('vavaConnection');location.reload();}catch(err){showError(err);}};
    if(client.user.role===3){const list=async()=>{const j=await client.request('/users');if(active())document.getElementById('userList').textContent=j.users.map(u=>u.name+' · '+{1:'只读',2:'操作员',3:'管理员'}[u.role]+' · '+u.devices+(u.disabled?' · 停用':'')).join('\n');};await list();if(!active())return;document.getElementById('userForm').onsubmit=async e=>{e.preventDefault();const f=e.target;try{await client.request('/users',{username:f.username.value,role:f.role.value,devices:f.devices.value,password:f.password.value,disabled:f.disabled.checked});f.password.value='';await list();notice('用户已保存，其现有会话已撤销。');}catch(err){showError(err);}};}
  } else if(page==='nas-settings') {
    content.innerHTML=`<section class="card"><h2>NAS 连接 · ${esc(client.stationId)}</h2><p class="hint">NAS 地址与挂载设置保存在当前基站，仅对 ${esc(client.stationId)} 生效。<a href="#station/nas-backup">管理此基站摄像头的 NAS 备份</a>。更换服务器或导出路径前，请先停用并等待卸载完成。</p><form id="nasConfig"><label>服务器域名或 IP<input name="host" required maxlength="180" pattern="[A-Za-z0-9:.\\-]+" autocomplete="off"></label><label>NFS 导出路径<input name="exportPath" required maxlength="240" placeholder="/volume1/cameras" autocomplete="off"></label><label><input name="enabled" type="checkbox">启用 NFS 连接</label><button>保存连接配置</button></form><div class="row"><button id="nasProbe">检查 / 重试连接</button><button id="nasUnmount">停用并卸载</button><button id="nasReload">重新读取配置</button></div><p class="hint">保存成功表示配置已写入；只有“可用”状态代表挂载及读写检查成功。现有历史任务始终绑定原目的地。</p></section><section class="card"><h2>连接状态</h2><div id="nasStatus">正在查询…</div></section>`;
    const ctl=new AbortController();mediaCleanup=()=>ctl.abort();let revision=0,busy=false,loaded=false;
    const form=document.getElementById('nasConfig');
    const names={queued:'等待处理',mounting:'正在挂载并检查',unmounting:'正在卸载',ready:'可用',disabled:'已停用 / 已卸载',retry_wait:'失败，等待重试'};
    const command=async(name,args=[])=>{const j=await client.command(name,args,{signal:ctl.signal});if(!j.ok)throw Error(j.error||'NFS 操作失败');return j;};
    const draw=(j,fill=false)=>{if(!active())return;
      if(fill||!loaded){form.host.value=j.host;form.exportPath.value=j.export;form.enabled.checked=j.enabled;revision=j.revision;loaded=true;}
      document.getElementById('nasStatus').innerHTML=`<dl class="kv"><dt>连接</dt><dd>${esc(names[j.phase]||j.phase)}</dd><dt>期望 / 已应用版本</dt><dd>${j.revision} / ${j.applied_revision}</dd><dt>可用 / 总容量</dt><dd>${j.ready?`${(j.available_bytes/1073741824).toFixed(2)} / ${(j.total_bytes/1073741824).toFixed(2)} GiB`:'暂不可用'}</dd><dt>正在备份</dt><dd>${j.active_copies} 段</dd><dt>归档格式</dt><dd>TS + MP4 · H.264 / AAC</dd><dt>最近检查</dt><dd>${j.checked_unix_seconds?esc(new Date(j.checked_unix_seconds*1000).toLocaleString()):'尚未检查'}</dd>${j.error?`<dt>失败原因</dt><dd>${esc(j.error)}</dd>`:''}${revision!==j.revision?'<dt>配置变化</dt><dd>配置已由其他操作更新，请重新读取后保存。</dd>':''}</dl>`;
    };
    const load=async fill=>{if(busy)return;busy=true;try{draw(await command('nas-status'),fill);}finally{busy=false;}};
    form.onsubmit=async e=>{e.preventDefault();if(busy)return;busy=true;try{draw(await command('nas-config',[revision,form.enabled.checked?1:0,form.host.value.trim(),form.exportPath.value.trim()]),true);notice('连接配置已保存，正在应用。');}catch(err){showError(err);}finally{busy=false;}};
    const action=async action=>{if(busy)return;busy=true;try{draw(await command('nas-control',[revision,action]),true);notice(action==='unmount'?'已请求停用并卸载。':'已请求重新检查。');}catch(err){showError(err);}finally{busy=false;}};
    document.getElementById('nasProbe').onclick=()=>action('probe');document.getElementById('nasUnmount').onclick=()=>action('unmount');document.getElementById('nasReload').onclick=()=>load(true).catch(showError);
    if(client.user.role<2){form.querySelectorAll('input,button').forEach(e=>e.disabled=true);document.getElementById('nasProbe').disabled=document.getElementById('nasUnmount').disabled=true;}
    await load(true);if(!active())return true;(async()=>{try{while(active()&&!ctl.signal.aborted){await delay(2500,ctl.signal);if(active())await load(false);}}catch(e){if(e.name!=='AbortError')showError(e);}})();
  } else if(['storage-services','cloud-settings','nas-policy'].includes(page)) {
    if(!camera){content.innerHTML='<section class="card">先选择已配对摄像头。</section>';return;}
    const nas=page==='nas-policy',kind=nas?'nas':'cloud',channel=nas?'nas-list':'upload-list',p=camera.policy;
    content.innerHTML=`<section class="card"><h2>${nas?'NAS 备份 · '+esc(client.stationId):'云存储上传'} · ${esc(camera.serial)}</h2><p><a href="#${nas?'station/nas':'cloud-storage'}">${nas?'返回此基站 NAS 连接与容量':'查看云存储视频'}</a></p><form id="cloudPolicy"><label><input name="enabled" type="checkbox" ${(nas?p.nas_backup:p.cloud_upload)?'checked':''}>${nas?'将此摄像头事件短片备份到基站挂载的 NAS':'将此摄像头事件短片上传到云存储服务器'}</label><p class="hint">仅影响事件短片。与${nas?'云存储':' NAS 备份'}开关独立；不追溯关闭期间产生的短片。停用不删除历史文件。本机实时录像不受此设置影响。</p><button>${nas?'保存 NAS 备份设置':'保存云存储设置'}</button><button type="button" id="storagePolicyReload">读取最新设置</button><p id="storagePolicyStatus" class="hint"></p></form></section>${nas?'':'<section class="card"><h2>后台云上传带宽</h2><form id="uploadRate"><label><span id="uploadRateLabel">云上传限速（KiB/s）</span><input name="rate" type="number" min="1" max="4096" value="128" required></label><p class="hint" id="uploadRateHint">仅限制后台云存储上传。TF 按需回放与下载不受此限速约束；控制、心跳与实时观看独立处理。</p><button>保存限速</button></form></section>'}<section class="card"><h2>${nas?'NAS 备份任务':'云上传任务'}</h2><p class="hint">任务绑定创建时的目的地；更换目的地后不会自动转发旧任务。只有 TF 原片已不在且任务结束后才能清理历史；清理任务不删除已上传或备份的视频。</p><button id="storageTasksReload">刷新</button><div id="storageTasks"></div></section>`;
    const pages={[channel]:0};let loading=false,saving=false;
    const stateName={queued:'等待处理',uploading:'上传中',copying:'备份中',committed:'已完成',retry_wait:'等待重试',paused:'已暂停',failed:'失败',destination_changed:'目的地已变更',cancelled:'已取消',source_expired:'源文件已过期'};
    const load=async()=>{if(loading)return;loading=true;try{const results=[];for(const cmd of [channel]){try{results.push({channel:cmd,...await api(`if-serial ${camera.serial} ${cmd} ${camera.slot} ${pages[cmd]}`)});}catch(e){results.push({channel:cmd,error:e.message});}}if(!active())return;
      document.getElementById('storageTasks').innerHTML=results.map(j=>`<section><h3>${j.channel==='upload-list'?'云存储上传':'NAS 备份'}</h3>${j.error?`<p>${esc(j.error)}</p>`:(j.uploads||j.records||[]).map(r=>`<article class="info"><strong>${esc(stateName[r.state]||r.state)}</strong> · ${esc(r.id)}<p>${r.bytes!==undefined?`${((r.offset||0)/1048576).toFixed(2)} / ${(r.bytes/1048576).toFixed(2)} MiB · `:''}${esc(r.error||'')}</p>${client.user.role>1?`<div class="row">${['queued','uploading','copying','retry_wait','paused','destination_changed'].includes(r.state)?`<button data-storage-action="cancel" data-channel="${j.channel}" data-id="${esc(r.id)}">取消</button>`:''}${['failed','cancelled','retry_wait','source_expired'].includes(r.state)?`<button data-storage-action="retry" data-channel="${j.channel}" data-id="${esc(r.id)}">重试</button>`:''}${['committed','failed','cancelled','source_expired'].includes(r.state)?`<button data-storage-action="forget" data-channel="${j.channel}" data-id="${esc(r.id)}">清理历史</button>`:''}</div>`:''}</article>`).join('')||'<p class="empty">暂无任务。</p>'}<div class="row"><button data-storage-page="-1" data-channel="${j.channel}" ${pages[j.channel]===0?'disabled':''}>上一页</button><span>第 ${pages[j.channel]+1} 页 · ${j.total||0} 项</span><button data-storage-page="1" data-channel="${j.channel}" ${(pages[j.channel]+1)*16>=(j.total||0)?'disabled':''}>下一页</button></div></section>`).join('');
    }finally{loading=false;}};
    document.getElementById('storageTasksReload').onclick=()=>load().catch(showError);
    document.getElementById('storageTasks').onclick=async e=>{const b=e.target.closest('button');if(!b||b.disabled)return;try{if(b.dataset.storagePage){pages[b.dataset.channel]+=Number(b.dataset.storagePage);await load();return;}if(b.dataset.storageAction){const cmd=b.dataset.channel==='upload-list'?'upload-task':'nas-task';const j=await api(`if-serial ${camera.serial} ${cmd} ${camera.slot} ${b.dataset.id} ${b.dataset.storageAction}`);if(!j.ok)throw Error(j.error);await load();notice(b.dataset.storageAction==='forget'?'历史已清理。':'任务已更新。');}}catch(err){showError(err);}};
    const policyForm=document.getElementById('cloudPolicy'),policyStatus=document.getElementById('storagePolicyStatus');
    const reloadPolicy=async(preserve=false)=>{
      const j=await client.command('policy',[camera.slot],{cameraSerial:camera.serial});if(!j.ok)throw Error(j.error||'读取策略失败');if(!active())return;
      Object.assign(p,j.policy);if(!preserve)policyForm.enabled.checked=nas?p.nas_backup:p.cloud_upload;
      policyStatus.textContent=preserve?'设置已被其他操作修改；已读取最新版本并保留你的选择。请核对后再次保存。':'已读取基站当前保存的设置。';
    };
    document.getElementById('storagePolicyReload').onclick=()=>{if(!saving)reloadPolicy().catch(showError);};
    policyForm.onsubmit=async e=>{e.preventDefault();if(saving)return;saving=true;const button=e.submitter;if(button)button.disabled=true;
      try{const j=await api(storagePolicyCommand(camera,kind,policyForm.enabled.checked));if(!active())return;if(j.policy)Object.assign(p,j.policy);policyStatus.textContent='基站已保存。';notice((nas?'NAS 备份':'云存储')+'设置已保存。');await load();}
      catch(err){if(err.code==='configuration_conflict'){try{await reloadPolicy(true);}catch(e){showError(e);}}else showError(err);}
      finally{saving=false;if(active()&&button)button.disabled=client.user.role<2;}
    };
    if(!nas){document.getElementById('uploadRate').onsubmit=async e=>{e.preventDefault();try{await api('upload-rate '+e.target.rate.value);notice('上传限速已保存，重启后仍有效。');}catch(err){showError(err);}};
      if(client.user.role>1){try{const j=await api('upload-rate');if(active()){document.getElementById('uploadRate').rate.value=j.rate_kib;if(j.scope!=='cloud_upload'){document.getElementById('uploadRateLabel').textContent='总上传限速（KiB/s）';document.getElementById('uploadRateHint').textContent='此基站旧版程序仍将云上传与 TF 调取共用限速。更新 Agent 后可分离。';}}}catch(e){showError(e);}}}
    if(!active())return true;
    if(client.user.role<2)for(const id of nas?['cloudPolicy']:['uploadRate','cloudPolicy'])document.getElementById(id).querySelectorAll('input,button').forEach(e=>e.disabled=true);
    document.getElementById('storagePolicyReload').disabled=false;
    await load();
  } else return false;
  return true;
}
function eventLabel(kind,event){if(kind==='camera_report_200'&&event){if(event.result_raw!==1)return '未确认的检测报告';return detectionKind({file_type:event.type_raw===1?5:event.type_raw===5?6:0}).label;}return ({station_started:'基站启动',pairing_stage_saved:'配对进度',camera_report_200:'摄像头检测报告',pir_wake_report:'PIR 唤醒',recording_requested:'开始事件录像',recording_stop_requested:'录像正在收尾',journal_gap:'事件日志缺口',recording_committed:'录像已保存',recording_failed:'录像未保存',recording_recovered:'录像已恢复'})[kind]||kind||'事件';}
function clipLabel(state){return ({none:'未关联短片',unknown:'短片状态尚未报告',recording:'最后报告：录像中',finishing:'最后报告：正在收尾',failed:'最后报告：录像失败',committed:'最后报告：短片已保存；查看时核对当前副本'})[state]||'';}
const management={createLiveRecordParser,eventLabel,createViewingFrameCache,captureViewingFrame,viewingFrame:(client,camera)=>viewingFrames.get(client,camera),fillCameraPreviews,cameraPreviewStamp,preferViewingFrame,detectionKind,detectionBadge,clipCameraName,loadThumbnail,previewId,createThumbnailLoader,pictureQuality,waitLivePicture,createAdaptiveQuality,observedLiveQuality,switchLiveQuality,createClient,loadRecordingCameras,createRecordingPager,recordingRange,storageError,resolveClip,createLocalRecording,storagePolicyCommand,scopedClient,requiresStation,login,render,pages:['live','recordings','events','tasks','account','storage-services','nas-settings','archive-policy','storage-policy','event-clip','server-recordings','tf-clip','tf-cache','cloud-storage','legacy-recordings','cloud-settings','nas-policy'],hasLocalRecording:()=>[...recordings].some(r=>r.busy),finishLocalRecording:()=>Promise.all([...recordings].map(r=>r.stop())),invalidate(){++renderVersion;const cleanup=mediaCleanup;mediaCleanup=()=>{};const result=cleanup();pageController?.abort();pageController=null;return Promise.resolve(result);}};
if(typeof module!=='undefined'&&module.exports)module.exports=management;else globalThis.MYVAVA_MANAGEMENT=management;
})();
