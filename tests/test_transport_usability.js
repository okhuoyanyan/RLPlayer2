// Offline Edge regression for status ordering, decoded artwork and transport inputs.
// Run with the normal RLPlayer firewall guard. --assets may point at HTML/JS
// captured from a released backend. Every browser request is settled in memory.
'use strict';
const fs = require('node:fs');
const path = require('node:path');
const {spawn,spawnSync} = require('node:child_process');
const {once} = require('node:events');
const delay = ms => new Promise(resolve => setTimeout(resolve,ms));
function option(name,fallback) {
  const index = process.argv.indexOf(name);
  return index < 0 ? fallback : process.argv[index+1];
}
const root=path.resolve(option('--source',process.env.RLPLAYER_TEST_SOURCE_ROOT || path.join(__dirname,'..')));
const workRoot=path.resolve(option('--work-root','transport-usability-evidence'));
fs.mkdirSync(workRoot,{recursive:true});
const run=fs.mkdtempSync(path.join(workRoot,'transport-'));
const errors=[];
function program(command,args) {
  const p=spawnSync(command,args,{encoding:'utf8',windowsHide:true});
  if (p.error || p.status!==0) throw Error([p.error,p.stdout,p.stderr].filter(Boolean).join('\n'));
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
  const responding=new Set();
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
  const api={send,async attach(){const {targetId}=await send('Target.createTarget',{url:'about:blank'});const result=await send('Target.attachToTarget',{targetId,flatten:true});sessionId=result.sessionId;},async close(){api.onRequest=null;await Promise.all([...responding]);if(proc.exitCode===null){try{await send('Browser.close',{},null);}catch{}proc.stdio[3].end();}await closed;},onRequest:null};
  proc.stdio[4].on('data',chunk=>{
    buffer=Buffer.concat([buffer,chunk]);
    let split;
    while((split=buffer.indexOf(0))!==-1){
      const msg=JSON.parse(buffer.subarray(0,split).toString('utf8'));buffer=buffer.subarray(split+1);
      if(msg.id&&pending.has(msg.id)){const p=pending.get(msg.id);pending.delete(msg.id);clearTimeout(p.timeout);msg.error?p.reject(Error(msg.error.message)):p.resolve(msg.result);}
      else if(msg.method==='Runtime.exceptionThrown')errors.push(msg.params.exceptionDetails.exception?.description||msg.params.exceptionDetails.text);
      else if(msg.method==='Fetch.requestPaused'&&api.onRequest){
        const response=api.onRequest(msg.params).catch(e=>errors.push(String(e)));
        responding.add(response);response.finally(()=>responding.delete(response));
      }
    }
  });
  return api;
}
const origin='https://transport-test.invalid';
const files={A:'Q:/A/first.flac',B:'Q:/B/second.flac',C:'Q:/C/no-cover.flac'};
const report={checks:[],art:[],geometry:[],requests:[],errors};
const gate=option('--cover-only','')==='1';
function check(label,ok,details){report.checks.push({label,ok:!!ok,details});if(!ok)console.error('FAIL',label,details);}
async function waitFor(fn,label) {
  const end=Date.now()+4500;
  while(Date.now()<end){const value=await fn();if(value)return value;await delay(25);}
  throw Error('Timed out waiting for '+label);
}
async function main(){
  const assets=exportPage(),browser=startBrowser();
  let file=files.A,holdStatus=false,metaDelay='',imageDelay='',volume=-42.5,queuedBody='',artVersion='fixture';
  const statusWait=[],metadataWait=[],imageWait=[],writes=[];
  const status=f=>({ok:true,playing:!!f,paused:false,current:f,pos:18,dur:f?120:0,volDb:volume,wf:0,wr:0,pulls:0,inRate:44100,outRate:44100,outBits:16});
  const metadata=f=>({ok:true,title:f===files.A?'One':f===files.B?'Two':'Empty',artist:'Fixture',album:f,hasCover:f!==files.C,coverUrl:'/api/cover?file='+encodeURIComponent(f)+'&v='+artVersion});
  const picture=f=>'<svg xmlns="http://www.w3.org/2000/svg" width="12" height="12"><rect width="12" height="12" fill="'+(f===files.A?'#ff0000':'#0080ff')+'"/></svg>';
  const send=(requestId,body,type='application/json',code=200)=>browser.send('Fetch.fulfillRequest',{requestId,responseCode:code,responseHeaders:[{name:'Content-Type',value:type},{name:'Cache-Control',value:'no-store'}],body:Buffer.from(typeof body==='string'?body:JSON.stringify(body)).toString('base64')});
  const evaluate=async expression=>{const r=await browser.send('Runtime.evaluate',{expression,awaitPromise:true,returnByValue:true});if(r.exceptionDetails)throw Error(r.exceptionDetails.exception?.description||r.exceptionDetails.text);return r.result.value;};
  const ready=(expression,label=expression)=>waitFor(()=>evaluate('Boolean('+expression+')'),label);
  const screenshot=async name=>{const p=await browser.send('Page.captureScreenshot',{format:'png'});fs.writeFileSync(path.join(run,name+'.png'),Buffer.from(p.data,'base64'));};
  const poll=async f=>{const pending=await waitFor(()=>statusWait.shift(),'next actual status request');await send(pending,status(f));};
  const art=()=>evaluate("(()=>{const im=D('npCover');let color='';try{const c=document.createElement('canvas');c.width=c.height=1;const cx=c.getContext('2d');cx.drawImage(im,0,0,1,1);color=Array.from(cx.getImageData(0,0,1,1).data).slice(0,3).join(',')}catch(_){}return {file:S.lastCur,title:D('npTitle').textContent,src:im.currentSrc,color};})()");
  const canvasColor=async (f,color)=>waitFor(async()=>{const r=await art();return r.file===f&&r.color===color&&r;},'decoded pixels '+color);
  const key=async name=>{const codes={ArrowRight:39,ArrowUp:38,Home:36,End:35};for(const type of ['keyDown','keyUp']) await browser.send('Input.dispatchKeyEvent',{type,key:name,code:name,windowsVirtualKeyCode:codes[name],nativeVirtualKeyCode:codes[name]});};
  const width=w=>browser.send('Emulation.setDeviceMetricsOverride',{width:w,height:w<600?720:800,deviceScaleFactor:1,mobile:false});
  const itemGeometry=()=>evaluate("(()=>{const rect=id=>{const r=D(id).getBoundingClientRect();return {x:r.x,y:r.y,w:r.width,h:r.height};};return {width:innerWidth,seek:rect('seek'),volume:rect('vol'),db:D('barVolInput')&&rect('barVolInput'),overflow:document.documentElement.scrollWidth-innerWidth};})()");
  try{
    await browser.attach();await browser.send('Page.enable');await browser.send('Runtime.enable');
    browser.onRequest=async({requestId,request})=>{
      const u=new URL(request.url),route=u.pathname;
      if(u.origin!==origin){errors.push('Unexpected outside URL');await send(requestId,'','text/plain',403);return;}
      if(route==='/')return send(requestId,assets.html,'text/html; charset=utf-8');
      if(route==='/app.js')return send(requestId,assets.js,'application/javascript; charset=utf-8');
      if(route==='/api/status'){if(holdStatus){statusWait.push(requestId);return;}return send(requestId,status(file));}
      if(route==='/api/meta'){const f=u.searchParams.get('file');if(f===metaDelay){metadataWait.push(requestId);return;}return send(requestId,metadata(f));}
      if(route==='/api/cover'){const f=u.searchParams.get('file');if(f===imageDelay){imageWait.push(requestId);return;}return f===files.C?send(requestId,'','image/svg+xml',404):send(requestId,picture(f),'image/svg+xml');}
      if(route==='/api/settings'&&request.method==='GET')return send(requestId,{ok:true,kv:{backend:'null',device:'null',volume:'-60',quickstart:'1',uilang:'zh',metaon:'0',metallm:'0',autoanl:'0',anlmaster:'0'}});
      if(route==='/api/signal')return send(requestId,{ok:true,path:{src:file,volume:volume,inRate:44100,outRate:44100,inBits:16,outBits:16,inCh:2,outCh:2,bitPerfect:'no'}});
      if(route==='/api/i18n')return send(requestId,{ok:true,lang:'zh',setting:'zh',map:{}});
      if(route==='/api/queue')return send(requestId,{ok:true,items:[files.A,files.B,files.C],index:0,cues:[[-1,-1],[-1,-1],[-1,-1]]});
      if(route==='/api/seek'||route==='/api/volume'||route==='/api/queue/set'){writes.push({route,query:u.search,body:request.postData||''});if(route==='/api/volume')volume=Number(u.searchParams.get('db'));if(route==='/api/queue/set')queuedBody=request.postData;return send(requestId,{ok:true,count:2});}
      if(['/api/play','/api/pause','/api/stop','/api/feedback','/api/playmode'].includes(route))errors.push('Unrequested audio or feedback '+route);
      return send(requestId,{ok:true,items:[],albums:[],artists:[],roots:[],tracks:[],lines:[],providers:[],scanning:false,rates:[44100]});
    };
    await browser.send('Fetch.enable',{patterns:[{urlPattern:'*',requestStage:'Request'}]});
    await width(1340);await browser.send('Page.navigate',{url:origin+'/'});
    await ready('window.S && S.curMeta && PLAYBACK_SETTINGS.ready');
    report.geometry.push(await itemGeometry());
    await evaluate('openNowPlaying()');await canvasColor(files.A,'255,0,0');
    holdStatus=true;await evaluate('tick();tick()');await waitFor(()=>statusWait.length>=2,'two overlapping polls');
    const early=statusWait.shift();file=files.B;await poll(file);
    report.art.push({stage:'B loaded',...await canvasColor(file,'0,128,255')});
    await send(early,status(files.A));await delay(220);
    const stale=await art();report.art.push({stage:'older A status delivered',...stale});
    check('late A status cannot replace decoded B artwork or title',stale.file===files.B&&stale.color==='0,128,255'&&stale.title==='Two');
    await screenshot('late-status');
    if(!gate){
      file=files.A;metaDelay=files.A;await poll(file);await waitFor(()=>metadataWait.length,'delayed A metadata');
      file=files.C;await poll(file);await ready("D('npTitle').textContent==='Empty' && D('npCover').currentSrc.startsWith('data:')");
      metaDelay='';await send(metadataWait.shift(),metadata(files.A));await delay(130);
      check('no-cover C rejects A metadata which arrived late',(await art()).src.startsWith('data:')&&(await art()).title==='Empty');
      artVersion='delayed-image';file=files.A;imageDelay=files.A;await poll(file);await waitFor(()=>imageWait.length,'late A image');
      file=files.B;await poll(file);await canvasColor(file,'0,128,255');
      imageDelay='';while(imageWait.length)await send(imageWait.shift(),picture(files.A),'image/svg+xml');
      await delay(160);check('decoded B keeps its own pixels after A image arrives',(await art()).color==='0,128,255');
      await evaluate('closeNowPlaying()');await ready('D("transportSeek") && !D("transportSeek").disabled');
      for(const w of [1340,1024,800,640,390,320]){
        await width(w);const g=await itemGeometry();report.geometry.push(g);
        check('timeline and volume reachable at '+w,g.seek.h>=24&&g.overflow===0&&g.db.w>0&&g.db.x>=0&&g.db.x+g.db.w<=w+1&&g.volume.x+g.volume.w<=w+1,g);
      }
      await width(1340);await evaluate("D('transportSeek').focus()");
      for(const [name,f] of [['Home',0],['ArrowRight',5/120],['End',.9999]]){
        const n=writes.length;await key(name);const got=await waitFor(()=>writes.slice(n).find(r=>r.route==='/api/seek'),'seek '+name);
        check('focused timeline keyboard '+name,Math.abs(Number(new URLSearchParams(got.query).get('pos'))-f)<.001);
      }
      const g=await itemGeometry(),y=g.seek.y+g.seek.h/2,x=g.seek.x+g.seek.w*.75;
      const n=writes.length;for(const type of ['mousePressed','mouseReleased'])await browser.send('Input.dispatchMouseEvent',{type,x,y,button:'left',clickCount:1});
      const pointer=await waitFor(()=>writes.slice(n).find(r=>r.route==='/api/seek'),'pointer seek');
      check('pointer seeks to three quarters of track',Math.abs(Number(new URLSearchParams(pointer.query).get('pos'))-.75)<.025);
      for(const [name,action] of [['start','touchStart'],['end','touchEnd']]){
        if(name==='start')await browser.send('Emulation.setTouchEmulationEnabled',{enabled:true,maxTouchPoints:1});
        await browser.send('Input.dispatchTouchEvent',{type:action,touchPoints:name==='start'?[{x:g.seek.x+g.seek.w*.25,y}]:[]});
      }
      await waitFor(()=>writes.filter(r=>r.route==='/api/seek').length>=5,'touch seek');
      check('touch seeks to one quarter of track',Math.abs(Number(new URLSearchParams(writes.filter(r=>r.route==='/api/seek').at(-1).query).get('pos'))-.25)<.03);
      await browser.send('Emulation.setTouchEmulationEnabled',{enabled:false});
      check('volume label input and slider report backend dB',await evaluate("D('barVolDb').textContent==='-42.5 dB' && Number(D('vol').value)===-42.5 && Number(D('barVolInput').value)===-42.5"));
      const targetVolume=async expected=>waitFor(()=>writes.findLast(r=>r.route==='/api/volume'&&Number(new URLSearchParams(r.query).get('db'))===expected),'volume '+expected);
      await evaluate("D('volFloor').click()");await targetVolume(-120);check('minimum-volume action reports exact attenuation',await evaluate("D('barVolDb').textContent==='-120.0 dB'"));
      await evaluate("D('volFloor').click()");await targetVolume(-42.5);check('restore returns to the original dB',await evaluate("D('barVolDb').textContent==='-42.5 dB'"));
      await evaluate('showChain()');await ready("D('mbody').innerText.includes('Shift')");check('shortcut available in signal dialog',true);
      await evaluate("D('mclose').click();setView('queue');renderQueuePage()");await ready("D('qImportM3u')");
      file='';await poll(file);await ready('!S.playing');
      const selectPlaylist=()=>evaluate("(()=>{const dt=new DataTransfer();dt.items.add(new File(['#EXTM3U\\r\\n..\\\\A.flac\\r\\n#RTA-CUE 10 20\\r\\nD:\\\\B.flac'], 'list.m3u8'));D('qM3uFile').files=dt.files;D('qM3uFile').dispatchEvent(new Event('change'));})()");
      await selectPlaylist();await ready("D('askIn')");await evaluate("D('askIn').value='D:\\\\Library\\\\Lists';D('askOk').click()");await ready("D('cfYes')");
      check('import preview writes nothing',!queuedBody);await evaluate("D('cfNo').click()");check('cancel preserves queue',!queuedBody);
      await selectPlaylist();await ready("D('askIn')");await evaluate("D('askIn').value='D:\\\\Library\\\\Lists';D('askOk').click()");await ready("D('cfYes')");await evaluate("D('cfYes').click()");
      await waitFor(()=>queuedBody,'explicit queue import');check('import preserves ordering, absolute paths and CUE interval',new URLSearchParams(queuedBody).get('files')==='D:\\Library\\A.flac\nD:\\B.flac\t10\t20');
      const parser=await evaluate("(()=>{const ok=parseTransportM3u('\\uFEFF#EXTM3U\\r\\nfile:///D:/hi%20there.flac\\r\\n\\\\\\\\server\\\\share\\\\file.wav','');const invalid=['https://example.invalid/track','D:foo.flac','#EXT-X-VERSION:3\\nD:/audio.wav','#RTA-CUE 20 10\\nD:/audio.wav'];return {paths:ok.items,invalid:invalid.filter(x=>{try{parseTransportM3u(x,'');return false}catch(_){return true}}).length}})()");
      check('file URI, UNC and malformed input are distinguished',parser.paths[0]==='D:\\hi there.flac'&&parser.paths[1]==='\\\\server\\share\\file.wav'&&parser.invalid===4,parser);
      check('no import started playback',errors.length===0);
    }
  }finally{
    report.requests=writes;
    fs.writeFileSync(path.join(run,'result.json'),JSON.stringify(report,null,2));
    await browser.close();
  }
  console.log(JSON.stringify({passed:report.checks.filter(c=>c.ok).length,checks:report.checks.length,errors,evidence:run}));
  if(errors.length||report.checks.some(c=>!c.ok))process.exitCode=1;
}
main().catch(e=>{console.error(e);process.exitCode=1;});

