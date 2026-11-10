/* VAVA HTTP API v1 client. No DOM or framework dependency; browser + Node. */
(() => {
'use strict';
function normalizeBase(value='') {
  if(typeof value!=='string')throw Error('基站地址必须是字符串。');
  value=value.trim();
  if(!value)return '';
  if(value.startsWith('/')&&!value.startsWith('//')) {
    if(/[?#\\\s]/.test(value)||value.split('/').some(x=>x==='.'||x==='..'))throw Error('基站地址不能包含查询参数、片段或相对路径。');
    return value.replace(/\/+$/,'');
  }
  const url=new URL(value);
  if(!['http:','https:'].includes(url.protocol)||url.username||url.password||url.search||url.hash)
    throw Error('请输入 HTTP 或 HTTPS 基站地址，不要在地址中包含密钥。');
  return url.href.replace(/\/+$/,'');
}
function envelope(command,args=[],guards={}) {
  if(typeof command!=='string'||!Array.isArray(args))throw Error('Invalid API command');
  const atom=s=>typeof s==='string'&&/^[\x21-\x7e]+$/.test(s);
  if(!atom(command))throw Error('Invalid API command');
  const values=args.map(x=>{
    if(typeof x!=='string'&&!(typeof x==='number'&&Number.isSafeInteger(x)))throw Error('Invalid API argument');
    const s=String(x);if(!atom(s))throw Error('Invalid API argument');return s;
  });
  const result={command,args:values};
  if(guards.cameraSerial!==undefined){if(!atom(guards.cameraSerial))throw Error('Invalid camera identity');result.camera_serial=guards.cameraSerial;}
  if(guards.expectedRevision!==undefined){if(!Number.isInteger(guards.expectedRevision)||guards.expectedRevision<0||guards.expectedRevision>0xffffffff)throw Error('Invalid revision');result.expected_revision=guards.expectedRevision;}
  return result;
}
// Adapter for the bundled UI's existing command builders. External clients can
// use command()/camera() directly without constructing a command line.
function fromCommandLine(line) {
  if(typeof line!=='string'||/[\r\n]/.test(line))throw Error('Invalid command line');
  const words=line.trim().split(/[ \t]+/),guards={};
  while(words[0]==='if-serial'||words[0]==='if-revision'){
    const type=words.shift(),value=words.shift();
    if(type==='if-serial'){if(guards.cameraSerial!==undefined)throw Error('Duplicate identity');guards.cameraSerial=value;}
    else {if(guards.expectedRevision!==undefined||!/^\d+$/.test(value))throw Error('Invalid revision');guards.expectedRevision=Number(value);}
  }
  return envelope(words.shift(),words,guards);
}
function createClient({baseUrl='',token,fetchImpl=globalThis.fetch}={}) {
  const base=normalizeBase(baseUrl);
  if(typeof token!=='string'||token.length<32||/[\r\n]/.test(token))throw Error('管理密钥至少需要 32 个字符。');
  const request=(path,body,signal)=>fetchImpl(base+path,{
    method:body===undefined?'GET':'POST',credentials:'omit',redirect:'error',cache:'no-store',
    headers:{Authorization:'Bearer '+token,...(body===undefined?{}:{'Content-Type':'application/json'})},
    ...(body===undefined?{}:{body:JSON.stringify(body)}),signal
  });
  const json=async response=>{
    const value=await response.json();
    if(!response.ok||!value.ok){const error=Error(value.error||'request_failed');error.code=value.error;error.status=response.status;throw error;}
    return value;
  };
  const raw=(command,args=[],options={})=>request('/api/v1/commands',envelope(command,args,options),options.signal);
  return Object.freeze({
    baseUrl:base,
    status:options=>request('/api/v1/status',undefined,options?.signal).then(json),
    capabilities:options=>request('/api/v1/capabilities',undefined,options?.signal).then(json),
    command:(command,args,options)=>raw(command,args,options).then(json),
    commandRaw:raw,
    camera:(camera,command,args=[],options={})=>raw(command,[camera.slot,...args],{...options,cameraSerial:camera.serial}).then(json),
    commandLineRaw:(line,options={})=>request('/api/v1/commands',fromCommandLine(line),options.signal)
  });
}
const api=Object.freeze({createClient,normalizeBase,fromCommandLine,envelope});
if(typeof module!=='undefined'&&module.exports)module.exports=api;
else globalThis.VavaAPI=api;
})();
