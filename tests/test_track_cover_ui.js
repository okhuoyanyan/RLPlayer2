'use strict';
// Exercise actual production UI callbacks with controlled out-of-order responses.
const fs = require('fs');
const path = require('path');
const vm = require('vm');
const source = fs.readFileSync(process.argv[2] || path.join(__dirname,'../src/net/ui_script.h'),'utf8');
const section = source.slice(source.indexOf('function buildNpMetaHtml('),source.indexOf('/* ---------------- 状态 ---------------- */'));
if (!section.includes('function loadMeta(')) throw Error('Production metadata seam missing');
let checks=0, failures=0;
function check(ok,label) { checks++; if(!ok){failures++;console.log('FAIL '+label);} else console.log('PASS '+label); }
function environment() {
  const elements={}; const requests=[]; const timers=[]; const uploads=[]; const intervals=new Map(); let intervalId=0;
  const D=id=>elements[id]||(elements[id]={textContent:'',title:'',style:{backgroundImage:''},classList:{contains:()=>false,add:()=>{},remove:()=>{}},querySelectorAll:()=>[]});
  const ctx={S:{cur:'old.wav',lastCur:'old.wav',curMeta:{title:'Old'},curCh:2},D,
    api:(url,body)=>new Promise((resolve,reject)=>requests.push({url,body,resolve,reject})),
    XMLHttpRequest:function(){uploads.push(this);this.open=(method,url)=>{this.method=method;this.url=url;};this.send=body=>{this.body=body;};},
    nam:p=>p,t:x=>x,tf:(s,values)=>s.replace('{0}',values[0]),esc:x=>x,
    renderNp:()=>{},Date,URL,encodeURIComponent,location:{href:'http://127.0.0.1:8899/'},
    setTimeout:fn=>{timers.push(fn);return timers.length;},document:{querySelectorAll:()=>[]},
    setInterval:(fn,ms)=>{const id=++intervalId;intervals.set(id,{fn,ms});return id;},clearInterval:id=>intervals.delete(id),
    window:{addEventListener:()=>{}}};
  vm.createContext(ctx);vm.runInContext(section,ctx);
  // Load the production cache-observation dependencies, rather than hiding them
  // behind a no-op stub when exercising the older artwork seams below.
  const coverState=source.indexOf('var COVP ='),coverStop=source.indexOf('function stopCachedCoverImages(');
  vm.runInContext(source.slice(coverState,source.indexOf('function startCoverPrefetch(',coverState)),ctx);
  vm.runInContext(source.slice(coverStop,source.indexOf('var albumListRequest =',coverStop)),ctx);
  D('cv').style.backgroundImage="url('old-cover.jpg')";
  return {ctx,D,requests,timers,uploads,intervals};
}
async function flush(){await Promise.resolve();await Promise.resolve();await Promise.resolve();}
(async()=>{
  {
    const e=environment();e.ctx.S.cur='new.wav';e.ctx.S.lastCur='new.wav';e.ctx.loadMeta('new.wav');
    check(e.D('cv').style.backgroundImage==='','switch clears previous song image before request completes');
    check(!e.ctx.S.curMeta,'switch clears previous song metadata before request completes');
    e.requests[0].resolve({title:'New',album:'Tagged',hasCover:true,coverUrl:'/api/cover?file=new.wav&v=revision2'});await flush();
    check(e.D('cv').style.backgroundImage.includes('v=revision2'),'uses authoritative versioned cover URL');
  }
  {
    const e=environment();e.ctx.loadMeta('old.wav');
    e.ctx.S.cur='new.wav';e.ctx.S.lastCur='new.wav';e.ctx.loadMeta('new.wav');
    e.requests[1].resolve({title:'New',hasCover:true});await flush();
    e.requests[0].resolve({title:'Old',hasCover:true});await flush();
    check(e.D('npt').textContent==='New','late previous song response cannot replace current title');
    check(e.D('cv').style.backgroundImage.includes('new.wav'),'late previous song response cannot replace current cover');
  }
  {
    const e=environment();e.ctx.loadMeta('old.wav');
    e.ctx.S.cur='new.wav';e.ctx.S.lastCur='new.wav';e.ctx.loadMeta('new.wav');
    e.requests[1].resolve({title:'New',channels:6,hasCover:true});await flush();
    e.requests[0].reject(Error('late old failure'));await flush();
    check(e.D('npt').textContent==='New','late previous song failure cannot reset current title');
    check(e.ctx.S.curCh===6,'late previous song failure cannot reset current channel metadata');
  }
  {
    const e=environment();e.ctx.loadMeta('old.wav');e.ctx.loadMeta('old.wav');
    e.requests[1].resolve({title:'Fresh',hasCover:true,coverUrl:'/api/cover?file=old.wav&v=fresh'});await flush();
    e.requests[0].resolve({title:'Stale',hasCover:false});await flush();
    check(e.D('npt').textContent==='Fresh'&&e.D('cv').style.backgroundImage.includes('v=fresh'),'late same-file response cannot hide newly acquired art');
  }
  {
    const e=environment();e.ctx.loadMeta('old.wav');e.ctx.S.cur='';e.ctx.S.lastCur='';
    e.requests[0].resolve({title:'Old',hasCover:true});await flush();
    check(e.D('npt').textContent!=='Old','metadata response after stop is ignored');
  }
  {
    const e=environment();e.ctx.loadMeta('old.wav');
    e.requests[0].resolve({title:'Track',album:'',folderName:'TEST站测评曲',hasCover:false});await flush();
    check(e.D('npart').textContent==='文件夹：TEST站测评曲','folder context is labeled without inventing a release album');
    check(e.D('cv').style.backgroundImage==='','a confirmed absence of cover clears the old image');
  }
  {
    const e=environment();e.ctx.loadMeta('old.wav');
    e.requests[0].resolve({title:'Track',hasCover:true,coverUrl:'https://untrusted.invalid/art.jpg'});await flush();
    check(e.D('cv').style.backgroundImage.includes('/api/cover?')&&!e.D('cv').style.backgroundImage.includes('untrusted'),'art uses a local resolver URL');
  }
  {
    const e=environment();e.ctx.S.view='album';
    const begin=source.indexOf('function refreshCoverImages(');
    vm.runInContext(source.slice(begin,source.indexOf('\n}',begin)+2),e.ctx);
    e.ctx.refreshCoverImages();e.ctx.refreshCoverImages();
    check(e.timers.length===1,'cover completion refresh is coalesced outside the album grid');
    e.timers.forEach(fn=>fn());
    check(e.requests.length===1&&e.requests[0].url.includes('old.wav'),'cover completion refreshes current song without navigation');
    check(e.ctx.S.view==='album','cover refresh keeps current detail view');
  }
  {
    const e=environment();const image={name:'chosen.png'};
    const upload=e.ctx.saveTrackCover('old.wav',image);
    check(e.uploads[0].method==='POST'&&e.uploads[0].url==='/api/cover/set?file=old.wav'&&e.uploads[0].body===image,'song art upload names exactly the selected track');
    e.ctx.S.lastCur='new.wav';e.uploads[0].status=200;e.uploads[0].responseText='{"ok":true}';e.uploads[0].onload();await upload;
    check(e.requests.length===0,'upload completion after song switch cannot reload previous metadata');
  }
  {
    const e=environment();const upload=e.ctx.saveTrackCover('old.wav',{});
    e.uploads[0].status=200;e.uploads[0].responseText='{"ok":true}';e.uploads[0].onload();await upload;
    check(e.requests.length===1&&e.requests[0].url.includes('old.wav'),'successful current-song upload refreshes its authoritative metadata');
  }
  for (const failure of ['http','json','network','timeout']) {
    const e=environment();let rejected=false;
    const upload=e.ctx.saveTrackCover('old.wav',{}).catch(()=>{rejected=true;});
    if(failure==='network')e.uploads[0].onerror();
    else if(failure==='timeout')e.uploads[0].ontimeout();
    else {e.uploads[0].status=failure==='http'?500:200;e.uploads[0].responseText=failure==='json'?'{broken':'{"ok":true}';e.uploads[0].onload();}
    await upload;check(rejected&&e.requests.length===0,'failed '+failure+' upload cannot claim success or refresh art');
  }
  {
    const e=environment();const reset=e.ctx.resetTrackCover('old.wav');
    check(e.requests[0].url==='/api/cover/reset?file=old.wav'&&e.requests[0].body==='','reset uses explicit POST body for exactly the selected track');
    e.ctx.S.lastCur='new.wav';e.requests[0].resolve({ok:true});await reset;
    check(e.requests.length===1,'reset completion after song switch cannot reload previous metadata');
  }
  {
    const e=environment();let rejected=false;const reset=e.ctx.resetTrackCover('old.wav').catch(()=>{rejected=true;});
    e.requests[0].resolve({ok:false,error:'write refused'});await reset;
    check(rejected&&e.requests.length===1,'reset storage failure is reported without a false UI refresh');
  }
  for (const wasScanning of [true,false]) {
    const e=environment();e.ctx.S.scanning=wasScanning;e.ctx.S.view='settings';
    Object.assign(e.ctx,{renderLibStats:()=>{},toast:()=>{},drawRoots:()=>{},window:{}});
    const begin=source.indexOf('function loadLibStats(');
    vm.runInContext(source.slice(begin,source.indexOf('\n}',begin)+2),e.ctx);
    e.ctx.loadLibStats();e.requests[0].resolve({scanning:false,roots:[],tracks:18,albums:18});await flush();
    e.timers.forEach(fn=>fn());
    check(wasScanning ? e.requests.length===2&&e.requests[1].url.includes('/api/meta?file=old.wav') : e.requests.length===1,
      wasScanning ? 'completed metadata migration refreshes current song without switching tracks' : 'idle library polling does not repeatedly reload current metadata');
  }
  {
    const from=source.indexOf('var albumArtwork ='), to=source.indexOf('function openAlbum(',from);
    check(from>=0 && to>from,'album track list has a production artwork binding');
    if(from>=0 && to>from){
      const e=environment(), album={k:'abc',tracks:[{path:'One & A (Live).wav'},{path:'Two.wav'},{path:'Three.wav'}]};
      const images=album.tracks.map((_,i)=>({dataset:{songIndex:String(i)},style:{},isConnected:true}));
      let visible,disconnected=0;
      Object.assign(e.ctx,{META:{on:1},IntersectionObserver:function(callback){visible=callback;this.observe=()=>{};this.disconnect=()=>{disconnected++;};}});
      e.ctx.S.view='album';e.ctx.S.album=album;e.D('list').querySelectorAll=()=>images;
      vm.runInContext(source.slice(from,to),e.ctx);
      e.ctx.bindAlbumArtwork(album);
      check(e.intervals.size===1&&Array.from(e.intervals.values())[0].ms===2000,'album starts the real bounded cache observation timer');
      check(images.every(im=>!im.src)&&e.requests.length===0,'offscreen album rows start with no disk or online requests');
      visible(images.map(im=>({target:im,isIntersecting:true})));
      check(images[0].src==='/api/cover?file=One%20%26%20A%20(Live).wav'&&images[1].src==='/api/cover?file=Two.wav','visible song rows request their own exact audio paths');
      images.forEach(im=>im.onerror());
      check(e.requests.length===2&&e.requests.every(r=>r.body===''&&r.url.startsWith('/api/cover/lookup?file=')),'visible missing artwork uses bounded explicit song lookups');
      e.requests[0].resolve({ok:true,hasCover:false,pending:true,retryAfterMs:500});await flush();
      check(e.timers.length===1&&e.requests.length===2,'pending artwork waits without opening an extra queue slot');
      e.timers.shift()();e.requests[2].resolve({ok:true,hasCover:true,coverUrl:'/api/cover?file=One%20%26%20A%20%28Live%29.wav&v=123'});await flush();
      check(images[0].src.endsWith('&v=123')&&e.requests.length===4,'equivalently escaped URLs refresh the correct Live recording and release the slot');
      e.ctx.stopAlbumArtwork();e.ctx.S.view='settings';
      check(e.intervals.size===0,'leaving album clears the real cache observation timer');
      const before=images[1].src;e.requests[1].resolve({ok:true,hasCover:true,coverUrl:'/api/cover?file=Two.wav&v=late'});await flush();
      check(images[1].src===before&&disconnected===1,'leaving an album discards late artwork results');
      e.ctx.S.view='album';e.ctx.META.on=0;images.forEach(im=>{im.src='';});e.ctx.bindAlbumArtwork(album);
      visible([{target:images[2],isIntersecting:true}]);images[2].onerror();await flush();
      check(e.requests.length===4,'disabling online metadata leaves local images available without remote lookup');
    }
  }
  {
    const e=environment();let errors=0;
    Object.assign(e.ctx,{startCoverPrefetch:()=>{},head:()=>{},albumCardHtml:a=>a.name,artaWarmSoon:()=>{},renderPanelError:()=>{errors++;}});
    const begin=source.indexOf('function loadAlbums(q, title)'), end=source.indexOf('\n}',begin)+2;
    vm.runInContext('var albumListRequest=0;'+source.slice(begin,end),e.ctx);
    e.ctx.loadAlbums();e.ctx.S.view='album';e.D('list').innerHTML='Duca detail';
    e.requests[0].resolve({albums:[{name:'old grid'}]});await flush();
    check(e.D('list').innerHTML==='Duca detail','late album list cannot replace the open release detail');
    e.ctx.loadAlbums('first');e.ctx.loadAlbums('second');
    e.requests[2].resolve({albums:[{name:'second'}]});await flush();
    e.requests[1].resolve({albums:[{name:'first'}]});await flush();
    check(e.ctx.S.albums[0].name==='second','a slower previous filter cannot overwrite the current album list');
    e.ctx.loadAlbums('obsolete');e.ctx.loadAlbums('current');
    e.requests[3].reject(Error('stale'));await flush();
    check(errors===0,'obsolete album failures cannot show a retry panel on the newer list');
  }
  console.log(JSON.stringify({checks,failures}));process.exitCode=failures?1:0;
})().catch(e=>{console.error(e);process.exitCode=1;});
