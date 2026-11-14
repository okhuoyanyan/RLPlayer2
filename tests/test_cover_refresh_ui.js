// Full production HTML/JS, exported offline from the UI headers. No player/audio process.
// A loopback-only mock serves owned cache responses; it never contacts cover providers.
const fs=require('fs'),path=require('path'),http=require('http'),net=require('net'),cp=require('child_process');
const argv=process.argv.slice(2),arg=(n,d)=>{const i=argv.indexOf(n);return i<0?d:argv[i+1]},root=path.resolve(arg('--source',path.join(__dirname,'..')));
const workRoot=arg('--work-root',path.join(root,'build-cover-ui'));fs.mkdirSync(workRoot,{recursive:true});const home=fs.mkdtempSync(path.join(workRoot,'refresh-'));
const compiler=arg('--cxx',process.env.CXX||'C:/Program Files/AMD/ROCm/7.2/bin/clang++.exe');
const cpp=path.join(home,'export-ui.cpp'),exe=path.join(home,'export-ui.exe'),htmlFile=path.join(home,'ui.html'),jsFile=path.join(home,'app.js');
fs.writeFileSync(cpp,'#include <fstream>\n#include "net/ui_page.h"\n#include "net/ui_script.h"\nint main(int n,char**v){if(n!=3)return 2;std::ofstream(v[1],std::ios::binary)<<rta::net::rta_ui_page();std::ofstream(v[2],std::ios::binary)<<rta::net::rta_ui_script();}\n');
const build=cp.spawnSync(compiler,['-std=c++23','-O0','-I'+path.join(root,'src'),cpp,'-o',exe],{windowsHide:true,encoding:'utf8'});if(build.status!==0)throw Error('Offline UI export compile failed: '+build.stdout+build.stderr);
const exportUi=cp.spawnSync(exe,[htmlFile,jsFile],{windowsHide:true,encoding:'utf8'});if(exportUi.status!==0)throw Error('Offline UI export failed');
const html=fs.readFileSync(htmlFile,'utf8').replaceAll('FILTER_OPTS_1X','<option value="auto">Auto</option>').replaceAll('FILTER_OPTS_NX','<option value="">Follow1x</option><option value="auto">Auto</option>').replaceAll('SHAPER_OPTS','<option value="none">none</option>');
const script=fs.readFileSync(jsFile),png=Buffer.from('iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+aN1kAAAAASUVORK5CYII=','base64');
const sleep=ms=>new Promise(r=>setTimeout(r,ms)),free=()=>new Promise(r=>{const s=net.createServer();s.listen(0,'127.0.0.1',()=>{const p=s.address().port;s.close(()=>r(p));});});
let current,records=[],browser,ws,id=0,pending=new Map(),exceptions=[],outside=[],checks=[];
function check(ok,name,detail){checks.push({ok,name,detail});console.log((ok?'PASS ':'FAIL ')+name)}
const albumKey='0123456789abcdef',groupKey='fedcba9876543210',trackPath='D:/Owned album/01. track.wav';
function albums(){return [{k:albumKey,name:'Owned album',artist:'Artist',cv:current.available},{k:groupKey,name:'Directory collection',artist:'Mixed artists',cv:false,albumIdentitySource:'directory_inferred'}]}
function fixtureSettings(){return {backend:'null',device:'null',volume:'-60',rate:'src',quickstart:'0',gpuvendor:'off',metaon:current.name.includes('meta-off')?'0':'1',metallm:'0',metasummary:'0',autoanl:'0',anlmaster:'0',roomcorrauto:'0',uilang:'zh'}}
const server=http.createServer((req,res)=>{const u=new URL(req.url,'http://127.0.0.1');let body='';req.on('data',b=>body+=b);req.on('end',()=>{
 records.push({scenario:current.name,path:u.pathname,query:u.search,method:req.method,at:Date.now(),body});res.setHeader('Cache-Control','no-store');
 const json=x=>{res.setHeader('Content-Type','application/json');res.end(JSON.stringify(x))};
 const complete=()=>{if(current.name!=='negative'&&!current.timer){const owner=current;owner.timer=setTimeout(()=>{owner.available=true;owner.completedAt=Date.now()},900)}};
 if(u.pathname==='/'){res.setHeader('Content-Type','text/html; charset=utf-8');res.end(html);return}
 if(u.pathname==='/app.js'){res.setHeader('Content-Type','text/javascript; charset=utf-8');res.end(script);return}
 if(u.pathname==='/api/settings'){json({ok:true,kv:fixtureSettings()});return}
 if(u.pathname==='/api/workbench/state'){json({ok:true,settings:fixtureSettings(),profile:'bitperfect',style:'custom',revision:'cover-fixture',playing:false,paused:false,file:(current.name.startsWith('np')||current.name==='negative')?(current.file||trackPath):'',position:0,duration:0,queueIndex:0,cueStart:-1,cueEnd:-1,peq:{bypass:true,preamp:0,bands:[]}});return}
 if(u.pathname==='/api/i18n'){if(current.name==='warm'){const owner=current;setTimeout(()=>{owner.i18nSent=true;json({ok:true,lang:'zh',map:{}})},500)}else json({ok:true,lang:'zh',map:{}});return}
 if(u.pathname==='/api/status'){json({ok:true,playing:false,paused:current.name==='np-late-meta-off',current:(current.name.startsWith('np')||current.name==='negative')?(current.file||trackPath):'',dur:0,pos:0,volDb:-60,wf:0,pulls:0,gpu:false,hasLlmKey:false});return}
 if(u.pathname==='/api/queue'){json({ok:true,items:[],cues:[],index:0,repeat:0,mode:0});return}
 if(u.pathname==='/api/lib/stats'){json({ok:true,tracks:1,albums:2,scanning:false,roots:['D:/Fixture']});return}
 if(u.pathname==='/api/lib/albums'){if(current.name==='warm')complete();const snapshot={ok:true,albums:albums()};if(current.delayNext){current.delayNext=false;current.delayed=true;setTimeout(()=>json(snapshot),1000)}else json(snapshot);return}
 if(u.pathname==='/api/lib/album'){json({ok:true,k:albumKey,name:'Owned album',artist:'Artist',cv:current.available,tracks:[{path:trackPath,title:'Track',artist:'Artist',dur:60,sr:44100,bits:16,ch:2}]});return}
 if(u.pathname==='/api/lib/cover/prefetch'){if(current.name==='web')complete();json({ok:true,queued:current.name==='web'?1:0,running:current.name==='web'&&!current.available});return}
 if(u.pathname==='/api/lib/cover/progress'){json({ok:true,running:current.name==='web'&&!current.available,done:current.available?1:0,total:1,hit:current.name==='web'&&current.available?1:0});return}
 if(u.pathname==='/api/lib/cover'){if(u.searchParams.get('k')===albumKey&&current.available){res.setHeader('Content-Type','image/png');res.end(png)}else{res.statusCode=404;res.end('No owned album cover')}return}
 if(u.pathname==='/api/cover'){if(current.name==='track-meta-off'&&!current.available)complete();if(current.available){res.setHeader('Content-Type','image/png');if(current.name==='np-late-meta-off'){current.heldImage=true;setTimeout(()=>res.end(png),1200)}else res.end(png)}else{res.statusCode=404;res.end('No owned track cover')}return}
 if(u.pathname==='/api/cover/lookup'){complete();json(current.available?{ok:true,hasCover:true,coverUrl:'/api/cover?file='+encodeURIComponent(trackPath)+'&v=2'}:{ok:true,hasCover:false,pending:false});return}
 if(u.pathname==='/api/meta'){if(current.name.startsWith('np'))complete();const file=u.searchParams.get('file');json({ok:true,path:file,title:'Track',hasCover:current.available,coverUrl:'/api/cover?file='+encodeURIComponent(file)+'&v=2'});return}
 if(u.pathname==='/api/filters'){json({ok:true,filters:[]});return}
 if(u.pathname==='/api/rates'){json({ok:true,rates:[44100],sysRate:44100});return}
 if(u.pathname==='/api/providers'){json({ok:true,items:[]});return}
 if(u.pathname==='/favicon.ico'){res.statusCode=204;res.end();return}
 json({ok:true,items:[],albums:[],tracks:[],roots:[],configured:false});
 })});
function send(method,params={}){return new Promise((res,rej)=>{const i=++id;pending.set(i,{res,rej});ws.send(JSON.stringify({id:i,method,params}));});}
async function ev(expression){const j=await send('Runtime.evaluate',{expression,awaitPromise:true,returnByValue:true});if(j.exceptionDetails)throw Error(JSON.stringify(j.exceptionDetails));return j.result.value;}
async function until(expression,ms=6000){for(let n=0;n<ms/50;n++){if(await ev('!!('+expression+')'))return true;await sleep(50)}return false}
(async()=>{let base;try{
 await new Promise(r=>server.listen(0,'127.0.0.1',r));base='http://127.0.0.1:'+server.address().port;
 const edge=['C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe','C:/Program Files/Microsoft/Edge/Application/msedge.exe'].find(fs.existsSync);if(!edge)throw Error('Edge unavailable');const dp=await free();
 browser=cp.spawn(edge,['--headless=new','--disable-gpu','--disable-background-networking','--no-first-run','--user-data-dir='+path.join(home,'edge-profile'),'--remote-debugging-port='+dp,'--window-size=1400,1000','about:blank'],{windowsHide:true,stdio:'ignore'});
 let targets;for(let i=0;i<100;i++){try{targets=await(await fetch('http://127.0.0.1:'+dp+'/json/list')).json();if(targets.some(t=>t.type==='page'))break}catch{}await sleep(100)}
 ws=new WebSocket(targets.find(t=>t.type==='page').webSocketDebuggerUrl);await new Promise((r,j)=>{ws.onopen=r;ws.onerror=j});ws.onmessage=e=>{const m=JSON.parse(e.data);if(m.id&&pending.has(m.id)){const p=pending.get(m.id);pending.delete(m.id);m.error?p.rej(Error(JSON.stringify(m.error))):p.res(m.result)}else if(m.method==='Runtime.exceptionThrown')exceptions.push(m.params.exceptionDetails);else if(m.method==='Fetch.requestPaused'){const p=m.params;if(p.request.url.startsWith(base+'/'))send('Fetch.continueRequest',{requestId:p.requestId});else{outside.push(p.request.url);send('Fetch.fulfillRequest',{requestId:p.requestId,responseCode:403,body:''})}}};
 await send('Runtime.enable');await send('Page.enable');await send('Fetch.enable',{patterns:[{urlPattern:'*',requestStage:'Request'}]});await send('Page.addScriptToEvaluateOnNewDocument',{source:"localStorage.setItem('rta-quickstart-seen','1');"});
 for(const name of ['web','warm','track','track-meta-off','np-meta-off','np-late-meta-off','negative']){
  current={name,available:false,timer:null};await send('Page.navigate',{url:base+'/?scenario='+name});
   if(!await until("window.S&&PLAYBACK_SETTINGS.ready&&location.search==='?scenario="+name+"'&&document.querySelector('.side .nav[data-v=albums]')"))throw Error('Full production home failed '+name);
   const prefetchBeforeNavigation=records.filter(r=>r.scenario===name&&r.path==='/api/lib/cover/prefetch').length;
   // The merged listening home is the default landing page. Enter albums through
   // its ordinary navigation before measuring the album artwork observer.
   await ev("document.querySelector('.side .nav[data-v=albums]').click()");
   if(!await until("S.view==='albums'&&document.querySelectorAll('#list .card').length===2"))throw Error('Full production albums failed '+name);
   for(let n=0;n<40&&records.filter(r=>r.scenario===name&&r.path==='/api/lib/cover/prefetch').length<prefetchBeforeNavigation+1;n++)await sleep(25);
   const prefetchAtEntry=records.filter(r=>r.scenario===name&&r.path==='/api/lib/cover/prefetch').length;
   check(prefetchAtEntry===prefetchBeforeNavigation+1,name+' explicit album entry starts exactly one prefetch',{before:prefetchBeforeNavigation,after:prefetchAtEntry});
  if(name.startsWith('np')){
   if(!await until('S.curMeta&&S.lastCur'))throw Error('NowPlaying initial metadata failed');await ev('openNowPlaying()');
   const metaCount=records.filter(r=>r.scenario===name&&r.path==='/api/meta').length;
   if(name==='np-late-meta-off'){
    for(let n=0;n<100&&!current.heldImage;n++)await sleep(50);check(current.heldImage,'paused NowPlaying issues a real cache-only image request');
    current.file='D:/Other owned album/02. next.wav';current.available=false;
    if(!await until("S.lastCur==='D:/Other owned album/02. next.wav'&&S.metaFile===S.lastCur"))throw Error('Status-driven song switch did not arrive');await sleep(1400);
    check(await ev("D('cv').style.backgroundImage===''&&D('npCover').getAttribute('src').startsWith('data:')"),'late decoded image cannot replace the newly selected song placeholder');
    check(records.filter(r=>r.scenario===name&&r.path==='/api/meta').length===metaCount+1,'song switch reads new metadata once without refresh-driven full metadata calls');
    await ev('closeNowPlaying()');check(await ev('S.coverCacheWatch===null'),'closing the current-song page cancels its cache probe');
   }else{
    const appeared=await until("D('npCover').naturalWidth===1&&D('cv').style.backgroundImage.includes('/api/cover?')",4500);
    check(current.available,'NowPlaying local cache really becomes positive after initial missing metadata');
    check(appeared,'stopped NowPlaying with online metadata disabled naturally displays later cached artwork');
    check(records.filter(r=>r.scenario===name&&r.path==='/api/meta').length===metaCount,'NowPlaying cache refresh never repeats full metadata or LLM-capable request');
   }
   check(records.filter(r=>r.scenario===name&&r.path==='/api/cover/lookup').length===0,'NowPlaying cache refresh never invokes online lookup');
  }else if(name==='web'||name==='warm'){
   await ev("window.initialCoverNode=document.querySelector('#list img[data-album-key=\""+albumKey+"\"]');window.initialAlbums=S.albums;D('q').value='retained filter';D('list').style.height='150px';D('list').style.overflow='auto';var spacer=document.createElement('div');spacer.style.height='1200px';D('list').appendChild(spacer);D('list').scrollTop=47;window.initialScroll=D('list').scrollTop");
   if(name==='warm'){for(let n=0;n<40&&!current.i18nSent;n++)await sleep(25);await sleep(200);check(await ev("COVP.cacheTimer!==0&&S.view==='albums'"),'delayed language initialization preserves the same album cache observer');}
   const appeared=await until("document.querySelector('#list img[data-album-key=\""+albumKey+"\"]').naturalWidth===1",4500);
   check(current.available,name+' backend completion really made owned album cover available');
   check(appeared,name+' same-page background completion naturally paints the owned image');
   check(await ev("S.view==='albums'&&document.querySelector('#list img[data-album-key=\""+groupKey+"\"]').getAttribute('src')===null"),name+' unrelated directory group never borrows artwork');
   check(await ev("document.querySelector('#list img[data-album-key=\""+albumKey+"\"]')===initialCoverNode&&S.albums===initialAlbums&&D('q').value==='retained filter'&&D('list').scrollTop===initialScroll"),name+' completion preserves DOM scroll filter and album state');
   if(name==='warm'){
    current.delayNext=true;await ev("initialCoverNode.removeAttribute('src');refreshCoverImages(true)");for(let n=0;n<40&&!current.delayed;n++)await sleep(25);
     check(current.delayed,'late completion fixture holds a real HTTP album snapshot');await ev("setView('artists')");await sleep(1200);
     check(await ev("initialCoverNode.getAttribute('src')===null&&S.view==='artists'&&COVP.cacheTimer===0"),'late album snapshot cannot revive an image or timer after leaving its view');
   }
  }else{
   await ev("document.querySelector('#list .card[data-i=\"0\"]').click()");if(!await until("S.view==='album'&&document.querySelector('img[data-song-index]')&&document.querySelector('img[data-song-index]').songArtwork"))throw Error('Full album page failed '+name);
   await ev("window.initialCoverNode=document.querySelector('img[data-song-index]')");
   const appeared=await until("document.querySelector('img[data-song-index]').naturalWidth===1",4500);
   if(name==='negative')check(!appeared,'permanent negative cache remains a placeholder');else{check(current.available,name+' backend cache/meta really became positive');check(appeared,name+' same album page naturally refreshes a previously missing track image')}
   check(await ev("S.view==='album'&&document.querySelector('img[data-song-index]')===initialCoverNode"),name+' refresh preserves current view and existing DOM node');
   const lookups=records.filter(r=>r.scenario===name&&r.path==='/api/cover/lookup').length;
   check(lookups===(name==='track-meta-off'?0:1),name+' does not repeatedly invoke online cover lookup',lookups);
   if(name==='negative'){
    check(await ev("initialCoverNode.style.visibility==='hidden'&&initialCoverNode.naturalWidth===0"),'permanent real HTTP 404 retains the placeholder');
    const remaining=await ev('Math.max(0,COVP.cacheDeadline-Date.now())');console.log('Waiting '+remaining+'ms for the real 60-second cache deadline');await sleep(remaining+2100);
    const count=records.filter(r=>r.scenario===name&&(r.path==='/api/cover'||r.path==='/api/lib/albums')).length;await sleep(2200);
    check(await ev('COVP.cacheTimer===0')&&count===records.filter(r=>r.scenario===name&&(r.path==='/api/cover'||r.path==='/api/lib/albums')).length,'real 60-second observation deadline stops further cache reads');
    check(records.filter(r=>r.scenario===name&&r.path==='/api/cover/lookup').length===1,'permanent negative never re-runs online lookup during the full minute');
    check(await ev('S.coverCacheWatch===null'),'current-song negative cache probe also stops after a real minute');
    check(records.filter(r=>r.scenario===name&&r.path==='/api/meta').length===1,'full-minute negative probing never re-runs complete song metadata');
    await ev("watchCachedCoverImages();window.dispatchEvent(new Event('pagehide'))");check(await ev('COVP.cacheTimer===0'),'page close lifecycle cancels cache timer');
    await ev("watchCachedCoverImages();setView('artists')");check(await ev('COVP.cacheTimer===0&&!initialCoverNode.songArtwork.current()'),'view or album change invalidates the old track and timer');
   }
  }
  check(records.filter(r=>r.scenario===name&&r.path==='/api/lib/cover/prefetch').length===prefetchAtEntry,name+' cache refresh does not repeatedly start web prefetch',prefetchAtEntry);
 }
 check(exceptions.length===0,'full production DOM produces no JavaScript exceptions',exceptions);check(outside.length===0,'no external service/paid URL was requested',outside);
 check(!records.some(r=>/^\/api\/(?:play|lib\/play|skip|input\/start|roomcorr\/measure)$/.test(r.path)),'no playback or audio-device endpoint requested');
 check(!records.some(r=>r.path==='/api/queue/set'||r.path==='/api/queue/clear'),'cover refresh never changes the queue');
 check(!records.some(r=>r.path==='/api/workbench/apply'),'cover refresh never changes listening or console parameters');
 const summary={checks,exceptions,outside,records,home,fullProductionDom:true,offlineUiRenderer:true,realPlayerStarted:false};fs.writeFileSync(path.join(home,'results.json'),JSON.stringify(summary,null,2));console.log(JSON.stringify({home,checks:checks.length,failures:checks.filter(c=>!c.ok).length}));if(checks.some(c=>!c.ok))process.exitCode=1;
 }finally{if(current?.timer)clearTimeout(current.timer);if(ws&&ws.readyState===1){send('Browser.close').catch(()=>{});await sleep(400);ws.close()}await new Promise(r=>server.close(r));}
})().catch(e=>{console.error(e);process.exitCode=1});

