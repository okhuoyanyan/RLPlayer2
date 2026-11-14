// Actual production HTML/JS in a real DOM; loopback settings boundary, no player/audio.
const fs=require('fs'),path=require('path'),http=require('http'),net=require('net'),cp=require('child_process'),crypto=require('crypto');
const args=process.argv.slice(2),arg=(n,d)=>{const i=args.indexOf(n);return i<0?d:args[i+1]},root=path.resolve(arg('--source',path.join(__dirname,'..')));
const suite=arg('--suite','quickstart');
const work=arg('--work-root',path.join(root,'build-ui332'));fs.mkdirSync(work,{recursive:true});const home=fs.mkdtempSync(path.join(work,'quickstart-'));
const cpp=path.join(home,'export-ui.cpp'),exe=path.join(home,'export-ui.exe'),htmlFile=path.join(home,'ui.html'),jsFile=path.join(home,'app.js');
fs.writeFileSync(cpp,'#include <fstream>\n#include "net/ui_page.h"\n#include "net/ui_script.h"\nint main(int n,char**v){if(n!=3)return 2;std::ofstream(v[1],std::ios::binary)<<rta::net::rta_ui_page();std::ofstream(v[2],std::ios::binary)<<rta::net::rta_ui_script();}\n');
const built=cp.spawnSync(arg('--cxx','C:/Program Files/AMD/ROCm/7.2/bin/clang++.exe'),['-std=c++23','-O0','-I'+path.join(root,'src'),cpp,'-o',exe],{windowsHide:true,encoding:'utf8'});if(built.status!==0)throw Error(built.stdout+built.stderr);
if(cp.spawnSync(exe,[htmlFile,jsFile],{windowsHide:true}).status!==0)throw Error('UI export failed');
const html=fs.readFileSync(htmlFile,'utf8').replaceAll('FILTER_OPTS_1X','<option value="auto">Auto</option>').replaceAll('FILTER_OPTS_NX','<option value="">Follow1x</option>').replaceAll('SHAPER_OPTS','<option value="auto">Auto</option>'),script=fs.readFileSync(jsFile);
const sleep=ms=>new Promise(r=>setTimeout(r,ms)),free=()=>new Promise(r=>{const s=net.createServer();s.listen(0,'127.0.0.1',()=>{let p=s.address().port;s.close(()=>r(p));});});
let scenario='default',settings={backend:'null',device:'null',volume:'-60',rate:'src',filter:'',filterNx:'',cleanup:'0',cleanup_auto:'1',cleanup_strength:'0.5',cleanup_noise_db:'-101.1',gpuvendor:'off',metaon:'0',metallm:'0',anlmaster:'0',autoanl:'0',roomcorrauto:'0'},delaySettings=0,failSave=false,delayCaps=0;
let signalFacts=null,castFacts=null,signalDelay=0;
let browser,ws,nextId=0,pending=new Map(),records=[],checks=[],exceptions=[],outside=[];
function check(ok,name,detail){checks.push({ok,name,detail});console.log((ok?'PASS ':'FAIL ')+name);}
const server=http.createServer((req,res)=>{const u=new URL(req.url,'http://127.0.0.1');let body='';req.on('data',b=>body+=b);req.on('end',()=>{
 records.push({scenario,path:u.pathname,method:req.method,query:u.search,body});res.setHeader('Cache-Control','no-store');const json=x=>{res.setHeader('Content-Type','application/json');res.end(JSON.stringify(x))};
 if(u.pathname==='/'){res.setHeader('Content-Type','text/html; charset=utf-8');res.end(html);return;}
 if(u.pathname==='/app.js'){res.setHeader('Content-Type','text/javascript; charset=utf-8');res.end(script);return;}
 if(u.pathname==='/api/settings'){
  if(req.method==='POST'){if(failSave){res.statusCode=503;json({ok:false,error:'Private fixture save failed'});return;}for(const [k,v] of new URLSearchParams(body))settings[k]=v;json({ok:true,merged:true});return;}
  const snapshot={ok:true,kv:{...settings}};if(delaySettings)setTimeout(()=>json(snapshot),delaySettings);else json(snapshot);return;
 }
 if(u.pathname==='/api/workbench/profiles'){json({ok:true,active:'A',slots:['A','B','C','D'].map(id=>({id,name:id,settings:{...settings},peq:{bypass:true,preamp:0,bands:[]},eqId:'',eqName:'',migrated:true})),loop:{enabled:false,start:0,end:0,file:''}});return;}
 if(u.pathname==='/api/workbench/eqpresets'){json({ok:true,slots:Array.from({length:16},(_,i)=>({id:String(i+1),name:'',source:'custom',peq:null,empty:true})),activeId:''});return;}
 if(u.pathname==='/api/workbench/profile'){
   const patch=Object.fromEntries(new URLSearchParams(body));
   if(req.method!=='POST'||patch.action!=='update'||patch.id!=='A'){res.statusCode=400;json({ok:false,error:'Unexpected profile mutation'});return;}
   if(failSave){res.statusCode=503;json({ok:false,error:'Private fixture save failed'});return;}
   for(const [k,v] of Object.entries(patch))if(!['action','id'].includes(k))settings[k]=v;
   json({ok:true});return;
 }
 if(u.pathname==='/api/i18n'){
   const requested=u.searchParams.get('lang'),lang=['en','ja','fr','ru','es','ar'].includes(requested)?requested:'zh';
   const map=lang==='zh'?{}:JSON.parse(fs.readFileSync(path.join(root,'data/i18n',lang+'.json'),'utf8'));
   json({ok:true,lang,map});return;
 }
 if(u.pathname==='/api/status'){json({ok:true,playing:false,current:'',dur:0,pos:0,volDb:-60,wf:0,wr:0,pulls:0,gpu:false});return;}
 if(u.pathname==='/api/queue'){json({ok:true,items:[],cues:[],index:0,repeat:0,mode:0});return;}
 if(u.pathname==='/api/workbench/state'){json({ok:true,settings:{...settings},savedSettings:{...settings},file:'',playing:false,paused:false,position:0,revision:'1',peq:{bypass:1,preamp:0,bands:[]},profile:'pcm',style:'custom'});return;}
 if(u.pathname==='/api/devices'){json({ok:true,devices:[{id:'fixture',name:'Private fixture DAC'}],asioDrivers:[]});return;}
 if(u.pathname==='/api/preset/caps'){const data={ok:true,device:{name:'Private fixture DAC',rates:[44100,176400],maxRate:176400,exclusive:true},notes:[]};if(delayCaps)setTimeout(()=>json(data),delayCaps);else json(data);return;}
 if(u.pathname==='/api/signal'){const snapshot={ok:!!signalFacts,path:signalFacts&&{...signalFacts}};if(signalDelay)setTimeout(()=>json(snapshot),signalDelay);else json(snapshot);return;}
 if(u.pathname==='/api/cast/state'){json(castFacts||{ok:true,running:false,phase:'idle'});return;}
 if(u.pathname==='/api/lib/stats'){json({ok:true,tracks:0,albums:0,scanning:false,roots:[]});return;}
 if(u.pathname==='/api/lib/albums'){json({ok:true,albums:[]});return;}
 if(u.pathname==='/api/filters'){json({ok:true,filters:[]});return;}
 if(u.pathname==='/api/rates'){json({ok:true,rates:[44100],sysRate:44100});return;}
 if(u.pathname==='/api/providers'){json({ok:true,items:[]});return;}
 if(u.pathname==='/favicon.ico'){res.statusCode=204;res.end();return;}
 json({ok:true,items:[],albums:[],roots:[],tracks:[],configured:false});
})});
function send(method,params={}){return new Promise((resolve,reject)=>{const id=++nextId;pending.set(id,{resolve,reject});ws.send(JSON.stringify({id,method,params}));});}
async function ev(expression){const j=await send('Runtime.evaluate',{expression,awaitPromise:true,returnByValue:true});if(j.exceptionDetails)throw Error(JSON.stringify(j.exceptionDetails));return j.result.value;}
async function until(expression,ms=7000){for(let n=0;n<ms/50;n++){if(await ev('!!('+expression+')'))return true;await sleep(50);}return false;}
async function processingIdle(){if(!await until('!RL_STUDIO.pending&&!rlCompare.saving&&!rlWorkbenchQueue.pending&&!Object.keys(RL_STUDIO.draft).length&&!Object.keys(RL_STUDIO.inflight).length'))throw Error('Automatic professional save did not finish');}
(async()=>{let base;try{
 await new Promise(r=>server.listen(0,'127.0.0.1',r));base='http://127.0.0.1:'+server.address().port;
 const edge=['C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe','C:/Program Files/Microsoft/Edge/Application/msedge.exe'].find(fs.existsSync);if(!edge)throw Error('Edge unavailable');const port=await free();
 browser=cp.spawn(edge,['--headless=new','--disable-gpu','--disable-background-networking','--no-first-run','--user-data-dir='+path.join(home,'edge-profile'),'--remote-debugging-port='+port,'--window-size=1400,1000','about:blank'],{windowsHide:true,stdio:'ignore'});
 let targets;for(let n=0;n<100;n++){try{targets=await(await fetch('http://127.0.0.1:'+port+'/json/list')).json();if(targets.some(x=>x.type==='page'))break;}catch{}await sleep(100);}
 ws=new WebSocket(targets.find(x=>x.type==='page').webSocketDebuggerUrl);await new Promise((resolve,reject)=>{ws.onopen=resolve;ws.onerror=reject;});
 ws.onmessage=e=>{const m=JSON.parse(e.data);if(m.id&&pending.has(m.id)){const p=pending.get(m.id);pending.delete(m.id);m.error?p.reject(Error(JSON.stringify(m.error))):p.resolve(m.result);}else if(m.method==='Runtime.exceptionThrown')exceptions.push(m.params.exceptionDetails);else if(m.method==='Fetch.requestPaused'){if(m.params.request.url.startsWith(base+'/'))send('Fetch.continueRequest',{requestId:m.params.requestId}).catch(()=>{});else{outside.push(m.params.request.url);send('Fetch.failRequest',{requestId:m.params.requestId,errorReason:'BlockedByClient'}).catch(()=>{});}}};
 await send('Runtime.enable');await send('Page.enable');await send('Fetch.enable',{patterns:[{urlPattern:'*'}]});
 await send('Page.addScriptToEvaluateOnNewDocument',{source:"localStorage.setItem('rta-quickstart-seen','1');window.processingField=function(key){var dialog=document.getElementById('rlStudioAdvancedDialog');return dialog&&dialog.querySelector('[data-studio-key=\"'+key+'\"]')}"});
 await send('Page.navigate',{url:base+'/'});if(!await until('typeof PLAYBACK_SETTINGS!=="undefined"&&PLAYBACK_SETTINGS.ready'))throw Error('Production settings did not load');await sleep(350);
 if(suite==='quickstart'){
 check(await ev("S.view==='listening'&&!!D('rlListeningControls')"),'merged listening home is the default landing page');
 check(await ev("!D('navQuickStart')&&!D('rlQuickSetup')&&!D('qsTitle')"),'only one home entry and no second quick-start section remain');
 check(await ev("document.querySelector('.side .nav[data-v=settings]').hidden"),'listening mode excludes professional settings from navigation');
 check(await ev("S.vol===-60"),'merged home restores exact persisted volume');
 for(const value of ['0','1']){
  scenario='legacy-'+value;settings.quickstart=value;
  await send('Page.navigate',{url:base+'/?case=legacy'+value});await until('typeof PLAYBACK_SETTINGS!=="undefined"&&PLAYBACK_SETTINGS.ready&&!!D("rlListeningControls")&&location.search==="?case=legacy'+value+'"');await sleep(150);
  check(await ev("S.view==='listening'&&!D('navQuickStart')"),'legacy quick-start '+value+' migrates to the merged home');
  check(!records.filter(r=>r.scenario===scenario).some(r=>r.method==='POST'&&['/api/settings','/api/workbench/apply','/api/workbench/profile','/api/workbench/eqpresets','/api/peq'].includes(r.path)),'landing-page migration has zero configuration writes');
 }
 await ev("setView('station')");
 check(await ev("S.view==='listening'&&!!D('rlListeningControls')"),'legacy station links resolve to the merged home');
 await ev("rlSetMode('pro',true)");
 check(await ev("!!D('rlStudio')&&D('rlStudioHeading').textContent==='数字音乐台'&&!document.querySelector('.studioMechanism')"),'professional home is the digital console without a vinyl deck');
 check(await ev("!document.querySelector('.side .nav[data-v=settings]').hidden"),'professional settings have a visible entry');
 await ev("openSettingsTab('adv')");
 check(await ev("S.view==='settings'&&!D('sp_quickstart')"),'superseded quick-start preference is removed from advanced settings');
 await ev("rlSetMode('listen',true)");
 check(await ev("S.view==='listening'&&!D('setpage').classList.contains('on')"),'switching to listening exits the professional settings surface');
 scenario='delayed-home';delaySettings=1000;
 await send('Page.navigate',{url:base+'/?case=delayed-home'});await until('typeof PLAYBACK_SETTINGS!=="undefined"&&location.search==="?case=delayed-home"');
 await ev("setView('tracks')");await until('typeof PLAYBACK_SETTINGS!=="undefined"&&PLAYBACK_SETTINGS.ready');await sleep(100);
 check(await ev("S.view==='tracks'"),'late saved settings cannot steal an explicitly selected view');
 check(await ev("S.vol===-60"),'late settings preserve the safe restored gain');
 delaySettings=0;
 }else if(suite==='cleanup'){
 await ev("openSettingsTab('pcm')");if(!await until('D("rlStudioAdvancedDialog").open&&rlCompare.activeId==="A"'))throw Error('New professional PCM card did not open');
 const dialogGeometry=[];
 for(const [width,height] of [[1340,900],[768,880],[320,680]]){
  await send('Emulation.setDeviceMetricsOverride',{width,height,deviceScaleFactor:1,mobile:false});
  await ev("openSettingsTab('pcm')");
  const g=await ev('(()=>{const d=D("rlStudioAdvancedDialog"),r=d.getBoundingClientRect(),b=d.querySelector("header button").getBoundingClientRect();return {left:r.left,right:r.right,top:r.top,bottom:r.bottom,width:r.width,height:r.height,scrollHeight:d.scrollHeight,clientHeight:d.clientHeight,viewportWidth:innerWidth,viewportHeight:innerHeight,closeX:b.x+b.width/2,closeY:b.y+b.height/2}})()');
  dialogGeometry.push(g);
  check(g.left>=17&&g.right<=width-17&&g.top>=17&&g.bottom<=height-17&&Math.abs((g.left+g.right)/2-width/2)<=1&&Math.abs((g.top+g.bottom)/2-height/2)<=1,'PCM advanced card is centered with safe viewport margins '+width,g);
  if(width===320)check(g.scrollHeight>g.clientHeight,'narrow PCM advanced card scrolls inside its safe viewport',g);
  const image=await send('Page.captureScreenshot',{format:'png',captureBeyondViewport:false});fs.writeFileSync(path.join(home,'pcm-advanced-'+width+'.png'),Buffer.from(image.data,'base64'));
  await send('Input.dispatchMouseEvent',{type:'mousePressed',x:g.closeX,y:g.closeY,button:'left',clickCount:1});await send('Input.dispatchMouseEvent',{type:'mouseReleased',x:g.closeX,y:g.closeY,button:'left',clickCount:1});
  check(await until('!D("rlStudioAdvancedDialog")'),'real close button dismisses PCM advanced card '+width);
 }
 fs.writeFileSync(path.join(home,'pcm-advanced-geometry.json'),JSON.stringify(dialogGeometry,null,2));
 await send('Emulation.setDeviceMetricsOverride',{width:1400,height:1000,deviceScaleFactor:1,mobile:false});await ev("openSettingsTab('pcm')");
 check(await ev("!!processingField('cleanup_auto')&&processingField('cleanup_auto').checked&&processingField('cleanup_noise_db').disabled&&Number(processingField('cleanup_noise_db').value)===-101.1&&ADV.cleanup===0"),'cleanup defaults off with Auto selected and preserved, noneditable manual floor');
 check(await ev("!!D('rlStudioCleanupStatus')&&D('rlStudioCleanupStatus').textContent.includes('已关闭')&&!D('rlStudioCleanupStatus').textContent.includes('-101.1')"),'disabled cleanup status never calls the stored manual floor a measurement');
 await ev("processingField('cleanup_auto').checked=false;processingField('cleanup_auto').dispatchEvent(new Event('change',{bubbles:true}))");await processingIdle();
 check(settings.cleanup_auto==='0'&&await ev("!processingField('cleanup_noise_db').disabled&&ADV.cleanupAuto===0"),'Manual mode persists and enables the retained floor editor');
 await ev("processingField('cleanup_noise_db').value='-95';processingField('cleanup_noise_db').dispatchEvent(new Event('change',{bubbles:true}))");await processingIdle();
 check(settings.cleanup_noise_db==='-95'&&await ev('ADV.cleanupNoiseDb===-95'),'Manual noise floor is saved without an automatic estimate');
 await ev("processingField('cleanup_auto').checked=true;processingField('cleanup_auto').dispatchEvent(new Event('change',{bubbles:true}))");await processingIdle();
 check(settings.cleanup_auto==='1'&&settings.cleanup_noise_db==='-95'&&await ev("processingField('cleanup_noise_db').disabled&&Number(processingField('cleanup_noise_db').value)===-95"),'returning to Auto retains the manual value instead of overwriting it');
 await ev("processingField('cleanup').click()");await processingIdle();
 check(settings.cleanup==='1'&&await ev('ADV.cleanup===1'),'cleanup enable remains a separate persisted master switch');
 if(!await ev("typeof rlStudioRefreshCleanupStatus==='function'"))check(false,'production cleanup status has a real read-only refresh entry');
 else {
 signalFacts={src:'owned.wav',bitPerfect:'no',bitPerfectWhy:'fixture',inRate:44100,inBits:16,inCh:2,outRate:44100,outBits:16,volume:-60,buffer:100,cleanupRequested:true,cleanupAuto:true,cleanupAutoState:'learning',cleanupAutoEstimateValid:false,cleanupEstimatedNoiseFloorDbfs:null,cleanupAppliedNoiseFloorDbfs:null};
 await ev('rlStudioRefreshCleanupStatus()');
 check(await ev("D('rlStudioCleanupStatus').textContent.includes('等待可信安静')&&!D('rlStudioCleanupStatus').textContent.includes('-95')&&!D('rlStudioCleanupStatus').textContent.includes('-101.1')"),'learning reports a quiet-passage wait and never fabricates a measured floor');
 signalFacts={...signalFacts,cleanupAutoState:'insufficient',cleanupEstimatedNoiseFloorDbfs:-144};await ev('rlStudioRefreshCleanupStatus()');
 check(await ev("D('rlStudioCleanupStatus').textContent.includes('证据不足')&&!D('rlStudioCleanupStatus').textContent.includes('-144')&&!D('rlStudioCleanupStatus').textContent.includes('0.0 dBFS')"),'invalid estimates and null applied floors do not become silence or zero-dB measurements');
 signalFacts={...signalFacts,cleanupAutoState:'estimated',cleanupAutoEstimateValid:true,cleanupEstimatedNoiseFloorDbfs:-92.75,cleanupAppliedNoiseFloorDbfs:-92.75};await ev('rlStudioRefreshCleanupStatus()');
 check(await ev("D('rlStudioCleanupStatus').textContent.includes('-92.8')&&D('rlStudioCleanupStatus').textContent.includes('上次')"),'a valid stopped-session estimate is explicitly shown as the last processing report');
 check(await ev("ADV.cleanupNoiseDb===-95&&Number(processingField('cleanup_noise_db').value)===-95"),'reported estimates never overwrite the saved manual floor');
 await ev('showChain()');await until("!!D('mbody').querySelector('.cnode .k')");
 check(await ev("D('mbody').querySelector('.cnode .k').textContent.includes('bit-perfect')"),'signal chain keeps its authoritative bit-perfect conclusion first');
 check(await ev("D('mbody').textContent.includes('-92.8')&&D('mbody').textContent.includes('录音噪声清理')"),'existing signal dialog displays the actual reported cleanup estimate');
 const requestsBeforeIdle=records.filter(r=>r.path==='/api/signal').length;await sleep(1250);
 check(records.filter(r=>r.path==='/api/signal').length===requestsBeforeIdle,'status display creates no per-tick requests or background audio scans');
 await ev("processingField('cleanup_auto').checked=false;processingField('cleanup_auto').dispatchEvent(new Event('change',{bubbles:true}))");await processingIdle();await ev('rlStudioRefreshCleanupStatus()');
 check(await ev("D('rlStudioCleanupStatus').textContent.includes('下次播放')&&!D('rlStudioCleanupStatus').textContent.includes('-92.8')"),'mode mismatch is marked for the next playback instead of presenting old estimates as current');
 signalFacts={...signalFacts,cleanupAuto:false,cleanupAutoState:'manual',cleanupNoiseDb:-95,cleanupAppliedNoiseFloorDbfs:-144};await ev('rlStudioRefreshCleanupStatus()');
 check(await ev("!D('rlStudioCleanupStatus').textContent.includes('-144')&&D('rlStudioCleanupStatus').textContent.includes('尚未测量')"),'out-of-range manual snapshot placeholders are not presented as applied measurements');
 signalFacts={...signalFacts,cleanupAppliedNoiseFloorDbfs:-95};await ev('rlStudioRefreshCleanupStatus()');
 check(await ev("D('rlStudioCleanupStatus').textContent.includes('-95.0')&&D('rlStudioCleanupStatus').textContent.includes('手动噪声底')"),'a valid backend Manual applied floor is displayed as a reported value');
 await ev("processingField('cleanup_auto').checked=true;processingField('cleanup_auto').dispatchEvent(new Event('change',{bubbles:true}))");await processingIdle();
 signalFacts={...signalFacts,cleanupAuto:true,cleanupAutoState:'estimated'};
 signalDelay=900;signalFacts={...signalFacts,cleanupEstimatedNoiseFloorDbfs:-80,cleanupAppliedNoiseFloorDbfs:-80};await ev('void rlStudioRefreshCleanupStatus()');
 castFacts={...signalFacts,cleanupEstimatedNoiseFloorDbfs:-91.25,cleanupAppliedNoiseFloorDbfs:-91.25,ok:true,running:false,phase:'idle'};
 settings.backend='upnp';await ev("rlConsole.refresh(true).then(()=>rlStudioRefreshCleanupStatus())");await sleep(1100);
 check(await ev("D('rlStudioCleanupStatus').textContent.includes('-91.3')&&!D('rlStudioCleanupStatus').textContent.includes('-80.0')"),'cast root fields are used and an older local response cannot replace them');
 check(records.some(r=>r.path==='/api/cast/state'),'UPnP cleanup reads only its own existing state endpoint');
 signalDelay=0;settings.backend='null';await ev("rlConsole.refresh(true).then(()=>setView('albums'))");await sleep(50);signalFacts={...signalFacts,cleanupEstimatedNoiseFloorDbfs:-88,cleanupAppliedNoiseFloorDbfs:-88};signalDelay=700;
 await ev("openSettingsTab('pcm');setView('tracks')");await sleep(850);
 check(await ev("S.view==='tracks'"),'a late cleanup status response never changes the active view');
 signalDelay=0;
 for(const lang of ['en','ja','fr','ru','es','ar']){
  const map=JSON.parse(fs.readFileSync(path.join(root,'data/i18n',lang+'.json'),'utf8'));
  // Use the shipped switch so existing controls are translated in place.
  await ev('i18nSetLang('+JSON.stringify(lang)+',false)');await ev('openSettingsTab("pcm")');
  check(await ev('processingField("cleanup_auto").closest("label").textContent.includes('+JSON.stringify(map['自动估计噪声底'])+')&&processingField("cleanup_noise_db").parentElement.textContent.includes('+JSON.stringify(map['手动噪声底 · dBFS'])+')'),lang+' actual cleanup mode and manual-value labels are translated');
  await ev('rlStudioRefreshCleanupStatus()');
  check(await ev('D("rlStudioCleanupStatus").textContent.includes('+JSON.stringify(map['自动估计噪声底：{0} dBFS'].replace('{0}','-88.0'))+')&&D("rlStudioCleanupStatus").textContent.includes('+JSON.stringify(map['上次处理：{0}'].split('{0}')[0])+')'),lang+' actual reported estimate uses translated templates');
  await ev('openSettingsTab("adv")');
  check(await ev('!!D("sp_uilang")&&!D("sp_quickstart")'),lang+' advanced language selection remains while obsolete home toggle is absent');
 }
 // A form first built in another language must keep its source labels. Both
 // forms must survive a language change while detached, including unsaved text.
 await send('Page.navigate',{url:base+'/?case=cold-language'});
 if(!await until('location.search==="?case=cold-language" && window.PLAYBACK_SETTINGS && PLAYBACK_SETTINGS.ready && window.I18N && I18N.observer'))throw Error('Cold document initialization failed');
 const words=lang=>JSON.parse(fs.readFileSync(path.join(root,'data/i18n',lang+'.json'),'utf8'));
 const cleanupLabels=lang=>'processingField("cleanup_auto").closest("label").textContent.includes('+JSON.stringify(words(lang)['自动估计噪声底'])+') && processingField("cleanup_noise_db").parentElement.textContent.includes('+JSON.stringify(words(lang)['手动噪声底 · dBFS'])+')';
 await ev('i18nSetLang("en",false)');await ev("openSettingsTab('diag');D('sp_fbtitle').value='Unsent English draft';openSettingsTab('pcm')");
 check(await ev(cleanupLabels('en')),'first expert render in English matches real processing labels');
 await ev('i18nSetLang("fr",false)');
 check(await ev(cleanupLabels('fr')),'attached expert controls update to French in place');
 await ev("rlSetMode('listen',false)");
 check(await ev("S.view==='listening'&&!D('setpage').classList.contains('on')"),'listening mode returns to its merged home after a language change');
 await ev('i18nSetLang("ja",false)');await ev('openSettingsTab("pcm")');
 check(await ev(cleanupLabels('ja')),'hidden expert form updates to Japanese on return');
 await ev('openSettingsTab("diag")');
 check(await ev("D('sp_fbtitle').value==='Unsent English draft'"),'unsent expert feedback survives mode and language changes');

 }
 }else throw Error('Unknown suite '+suite);
 check(!records.some(r=>/^\/api\/(?:play|lib\/play|sq\/play|skip|pause|seek|cast\/start|input\/start|roomcorr\/measure|volume|preset\/apply)(?:\?|$)/.test(r.path)),'no playback, device, volume, or preset action is sent');
 check(!records.some(r=>r.path==='/api/queue/set'||r.path==='/api/queue/clear'),'preference never writes the queue');
 check(records.filter(r=>r.path==='/api/workbench/profile'&&r.method==='POST').every(r=>suite==='cleanup'&&[...new URLSearchParams(r.body).keys()].every(k=>['action','id','cleanup','cleanup_auto','cleanup_strength','cleanup_noise_db'].includes(k))),'automatic cleanup writes contain only the active-group cleanup parameters');
 check(exceptions.length===0,'all production DOM functions run without JavaScript exceptions',exceptions);
 check(outside.length===0,'all requests remain on the owned loopback fixture',outside);
 const sourceHashes=Object.fromEntries(['src/net/ui_script.h','src/net/ui_page.h','src/net/ui_studio.h','src/net/ui_workbench_state.h','src/net/ui_listening_home.h'].map(n=>[n,crypto.createHash('sha256').update(fs.readFileSync(path.join(root,n))).digest('hex')]));
 const result={home,suite,sourceHashes,checks,exceptions,outside,records,fullProductionDom:true,playerStarted:false};fs.writeFileSync(path.join(home,'results.json'),JSON.stringify(result,null,2));console.log(JSON.stringify({home,checks:checks.length,failures:checks.filter(x=>!x.ok).length}));if(checks.some(x=>!x.ok))process.exitCode=1;
 }finally{if(ws&&ws.readyState===1){send('Browser.close').catch(()=>{});await sleep(300);ws.close();}server.closeIdleConnections();await new Promise(r=>server.close(r));}
})().catch(e=>{console.error(e);process.exitCode=1});
