// Render the shipped page and script in an isolated Edge profile. Every URL is
// answered through its private DevTools pipe. No HTTP server or player is used.
'use strict';
// ⚠️ 未解决项：workspace-B0VLOf 曾有一次改名后点击上一组未发请求，未能定位。
// 随后的完整 279 断言与连续 20 轮改名/上一组均通过；以此作为当前回归基线。
// catch 中保留 switching/队列/编辑状态取证，不把该单次现象记为已定位修复。
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const {spawn, spawnSync} = require('node:child_process');
const {once} = require('node:events');
const delay = ms => new Promise(resolve => setTimeout(resolve, ms));
function option(name, fallback) {
  const i = process.argv.indexOf(name);
  return i < 0 ? fallback : process.argv[i + 1];
}
const root = path.resolve(option('--source', process.env.RLPLAYER_TEST_SOURCE_ROOT || path.join(__dirname, '..')));
const workRoot = path.resolve(option('--work-root', 'workspace335-evidence'));
fs.mkdirSync(workRoot, {recursive:true});
const run = fs.mkdtempSync(path.join(workRoot, 'workspace-'));
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
  const pending=new Map(); let seq=0, buffer=Buffer.alloc(0), sessionId;
  const closed=once(proc,'exit');
  function send(method,params={},session=sessionId) {
    return new Promise((resolve,reject)=>{
      // 2026-11-14（3.3.11）：8 s 对冷启动的无头 Edge 太紧 —— 机器上有其它活时 Page.navigate
 // 会在 8 s 处被掐断（实测门禁里随机挂在 controls336 / workspace335_integration）。
 // 放宽到 30 s：真挂死仍会被 ctest 的 TIMEOUT 收掉，不会变成静默假绿。
  const id=++seq;const timeout=setTimeout(()=>{pending.delete(id);reject(Error('DevTools timed out on '+method));},30000);
      pending.set(id,{resolve,reject,timeout});
      proc.stdio[3].write(JSON.stringify(session ? {id,method,params,sessionId:session} : {id,method,params})+'\0');
    });
  }
  const api={send,async attach(){const {targetId}=await send('Target.createTarget',{url:'about:blank'});const result=await send('Target.attachToTarget',{targetId,flatten:true});sessionId=result.sessionId;},async close(){if(proc.exitCode===null){try{await send('Browser.close',{},null);}catch{}proc.stdio[3].end();}await closed;},onRequest:null};
  proc.stdio[4].on('data',chunk=>{
    buffer=Buffer.concat([buffer,chunk]);
    let split;
    while((split=buffer.indexOf(0))!==-1){
      const msg=JSON.parse(buffer.subarray(0,split).toString('utf8'));buffer=buffer.subarray(split+1);
      if(msg.id&&pending.has(msg.id)){const p=pending.get(msg.id);pending.delete(msg.id);clearTimeout(p.timeout);msg.error?p.reject(Error(msg.error.message)):p.resolve(msg.result);}
      else if(msg.method==='Runtime.exceptionThrown')errors.push(msg.params.exceptionDetails.exception?.description||msg.params.exceptionDetails.text);
      else if(msg.method==='Fetch.requestPaused'&&api.onRequest)api.onRequest(msg.params).catch(e=>{errors.push(String(e));});
    }
  });
  return api;
}
const settings={quickstart:'1',uilang:'zh',filter:'poly-sinc-short-lp',filterNx:'',backend:'null',device:'null',volume:'-64.5',rate:'88200',dsd:'0',dsdnative:'0',twostage:'auto',shaper:'auto',modfamily:'dsd',modorder:'5',sdmfilt:'',sdmfiltnx:'',directsdm:'0',gpuvendor:'off',metaon:'0',metallm:'0',anlmaster:'1',autoanl:'1',roomcorrauto:'0',ui_profile:'pcm',ui_style:'balanced'};
const mutations=[],volumeWrites=[],preferenceWrites=[],requests=[],forbidden=[],errors=[],checks=[],geometry=[];
const facts={language:'zh',files:[fixtureTracks[0]],current:fixtureTracks[0],paused:false,playing:true,profile:'pcm',style:'balanced',revision:1,allowWorkbench:false,failWorkbench:false,rates:[44100,48000],rawVolumeLocked:false,volumeWritable:true,actualVolume:-64.5,allowVolume:false};
const health={status:'ready',file:fixtureTracks[0],rate:44100,bits:16,channels:2,duration:180,dr_db:12.4,lufs:-17.2,lra:8.3,peak_dbfs:-1,true_peak_dbtp:-.8,cutoff_hz:21500,fake_verdict:'none',quality:{available:true,clipped_runs:0,clipped_pct:0,snr_db:82,crest_db:13,dc_dbfs:-120,bandwidth_hz:21500,spectral_centroid_hz:3400,spectral_rolloff_hz:16000,spectral_flatness:.1,dyn_rms_db:-22}};
let peq={bypass:1,preamp:0,name:'Flat fixture',bands:Array.from({length:20},(_,i)=>({i:i+1,on:1,type:'PK',freq:i?1000+i*200:80,gain:0,q:1})),points:[[20,0],[1000,0],[20000,0]]};
const processingKeys=['filter','filterNx','rate','rateFamilyPolicy','dsd','dsdnative','directsdm','shaper','twostage','taps','atten','transition','phase','modorder','modfamily','modcut','moddith','oobgain','ahmbits','eq','ir','irgpu','roomcorrauto','isp','loudness','cleanup','cleanup_auto','cleanup_strength','cleanup_noise_db','sdmfilt','sdmfiltnx','dsdkind','dsdconv','dsd2stage','bits','channels','trim','rgmode','rgclip'];
const profileServer={active:'A',slots:['A','B','C','D'].map(id=>({id,name:id,settings:Object.fromEntries(processingKeys.filter(k=>settings[k]!==undefined).map(k=>[k,settings[k]])),peq:JSON.parse(JSON.stringify(peq)),eqId:'',eqName:''})),loop:{enabled:false,start:0,end:180,file:facts.current}};
const eqBank=Array.from({length:16},(_,i)=>({id:String(i+1),name:'',peq:null,source:'custom'}));
function snapshotActive(){const slot=profileServer.slots.find(s=>s.id===profileServer.active);slot.settings=Object.fromEntries(processingKeys.filter(k=>settings[k]!==undefined).map(k=>[k,settings[k]]));slot.peq=JSON.parse(JSON.stringify(peq));}
function workbench(){return {ok:true,settings:{...settings},profile:facts.profile,style:facts.style,revision:String(facts.revision),playing:facts.playing,paused:facts.paused,file:facts.current,position:12.75,duration:180,queueIndex:0,cueStart:-1,cueEnd:-1,volumeWritable:facts.volumeWritable,rawVolumeLocked:facts.rawVolumeLocked,peq:JSON.parse(JSON.stringify(peq)),actual:{outRate:Number(settings.rate)||44100,outBits:24,device:'Null fixture'}};}
function mock(url,method,body){
 const p=url.pathname;requests.push({path:p,method,query:url.search});
 if(p==='/api/waveform')return {ok:true,state:'ready',duration:180,bins:[0,.1,.2,.3,.4,.5,.6,.8,.5,.2]};
 if(p==='/api/workbench/profiles')return {ok:true,...JSON.parse(JSON.stringify(profileServer))};
 if(p==='/api/workbench/profile'){
  const patch=Object.fromEntries(new URLSearchParams(body));
  if(!facts.allowWorkbench){forbidden.push('unexpected '+p);return {ok:false,error:'Unapproved fixture mutation'};}
  mutations.push({endpoint:p,...patch});if(facts.failWorkbench)return {ok:false,error:'Fixture device unavailable'};
  const target=profileServer.slots.find(s=>s.id===patch.id);if(!target)return {ok:false,error:'Unknown slot'};
  if(patch.action==='rename'){target.name=patch.name;return {ok:true};}
  if(patch.action==='select'){snapshotActive();profileServer.active=target.id;Object.assign(settings,target.settings);peq=JSON.parse(JSON.stringify(target.peq));facts.revision++;return {ok:true};}
  if(patch.id!==profileServer.active)return {ok:false,error:'Wrong active slot'};
  for(const [key,value] of Object.entries(patch)){if(key==='peq')peq={...peq,...JSON.parse(value)};else if(processingKeys.includes(key))settings[key]=value;}
  snapshotActive();facts.revision++;return {ok:true};
 }
 if(p==='/api/workbench/loop'){
  const patch=Object.fromEntries(new URLSearchParams(body));if(patch.expected_revision&&patch.expected_revision!==String(facts.revision))return {ok:false,error:'Revision changed',__fixtureStatus:409};
  profileServer.loop={enabled:patch.enabled==='1',repeat:patch.repeat!=='0',start:Number(patch.start),end:Number(patch.end),file:patch.file};mutations.push({endpoint:p,...patch});return {ok:true};
 }
 if(p==='/api/workbench/eqpresets'&&method!=='POST')return {ok:true,slots:JSON.parse(JSON.stringify(eqBank))};
 if(p==='/api/workbench/eqpresets'){
  const patch=Object.fromEntries(new URLSearchParams(body)),slot=eqBank.find(s=>s.id===patch.id);if(!slot)return {ok:true,slots:JSON.parse(JSON.stringify(eqBank))};
  if(facts.failWorkbench)return {ok:false,error:'Fixture EQ failed'};mutations.push({endpoint:p,...patch});
  if(patch.action==='save'){slot.name=patch.name;slot.peq=JSON.parse(patch.peq);slot.source=patch.source||'custom';}
  if(patch.action==='load'&&slot.peq){peq=JSON.parse(JSON.stringify(slot.peq));snapshotActive();const active=profileServer.slots.find(s=>s.id===profileServer.active);active.eqId=slot.id;active.eqName=slot.name;facts.revision++;}
  if(patch.action==='delete'){slot.peq=null;slot.name='';}
  return {ok:true};
 }

 if(p==='/api/peq'&&!body)return {ok:true,...peq,bypass:peq.bypass?1:0,fs:44100,points:peq.bands.map(function(b,i){return [20+i*10,b.gain]})};
 if(p==='/api/peq'){
  const patch=Object.fromEntries(new URLSearchParams(body));if(!facts.allowWorkbench){forbidden.push('unexpected '+p+' body='+String(body).slice(0,300));return {ok:false,error:'Unapproved fixture mutation'};}mutations.push({endpoint:p,...patch});
  if(facts.failWorkbench)return {ok:false,error:'Fixture EQ failed'};
  peq.bypass=patch.bypass==='1';peq.preamp=Number(patch.preamp)||0;
  if(patch.bands){peq.bands=patch.bands.split(';').filter(Boolean).map(function(item){var a=item.split(',');return {i:Number(a[0]),type:a[1],freq:Number(a[2]),gain:Number(a[3]),q:Number(a[4]),on:!a[5]};});}
  snapshotActive();facts.revision++;return {ok:true,...peq,bypass:peq.bypass?1:0,fs:44100,points:peq.bands.map(function(b,i){return [20+i*10,b.gain]})};
 }
 if(p==='/api/workbench/apply'){
  if(!facts.allowWorkbench){forbidden.push('unexpected '+p);return {ok:false,error:'unapproved fixture mutation'};}
  const patch=Object.fromEntries(new URLSearchParams(body));mutations.push(patch);
  assert.equal(patch.expected_file,facts.current);assert.equal(patch.expected_revision,String(facts.revision));
  if(facts.failWorkbench)return {ok:false,error:'Fixture device unavailable'};
  for(const [key,value] of Object.entries(patch)){
   if(key==='peq')peq={...peq,...JSON.parse(value)};
   else if(!['expected_file','expected_revision','profile','style','position'].includes(key))settings[key]=value;
  }
  if(patch.profile){facts.profile=patch.profile;settings.ui_profile=patch.profile;settings.rate=patch.profile==='bitperfect'?'src':'176400';settings.filter=patch.profile==='bitperfect'?'':'poly-sinc-short-lp';settings.dsd=patch.profile==='dsd'?'128':'0';}
  if(patch.style){facts.style=patch.style;settings.ui_style=patch.style;settings.filter={warm:'poly-sinc-gauss-medium',balanced:'poly-sinc-short-lp',sharp:'poly-sinc-long-lp'}[patch.style];}
  snapshotActive();facts.revision++;return {ok:true};
 }
 if(p==='/api/volume'&&facts.allowVolume){
  if(facts.volumePlans&&facts.volumePlans.length){
   const plan=facts.volumePlans.shift(),db=Number(url.searchParams.get('db'));
   volumeWrites.push({db,rejected:plan.status!==200,planned:plan.status});
   if(plan.status===409&&plan.lockRaw!==false){facts.rawVolumeLocked=true;facts.volumeWritable=false;facts.actualVolume=0;settings.volume='0';}
   else if(plan.status===200){facts.rawVolumeLocked=false;facts.volumeWritable=true;facts.actualVolume=db;}
   return {ok:plan.status===200,db,error:plan.status===409?'原始 DSD 码流不支持软件音量':'',__fixtureDelay:plan.delay||0,__fixtureStatus:plan.status===409?409:200,__fixtureNetwork:plan.status==='network'};
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
  case '/api/status':return {ok:true,playing:facts.playing,paused:facts.paused,current:facts.current,rate:44100,dur:180,pos:12.75,volDb:facts.actualVolume,startDb:facts.actualVolume,volumeWritable:facts.volumeWritable,rawVolumeLocked:facts.rawVolumeLocked,outRate:Number(settings.rate)||44100,outBits:24,outMode:'pcm',bitPerfect:'no',resample:true,error:'',gpu:false,summaryEnabled:false,hasLlmKey:false};
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
function renderedTextContrast(selector){
 const element=document.querySelector(selector);
 if(!element||!element.checkVisibility())throw Error('Contrast target is hidden: '+selector);
 function rgba(value){return (value.match(/[\d.]+/g)||[]).map(Number);}
 function luminance(channels){
  return channels.map(value=>{value/=255;return value<=.04045?value/12.92:Math.pow((value+.055)/1.055,2.4);})
   .reduce((sum,value,index)=>sum+value*[.2126,.7152,.0722][index],0);
 }
 const layers=[];let opacity=1;
 for(let node=element;node;node=node.parentElement){
  const css=getComputedStyle(node);
  if(css.backgroundImage!=='none')throw Error('Contrast measurement requires the actual solid background: '+selector);
  opacity*=Number(css.opacity);
  layers.push(rgba(css.backgroundColor));
 }
 let background=[255,255,255];
 layers.reverse().forEach(channels=>{
  const alpha=channels.length>3?channels[3]:1;
  background=background.map((value,index)=>channels[index]*alpha+value*(1-alpha));
 });
 const style=getComputedStyle(element),color=rgba(style.color),alpha=color.length>3?color[3]:1;
 const foreground=background.map((value,index)=>color[index]*alpha+value*(1-alpha));
 const first=luminance(foreground),second=luminance(background);
 return {selector,text:element.textContent,color:style.color,background,opacity,fontSize:style.fontSize,ratio:(Math.max(first,second)+.05)/(Math.min(first,second)+.05)};
}
async function main(){
 const assets=exportPage(),browser=startBrowser();let scenario='startup';
 const listen='#rlViewModes [data-rl-mode=listen]',pro='#rlViewModes [data-rl-mode=pro]';
 async function evaluate(expression){const r=await browser.send('Runtime.evaluate',{expression,awaitPromise:true,returnByValue:true});if(r.exceptionDetails)throw Error(r.exceptionDetails.exception?.description||r.exceptionDetails.text);return r.result.value;}
 async function until(expression,label){const end=Date.now()+10000;while(Date.now()<end){if(await evaluate('Boolean('+expression+')'))return;await delay(45);}throw Error('Not ready '+(label||expression));}
 async function check(label,expression){await until(expression,label);checked(label,true);}
 async function click(selector){
  await until('document.querySelector('+JSON.stringify(selector)+')',selector);
  const hit=await evaluate('(()=>{const e=document.querySelector('+JSON.stringify(selector)+');e.scrollIntoView({block:"nearest",inline:"nearest"});const r=e.getBoundingClientRect(),x=r.x+r.width/2,y=r.y+r.height/2,h=document.elementFromPoint(x,y);return {x,y,w:r.width,h:r.height,hit:!!h&&(h===e||e.contains(h)),label:e.textContent.slice(0,60),disabled:!!e.disabled}})()');
  assert.ok(hit.w>0&&hit.h>0&&hit.hit&&!hit.disabled,'Clickable '+selector+' '+JSON.stringify(hit));
  await browser.send('Input.dispatchMouseEvent',{type:'mousePressed',x:hit.x,y:hit.y,button:'left',clickCount:1});await browser.send('Input.dispatchMouseEvent',{type:'mouseReleased',x:hit.x,y:hit.y,button:'left',clickCount:1});
 }
 async function select(id,value){await evaluate('var el=D('+JSON.stringify(id)+');el.value='+JSON.stringify(value)+';el.dispatchEvent(new Event("change",{bubbles:true}))');}
 async function shot(name){const picture=await browser.send('Page.captureScreenshot',{format:'png',captureBeyondViewport:false});fs.writeFileSync(path.join(run,name+'.png'),Buffer.from(picture.data,'base64'));}
 async function dynamicAgreement(language){
  const cases=[
   [null,'动态：样本太短，未出结论','mut'],[0,'动态：样本太短，未出结论','mut'],
   [5.99,'强压缩 · 几乎全程顶满','warn'],[6,'偏压缩 · 响度战争母带','warn'],[8.99,'偏压缩 · 响度战争母带','warn'],
   [9,'中等动态 · 流行/摇滚常见',''],[9.99,'中等动态 · 流行/摇滚常见',''],[10,'中等动态 · 流行/摇滚常见',''],
   [10.87,'中等动态 · 流行/摇滚常见',''],[13.99,'中等动态 · 流行/摇滚常见',''],
   [14,'宽动态 · 轻响层次清楚','ok'],[14.01,'宽动态 · 轻响层次清楚','ok'],[20,'宽动态 · 轻响层次清楚','ok']
  ];
  const read=selector=>evaluate('(()=>{const e=document.querySelector('+JSON.stringify(selector)+');return {text:e.textContent,kind:["mut","warn","ok"].find(c=>e.classList.contains(c))||""}})()');
  for(const [dr,label,kind] of cases){
   health.dr_db=dr;
   await evaluate('anlRun(ANL.cur,true)');await until('ANL.cache[ANL.cur].dr_db==='+JSON.stringify(dr)+'&&!ANL.busy[ANL.cur]');
   await evaluate('anlSetLvl(0)');const expected=await evaluate('t('+JSON.stringify(label)+')');
   await until('D("npAnlDynSummary").querySelector("strong").textContent==='+JSON.stringify(expected));
   const summary=await read('#npAnlDynSummary strong');
   await evaluate('anlSetLvl(2)');await until('D("npAnlGrid").querySelector("#npAnlDyn .npanal-verdict").textContent==='+JSON.stringify(expected));
   const expanded=await read('#npAnlGrid #npAnlDyn .npanal-verdict');
   await evaluate('openHealthModal(ANL.cur)');await until('D("mbody").querySelector("#npAnlDyn .npanal-verdict")&&D("mbody").querySelector("#npAnlDyn .npanal-verdict").textContent==='+JSON.stringify(expected));
   const context=await read('#mbody #npAnlDyn .npanal-verdict');
   geometry.push({language,dr,expected,kind,summary,expanded,context});
   checked('DR '+String(dr)+' has one established verdict across all analysis entries '+language,
     [summary,expanded,context].every(result=>result.text===expected&&result.kind===kind));
   await evaluate('D("mclose").click();anlSetLvl(0)');
  }
  health.dr_db=12.4;await evaluate('anlRun(ANL.cur,true)');await until('ANL.cache[ANL.cur].dr_db===12.4&&!ANL.busy[ANL.cur]');
 }
 try{
  await browser.attach();await browser.send('Page.enable');await browser.send('Runtime.enable');
  browser.onRequest=async({requestId,request})=>{
   const url=new URL(request.url);let type='application/json',body='',code=200;
   if(url.origin!==origin){forbidden.push('external '+url.href);code=403;}
   else if(url.pathname==='/'){type='text/html; charset=utf-8';body=assets.html.replace('FILTER_OPTS_1X','<option value=auto>Auto</option><option value=poly-sinc-short-lp>Short</option><option value=poly-sinc-gauss-medium>Warm</option><option value=poly-sinc-long-lp>Sharp</option>').replace('FILTER_OPTS_NX','<option value="">Follow 1x</option><option value=poly-sinc-long-lp>Sharp Nx</option>').replace('SHAPER_OPTS','<option value=auto>Auto</option><option value=none>None</option>');}
   else if(url.pathname==='/app.js'){type='application/javascript; charset=utf-8';body=assets.js;}
   else if(url.pathname==='/api/cover'){type='image/png';code=404;}
   else if(url.pathname.startsWith('/api/')){
    const response=mock(url,request.method,request.postData),lag=response.__fixtureDelay||(url.pathname==='/api/settings'?facts.preferenceDelay||0:0),failed=response.__fixtureNetwork;
    if(response.__fixtureStatus)code=response.__fixtureStatus;
    delete response.__fixtureDelay;delete response.__fixtureNetwork;delete response.__fixtureStatus;
    body=JSON.stringify(response);
    if(url.pathname==='/api/volume'&&response.ok===false)code=409;
    if(lag)await delay(lag);
    if(failed){await browser.send('Fetch.failRequest',{requestId,errorReason:'ConnectionReset'});return;}
    if(url.pathname==='/api/rates'&&facts.rateDelay)await delay(facts.rateDelay);
   }
   else code=404;
   await browser.send('Fetch.fulfillRequest',{requestId,responseCode:code,responseHeaders:[{name:'Content-Type',value:type},{name:'Cache-Control',value:'no-store'}],body:Buffer.from(body).toString('base64')});
  };
  await browser.send('Fetch.enable',{patterns:[{urlPattern:'*',requestStage:'Request'}]});
  await browser.send('Emulation.setDeviceMetricsOverride',{width:1340,height:880,deviceScaleFactor:1,mobile:false});
  await browser.send('Page.navigate',{url:origin+'/'});await until('typeof PLAYBACK_SETTINGS!=="undefined"&&PLAYBACK_SETTINGS.ready&&typeof rlConsole!=="undefined"');
  await click('.side .nav[data-v=listening]');await until('D("rlListenHome")&&rlConsole.state');await click(pro);
  await check('professional mode reaches the real digital console','rlMode==="pro"&&D("rlStudio").checkVisibility()&&D("rlHomeNavLabel").textContent==="数字音乐台"');
  if(option('--compare-stress-only','')==='1'){
   facts.allowWorkbench=true;
   for(let i=0;i<20;i++){
    await click('[data-studio-slot=B]');await until('rlCompare.activeId==="B"&&!RL_STUDIO.pending&&!RL_STUDIO.switching&&rlWorkbenchQueue.pending===0');
    await evaluate('document.querySelector("[data-slot-name=B]").dispatchEvent(new MouseEvent("dblclick",{bubbles:true}));var stressField=document.querySelector("[data-slot-name=B] input");stressField.value='+JSON.stringify('Stress '+i)+';stressField.dispatchEvent(new KeyboardEvent("keydown",{key:"Enter",bubbles:true}))');
    await until('rlCompare.slots[1].name==='+JSON.stringify('Stress '+i)+'&&!RL_STUDIO.pending&&!rlCompare.saving');
    const count=mutations.length;await click('[data-studio-slot=A]');
    await until('rlCompare.activeId==="A"&&!RL_STUDIO.pending&&!RL_STUDIO.switching');
    checked('rename B then previous selects A without swallowing the request '+i,mutations.slice(count).some(p=>p.endpoint==='/api/workbench/profile'&&p.action==='select'&&p.id==='A'));
   }
   console.log(JSON.stringify({checks:checks.length,evidence:run}));return;
  }
  await check('professional mode keeps the system section title','D("rlSystemSection")&&D("rlSystemSection").checkVisibility()');
  await check('no leftover quick-start module is mounted','!D("rlQuickSetup")&&(!D("navQuickStart")||!D("navQuickStart").checkVisibility())');
  await check('listening hero replaces its duplicate playback button with a favorite heart','!D("rlListenPlay")&&!!D("rlListenFavorite")');
  const orphan=await evaluate('Array.from(document.querySelectorAll("#list button")).filter(e=>e.checkVisibility()&&/开始播放|Start playback|Start playing/.test(e.textContent)&&!e.closest("#rlStudio")).map(e=>({id:e.id,text:e.textContent}))');
  checked('professional homepage has no isolated start-playback button',orphan.length===0);
  await check('comparison slots initialize from the server baseline without fabricated presets','rlCompare.slots.length===4&&rlCompare.slots.every(x=>x.settings.filter==="poly-sinc-short-lp")&&rlCompare.activeId==="A"');
  await check('PCM DSD output and full EQ read real shared state','D("rlStudioFilter1").value===rlConsole.state.settings.filter&&D("rlStudioModFamily").value==="dsd"&&!!D("rlEqEnabled")&&!D("rlEqEnabled").checked&&D("rlEqRows").querySelectorAll(".rlEqr").length===20');
  checked('opening modes makes no parameter or audio writes',mutations.length===0&&forbidden.length===0);
  scenario='raw DSD actual software-volume lock';
  facts.rawVolumeLocked=true;facts.volumeWritable=false;facts.actualVolume=0;settings.volume='0';
  await evaluate('loadSettings()');
  const rawStatusBefore=await evaluate('statusApplied');await evaluate('tick()');await until('statusApplied>'+rawStatusBefore);
  const rawControls=await evaluate('({bar:D("vol").disabled,nowPlaying:D("npVol").disabled,floor:D("volFloor").disabled,barValue:Number(D("vol").value),npValue:Number(D("npVol").value),floorPressed:D("volFloor").getAttribute("aria-pressed")})');
  geometry.push({rawControls});console.log(JSON.stringify({rawControls}));
  checked('actual raw DSD locks both sliders and cannot offer false software mute',rawControls.bar&&rawControls.nowPlaying&&rawControls.floor&&rawControls.barValue===0&&rawControls.npValue===0&&rawControls.floorPressed==='false');
  facts.paused=true;const pausedRawBefore=await evaluate('statusApplied');await evaluate('tick()');await until('statusApplied>'+pausedRawBefore);
  await check('a paused raw session keeps software volume and mute locked',
    'S.paused&&D("vol").disabled&&D("npVol").disabled&&D("volFloor").disabled&&D("vol").value==="0"');
  facts.paused=false;
  facts.allowVolume=true;
  await evaluate('D("vol").value=-6;D("vol").dispatchEvent(new Event("input",{bubbles:true}));D("npVol").value=-12;D("npVol").dispatchEvent(new Event("input",{bubbles:true}));D("volFloor").click();D("volFloor").onclick();document.body.dispatchEvent(new KeyboardEvent("keydown",{key:"ArrowDown",bubbles:true}));document.body.dispatchEvent(new KeyboardEvent("keydown",{key:"ArrowUp",bubbles:true}));setVol(-24);updatePlaybackSettingsGate();syncVolUi(-35)');
  await delay(60);
  checked('raw sliders mute gestures shortcuts and direct setVol never submit attenuation',volumeWrites.length===0&&await evaluate('S.vol===0&&D("vol").value==="0"&&D("npVol").value==="0"&&D("volFloor").disabled&&D("volFloor").getAttribute("aria-pressed")==="false"&&volumeRestore===null'));
  await check('raw volume explains hardware control and a processable alternative',
    '!D("volumePending").hidden&&D("volumePending").textContent==="设备调音"&&D("volumePending").title.includes("设备音量")&&D("rlStudioRawVolumeNotice").textContent.includes("PCM")&&!D("rlStudioRawVolumeNotice").hidden');
  await shot('raw-volume-professional');
  facts.rawVolumeLocked=undefined;facts.volumeWritable=undefined;facts.actualVolume=-64.5;settings.volume='-64.5';settings.dsdnative='1';settings.dsd='128';
  await evaluate('loadSettings()');const unknownVolumeBefore=await evaluate('statusApplied');await evaluate('tick()');await until('statusApplied>'+unknownVolumeBefore);
  await check('missing raw flags do not infer a lock from Native configuration',
    '!D("vol").disabled&&!D("npVol").disabled&&!D("volFloor").disabled&&D("volumePending").hidden&&D("vol").value==="-64.5"');
  settings.dsdnative='0';settings.dsd='0';facts.rawVolumeLocked=false;facts.volumeWritable=true;
  await evaluate('loadSettings()');const writableBefore=await evaluate('statusApplied');await evaluate('tick()');await until('statusApplied>'+writableBefore);
  facts.rejectVolume=true;const requestsBeforeRejection=requests.length;
  await evaluate('setVol(-30)');
  await check('a raw startup rejection rolls back optimistic gain to authoritative zero',
    'D("vol").disabled&&D("npVol").disabled&&D("volFloor").disabled&&S.vol===0&&D("vol").value==="0"&&D("npVol").value==="0"&&D("volFloor").getAttribute("aria-pressed")==="false"');
  checked('the rejected volume request is not retried and status is reread with GET',
    volumeWrites.length===1&&volumeWrites[0].db===-30&&volumeWrites[0].rejected&&requests.slice(requestsBeforeRejection).some(request=>request.path==='/api/status'&&request.method==='GET'));
  facts.rejectVolume=false;facts.rawVolumeLocked=false;facts.volumeWritable=true;facts.playing=false;
  const stoppedBefore=await evaluate('statusApplied');await evaluate('tick()');await until('statusApplied>'+stoppedBefore);
  await check('an explicit stopped session unlocks volume without restoring stale attenuation',
    '!D("vol").disabled&&!D("npVol").disabled&&D("vol").value==="0"&&D("volFloor").getAttribute("aria-pressed")==="false"');
  facts.playing=true;await evaluate('setVol(-65.5)');
  checked('a regular processable path still accepts software volume',volumeWrites.length===2&&!volumeWrites[1].rejected&&facts.actualVolume===-65.5&&await evaluate('D("vol").value==="-65.5"&&!D("vol").disabled'));
  facts.allowVolume=false;
  facts.rawVolumeLocked=false;facts.volumeWritable=true;facts.actualVolume=-64.5;settings.volume='-64.5';
  await evaluate('loadSettings()');const ordinaryStatusBefore=await evaluate('statusApplied');await evaluate('tick()');await until('statusApplied>'+ordinaryStatusBefore);
  scenario='real slider input-change persistence';
  facts.allowVolume=true;facts.allowPreferences=true;
  const rejectedGestures=[];
  for(const id of ['vol','npVol']){
   facts.rawVolumeLocked=false;facts.volumeWritable=true;facts.actualVolume=-64.5;settings.volume='-64.5';
   await evaluate('loadSettings()');const beforeStatus=await evaluate('statusApplied');await evaluate('tick()');await until('statusApplied>'+beforeStatus);
   const beforePreferences=preferenceWrites.length;facts.volumePlans=[{status:409,delay:550}];
   await evaluate('var slider=D('+JSON.stringify(id)+');slider.value=-70;slider.dispatchEvent(new Event("input",{bubbles:true}));slider.dispatchEvent(new Event("change",{bubbles:true}))');
   await delay(750);
   rejectedGestures.push({slider:id,settings:preferenceWrites.slice(beforePreferences),saved:settings.volume,actual:facts.actualVolume});
  }
  geometry.push({rejectedGestures});console.log(JSON.stringify({rejectedGestures}));
  checked('both actual input-change gestures refuse to persist a rejected gain',rejectedGestures.every(result=>result.saved==='0'&&!result.settings.some(patch=>patch.volume==='-70')));
  const initialPreferences={...settings,volume:'-64.5'};
  async function waitFixture(predicate,label){const end=Date.now()+10000;while(!predicate()&&Date.now()<end)await delay(15);assert.ok(predicate(),label);}
  async function prepareVolumeGesture(id){
   facts.rawVolumeLocked=false;facts.volumeWritable=true;facts.actualVolume=-64.5;settings.volume='-64.5';facts.volumePlans=[];facts.preferenceDelay=0;
   await evaluate('loadSettings()');const before=await evaluate('statusApplied');await evaluate('tick()');await until('statusApplied>'+before);
   await evaluate(id==='npVol'?'openNowPlaying()':'closeNowPlaying()');
  }
  async function sliderGesture(id,value,release=true){
   await evaluate('var slider=D('+JSON.stringify(id)+');slider.value='+value+';slider.dispatchEvent(new Event("input",{bubbles:true}));'+(release?'slider.dispatchEvent(new Event("change",{bubbles:true}))':''));
  }
  for(const id of ['vol','npVol']){
   await prepareVolumeGesture(id);let p=preferenceWrites.length,v=volumeWrites.length;
   facts.volumePlans=[{status:200,delay:550}];await sliderGesture(id,-68.5);
   await waitFixture(()=>volumeWrites.length===v+1,'one real volume input '+id);await delay(350);
   checked('a released '+id+' cannot persist its preview before delayed 200',preferenceWrites.slice(p).every(patch=>patch.volume===undefined)&&settings.volume==='-64.5');
   await waitFixture(()=>preferenceWrites.slice(p).some(patch=>patch.volume==='-68.5'),'accepted release persisted '+id);
   await evaluate('D('+JSON.stringify(id)+').dispatchEvent(new Event("change",{bubbles:true}));D('+JSON.stringify(id)+').dispatchEvent(new Event("change",{bubbles:true}))');await delay(60);
   checked('a confirmed '+id+' release persists once without repeating the volume API',
     volumeWrites.length===v+1&&preferenceWrites.slice(p).filter(patch=>patch.volume==='-68.5').length===1&&settings.volume==='-68.5'&&facts.actualVolume===-68.5);
   await prepareVolumeGesture(id);p=preferenceWrites.length;v=volumeWrites.length;
   facts.volumePlans=[{status:200,delay:40}];await sliderGesture(id,-66.5,false);await delay(120);
   checked('a successful '+id+' input still waits for the real release to persist',preferenceWrites.length===p&&settings.volume==='-64.5');
   await evaluate('D('+JSON.stringify(id)+').dispatchEvent(new Event("change",{bubbles:true}))');
   await waitFixture(()=>settings.volume==='-66.5','ack before release persisted '+id);
   checked('a '+id+' release after 200 persists its confirmed value once',volumeWrites.length===v+1&&preferenceWrites.slice(p).filter(patch=>patch.volume==='-66.5').length===1);
   await prepareVolumeGesture(id);p=preferenceWrites.length;v=volumeWrites.length;
   facts.volumePlans=[{status:'network',delay:550}];await sliderGesture(id,-72.5);await delay(750);
   checked('a failed '+id+' input-change never persists or retries the failed gain',
     settings.volume==='-64.5'&&volumeWrites.length===v+1&&!preferenceWrites.slice(p).some(patch=>patch.volume==='-72.5')&&await evaluate('D("vol").value==="-64.5"&&D("npVol").value==="-64.5"'));
  }
  for(const immediate of [true,false]){
   await prepareVolumeGesture('vol');const p=preferenceWrites.length;
   facts.volumePlans=[{status:200,delay:650}];await sliderGesture('vol',-62.5);
   if(immediate)await evaluate('D("sLoud").checked=!D("sLoud").checked;S.loud=D("sLoud").checked;saveSettings(true)');
   else await evaluate('D("sLoud").checked=!D("sLoud").checked;D("sLoud").dispatchEvent(new Event("change",{bubbles:true}))');
   await delay(350);
   checked('other '+(immediate?'immediate':'debounced')+' settings save during a pending gesture without its volume',
     preferenceWrites.length>p&&preferenceWrites.slice(p).every(patch=>patch.volume===undefined)&&settings.volume==='-64.5');
   await waitFixture(()=>settings.volume==='-62.5','confirmed gesture after other settings save');
   checked('the final accepted gain persists after '+(immediate?'immediate':'debounced')+' unrelated settings',preferenceWrites.slice(p).filter(patch=>patch.volume==='-62.5').length===1);
  }
  await prepareVolumeGesture('vol');{
   const p=preferenceWrites.length;facts.volumePlans=[{status:409,delay:650}];
   await evaluate('D("sLoud").checked=!D("sLoud").checked;D("sLoud").dispatchEvent(new Event("change",{bubbles:true}))');
   await sliderGesture('vol',-74);await delay(350);
   checked('an older captured debounce body is sanitized when volume becomes pending',
     preferenceWrites.length>p&&preferenceWrites.slice(p).every(patch=>patch.volume===undefined||patch.volume==='0')&&settings.volume==='0');
   await delay(400);
  }
  for(const status of [200,409,'network']){
   await prepareVolumeGesture('vol');const p=preferenceWrites.length,v=volumeWrites.length;
   facts.volumePlans=[{status,delay:650,lockRaw:false},{status:200,delay:100}];
   await sliderGesture('vol',-75);await waitFixture(()=>volumeWrites.length===v+1,'first overlapping gesture');
   await evaluate('openNowPlaying()');await sliderGesture('npVol',-55);await delay(800);
   checked('an expired '+status+' reply cannot persist or restore the preceding gesture',
     volumeWrites.length===v+2&&settings.volume==='-55'&&preferenceWrites.slice(p).filter(patch=>patch.volume==='-55').length===1&&!preferenceWrites.slice(p).some(patch=>patch.volume==='-75')&&await evaluate('D("vol").value==="-55"&&D("npVol").value==="-55"&&!D("vol").disabled'));
  }
  await prepareVolumeGesture('vol');{
   const p=preferenceWrites.length,v=volumeWrites.length;facts.preferenceDelay=550;
   await evaluate('saveSettings(true);true');await waitFixture(()=>preferenceWrites.length>p,'unrelated preference queue starts');facts.preferenceDelay=0;
   facts.volumePlans=[{status:200,delay:30},{status:200,delay:50}];
   await sliderGesture('vol',-76);await delay(80);await sliderGesture('npVol',-56);await delay(750);
   checked('a superseded confirmed gesture queued behind settings cannot write its old volume',
     volumeWrites.length===v+2&&!preferenceWrites.slice(p).some(patch=>patch.volume==='-76')&&preferenceWrites.slice(p).filter(patch=>patch.volume==='-56').length===1&&settings.volume==='-56');
  }
  Object.keys(settings).forEach(key=>delete settings[key]);Object.assign(settings,initialPreferences);await evaluate('closeNowPlaying()');
  facts.allowVolume=false;facts.allowPreferences=false;facts.rawVolumeLocked=false;facts.volumeWritable=true;facts.actualVolume=-64.5;settings.volume='-64.5';
  await evaluate('loadSettings()');const persistenceStatusBefore=await evaluate('statusApplied');await evaluate('tick()');await until('statusApplied>'+persistenceStatusBefore);
  scenario='actual output and analysis agreement';
  await evaluate('loadRates()');await delay(150);await evaluate('rlStudioSync()');
  const rateLabel=await evaluate('({value:D("rlStudioPcmRate").value,label:D("rlStudioPcmRate").selectedOptions[0].textContent,disabled:D("rlStudioPcmRate").selectedOptions[0].disabled,actual:rlConsole.state.actual.outRate})');
  health.dr_db=10.87;
  await evaluate('closeNowPlaying();rlStudioTab("monitor");anlRun(ANL.cur,true)');await until('ANL.cache[ANL.cur]&&ANL.cache[ANL.cur].dr_db===10.87&&!ANL.busy[ANL.cur]');
  const summary=await evaluate('D("npAnlDynSummary").querySelector("strong").textContent');
  await evaluate('anlSetLvl(2)');
  const expanded=await evaluate('D("npAnlGrid").querySelector("#npAnlDyn .npanal-verdict").textContent');
  await evaluate('openHealthModal(ANL.cur)');await until('D("mbody").querySelector("#npAnlDyn .npanal-verdict")');
  const modal=await evaluate('D("mbody").querySelector("#npAnlDyn .npanal-verdict").textContent');
  const regression={rateLabel,dr:10.87,summary,expanded,modal};geometry.push({regression});console.log(JSON.stringify({regression}));
  checked('Null actual 88.2k output is not constrained by the default DAC',
    rateLabel.value==='88200'&&!rateLabel.disabled&&!rateLabel.label.includes('设备不支持')&&rateLabel.label.includes('离线处理'));
  checked('DR10.87 agrees across summary expanded and context analysis',
    summary==='中等动态 · 流行/摇滚常见'&&expanded===summary&&modal===summary);
  await evaluate('D("mclose").click();closeNowPlaying();anlSetLvl(0);rlStudioTab("ab")');health.dr_db=12.4;
  await evaluate('anlRun(ANL.cur,true)');await until('ANL.cache[ANL.cur].dr_db===12.4&&!ANL.busy[ANL.cur]');
  checked('Null capability does not probe an unrelated WASAPI endpoint',!requests.some(request=>request.path==='/api/rates'));
  settings.backend='wasapi';settings.device='fixture-dac-A';facts.rates=[44100,48000];
  await evaluate('loadSettings().then(function(){return rlConsole.refresh()})');await until('D("sRate").dataset.rlRateState==="known"');
  await check('known hardware restrictions retain the real saved selection',
    '[D("sRate"),D("rlStudioPcmRate")].every(e=>e.value==="88200"&&e.selectedOptions[0].disabled&&e.selectedOptions[0].textContent.includes("设备不支持"))');
  checked('hardware rates are requested for the selected physical endpoint',requests.some(request=>request.path==='/api/rates'&&request.query==='?device=fixture-dac-A'));
  await click('.side .nav[data-v=settings]');await until('D("sp_rate")');await evaluate('loadRates()');
  await check('advanced settings share the same hardware restriction',
    'D("sp_rate").value==="88200"&&D("sp_rate").selectedOptions[0].disabled');
  await click('.side .nav[data-v=listening]');
  facts.rates=[];await evaluate('loadRates()');
  await check('an empty capability report is unknown rather than unsupported',
    '[D("sRate"),D("sp_rate"),D("rlStudioPcmRate")].every(e=>e.value==="88200"&&!e.selectedOptions[0].disabled&&e.selectedOptions[0].textContent.includes("能力未确认"))');
  facts.rates=[44100,88200,192000];await evaluate('loadRates()');await select('rlStudioPcmRate','192000');await evaluate('clearTimeout(RL_STUDIO.timer)');
  facts.rates=[44100,88200];await evaluate('loadRates()');
  await check('a capability refresh preserves an unsupported edited draft and blocks apply',
    'RL_STUDIO.draft.rate==="192000"&&D("rlStudioPcmRate").value==="192000"&&D("rlStudioPcmRate").selectedOptions[0].disabled&&D("sRate").value==="88200"');
  facts.rates=[];await evaluate('loadRates()');
  await check('an unconfirmed capability preserves the draft without claiming it is unsupported',
    'RL_STUDIO.draft.rate==="192000"&&D("rlStudioPcmRate").value==="192000"&&!D("rlStudioPcmRate").selectedOptions[0].disabled');
  facts.rates=[44100];facts.rateDelay=150;
  await evaluate('window.rateRegressionPending=loadRates();true');await delay(30);
  settings.backend='null';settings.device='null';await evaluate('loadSettings().then(function(){return rlConsole.refresh()})');
  await evaluate('window.rateRegressionPending');facts.rateDelay=0;
  await check('a late previous-device reply cannot relabel Null or destroy the draft',
    'D("sRate").dataset.rlRateState==="offline"&&D("rlStudioPcmRate").value==="192000"&&!D("rlStudioPcmRate").selectedOptions[0].disabled&&D("rlStudioPcmRate").selectedOptions[0].textContent.includes("离线处理")');
  await evaluate('RL_STUDIO.draft={};rlStudioSync()');
  settings.backend='wasapi-shared';settings.device='fixture-dac-A';facts.rates=[44100];
  await evaluate('loadSettings().then(function(){return rlConsole.refresh()})');await evaluate('loadRates()');
  await check('exclusive-device limits do not falsely constrain shared-mode processing',
    'D("rlStudioPcmRate").value==="88200"&&!D("rlStudioPcmRate").selectedOptions[0].disabled&&D("sRate").dataset.rlRateState==="unknown"');
  settings.backend='asio';settings.device='fixture-asio';facts.caps={ok:true,device:{backend:'wasapi',rates:[44100],maxRate:44100}};
  await evaluate('loadSettings().then(function(){return rlConsole.refresh()})');await evaluate('loadRates()');
  await check('an ASIO query cannot borrow a WASAPI capability result',
    'D("sRate").dataset.rlRateState==="unknown"&&!D("rlStudioPcmRate").selectedOptions[0].disabled');
  facts.caps={ok:true,device:{backend:'asio',name:'Fixture ASIO',rates:[44100,88200],maxRate:88200}};
  await evaluate('loadRates()');
  await check('confirmed ASIO limits apply to the actual driver',
    'D("sRate").dataset.rlRateState==="known"&&!D("rlStudioPcmRate").selectedOptions[0].disabled&&Array.from(D("rlStudioPcmRate").options).find(o=>o.value==="192000").disabled');
  settings.backend='upnp';settings.device='';const requestsBeforeRemote=requests.filter(request=>request.path==='/api/rates').length;
  await evaluate('loadSettings().then(function(){return rlConsole.refresh()})');await evaluate('loadRates()');
  checked('remote output does not inherit or probe local hardware restrictions',requests.filter(request=>request.path==='/api/rates').length===requestsBeforeRemote&&await evaluate('D("sRate").dataset.rlRateState==="unknown"&&!D("rlStudioPcmRate").selectedOptions[0].disabled'));
  settings.backend='null';settings.device='null';facts.caps=null;facts.rates=[44100,48000];
  await evaluate('loadSettings().then(function(){return rlConsole.refresh()})');await until('D("sRate").dataset.rlRateState==="offline"');
  checked('capability reads and draft recovery make no parameter writes',mutations.length===0&&forbidden.length===0);
  facts.allowWorkbench=true;
  await select('rlStudioFilter1','poly-sinc-gauss-medium');await evaluate('rlStudioFlush()');await until('!rlConsole.busy&&!RL_STUDIO.pending&&rlConsole.state.settings.filter==="poly-sinc-gauss-medium"');
  checked('filter editor reaches the server-owned active configuration',mutations.at(-1).endpoint==='/api/workbench/profile'&&mutations.at(-1).action==='update'&&mutations.at(-1).id==='A'&&mutations.at(-1).filter==='poly-sinc-gauss-medium');
  checked('same-page filter change preserves exact gain',settings.volume==='-64.5'&&mutations.at(-1).volume===undefined);
  await check('shared model resynchronizes legacy playback controls','D("sFilt").value==="poly-sinc-gauss-medium"&&ADV.device==="null"');
  await check('A automatically snapshots the applied real state','rlCompare.slots[0].settings.filter==="poly-sinc-gauss-medium"&&!RL_STUDIO.pending&&!D("rlStudioSlotSave")');
  await click('[data-studio-slot=B]');await until('!RL_STUDIO.pending&&rlCompare.activeId==="B"');
  await select('rlStudioFilter1','poly-sinc-long-lp');await evaluate('rlStudioFlush()');await until('!rlConsole.busy&&!RL_STUDIO.pending&&rlCompare.slots[1].settings.filter==="poly-sinc-long-lp"');
  await evaluate('document.querySelector("[data-slot-name=B]").dispatchEvent(new MouseEvent("dblclick",{bubbles:true}));var renameField=document.querySelector("[data-slot-name=B] input");renameField.value="Sharp B";renameField.dispatchEvent(new KeyboardEvent("keydown",{key:"Enter",bubbles:true}))');
  await check('B stores its independent filter and editable name on the server','rlCompare.slots[1].settings.filter==="poly-sinc-long-lp"&&rlCompare.slots[1].name==="Sharp B"&&!RL_STUDIO.pending');
  checked('live comparison no longer writes replacement local-storage snapshots',await evaluate('localStorage.getItem(rlCompare.storageKey)')===null);
  await click('[data-studio-slot=A]');await until('!rlConsole.busy&&!RL_STUDIO.pending&&rlCompare.activeId==="A"');
  await check('left A/B switch restores A in all live controls','rlConsole.state.settings.filter==="poly-sinc-gauss-medium"&&D("rlStudioFilter1").value==="poly-sinc-gauss-medium"&&D("sFilt").value==="poly-sinc-gauss-medium"');
  await evaluate('D("rlStudioRangeStart").value="12.75";D("rlStudioRangeEnd").value="42.75";D("rlStudioRangeEnd").dispatchEvent(new Event("change",{bubbles:true}))');
  await until('rlCompare.loop.enabled&&rlCompare.loop.start===12.75&&rlCompare.loop.end===42.75&&rlWorkbenchQueue.pending===0');
  checked('fixed segment sends real passage boundaries and revision to the backend',mutations.at(-1).endpoint==='/api/workbench/loop'&&Number(mutations.at(-1).start)===12.75&&Number(mutations.at(-1).end)===42.75&&mutations.at(-1).expected_revision===String(facts.revision));
  await click('[data-studio-slot=B]');await until('!rlConsole.busy&&!RL_STUDIO.pending&&rlCompare.activeId==="B"');
  await check('comparison retains the same selected passage','rlCompare.loop.start===12.75&&rlCompare.loop.end===42.75&&rlCompare.loop.enabled');
  await check('snapshot excludes device routing and session volume','!Object.hasOwn(rlCompare.slots[1].settings,"volume")&&!Object.hasOwn(rlCompare.slots[1].settings,"backend")&&!Object.hasOwn(rlCompare.slots[1].settings,"device")');
  await click('#rlStudioTab_eq');await until('D("rlEqRows").querySelectorAll(".rlEqr").length===20');
  await evaluate('D("rlEqPreamp").value="-3";D("rlEqPreamp").dispatchEvent(new Event("input",{bubbles:true}))');
  await until('!rlConsole.busy&&!RL_STUDIO.pending&&rlConsole.state.peq.preamp===-3');
  checked('full PEQ sends all twenty bands through /api/peq',mutations.at(-1).endpoint==='/api/peq'&&Number(mutations.at(-1).preamp)===-3&&mutations.at(-1).bands.split(';').length===20);
  const beforeEqArrow={volume:volumeWrites.length,forbidden:forbidden.length};
  await evaluate('document.querySelector("#rlEqRows .rlEqr").focus()');
  for(const type of ['keyDown','keyUp'])await browser.send('Input.dispatchKeyEvent',{type,key:'ArrowUp',code:'ArrowUp',windowsVirtualKeyCode:38,nativeVirtualKeyCode:38});
  await until('!rlConsole.busy&&!RL_STUDIO.pending&&Math.abs(rlConsole.state.peq.bands[0].gain-.1)<.001');
  checked('a focused EQ row arrow changes its band without global volume or playback shortcuts',volumeWrites.length===beforeEqArrow.volume&&forbidden.length===beforeEqArrow.forbidden);
  await click('#rlStudioTab_ab');
  await click('.side .nav[data-v=settings]');await check('professional settings still mount in the real route','S.view==="settings"&&D("setpage").checkVisibility()&&!!D("sp_backend")');
  await check('automatic-analysis help describes cached results without a removed button',
    'D("sp_autoanl").closest("label").querySelector(".tip").getAttribute("data-tip")==="开启后会自动分析当前曲目并缓存结果。关闭后仍可查看已有结果。"');
  settings.autoanl='0';await evaluate('loadSettings();setView("listening");rlStudioTab("monitor")');await until('ANL.auto===0&&ANL.cache[ANL.cur]');
  await evaluate('anlSetLvl(2)');const healthBeforeAutoOff=requests.filter(request=>request.path==='/api/health').length;await delay(450);
  checked('turning automatic analysis off retains existing metrics without a new request',
    requests.filter(request=>request.path==='/api/health').length===healthBeforeAutoOff&&await evaluate('!D("npAnlGrid").hidden&&D("npAnlGrid").textContent.includes("12.4")&&D("npAnlGrid").textContent.includes("82")'));
  settings.autoanl='1';await evaluate('loadSettings();anlSetLvl(0);closeNowPlaying();rlStudioTab("ab")');await until('ANL.auto===1&&PLAYBACK_SETTINGS.ready');
  await click(listen);await check('mode switch from settings returns to listening home','S.view==="listening"&&D("rlListeningControls").checkVisibility()&&!D("setpage").checkVisibility()');
  await check('listening route hides professional navigation','!document.querySelector(".side .nav[data-v=settings]").checkVisibility()&&!D("rlStudio").checkVisibility()');
  await check('listening route hides the empty system section title','D("rlSystemSection")&&!D("rlSystemSection").checkVisibility()');
  await click(pro);await check('returning to professional home retains actual edits','D("rlStudio").checkVisibility()&&D("rlStudioFilter1").value==="poly-sinc-long-lp"');
  await evaluate('D("list").scrollTop=0;D("rlListenHome").scrollIntoView({block:"start"})');
  await check('desktop PCM DSD and output advanced entrances fit the first screen','["pcm","sdm","output"].every(group=>{const r=document.querySelector("[data-studio-advanced="+group+"]").getBoundingClientRect();return r.top>=0&&r.bottom<innerHeight-85})');
  const variants=[[1340,'zh'],[1100,'en'],[1024,'fr'],[900,'es'],[800,'ja'],[720,'ru'],[640,'ar']];
  for(const [width,language] of variants){
   scenario=width+'-'+language;facts.language=language;await browser.send('Emulation.setDeviceMetricsOverride',{width,height:880,deviceScaleFactor:1,mobile:false});await evaluate('i18nSetLang('+JSON.stringify(language)+',false)');await delay(100);
   await check('professional module fits whole app '+scenario,'D("rlStudio").scrollWidth<=D("rlStudio").clientWidth+1');
   await evaluate('rlStudioTab("monitor");anlSetLvl(0)');await until('ANL.cache[ANL.cur]&&D("npAnlMore")&&!D("npAnlMore").disabled');
   await check('sound monitoring offers the established compact summary '+scenario,'D("npAnlGrid").hidden&&document.querySelectorAll("#npAnlCtl button").length===4');
   checked('analysis and its controls belong only to the professional monitor '+scenario,await evaluate('!!document.querySelector("#rlStudio #npAnlNow")&&!document.querySelector("#npPage .npanal")'));
   await evaluate('D("npAnlCtl").scrollIntoView({block:"center"})');
   const rects=await evaluate('["npAnlDynSummary","npAnlQualSummary","npAnlFakeSummary","npAnlMore"].map(id=>{const e=D(id),r=e.getBoundingClientRect();return {id,x:r.x,y:r.y,width:r.width,height:r.height,visible:e.checkVisibility(),label:e.textContent}})');
   geometry.push({width,language,analysis:rects});
   checked('dynamic quality lossless expand share one y '+scenario,rects.every(r=>r.visible&&r.width>0&&Math.abs(r.y-rects[0].y)<1));
   checked('analysis summary stays inside the viewport '+scenario,rects.every(r=>r.x>=0&&r.x+r.width<=width+1));
   await click('#npAnlMore');await check('expanded monitor retains all three metric cards '+scenario,'!D("npAnlGrid").hidden&&D("npAnlGrid").querySelectorAll(".npanal-card").length===3&&D("npAnlGrid").textContent.includes("12.4")&&D("npAnlGrid").textContent.includes("82")');
   await shot('monitor-expanded-'+scenario);await click('#npAnlMore');await check('monitor can return to its summary '+scenario,'D("npAnlGrid").hidden&&ANL.lvl===0');
   await shot('monitor-summary-'+scenario);await dynamicAgreement(language);
   await evaluate('openNowPlaying()');await check('track detail has no analysis even in professional mode '+scenario,'D("npPage").classList.contains("on")&&!D("npPage").querySelector(".npanal")');
   facts.rawVolumeLocked=true;facts.volumeWritable=false;facts.actualVolume=0;settings.volume='0';
   const variantRawBefore=await evaluate('statusApplied');await evaluate('tick()');await until('statusApplied>'+variantRawBefore);
   await check('raw volume has truthful controls and localized hardware guidance '+scenario,
     'D("vol").disabled&&D("npVol").disabled&&D("volFloor").disabled&&D("barVolInput").disabled&&D("npVolInput").disabled&&D("npVolFloor").disabled&&D("npVol").value==="0"&&D("barVolDb").textContent==="0.0 dB"&&D("npRawVolumeNotice").textContent===t("设备调音")&&D("npRawVolumeNotice").title===rawVolumeMessage()');
   checked('raw now-playing volume stays inside the viewport '+scenario,await evaluate('(()=>{const r=D("npVol").parentElement.getBoundingClientRect();return r.x>=0&&r.right<=innerWidth+1})()'));
   await shot('raw-volume-nowplaying-'+scenario);
   facts.rawVolumeLocked=false;facts.volumeWritable=true;facts.actualVolume=-64.5;settings.volume='-64.5';
   const variantWritableBefore=await evaluate('statusApplied');await evaluate('tick()');await until('statusApplied>'+variantWritableBefore);
   await check('processable playback regains its actual gain after the raw session '+scenario,
     '!D("vol").disabled&&!D("npVol").disabled&&D("vol").value==="-64.5"&&S.vol===-64.5&&D("npRawVolumeNotice").hidden');
   await evaluate('closeNowPlaying();rlStudioTab("ab")');await shot('digital-console-'+scenario);
  }
  scenario='theme error contrast';facts.language='zh';facts.failWorkbench=true;
  await browser.send('Emulation.setDeviceMetricsOverride',{width:1340,height:880,deviceScaleFactor:1,mobile:false});
  await evaluate('i18nSetLang("zh",false)');
  await select('rlStudioFilter1','poly-sinc-gauss-medium');await evaluate('rlStudioFlush().catch(function(){})');
  await until('!RL_STUDIO.pending&&!rlConsole.busy&&D("rlStudioMessage").dataset.error==="true"');
  for(const theme of ['dark','light','blue']){
   await evaluate('setTheme('+JSON.stringify(theme)+',false)');await delay(250);
   const error=await evaluate('('+renderedTextContrast.toString()+')("#rlStudioMessage")');
   const draft=await evaluate('('+renderedTextContrast.toString()+')(".digitalSlot[aria-current=true] .digitalSlotState")');
   geometry.push({theme,mode:'pro',error,draft});
   checked('professional error text has 4.5:1 contrast '+theme,error.ratio>=4.5&&error.opacity>.99);
   checked('professional draft text remains readable '+theme,draft.ratio>=4.5);
   await shot('professional-error-'+theme);
  }
  await evaluate('RL_STUDIO.draft={};rlStudioSync()');await click(listen);
  for(const theme of ['dark','light','blue']){
   await evaluate('setTheme('+JSON.stringify(theme)+',false)');
   await click('[data-rl-listen-task=style]');await click('[data-rl-listening-style=warm]');await click('#rlListeningDialogApply');
   await until('!rlListening.working&&!rlConsole.busy&&D("rlListeningDialogMessage").dataset.error==="true"');
   await delay(250);
   const dialogError=await evaluate('('+renderedTextContrast.toString()+')("#rlListeningDialogMessage")');
   checked('listening dialog error has 4.5:1 contrast '+theme,dialogError.ratio>=4.5&&dialogError.opacity>.99);
   await shot('listening-dialog-error-'+theme);await click('#rlListeningDialogCancel');
   const homeError=await evaluate('('+renderedTextContrast.toString()+')("#rlListeningNotice")');
   checked('listening home error has 4.5:1 contrast '+theme,homeError.ratio>=4.5&&homeError.opacity>.99);
   geometry.push({theme,mode:'listen',dialogError,homeError});
   await shot('listening-home-error-'+theme);
  }
  facts.failWorkbench=false;
  scenario='restricted native QA bridge';
  await evaluate('qaExec("workbench:mode:pro")');
  await check('QA mode action uses the real professional route','rlMode==="pro"&&D("rlStudio").checkVisibility()');
  const qaScrollWrites=mutations.length;
  await evaluate('qaExec("workbench:rack")');
  await check('QA rack scrolls the real DSD and PEQ controls into view',
    'D("rlStudioModFamily").getBoundingClientRect().top>=0&&D("rlStudioModFamily").getBoundingClientRect().bottom<innerHeight-85');
  const qaRackTop=await evaluate('D("rlStudio").getBoundingClientRect().top');
  await evaluate('qaExec("workbench:top")');
  await check('QA top returns to the real console header','D("rlStudio").getBoundingClientRect().top>=0&&D("rlStudio").getBoundingClientRect().top>'+qaRackTop);
  checked('QA rack and top actions make no configuration writes',mutations.length===qaScrollWrites);
  await evaluate('qaExec("workbench:state")');
  const qaState=await evaluate('JSON.parse(qaPanelsText().replace(/^\\[WORKBENCH\\] /,""))');
  geometry.push({nativeQaState:qaState});
  checked('QA state reports the real renderer and shared configuration',
    qaState.mode==='pro'&&qaState.view==='listening'&&qaState.viewport[0]===1340
    &&qaState.state.file===facts.current&&qaState.state.settings.filter===settings.filter
    &&qaState.queue.items[0]===facts.current&&qaState.comparison.slots.length===4
    &&qaState.state.peq.preamp===peq.preamp&&qaState.state.queueIndex===0
    &&qaState.state.paused===false&&qaState.paused===false
    &&qaState.metadata.title==='Fixture concerto'&&typeof qaState.cover==='string'
    &&qaState.settingsHidden===false&&qaState.legacyAnalysisControls===false
    &&qaState.geometry.length===6&&qaState.geometry.every(item=>Number.isFinite(item.x)&&Number.isFinite(item.y))
    &&qaState.geometry[0].id==='[data-studio-advanced=pcm]'&&qaState.geometry[0].w>0);
  const qaBeforeUnknown={writes:mutations.length,revision:facts.revision,volume:settings.volume,
    storage:await evaluate('localStorage.getItem(rlCompare.storageKey)'),
    identity:await evaluate('JSON.stringify([S.view,rlMode,D("rlListeningDialog").open])')};
  await evaluate('["workbench:eval:alert(1)","workbench:slot:E","workbench:style:unknown","workbench:mode:listen;volume=0","workbench:apply?volume=0"].forEach(qaExec)');
  await delay(60);
  checked('unknown QA workspace actions cannot write settings or change modes',
    mutations.length===qaBeforeUnknown.writes&&facts.revision===qaBeforeUnknown.revision
    &&settings.volume===qaBeforeUnknown.volume
    &&await evaluate('localStorage.getItem(rlCompare.storageKey)')===qaBeforeUnknown.storage
    &&await evaluate('JSON.stringify([S.view,rlMode,D("rlListeningDialog").open])')===qaBeforeUnknown.identity);
  await evaluate('qaExec("workbench:slot:C");qaExec("workbench:save")');
  await check('QA flush keeps the selected C actual parameters automatic','!RL_STUDIO.pending&&rlCompare.activeId==="C"&&rlCompare.slots[2].settings.filter===rlConsole.state.settings.filter&&!D("rlStudioSlotSave")');
  await evaluate('qaExec("workbench:mode:listen")');
  await check('QA listening action keeps settings navigation hidden','rlMode==="listen"&&!document.querySelector(".side .nav[data-v=settings]").checkVisibility()');
  // HTMLDialogElement.close queues a close event. Reopening synchronously must
  // not let that old event clear the newly opened card's task or draft.
  await evaluate('qaExec("workbench:card:output");qaExec("workbench:cancel");qaExec("workbench:card:style");qaExec("workbench:style:warm")');
  await delay(100);
  await check('a queued old dialog close preserves the newly opened card',
    'D("rlListeningDialog").open&&rlListening.task==="style"&&rlListening.draft&&rlListening.draft.style==="warm"&&document.querySelector("[data-rl-listening-style=warm]").getAttribute("aria-pressed")==="true"');
  await evaluate('qaExec("workbench:cancel")');await until('!D("rlListeningDialog").open&&rlListening.task===""');
  for(const card of ['output','profile']){
   const beforeCard=mutations.length;
   await evaluate('qaExec('+JSON.stringify('workbench:card:'+card)+')');
   await check('QA opens the real '+card+' card','D("rlListeningDialog").open&&rlListening.task==='+JSON.stringify(card));
   await evaluate('qaExec("workbench:cancel")');await until('!D("rlListeningDialog").open&&rlListening.task===""');
   checked('QA '+card+' preview and cancel are read only',mutations.length===beforeCard);
  }
  await evaluate('qaExec("workbench:card:style")');
  for(const style of ['warm','balanced','sharp']){
   await evaluate('qaExec('+JSON.stringify('workbench:style:'+style)+')');
   await check('QA style action selects the actual '+style+' option',
     'rlListening.draft.style==='+JSON.stringify(style)+'&&document.querySelector("[data-rl-listening-style='+style+']").getAttribute("aria-pressed")==="true"');
  }
  await evaluate('qaExec("workbench:style:warm");qaExec("workbench:apply")');
  await check('QA apply submits a real style change and closes the card',
    '!D("rlListeningDialog").open&&!rlConsole.busy&&rlConsole.state.style==="warm"');
  checked('QA style apply keeps source revision checks and exact gain',
    mutations.at(-1).style==='warm'&&mutations.at(-1).expected_file===facts.current
    &&mutations.at(-1).expected_revision&&settings.volume==='-64.5');
  await evaluate('qaExec("workbench:mode:pro");qaExec("workbench:slot:D");qaExec("workbench:save")');
  await check('QA selects server-owned D without copying another group','!RL_STUDIO.pending&&rlCompare.activeId==="D"&&rlCompare.slots[3].settings.filter===rlConsole.state.settings.filter&&rlCompare.slots[2].settings.filter==="poly-sinc-gauss-medium"');
  await evaluate('qaExec("workbench:pin")');
  await check('QA legacy pin maps to the real current passage boundary','!RL_STUDIO.pending&&rlWorkbenchQueue.pending===0&&rlCompare.loop.enabled&&rlCompare.loop.start===12.75');
  await evaluate('qaExec("workbench:prev")');
  await check('QA previous moves to saved C','!RL_STUDIO.pending&&!rlConsole.busy&&rlCompare.activeId==="C"');
  await evaluate('qaExec("workbench:next")');
  await check('QA next returns to saved D','!RL_STUDIO.pending&&!rlConsole.busy&&rlCompare.activeId==="D"');
  for(const slot of ['A','B']){
   await evaluate('qaExec('+JSON.stringify('workbench:slot:'+slot)+')');
   await check('QA selects and loads saved '+slot,'!RL_STUDIO.pending&&!rlConsole.busy&&rlCompare.activeId==='+JSON.stringify(slot));
  }
  checked('all workflows preserve actual session gain',settings.volume==='-64.5');
  checked('no playback, direct-settings or external action escaped fixture',forbidden.length===0);
  checked('whole production script has no uncaught JavaScript error',errors.length===0);
  console.log(JSON.stringify({checks:checks.length,failures:0,evidence:run}));
 }catch(error){try{geometry.push({failureState:await evaluate('({studio:{pending:RL_STUDIO.pending,switching:RL_STUDIO.switching,draft:RL_STUDIO.draft,inflight:RL_STUDIO.inflight,rename:RL_STUDIO.rename},compare:{activeId:rlCompare.activeId,error:rlCompare.error,saving:rlCompare.saving,dirty:rlCompare.dirty()},eq:{editing:RL_EQ.editing,slot:RL_EQ.slot},queued:rlWorkbenchQueue.pending,console:{busy:rlConsole.busy,error:rlConsole.error},message:D("rlStudioMessage").textContent})')});await shot('failure')}catch{}console.error({scenario,errors,forbidden,diagnostic:geometry.at(-1),evidence:run});throw error;}
 finally{fs.writeFileSync(path.join(run,'result.json'),JSON.stringify({checks,geometry,mutations,volumeWrites,preferenceWrites,requests,forbidden,errors,lastScenario:scenario},null,2));await browser.close();}
}
main().catch(error=>{console.error(error);process.exitCode=1;});
