/* VAVA Home management shell; camera configuration and media behavior are independent of presentation.
 * A saved station preference, a transport echo and a hardware effect differ. */
(() => {
'use strict';
const esc=s=>String(s??'').replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
const own = (o, k) => Object.prototype.hasOwnProperty.call(o, k);
const cameraName=d=>d?.profile?.name||`摄像头 ${(d?.slot??0)+1}`;
function nameArgument(value) {
  const bytes=new TextEncoder().encode(value);
  if([...value].length>32 || bytes.length>96 || /[\u0000-\u001f\u007f-\u009f\u2028-\u202e\u2066-\u2069\ud800-\udfff]/u.test(value))
    throw Error('名称最多 32 个字符、96 个 UTF-8 字节，不能包含控制字符。');
  return bytes.length?Array.from(bytes,b=>b.toString(16).padStart(2,'0')).join(''):'-';
}
function mergeDraft(draft, source) {
  const d = draft || {base:{}, values:{}, dirty:new Set(), conflicts:new Set()};
  for (const [k, value] of Object.entries(source)) {
    const v = String(value ?? '');
    if (!d.dirty.has(k)) { d.base[k]=v; d.values[k]=v; d.conflicts.delete(k); }
    else if (d.base[k] !== v) d.conflicts.add(k);
    else d.conflicts.delete(k);
  }
  return d;
}
function editDraft(d, k, value) {
  d.values[k]=String(value);
  if (d.values[k] === d.base[k]) { d.dirty.delete(k); d.conflicts.delete(k); }
  else d.dirty.add(k);
}
function commitDraft(d, sent) {
  for (const [k, v] of Object.entries(sent)) {
    d.base[k]=String(v); d.conflicts.delete(k);
    if (d.values[k]===String(v)) d.dirty.delete(k);
  }
}
function discardDraft(d, source) {
  d.dirty.clear();d.conflicts.clear();return mergeDraft(d,source);
}
function batteryReading(d) {
  if (!Number.isFinite(d?.heartbeat_age_ms) || d.heartbeat_age_ms<0 ||
      !Number.isInteger(d.battery_raw) || d.battery_raw<0 || d.battery_raw>100) return null;
  return {percent:d.battery_raw,fresh:d.heartbeat_current===true && d.heartbeat_age_ms<15000,
    charging:d.power_raw===1?true:d.power_raw===0?false:null};
}
const batteryPower=battery=>battery.charging===true?(battery.percent===100?'已充满':'充电中'):battery.charging===false?'电池供电':'供电状态未知';
function batteryReportTime(d,snapshot) {
  if (!batteryReading(d) || snapshot?.clock_valid!==true || !Number.isFinite(snapshot.unix_seconds)) return null;
  const milliseconds=snapshot.unix_seconds*1000-d.heartbeat_age_ms;
  return milliseconds>0 && Number.isFinite(new Date(milliseconds).getTime())?milliseconds:null;
}
function batteryTimeText(milliseconds) {
  return milliseconds===null?'报告时间未知':new Date(milliseconds).toLocaleString('zh-CN',{month:'2-digit',day:'2-digit',hour:'2-digit',minute:'2-digit',hourCycle:'h23'});
}
function telemetry(d,snapshot) {
  const battery=batteryReading(d);
  if(!battery)return '尚无电量报告';
  return battery.charging===true?batteryPower(battery):`电量 ${battery.percent}% · ${batteryTimeText(batteryReportTime(d,snapshot))}`;
}
function batteryCell(d,snapshot) {
  const battery=batteryReading(d),reportedAt=batteryReportTime(d,snapshot);
  const time=reportedAt===null?'报告时间未知':`<time datetime="${new Date(reportedAt).toISOString()}" title="${esc(new Date(reportedAt).toLocaleString())}">${esc(batteryTimeText(reportedAt))}</time>`;
  return `<div class="battery-status${battery?.charging?' charging':battery&&battery.percent<=15?' low':''}" aria-label="${telemetry(d,snapshot)}"><svg class="battery-symbol" viewBox="0 0 26 14" aria-hidden="true"><rect x="1" y="1" width="21" height="12" rx="2" fill="none" stroke="currentColor" stroke-width="1.5"/><path d="M24 5v4" stroke="currentColor" stroke-width="2"/>${battery?`<rect x="3" y="3" width="${battery.percent*0.17}" height="8" rx="0.5" fill="currentColor"/>`:''}</svg>${battery?.charging?'<svg class="charging-mark" viewBox="0 0 10 14" aria-hidden="true"><path d="M6 0 0 8h4l-1 6 7-9H6z" fill="currentColor"/></svg>':''}${battery?.charging?'':`<span>${battery?`${battery.percent}%`:'暂无数据'}</span>`}</div>${battery?`<div class="cell-sub">${battery.charging?`${battery.percent}% · ${batteryPower(battery)}`:time}</div>`:''}`;
}
// Sleep closes media/control while the camera retains its ALIVE socket.
// Use the station's current connections, never pairing or a stale sleep report.
function cameraOnline(d,stationOnline=true) {
  return stationOnline===true&&d?.paired===true&&Array.isArray(d.connections)&&d.connections.some(v=>v===true);
}
function reportedSetting(group,key,reports={},operations={}) {
  let source,field,operation=key,convert=v=>v,valid=()=>true;
  if(group==='pir') {
    source='pir_config_42';operation='pir-mode';
    field={mode:'mode_raw',sensitivity:'sensitivity_raw',wait:'wait_seconds_raw',record:'record_seconds_raw'}[key];
    valid=v=>Number.isInteger(v)&&(key==='mode'?v>=0&&v<=2:key==='sensitivity'?v>=1&&v<=3:key==='wait'?v>=0&&v<=120&&v%5===0:v>=5&&v<=60&&v%5===0);
    if(key==='mode')convert=v=>['battery','monitor','custom'][v];
  } else if(group==='arming') {
    if(key!=='level')return null;
    source='pir_level_30';field='level_raw';operation='pir-level';valid=v=>Number.isInteger(v)&&v>=0&&v<=3;
  } else if(key==='detection-box') {
    source='algorithm_203';field='pd_rect_raw';valid=v=>Number.isInteger(v)&&v>=0&&v<=1;
  } else if(key==='volume') {
    source='volume_37';field='volume_raw';valid=v=>Number.isInteger(v)&&v>=0&&v<=100;
  } else {
    source='first_connect';field={mirror:'mirror_raw',ir:'ir_raw',antiflicker:'antiflicker_wire_raw',osd:'osd_raw','playback-audio':'audio_playback_raw',quality:'live_quality_raw'}[key];
    if(!field)return null;
    valid=v=>Number.isInteger(v)&&v>=0&&v<=(key==='mirror'?3:key==='quality'?2:1);
    if(key==='antiflicker')convert=v=>v===0?50:60;
  }
  const report=reports[source];if(!report||!own(report.values,field))return null;
  const raw=report.values[field],op=operations[operation],comparable=valid(raw);
  return {source,raw,value:comparable?String(convert(raw)):null,comparable,current:report.current_connection,
    age:report.age_ms,olderThanRequest:!!(report.current_connection&&op?.current_connection&&op.sent_age_ms!==null&&
      op.sent_age_ms!==undefined&&report.age_ms>op.sent_age_ms)};
}
const recordingTrigger=trigger=>({pir_first_connect:'PIR 触发',manual:'手动'})[trigger]||'未知来源';
const recordingEnd=reason=>({manual_stop:'手动结束',duration_reached:'达到时长',control_disconnected:'控制连接断开',camera_disabled:'设备停用',disarmed:'策略撤防',first_frame_timeout:'等待首帧超时',service_shutdown:'服务停止',control_send_failed:'控制发送失败',record_queue_failed:'录像入队失败',storage_failed:'存储失败'})[reason]||reason;
function groupSettingKeys(group,fields,desired) {
  return (group==='pir'?['pir-mode']:fields.map(f=>f.key)).filter(k=>own(desired.values,k));
}
function configurationFieldLocked(group,key,values,policy={}) {
  if(group!=='arming')return false;
  return (key==='level'&&policy.level_source==='work_mode') ||
    (key==='record'&&values['camera-duration']==='1');
}
function reconcilePolicyDraft(d,policy) {
  const restore=(key,value)=>{
    d.values[key]=d.base[key]=String(value);
    d.dirty.delete(key);d.conflicts.delete(key);
  };
  if(policy.level_source==='work_mode')restore('level',policy.level);
  if(d.values['camera-duration']==='1')restore('record',policy.record_seconds);
}
async function collectMedia(reader, expected, limit, onProgress, signal) {
  let received=0;const chunks=[];
  const abort=()=>{reader.cancel().catch(()=>{});};
  signal.addEventListener('abort',abort,{once:true});
  try {
    if(expected!==null && (!Number.isSafeInteger(expected) || expected<0))throw Error('下载长度无效。');
    if(expected!==null && expected>limit)throw Error('录像超过网页 64 MiB 下载上限，请使用 vavactl 流式下载。');
    for(;;) {
      if(signal.aborted)throw new DOMException('已取消下载','AbortError');
      const {done,value}=await reader.read();
      if(signal.aborted)throw new DOMException('已取消下载','AbortError');
      if(done)break;
      received+=value.byteLength;
      if(received>limit)throw Error('下载超过网页 64 MiB 上限，请使用 vavactl 流式下载。');
      if(expected!==null && received>expected)throw Error('下载长度不符，未保存文件。');
      chunks.push(value);onProgress(received,expected);
    }
    if(!received || (expected!==null && received!==expected))throw Error('下载不完整，未保存文件，请重试。');
    return new Blob(chunks,{type:'video/h264'});
  } finally {
    signal.removeEventListener('abort',abort);await reader.cancel().catch(()=>{});
  }
}
function createPairingFlow({client,onChange=()=>{},onStatus=()=>{},onDismiss=()=>{},now=()=>performance.now()}) {
  const stationId=client.stationId,controller=new AbortController();
  let generation=0,inFlight=null,boot;
  const d={seconds:120,requesting:false,closing:false,accepted:false,resumed:false,operation:null,id:null,deadline:0,error:'',unavailable:false,cancelRequested:false,closed:false};
  const emit=()=>{if(!d.closed)onChange({...d,remainingSeconds:d.operation?.active&&d.operation.phase!=='opening_ap'?Math.max(0,Math.ceil((d.deadline-now())/1000)):null});};
  const messages={camera_slots_full:'已登记 4 台摄像头，请先移除不再使用的摄像头。',pair_operation_active:'已有配对任务正在进行，请重新读取进度。',pair_ap_busy_or_helper_failed:'临时热点开启失败，请检查基站网络后重试。',camera_removal_active_or_unconfirmed:'还有摄像头恢复或删除操作，请先核对其结果。',device_boot_changed:'基站已重启，请关闭对话框后重新查看配对状态。',camera_pairing_changed:'配对任务已由其他操作更新，请关闭后重新查看。'};
  const message=e=>messages[e.code]||e.message||'无法确认基站响应，请重新读取状态。';
  const call=async(name,args=[])=>{
    const j=await client.command(name,args,{stationId,expectedBoot:boot,signal:controller.signal});
    if(!j.ok)throw Object.assign(Error(j.error||'请求失败'),{code:j.error});return j;
  };
  const read=async()=>{const at=now(),s=await call('status');if(!s.pairing?.id)throw Error('基站未返回有效配对状态。');return {s,at};};
  function dismiss(){if(d.closed)return;d.closed=true;++generation;controller.abort();onDismiss();}
  function receive({s,at}) {
    const p=s.pairing;
    if(p.id!==d.id){d.accepted=false;d.operation=null;throw Object.assign(Error('配对任务已变化'),{code:'camera_pairing_changed'});}
    onStatus(s);d.operation=p;d.deadline=at+Math.max(0,Number(p.remaining_ms)||0);d.unavailable=false;
    if(d.cancelRequested&&!p.active&&p.phase==='cancelled'){dismiss();return;}
  }
  async function poll() {
    if(d.closed||d.requesting||d.closing||!d.accepted||(d.operation&&!d.operation.active))return;
    if(inFlight)return inFlight;
    const gen=generation;
    const pending=(async()=>{try{const reply=await read();if(gen!==generation||d.closed)return;receive(reply);}
      catch(e){if(gen!==generation||d.closed)return;d.unavailable=true;if(['device_boot_changed','camera_pairing_changed'].includes(e.code)){d.accepted=false;d.error=message(e);}}
      finally{if(gen===generation)emit();}})();
    inFlight=pending;try{await pending;}finally{if(inFlight===pending)inFlight=null;}
  }
  async function start() {
    if(d.closed||d.requesting||d.closing)return;
    ++generation;d.requesting=true;d.error='';d.unavailable=false;d.accepted=false;d.operation=null;d.cancelRequested=false;d.resumed=false;boot=undefined;emit();
    try {
      await inFlight;if(d.closed)return;const before=await read();if(d.closed)return;boot=client.deviceBoot?.(stationId);onStatus(before.s);
      if(before.s.pairing.active){d.id=before.s.pairing.id;d.accepted=true;d.resumed=true;receive(before);}
      else {
        const reply=await call('pair-open',['120']);
        if(!/^[a-f0-9]{16}$/.test(reply.operation_id)||reply.operation_id==='0000000000000000')throw Error('配对请求结果未确认，请重新读取状态。');
        d.id=reply.operation_id;d.accepted=true;
      }
    }catch(e){d.error='开启失败：'+message(e);}
    finally{d.requesting=false;emit();}
    if(d.accepted)await poll();
  }
  async function cancel() {
    if(d.closed||d.requesting||d.closing)return;
    ++generation;d.closing=true;d.error='';emit();
    try {
      await inFlight;const before=await read();receive(before);
      if(d.closed)return;
      if(!before.s.pairing.active){emit();return;}
      d.cancelRequested=true;await call('pair-close');
      receive(await read());
    }catch(e){d.error='关闭失败：'+message(e);}
    finally{d.closing=false;emit();}
  }
  return {start,poll,tick:emit,close:()=>d.accepted&&(!d.operation||d.operation.active)?cancel():dismiss(),dismiss};
}
// These models are also exercised without a browser by tests/web-state.cjs.
if (typeof document === 'undefined') {
  if (typeof module !== 'undefined') module.exports={cameraName, nameArgument, mergeDraft, editDraft, commitDraft, discardDraft, batteryReading, batteryReportTime, batteryCell, telemetry, cameraOnline, reportedSetting, groupSettingKeys, configurationFieldLocked, reconcilePolicyDraft, recordingTrigger, recordingEnd, collectMedia,createPairingFlow};
  return;
}
const $=id=>document.getElementById(id);
const management=globalThis.MYVAVA_MANAGEMENT;
if(management)$('navigation').hidden=true;
const markupCache=new WeakMap();
function stableHTML(el,value){if(markupCache.get(el)!==value){el.innerHTML=value;markupCache.set(el,value);}}
const fmtBytes=n=>n>=1073741824?(n/1073741824).toFixed(1)+' GiB':n<1048576?(n/1024).toFixed(1)+' KiB':(n/1048576).toFixed(1)+' MiB';
const cameraIcon='<svg class="camera-icon" viewBox="0 0 64 64" fill="none" aria-hidden="true"><rect x="6" y="13" width="41" height="38" rx="10" stroke="currentColor" stroke-width="2"/><circle cx="26" cy="32" r="11" stroke="currentColor" stroke-width="2"/><circle cx="26" cy="32" r="4" fill="currentColor"/><path d="m47 25 11-6v26l-11-6" stroke="currentColor" stroke-width="2"/></svg>';
const names={overview:'概述',live:'观看与控制',recordings:'本地录像',camera:'摄像头设置',station:'基站设置'};
if(management)Object.assign(names,{live:'实时观看',recordings:'事件短片',events:'事件消息',tasks:'操作记录',account:'账户与授权','archive-policy':'云存储留存','event-clip':'TF 卡视频回放','tf-clip':'TF 卡视频回放','tf-cache':'TF 回放缓存','storage-policy':'TF 留存策略','nas-settings':'基站 NAS','nas-policy':'NAS 备份设置','storage-services':'云存储设置','cloud-settings':'云存储设置','cloud-storage':'云存储视频','legacy-recordings':'原厂历史导入','server-recordings':'视频来源'});
const subtitles={overview:'查看当前基站、摄像头与事件存储的运行情况。',live:'通过本地 RTSP 观看，按需唤醒摄像头。',recordings:'在基站保存、下载和管理录像。',camera:'按摄像头独立配置，保存后跟踪下发状态。',station:'配对、存储与服务状态，集中管理。'};
if(management)Object.assign(subtitles,{live:'按需观看；录像按钮将音视频保存到当前手机或电脑。',recordings:'基站 TF 卡上的事件短片，独立于云存储。',events:'查看检测、设备与视频事件。',tasks:'查询已提交操作及其确定或未知结果。',account:'管理登录账户和基站访问范围。','archive-policy':'仅管理云上传短片的期限与容量。','event-clip':'来源：基站 TF 卡。','tf-clip':'来源：基站 TF 卡。','tf-cache':'用于 TF 视频回放的临时缓存。','storage-policy':'控制 TF 配额循环清理和待同步原片保护。','nas-settings':'基站独立挂载 NAS，备份事件短片。','nas-policy':'选择当前摄像头是否备份到 NAS。','storage-services':'选择当前摄像头是否上传到云存储。','cloud-settings':'选择当前摄像头是否上传到云存储。','cloud-storage':'已上传到自有服务器的事件短片。','legacy-recordings':'通过离线工具导入的原厂历史文件。','server-recordings':'核对视频的原始来源。'});
const stateLabels={recovering_keepalive:'正在恢复保活连接',offline:'离线',awake:'已唤醒',receiving_video:'正在接收视频',keepalive_connected:'保活连接',sleep_observed:'已报告休眠',waking:'正在唤醒',sleep_requested:'已请求休眠',pair_offered:'配对中',pair_pending:'等待配对确认'};
const deliveryLabels={quality_requires_media_idle:'等待停止观看和事件短片保存完成',detection_box_requires_media_idle:'等待停止观看和事件短片保存完成',detection_box_firmware_not_verified:'该型号或固件尚未验证检测框写控制',valid_current_algorithm_report_203_required:'等待本次连接的有效算法配置报告',unmanaged:'未启用策略托管',retry_pending:'等待自动重试',setting_in_progress:'等待当前设置完成',retry_exhausted:'自动重试已用尽',waiting_transition:'等待设备状态切换',rate_limited:'下发频率已限制',waking:'正在唤醒',camera_disabled:'摄像头已停用',apply_deadline_expired:'等待下发超时，需要手动重试',quality_requires_idle_disarmed_camera:'旧版基站需停止观看、录像与布防',managed_policy_required:'需要先启用基站布防策略',operation_id_exhausted:'请求编号已用尽，需要重启服务',explicit_apply_required:'等待手动下发',explicit_retry_required:'下发中断，需要手动重试',echo_observed_effect_unknown:'收到回显，实际效果待核验',waiting_control:'等待控制连接',waiting_pir_report:'等待 PIR 配置报告',waiting_idle_for_quality:'等待媒体空闲',queued:'等待下发',sent:'已发送，等待回显',echo_observed:'收到匹配回显',echo_timeout:'等待回显超时',timeout:'下发超时',disconnected:'连接已断开',queue_timeout:'发送排队超时',idle:'无待下发设置',waiting_connection:'等待摄像头连接',waiting_camera:'等待摄像头连接',waiting_echo:'等待回显',stored:'基站已保存',not_requested:'未请求下发',never_requested:'未请求下发',none:'尚未下发',saved_not_applied:'已保存，尚未下发',complete:'下发队列已完成'};
const deliveryLabel=x=>deliveryLabels[x]||x||'尚未下发';
Object.assign(deliveryLabels,{new_value_pending:'新值等待上一请求完成',waiting_other_setting:'等待其他设置完成',preferences_cleared_in_flight_unchanged:'基站保存值已清除'});
const errors={pir_level_owned_by_work_mode:'灵敏度由摄像头工作模式决定，请在工作模式页面修改。',plan_not_found:'计划条目已不存在，请刷新。',invalid_camera_name:'名称最多 32 个字符、96 个 UTF-8 字节，不能包含控制字符。',profile_write_failed:'基站无法保存名称，请检查状态目录。',configuration_conflict:'基站配置已被其他请求修改；草稿已保留，请核对新值后再次保存。',enable_requires_managed_policy:'更改设备启停需要策略托管，不能同时选择手动诊断模式。',camera_identity_changed:'槽位对应的摄像头已改变，请刷新后重新选择。',invalid_policy_value:'策略参数超出允许范围。',invalid_pir_custom_parameters:'自定义 PIR 参数不合法，请检查范围和步长。',quality_requires_idle:'更改画质需要先停止观看、录像并撤防。',record_start_unavailable:'当前无法开始录像，请检查连接、启用状态和存储配额。',camera_disabled:'摄像头已停用。',use_rtsp_or_local_media_client:'请使用 RTSP 或本地媒体客户端。'};
const opts=(...rows)=>rows;
const groups={
 general:{title:'名称与设备信息',desc:'给摄像头命名，查看设备实际报告的版本。',kind:'profile',fields:[
  {key:'name',type:'text',label:'摄像头名称',hint:'保存在本基站，最多 32 个字符、96 个 UTF-8 字节。留空恢复默认名称；保存不会唤醒摄像头。'}]},
 image:{title:'图像与日夜',desc:'画面方向、红外策略和抗闪烁。',kind:'settings',note:'水印和抗闪烁为扩展配置；原厂 VA-HS03 摄像头设置页面隐藏水印入口。',fields:[
  {key:'mirror',label:'画面方向',hint:'调整摄像头的安装朝向。',options:opts(['0','正常'],['1','水平镜像'],['2','垂直翻转'],['3','镜像并翻转'])},
  {key:'ir',label:'红外策略',hint:'保留原固件的 0 / 1 取值；日夜行为尚待实机确认。',options:opts(['0','策略 0'],['1','策略 1'])},
  {key:'antiflicker',label:'抗闪烁',hint:'根据拍摄环境的交流电频率选择。',options:opts(['50','50 Hz'],['60','60 Hz'])},
  {key:'osd',label:'水印配置',hint:'配置命令已实现；画面是否显示仍待实机核对。',options:opts(['0','关闭'],['1','开启'])}]},
 video:{title:'视频画质',desc:'使用原固件已有的直播画质档位。',kind:'settings',note:'设置页保存固定档位，等待观看和事件短片结束后下发。观看页支持切档和自动选择；切档时播放器会短暂重新连接。',fields:[
  {key:'quality',label:'直播画质',hint:'此款摄像头实测为 1080p / 720p / 360p；原厂目标码率 1000 / 700 / 400 kbps。',options:opts(['0','高 · 1000 kbps'],['1','中 · 700 kbps'],['2','低 · 400 kbps'])}]},
 audio:{title:'音频扩展配置',desc:'控制摄像头的声音播放配置。',kind:'settings',note:management?'实时观看、事件短片和本机录像支持摄像头音频。此处音量控制与扬声器实际发声仍需实机验证。':'原厂 VA-HS03 摄像头设置页面隐藏音频入口。本地 RTSP 和录像目前仅含视频；音量、对讲解码与发声仍需实机验证。',fields:[
  {key:'playback-audio',label:'声音播放',hint:'允许或关闭摄像头端的声音播放。',options:opts(['1','允许'],['0','关闭'])},
  {key:'volume',label:'扬声器音量配置',hint:'原固件配置范围为 0–100。',min:0,max:100,step:1}]},
 pir:{title:'摄像头工作模式',desc:'选择续航优先、最佳监控或自定义。',kind:'pir',note:'续航优先：中灵敏度、短片 10 秒、再触发间隔 20 秒。最佳监控：高灵敏度、短片 20 秒、再触发间隔 0 秒。工作模式决定灵敏度，每周计划决定何时布防。',fields:[
  {key:'mode',label:'工作模式',hint:'选择预设，或自行指定参数。',options:opts(['battery','续航优先'],['monitor','最佳监控'],['custom','自定义'])},
  {key:'sensitivity',label:'灵敏度',hint:'自定义模式使用所选档位；计划撤防后，再次布防仍恢复此档位。',options:opts(['1','1 · 高'],['2','2 · 中'],['3','3 · 低']),custom:true},
  {key:'record',label:'短片长度（秒）',hint:'5–60 秒，步长 5 秒。默认由基站跟随此工作模式保存事件短片。',min:5,max:60,step:5,custom:true},
  {key:'wait',label:'再触发间隔（秒）',hint:'5–120 秒，步长 5 秒。这是两次触发之间的间隔。',min:5,max:120,step:5,custom:true}]},
 arming:{title:'布防与休眠',desc:'由基站计算布防状态并控制摄像头。',kind:'policy',fields:[
  {key:'enabled',label:'设备启用',hint:'更改启停会启用策略托管。停用会结束观看、录像并撤防。',options:opts(['1','启用'],['0','停用'])},
  {key:'mode',label:'布防策略',hint:'计划模式依据每周计划和时区响应 PIR。持续布防不代表持续录像。',options:opts(['off','持续撤防'],['on','持续布防'],['schedule','按每周计划'],['diagnostic','手动诊断 · 停止策略托管'])},
  {key:'level',label:'布防灵敏度',hint:'保存工作模式后自动跟随该模式；尚未保存时可设置兼容档位。',options:opts(['1','高'],['2','中'],['3','低'])},
  {key:'record',label:'基站指定短片时长（秒）',hint:'仅选择“基站指定时长”时生效，5–120 秒。',min:5,max:120,step:1},
  {key:'idle',label:'空闲休眠（秒）',hint:'0 为关闭；托管启用时，在没有观看、录像或待处理设置后计时。',min:0,max:3600,step:1},
  {key:'timezone',label:'计划时区（UTC 分钟偏移）',hint:'中国标准时间为 +480；固定偏移，不自动处理夏令时。',min:-720,max:840,step:1}]},
 schedule:{title:'每周计划',desc:'最多 10 条，支持跨午夜和全天。'},
 detection:{title:'检测框与检测报告',desc:'控制画面上的检测框标记。',kind:'settings',note:'当前支持 VA-HS03 / 1.4.0.54。等待有效检测配置报告和媒体空闲后下发，避免干扰事件短片。关闭画框不等于关闭移动侦测。',fields:[
  {key:'detection-box',label:'检测框显示',hint:'按原厂 App 同时设置 enable 和 rect，灵敏度使用原厂值 75。在检测到目标时显示标记。',options:opts(['0','关闭'],['1','开启'])}]},
 streams:{title:'流地址与设备',desc:'本地访问与设备操作。'}
};
if(management){
  groups.arming.fields.splice(4,0,
    {key:'camera-duration',label:'PIR 录像时长来源',hint:'跟随当前连接的有效 PIR 报告；报告缺失时以 10 秒为回退，并保留 10 秒收尾期限。',options:opts(['1','跟随摄像头工作模式'],['0','基站指定时长'])},
    {key:'confirm-detection',label:'PIR 短片保留规则',hint:'检测确认指原始 result=1 报告，并不等于已经识别人形。无确认短片在收尾时清除。',options:opts(['1','收到检测报告后保留'],['0','每次 PIR 唤醒都保留'])});
}
const stationGroups={pairing:'摄像头配对',storage:'存储',...(management?{nas:'NAS 存储'}:{}),network:'网络与访问',time:'时间',diagnostics:'诊断'};
const pairingLabels={idle:'尚未开始',opening_ap:'正在启动临时热点',waiting_camera:'等待摄像头进入配对模式',credentials_sent:'已下发正式网络信息',waiting_formal_connection:'等待摄像头连接正式网络',waiting_confirmation:'等待摄像头确认',completed:'配对完成',cancelled:'已取消',expired:'已超时',interrupted:'已中断',failed:'配对未完成'};
let key='', apiClient=null, state=null, selected='', page='overview', section='general', stationSection='pairing';
let baseOnline=null,lastRefreshError='',busy=false, refreshing=null, settingsSeq=0, recordsSeq=0, recordsPage=0, session=0;
const settingsCache=new Map(), drafts=new Map(), planDrafts=new Map();
let downloadJob=null, recordsRequest=null, pairingDialog=null;
const device=()=>state?.devices.find(d=>String(d.slot)===selected);
const managedPage=()=>page==='station'?({nas:'nas-settings','nas-backup':'nas-policy'}[stationSection]||page):page;
const identity=d=>d?`${d.slot}:${d.serial}`:'';
const cache=()=>settingsCache.get(identity(device()));
const notice=(s,error=false)=>{ $('notice').textContent=s; $('notice').hidden=!s; $('notice').classList.toggle('error',error); };
const cameraCommand=(d,command,tail='',revision=null)=>{if(!d)throw Error('请先选择已配对摄像头。');return `if-serial ${d.serial} ${revision===null?'':`if-revision ${revision} `}${command} ${d.slot}${tail?' '+tail:''}`;};
async function api(command) {
  const ctl=new AbortController(), timer=setTimeout(()=>ctl.abort(),management?45000:8000);
  try {
    const r=await apiClient.commandLineRaw(command,{signal:ctl.signal});
    if(r.status===401)throw Error('管理密钥无效或已更换，请退出后重新连接。');
    const j=await r.json(); if(!r.ok || !j.ok){const e=Error(errors[j.error]||j.error||'请求失败');e.code=j.error;throw e;} return j;
  } catch(e) { if(e.name==='AbortError')throw Error('请求超时，请刷新确认状态后再重试。'); throw e; }
  finally { clearTimeout(timer); }
}
function showPairingDialog() {
  if(pairingDialog)return;
  notice('');const source=apiClient,gen=session,stationId=source.stationId;
  const dialog=document.createElement('dialog');dialog.className='pairing-dialog';
  dialog.setAttribute('aria-labelledby','pairingTitle');dialog.setAttribute('aria-describedby','pairingIntro');
  dialog.innerHTML='<h2 id="pairingTitle">摄像头配对</h2><p id="pairingIntro"></p><p id="pairingPhase" role="status"></p><p id="pairingCountdown" class="pairing-countdown" aria-live="off" hidden></p><p id="pairingSerial" hidden></p><p id="pairingUnavailable" role="status" hidden>暂时无法读取基站状态，正在重试；请勿重复打开配对窗口。</p><p id="pairingError" class="pairing-error" role="alert" hidden></p><p class="hint">请将摄像头放在基站附近，并让它进入配对模式。配对成功后，到摄像头设置选择工作模式。</p><div class="row pairing-actions"><button id="pairingRetry" class="primary" hidden>重试</button><button id="pairingDismiss">取消配对</button></div>';
  const el=id=>dialog.querySelector('#'+id);let ticks=0,timer;
  const flow=createPairingFlow({client:source,
    onStatus:s=>{if(gen===session&&apiClient===source&&source.stationId===stationId){state=s;updateShell();updateStatusPanels();}},
    onChange:d=>{
      const p=d.operation,working=d.requesting||d.closing,active=d.accepted&&(!p||p.active);
      const result={completed:'摄像头配对成功。',cancelled:'配对窗口已关闭。',expired:'配对窗口已超时，请重新打开后再试。',interrupted:'配对已中断，请重新打开后再试。',failed:'配对失败，请检查摄像头和基站网络后重试。'};
      el('pairingIntro').textContent=d.requesting?'正在读取基站并请求配对窗口…':p&&!p.active?(result[p.phase]||'当前没有配对任务。'):d.accepted?(d.resumed?'配对窗口已开启，请让摄像头进入配对模式。':'已请求 120 秒配对窗口，请让摄像头进入配对模式。'):'未能确认配对请求。';
      el('pairingPhase').textContent=d.closing?'正在关闭配对窗口…':p?.active?(p.phase==='opening_ap'?'正在开启临时热点，热点就绪后开始倒计时。':pairingLabels[p.phase]||p.phase):d.accepted&&!p?'正在同步本次配对状态…':'';
      el('pairingCountdown').hidden=d.remainingSeconds===null||d.closing;
      el('pairingCountdown').textContent=d.remainingSeconds?'剩余 '+d.remainingSeconds+' 秒':'正在确认配对结果…';
      el('pairingSerial').hidden=!p?.serial;el('pairingSerial').textContent=p?.serial?'摄像头 SN：'+p.serial:'';
      el('pairingUnavailable').hidden=!d.unavailable;el('pairingError').hidden=!d.error;el('pairingError').textContent=d.error;
      el('pairingDismiss').textContent=active?'取消配对':'关闭';el('pairingDismiss').disabled=working;
      el('pairingRetry').hidden=working||active||(!d.error&&!['expired','interrupted','failed'].includes(p?.phase));
    },
    onDismiss:()=>{clearInterval(timer);dialog.close();dialog.remove();pairingDialog=null;refreshAfterWrite().catch(()=>{});}
  });
  pairingDialog=flow;document.body.append(dialog);dialog.showModal();
  el('pairingDismiss').onclick=()=>flow.close();el('pairingRetry').onclick=()=>flow.start();
  dialog.addEventListener('cancel',e=>{e.preventDefault();flow.close();});
  timer=setInterval(()=>{flow.tick();if(++ticks%2===0)flow.poll();},1000);
  flow.start();
}
function getSource(group,d,j) {
  if(group==='general')return {name:d.profile?.name??''};
  if(group==='arming') {const p=d.policy;return {enabled:p.enabled?'1':'0',mode:p.managed?p.mode:'diagnostic',level:p.level,record:p.record_seconds,idle:p.idle_seconds,timezone:p.utc_offset_minutes,...(management?{'camera-duration':p.camera_duration?'1':'0','confirm-detection':p.confirm_detection?'1':'0'}:{})};}
  if(group==='pir') {const p=j?.desired.values['pir-mode']; return {mode:p?['battery','monitor','custom'][p.mode]:'',sensitivity:p?.sensitivity??'',wait:p?.wait_seconds??'',record:p?.record_seconds??''};}
  return Object.fromEntries(groups[group].fields.map(f=>[f.key,j?.desired.values[f.key]??'']));
}
function getDraft(group,d=device(),j=cache()) {
  const id=identity(d)+'/'+group;
  const v=mergeDraft(drafts.get(id),getSource(group,d,j)); drafts.set(id,v); return v;
}
function dirtyTotal() {return [...drafts.values()].reduce((n,d)=>n+d.dirty.size,0)+[...planDrafts.values()].filter(d=>d.dirty).length;}
function groupDirty(group) {return group==='schedule'?!!planDrafts.get(identity(device()))?.dirty:!!drafts.get(identity(device())+'/'+group)?.dirty.size;}
function updateRail() {
  const q=$('search').value.toLowerCase().trim(), camera=page==='camera';
  const rows=camera?Object.entries(groups).map(([id,g])=>[id,g.title,[g.title,g.desc,...(g.fields||[]).flatMap(f=>[f.label,f.key,f.hint])].join(' ')]):Object.entries(stationGroups).map(([id,title])=>[id,title,title]);
  const markup=rows.filter(r=>r[2].toLowerCase().includes(q)).map(([id,title])=>`<button data-section="${id}" class="${id===(camera?section:stationSection==='nas-backup'?'nas':stationSection)?'active':''}" ${id===(camera?section:stationSection==='nas-backup'?'nas':stationSection)?'aria-current="page"':''}>${title}${camera&&groupDirty(id)?'<span class="dot" aria-label="有未保存草稿">•</span>':''}</button>`).join('')||'<p class="hint">没有匹配的配置。</p>';
  stableHTML($('sections'),markup);
}
function updateContext() {
  const d=device(), contextual=['camera','live','cloud-settings','storage-services','nas-policy'].includes(managedPage())||(!management&&page==='recordings');
  $('cameraSelect').hidden=!contextual;document.querySelector('label[for="cameraSelect"]').hidden=!contextual;
  $('cameraContext').hidden=!contextual||!d;
  if(d) $('cameraContext').innerHTML=`<span><strong>${esc(cameraName(d))}</strong> · ${esc(d.serial)} · ${esc(stateLabels[d.state]||d.state)}</span><span>${d.policy.enabled?'已启用':'已停用'} · ${!d.policy.managed?'手动诊断 · 未托管布防':d.policy.desired_armed?'策略要求布防':'策略要求撤防'} · ${esc(telemetry(d,state))}</span>`;
}
function updateShell() {
  $('version').textContent=state?.version||'—';
  $('connection').textContent=baseOnline===false?'基站离线 · 服务器已连接':'基站已连接'; $('connection').className=baseOnline===false?'badge warn':'badge good';
  if(!state){$('cameraSelect').innerHTML='<option value="">暂无摄像头状态</option>';$('cameraSelect').disabled=true;updateContext();return;}
  const optionsMarkup=state.devices.length?state.devices.map(d=>`<option value="${d.slot}">${esc(cameraName(d))} · ${esc(d.serial)}</option>`).join(''):'<option value="">尚无摄像头</option>';
  stableHTML($('cameraSelect'),optionsMarkup);
  $('cameraSelect').value=selected; $('cameraSelect').disabled=!state.devices.length;
  updateContext();
}
async function refresh() {
  if(!key)return;
  if(refreshing)return refreshing;
  const gen=session;
  refreshing=(async()=>{
    const wasOnline=baseOnline;
    if(management){
      const directory=await apiClient.devices(AbortSignal.timeout(8000));if(gen!==session)return;
      stableHTML($('stationSelect'),directory.devices.map(d=>`<option value="${esc(d.id)}">${esc(d.id)} · ${d.online?'在线':'离线'}</option>`).join(''));
      $('stationSelect').value=apiClient.stationId;
      baseOnline=!!directory.devices.find(d=>d.id===apiClient.stationId)?.online;
      if(!baseOnline){updateShell();if(wasOnline!==false&&management.requiresStation(page))render();lastRefreshError='';return;}
    }
    let j;try{j=await api('status');}catch(e){
      if(management&&e.code==='device_offline'&&gen===session){baseOnline=false;updateShell();if(wasOnline!==false&&management.requiresStation(page))render();return;}throw e;
    }
    if(gen!==session)return;const old=identity(device());state=j;baseOnline=true;lastRefreshError='';
    if(!state.devices.some(d=>String(d.slot)===selected))selected=String(state.devices[0]?.slot??'');
    updateShell();
    if(old!==identity(device())||wasOnline===false) {settingsSeq++;recordsSeq++;recordsPage=0;if(!management||management.requiresStation(page))render();}
    else updateStatusPanels();
    if(!management||page==='camera')await loadSettings();
  })();
  try {await refreshing;} finally {refreshing=null;}
}
async function refreshAfterWrite(){if(refreshing)await refreshing.catch(()=>{});await refresh();}
async function loadSettings() {
  const d=device();if(!d || !d.paired || baseOnline===false)return;
  const seq=++settingsSeq, gen=session, id=identity(d);
  const j=await api(cameraCommand(d,'settings'));
  if(gen!==session || seq!==settingsSeq || id!==identity(device()))return;
  settingsCache.set(id,j);
  for(const [g,meta] of Object.entries(groups))if(meta.fields)getDraft(g,d,j);
  if(page==='camera' && groups[section].fields) {
    if(!$('settingsForm'))renderCamera();else syncForm();
  }
  if(page==='camera' && section==='detection')renderDetection();
  updateDelivery();updateRail();
}
let overviewPreviewCleanup=()=>{},liveDialog=null;
function closeLiveDialog(restore=true) {
  if(!liveDialog)return Promise.resolve();
  const current=liveDialog;liveDialog=null;
  // invalidate captures the last displayed frame before detaching the player.
  const stopped=management.invalidate();current.dialog.close();current.dialog.remove();
  if(restore&&page==='overview'&&state)renderOverview();
  if(current.trigger?.isConnected)current.trigger.focus();
  return stopped;
}
async function openLiveDialog(camera,trigger) {
  if(liveDialog||!camera?.paired)return;
  const dialog=document.createElement('dialog');dialog.className='live-dialog';dialog.setAttribute('aria-labelledby','liveDialogTitle');
  dialog.innerHTML=`<header class="live-dialog-head"><div><h2 id="liveDialogTitle">${esc(cameraName(camera))}</h2><p class="serial">${esc(camera.serial)}</p></div><button type="button" class="live-dialog-close" aria-label="关闭实时观看">×</button></header><p class="live-dialog-notice" role="alert" hidden></p><div class="live-dialog-content"></div>`;
  const current={dialog,trigger};liveDialog=current;document.body.append(dialog);dialog.showModal();
  dialog.querySelector('.live-dialog-close').onclick=()=>{void closeLiveDialog().catch(e=>notice(e.message,true));};
  dialog.addEventListener('cancel',e=>{e.preventDefault();void closeLiveDialog().catch(e=>notice(e.message,true));});
  const localNotice=(text,error=false)=>{if(liveDialog!==current)return;const el=dialog.querySelector('.live-dialog-notice');el.hidden=!text;el.textContent=text;el.classList.toggle('error',error);};
  try{await management.render('live',{client:apiClient,content:dialog.querySelector('.live-dialog-content'),notice:localNotice,camera,autoStart:true,embedded:true});}
  catch(e){if(e.name!=='AbortError')localNotice(e.message,true);}
}

function render() {
  void closeLiveDialog(false);
  overviewPreviewCleanup();overviewPreviewCleanup=()=>{};
  if(!key||!apiClient)return;
  // Navigation may have replaced content without using stableHTML.
  markupCache.delete($('content'));
  const nasPage=management&&page==='station'&&['nas','nas-backup'].includes(stationSection);
  $('pageTitle').textContent=nasPage?'基站设置 · '+(stationSection==='nas'?'NAS 存储':'NAS 摄像头备份'):names[page];
  $('pageSubtitle').textContent=nasPage?'当前基站：'+apiClient.stationId+'。NAS 配置与备份任务仅对这台基站生效。':subtitles[page];
  $('breadcrumb').textContent='设备管理 / '+(names[page]||'概述');
  $('railHint').textContent=page==='station'?'配对、TF 存储、NAS 与时间均属于当前选中的基站。网络与访问参数在部署配置中设置。':'每台摄像头独立保存设置。切换页面会保留未保存草稿。';
  const navigationPage=management?({'tf-clip':'recordings','event-clip':'recordings','tf-cache':'recordings','storage-policy':'recordings','cloud-settings':'cloud-storage','storage-services':'cloud-storage','archive-policy':'cloud-storage','legacy-recordings':'cloud-storage','nas-settings':'station','nas-policy':'station'}[page]||page):page;
  document.querySelectorAll('[data-page]').forEach(b=>{b.classList.toggle('active',b.dataset.page===navigationPage);if(b.dataset.page===navigationPage)b.setAttribute('aria-current','page');else b.removeAttribute('aria-current');});
  const withRail=page==='camera'||page==='station'; $('rail').hidden=!withRail; $('layout').classList.toggle('with-rail',withRail);
  updateContext();updateRail();
  if(management){management.invalidate();if(baseOnline===false&&management.requiresStation(page)){$('content').innerHTML='<section class="card"><h2>基站当前离线</h2><p>此功能需要基站连接。<a href="#cloud-storage">已上传的云存储视频</a>和<a href="#events">已接收的事件</a>仍可查看。</p></section>';return;}if(management.pages.includes(managedPage())){management.render(managedPage(),{client:apiClient,content:$('content'),notice,camera:device(),cameras:state?.devices||[],api}).catch(e=>{if(e.name!=='AbortError')notice(e.message,true);});return;}}
  if(!state){$('content').innerHTML='<section class="card">基站离线。已上传的云存储视频、事件与操作记录仍可查看。</section>';return;}
  if(page==='overview')renderOverview();
  else if(page==='station')renderStation();
  else if(!device())$('content').innerHTML='<section class="card empty"><h2>尚无已配对摄像头</h2><p>先在基站中打开配对窗口，添加摄像头。</p><button data-action="goto-pair" class="primary">前往配对</button></section>';
  else if(!device().paired)$('content').innerHTML='<section class="card empty">配对尚未完成，请等待摄像头确认。</section>';
  else if(page==='camera')renderCamera();
  else if(page==='live')renderLive();
  else if(page==='recordings')renderRecordings();
}
// Status polling patches text and controls in place. In particular, it must
// preserve the decoded preview image and any in-flight thumbnail request.
function patchOverview(container,html) {
  const grid=container.querySelector('.camera-dashboard-grid');
  if(!grid||!markupCache.has(container)){stableHTML(container,html);return;}
  const template=document.createElement('template');template.innerHTML=html;
  const fresh=template.content;
  for(const selector of ['.stats','.station-cameras>.panel-head','.overview-grid>.card:last-child','.station-note']){
    const old=container.querySelector(selector),next=fresh.querySelector(selector);
    if(old&&next)stableHTML(old,next.innerHTML);
  }
  const previous=new Map([...grid.querySelectorAll('[data-camera-identity]')].map(el=>[el.dataset.cameraIdentity,el]));
  const rows=[...fresh.querySelectorAll('[data-camera-identity]')];
  if(rows.length){
    grid.querySelector('.empty')?.remove();
    rows.forEach((next,index)=>{
      const old=previous.get(next.dataset.cameraIdentity),row=old||next;
      if(old){for(const selector of ['.camera-card-top','.camera-card-bottom'])stableHTML(old.querySelector(selector),next.querySelector(selector).innerHTML);previous.delete(next.dataset.cameraIdentity);}
      if(grid.children[index]!==row)grid.insertBefore(row,grid.children[index]||null);
    });
    for(const el of previous.values())el.remove();
  }else {if(previous.size)markupCache.delete(grid);stableHTML(grid,fresh.querySelector('.camera-dashboard-grid').innerHTML);}
}
function renderOverview() {
  const ds=state.devices,s=state.storage,paired=ds.filter(d=>d.paired),awake=ds.filter(d=>cameraOnline(d,baseOnline));
  const quota=s.quota_bytes>0?Math.min(100,s.bytes/s.quota_bytes*100):0;
  const quotaText=quota>0&&quota<0.1?'&lt; 0.1':quota.toFixed(1).replace(/\.0$/,'');
  const oldContent=$('content').firstElementChild;
  const rows=ds.map(d=>`<article class="camera-card camera-dashboard" data-camera-identity="${esc(identity(d))}"><div class="camera-card-top"><div><h3>${esc(cameraName(d))}</h3><p class="serial">${esc(d.serial)}</p></div><div class="camera-card-actions"><a class="camera-clips" href="#recordings/${esc(d.serial)}/${d.slot}" aria-label="${esc(cameraName(d))} 的事件短片" title="事件短片"><svg viewBox="0 0 28 28" fill="none" aria-hidden="true"><path d="M11 4h11a3 3 0 0 1 3 3v15a3 3 0 0 1-3 3H6a3 3 0 0 1-3-3V7a3 3 0 0 1 3-3h1M9 2l4 5M11 11l7 4-7 4z" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"/></svg></a><button class="camera-settings" data-open="camera" data-slot="${d.slot}" aria-label="${esc(cameraName(d))} 的设置" title="摄像头设置"><svg viewBox="0 0 24 24" fill="none" aria-hidden="true"><path d="M18.94 9.74L21.29 10.02L21.29 13.98L18.94 14.26L18.50 15.31L19.97 17.17L17.17 19.97L15.31 18.50L14.26 18.94L13.98 21.29L10.02 21.29L9.74 18.94L8.69 18.50L6.83 19.97L4.03 17.17L5.50 15.31L5.06 14.26L2.71 13.98L2.71 10.02L5.06 9.74L5.50 8.69L4.03 6.83L6.83 4.03L8.69 5.50L9.74 5.06L10.02 2.71L13.98 2.71L14.26 5.06L15.31 5.50L17.17 4.03L19.97 6.83L18.50 8.69Z" stroke="currentColor" stroke-width="1.8" stroke-linejoin="round"/><circle cx="12" cy="12" r="3.2" stroke="currentColor" stroke-width="1.8"/></svg></button></div></div><button class="camera-cover clip-preview" data-camera-preview="${esc(d.serial)}" data-open="live" data-slot="${d.slot}" aria-label="观看 ${esc(cameraName(d))}">最近事件画面</button><div class="camera-card-bottom"><div class="camera-state"><span class="camera-connectivity ${cameraOnline(d,baseOnline)?'online':'offline'}"><span class="connection-dot" aria-hidden="true"></span>${cameraOnline(d,baseOnline)?'在线':'离线'}</span><span>${esc(stateLabels[d.state]||d.state)}${d.recording.active?' · 正在保存事件短片':''}</span></div><div class="camera-battery">${batteryCell(d,state)}</div></div><p class="camera-preview-time hint">历史事件预览 · 点击画面实时观看</p></article>`).join('');
  patchOverview($('content'),`<div class="stats"><article class="card stat"><p>已配对摄像头</p><div class="value">${paired.length}<small>/ 4 台</small></div><p class="stat-foot">${Math.max(0,4-ds.length)} 个可用槽位</p></article><article class="card stat"><p>在线摄像头</p><div class="value">${awake.length}<small>台</small></div><p class="stat-foot">低功耗摄像头空闲时休眠</p></article><article class="card stat"><p>事件短片</p><div class="value">${s.records}<small>段</small></div><p class="stat-foot">${s.active?`${s.active} 路正在保存`:'当前没有正在保存的短片'}</p></article><article class="card stat"><p>录像配额使用</p><div class="value">${quotaText}<small>%</small></div><p class="stat-foot">${s.available?'基站存储可用':'基站存储暂不可用'}</p></article></div>
  <div class="overview-grid"><section class="card station-cameras"><div class="panel-head"><h2>${esc(apiClient?.stationId||'当前基站')}</h2>${ds.length<4?'<a class="panel-add-camera" href="#station/pairing">＋ 添加摄像头</a>':'<span class="hint">4 / 4 台</span>'}</div><div class="camera-dashboard-grid">${rows||'<p class="empty">尚未添加摄像头。点击上方“添加摄像头”打开配对窗口，再让摄像头进入配对模式。</p>'}</div></section>
  <section class="card"><div class="panel-head"><h2>基站本地存储</h2><span class="badge ${s.available?'good':'warn'}">${s.available?'可用':'未就绪'}</span></div><div class="storage-panel"><h3>事件短片</h3><div class="storage-usage">${fmtBytes(s.bytes)}<small> / ${fmtBytes(s.quota_bytes)}</small></div><p class="hint">录像占用 / 录像配额</p><div class="meter"><span style="width:${quota}%"></span></div><p class="hint">配额是录像使用上限，不代表磁盘总容量。</p><p class="hint">${s.encrypted?'新短片加密保存':'新短片未启用加密'} · 本机录像保存到观看设备</p><div class="storage-links"><a href="#recordings">查看事件短片 <span>›</span></a><a href="#cloud-storage">查看云存储视频 <span>›</span></a><a href="#station/nas">此基站的 NAS 设置 <span>›</span></a></div></div></section></div>
  <div class="station-note"><span class="badge ${state.clock_valid?'good':'warn'}">${state.clock_valid?'基站时间已同步':'基站时间尚未确认'}</span><span>${state.clock_valid?'每周布防计划可按配置运行':'每周计划暂时保持撤防'}</span><a href="#station/time">时间设置 ›</a></div>`);
  if(management){if(oldContent!==$('content').firstElementChild){overviewPreviewCleanup();overviewPreviewCleanup=management.fillCameraPreviews(apiClient,$('content'),ds,s);}else overviewPreviewCleanup.update?.(ds,s);}
}

function formField(f,d) {
  const val=d.values[f.key]??'', options=f.options;
  const control=f.type==='text'?`<input id="field-${f.key}" data-field="${f.key}" type="text" maxlength="64" value="${esc(val)}" placeholder="例如：前门摄像头">`:options?`<select id="field-${f.key}" data-field="${f.key}"><option value="">未保存 · 请选择</option>${options.map(([v,t])=>`<option value="${v}" ${val===v?'selected':''}>${t}</option>`).join('')}</select>`:`<input id="field-${f.key}" data-field="${f.key}" type="number" min="${f.min}" max="${f.max}" step="${f.step}" value="${esc(val)}" placeholder="未保存">`;
  return `<div class="field ${d.dirty.has(f.key)?'dirty':''}" data-row="${f.key}" ${f.custom&&d.values.mode!=='custom'?'hidden':''}><div><label for="field-${f.key}">${f.label}</label><p class="hint" id="hint-${f.key}">${f.hint}</p></div><div class="field-value">${control}<span class="source" id="source-${f.key}"></span><span class="observed" id="observed-${f.key}"></span></div></div>`;
}
function renderCamera() {
  const d=device(); if(!d)return;
  if(section==='schedule'){renderSchedule();return;}
  if(section==='streams'){renderStreams();return;}
  if(!cache()) { $('content').innerHTML='<section class="card empty">正在读取摄像头配置…</section>';return; }
  const g=groups[section], draft=getDraft(section);
  $('content').innerHTML=`<section class="card"><div class="form-head"><h2>${g.title}</h2><p>${g.desc}</p></div>${g.note?`<div class="info warning">${g.note}</div>`:''}<div id="prerequisite"></div><form id="settingsForm" autocomplete="off">${g.fields.map(f=>formField(f,draft)).join('')}<div class="savebar"><span id="dirtyCount"></span><button type="button" data-action="discard">取消更改</button><button type="submit" class="primary" id="saveSettings">保存更改</button></div></form><div id="delivery" class="delivery"></div><details><summary>配置与回显诊断</summary><pre id="settingsJson"></pre></details></section>`;
  $('settingsForm').addEventListener('input',onField);
  $('settingsForm').addEventListener('change',onField);
  $('settingsForm').addEventListener('submit',saveSettings);
  if(section==='general'){
    $('delivery').remove();$('settingsJson').parentElement.remove();
    $('content').insertAdjacentHTML('beforeend','<section class="card"><h2>设备报告</h2><div id="deviceInfo"></div></section>');
    updateDeviceInfo();
  }
  if(section==='detection'){
    $('content').insertAdjacentHTML('beforeend','<section class="card" id="detectionReport"></section>');renderDetection();
  }
  for(const f of g.fields)$('field-'+f.key).setAttribute('aria-describedby','hint-'+f.key);
  syncForm();updateDelivery();
}
function onField(e) {
  const k=e.target.dataset.field;if(!k)return;
  const d=getDraft(section);editDraft(d,k,e.target.value);
  syncForm();updateRail();
}
function syncForm() {
  if(!$('settingsForm'))return;
  const g=groups[section],d=getDraft(section);
  const currentPolicy=device().policy;
  if(section==='arming')reconcilePolicyDraft(d,currentPolicy);
  for(const f of g.fields) {
    const locked=configurationFieldLocked(section,f.key,d.values,currentPolicy);
    const el=$('field-'+f.key);if(locked||document.activeElement!==el)el.value=d.values[f.key]??'';
    el.disabled=busy;const row=document.querySelector(`[data-row="${f.key}"]`);row.classList.toggle('dirty',d.dirty.has(f.key));
    if(f.custom){row.hidden=d.values.mode!=='custom';el.disabled=busy||row.hidden;}
    if(locked)el.disabled=true;
    $('source-'+f.key).textContent=d.dirty.has(f.key)?(d.conflicts.has(f.key)?'基站值已变化；保留了你的草稿':'未保存的更改'):(d.base[f.key]===''?'基站尚未保存此项；不代表摄像头默认值':'基站保存值');
    if(section==='arming'&&f.key==='level'&&currentPolicy.level_source==='work_mode')$('source-'+f.key).textContent='跟随摄像头工作模式；请在工作模式中修改';
    if(section==='arming'&&f.key==='record'&&d.values['camera-duration']==='1')$('source-'+f.key).textContent='当前跟随摄像头，此指定值不参与录制';
  }
  $('dirtyCount').textContent=d.dirty.size?`${d.dirty.size} 项未保存${d.conflicts.size?' · 基站配置已变化，保存前请核对':''}`:'没有未保存的更改';
  $('saveSettings').disabled=busy||!d.dirty.size;
  if(g.kind==='profile'){
    $('source-name').textContent=d.dirty.has('name')?(d.conflicts.has('name')?'名称已被其他请求更改，草稿已保留':'未保存的名称'):(d.base.name?'基站保存的名称':'使用默认名称');
    $('observed-name').textContent='';updateDeviceInfo();return;
  }
  const deviceNow=device(),j=cache();
  for(const f of g.fields) {
    const report=reportedSetting(section,f.key,j?.settings.reports,j?.settings.operations),el=$('observed-'+f.key);
    if(!report){el.textContent=section==='arming'&&f.key!=='level'?'':f.key==='quality'?'暂无可独立核验画质的报告':'尚未收到此项配置报告';continue;}
    const text=report.comparable?(f.options?.find(([v])=>v===report.value)?.[1]||report.value):`原始值 ${report.raw}（未解释）`;
    const source=section==='arming'?'MCU 档位报告':'摄像头报告';
    const age=report.age<60000?`${Math.floor(report.age/1000)} 秒前`:`${Math.floor(report.age/60000)} 分钟前`;
    let detail=!report.current?'历史连接报告':report.olderThanRequest?'早于最近下发':'本次连接报告';
    const saved=String(getSource(section,deviceNow,j)[f.key]??'');
    if(section!=='arming'&&saved!==''&&report.comparable&&report.current&&!report.olderThanRequest)
      detail+=report.value===saved?' · 与基站保存值一致':' · 与基站保存值不同';
    if(section==='arming'&&report.value==='0')detail+=' · 报告为撤防档位';
    el.textContent=`${source}：${text} · ${age}；${detail}。`;
  }
  stableHTML($('prerequisite'),section==='pir'&&!deviceNow.policy.managed?'<div class="info warning">当前为手动诊断模式。本页可保存，但下发需要基站托管布防；保存不会自动启用托管。请先选择布防策略，再重新下发本组。<div class="row" style="margin-top:10px"><button data-action="goto-arming">前往布防与休眠</button></div></div>':'');
}
async function saveSettings(ev) {
  ev.preventDefault();if(busy)return;
  const group=section,g=groups[group],d=device(),j=cache(),draft=getDraft(group,d,j);
  if(!draft.dirty.size)return;
  if(draft.conflicts.size&&!confirm('基站上的部分配置已被其他请求改变。是否用当前草稿覆盖这些字段？'))return;
  const sent=Object.fromEntries([...draft.dirty].filter(k=>!configurationFieldLocked(group,k,draft.values,d.policy)).map(k=>[k,draft.values[k]]));
  if(!Object.keys(sent).length){notice('当前更改的字段由工作模式控制，不需要单独保存。');return;}
  let command;
  if(g.kind==='profile') {
    try {command=cameraCommand(d,'name',nameArgument(draft.values.name),d.profile.revision);}
    catch(e){notice(e.message,true);return;}
  } else if(g.kind==='pir') {
    const v=draft.values;if(!v.mode){notice('请选择 PIR 工作模式。',true);return;}
    if(v.mode==='custom'&&['sensitivity','wait','record'].some(k=>v[k]==='')){notice('请填写完整的自定义 PIR 参数。',true);return;}
    command=cameraCommand(d,'configure-pir',v.mode+(v.mode==='custom'?` ${v.sensitivity} ${v.wait} ${v.record}`:''),j.desired.revision);
  } else {
    if(Object.values(sent).some(v=>v==='')){notice('请选择有效值；清空输入不会删除基站配置。',true);return;}
    command=cameraCommand(d,g.kind==='policy'?'policy-config':'configure-many',Object.entries(sent).map(([k,v])=>`${k}=${v}`).join(' '),g.kind==='policy'?d.policy.revision:j.desired.revision);
  }
  busy=true;settingsSeq++;syncForm();let saved=false;
  try {
    const out=await api(command);saved=true;commitDraft(draft,sent);
    if(out.desired) {
      j.desired=out.desired;
      if(g.kind==='pir') {const source=getSource(group,d,j);Object.assign(draft.base,Object.fromEntries(Object.entries(source).map(([k,v])=>[k,String(v)])));Object.assign(draft.values,draft.base);draft.dirty.clear();draft.conflicts.clear();}
    }
    if(out.policy)d.policy=out.policy;
    if(out.profile)d.profile=out.profile;
    notice(g.kind==='profile'?'摄像头名称已保存到基站。':`${cameraName(d)}：配置已保存到基站。${g.kind==='policy'?(d.policy.managed?'布防状态将由策略计算；回显不代表设备已执行。':'已取消布防托管，不能据此判断摄像头是否撤防。'):out.desired?.items?.['pir-mode']?.phase==='managed_policy_required'&&g.kind==='pir'?'请先启用基站布防策略，再重新下发本组。':'等待下发或摄像头回显，尚不能据此确认实际效果。'}`);
    await refreshAfterWrite();
  } catch(e) {if(e.code==='configuration_conflict')await refreshAfterWrite().catch(()=>{});notice(saved?'基站已保存，但刷新状态失败：'+e.message:e.message,true);}
  finally {busy=false;if($('settingsForm')){syncForm();updateDelivery();}updateRail();}
}
function updateDelivery() {
  if(!$('delivery'))return;
  const j=cache(),d=device(),g=groups[section];
  if(!j||!d)return;
  const policy=g.kind==='policy',keys=groupSettingKeys(section,g.fields||[],j.desired);
  const blocked=section==='pir'&&!d.policy.managed;
  const rows=keys.map(k=>{
    const item=j.desired.items?.[k],phase=item?.phase||'explicit_apply_required';
    const label=k==='pir-mode'?'PIR 工作模式':g.fields.find(f=>f.key===k).label;
    const history=item?.operation_id&&!item.current_connection?' · 历史连接':'';
    return `<li><div><strong>${esc(label)}</strong><span>${esc(deliveryLabel(phase)+history)}</span></div><button data-setting-retry="${k}" aria-label="重新下发${esc(label)}" ${busy||blocked?'disabled':''}>重新下发</button></li>`;
  }).join('');
  stableHTML($('delivery'),`<h3>${policy?'布防下发状态':'本组下发状态'}${policy?` <span class="badge">${esc(deliveryLabel(d.policy.delivery))}</span>`:''}</h3><p class="hint">${policy?'策略保存后会重新计算目标状态。': '只展示本组已保存项；重新下发会发送基站保存值。'} 回显及配置报告不证明硬件实际生效，报告也不是实时查询。</p>${policy?`<p class="hint">${!d.policy.managed?'未启用布防托管；不能据此判断摄像头当前是否布防。':`当前目标：${d.policy.desired_armed?'布防，档位 '+d.policy.level:'撤防，档位 0'}。`}${d.policy.level_source==='work_mode'?'灵敏度跟随工作模式；撤防为 0，重新布防恢复模式档位。':'尚未保存工作模式，使用兼容布防档位。'}</p>`:`<ul class="delivery-items">${rows||'<li class="hint">本组尚无已保存设置。</li>'}</ul>`}<div class="row" style="margin-top:14px"><button data-action="${policy?'retry-arm':'retry-settings'}" ${busy||(!policy&&(!keys.length||blocked))?'disabled':''}>${policy?'重试布防下发':'重新下发本组保存值'}</button></div>`);
  if($('settingsJson'))$('settingsJson').textContent=JSON.stringify(policy?d.policy:j,null,2);
}
function reportAge(report) {
  if(!report || report.age_ms<0)return '尚未收到报告';
  return `${report.current_connection?'本次连接':'历史报告'} · ${Math.floor(report.age_ms/1000)} 秒前`;
}
function updateDeviceInfo() {
  if(!$('deviceInfo')||!device())return;
  const d=device(),r=d.identity_report;
  stableHTML($('deviceInfo'),`<dl class="kv"><dt>序列号</dt><dd>${esc(d.serial)}</dd><dt>摄像头商品型号</dt><dd>${esc(r?.retail_model||'尚未确认')}</dd><dt>内部硬件标识</dt><dd>${esc(r?.hardware||'尚无有效报告')}</dd><dt>摄像头固件版本</dt><dd>${esc(r?.firmware||'尚无有效报告')}</dd><dt>MCU 固件版本</dt><dd>${esc(r?.mcu_firmware||'尚无有效报告')}</dd><dt>信息来源</dt><dd>${r?.seen?'摄像头上线报告 · '+esc(reportAge(r)):'尚未收到摄像头上线报告'}</dd><dt>网络地址</dt><dd>${esc(d.ip)}</dd></dl><p class="hint">内部硬件标识不等同于商品型号。断线后的版本信息保留为历史报告；仅心跳重连不会把它标为新报告。</p>`);
}
function renderDetection() {
  if(!$('detectionReport'))return;
  const d=device(),r=cache()?.settings.reports.algorithm_203,v=r?.values;
  const flag=n=>n===0?'关闭（报告值 0）':n===1?'开启（报告值 1）':n===undefined?'尚无报告':`未知值 ${n}`;
  const event=d.last_detection;
  stableHTML($('detectionReport'),`<h2>检测配置报告</h2><dl class="kv"><dt>检测框参数</dt><dd>${esc(flag(v?.pd_rect_raw))}</dd><dt>报告时间</dt><dd>${esc(reportAge(r))}</dd></dl><details><summary>原始算法配置报告</summary><pre>${esc(JSON.stringify(r??null,null,2))}</pre></details><h3>最近检测报告</h3><p>${event?esc(reportAge(event)):'尚未收到检测报告'}</p>${event?`<dl class="kv"><dt>结果原始值</dt><dd>${event.result_raw}</dd><dt>类型原始值</dt><dd>${event.type_raw}</dd></dl>`:''}<p class="hint">报告不直接视为一次新的移动报警。手机和邮件通知尚未提供。</p>`);
}
function renderStreams() {
  const d=device();$('content').innerHTML=`<section class="card"><h2>流地址与设备</h2>${streamInfo(d)}<dl class="kv"><dt>设备序列号</dt><dd>${esc(d.serial)}</dd><dt>摄像头地址</dt><dd>${esc(d.ip)}</dd><dt>关联方式</dt><dd>${d.association==='neighbor_mac'?'邻居表 MAC 关联':'主机模拟 · IP 关联'}</dd><dt>设备状态</dt><dd id="deviceState">${esc(stateLabels[d.state]||d.state)}</dd></dl><div class="row"><button data-action="wake">唤醒</button><button data-action="sleep">请求休眠</button></div></section><section class="card"><h3>移除配对记录</h3><p class="hint">移除本基站的设备记录及已保存设置。摄像头不会恢复出厂设置；重新使用时需要再次配对。</p><div class="row" style="margin-top:16px"><button data-action="forget" class="danger">移除这台摄像头</button></div></section>`;
}
function streamInfo(d) {
  if(management)return '<div class="info">摄像头媒体由管理服务器转发。</div>';
  if(!state.rtsp.enabled)return '<div class="info warning">RTSP 服务尚未启用，请在基站部署配置中启用后重启服务。</div>';
  const url=`rtsp://${location.hostname}:${state.rtsp.port}/camera/${d.slot}/1`;
  return `<p class="hint">使用 VLC、ffplay 或 NVR 打开主视频流：</p><code class="stream-address">${esc(url)}</code><p class="hint">${state.rtsp.authentication?'使用部署时配置的 RTSP 用户名和密码。':'RTSP 当前未配置认证。'} 当前为 H.264 视频，不含音轨。</p>`;
}
function renderLive() {
  const d=device();$('content').innerHTML=`<div class="watch-grid"><section class="card"><div class="watch-stage">${cameraIcon}<div><h2>通过本地播放器观看</h2><p class="hint">将下方 RTSP 地址加入 VLC 或 NVR。浏览器内实时播放尚未提供。</p></div><span class="badge" id="liveState">${esc(stateLabels[d.state]||d.state)}</span></div><div style="margin-top:20px">${streamInfo(d)}</div></section><section class="card"><div class="form-head"><h2>${esc(cameraName(d))}</h2><p>${esc(d.serial)}</p></div><dl class="kv"><dt>观看请求</dt><dd id="liveRequested"></dd><dt>已收视频帧</dt><dd id="frameCount"></dd><dt>基站录像</dt><dd id="recordState"></dd></dl><div class="row"><button data-action="live" class="primary">开始观看</button><button data-action="stop">停止观看请求</button><button data-action="wake">唤醒</button><button data-action="sleep">休眠</button></div><div class="info">RTSP 客户端连接也会自动请求视频。停止这里的观看请求不会断开仍在观看的 RTSP 客户端。</div><div class="row"><button data-action="snapshot">下载单帧</button><button data-action="record-start">保存到基站</button><button data-action="record-stop">结束基站录像</button></div><p class="hint" style="margin-top:15px">录像保存到基站存储，完成后可到录像页下载。单帧为 H.264 关键帧数据，可用随附媒体工具转换为 JPEG。</p></section></div>`;updateStatusPanels();
}
function planState() {
  const id=identity(device());if(!planDrafts.has(id))planDrafts.set(id,{index:'0',days:'12345',start:'09:00:00',end:'18:00:00',allDay:false,dirty:false});return planDrafts.get(id);
}
const timeOf=s=>[Math.floor(s/3600),Math.floor(s/60)%60,s%60].map(v=>String(v).padStart(2,'0')).join(':');
const daysText=mask=>['日','一','二','三','四','五','六'].filter((_,i)=>mask&(1<<i)).map(s=>'周'+s).join('、');
function renderSchedule() {
  const p=planState();
  $('content').innerHTML=`<section class="card"><div class="form-head"><h2>每周布防计划</h2><p>开始时刻包含、结束时刻不包含；跨午夜的时段归属于开始日。</p></div><div id="scheduleStatus" class="info"></div><div class="table-wrap"><table><thead><tr><th>编号</th><th>重复</th><th>时段</th><th>操作</th></tr></thead><tbody id="plans"></tbody></table></div><form class="plan-editor" id="planForm"><h3>新建 / 编辑条目</h3><div class="row"><label>条目编号<select id="planIndex">${Array.from({length:10},(_,i)=>`<option value="${i}" ${String(i)===p.index?'selected':''}>${i+1}</option>`).join('')}</select></label><label>开始<input type="time" step="1" id="planStart" value="${p.start}" required></label><label>结束<input type="time" step="1" id="planEnd" value="${p.end}" required></label></div><div class="weekdays">${['日','一','二','三','四','五','六'].map((s,i)=>`<label><input type="checkbox" data-day="${i}" ${p.days.includes(String(i))?'checked':''}>周${s}</label>`).join('')}<label><input type="checkbox" id="planAllDay" ${p.allDay?'checked':''}>全天</label></div><p id="planHint" class="hint"></p><div class="row end"><button type="button" data-action="plan-discard">取消草稿</button><button type="submit" class="primary">保存条目</button></div></form><p class="hint" style="margin-top:18px">计划时区和布防模式在“布防与休眠”设置；保存时段不会自动切换到计划布防。</p></section>`;
  const update=()=>{p.index=$('planIndex').value;p.start=$('planStart').value;p.end=$('planEnd').value;p.allDay=$('planAllDay').checked;p.days=[...document.querySelectorAll('[data-day]:checked')].map(e=>e.dataset.day).join('');p.dirty=true;syncPlanEditor();updateRail();};
  $('planForm').oninput=update;$('planForm').onchange=update;
  $('planForm').onsubmit=async e=>{
    e.preventDefault();if(busy)return;const d=device(),sent={...p};if(!sent.days){notice('请至少选择一个重复日。',true);return;}
    if(d.policy.plans.some(x=>String(x.index)===sent.index)&&!confirm(`替换计划条目 ${Number(sent.index)+1}？`))return;
    const hm=s=>s.length===5?s+':00':s;
    if(await action(cameraCommand(d,'plan',sent.allDay?`all-day ${sent.index} ${sent.days}`:`set ${sent.index} ${sent.days} ${hm(sent.start)} ${hm(sent.end)}`,d.policy.revision),'每周计划条目已保存。')) {
      if(['index','days','start','end','allDay'].every(k=>p[k]===sent[k]))p.dirty=false;
      if(identity(device())===identity(d)){syncPlanEditor();updateRail();}
    }
  };
  syncPlanEditor();updateSchedule();
}
function syncPlanEditor() {
  if(!$('planHint'))return;const p=planState();
  for(const el of $('planForm').querySelectorAll('input,select,button'))el.disabled=busy;
  $('planStart').disabled=$('planEnd').disabled=busy||p.allDay;
  $('planHint').textContent=busy?'正在保存并刷新计划…':p.dirty?'此条目有未保存草稿。':'选择条目编号后保存；已有同编号条目会被替换，停用状态保持不变。';
}
function updateSchedule() {
  if(!$('plans'))return;const p=device().policy;
  $('scheduleStatus').textContent=`${p.clock_valid?'时钟已确认':'时钟未确认，计划保持撤防'} · UTC ${p.utc_offset_minutes>=0?'+':''}${p.utc_offset_minutes/60} · 当前${p.managed&&p.mode==='schedule'?'使用每周计划':'未使用每周计划'}`;
  stableHTML($('plans'),p.plans.length?p.plans.map(q=>`<tr><td>${q.index+1}${q.enabled===false?' · 已停用':''}</td><td>${daysText(q.days_mask)}</td><td>${q.all_day?'全天':timeOf(q.start_seconds)+' – '+timeOf(q.end_seconds)+(q.start_seconds>q.end_seconds?'（次日）':'')}</td><td><div class="row">${q.enabled===undefined?'':`<button data-plan-toggle="${q.index}" data-enabled="${q.enabled?0:1}">${q.enabled?'停用':'启用'}</button>`}<button data-plan-edit="${q.index}">编辑</button><button data-plan-delete="${q.index}" class="danger">删除</button></div></td></tr>`).join(''):'<tr><td colspan="4" class="empty">暂无计划。添加时段后，在布防策略中选择“按每周计划”。</td></tr>');
  for(const el of $('plans').querySelectorAll('button'))el.disabled=busy;
  syncPlanEditor();
}
function renderRecordings() {
  $('content').innerHTML='<section class="card"><div class="section-heading" style="margin-top:0"><div><h2>基站录像</h2><p class="hint">PIR 事件短片和手动保存到基站的片段，按时间倒序。</p></div><button data-action="records-refresh">刷新列表</button></div><div class="row"><button data-action="record-start" class="primary">手动保存到基站</button><button data-action="record-stop">结束基站录像</button><span id="recordState" class="badge"></span></div><div class="table-wrap"><table><thead><tr><th>时间（浏览器时区）</th><th>来源</th><th>大小 / 帧数</th><th>结束原因</th><th>操作</th></tr></thead><tbody id="records"><tr><td colspan="5" class="empty">正在读取…</td></tr></tbody></table></div><div class="pagination"><button data-action="records-prev">上一页</button><span id="recordPage"></span><button data-action="records-next">下一页</button></div><p class="hint" style="margin-top:20px">下载为 H.264 视频，目前不含音轨。配额满后停止新录像；下载后可删除不需要的文件。</p></section>';
  updateStatusPanels();loadRecords().catch(e=>notice(e.message,true));
}
async function loadRecords() {
  const d=device(),target=$('records');if(!d||!target)return;
  const gen=session,p=recordsPage,id=identity(d);
  if(recordsRequest?.seq===recordsSeq&&recordsRequest.gen===gen&&recordsRequest.id===id&&recordsRequest.page===p&&recordsRequest.target===target)return;
  const seq=++recordsSeq,ticket={seq,gen,id,page:p,target};recordsRequest=ticket;
  try {
  const j=await api(cameraCommand(d,'record-list',String(p)));
  if(seq!==recordsSeq||gen!==session||id!==identity(device())||$('records')!==target)return;
  const last=Math.max(0,Math.ceil(j.total/16)-1);
  if(p>last){recordsPage=last;return loadRecords();}
  $('recordPage').textContent=`第 ${p+1} 页 · 共 ${j.total} 段`;
  document.querySelector('[data-action="records-prev"]').disabled=p===0;
  document.querySelector('[data-action="records-next"]').disabled=p>=7||(p+1)*16>=j.total;
  stableHTML($('records'),j.records.length?j.records.map(r=>`<tr><td>${esc(new Date(r.unix_seconds*1000).toLocaleString())}</td><td>${esc(recordingTrigger(r.trigger))}</td><td>${fmtBytes(r.bytes)} / ${r.frames}</td><td>${esc(recordingEnd(r.end_reason))}</td><td><div class="row"><button data-record-download="${r.id}">下载</button><button data-record-delete="${r.id}" class="danger">删除</button></div></td></tr>`).join(''):'<tr><td colspan="5" class="empty">这一页没有录像。</td></tr>');
  } catch(e) {if(seq===recordsSeq&&gen===session&&$('records')===target)throw e;}
  finally {if(recordsRequest===ticket)recordsRequest=null;}
}
function renderStation() {
  if(stationSection==='pairing')$('content').innerHTML=`<section class="card"><div class="form-head"><h2>摄像头配对</h2><p>最多支持 4 台摄像头，统一连接到基站专用 AP。</p></div><dl class="kv"><dt>摄像头 AP</dt><dd>${esc(state.network.ssid)}</dd><dt>AP 管理</dt><dd>${state.ap_managed?'已配置 AP 管理助手':'未配置 AP 管理助手'}</dd><dt>配对状态</dt><dd id="pairState"></dd></dl>${!state.ap_managed?'<div class="info warning">当前只能打开协议配对窗口，不能自动创建配对热点。OpenWrt 部署时需要启用随附的 AP 管理助手。</div>':''}<div class="row"><button data-action="pair-open" class="primary" ${management&&apiClient.user.role<=1?'disabled':''}>打开配对窗口</button></div><p class="hint" style="margin-top:18px">打开 120 秒配对窗口后，让摄像头进入配对模式。进度和倒计时将在对话框中显示；有正在进行的配对时，此入口查看当前进度。</p></section>`;
  else if(stationSection==='storage')$('content').innerHTML='<section class="card"><h2>基站录像存储</h2><div id="storageStats"></div><div class="info">录像保存在基站配置的目录中。配额限制针对录像文件，不代表磁盘剩余空间；磁盘不足也会使录像失败。</div><h3>清理孤立文件</h3><p class="hint">清理未完成、未提交的录像残留。仅在没有录像或下载时执行；已提交录像请到录像页逐段删除。</p><div class="row" style="margin-top:18px"><button data-action="storage-cleanup">清理录像残留</button></div></section>';
  else if(stationSection==='network')$('content').innerHTML=`<section class="card"><h2>网络与访问</h2><p class="muted">${management?'管理网页由服务器提供，基站负责连接摄像头与服务器。':'这里显示服务实际监听配置。'}</p><dl class="kv"><dt>摄像头侧监听</dt><dd>${esc(state.network.camera_bind)}</dd><dt>摄像头 AP 名称</dt><dd>${esc(state.network.ssid)}</dd><dt>控制 / 媒体 / 保活</dt><dd>${state.ports.join(' / ')}</dd>${management?`<dt>当前管理服务器</dt><dd>${esc(apiClient.baseUrl||location.origin)}</dd><dt>当前基站</dt><dd>${esc(apiClient.stationId)}</dd><dt>服务器连接</dt><dd>由基站主动发起加密 UDP 连接；地址与端口在部署配置中设置。</dd>`:`<dt>管理服务</dt><dd>${esc(state.http.bind)}:${state.http.port} · ${state.http.authentication?'管理密钥认证':'未启用认证'}</dd><dt>RTSP 服务</dt><dd>${state.rtsp.enabled?esc(state.rtsp.bind)+':'+state.rtsp.port:'未启用'} · ${state.rtsp.authentication?'账号认证':'未配置账号认证'}</dd>`}</dl><div class="info">AP、地址、端口和访问凭据由 OpenWrt 的 vava-agent 配置管理。网页当前只读展示这些项目。</div></section>`;
  else if(stationSection==='time')$('content').innerHTML='<section class="card"><h2>基站时间</h2><dl class="kv"><dt>基站系统时间</dt><dd id="stationTime"></dd><dt>时钟可信状态</dt><dd id="clockState"></dd><dt>服务运行时间</dt><dd id="uptime"></dd></dl><div class="info warning">“确认时钟”只标记当前时钟可信，不会设置系统时间。先通过 OpenWrt 的 NTP 校准；网页时间按当前浏览器时区显示。每台摄像头的计划另有 UTC 偏移。</div><div class="row"><button data-action="clock-valid" class="primary">确认时钟已校准</button><button data-action="clock-invalid">标记时钟不可信</button></div></section>';
  else $('content').innerHTML='<section class="card"><h2>服务诊断</h2><p class="hint">状态中不包含管理密钥或 AP 密码。设备序列号和内网地址仍属于你的设备信息。</p><div id="resourceStats"></div><details open><summary>基站状态 JSON</summary><pre id="stateJson"></pre></details></section>';
  updateStatusPanels();
}
function updateStatusPanels() {
  if(!state || (management&&management.pages.includes(managedPage())))return;
  if(page==='overview'){renderOverview();return;}
  updateDeviceInfo();
  const d=device(),s=state.storage;
  const text=(id,value)=>{if($(id))$(id).textContent=value;};
  if(d) {text('liveRequested',d.live_requested?'已请求':'未请求');text('frameCount',d.video_frames);text('liveState',stateLabels[d.state]||d.state);text('deviceState',stateLabels[d.state]||d.state);text('recordState',d.recording.failed?'录像失败：'+(d.recording.reason||'写盘失败'):d.recording.active?'正在录像 · '+d.recording.frames+' 帧':d.recording.finishing?'正在完成写盘…':'未录制');}
  text('pairState',pairingLabels[state.pairing?.phase]||(state.pair_window_ms>0?'配对窗口已开启':'配对窗口已关闭'));

  text('stationTime',new Date(state.unix_seconds*1000).toLocaleString());text('clockState',state.clock_valid?'已标记为可信':'尚未确认');text('uptime',Math.floor(state.uptime_seconds/3600)+' 小时 '+Math.floor(state.uptime_seconds%3600/60)+' 分钟');
  text('stateJson',JSON.stringify(state,null,2));
  if($('storageStats'))$('storageStats').innerHTML=`<dl class="kv">${management?`<dt>TF 存储</dt><dd>${s.available?'可用':'不可用 · 配对与控制继续运行'}</dd>${s.error?`<dt>存储状态</dt><dd>${esc(s.error)}</dd>`:''}`:''}<dt>已用 / 配额</dt><dd>${fmtBytes(s.bytes)} / ${fmtBytes(s.quota_bytes)}</dd><dt>已提交录像</dt><dd>${s.records} 段</dd>${management?`<dt>新录像加密</dt><dd>${s.encrypted?'AES-256-GCM · 已启用':'未启用'}</dd>${s.encrypted?`<dt>当前密钥编号</dt><dd>${esc(s.key_id)}</dd>`:''}`:''}<dt>正在写入</dt><dd>${s.active} 路 · 队列 ${fmtBytes(s.queued_bytes)}</dd></dl><div class="meter"><span style="width:${Math.min(100,s.bytes/s.quota_bytes*100)}%"></span></div>`;
  if($('resourceStats'))$('resourceStats').innerHTML=`<dl class="kv"><dt>连接数量</dt><dd>${state.resources.clients} / 32</dd><dt>输出队列内存</dt><dd>${fmtBytes(state.resources.tx_allocated)}</dd><dt>媒体缓冲内存</dt><dd>${fmtBytes(state.resources.media_allocated)}</dd></dl>`;
  updateSchedule();updateDelivery();
  if(page==='recordings')loadRecords().catch(e=>notice(e.message,true));
}
async function action(command,message) {
  if(busy)return false;busy=true;settingsSeq++;let saved=false;const gen=session;
  updateSchedule();
  try {const j=await api(command);saved=true;if(gen!==session)return false;notice(message||'请求已接受。');await refreshAfterWrite();return j;}
  catch(e){if(e.code==='configuration_conflict')await refreshAfterWrite().catch(()=>{});if(gen===session)notice((saved?'请求已接受，但刷新失败：':'')+e.message,true);return saved;}
  finally {busy=false;if($('settingsForm')){syncForm();updateDelivery();}updateSchedule();}
}
async function download(command,name,label) {
  if(downloadJob){notice('已有下载任务，请完成或取消后再下载。',true);return;}
  const ctl=new AbortController(),job={controller:ctl,name,label};downloadJob=job;
  let idleTimer,timedOut=false;
  const resetDeadline=()=>{clearTimeout(idleTimer);idleTimer=setTimeout(()=>{timedOut=true;ctl.abort();},30000);};
  const progress=(received,total)=>{
    resetDeadline();$('downloadLabel').textContent=label+' · '+name;
    $('downloadSize').textContent=fmtBytes(received)+(total===null?'':(' / '+fmtBytes(total)));
    if(total){$('downloadProgress').max=total;$('downloadProgress').value=received;}
    else $('downloadProgress').removeAttribute('value');
  };
  notice('');$('downloadPanel').hidden=false;progress(0,null);
  try {
    const r=await apiClient.commandLineRaw(command,{signal:ctl.signal});
    if(r.status===401)throw Error('管理密钥无效或已更换。');
    if(!r.ok||r.headers.get('content-type')!=='video/h264') {
      const j=await r.json();throw Error(errors[j.error]||j.error||'未返回媒体数据。');
    }
    const length=r.headers.get('content-length'), expected=length===null?null:Number(length);
    progress(0,expected);
    const blob=await collectMedia(r.body.getReader(),expected,64*1024*1024,progress,ctl.signal);
    const url=URL.createObjectURL(blob),a=document.createElement('a');a.href=url;a.download=name;a.click();
    setTimeout(()=>URL.revokeObjectURL(url),30000);notice(label+(expected===null?'：数据已接收，已交给浏览器保存。':'：接收长度校验通过，已交给浏览器保存。'));
  } catch(e) {notice(timedOut?'下载连续 30 秒没有收到数据，已取消，请重试。':ctl.signal.aborted?'下载已取消。':e.message,!ctl.signal.aborted||timedOut);}
  finally {clearTimeout(idleTimer);if(downloadJob===job){downloadJob=null;$('downloadPanel').hidden=true;}}
}
function goto(p,sub) {const hash='#'+p+(sub?'/'+sub:'');if(location.hash===hash)route();else location.hash=hash;}
function route() {
  let [p,sub]=location.hash.slice(1).split('/');
  if(management&&(p==='nas-settings'||p==='nas-policy')){sub=p==='nas-settings'?'nas':'nas-backup';p='station';history.replaceState(null,'','#station/'+sub);}
  if(management&&p==='live'){p='overview';history.replaceState(null,'','#overview');}
  page=own(names,p)?p:'overview';
  if(page==='camera'&&own(groups,sub))section=sub;
  if(page==='station'&&(own(stationGroups,sub)||(management&&sub==='nas-backup')))stationSection=sub;
  $('search').value='';recordsSeq++;render();if(page==='camera'&&key)loadSettings().catch(e=>notice(e.message,true));
}
$('content').addEventListener('click',async e=>{
  const b=e.target.closest('button');if(!b||b.disabled)return;
  try {
    if(b.dataset.open){selected=b.dataset.slot;settingsSeq++;recordsSeq++;recordsPage=0;updateShell();if(management&&b.dataset.open==='live'){await openLiveDialog(device(),b);return;}goto(b.dataset.open);await loadSettings();return;}
    const d=device();
    if(b.dataset.settingRetry){await action(cameraCommand(d,'apply-settings',b.dataset.settingRetry,cache().desired.revision),'已请求重新下发所选设置。');return;}
    if(b.dataset.planEdit!==undefined){const p=planState();if(p.dirty&&!confirm('丢弃当前计划草稿，读取这个条目？'))return;const q=d.policy.plans.find(q=>q.index===Number(b.dataset.planEdit));if(!q)return;Object.assign(p,{index:String(q.index),days:Array.from({length:7},(_,i)=>i).filter(i=>q.days_mask&(1<<i)).join(''),start:timeOf(q.start_seconds),end:timeOf(q.end_seconds),allDay:q.all_day,dirty:false});renderSchedule();updateRail();return;}
    if(b.dataset.planToggle!==undefined){await action(cameraCommand(d,'plan',`enable ${b.dataset.planToggle} ${b.dataset.enabled}`,d.policy.revision),b.dataset.enabled==='1'?'计划已启用。':'计划已停用，时段仍保留。');return;}
    if(b.dataset.planDelete!==undefined){if(confirm('删除这个每周计划条目？'))await action(cameraCommand(d,'plan','delete '+b.dataset.planDelete,d.policy.revision),'计划条目已删除。');return;}
    if(b.dataset.recordDownload){await download(cameraCommand(d,'record-get',b.dataset.recordDownload),b.dataset.recordDownload+'.h264',`摄像头 ${d.slot+1} · ${d.serial}`);return;}
    if(b.dataset.recordDelete){if(confirm('永久删除这段本地录像？')){await action(cameraCommand(d,'record-delete',b.dataset.recordDelete),'录像已删除。');await loadRecords();}return;}
    const a=b.dataset.action;
    if(a==='goto-pair'){goto('station','pairing');return;}
    if(a==='goto-arming'){goto('camera','arming');return;}
    if(a==='retry-settings'){const keys=groupSettingKeys(section,groups[section].fields,cache().desired);if(keys.length)await action(cameraCommand(d,'apply-settings',keys.join(' '),cache().desired.revision),'已请求重新下发本组保存值。');return;}
    if(a==='discard'){const draft=getDraft(section);discardDraft(draft,getSource(section,device(),cache()));notice('');renderCamera();updateRail();return;}
    if(a==='plan-discard'){planDrafts.delete(identity(d));notice('');renderSchedule();updateRail();return;}
    if(a==='records-refresh'){await loadRecords();return;}
    if(a==='records-prev'||a==='records-next'){recordsPage=Math.max(0,Math.min(7,recordsPage+(a==='records-prev'?-1:1)));await loadRecords();return;}
    if(a==='snapshot'){await download(cameraCommand(d,'snapshot'),`camera-${d.slot+1}-snapshot.h264`,`摄像头 ${d.slot+1} · ${d.serial}`);return;}
    if(a==='pair-open'){showPairingDialog();return;}
    if(a==='clock-valid'){if(confirm('已通过 NTP 或其他方式核对基站时间？此操作不会修改系统时钟。'))await action('clock-valid 1','基站时钟已标记为可信。');return;}
    if(a==='clock-invalid'){await action('clock-valid 0','时钟已标记为不可信，计划模式将保持撤防。');return;}
    if(a==='storage-cleanup'){if(confirm('清理未提交的录像残留文件？')){const j=await action('storage-cleanup','清理完成。');if(j&&typeof j==='object')notice('已清理 '+fmtBytes(j.removed_bytes)+' 残留文件。');}return;}
    if(a==='forget'){if(confirm(`移除 ${cameraName(d)}（${d.serial}）及其基站设置？`)){if(await action(cameraCommand(d,'forget'),'摄像头配对记录已移除。'))goto('overview');}return;}
    const cmds={'retry-arm':['arm','retry','已请求重试布防下发。'],'record-start':['record','start','已请求保存到基站，等待视频帧。'],'record-stop':['record','stop','已请求结束录像。']};
    if(cmds[a]){const [cmd,tail,msg]=cmds[a];await action(cameraCommand(d,cmd,tail),msg);if(page==='recordings')await loadRecords();return;}
    if(['live','stop','wake','sleep'].includes(a))await action(cameraCommand(d,a),'摄像头 '+(d.slot+1)+'：'+({live:'已请求观看。',stop:'已停止手动观看请求。',wake:'已请求唤醒。',sleep:'已请求休眠。'}[a]));
  } catch(err){notice(err.message,true);}
});
$('navigation').onclick=e=>{const b=e.target.closest('[data-page]');if(b)goto(b.dataset.page);};
$('sections').onclick=e=>{const b=e.target.closest('[data-section]');if(!b)return;goto(page,b.dataset.section);};
$('search').oninput=updateRail;
$('cameraSelect').onchange=async()=>{selected=$('cameraSelect').value;settingsSeq++;recordsSeq++;recordsPage=0;render();try{await loadSettings();}catch(e){notice(e.message,true);}};
$('refresh').onclick=async()=>{const button=$('refresh');button.disabled=true;try{await refresh();
  if(management){if(['recordings','cloud-storage','legacy-recordings','events','tasks'].includes(page))render();}
  else if(page==='recordings')await loadRecords();
  notice(baseOnline===false?'基站离线；服务器中的云视频与事件仍可查看。':'状态已刷新。当前观看与未提交的设置已保留。');
}catch(e){notice(e.message,true);}finally{button.disabled=false;}};
async function connect(candidate,base=$('apiBase').value,resume=false) {
  key=candidate;session++;notice('');$('connect').disabled=true;
  try {
    if(management&&!resume){const login=await management.login(base,$('username').value,candidate);key=login.token;}
    apiClient=(management||VavaAPI).createClient({baseUrl:base,token:key});
    const capabilities=await apiClient.capabilities({signal:AbortSignal.timeout(8000)});
    if(capabilities.api_version!=='1')throw Error('API 版本不兼容。');
    try{await refresh();}catch(e){if(!management)throw e;notice(e.message,true);}
    if(!state&&!management)throw Error('尚未读到基站状态。');
    sessionStorage.setItem('vavaConnection',JSON.stringify({base:apiClient.baseUrl,token:key}));$('token').value='';$('login').hidden=true;document.body.classList.add('authenticated');$('workspace').hidden=false;$('logout').hidden=false;$('navigation').hidden=false;route();
  } catch(e){key='';apiClient=null;sessionStorage.removeItem('vavaConnection');notice(e.message,true);$('connection').textContent='未连接';$('connection').className='badge';}
  finally{$('connect').disabled=false;}
}
$('loginForm').onsubmit=e=>{e.preventDefault();connect(management?$('token').value:$('token').value.trim());};
$('logout').onclick=async()=>{if((dirtyTotal()||downloadJob)&&!confirm('退出会清除未保存草稿并取消下载，是否继续？'))return;downloadJob?.controller.abort();if(management)try{await management.finishLocalRecording();management.invalidate();await apiClient.logout();}catch(e){notice(e.message,true);return;}session++;key='';apiClient=null;sessionStorage.removeItem('vavaConnection');drafts.clear();planDrafts.clear();location.reload();};
if(management)$('stationSelect').onchange=async()=>{if(dirtyTotal()&&!confirm('切换基站会清除未保存草稿，是否继续？')){$('stationSelect').value=apiClient.stationId;return;}const id=$('stationSelect').value;$('stationSelect').disabled=true;try{await management.finishLocalRecording();management.invalidate();apiClient.select(id);location.reload();}catch(e){$('stationSelect').disabled=false;$('stationSelect').value=apiClient.stationId;notice(e.message,true);}};
$('downloadCancel').onclick=()=>downloadJob?.controller.abort();
$('theme').onclick=()=>{const t=document.documentElement.dataset.theme==='dark'?'light':'dark';document.documentElement.dataset.theme=t;localStorage.setItem('vavaTheme',t);};
const theme=localStorage.getItem('vavaTheme');if(theme==='light'||theme==='dark')document.documentElement.dataset.theme=theme;
window.addEventListener('hashchange',route);
window.addEventListener('beforeunload',e=>{if(dirtyTotal()||downloadJob||management?.hasLocalRecording()){e.preventDefault();e.returnValue='';}});
setInterval(()=>{if(key&&!busy&&!pairingDialog)refresh().catch(e=>{$('connection').textContent=management?'服务器连接异常':'连接异常';$('connection').className='badge warn';if(e.message!==lastRefreshError){lastRefreshError=e.message;notice(e.message,true);}});},5000);
sessionStorage.removeItem('vavaKey'); // Old credentials had no station address association.
$('apiBase').value=globalThis.VAVA_CONFIG?.apiBase||'';
try {const saved=JSON.parse(sessionStorage.getItem('vavaConnection')||'null');if(saved&&typeof saved.base==='string'&&typeof saved.token==='string'){ $('apiBase').value=saved.base;connect(saved.token,saved.base,true); }}
catch {sessionStorage.removeItem('vavaConnection');}
})();
