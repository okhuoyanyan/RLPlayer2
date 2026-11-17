// Render the shipped page and script in an isolated Edge profile. Every URL is
// answered through its private DevTools pipe. No HTTP server or player is used.
'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const {spawn, spawnSync} = require('node:child_process');
const {once, EventEmitter} = require('node:events');
const delay = ms => new Promise(resolve => setTimeout(resolve, ms));
function option(name, fallback) {
  const i = process.argv.indexOf(name);
  return i < 0 ? fallback : process.argv[i + 1];
}
const root = path.resolve(option('--source', process.env.RLPLAYER_TEST_SOURCE_ROOT || path.join(__dirname, '..')));
const workRoot = path.resolve(option('--work-root', 'build-controls336/evidence'));
fs.mkdirSync(workRoot, {recursive:true});
const run = fs.mkdtempSync(path.join(workRoot, 'controls336-'));
const origin = 'https://rlplayer-ui.invalid';
const fixtureTracks = ['Q:/fixture/First movement.flac', 'Q:/fixture/Second movement.flac'];
const langs = ['en', 'fr', 'ja', 'es', 'ru', 'ar'];
const dictionaries = Object.fromEntries(langs.map(lang => [lang, JSON.parse(fs.readFileSync(path.join(root,'data/i18n',lang+'.json'),'utf8'))]));
function checked(name, value) { assert.ok(value, name); checks.push(name); console.log('PASS '+name); }
function program(command, args) {
  const r = spawnSync(command,args,{encoding:'utf8',windowsHide:true});
  if (r.error || r.status !== 0) throw Error([r.error,r.stdout,r.stderr].filter(Boolean).join('\n'));
}
function exportPage() {
  const cpp=path.join(run,'export-ui.cpp'), exe=path.join(run,'export-ui.exe'), html=path.join(run,'page.html'), js=path.join(run,'app.js');
  const borrowed=option('--assets','');
  if (borrowed) return {html:fs.readFileSync(path.join(borrowed,'page.html'),'utf8'),js:fs.readFileSync(path.join(borrowed,'app.js'),'utf8')};
  fs.writeFileSync(cpp,'#include <fstream>\n#include "net/ui_page.h"\n#include "net/ui_script.h"\nint main(int argc,char**argv){if(argc!=3)return 2;std::ofstream page(argv[1],std::ios::binary),js(argv[2],std::ios::binary);page<<rta::net::rta_ui_page();js<<rta::net::rta_ui_script();return page&&js?0:1;}\n');
  program(option('--cxx','C:/Program Files/AMD/ROCm/7.2/bin/clang++.exe'),['-std=c++23','-O0','-I'+path.join(root,'src'),cpp,'-o',exe]);
  program(exe,[html,js]);
  return {html:fs.readFileSync(html,'utf8'),js:fs.readFileSync(js,'utf8')};
}
function startBrowser() {
  const exe=option('--edge', ['C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe','C:/Program Files/Microsoft/Edge/Application/msedge.exe'].find(fs.existsSync));
  if (!exe) throw Error('The production DOM regression requires Microsoft Edge.');
  // Chromium reads commands from FD 3 and writes its NUL-delimited replies to FD 4.
  const proc=spawn(exe,['--headless=new','--disable-gpu','--disable-background-networking','--disable-component-update','--no-first-run','--mute-audio','--remote-debugging-pipe','--user-data-dir='+path.join(run,'profile'),'about:blank'],{windowsHide:true,stdio:['ignore','ignore','ignore','pipe','pipe']});
  return connectBrowser(proc);
}
function connectBrowser(proc, reportError=error=>errors.push(String(error))) {
  const pending=new Map(); let seq=0, buffer=Buffer.alloc(0), sessionId;
  let closing=false, exited=false, closePromise;
  const closed=once(proc,'exit');closed.catch(()=>{}); // attach() still reports spawn errors; close() awaits this promise.
  const ended=()=>Object.assign(Error('DevTools browser is closing or closed'),{code:'CDP_CLOSED'});
  function rejectPending(error){for(const p of pending.values()){clearTimeout(p.timeout);p.reject(error);}pending.clear();}
  function transportError(error){if(!closing)reportError(error);rejectPending(error);}
  proc.once('exit',()=>{exited=true;const error=ended();if(!closing)reportError(error);rejectPending(error);});
  proc.once('error',transportError);
  proc.stdio[3].on('error',transportError);proc.stdio[4].on('error',transportError);
  function command(method,params,session,shutdown=false) {
    if((closing&&!shutdown)||exited||proc.stdio[3].destroyed||proc.stdio[3].writableEnded)return Promise.reject(ended());
    return new Promise((resolve,reject)=>{
      // 2026-11-14（3.3.11）：8 s 对冷启动的无头 Edge 太紧 —— 机器上有其它活时 Page.navigate
 // 会在 8 s 处被掐断（实测门禁里随机挂在 controls336 / workspace335_integration）。
 // 放宽到 30 s：真挂死仍会被 ctest 的 TIMEOUT 收掉，不会变成静默假绿。
  const id=++seq;const timeout=setTimeout(()=>{pending.delete(id);reject(Error('DevTools timed out on '+method));},30000);
      pending.set(id,{resolve,reject,timeout});
      function failed(error){if(!error||!pending.has(id))return;pending.delete(id);clearTimeout(timeout);reject(error);}
      try{proc.stdio[3].write(JSON.stringify(session ? {id,method,params,sessionId:session} : {id,method,params})+'\0',failed);}catch(error){failed(error);}
    });
  }
  function send(method,params={},session=sessionId){return command(method,params,session);}
  const api={send,async attach(){const {targetId}=await send('Target.createTarget',{url:'about:blank'});const result=await send('Target.attachToTarget',{targetId,flatten:true});sessionId=result.sessionId;},close(){
    if(closePromise)return closePromise;
    // Stop new fixture work before Browser.close; delayed handlers may only receive CDP_CLOSED.
    closing=true;api.onRequest=null;rejectPending(ended());
    closePromise=(async()=>{try{if(!exited&&proc.exitCode===null){try{await command('Browser.close',{},null,true);}catch(error){if(error.code!=='CDP_CLOSED')throw error;}}}finally{if(!proc.stdio[3].destroyed&&!proc.stdio[3].writableEnded)proc.stdio[3].end();await closed;rejectPending(ended());}})();
    return closePromise;
  },onRequest:null};
  proc.stdio[4].on('data',chunk=>{
    buffer=Buffer.concat([buffer,chunk]);
    let split;
    while((split=buffer.indexOf(0))!==-1){
      const msg=JSON.parse(buffer.subarray(0,split).toString('utf8'));buffer=buffer.subarray(split+1);
      if(msg.id&&pending.has(msg.id)){const p=pending.get(msg.id);pending.delete(msg.id);clearTimeout(p.timeout);msg.error?p.reject(Error(msg.error.message)):p.resolve(msg.result);}
      else if(msg.method==='Runtime.exceptionThrown')reportError(msg.params.exceptionDetails.exception?.description||msg.params.exceptionDetails.text);
      else if(msg.method==='Fetch.requestPaused'&&!closing&&api.onRequest)api.onRequest(msg.params).catch(error=>{if(!(closing&&error.code==='CDP_CLOSED'))reportError(error);});
    }
  });
  return api;
}
async function checkBrowserLifecycle(){
  const failures=[],writes=[],verified=[];let endCalls=0;
  async function verify(label,check){await check();verified.push(label);}
  const proc=new EventEmitter();proc.exitCode=null;proc.stdio=[];
  proc.stdio[3]=Object.assign(new EventEmitter(),{writableEnded:false,destroyed:false,write(value,callback){assert.equal(this.writableEnded,false,'never write an ended pipe');writes.push(JSON.parse(value.slice(0,-1)));callback?.();},end(){this.writableEnded=true;endCalls++;}});
  proc.stdio[4]=new EventEmitter();
  const browser=connectBrowser(proc,error=>failures.push(String(error)));
  const reply=message=>proc.stdio[4].emit('data',Buffer.from(JSON.stringify(message)+'\0'));
  const rejected=browser.send('Runtime.fixtureError');reply({id:writes.at(-1).id,error:{message:'visible fixture protocol error'}});
  await verify('protocol errors reject during normal operation',()=>assert.rejects(rejected,/visible fixture protocol error/));
  const transport=browser.send('Runtime.transportError');proc.stdio[3].emit('error',Error('visible fixture transport error'));
  await verify('transport errors reject during normal operation',()=>assert.rejects(transport,/visible fixture transport error/));
  await verify('transport errors are reported rather than swallowed',()=>assert.deepEqual(failures,['Error: visible fixture transport error']));failures.length=0;
  const stalled=browser.send('Runtime.pending'),cancelled=verify('pending requests reject when shutdown starts',()=>assert.rejects(stalled,{code:'CDP_CLOSED'}));
  let requestCalls=0,release;const held=new Promise(resolve=>release=resolve);
  browser.onRequest=async()=>{requestCalls++;await held;await browser.send('Fetch.fulfillRequest');};
  reply({method:'Fetch.requestPaused',params:{requestId:'in-flight'}});
  const closing=browser.close();reply({method:'Fetch.requestPaused',params:{requestId:'late'}});release();await cancelled;
  await verify('closing stops newly arriving fixture requests',()=>assert.equal(requestCalls,1));
  await verify('only the private close command may write during shutdown',()=>assert.equal(writes.at(-1).method,'Browser.close'));reply({id:writes.at(-1).id,result:{}});
  proc.exitCode=0;proc.emit('exit',0);await closing;await browser.close();
  const written=writes.length;await verify('closed send explicitly rejects',()=>assert.rejects(browser.send('Fetch.fulfillRequest'),{code:'CDP_CLOSED'}));
  await verify('closed send never touches the pipe',()=>assert.equal(writes.length,written));
  await verify('close is idempotent',()=>assert.equal(endCalls,1));
  await verify('delayed fixture rejection is clean during shutdown',()=>assert.deepEqual(failures,[]));
  fs.writeFileSync(path.join(run,'cdp-lifecycle.json'),JSON.stringify({checks:verified,failures:0,writes:writes.map(x=>x.method),requestCalls,endCalls},null,2));
  console.log('PASS CDP lifecycle: '+verified.length+' offline boundary checks');
}
const settings={quickstart:'1',uilang:'zh',filter:'poly-sinc-short-lp',filterNx:'',backend:'null',device:'null',volume:'-64.5',rate:'88200',dsd:'0',dsdnative:'0',twostage:'auto',shaper:'auto',modfamily:'dsd',modorder:'5',sdmfilt:'',sdmfiltnx:'',directsdm:'0',gpuvendor:'off',metaon:'0',metallm:'0',anlmaster:'1',autoanl:'1',roomcorrauto:'0',ui_profile:'pcm',ui_style:'balanced'};
const mutations=[],volumeWrites=[],preferenceWrites=[],requests=[],forbidden=[],errors=[],checks=[],geometry=[];
const facts={language:'zh',files:[fixtureTracks[0]],current:fixtureTracks[0],paused:false,playing:true,profile:'pcm',style:'balanced',revision:1,allowWorkbench:false,failWorkbench:false,rates:[44100,48000],rawVolumeLocked:false,volumeWritable:true,actualVolume:-64.5,allowVolume:false};
const health={status:'ready',file:fixtureTracks[0],rate:44100,bits:16,channels:2,duration:180,dr_db:12.4,lufs:-17.2,lra:8.3,peak_dbfs:-1,true_peak_dbtp:-.8,cutoff_hz:21500,fake_verdict:'none',quality:{available:true,clipped_runs:0,clipped_pct:0,snr_db:82,crest_db:13,dc_dbfs:-120,bandwidth_hz:21500,spectral_centroid_hz:3400,spectral_rolloff_hz:16000,spectral_flatness:.1,dyn_rms_db:-22}};
let peq={bypass:1,preamp:0,name:'Flat fixture',bands:[{i:1,on:1,type:'PK',freq:80,gain:0,q:1},{i:2,on:1,type:'PK',freq:1000,gain:0,q:.8}],points:[[20,0],[1000,0],[20000,0]]};
function workbench(){return {ok:true,settings:{...settings},profile:facts.profile,style:facts.style,revision:String(facts.revision),playing:facts.playing,paused:facts.paused,file:facts.current,position:12.75,duration:180,queueIndex:0,cueStart:-1,cueEnd:-1,volumeWritable:facts.volumeWritable,rawVolumeLocked:facts.rawVolumeLocked,peq:JSON.parse(JSON.stringify(peq)),actual:{outRate:Number(settings.rate)||44100,outBits:24,device:'Null fixture'}};}
function mock(url,method,body){
 const p=url.pathname;requests.push({path:p,method,query:url.search});
 if(p==='/api/lib/stats'&&facts.statsPlan){const plan=facts.statsPlan;facts.statsPlan=null;return {ok:true,roots:plan.roots,tracks:0,albums:0,scanning:false,__fixtureDelay:plan.delay};}
 if(p==='/api/lib/stats')return {ok:true,tracks:(facts.libraryRoots||[]).length?2:0,albums:0,roots:[...(facts.libraryRoots||[])],scanning:false};
 if(p==='/api/lib/roots'&&method==='POST'){
  const patch=Object.fromEntries(new URLSearchParams(body));facts.libraryActions ||= [];facts.libraryActions.push(patch);
  if(facts.failLibrary)return {ok:false,error:'Fixture folder write failed'};
  facts.libraryRoots ||= [];if(patch.add&&!facts.libraryRoots.includes(patch.add))facts.libraryRoots.push(patch.add);if(patch.remove)facts.libraryRoots=facts.libraryRoots.filter(p=>p!==patch.remove);return {ok:true};
 }
 if(p==='/api/lib/scan'){facts.libraryScans=(facts.libraryScans||0)+1;return facts.failScan?{ok:false,error:'Fixture scan failed'}:{ok:true};}
 if(p==='/api/waveform'){
  const file=url.searchParams.get('file'),plan=(facts.wavePlans||{})[file];
  if(!plan)return {ok:false,state:'error',duration:180,bins:[],error:'Fixture waveform unavailable'};
  if(plan.pending>0){plan.pending--;return {ok:true,state:'pending',duration:plan.duration||180,bins:[]};}
  return {ok:plan.state!=='error',state:plan.state||'ready',duration:plan.duration||180,bins:plan.bins||[],error:plan.error||'',__fixtureDelay:plan.delay||0};
 }
 if(p==='/api/fav/list')return {ok:true,files:[...(facts.favorites||new Set())]};
 if(p==='/api/fav'){
  const file=url.searchParams.get('file');facts.favorites ||= new Set();
  if(url.searchParams.get('on')==='1')facts.favorites.add(file);else facts.favorites.delete(file);
  return {ok:true};
 }
 if(p==='/api/workbench/profiles')return {ok:true,active:'A',slots:['A','B','C','D'].map(id=>({id,name:id,settings:{...settings},peq:JSON.parse(JSON.stringify(peq)),eqId:'',eqName:''})),loop:{enabled:false,start:0,end:180,file:facts.current}};
 if(p==='/api/workbench/profile'||p==='/api/workbench/loop')return {ok:true};
 if(p==='/api/workbench/apply'){
  if(!facts.allowWorkbench){forbidden.push('unexpected '+p);return {ok:false,error:'unapproved fixture mutation'};}
  const patch=Object.fromEntries(new URLSearchParams(body));mutations.push(patch);
  const planned=facts.workbenchPlan;facts.workbenchPlan=null;if(planned&&planned.fail)return {ok:false,error:'Old profile request failed',__fixtureDelay:planned.delay||500};
  assert.equal(patch.expected_file,facts.current);assert.equal(patch.expected_revision,String(facts.revision));
  if(facts.failWorkbench)return {ok:false,error:'Fixture device unavailable'};
  for(const [key,value] of Object.entries(patch)){
   if(key==='peq')peq={...peq,...JSON.parse(value)};
   else if(!['expected_file','expected_revision','profile','style','position'].includes(key))settings[key]=value;
  }
  if(patch.profile){facts.profile=patch.profile;settings.ui_profile=patch.profile;settings.rate=patch.profile==='bitperfect'?'src':'176400';settings.filter=patch.profile==='bitperfect'?'':'poly-sinc-short-lp';settings.dsd=patch.profile==='dsd'?'128':'0';}
  if(patch.style){facts.style=patch.style;settings.ui_style=patch.style;settings.filter={warm:'poly-sinc-gauss-medium',balanced:'poly-sinc-short-lp',sharp:'poly-sinc-long-lp'}[patch.style];}
  facts.revision++;return {ok:true,__fixtureDelay:planned&&planned.delay||0};
 }
 if(p==='/api/volume'&&facts.allowVolume){
  if(facts.volumePlans&&facts.volumePlans.length){
   const plan=facts.volumePlans.shift(),db=Number(url.searchParams.get('db'));
   volumeWrites.push({db,rejected:plan.status!==200,planned:plan.status});
   if(plan.status===409&&plan.lockRaw!==false){facts.rawVolumeLocked=true;facts.volumeWritable=false;facts.actualVolume=0;settings.volume='0';}
   else if(plan.status===200&&!facts.deferVolumeApply){facts.rawVolumeLocked=false;facts.volumeWritable=true;facts.actualVolume=db;}
   return {ok:plan.status===200,db,__fixtureActualDb:facts.deferVolumeApply&&plan.status===200?db:undefined,error:plan.status===409?'原始 DSD 码流不支持软件音量':'',__fixtureDelay:plan.delay||0,__fixtureStatus:plan.status===409?409:200,__fixtureNetwork:plan.status==='network'};
  }
  const db=Number(url.searchParams.get('db'));volumeWrites.push({db,rejected:!!(facts.rejectVolume||facts.rawVolumeLocked)});
  if(facts.rejectVolume||facts.rawVolumeLocked){
   facts.rawVolumeLocked=true;facts.volumeWritable=false;facts.actualVolume=0;settings.volume='0';
   return {ok:false,error:'原始 DSD 码流不支持软件音量；请使用设备音量，或切换到支持音量处理的 DoP。',volumeWritable:false,rawVolumeLocked:true,db:0};
  }
  facts.actualVolume=db;return {ok:true,db};
 }
 if(p==='/api/settings'&&method==='POST'&&facts.allowPreferences){
  const patch=Object.fromEntries(new URLSearchParams(body));preferenceWrites.push(patch);Object.assign(settings,patch);return {ok:true,merged:true};
 }
 if(p==='/api/settings'&&method==='POST'||/^\/api\/(?:play|pause|stop|skip|seek|volume|playmode|qdel|qmove|lib\/play|sq\/play|cast\/start|input\/start|roomcorr\/measure|preset\/apply|queue\/(?:set|clear|add|sort|undo))$/.test(p)){
  forbidden.push(method+' '+p);return {ok:false,error:'unexpected real action'};
 }
 switch(p){
  case '/api/settings':return {ok:true,kv:{...settings,uilang:facts.language}};
  case '/api/workbench/state':return workbench();
  case '/api/status':return {ok:true,playing:facts.playing,paused:facts.paused,current:facts.current,cueStart:facts.cueStart??-1,cueEnd:facts.cueEnd??-1,queueIndex:0,rate:44100,dur:180,pos:12.75,volDb:facts.actualVolume,startDb:facts.actualVolume,volumeWritable:facts.volumeWritable,rawVolumeLocked:facts.rawVolumeLocked,outRate:Number(settings.rate)||44100,outBits:24,outMode:'pcm',bitPerfect:'no',resample:true,error:'',gpu:false,summaryEnabled:false,hasLlmKey:false};
  case '/api/queue':return {ok:true,items:facts.files,cues:[[-1,-1]],index:0,mode:0,repeat:0,shuffle:false};
  case '/api/ping':return {ok:true,version:'3.3.5'};
  case '/api/i18n':return {ok:true,lang:facts.language,map:facts.language==='zh'?{}:dictionaries[facts.language]};
  case '/api/preset/caps':return facts.caps||{ok:true,device:{name:'Null fixture',rates:[44100,88200,176400],maxRate:176400,dop:true,dopMax:'dsd128',dsdNative:false,exclusive:true},notes:[]};
  case '/api/devices':return {ok:true,devices:[{id:'dac-fixture',name:'Isolated fixture DAC'}],asioDrivers:[]};
  case '/api/lib/stats':return {ok:true,tracks:1,albums:1,scanning:false,roots:[]};
  case '/api/lib/albums':return {ok:true,albums:[]};
  case '/api/filters':return {ok:true,items:[],filters:[]};
  case '/api/rates':return {ok:true,rates:facts.rates,sysRate:44100};
  case '/api/signal':return {ok:!!facts.current,path:{src:facts.current,device:'Null fixture',deviceId:'null',inRate:44100,inBits:16,outRate:Number(settings.rate)||44100,outBits:24,outMode:'pcm',codec:'PCM_S16LE',filter:settings.filter,resample:true,bitPerfect:'no'}};
  case '/api/health':return {ok:true,health,cached:true,runs:1,hits:1};
  case '/api/peq':return {ok:true,...peq};
  case '/api/info':case '/api/meta':return {ok:true,file:facts.current,title:'Fixture concerto',artist:'Fixture artist',album:'Fixture album',sr:44100,rate:44100,bits:16,channels:2,hasCover:false};
  case '/api/lyrics':return {ok:true,text:'Fixture plain lyrics',lines:[]};
  default:return {ok:true,items:[],albums:[],roots:[],tracks:[],configured:false};
 }
}

async function main(){
 await checkBrowserLifecycle();
 if(process.argv.includes('--cdp-self-test'))return;
 const assets=exportPage(),browser=startBrowser();let scenario='startup';const aborted=[];
 const evaluate=async expression=>{const r=await browser.send('Runtime.evaluate',{expression,awaitPromise:true,returnByValue:true});if(r.exceptionDetails)throw Error(r.exceptionDetails.exception?.description||r.exceptionDetails.text);return r.result.value;};
 const until=async(expression,label)=>{const end=Date.now()+10000;while(Date.now()<end){if(await evaluate('Boolean('+expression+')'))return;await delay(30);}throw Error('Not ready '+(label||expression));};
 const check=async(label,expression)=>{await until(expression,label);checked(label,true);};
 const click=async selector=>{const r=await evaluate('(()=>{const e=document.querySelector('+JSON.stringify(selector)+');e.scrollIntoView({block:"nearest"});const r=e.getBoundingClientRect();return{x:r.x+r.width/2,y:r.y+r.height/2,w:r.width,h:r.height}})()');assert.ok(r.w>0&&r.h>0,'Visible '+selector);for(const type of ['mousePressed','mouseReleased'])await browser.send('Input.dispatchMouseEvent',{type,x:r.x,y:r.y,button:'left',clickCount:1});};
 const key=async(name,shift=false)=>{for(const type of ['keyDown','keyUp'])await browser.send('Input.dispatchKeyEvent',{type,key:name,code:name,modifiers:shift?8:0,windowsVirtualKeyCode:{Enter:13,Escape:27,ArrowLeft:37,ArrowRight:39,ArrowUp:38,ArrowDown:40,Home:36,End:35,' ':32}[name]||0});};
 const tick=async()=>{const n=await evaluate('statusApplied');await evaluate('tick()');await until('statusApplied>'+n);};
 const resetVolume=async()=>{facts.rawVolumeLocked=false;facts.volumeWritable=true;facts.actualVolume=-64.5;settings.volume='-64.5';facts.volumePlans=[];await evaluate('loadSettings()');await tick();};
 const precise=async(id,value)=>{await evaluate('D('+JSON.stringify(id)+').focus();D('+JSON.stringify(id)+').value='+JSON.stringify(value));await key('Enter');};
 const fulfilled=new Set();
 try{
  await browser.attach();await browser.send('Page.enable');await browser.send('Runtime.enable');
  browser.onRequest=async({requestId,request})=>{
   const url=new URL(request.url);let type='application/json',body='',code=200;
   if(url.origin!==origin){forbidden.push('external '+url.href);code=403;}
   else if(url.pathname==='/'){type='text/html; charset=utf-8';body=assets.html.replace('FILTER_OPTS_1X','<option value=auto>Auto</option><option value=poly-sinc-short-lp>Short</option><option value=poly-sinc-gauss-medium>Warm</option><option value=poly-sinc-long-lp>Sharp</option>').replace('FILTER_OPTS_NX','<option value="">Follow 1x</option>').replace('SHAPER_OPTS','<option value=auto>Auto</option>');}
   else if(url.pathname==='/app.js'){type='application/javascript; charset=utf-8';body=assets.js;}
   else if(url.pathname.startsWith('/api/')){
    const response=mock(url,request.method,request.postData),lag=response.__fixtureDelay||0,network=response.__fixtureNetwork,applyDb=response.__fixtureActualDb;
    if(response.__fixtureStatus)code=response.__fixtureStatus;delete response.__fixtureDelay;delete response.__fixtureNetwork;delete response.__fixtureStatus;delete response.__fixtureActualDb;body=JSON.stringify(response);
    if(lag)await delay(lag);
    if(typeof applyDb==='number'){facts.actualVolume=applyDb;facts.volumeApplyOrder ||= [];facts.volumeApplyOrder.push(applyDb);}
    if(network){await browser.send('Fetch.failRequest',{requestId,errorReason:'ConnectionReset'});return;}
   }else code=404;
   try{await browser.send('Fetch.fulfillRequest',{requestId,responseCode:code,responseHeaders:[{name:'Content-Type',value:type},{name:'Cache-Control',value:'no-store'}],body:Buffer.from(body).toString('base64')});fulfilled.add(requestId);}catch(error){if(url.pathname==='/api/waveform')aborted.push(requestId);else throw error;}
  };
  await browser.send('Fetch.enable',{patterns:[{urlPattern:'*',requestStage:'Request'}]});await browser.send('Emulation.setDeviceMetricsOverride',{width:1340,height:880,deviceScaleFactor:1,mobile:false});await browser.send('Page.navigate',{url:origin+'/'});
  await until('typeof PLAYBACK_SETTINGS!=="undefined"&&PLAYBACK_SETTINGS.ready&&D("rlListenFavorite")&&S.lastCur&&rlConsole.state&&rlConsole.state.settings');
  scenario='old card completion versus reopened music folders';
  const raceOutcomes=[];
  for(const fail of [false,true]){
   facts.allowWorkbench=true;facts.workbenchPlan={fail,delay:650};
   await until('!rlListening.working&&!rlConsole.busy&&rlConsole.state&&rlConsole.state.settings');
   await click('[data-rl-listen-task=profile]');
   await until('D("rlListeningDialog").open&&!document.querySelector("[data-rl-listening-profile=pcm]").disabled');
   await click('[data-rl-listening-profile=pcm]');
   await until('rlListening.draft.profile==="pcm"&&!D("rlListeningDialogApply").disabled');
   const mutationCount=mutations.length;await click('#rlListeningDialogApply');
   const dispatchLimit=Date.now()+10000;while(mutations.length===mutationCount&&Date.now()<dispatchLimit)await delay(10);
   assert.equal(mutations.length,mutationCount+1,'old profile request was actually dispatched');
   await delay(80);await evaluate('D("rlListeningDialog").close();D("rlListeningFoldersButton").click()');
   await until('D("rlListeningDialog").open&&rlListening.task==="library"');
   await until('!rlListening.working&&!rlConsole.busy');
   raceOutcomes.push({fail,...await evaluate('({open:D("rlListeningDialog").open,task:rlListening.task,message:D("rlListeningDialogMessage").textContent})')});
   await evaluate('if(D("rlListeningDialog").open)D("rlListeningDialog").close()');await delay(30);facts.allowWorkbench=false;
  }
  geometry.push({raceOutcomes});console.log(JSON.stringify({raceOutcomes}));
  checked('old profile success cannot close a newly opened music-folder modal',raceOutcomes[0].open&&raceOutcomes[0].task==='library');
  checked('old profile failure cannot write its error into the new music-folder modal',raceOutcomes[1].open&&raceOutcomes[1].task==='library'&&!raceOutcomes[1].message.includes('Old profile request failed'));
  if(option('--dialog-races-only','')==='1'){console.log(JSON.stringify({checks:checks.length,raceOutcomes,evidence:run}));return;}
  checked('listening hero contains a favorite heart and no duplicate pause button',await evaluate('!!D("rlListenFavorite").querySelector("svg")&&!D("rlListenPlay")&&!!D("rlListenMore")'));
  await click('#rlListenFavorite');await check('favorite heart uses the existing persistent favorite API','FAV_SET.has(S.lastCur)&&D("rlListenFavorite").getAttribute("aria-pressed")==="true"&&D("npFavBtn").classList.contains("on")');
  await click('#rlListenFavorite');await check('favorite can be removed from both views','!FAV_SET.has(S.lastCur)&&D("rlListenFavorite").getAttribute("aria-pressed")==="false"&&!D("npFavBtn").classList.contains("on")');
  for(const width of [1340,1100]){
   await browser.send('Emulation.setDeviceMetricsOverride',{width,height:880,deviceScaleFactor:1,mobile:false});
   const boxes=await evaluate('Array.from(document.querySelectorAll(".rlListeningChoices>.rlListeningChoice")).map(e=>{const r=e.getBoundingClientRect();return {x:r.x,y:r.y,w:r.width,h:r.height}})');
   checked('four listening cards have equal widths at '+width,boxes.length===4&&boxes.every(b=>b.w>0&&Math.abs(b.w-boxes[0].w)<1&&Math.abs(b.y-boxes[0].y)<1));
  }
  await browser.send('Emulation.setDeviceMetricsOverride',{width:1340,height:880,deviceScaleFactor:1,mobile:false});
  facts.allowWorkbench=true;
  await click('[data-rl-listen-task=profile]');
  await until('D("rlListeningDialog").open&&!document.querySelector("[data-rl-listening-profile=bitperfect]").disabled');
  await click('[data-rl-listening-profile=bitperfect]');await click('#rlListeningDialogApply');
  await until('!rlListening.working&&!rlConsole.busy&&!D("rlListeningDialog").open&&rlConsole.state.profile==="bitperfect"');
  checked('original playback correctly disables the listening style choice',await evaluate('document.querySelector("[data-rl-listen-task=style]").disabled'));
  await click('[data-rl-listen-task=style]');
  checked('a disabled original-playback style choice cannot open its dialog',await evaluate('!D("rlListeningDialog").open'));
  await click('[data-rl-listen-task=profile]');
  await until('D("rlListeningDialog").open&&!document.querySelector("[data-rl-listening-profile=pcm]").disabled');
  await click('[data-rl-listening-profile=pcm]');
  await until('rlListening.draft.profile==="pcm"&&!D("rlListeningDialogApply").disabled');
  await click('#rlListeningDialogApply');
  await until('!rlListening.working&&!rlConsole.busy&&!D("rlListeningDialog").open&&rlConsole.state.profile==="pcm"&&!document.querySelector("[data-rl-listen-task=style]").disabled');
  checked('real PCM selection unlocks the style choice before its wording is tested',facts.profile==='pcm'&&settings.rate!=='src');
  facts.allowWorkbench=false;
  await click('[data-rl-listen-task=style]');
  checked('listening style uses everyday descriptions',await evaluate('D("rlListeningDialog").open&&["温和","均衡","锐利"].every(s=>D("rlListeningDialogBody").textContent.includes(s))&&!/滤波|陡峭|相位|抽头/.test(D("rlListeningDialogBody").textContent)'));
  await click('#rlListeningDialogCancel');await until('!D("rlListeningDialog").open');
  const locationBefore=await evaluate('S.view');await click('#rlListeningFoldersButton');await check('music folders open in a same-page modal','D("rlListeningDialog").open&&rlListening.task==="library"');
  checked('opening library management keeps the listening route',await evaluate('S.view')===locationBefore);
  checked('new empty data does not invent an existing music root',await evaluate('rlListening.folderRoots.length===0&&D("rlListeningLibraryHint")&&!D("rlListeningLibraryHint").hidden'));
  await evaluate('rlReceiveMusicFolder("Q:/音乐 & Unicode space")');
  checked('native picker callback fills the live folder input',await evaluate('D("rlListeningFolderPath").value==="Q:/音乐 & Unicode space"'));
  facts.statsPlan={roots:[],delay:650};await evaluate('rlListeningFetchFolders(true);true');await delay(70);
  await click('#rlListeningFolderAdd');await check('folder add is confirmed and scanned before showing success','!rlListening.folderBusy&&rlListening.folderRoots.length===1&&D("rlListeningDialogMessage").dataset.error!=="true"');
  checked('confirmed add preserves the exact Unicode path and starts one scan',facts.libraryRoots[0]==='Q:/音乐 & Unicode space'&&facts.libraryScans===1&&facts.libraryActions[0].add==='Q:/音乐 & Unicode space');
  await delay(750);checked('a late old folder read cannot overwrite a confirmed new root',await evaluate('rlListening.folderRoots.length===1&&rlListening.folderRoots[0]==="Q:/音乐 & Unicode space"&&D("rlListeningFolderList").textContent.includes("Q:/音乐 & Unicode space")'));
  facts.failLibrary=true;await evaluate('D("rlListeningFolderPath").value="Q:/should-fail"');await click('#rlListeningFolderAdd');await check('failed folder add is shown as an error','!rlListening.folderBusy&&D("rlListeningDialogMessage").dataset.error==="true"&&D("rlListeningFolderPath").value==="Q:/should-fail"');
  checked('failed folder add neither changes roots nor starts a scan',facts.libraryRoots.length===1&&facts.libraryScans===1);
  facts.failLibrary=false;facts.failScan=true;await evaluate('D("rlListeningFolderPath").value="Q:/saved-but-scan-fails"');await click('#rlListeningFolderAdd');await check('a saved root with failed scan remains an explicit partial failure','!rlListening.folderBusy&&rlListening.folderRoots.length===2&&D("rlListeningDialogMessage").dataset.error==="true"&&D("rlListeningDialogMessage").textContent.includes("Fixture scan failed")');
  facts.failScan=false;await click('#rlListeningFolderList li button');await check('remove updates only the configured folder list','!rlListening.folderBusy&&rlListening.folderRoots.length===1');
  checked('remove uses the existing root API and refreshes scanning',facts.libraryActions.at(-1).remove==='Q:/音乐 & Unicode space'&&facts.libraryScans===3);
  await click('#rlListeningDialogCancel');await until('!D("rlListeningDialog").open');await click('#rlListeningFoldersButton');await check('closed folder management opens again with persisted roots','D("rlListeningDialog").open&&rlListening.task==="library"&&rlListening.folderRoots[0]==="Q:/saved-but-scan-fails"');
  await click('#rlListeningDialogCancel');await until('!D("rlListeningDialog").open');
  await evaluate('rlOpenListeningCard("library");D("rlListeningDialog").close();rlOpenListeningCard("library")');await delay(80);await check('a previous queued close cannot clear a reopened library modal','D("rlListeningDialog").open&&rlListening.task==="library"');await click('#rlListeningDialogCancel');
  await evaluate('openNowPlaying()');
  checked('listening track detail contains no audio-analysis column or panel',await evaluate('!D("npPage").querySelector("#npAnal,.npanal,[aria-label=音频分析]")'));
  checked('plain-lyrics hint has a transparent background',await evaluate('(()=>{const e=D("npLyric").querySelector(".ly-hint");return !e||getComputedStyle(e).backgroundColor==="rgba(0, 0, 0, 0)"})()'));
  facts.allowVolume=true;facts.allowPreferences=true;
  for(const id of ['barVolInput','npVolInput']){
   scenario='precise '+id;await resetVolume();let n=volumeWrites.length,p=preferenceWrites.length;
   await precise(id,'-42.3');await until('!VOLUME_WRITE.current.pending&&VOLUME_WRITE.current.persisted');
   checked(id+' submits exact tenths once and confirms before saving',volumeWrites.length===n+1&&volumeWrites[n].db===-42.3&&preferenceWrites.slice(p).filter(v=>v.volume==='-42.3').length===1);
   await check(id+' mirrors its accepted gain in both sliders and fields','[D("vol"),D("npVol"),D("barVolInput"),D("npVolInput")].every(e=>Number(e.value)===-42.3)');
   n=volumeWrites.length;p=preferenceWrites.length;
   for(const value of ['20.1','-120.1','']){await precise(id,value);await delay(30);}
   checked(id+' rejects empty and out-of-range values without API or disk writes',volumeWrites.length===n&&preferenceWrites.length===p);
   await evaluate('D('+JSON.stringify(id)+').focus();D('+JSON.stringify(id)+').value="-31.7"');await key(' ');await key('ArrowRight');
   checked(id+' editing does not trigger playback or track shortcuts',forbidden.length===0);
   facts.actualVolume=-48.5;await tick();
   checked(id+' polling preserves an unfinished exact entry',await evaluate('D('+JSON.stringify(id)+').value==="-31.7"'));
   await key('Escape');await check(id+' Escape restores the actual backend level','Number(D('+JSON.stringify(id)+').value)===-48.5');
   await resetVolume();n=volumeWrites.length;p=preferenceWrites.length;facts.volumePlans=[{status:200,delay:650},{status:200,delay:30}];
   await precise(id,'-50.2');await delay(30);await precise(id,'-51.3');await delay(800);
   checked(id+' ignores an older late response and persists only the newer confirmation',volumeWrites.length===n+2&&preferenceWrites.slice(p).filter(v=>v.volume==='-51.3').length===1&&!preferenceWrites.slice(p).some(v=>v.volume==='-50.2')&&settings.volume==='-51.3'&&await evaluate('Number(D("vol").value)===-51.3'));
   for(const failure of [409,'network']){
    await resetVolume();n=volumeWrites.length;p=preferenceWrites.length;facts.volumePlans=[{status:failure,delay:80,lockRaw:false}];await precise(id,'-32.1');await delay(250);
    checked(id+' '+failure+' does not persist an unaccepted level',volumeWrites.length===n+1&&preferenceWrites.slice(p).every(v=>v.volume===undefined)&&settings.volume==='-64.5');
   }
  }
  await resetVolume();facts.deferVolumeApply=true;facts.volumeApplyOrder=[];facts.volumePlans=[{status:200,delay:420},{status:200,delay:20}];
  const beforeSerial=volumeWrites.length,serialPreferences=preferenceWrites.length;
  await precise('barVolInput','-40.3');await delay(40);await precise('npVolInput','-43.2');await delay(120);
  checked('live volume requests are serialized before the first backend application completes',volumeWrites.length===beforeSerial+1&&facts.actualVolume===-64.5);
  await delay(550);checked('a delayed live application cannot overwrite the newer final volume',JSON.stringify(facts.volumeApplyOrder)===JSON.stringify([-40.3,-43.2])&&facts.actualVolume===-43.2&&settings.volume==='-43.2'&&preferenceWrites.slice(serialPreferences).every(p=>p.volume!=='-40.3'));
  await resetVolume();facts.volumeApplyOrder=[];facts.volumePlans=[{status:200,delay:420},{status:200,delay:20}];const beforeCoalesce=volumeWrites.length;
  await precise('barVolInput','-30.1');await delay(30);await precise('npVolInput','-35.2');await precise('barVolInput','-50.3');await delay(650);
  checked('superseded queued gestures never reach the backend',volumeWrites.length===beforeCoalesce+2&&volumeWrites.at(-1).db===-50.3&&!volumeWrites.slice(beforeCoalesce).some(w=>w.db===-35.2)&&facts.actualVolume===-50.3&&settings.volume==='-50.3');facts.deferVolumeApply=false;
  await resetVolume();await evaluate('closeNowPlaying()');let before=volumeWrites.length;
  await evaluate('D("volKnob").focus();D("volKnob").dispatchEvent(new KeyboardEvent("keydown",{key:"ArrowUp",bubbles:true}))');await until('VOLUME_WRITE.current.persisted');checked('volume knob ArrowUp increases by exactly 0.5 dB',volumeWrites.at(-1).db===-64&&volumeWrites.length===before+1);
  await evaluate('D("volKnob").dispatchEvent(new KeyboardEvent("keydown",{key:"ArrowDown",shiftKey:true,bubbles:true}))');await until('VOLUME_WRITE.current.persisted');checked('volume knob Shift fine step decreases by exactly 0.1 dB',volumeWrites.at(-1).db===-64.1);
  await click('#volFloor');await until('VOLUME_WRITE.current.persisted');await check('mute uses the real minimum level in both views','Number(D("vol").value)===-120&&D("volFloor").getAttribute("aria-pressed")==="true"&&D("npVolFloor").getAttribute("aria-pressed")==="true"');
  await click('#volFloor');await until('VOLUME_WRITE.current.persisted');checked('mute restores the previous exact volume',volumeWrites.at(-1).db===-64.1);
  // 3.3.9：鼠标只是移过旋钮（未按下）绝不能改音量。
  before=volumeWrites.length;
  const knobBox=await evaluate('(()=>{const r=D("volKnob").getBoundingClientRect();return{x:r.x,y:r.y,w:r.width,h:r.height}})()');
  await browser.send('Input.dispatchMouseEvent',{type:'mouseMoved',x:knobBox.x+knobBox.w*.8,y:knobBox.y+knobBox.h*.5,buttons:0});
  await browser.send('Input.dispatchMouseEvent',{type:'mouseMoved',x:knobBox.x+knobBox.w*.2,y:knobBox.y+knobBox.h*.9,buttons:0});
  await delay(180);
  checked('hovering across the volume knob never changes volume without a press',volumeWrites.length===before&&Math.abs(Number(await evaluate('D("vol").value'))-(-64.1))<1e-9);
  // 真实按住拖动才生效，且松手只提交一次。
  await browser.send('Input.dispatchMouseEvent',{type:'mousePressed',x:knobBox.x+knobBox.w*.5,y:knobBox.y+knobBox.h*.06,button:'left',buttons:1,clickCount:1});
  await browser.send('Input.dispatchMouseEvent',{type:'mouseMoved',x:knobBox.x+knobBox.w*.92,y:knobBox.y+knobBox.h*.5,button:'left',buttons:1});
  await browser.send('Input.dispatchMouseEvent',{type:'mouseReleased',x:knobBox.x+knobBox.w*.92,y:knobBox.y+knobBox.h*.5,button:'left',buttons:0});
  await delay(400);await until('VOLUME_WRITE.current.persisted');
  // 按下与拖动各产生一次实时音量请求是允许的（拖动本来就是连续调节）；
  // 关键是：总次数有界、最后一次落在拖动终点、松手后只持久化一次。
  checked('a real press-drag-release gesture moves the knob and persists one final value',
    volumeWrites.length>before&&volumeWrites.length<=before+3&&volumeWrites.at(-1).db>-64.1&&volumeWrites.at(-1).db<=20&&
    await evaluate('S.vol>'+(-64.1)));
  facts.rawVolumeLocked=true;facts.volumeWritable=false;facts.actualVolume=0;await tick();before=volumeWrites.length;
  for(const id of ['barVolInput','npVolInput'])await precise(id,'-70');
  await evaluate('D("volFloor").click();D("npVolFloor").click();D("volKnob").dispatchEvent(new KeyboardEvent("keydown",{key:"ArrowUp",bubbles:true}));D("npVolKnob").dispatchEvent(new KeyboardEvent("keydown",{key:"ArrowDown",bubbles:true}));setVol(-40)');await delay(50);
  checked('raw DSD disables numeric fields sliders knobs and both mute controls',await evaluate('["vol","npVol","barVolInput","npVolInput","volFloor","npVolFloor"].every(id=>D(id).disabled)&&D("volKnob").dataset.locked==="true"&&D("npVolKnob").dataset.locked==="true"&&S.vol===0&&Number(D("barVolInput").value)===0&&Number(D("npVolInput").value)===0'));
  checked('raw DSD refuses every synthetic software-volume gesture',volumeWrites.length===before);
  await resetVolume();
  scenario='real waveform';facts.wavePlans={};
  const wf='Q:/waveform336/真实波形 & Unicode space.flac',other='Q:/waveform336/real-B.flac',late='Q:/waveform336/late.flac',bad='Q:/waveform336/error.flac';
  facts.wavePlans[wf]={pending:2,duration:120,bins:[0,.1,.2,.3,.4,.5,.6,.7,.8,.9,1]};facts.wavePlans[other]={duration:120,bins:[1,0,1,0]};facts.wavePlans[late]={duration:120,bins:[.2,.2,.2],delay:650};facts.wavePlans[bad]={state:'error',error:'Fixture decode error'};
  await evaluate('closeNowPlaying();window.waveSeeks=[];window.waveRanges=[];window.waveHost=document.createElement("div");waveHost.id="waveTest336";waveHost.style="position:fixed;z-index:999;left:270px;top:100px;width:800px;padding:20px;background:var(--bg)";document.body.appendChild(waveHost);window.wave=rlMountWaveform(waveHost,{file:'+JSON.stringify(wf)+',duration:120,position:30,start:20,end:80,selection:true,onSeek:s=>waveSeeks.push(s),onRange:(a,b,committed)=>waveRanges.push({a,b,committed})})');
  await check('pending waveform has no fabricated SVG bins','waveHost.dataset.state==="pending"&&waveHost.querySelector("path").getAttribute("d")===""');
  await check('waveform becomes ready after asynchronous pending polls','waveHost.dataset.state==="ready"&&waveHost.querySelector("path").getAttribute("d").split("M").length===12');
  checked('ready SVG uses the supplied whole-track amplitude data',await evaluate('waveHost.querySelector("path").getAttribute("d").includes("M954.55 3.00V65.00")&&waveHost.querySelector(".rlWavePlayed").style.clipPath.includes("75%")'));
  const waveGeometry=await evaluate('(()=>{const r=waveHost.querySelector(".rlWavePlot").getBoundingClientRect();return{x:r.x,y:r.y,w:r.width,h:r.height}})()');
  const x=waveGeometry.x+waveGeometry.w*.75,y=waveGeometry.y+waveGeometry.h/2;
  for(const type of ['mousePressed','mouseReleased'])await browser.send('Input.dispatchMouseEvent',{type,x,y,button:'left',clickCount:1});await check('actual waveform click seeks to three quarters','waveSeeks.length&&Math.abs(waveSeeks.at(-1)-90)<2');
  await evaluate('waveHost.querySelector(".rlWaveSeek").focus()');await key('Home');await key('ArrowRight');checked('waveform keyboard seeks without triggering global playback',await evaluate('waveSeeks.at(-1)===5')&&forbidden.length===0);
  const handle=await evaluate('(()=>{const r=waveHost.querySelector(".rlWaveStart").getBoundingClientRect();return{x:r.x+r.width/2,y:r.y+r.height/2}})()');
  await browser.send('Input.dispatchMouseEvent',{type:'mousePressed',x:handle.x,y:handle.y,button:'left',buttons:1,clickCount:1});await browser.send('Input.dispatchMouseEvent',{type:'mouseMoved',x:waveGeometry.x+waveGeometry.w*.35,y:handle.y,button:'left',buttons:1});await browser.send('Input.dispatchMouseEvent',{type:'mouseReleased',x:waveGeometry.x+waveGeometry.w*.35,y:handle.y,button:'left',clickCount:1});
  await check('A/B start handle commits a real drag range','waveRanges.some(r=>r.committed&&Math.abs(r.a-42)<.2&&r.b===80)');
  await evaluate('waveHost.querySelector(".rlWaveEnd").focus()');await key('ArrowRight');await check('A/B end handle is reachable and precise by keyboard','waveRanges.at(-1).committed&&Math.abs(waveRanges.at(-1).b-80.1)<.001');
  await evaluate('wave.update({position:50});waveHost.querySelector(".rlWaveSetEnd").click()');await check('end button sets the current actual position','waveRanges.at(-1).committed&&waveRanges.at(-1).b===50');
  await evaluate('wave.update({file:'+JSON.stringify(late)+',duration:120,position:0})');await delay(50);await evaluate('wave.update({file:'+JSON.stringify(other)+',duration:120,position:12})');await check('track switch immediately displays only the new waveform','waveHost.dataset.file==='+JSON.stringify(other)+'&&waveHost.dataset.state==="ready"&&waveHost.querySelector("path").getAttribute("d").split("M").length===5');await delay(750);
  checked('late preceding-track bins cannot replace the new track',await evaluate('waveHost.dataset.file==='+JSON.stringify(other)+'&&waveHost.querySelector("path").getAttribute("d").split("M").length===5'));
  const priorSeeks=await evaluate('waveSeeks.length');await evaluate('var input=waveHost.querySelector(".rlWaveSeek");input.dispatchEvent(new PointerEvent("pointerdown",{bubbles:true}));input.value=90;input.dispatchEvent(new Event("input",{bubbles:true}));wave.update({file:'+JSON.stringify(wf)+',duration:120,position:0});input.dispatchEvent(new Event("input",{bubbles:true}));input.dispatchEvent(new Event("change",{bubbles:true}))');
  checked('a seek released after a track switch cannot seek the new track',await evaluate('waveSeeks.length')===priorSeeks);
  await evaluate('wave.update({file:'+JSON.stringify(bad)+',duration:120,position:20})');await check('waveform failure shows a clear usable progress fallback','waveHost.dataset.state==="error"&&!waveHost.querySelector(".rlWaveSeek").disabled&&waveHost.querySelector("path").getAttribute("d")===""&&!waveHost.querySelector(".rlWaveRetry").hidden');
  facts.wavePlans[bad]={duration:120,bins:[.5,.25,.75]};await click('#waveTest336 .rlWaveRetry');await check('failed waveform can be retried with real data','waveHost.dataset.state==="ready"&&waveHost.querySelector("path").getAttribute("d").split("M").length===4');
  facts.wavePlans[other]={duration:120,bins:[.9,.8,.7]};const remountRequests=requests.filter(r=>r.path==='/api/waveform').length;
  await evaluate('wave.destroy();window.wave=rlMountWaveform(waveHost,{file:'+JSON.stringify(other)+',duration:120,position:0,start:0,end:120,selection:true,onSeek:s=>waveSeeks.push(s),onRange:(a,b,committed)=>waveRanges.push({a,b,committed})})');
  await check('reopening a physical file asks the source-aware backend for fresh bins','waveHost.dataset.state==="ready"&&waveHost.querySelector("path").getAttribute("d").split("M").length===4');
  checked('a browser path-only cache cannot mask a changed source',requests.filter(r=>r.path==='/api/waveform').length>remountRequests);
  const waveCount=requests.filter(r=>r.path==='/api/waveform').length;
  await evaluate('wave.update({file:'+JSON.stringify(other)+',clipStart:10,clipLength:20,trackKey:"cue-one",duration:20,position:0})');await check('same-file CUE interval requests its own waveform','waveHost.dataset.state==="ready"');
  checked('CUE request carries explicit source interval to the decoder',requests.slice().reverse().some(r=>r.path==='/api/waveform'&&new URLSearchParams(r.query).get('clip_start')==='10'&&new URLSearchParams(r.query).get('clip_length')==='20')&&requests.filter(r=>r.path==='/api/waveform').length>waveCount);
  const cueSeeks=await evaluate('waveSeeks.length');await evaluate('var input=waveHost.querySelector(".rlWaveSeek");input.dispatchEvent(new PointerEvent("pointerdown",{bubbles:true}));input.value=15;input.dispatchEvent(new Event("input",{bubbles:true}));wave.update({file:'+JSON.stringify(other)+',clipStart:30,clipLength:20,trackKey:"cue-two",duration:20,position:0});input.dispatchEvent(new Event("change",{bubbles:true}))');
  checked('same physical file CUE switch cancels an old seek gesture',await evaluate('waveSeeks.length')===cueSeeks);
  await evaluate('window.waveScrubs=[];wave.update({file:'+JSON.stringify(other)+',duration:120,position:0,rangeControls:false,start:20,end:60,selection:true,dragSelect:true,onScrub:s=>window.waveScrubs.push(s)})');
  // 3.3.9：在选区内部按下拖动 = 整段平移（不再是重新框选），并且拖动过程中用 onScrub 跟手预览位置。
  const segBox=await evaluate('(()=>{const r=waveHost.querySelector(".rlWavePlot").getBoundingClientRect();return{x:r.x,y:r.y,w:r.width,h:r.height}})()');
  const insideX=segBox.x+segBox.w*(40/120),midY=segBox.y+segBox.h/2;
  const beforeMove=await evaluate('JSON.stringify([waveRanges.length,waveScrubs.length])');
  await browser.send('Input.dispatchMouseEvent',{type:'mousePressed',x:insideX,y:midY,button:'left',buttons:1,clickCount:1});
  await browser.send('Input.dispatchMouseEvent',{type:'mouseMoved',x:insideX+segBox.w*(10/120),y:midY,button:'left',buttons:1});
  await browser.send('Input.dispatchMouseEvent',{type:'mouseReleased',x:insideX+segBox.w*(10/120),y:midY,button:'left',buttons:0});
  await delay(120);
  const moved=await evaluate('JSON.stringify({last:waveRanges.at(-1),scrubs:window.waveScrubs.slice()})');
  checked('dragging inside the selection translates the whole segment instead of re-selecting ('+beforeMove+' -> '+moved+')',await evaluate('(()=>{const r=waveRanges.at(-1);return !!r&&r.committed&&Math.abs((r.b-r.a)-40)<.5&&Math.abs(r.a-30)<2;})()'));
  checked('segment drag previews intermediate positions for audible scrubbing',await evaluate('window.waveScrubs.length>0&&window.waveScrubs.every(v=>v>=0&&v<=120)'));
  await evaluate('wave.update({rangeControls:false})');checked('studio can hide duplicate range controls while retaining handles',await evaluate('waveHost.querySelector(".rlWaveRange").hidden&&!waveHost.querySelector(".rlWaveStart").hidden'));
  await evaluate('wave.update({file:"",duration:0})');await check('empty track clears old bins and disables seek and selection','waveHost.dataset.state==="empty"&&waveHost.querySelector(".rlWaveSeek").disabled&&waveHost.querySelector("path").getAttribute("d")===""&&waveHost.querySelector(".rlWaveStart").disabled');
  checked('only explicit waveform retry requests a fresh error-cache attempt',requests.filter(r=>r.path==='/api/waveform'&&new URLSearchParams(r.query).get('fresh')==='1').length===1);
  await evaluate('wave.destroy();waveHost.remove()');
  scenario='navigation and monitor';await evaluate('rlSetMode("pro",false);setView("listening")');
  await until('D("rlStudio")&&D("rlStudio").checkVisibility()');
  for(const group of ['pcm','sdm','eq']){
   await evaluate('openSettingsTab('+JSON.stringify(group)+')');
   checked('old '+group+' settings link routes to the digital console',await evaluate('S.view==="listening"&&rlMode==="pro"&&!D("setpage").checkVisibility()'));
   if(await evaluate('!!D("rlStudioAdvanced")&&D("rlStudioAdvanced").open'))await evaluate('D("rlStudioAdvanced").close()');
  }
  await evaluate('openSettingsTab("out")');await check('general output settings remain available','S.view==="settings"&&!!D("sp_backend")');
  checked('settings contain no PCM DSD or EQ tab entry',await evaluate('!document.querySelector("#setpage .stab[data-g=pcm],#setpage .stab[data-g=sdm],#setpage .stab[data-g=eq]")'));
  await evaluate('setView("listening")');await click('#rlStudioTab_monitor');
  checked('monitor distinguishes source-file metrics from processed live output',await evaluate('D("rlStudioPanel_monitor").textContent.includes("以下指标反映音源文件，不是处理后的实时输出。")'));
  facts.cueStart=10;facts.cueEnd=30;await tick();await check('CUE source scope is explicitly identified in monitoring','!D("rlMonitorCueNote").hidden');
  facts.cueStart=-1;facts.cueEnd=-1;await tick();await check('whole-file playback clears the CUE-specific scope note','D("rlMonitorCueNote").hidden');
  checked('monitor does not offer a constant repeat-analysis button',await evaluate('!D("npAnlNow")||D("npAnlNow").hidden'));
  checked('complete audio analysis lives under professional sound monitoring',await evaluate('!!document.querySelector("#rlStudio #npAnal")&&!document.querySelector("#npPage #npAnal")&&typeof anlCardsHtml==="function"&&!!D("npAnlNow")'));
  await evaluate('rlSetMode("listen",false);openNowPlaying()');checked('listening detail stays free of analysis after switching modes',await evaluate('!document.querySelector("#npPage .npanal")&&!D("rlStudio").checkVisibility()'));
  checked('all production JavaScript ran without an uncaught error',errors.length===0);checked('no physical playback or outside network action was made',forbidden.length===0);
  console.log(JSON.stringify({checks:checks.length,failures:0,evidence:run}));
 }catch(error){try{const pic=await browser.send('Page.captureScreenshot',{format:'png'});fs.writeFileSync(path.join(run,'failure.png'),Buffer.from(pic.data,'base64'));}catch{}console.error({scenario,errors,forbidden,evidence:run});throw error;}
 finally{try{await browser.close();}finally{fs.writeFileSync(path.join(run,'result.json'),JSON.stringify({checks,geometry,volumeWrites,preferenceWrites,requests,errors,forbidden,aborted,scenario},null,2));}assert.deepEqual(errors,[],'DevTools fixture remains error-free through browser shutdown');}
}
main().catch(error=>{console.error(error);process.exitCode=1;});
