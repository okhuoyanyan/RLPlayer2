// Render the shipped page and script in an isolated Edge profile. Every URL is
// answered through its private DevTools pipe. No HTTP server or player is used.
'use strict';
const assert = require('node:assert/strict');
const vm = require('node:vm');
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
const workRoot = path.resolve(option('--work-root', 'listening-modes-evidence'));
fs.mkdirSync(workRoot, {recursive:true});
const run = fs.mkdtempSync(path.join(workRoot, 'modes-'));
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
const settings={quickstart:'1',uilang:'zh',filter:'poly-sinc-short-lp',filterNx:'',backend:'wasapi',device:'dac-fixture',volume:'-42.5',rate:'88200',dsd:'0',directsdm:'0',gpuvendor:'off',metaon:'0',metallm:'0',anlmaster:'0',autoanl:'0',roomcorrauto:'0',ui_profile:'pcm',ui_style:'balanced'};
const mutations=[],requests=[],heldSettings=[],heldMetadata=[],forbidden=[],errors=[],checks=[],explicitPlays=[];
const facts={language:'zh',holdSettings:true,files:[],cues:[],current:'',paused:false,playing:false,allowWorkbench:false,failWorkbench:false,networkFail:false,profile:'pcm',style:'balanced',revision:1};
function runCapabilityMatrix() {
  const file=path.join(root,'src/net/ui_listening_home.h'),source=fs.readFileSync(file,'utf8');
  const production=[...source.matchAll(/return R"LISTENHOME\(([\s\S]*?)\)LISTENHOME";/g)][1][1];
  const context={rlConsole:{state:{settings:{backend:'wasapi'},file:''}},S:{metaFile:'',curMeta:null}};
  vm.createContext(context);vm.runInContext(production,context,{filename:file});
  const rows=[
    {name:'native-only 96k cannot deliver retained-gain DSD',backend:'asio',rates:[44100,48000,96000],native:true,rate:44100,want:false},
    {name:'44.1k source accepts a 44.1k-family carrier',rates:[44100,88200,176400],rate:44100,want:true},
    {name:'48k source rejects 44.1k-only carrier',rates:[44100,88200,176400],rate:48000,want:false},
    {name:'88.2k source keeps its 44.1k family',rates:[44100,88200,176400],rate:88200,want:true},
    {name:'96k source rejects 44.1k-only carrier',rates:[44100,88200,176400],rate:96000,want:false},
    {name:'44.1k DSD source accepts its carrier',rates:[176400],rate:2822400,file:'source.dsf',want:true},
    {name:'48k DSD source rejects 44.1k-only carrier',rates:[176400],rate:3072000,file:'source.dsf',want:false},
    {name:'44.1k source rejects 48k-only carrier',rates:[48000,96000,192000],rate:44100,want:false},
    {name:'48k source accepts a 48k-family carrier',rates:[48000,96000,192000],rate:48000,want:true},
    {name:'88.2k source rejects 48k-only carrier',rates:[192000],rate:88200,want:false},
    {name:'96k source accepts its 48k family',rates:[192000],rate:96000,want:true},
    {name:'44.1k DSD source rejects 48k-only carrier',rates:[192000],rate:2822400,file:'source.dsf',want:false},
    {name:'48k DSD source accepts its carrier',rates:[192000],rate:3072000,file:'source.dsf',want:true},
    {name:'mixed carriers accept PCM 44.1k',rates:[176400,192000],rate:44100,want:true},
    {name:'mixed carriers accept PCM 48k',rates:[176400,192000],rate:48000,want:true},
    {name:'mixed carriers accept DSD 44.1k',rates:[176400,192000],rate:2822400,file:'source.dsf',want:true},
    {name:'mixed carriers accept DSD 48k',rates:[176400,192000],rate:3072000,file:'source.dff',want:true},
    {name:'a high DSD256 carrier is sufficient',rates:[705600],rate:44100,want:true},
    {name:'48k DSD256 carrier is sufficient',rates:[768000],rate:48000,want:true},
    {name:'a DSD1024 carrier is included',rates:[2822400],rate:44100,want:true},
    {name:'a 48k DSD1024 carrier is included',rates:[3072000],rate:48000,want:true},
    {name:'stale metadata must not choose a source family',rates:[176400,192000],rate:44100,metaFile:'previous.wav',want:false},
    {name:'pending metadata holds DSD unavailable',rates:[176400,192000],meta:null,want:false},
    {name:'zero source rate is unconfirmed',rates:[176400,192000],rate:0,want:false},
    {name:'invalid source rate is unconfirmed',rates:[176400,192000],rate:'bad',want:false},
    {name:'no-file planning uses the backend 44.1k default',rates:[176400],file:'',rate:48000,want:true},
    {name:'no-file planning rejects a 48k-only carrier',rates:[192000],file:'',rate:44100,want:false},
    {name:'matching metadata can use the rate field',rates:[192000],meta:{rate:48000},want:true},
    {name:'matching metadata can use sampleRate',rates:[176400],meta:{sampleRate:44100},want:true},
    {name:'carrier membership works without native or DoP flags',rates:[176400],native:false,dop:false,rate:44100,want:true},
    {name:'empty rates never trust DSD capability flags',rates:[],native:true,dop:true,rate:44100,want:false},
    {name:'Null behavior is retained without source metadata',backend:'null',rates:[],meta:null,want:true},
    {name:'shared output keeps DSD unavailable',backend:'wasapi-shared',rates:[176400],rate:44100,want:false},
    {name:'network output keeps DSD unavailable',backend:'upnp',rates:[176400],rate:44100,want:false},
    {name:'PCM availability does not require source metadata',rates:[44100,176400],meta:null,profile:'pcm',want:true},
    {name:'original playback stays available',rates:[],meta:null,profile:'bitperfect',want:true},
    {name:'unknown profile is not selectable',rates:[176400],rate:44100,profile:'custom',want:false}
  ],results=[];
  try {
    for(const row of rows) {
      const currentFile=row.file===undefined?'source.wav':row.file;
      context.rlConsole.state={settings:{backend:row.backend||'wasapi'},file:currentFile};
      context.S={metaFile:row.metaFile===undefined?currentFile:row.metaFile,curMeta:Object.hasOwn(row,'meta')?row.meta:{sr:row.rate}};
      context.rlListening.caps={ok:true,device:{name:'Capability fixture',rates:row.rates,maxRate:Math.max(0,...row.rates),dsdNative:!!row.native,dop:row.dop===undefined?true:row.dop}};
      const actual=context.rlListeningProfileAvailable(row.profile||'dsd');
      results.push({name:row.name,expected:row.want,actual});checked('capability matrix · '+row.name,actual===row.want);
    }
  } finally {fs.writeFileSync(path.join(run,'capability-matrix.json'),JSON.stringify({results,source:file},null,2));}
}
function workbench(){return {ok:true,settings:{...settings},profile:facts.profile,mode:facts.profile,style:facts.style,revision:String(facts.revision),playing:facts.playing,paused:facts.paused,file:facts.current,position:12.75,duration:180,queueIndex:0,cueStart:-1,cueEnd:-1,peq:{bypass:true,preamp:0,bands:[]},actual:{outRate:Number(settings.rate)||44100,outBits:24,device:'Isolated fixture DAC'}};}
function mock(url,method,body) {
  const p=url.pathname;requests.push({path:p,method});
  if(p==='/api/workbench/apply') {
    if(!facts.allowWorkbench){forbidden.push('unapproved '+p);return {ok:false,error:'unexpected write'};}
    const patch=Object.fromEntries(new URLSearchParams(body));mutations.push(patch);
    if(facts.failWorkbench)return {ok:false,error:'fixture rejected'};
    assert.equal(patch.expected_file,facts.current);assert.equal(patch.expected_revision,String(facts.revision));
    for(const [k,v] of Object.entries(patch))if(!['expected_file','expected_revision','profile','style'].includes(k))settings[k]=v;
    if(patch.profile){facts.profile=patch.profile;settings.ui_profile=patch.profile;settings.rate=patch.profile==='bitperfect'?'src':'176400';settings.filter=patch.profile==='bitperfect'?'':'poly-sinc-short-lp';settings.dsd=patch.profile==='dsd'?'128':'0';}
    if(patch.style){facts.style=patch.style;settings.ui_style=patch.style;settings.filter=patch.style==='warm'?'gauss-long':patch.style==='sharp'?'poly-sinc-long-lp':'poly-sinc-short-lp';}
    facts.revision++;return {ok:true};
  }
  if(p==='/api/play'&&method==='POST'&&facts.nextExplicitPlay){facts.nextExplicitPlay=false;explicitPlays.push(Object.fromEntries(new URLSearchParams(body)));return {ok:true};}
  if((p==='/api/settings'||p==='/api/workbench/profile'||p==='/api/workbench/eqpresets'||p==='/api/workbench/loop'||p==='/api/peq')&&method==='POST'||/^\/api\/(?:play|pause|stop|skip|seek|volume|playmode|qdel|qmove|lib\/play|sq\/play|cast\/start|input\/start|roomcorr\/measure|preset\/apply|queue\/(?:set|clear|add|sort|undo))$/.test(p)) {
    forbidden.push(method+' '+p);return {ok:false,error:'forbidden fixture action'};
  }
  switch(p) {
    case '/api/settings':return {ok:true,kv:{...settings}};
    case '/api/workbench/state':return workbench();
    case '/api/workbench/profiles':return {ok:true,active:'A',slots:['A','B','C','D'].map(id=>({id,name:id,settings:{...settings},peq:{bypass:true,preamp:0,bands:[]},eqId:'',eqName:'',migrated:true})),loop:{enabled:false,start:0,end:0,file:''}};
    case '/api/workbench/eqpresets':return {ok:true,slots:Array.from({length:16},(_,i)=>({id:String(i+1),name:'',source:'custom',peq:null,empty:true})),activeId:''};
    case '/api/status':return {ok:true,playing:facts.playing,paused:facts.paused,current:facts.current,rate:44100,dur:facts.current?180:0,pos:facts.current?12.75:0,volDb:-42.5,startDb:-42.5,outRate:88200,outBits:24,outMode:'pcm',error:'',gpu:false};
    case '/api/queue':return {ok:true,items:facts.files,cues:facts.cues,index:0,mode:0,repeat:0,shuffle:false};
    case '/api/ping':return {ok:true,version:'3.3.6'};
    case '/api/i18n':return {ok:true,lang:facts.language,map:facts.language==='zh'?{}:dictionaries[facts.language]};
    case '/api/preset/caps':return {ok:true,device:{name:'Isolated fixture DAC',rates:[44100,88200,176400],maxRate:176400,dop:true,dopMax:'dsd128',dsdNative:false,exclusive:true},notes:[]};
    case '/api/devices':return {ok:true,devices:[{id:'dac-fixture',name:'Isolated fixture DAC'},{id:'second-fixture',name:'Second room DAC'}],asioDrivers:[{clsid:'direct-fixture',name:'Direct fixture DAC'}]};
    case '/api/cast/devices':return {ok:true,devices:[{location:'http://renderer.invalid/device.xml',friendlyName:'Living room'}]};
    case '/api/lib/stats':return {ok:true,tracks:0,albums:0,scanning:false,roots:[]};
    case '/api/lib/albums':return {ok:true,albums:[]};
    case '/api/filters':return {ok:true,items:[],filters:[]};
    case '/api/rates':return {ok:true,rates:[44100,88200,176400],sysRate:44100};
    case '/api/signal':return {ok:!!facts.current,path:facts.current?{src:facts.current,device:'Isolated fixture DAC',deviceId:settings.device,inRate:44100,inBits:16,outRate:88200,outBits:24,codec:'PCM_S16LE',filter:settings.filter}:null};
    case '/api/info':
    case '/api/meta':{const file=url.searchParams.get('file')||'';return {ok:true,file,path:file,title:'Fixture concerto',artist:'Fixture artist',album:'Fixture album',sr:/\.(dsf|dff)$/i.test(file)?2822400:44100,bits:16};}
    default:return {ok:true,items:[],albums:[],roots:[],tracks:[],configured:false};
  }
}
async function main() {
  const assets=exportPage(),browser=startBrowser();let scenario='startup';
  const listen='#rlViewModes [data-rl-mode=listen]',pro='#rlViewModes [data-rl-mode=pro]';
  async function evaluate(expression){const r=await browser.send('Runtime.evaluate',{expression,awaitPromise:true,returnByValue:true});if(r.exceptionDetails)throw Error(r.exceptionDetails.exception?.description||r.exceptionDetails.text);return r.result.value;}
  async function until(expression,label){const end=Date.now()+9000;while(Date.now()<end){if(await evaluate('Boolean('+expression+')'))return;await delay(55)}throw Error('Not ready: '+(label||expression));}
  async function check(name,expr){await until(expr,name);checked(name,true);}
  async function click(selector){
    await until('document.querySelector('+JSON.stringify(selector)+')',selector);
    const hit=await evaluate('(()=>{const e=document.querySelector('+JSON.stringify(selector)+');e.scrollIntoView({block:"nearest",inline:"nearest"});const r=e.getBoundingClientRect(),x=r.x+r.width/2,y=r.y+r.height/2,h=document.elementFromPoint(x,y);return {x,y,w:r.width,h:r.height,hit:!!h&&(h===e||e.contains(h)),label:e.textContent.slice(0,60)}})()');
    assert.ok(hit.w>0&&hit.h>0&&hit.hit,'Visible pointer target '+selector+' '+JSON.stringify(hit));
    await browser.send('Input.dispatchMouseEvent',{type:'mousePressed',x:hit.x,y:hit.y,button:'left',clickCount:1});await browser.send('Input.dispatchMouseEvent',{type:'mouseReleased',x:hit.x,y:hit.y,button:'left',clickCount:1});
  }
  async function select(id,value){await evaluate('var e=D('+JSON.stringify(id)+');e.value='+JSON.stringify(value)+';e.dispatchEvent(new Event("change",{bubbles:true}))');}
  async function close(){await click('#rlListeningDialogCancel');await until('!D("rlListeningDialog").open');await delay(60);}
  async function refresh(){await evaluate('rlConsole.refresh().then(()=>tick())');await delay(60);}
  async function setProfile(name,style){facts.profile=name;facts.style=style;settings.ui_profile=name;settings.ui_style=style;settings.rate=name==='bitperfect'?'src':'176400';settings.filter=name==='bitperfect'?'':style==='warm'?'gauss-long':style==='sharp'?'poly-sinc-long-lp':'poly-sinc-short-lp';settings.dsd=name==='dsd'?'128':'0';await refresh();}
  async function shot(tag){const x=await browser.send('Page.captureScreenshot',{format:'png',captureBeyondViewport:false});fs.writeFileSync(path.join(run,tag+'.png'),Buffer.from(x.data,'base64'));}
  async function press(key,code,vk){await browser.send('Input.dispatchKeyEvent',{type:'keyDown',key,code,windowsVirtualKeyCode:vk});await browser.send('Input.dispatchKeyEvent',{type:'keyUp',key,code,windowsVirtualKeyCode:vk});}
  try {
    await browser.attach();await browser.send('Page.enable');await browser.send('Runtime.enable');
    browser.onRequest=async({requestId,request})=>{
      const url=new URL(request.url);let type='application/json',body='',code=200;
      if(url.origin!==origin){forbidden.push('external '+url.href);code=403;}
      else if(url.pathname==='/'){type='text/html; charset=utf-8';body=assets.html.replace('FILTER_OPTS_1X','<option value=auto>Auto</option><option value=poly-sinc-short-lp>Short</option><option value=gauss-long>Warm</option><option value=poly-sinc-long-lp>Sharp</option>').replace('FILTER_OPTS_NX','<option value="">Follow 1x</option>').replace('SHAPER_OPTS','<option value=auto>Auto</option><option value=none>None</option>');}
      else if(url.pathname==='/app.js'){type='application/javascript; charset=utf-8';body=assets.js;}
      else if(url.pathname==='/api/settings'&&request.method==='GET'&&facts.holdSettings){heldSettings.push(requestId);return;}
      else if(url.pathname==='/api/meta'&&facts.holdMetadata){heldMetadata.push({requestId,file:url.searchParams.get('file')});return;}
      else if(url.pathname==='/api/workbench/apply'&&facts.networkFail){facts.networkFail=false;await browser.send('Fetch.failRequest',{requestId,errorReason:'InternetDisconnected'});return;}
      else if(url.pathname==='/api/cover'){type='image/png';code=404;}
      else if(url.pathname.startsWith('/api/'))body=JSON.stringify(mock(url,request.method,request.postData));
      else code=404;
      await browser.send('Fetch.fulfillRequest',{requestId,responseCode:code,responseHeaders:[{name:'Content-Type',value:type},{name:'Cache-Control',value:'no-store'}],body:Buffer.from(body).toString('base64')});
    };
    await browser.send('Fetch.enable',{patterns:[{urlPattern:'*',requestStage:'Request'}]});
    await browser.send('Emulation.setDeviceMetricsOverride',{width:1280,height:880,deviceScaleFactor:1,mobile:false});
    await browser.send('Page.navigate',{url:origin+'/'});await until('typeof D==="function"&&D("bPause")&&typeof PLAYBACK_SETTINGS!=="undefined"');
    await click('.side .nav[data-v=listening]');await until('D("rlListeningControls")');
    checked('startup holds authoritative settings',await evaluate('!PLAYBACK_SETTINGS.ready&&D("vol").disabled'));
    await evaluate('D("bPause").click();D("bNext").click();D("bPrev").click();D("vol").value="-10";D("vol").dispatchEvent(new Event("input"));saveSettings(true)');await delay(150);
    checked('early playback and gain edits cannot reach backend',mutations.length===0&&forbidden.length===0);
    checked('listening choices wait for saved settings',await evaluate('D("rlListeningDevice").disabled&&document.querySelector("[data-rl-listen-task=profile]").disabled'));
    facts.holdSettings=false;for(const requestId of heldSettings.splice(0))await browser.send('Fetch.fulfillRequest',{requestId,responseCode:200,responseHeaders:[{name:'Content-Type',value:'application/json'}],body:Buffer.from(JSON.stringify({ok:true,kv:{...settings}})).toString('base64')});
    await check('saved exact fractional gain unlocks ordinary controls','PLAYBACK_SETTINGS.ready&&S.vol===-42.5&&!D("rlListeningDevice").disabled');
    checked('settings completion never replays early actions',mutations.length===0&&forbidden.length===0);
    await check('four listening controls share the home','document.querySelectorAll("#rlListeningControls .rlListeningChoice").length===4&&!!D("rlListenHome")');
    await check('listening has one home and no settings or quick-start navigation','!document.querySelector(".side .nav[data-v=settings]").checkVisibility()&&(!D("navQuickStart")||!D("navQuickStart").checkVisibility())&&D("rlHomeNavLabel").textContent==="聆听室"');
    await check('direct dropdown names the current device','D("rlListeningDevice").selectedOptions[0].textContent==="Isolated fixture DAC"');
    await click('[data-rl-listen-task=profile]');await check('profile card stays on listening home','D("rlListeningDialog").open&&S.view!=="settings"&&document.querySelectorAll("[data-rl-listening-profile]").length===3');await close();
    await click('[data-rl-listen-task=output]');await check('output choices use ordinary words','D("rlListeningRoute").options.length===2&&D("rlListeningRoute").textContent.includes("本机")&&!/WASAPI|ASIO/.test(D("rlListeningDialog").textContent)&&D("rlListeningExclusive").checked');await close();
    checked('all card previews remain read only',mutations.length===0&&forbidden.length===0);
    facts.allowWorkbench=true;facts.failWorkbench=true;await select('rlListeningDevice','2');
    await check('a rejected direct device selection displays the failure','D("rlListeningNotice").textContent.includes("fixture rejected")&&!rlListening.working');
    checked('rejected device keeps saved values and exact gain',settings.device==='dac-fixture'&&settings.volume==='-42.5');
    await check('rejected dropdown reverts to authoritative selection','D("rlListeningDevice").selectedOptions[0].textContent==="Isolated fixture DAC"');
    facts.failWorkbench=false;await select('rlListeningDevice','2');
    await check('named device applies directly without changing page','rlConsole.state.settings.device==="second-fixture"&&!rlConsole.busy&&S.view!=="settings"');
    checked('device apply supplies revision and leaves volume alone',mutations.at(-1).device==='second-fixture'&&mutations.at(-1).expected_revision&&mutations.at(-1).volume===undefined&&settings.volume==='-42.5');
    scenario='source-metadata-pending';facts.current='Q:/fixture/metadata-pending.wav';facts.playing=true;facts.holdMetadata=true;await setProfile('pcm','balanced');
    await until('S.metaFile==="Q:/fixture/metadata-pending.wav"&&!S.curMeta');
    checked('current source metadata request is actually held',heldMetadata.length>0);
    const beforeMetadata=mutations.length;await click('[data-rl-listen-task=profile]');
    await check('pending source metadata disables DSD with the existing unconfirmed hint','document.querySelector("[data-rl-listening-profile=dsd]").disabled&&D("rlListeningCapabilityHint").textContent==="当前输出的升频能力尚未确认。"');
    facts.holdMetadata=false;
    for(const pending of heldMetadata.splice(0))await browser.send('Fetch.fulfillRequest',{requestId:pending.requestId,responseCode:200,responseHeaders:[{name:'Content-Type',value:'application/json'}],body:Buffer.from(JSON.stringify({ok:true,file:pending.file,path:pending.file,title:'Fixture concerto',sr:44100,bits:16})).toString('base64')});
    await check('matching source metadata naturally repaints the DSD choice','S.metaFile===rlConsole.state.file&&S.curMeta&&S.curMeta.sr===44100&&!document.querySelector("[data-rl-listening-profile=dsd]").disabled');
    checked('metadata completion never applies a playback configuration',mutations.length===beforeMetadata);await close();
    for(const file of ['Q:/fixture/source.wav','Q:/fixture/source.dsf']) {
      scenario='unified-dsd-'+path.extname(file);facts.current=file;facts.playing=true;await setProfile('pcm','balanced');
      await until('S.metaFile==='+JSON.stringify(file)+'&&S.curMeta&&Number(S.curMeta.sr)>0');
      const count=mutations.length;await click('[data-rl-listen-task=profile]');await click('[data-rl-listening-profile=dsd]');checked('DSD preview is read only for '+path.extname(file),mutations.length===count);
      await click('#rlListeningDialogApply');await check('DSD choice succeeds for '+path.extname(file),'!D("rlListeningDialog").open&&D("rlListeningProfileName").textContent==="DSD 升频"&&!rlConsole.busy');
      const patch=mutations.at(-1);checked('one unified DSD request carries the same '+path.extname(file)+' source',patch.profile==='dsd'&&patch.expected_file===file&&patch.style==='balanced');
    }
    for(const [style,label] of [['warm','温和'],['balanced','均衡'],['sharp','锐利']]) {
      scenario='style-'+style;await click('[data-rl-listen-task=style]');await click('[data-rl-listening-style='+style+']');await click('#rlListeningDialogApply');
      await check('style applies and reads back '+style,'!D("rlListeningDialog").open&&!rlConsole.busy&&D("rlListeningStyleName").textContent==='+JSON.stringify(label));checked('style changes preserve gain '+style,settings.volume==='-42.5'&&mutations.at(-1).style===style);
    }
    await click('[data-rl-listen-task=style]');await click('[data-rl-listening-style=warm]');const beforeNetwork=mutations.length;facts.networkFail=true;await click('#rlListeningDialogApply');
    await check('network failure keeps the card and reports error','D("rlListeningDialog").open&&!rlConsole.busy&&D("rlListeningDialogMessage").dataset.error==="true"');
    checked('network failure does not invent success or retry a write',mutations.length===beforeNetwork&&facts.style==='sharp');await check('actual style survives disconnected submit','D("rlListeningStyleName").textContent==="锐利"');
    await click('#rlListeningDialogApply');await check('explicit retry can finish without losing card draft','!D("rlListeningDialog").open&&!rlConsole.busy&&D("rlListeningStyleName").textContent==="温和"');
    await click('[data-rl-listen-task=profile]');await click('[data-rl-listening-profile=bitperfect]');await click('#rlListeningDialogApply');
    await check('straight route disables style without suggesting coloration','!D("rlListeningDialog").open&&document.querySelector("[data-rl-listen-task=style]").disabled&&D("rlListeningStyleName").textContent==="保持原音"');
    await click('[data-rl-listen-task=output]');await evaluate('D("rlListeningExclusive").checked=false;D("rlListeningExclusive").dispatchEvent(new Event("change"))');await click('#rlListeningDialogApply');await until('!D("rlListeningDialog").open&&!rlConsole.busy');
    await click('[data-rl-listen-task=profile]');await check('shared output makes unsupported upsampling explicit','document.querySelector("[data-rl-listening-profile=pcm]").disabled&&document.querySelector("[data-rl-listening-profile=dsd]").disabled&&D("rlListeningCapabilityHint").textContent.includes("独占")');await close();
    await select('rlListeningDevice','3');await until('rlConsole.state.settings.backend==="asio"&&!rlConsole.busy');await click('[data-rl-listen-task=output]');
    await check('direct hardware keeps exclusive on with a plain label','D("rlListeningExclusive").checked&&D("rlListeningExclusive").disabled&&D("rlListeningOutputHint").textContent==="设备直连"');await close();
    facts.current='';facts.playing=false;await refresh();await click('[data-rl-listen-task=output]');await select('rlListeningRoute','network');await until('D("rlListeningOutputDevice").options.length===2');
    await check('network output requires an explicit discovered renderer','D("rlListeningDialogApply").disabled&&D("rlListeningNetworkScan").checkVisibility()');
    await select('rlListeningOutputDevice','0');await click('#rlListeningDialogApply');await until('!D("rlListeningDialog").open&&!rlConsole.busy');
    checked('network apply carries renderer identity',mutations.at(-1).backend==='upnp'&&mutations.at(-1).upnpdev==='http://renderer.invalid/device.xml');
    settings.backend='wasapi';settings.device='dac-fixture';await setProfile('pcm','balanced');
    const cancelCount=mutations.length;await click('[data-rl-listen-task=style]');await press('Escape','Escape',27);await until('!D("rlListeningDialog").open');
    checked('Escape cancels without a write',mutations.length===cancelCount);await check('Escape returns keyboard focus to card','document.activeElement.dataset.rlListenTask==="style"');
    await click(pro);await check('professional mode uses digital music platform label','document.documentElement.dataset.rlMode==="pro"&&D("rlHomeNavLabel").textContent==="数字音乐台"');
    await check('professional settings remain reachable','document.querySelector(".side .nav[data-v=settings]").checkVisibility()');await click(listen);
    await check('returning to listening keeps shared saved device','document.documentElement.dataset.rlMode==="listen"&&D("rlListeningDevice").selectedOptions[0].textContent==="Isolated fixture DAC"');
    for(const w of [1280,800,640]){
      await browser.send('Emulation.setDeviceMetricsOverride',{width:w,height:880,deviceScaleFactor:1,mobile:false});await click('[data-rl-listen-task=style]');
      await check('popup fits real app viewport '+w,'D("rlListeningDialog").getBoundingClientRect().left>=0&&D("rlListeningDialog").getBoundingClientRect().right<=innerWidth');await close();
      await check('listening choices fit main content '+w,'D("rlListeningControls").scrollWidth<=D("rlListeningControls").clientWidth+1');
    }
    await browser.send('Emulation.setDeviceMetricsOverride',{width:1280,height:880,deviceScaleFactor:1,mobile:false});
    facts.files=[fixtureTracks[0],fixtureTracks[0],fixtureTracks[0]];facts.cues=[[12.5,33],[12.5,33],[12.5,33]];await evaluate('loadQ()');await until('D("rlListenQueueList").querySelectorAll("[data-rl-index]").length===3');facts.nextExplicitPlay=true;await click('#rlListenQueueList [data-rl-index="2"]');
    for(let i=0;i<40&&!explicitPlays.length;i++)await delay(25);
    checked('queue preserves third identical CUE item and both bounds',explicitPlays.length===1&&explicitPlays[0].file===fixtureTracks[0]&&explicitPlays[0].queueIndex==='2'&&explicitPlays[0].cue_start==='12.5'&&explicitPlays[0].cue_end==='33');
    await shot('listening-home');await click('[data-rl-listen-task=style]');await shot('listening-style-card');await close();
    for(const lang of langs){facts.language=lang;await evaluate('i18nSetLang('+JSON.stringify(lang)+',false)');await check('mode labels still translated '+lang,'document.querySelector('+JSON.stringify(listen)+').textContent.trim()==='+JSON.stringify(dictionaries[lang]['聆听模式'])+'&&document.querySelector('+JSON.stringify(pro)+').textContent.trim()==='+JSON.stringify(dictionaries[lang]['专业模式']));await click('[data-rl-listen-task=output]');await check('output remains same-page and fits '+lang,'D("rlListeningDialog").open&&D("rlListeningDialog").getBoundingClientRect().right<=innerWidth&&S.view!=="settings"');await close();}
    checked('all explicit parameter changes preserve fractional listening gain',settings.volume==='-42.5');
    checked('no unintended settings, playback, queue or external actions',forbidden.length===0);
    checked('production renderer reports zero JavaScript exceptions',errors.length===0);
    console.log(JSON.stringify({checks:checks.length,failures:0,evidence:run}));
  }catch(error){try{await shot('failure')}catch{}console.error({scenario,errors,forbidden,evidence:run});throw error;}
  finally{fs.writeFileSync(path.join(run,'result.json'),JSON.stringify({checks,mutations,explicitPlays,forbidden,errors,lastScenario:scenario},null,2));await browser.close();}
}
runCapabilityMatrix();
if(process.argv.includes('--capabilities-only'))console.log(JSON.stringify({checks:checks.length,failures:0,evidence:run}));
else main().catch(error=>{console.error(error);process.exitCode=1;});
