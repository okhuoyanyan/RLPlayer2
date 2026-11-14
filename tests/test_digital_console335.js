'use strict';
// Compiled professional module in Edge. All APIs and playback are inert fixtures.
const assert=require('node:assert/strict'),fs=require('node:fs'),path=require('node:path');
const {spawn,spawnSync}=require('node:child_process'),{once}=require('node:events');
const delay=ms=>new Promise(resolve=>setTimeout(resolve,ms));
function option(key,fallback){const i=process.argv.indexOf(key);return i<0?fallback:process.argv[i+1];}
const source=path.resolve(option('--source',path.join(__dirname,'..')));
const work=path.resolve(option('--work-root',path.join(source,'build-console335','evidence')));
fs.mkdirSync(work,{recursive:true});const run=fs.mkdtempSync(path.join(work,'console-'));
const errors=[],checks=[],origin='https://digital-console.invalid';
function program(exe,args){const r=spawnSync(exe,args,{encoding:'utf8',windowsHide:true});if(r.error||r.status!==0)throw Error([r.error,r.stdout,r.stderr].filter(Boolean).join('\n'));}
function exportModule(){
 const cpp=path.join(run,'export-ui.cpp'),exe=path.join(run,'export-ui.exe'),css=path.join(run,'studio.css'),js=path.join(run,'studio.js');
 fs.writeFileSync(cpp,'#include <fstream>\n#include "net/ui_studio.h"\n#include "net/ui_eq_console.h"\n#include "net/ui_workbench_state.h"\n#include "net/ui_waveform.h"\nint main(int argc,char**argv){if(argc!=3)return 2;std::ofstream a(argv[1],std::ios::binary),b(argv[2],std::ios::binary);a<<rta::net::rta_ui_studio_css()<<rta::net::rta_ui_eq_console_css()<<rta::net::rta_ui_waveform_css();b<<rta::net::rta_ui_workbench_state_js()<<rta::net::rta_ui_waveform_js()<<rta::net::rta_ui_eq_console_js()<<rta::net::rta_ui_studio_js();return a&&b?0:1;}\n');
 program(option('--cxx','C:/Program Files/AMD/ROCm/7.2/bin/clang++.exe'),['-std=c++23','-O0','-I'+path.join(source,'src'),cpp,'-o',exe]);program(exe,[css,js]);
 return {css:fs.readFileSync(css,'utf8'),js:fs.readFileSync(js,'utf8')};
}
function startBrowser(){
 const exe=option('--edge',['C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe','C:/Program Files/Microsoft/Edge/Application/msedge.exe'].find(fs.existsSync));
 if(!exe)throw Error('Edge missing');
 const proc=spawn(exe,['--headless=new','--disable-gpu','--disable-background-networking','--no-first-run','--mute-audio','--remote-debugging-pipe','--user-data-dir='+path.join(run,'profile'),'about:blank'],{windowsHide:true,stdio:['ignore','ignore','ignore','pipe','pipe']});
 const closed=once(proc,'exit'),pending=new Map();let id=0,session,buffer=Buffer.alloc(0);
 function send(method,params={},target=session){return new Promise((resolve,reject)=>{const key=++id,timeout=setTimeout(()=>{pending.delete(key);reject(Error('Timed out '+method));},10000);pending.set(key,{resolve,reject,timeout});proc.stdio[3].write(JSON.stringify(target?{id:key,method,params,sessionId:target}:{id:key,method,params})+'\0');});}
 const api={send,onRequest:null,async attach(){const t=await send('Target.createTarget',{url:'about:blank'});const r=await send('Target.attachToTarget',{targetId:t.targetId,flatten:true});session=r.sessionId;},async close(){if(proc.exitCode===null){await send('Browser.close',{},null);proc.stdio[3].end();}await closed;}};
 proc.stdio[4].on('data',chunk=>{buffer=Buffer.concat([buffer,chunk]);let split;while((split=buffer.indexOf(0))>=0){const m=JSON.parse(buffer.subarray(0,split).toString('utf8'));buffer=buffer.subarray(split+1);if(m.id&&pending.has(m.id)){const p=pending.get(m.id);pending.delete(m.id);clearTimeout(p.timeout);m.error?p.reject(Error(m.error.message)):p.resolve(m.result);}else if(m.method==='Runtime.exceptionThrown')errors.push(m.params.exceptionDetails.exception?.description||m.params.exceptionDetails.text);else if(m.method==='Fetch.requestPaused'&&api.onRequest)api.onRequest(m.params).catch(e=>errors.push(String(e)));}});
 return api;
}
const globals=String.raw`
var D=id=>document.getElementById(id),rlMode='pro',PLAYBACK_SETTINGS={ready:true};
var esc=v=>String(v===undefined?'':v).replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
var t=v=>v,tf=(v,items)=>v.replace(/\{(\d+)\}/g,(_,i)=>items[Number(i)]),fmt=v=>Math.floor(v/60)+':'+String(Math.floor(v%60)).padStart(2,'0');
var applyI18n=()=>{},parentFolder=v=>v,nam=v=>v.split('/').pop(),toast=()=>{};
var rlSetText=(id,value)=>{const e=D(id);if(e&&e.textContent!==value)e.textContent=value;};
var S={playing:true,paused:false,dur:120,q:{index:1,items:['Q:/first.flac','Q:/image.wav','Q:/image.wav'],cues:[[-1,-1],[45,165],[165,285]]}};
var network=[],plays=[],actions=[],failNext=false,confirms=0,signalDelay=false,signals=[];
var rlNav=v=>actions.push({nav:v}),openSettingsTab=v=>actions.push({settings:v}),playOrPause=()=>actions.push({playButton:true});
var playPath=(file,cs,ce,index)=>plays.push({file,cs,ce,index});
var signal={src:'Q:/image.wav',codec:'pcm_s16le',inBits:16,outRate:88200,outBits:24,outMode:'pcm',resample:true,filter:'poly-sinc-short-lp',device:'Fixture Null'};
var copy=v=>JSON.parse(JSON.stringify(v)),_capsCache=null,ensurePlaybackSettings=()=>PLAYBACK_SETTINGS.ready,loadSettings=()=>Promise.resolve({ok:true});
var initial={rate:'88200',rateFamilyPolicy:'prefer',filter:'poly-sinc-short-lp',filterNx:'',dsd:'0',dsdnative:'0',twostage:'0',shaper:'auto',modfamily:'dsd',modorder:'5',sdmfilt:'',sdmfiltnx:'',directsdm:'0',taps:'0',phase:'',modcut:'0',moddith:'-1',ahmbits:'1',volume:'-64.5',backend:'null',device:'Fixture Null'};
var initialPeq={bypass:0,preamp:0,bands:Array.from({length:20},(_,i)=>({i:i+1,on:true,type:'PK',freq:20*Math.pow(1000,i/19),gain:0,q:1.41})),points:[[20,0],[1000,0],[20000,0]],fs:88200};
var backend={active:'A',slots:['A','B','C','D'].map(id=>({id,name:id,settings:copy(initial),peq:copy(initialPeq),eqId:'',eqName:'',migrated:true})),loop:{enabled:false,repeat:0,start:0,end:120,file:''},bank:Array.from({length:16},(_,i)=>({id:String(i+1),name:'',source:'custom',peq:null,empty:true})),state:{ok:true,settings:copy(initial),peq:copy(initialPeq),playing:true,paused:false,file:'Q:/image.wav',position:20,duration:120,cueStart:45,cueEnd:165,queueIndex:1,revision:'1'}};
var confirm=()=>{confirms++;return true;};document.addEventListener('keydown',function(e){if(/^Arrow/.test(e.key))actions.push({globalArrow:e.key});});
window.fetch=async()=>new Response(JSON.stringify({ok:true,state:'ready',duration:120,bins:[.1,.4,.8,.2,.5,.9,.3,.7]}),{status:200,headers:{'content-type':'application/json'}});
function profile(){return backend.slots.find(s=>s.id===backend.active);}
function snapshot(){var settings=copy(backend.state.settings);for(var key of ['volume','backend','device'])delete settings[key];Object.assign(profile(),{settings,peq:copy(backend.state.peq)});}
function refreshPoints(){var gain=backend.state.peq.bands[1]?.gain||0;var pre=Number(backend.state.peq.preamp)||0;backend.state.peq.points=[[20,pre],[1000,gain+pre],[20000,pre]];}
var apiGetRaw=async function(url){network.push(url);if(url==='/api/workbench/profiles')return copy({ok:true,active:backend.active,slots:backend.slots,loop:backend.loop});if(url==='/api/workbench/eqpresets')return copy({ok:true,slots:backend.bank,activeId:profile().eqId});if(url==='/api/workbench/state')return copy(backend.state);if(url==='/api/opra/index')return {ok:true,items:[['h600','Sennheiser','HD 600'],['h650','Sennheiser','HD 650']]};if(url.startsWith('/api/opra/presets'))return {ok:true,items:[{id:'harman',author:'Fixture Lab',details:'Measured preset',gainDb:-4,bandsCount:20,bands:Array.from({length:20},function(_,i){return {type:'peak_dip',frequency:i===1?1000:100*(i+1),gain:i===1?3:0,q:1.41};})}]};if(url==='/api/signal'){if(signalDelay)return new Promise(resolve=>signals.push(resolve));return {ok:true,path:copy(signal)};}throw Error('Unexpected fixture read '+url);};
var api=url=>apiGetRaw(url);
var apiAct=async function(url,body){var p=Object.fromEntries(new URLSearchParams(body));actions.push({url,...p});if(window.holdNext){window.holdNext=false;await new Promise(resolve=>window.releaseAct=resolve);}if(failNext){failNext=false;return {ok:false,error:'Fixture rejected parameter'};}
 if(url==='/api/peq')return {ok:true};
 if(url==='/api/workbench/profile'){if(p.action==='update'){if(p.id!==backend.active)return {ok:false,error:'wrong active'};for(var [key,value] of Object.entries(p))if(!['action','id','peq','eqId'].includes(key))backend.state.settings[key]=value;if(p.peq){backend.state.peq=JSON.parse(p.peq);profile().eqId='';profile().eqName='';refreshPoints();}snapshot();}
 if(p.action==='select'){snapshot();backend.active=p.id;backend.state.settings={...backend.state.settings,...copy(profile().settings)};backend.state.peq=copy(profile().peq);}
 if(p.action==='rename')backend.slots.find(s=>s.id===p.id).name=p.name;
 }else if(url==='/api/workbench/eqpresets'){if(p.slotId!==backend.active)return {ok:false,error:'wrong group'};var slot=backend.bank.find(s=>s.id===p.id);if(p.action==='delete'){Object.assign(slot,{peq:null,empty:true,name:''});}else{if(p.action==='save')Object.assign(slot,{name:p.name,source:p.source,peq:JSON.parse(p.peq),empty:false});if(p.action==='rename')slot.name=p.name;if(p.action==='import'){var peq=copy(initialPeq);peq.preamp=-4;peq.bands=peq.bands.slice(0,10);peq.bands[1].gain=3;Object.assign(slot,{name:'HD 600 · Fixture Lab',source:'opra',peq,empty:false,productId:p.productId,presetId:p.presetId,link:'https://opra.roon.app/'});}backend.state.peq=copy(slot.peq);backend.state.settings.eq='';refreshPoints();snapshot();profile().eqId=slot.id;profile().eqName=slot.name;}}
 else if(url==='/api/workbench/loop'){backend.loop={...p,cueStart:backend.state.cueStart,cueEnd:backend.state.cueEnd,start:Number(p.start),end:Number(p.end)};}
 backend.state.revision=String(Number(backend.state.revision)+1);return {ok:true};};
`;
async function main(){
 const assets=exportModule(),browser=startBrowser();
 const base='<style>:root{--rl-title:#f4e9d4;--rl-copy:#e1d5c1;--rl-muted:#c2afa0;--rl-line:#635546;--rl-panel:#302b23;--rl-field:#211e1a;--rl-raised:#3b3329;--rl-accent:#e5c38a;--rl-ink:#342618;--rl-tint:#493d2e;--rl-serif:Georgia,serif}body{margin:0;background:#201d19;font:14px Arial,sans-serif}#rlListenHome{padding:18px;max-width:1200px;margin:auto;box-sizing:border-box}button{color:var(--rl-copy);background:var(--rl-raised);border:1px solid var(--rl-line)}#fixtureHidden{display:none}</style>';
 const hidden='<div id=fixtureHidden><button id=rlListenPlay>暂停</button><select id=sFilt><option value=poly-sinc-short-lp>poly-sinc-short-lp</option><option value=gauss-hires-lp>gauss-hires-lp</option><option value=poly-sinc-long-lp>poly-sinc-long-lp</option></select><select id=sFiltNx><option value="">跟随 1x</option><option value=poly-sinc-long-lp>poly-sinc-long-lp</option></select><select id=sRate><option value=src>源采样率</option><option value=88200>88200</option><option value=176400>176400</option><option value=follow>跟随系统</option><option value=max>自动</option></select><select id=sRateFamilyPolicy><option value=prefer>同族优先</option><option value=strict>严格同族</option></select><select id=s2s><option value=auto>自动</option><option value=0>关闭</option><option value=1>开启</option></select><select id=sShap><option value=auto>自动</option><option value=none>无</option></select></div>';
 const skeleton='<section id=rlListenHome><section id=rlProPath><div class=rlQueueHeading><h2>实际信号路径</h2></div><div class=rlPathNodes><div class=rlPathNode><small>源文件</small><strong id=rlPathSource>44.1 kHz</strong></div><div class=rlPathNode><small>声音处理</small><strong id=rlPathProcessing>升频</strong></div><div class=rlPathNode><small>设备输出</small><strong id=rlPathOutput>88.2 kHz / 24 bit</strong></div></div></section></section>';
 const html='<!doctype html><html data-rl-mode=pro><meta charset=utf-8>'+base+'<style>'+assets.css+'</style><body>'+hidden+skeleton+'<script>'+globals+assets.js+';rlInstallStudio();rlPaintStudio({status:"正在播放",title:"Fixture concerto",file:"Q:/image.wav",bg:"none",m:{artist:"Fixture Ensemble",album:"Digital control"},facts:{current:"Q:/image.wav",outRate:88200,outBits:24,outMode:"pcm"}});</script></body></html>';
 fs.writeFileSync(path.join(run,'fixture.html'),html);
 async function evaluate(expression){const r=await browser.send('Runtime.evaluate',{expression,awaitPromise:true,returnByValue:true});if(r.exceptionDetails)throw Error(r.exceptionDetails.exception?.description||r.exceptionDetails.text);return r.result.value;}
 async function check(label,expression){let ok=false;for(let i=0;i<80;i++){if(await evaluate('Boolean('+expression+')')){ok=true;break;}await delay(30);}if(!ok)console.error(JSON.stringify({label,errors,dom:await evaluate('({message:D("rlStudioMessage")?.textContent,eq:D("rlStudioEqCurveState")?.textContent,bypass:D("rlStudioPeqBypass")?.checked})')}));assert.ok(ok,label);checks.push(label);console.log('PASS '+label);}
 try{
  await browser.attach();await browser.send('Runtime.enable');await browser.send('Page.enable');
  browser.onRequest=async request=>browser.send('Fetch.fulfillRequest',{requestId:request.requestId,responseCode:200,responseHeaders:[{name:'content-type',value:'text/html; charset=utf-8'}],body:Buffer.from(html).toString('base64')});
  await browser.send('Fetch.enable',{patterns:[{urlPattern:'*',requestStage:'Request'}]});await browser.send('Page.navigate',{url:origin});
  await check('professional header names the digital console',"D('rlStudioHeading')?.textContent==='数字音乐台'");
  await check('three clear tabs share one workbench',"document.querySelectorAll('.digitalTabs [role=tab]').length===3&&!D('rlStudioPanel_ab').hidden&&D('rlStudioPanel_eq').hidden&&D('rlStudioPanel_monitor').hidden");
  await check('actual signal path remains visible',"D('rlProPath').closest('#rlStudio')&&D('rlStudioSource').textContent.includes('PCM_S16LE')");
  await check('vinyl mechanism and manual snapshot save are absent',"!document.querySelector('.studioMechanism,.studioPlatter,.studioArm,#rlStudioSlotSave')");
  await check('four groups initialize from the real backend state',"rlCompare.activeId==='A'&&rlCompare.slots.every(s=>s.settings)&&D('rlStudioSlots').querySelectorAll('button').length===4");
  await check('PCM format hides DSD bitrate controls in the actual layout',"D('rlStudioDsdRate').getBoundingClientRect().width===0&&D('rlStudioPcmRate').getBoundingClientRect().width>0");
  await check('two-stage zero is retained exactly',"D('rlStudioTwoStage').value==='0'");
  await check('opening performs no mutation or playback',"actions.length===0&&plays.length===0");
  await evaluate("D('rlStudioQueue').querySelector('[data-index=\"2\"]').click()");
  await check('same-file queue rows retain exact CUE and queue identity',"plays[0].file==='Q:/image.wav'&&plays[0].cs===165&&plays[0].ce===285&&plays[0].index===2");
  await check('redundant play button is gone; bottom transport owns playback',"!D('rlStudioPlay')&&!document.querySelector('.digitalCurrent button')");
  for(const [id,filter] of [['A','gauss-hires-lp'],['B','poly-sinc-long-lp'],['C','poly-sinc-short-lp'],['D','gauss-hires-lp']]){
   await evaluate('rlStudioSelect('+JSON.stringify(id)+')');
   await evaluate("D('rlStudioFilter1').value="+JSON.stringify(filter)+";D('rlStudioFilter1').dispatchEvent(new Event('change',{bubbles:true}))");
   await check('group '+id+' automatically saves and applies its filter',"!RL_STUDIO.pending&&Object.keys(RL_STUDIO.draft).length===0&&rlCompare.slots.find(s=>s.id==='"+id+"').settings.filter==="+JSON.stringify(filter));
  }
  await check('four groups preserve their independent values',"rlCompare.slots[0].settings.filter==='gauss-hires-lp'&&rlCompare.slots[1].settings.filter==='poly-sinc-long-lp'&&rlCompare.slots[2].settings.filter==='poly-sinc-short-lp'");
  await evaluate("rlStudioSelect('B')");await check('letter switching updates all inline controls',"rlCompare.activeId==='B'&&D('rlStudioFilter1').value==='poly-sinc-long-lp'");
  await evaluate("window.beforeDelay=actions.length;window.holdNext=true;D('rlStudioFilter1').value='gauss-hires-lp';D('rlStudioFilter1').dispatchEvent(new Event('change',{bubbles:true}))");
  await check('first automatic edit may remain in flight without replacing the field',"typeof window.releaseAct==='function'&&D('rlStudioFilter1').value==='gauss-hires-lp'");
  await evaluate("D('rlStudioFilter1').value='poly-sinc-short-lp';D('rlStudioFilter1').dispatchEvent(new Event('change',{bubbles:true}))");
  await check('a second edit is retained and ordered behind the first',"rlWorkbenchQueue.pending===2&&D('rlStudioFilter1').value==='poly-sinc-short-lp'");
  await evaluate('releaseAct();window.releaseAct=null');
  await check('continuous edits settle on the final value in the original group',"!RL_STUDIO.pending&&rlWorkbenchQueue.pending===0&&rlCompare.slots[1].settings.filter==='poly-sinc-short-lp'&&rlCompare.slots[0].settings.filter==='gauss-hires-lp'");
  await evaluate("window.beforeUnsupported=actions.length;D('rlStudioPcmRate').value='176400';D('rlStudioPcmRate').dispatchEvent(new Event('change',{bubbles:true}));D('sRate').querySelector('[value=\"176400\"]').disabled=true;rlStudioSyncOptions()");
  await check('capability refresh rejects an unsupported draft before automatic submission',"actions.length===beforeUnsupported&&RL_STUDIO.draft.rate==='176400'&&D('rlStudioPcmRate').value==='176400'&&D('rlStudioMessage').dataset.error==='true'");
  await evaluate("D('rlStudioPcmRate').value='88200';D('rlStudioPcmRate').dispatchEvent(new Event('change',{bubbles:true}))");await check('choosing an available rate recovers after capability rejection',"!RL_STUDIO.pending&&Object.keys(RL_STUDIO.draft).length===0&&rlCompare.slots[1].settings.rate==='88200'");
  await evaluate("document.querySelector('[data-slot-name=B]').dispatchEvent(new MouseEvent('dblclick',{bubbles:true}));var n=document.querySelector('[data-slot-name=B] input');n.value='Warm & precise';n.dispatchEvent(new KeyboardEvent('keydown',{key:'Enter',bubbles:true}))");
  await check('double click renames a group in place',"!RL_STUDIO.pending&&rlCompare.slots[1].name==='Warm & precise'&&D('rlStudioSlots').textContent.includes('Warm & precise')");
  await evaluate("document.querySelector('[data-studio-advanced=pcm]').click()");
  await check('PCM advanced panel contains taps phase attenuation and cleanup',"D('rlStudioAdvancedDialog').open&&D('rlStudioAdvancedDialog').querySelectorAll('[data-studio-key]').length===9");
  await evaluate("var f=D('rlStudioAdvancedDialog').querySelector('[data-studio-key=taps]');f.value='4096';f.dispatchEvent(new Event('input',{bubbles:true}));f.blur()");
  await check('advanced changes automatically reach current group',"!RL_STUDIO.pending&&rlCompare.slots[1].settings.taps==='4096'");
  await evaluate("D('rlStudioAdvancedDialog').close();document.querySelector('[data-studio-advanced=sdm]').click()");
  await check('DSD advanced panel preserves SDM and source conversion controls while 1x/Nx moved outside',"D('rlStudioAdvancedDialog').querySelector('[data-studio-key=moddith]')&&D('rlStudioAdvancedDialog').querySelector('[data-studio-key=dsdkind]')&&D('rlStudioSdmFilter1').dataset.studioKey==='sdmfilt'&&D('rlStudioSdmFilterNx').dataset.studioKey==='sdmfiltnx'");
  await evaluate("D('rlStudioAdvancedDialog').close();failNext=true;D('rlStudioModFamily').value='ec';D('rlStudioModFamily').dispatchEvent(new Event('change',{bubbles:true}))");
  await check('rejected automatic write restores authoritative value and shows failure',"!RL_STUDIO.pending&&Object.keys(RL_STUDIO.inflight).length===0&&D('rlStudioModFamily').value==='dsd'&&D('rlStudioMessage').dataset.error==='true'");
  await evaluate("D('rlStudioModFamily').value='ec';D('rlStudioModFamily').dispatchEvent(new Event('change',{bubbles:true}))");
  await check('a later operation succeeds after a rejected write',"!RL_STUDIO.pending&&rlCompare.slots[1].settings.modfamily==='ec'");
  await evaluate("backend.state.position=24.5;rlConsole.refresh().then(()=>D('rlStudioSetStart').click())");
  await check('set start uses exact current relative position',"RL_STUDIO.range.start===24.5&&Number(rlCompare.loop.start)===24.5");
  await evaluate("backend.state.position=31.25;rlConsole.refresh().then(()=>D('rlStudioSetEnd').click())");
  await check('set end and readout retain fractional time',"Number(rlCompare.loop.end)===31.25&&D('rlStudioRangeReadout').textContent.includes('0:31.25')");
  await evaluate("D('rlStudioLoop').checked=true;D('rlStudioLoop').dispatchEvent(new Event('change',{bubbles:true}))");
  await check('loop choice is enforced by backend segment contract',"rlCompare.loop.repeat==='1'&&rlCompare.loop.enabled==='1'");
  await evaluate("window.beforeInvalid=actions.length;rlStudioRangeChange(50,40,true)");await check('invalid boundaries never submit a segment mutation',"actions.length===beforeInvalid&&D('rlStudioRangeState').dataset.error==='true'");
  await check('shared waveform draws true supplied bins and two boundary handles',"D('rlStudioWaveform').dataset.state==='ready'&&D('rlStudioWaveform').querySelector('.rlWaveBase').getAttribute('d').includes('V')&&D('rlStudioWaveform').querySelectorAll('.rlWaveHandle').length===2");
  await evaluate("D('rlStudioTab_eq').click()");
  await check('EQ tab contains complete 20 bands and 16 initially empty positions',"!D('rlStudioPanel_eq').hidden&&D('rlEqRows').querySelectorAll('.rlEqr').length===20&&RL_EQ.bank.length===16&&RL_EQ.bank.every(s=>!s.peq)");
  await check('enable control uses direct positive wording',"D('rlEqEnabled').parentElement.textContent.includes('启用均衡')&&!D('rlStudioPanel_eq').textContent.includes('旁路')");
  await evaluate("D('rlEqMainToggle').click()");await check('20-band EQ can collapse from its own header',"D('rlEqMainBody').hidden&&D('rlEqMainToggle').getAttribute('aria-expanded')==='false'");
  await evaluate("D('rlEqMainToggle').click()");await check('20-band EQ expands again with bands intact',"!D('rlEqMainBody').hidden&&D('rlEqRows').querySelectorAll('.rlEqr').length===20");
 await evaluate("rlEqOpenDialog('custom','')");await delay(120);
 await check('EQ save dialog opens centered instead of at the top-left corner',"(()=>{const d=D('rlEqSaveDialog');if(!d||!d.open)return false;const r=d.getBoundingClientRect();return r.width>200&&Math.abs((r.left+r.width/2)-innerWidth/2)<8&&Math.abs((r.top+r.height/2)-innerHeight/2)<8;})()");
 await evaluate("D('rlEqSaveDialog').close()");
  await evaluate("D('rlEqRows').querySelector('[data-i=\"2\"] .rlEqG').value='4.5';D('rlEqRows').querySelector('[data-i=\"2\"] .rlEqG').dispatchEvent(new Event('input',{bubbles:true}))");
  await delay(500);
  await check('frequency table edits update curve state and active group together',"!RL_STUDIO.pending&&!RL_EQ.editing&&rlCompare.slots[1].peq.bands[1].gain===4.5&&Math.abs(Math.max.apply(null,RL_EQ_STATE.points.map(function(p){return p[1]}))-4.5)<0.05");
  await evaluate("D('rlEqUndo').click()");await check('undo restores the prior real PEQ snapshot',"!RL_STUDIO.pending&&rlCompare.slots[1].peq.bands[1].gain===0");
  await evaluate("var svg=D('rlEqCurve').querySelector('svg'),node=svg.querySelector('.rlEqhit[data-i=\"2\"]'),r=svg.getBoundingClientRect();node.dispatchEvent(new PointerEvent('pointerdown',{bubbles:true,pointerId:1,clientX:r.left+r.width*.3,clientY:r.top+r.height*.5}));svg.dispatchEvent(new PointerEvent('pointermove',{bubbles:true,pointerId:1,clientX:r.left+r.width*.45,clientY:r.top+r.height*.3}));svg.dispatchEvent(new PointerEvent('pointerup',{bubbles:true,pointerId:1}))");
  await check('curve drag changes frequency gain and submitted parameters',"!RL_STUDIO.pending&&!RL_EQ.editing&&rlCompare.slots[1].peq.bands[1].gain>0&&rlCompare.slots[1].peq.bands[1].freq>100");
  await evaluate("window.holdNext=true;var g=D('rlEqRows').querySelector('[data-i=\"2\"] .rlEqG');g.value='2';g.dispatchEvent(new Event('input',{bubbles:true}))");
  await check('EQ accepts a slow backend without replacing an active edit',"typeof window.releaseAct==='function'&&D('rlEqRows').querySelector('[data-i=\"2\"] .rlEqG').value==='2'");
  await evaluate("var g=D('rlEqRows').querySelector('[data-i=\"2\"] .rlEqG');g.value='3';g.dispatchEvent(new Event('input',{bubbles:true}))");await check('continuous EQ edits serialize behind the pending write',"RL_EQ_WRITE.busy===true&&document.querySelectorAll('#rlEqRows .rlEqr')[1].querySelector('.rlEqG').value==='3'");
  await evaluate('releaseAct();window.releaseAct=null');await check('latest EQ edit wins after both writes complete',"!RL_STUDIO.pending&&!RL_EQ.editing&&rlCompare.slots[1].peq.bands[1].gain===3");
  await evaluate("D('rlEqUndo').click()");await check('undo after concurrent edits returns to the immediately prior accepted value',"!RL_STUDIO.pending&&rlCompare.slots[1].peq.bands[1].gain===2");
  await evaluate("failNext=true;D('rlEqPreamp').value='-9';D('rlEqPreamp').dispatchEvent(new Event('input',{bubbles:true}))");await check('failed EQ write restores preamp curve and table from the backend',"!RL_STUDIO.pending&&!RL_EQ.editing&&D('rlEqPreamp').value==='0'&&D('rlEqMsg').dataset.error==='true'&&rlCompare.slots[1].peq.preamp===0");
  await evaluate("var row=D('rlEqRows').querySelector('[data-i=\"2\"]');row.querySelector('.rlEqF').value='20000';row.querySelector('.rlEqG').value='24';row.querySelector('.rlEqQ').value='20';rlEqApply()");await check('EQ accepts exact supported frequency gain and Q boundaries',"!RL_STUDIO.pending&&!RL_EQ.editing&&rlCompare.slots[1].peq.bands[1].freq===20000&&rlCompare.slots[1].peq.bands[1].gain===24&&rlCompare.slots[1].peq.bands[1].q===20");
  for(let i=1;i<=10;i++){
   await evaluate('rlEqBankSelect('+JSON.stringify(String(i))+');rlEqOpenDialog("custom","");D("rlEqSaveName").value="Custom '+i+'";D("rlEqSaveCommit").click()');
   await check('custom EQ position '+i+' is named and saved',"!RL_STUDIO.pending&&RL_EQ.bank["+(i-1)+"].name==='Custom "+i+"'&&RL_EQ.bank["+(i-1)+"].peq.bands.length===20");
  }
  await evaluate("rlEqRenameSlot('1')");
  await check('rename field opens for the selected saved EQ',"!!document.querySelector('.rlEqRenameInput')");
  await evaluate("var i=document.querySelector('.rlEqRenameInput');i.value='Renamed 1';i.dispatchEvent(new KeyboardEvent('keydown',{key:'Enter',bubbles:true}))");
  await check('renaming a saved EQ persists the new title',"RL_EQ.bank[0].name==='Renamed 1'");
  await check('playback quick EQ separates headphone and custom entries',"D('rlStudioQuickEqCustom').options.length===11&&D('rlStudioQuickEqHeadphone').options.length===1");
  await evaluate("rlEqBankSelect('3',true)");await check('clicking a saved EQ loads it and updates enable curve and table',"!RL_STUDIO.pending&&rlCompare.slots[1].eqId==='3'&&D('rlEqEnabled').checked&&D('rlEqRows').querySelectorAll('.rlEqr').length===20");
  await evaluate("rlEqBankSelect('10');D('rlEqDelete').click()");await check('deleting a user EQ clears only its saved position',"!RL_STUDIO.pending&&!RL_EQ.bank[9].peq&&!!RL_EQ.bank[8].peq");
  await evaluate("D('rlEqOpraSearch').value='HD 600';D('rlEqOpraSearch').dispatchEvent(new Event('input',{bubbles:true}))");
  await check('OPRA search displays matching real database results',"D('rlEqOpraResults').querySelectorAll('[data-product]').length===1&&D('rlEqOpraResults').textContent.includes('HD 600')");
  await evaluate("D('rlEqOpraResults').querySelector('button').click()");await check('selecting a headphone fetches all available calibration details',"D('rlEqOpraPresets').textContent.includes('Fixture Lab')&&D('rlEqOpraPresets').querySelector('[data-opra-preset]')");
  await evaluate("D('rlEqOpraPresets').querySelector('[data-opra-preset]').click()");await check('OPRA preset click previews without saving to a slot',"!!RL_EQ.opraPreview&&!D('rlEqOpraPresets').querySelector('[data-opra-save]').disabled&&RL_EQ_STATE.preamp===-4&&RL_EQ.bank.filter(function(s){return s.source==='opra'}).length===0");
  await evaluate("rlEqOpenDialog('opra','');D('rlEqSaveSlot').value='10';D('rlEqSaveName').value='Fixture OPRA';D('rlEqSaveCommit').click()");await check('OPRA save writes only the chosen slot and applies it',"!RL_STUDIO.pending&&RL_EQ.bank[9].source==='opra'&&RL_EQ_STATE.preamp===-4&&D('rlEqEnabled').checked&&rlCompare.slots[1].peq.bands[1].gain===3&&rlCompare.slots[0].peq.bands[1].gain===0");
  await evaluate("D('rlEqAutoPre').click()");await check('automatic preamp subtracts the existing preamp before finding filter gain',"!RL_STUDIO.pending&&!RL_EQ.editing&&Number(D('rlEqPreamp').value)===-4&&rlCompare.slots[1].peq.preamp===-4");
  await evaluate("D('rlEqRestore').click()");await check('one-click restore returns a flat curve and disables EQ',"!RL_EQ.editing&&RL_EQ_STATE.bypass===1&&D('rlEqEnabled').checked===false");
  await check('all profile writes retain global session volume and device',"backend.state.settings.volume==='-64.5'&&backend.state.settings.backend==='null'&&backend.state.settings.device==='Fixture Null'&&actions.filter(a=>a.url==='/api/workbench/profile').every(a=>!('volume'in a)&&!('backend'in a)&&!('device'in a))");
  await evaluate("backend.state.cueStart=165;backend.state.cueEnd=225;backend.state.queueIndex=2;backend.state.duration=60;backend.loop={enabled:false,repeat:1,file:backend.state.file,cueStart:45,cueEnd:165,start:70,end:110};rlConsole.refresh().then(()=>rlStudioWave())");await check('adjacent CUE tracks never inherit the earlier fragment boundaries',"RL_STUDIO.range.start===0&&RL_STUDIO.range.end===60&&!D('rlStudioLoop').checked");
  await evaluate("rlStudioTab('ab');window.holdNext=true;D('rlStudioFilter1').value='gauss-hires-lp';D('rlStudioFilter1').dispatchEvent(new Event('change',{bubbles:true}));window.delayedSwitch=rlStudioSelect('C');undefined");
  await check('switching groups locks DSP and EQ throughout the pending old-group flush',"typeof window.releaseAct==='function'&&RL_STUDIO.switching&&D('rlStudioFilter1').disabled&&D('rlStudioPanel_eq').inert");
  await evaluate("D('rlStudioFilter1').value='poly-sinc-long-lp';D('rlStudioFilter1').dispatchEvent(new Event('change',{bubbles:true}));var g=D('rlEqRows').querySelector('[data-i=\"2\"] .rlEqG');g.value='19';g.dispatchEvent(new Event('input',{bubbles:true}));releaseAct();window.releaseAct=null;window.delayedSwitch");
  await check('edits attempted during switching never leak from A/B into destination C',"!RL_STUDIO.switching&&!RL_STUDIO.pending&&rlCompare.activeId==='C'&&rlCompare.slots[2].settings.filter==='poly-sinc-short-lp'&&rlCompare.slots[2].peq.bands[1].gain===0&&Object.keys(RL_STUDIO.draft).length===0&&!D('rlStudioFilter1').disabled&&!D('rlStudioPanel_eq').inert");
  await evaluate("rlStudioTab('ab');document.querySelector('[data-slot-name=A]').scrollIntoView({block:'center'})");
  const renamePoint=await evaluate("(()=>{var r=document.querySelector('[data-slot-name=A]').getBoundingClientRect();return {x:r.left+r.width/2,y:r.top+r.height/2};})()");
  await browser.send('Input.dispatchMouseEvent',{type:'mouseMoved',...renamePoint});
  for(const clickCount of [1,2]){await browser.send('Input.dispatchMouseEvent',{type:'mousePressed',button:'left',clickCount,...renamePoint});await browser.send('Input.dispatchMouseEvent',{type:'mouseReleased',button:'left',clickCount,...renamePoint});if(clickCount===1)await delay(70);}
  await check('real mouse double click edits an inactive name without switching groups',"RL_STUDIO.rename==='A'&&rlCompare.activeId==='C'&&document.querySelector('[data-slot-name=A] input')===document.activeElement");
  await browser.send('Input.insertText',{text:'Real double click A'});await browser.send('Input.dispatchKeyEvent',{type:'keyDown',key:'Enter',code:'Enter',windowsVirtualKeyCode:13});await browser.send('Input.dispatchKeyEvent',{type:'keyUp',key:'Enter',code:'Enter',windowsVirtualKeyCode:13});
  await check('real double click name commits without losing independent selection',"!RL_STUDIO.pending&&!RL_STUDIO.rename&&rlCompare.activeId==='C'&&rlCompare.slots[0].name==='Real double click A'");
  await evaluate("rlStudioTab('eq');window.beforeBandKey=actions.filter(a=>a.globalArrow).length;document.querySelector('#rlEqRows .rlEqr[data-i=\"2\"]').focus()");
  await browser.send('Input.dispatchKeyEvent',{type:'keyDown',key:'ArrowUp',code:'ArrowUp',windowsVirtualKeyCode:38});await browser.send('Input.dispatchKeyEvent',{type:'keyUp',key:'ArrowUp',code:'ArrowUp',windowsVirtualKeyCode:38});
  await check('real arrow key changes only the selected EQ band and never reaches global transport',"!RL_STUDIO.pending&&!RL_EQ.editing&&rlCompare.slots[2].peq.bands[1].gain===.1&&actions.filter(a=>a.globalArrow).length===beforeBandKey");
  const geometry=[];
  for(const width of [320,768,1340]){
   await browser.send('Emulation.setDeviceMetricsOverride',{width,height:1000,deviceScaleFactor:1,mobile:false});
   for(const tab of ['ab','eq','monitor']){
    await evaluate('rlStudioTab('+JSON.stringify(tab)+');rlStudioSync()');await delay(80);
    const size=await evaluate('({viewport:innerWidth,scroll:document.documentElement.scrollWidth,modules:[...document.querySelectorAll(".digitalModule")].map(e=>Math.round(e.getBoundingClientRect().width))})');
    assert.ok(size.scroll<=width,'no horizontal overflow at '+width+' in '+tab);if(tab==='ab')assert.ok(size.modules.every(v=>v>0),'AB modules must actually be visible');geometry.push({tab,...size});checks.push('no horizontal overflow at '+width+' in '+tab);console.log('PASS no horizontal overflow at '+width+' in '+tab);
    const shot=await browser.send('Page.captureScreenshot',{format:'png',captureBeyondViewport:false});fs.writeFileSync(path.join(run,'module-'+tab+'-'+width+'.png'),Buffer.from(shot.data,'base64'));
   }
  }
  await evaluate("D('rlStudioTab_monitor').click()");await check('monitor is a separate accessible tab with current track metadata',"!D('rlStudioPanel_monitor').hidden&&D('rlStudioMonitorTrack').textContent==='Fixture concerto'&&D('rlStudioMonitorMeta').textContent.includes('Fixture Ensemble')");
  await evaluate("rlMode='listen';rlPaintStudio(RL_STUDIO.frame)");
  await check('console is hidden in listening mode',"D('rlStudio').hidden");
  assert.deepEqual(errors,[]);fs.writeFileSync(path.join(run,'results.json'),JSON.stringify({checks:checks.length,failures:0,errors,geometry,run},null,2));
  console.log(JSON.stringify({checks:checks.length,failures:0,run}));
 }finally{await browser.close();}
}
main().catch(error=>{console.error(error);process.exitCode=1;});
