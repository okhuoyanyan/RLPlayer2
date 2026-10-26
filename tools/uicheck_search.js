// uicheck_search.js — 261026-05 #1：搜索栏按视图分派
const { spawn } = require('child_process');
const fs = require('fs'); const path = require('path'); const os = require('os');
const URL_ = process.argv[2];
const EDGE = ['C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\msedge.exe','C:\\Program Files\\Microsoft\\Edge\\Application\\msedge.exe'].find(p=>fs.existsSync(p));
const PORT = 9950 + Math.floor(Math.random()*40);
const udd = path.join(os.tmpdir(), 'edge_s_' + process.pid + '_' + Date.now());
const child = spawn(EDGE, ['--headless=new','--disable-gpu','--no-first-run','--user-data-dir='+udd,'--remote-debugging-port='+PORT,'--window-size=1600,1000','about:blank'], {stdio:'ignore'});
const sleep = ms => new Promise(r=>setTimeout(r,ms));
let ws, id=0; const pending=new Map(); const fails=[]; let passes=0;
function send(m,p){return new Promise((res,rej)=>{const i=++id;pending.set(i,{res,rej});ws.send(JSON.stringify({id:i,method:m,params:p||{}}));});}
async function evalJs(e){const r=await send('Runtime.evaluate',{expression:e,awaitPromise:true,returnByValue:true});if(r.exceptionDetails)throw new Error('JS: '+r.exceptionDetails.text);return r.result&&r.result.value;}
function ck(ok,n,i){if(ok){passes++;console.log('  [PASS] '+n+(i!==undefined?'  '+JSON.stringify(i):''));}else{fails.push(n);console.log('  [FAIL] '+n+(i!==undefined?'  '+JSON.stringify(i):''));}}
async function waitFor(e,ms){const t0=Date.now();while(Date.now()-t0<(ms||10000)){if(await evalJs('!!('+e+')'))return true;await sleep(150);}return false;}
async function targets(){for(let i=0;i<60;i++){try{const r=await fetch('http://127.0.0.1:'+PORT+'/json/list');const j=await r.json();const p=j.filter(t=>t.type==='page');if(p.length)return p;}catch(e){}await sleep(250);}throw new Error('no devtools');}
(async()=>{
  const pages=await targets(); ws=new WebSocket(pages[0].webSocketDebuggerUrl);
  await new Promise((res,rej)=>{ws.onopen=res;ws.onerror=rej;});
  ws.onmessage=ev=>{const m=JSON.parse(ev.data);if(m.id&&pending.has(m.id)){const p=pending.get(m.id);pending.delete(m.id);m.error?p.rej(new Error(JSON.stringify(m.error))):p.res(m.result);}};
  await send('Runtime.enable'); await send('Page.enable'); await send('Page.navigate',{url:URL_});
  await sleep(3000); await waitFor("window.S && S.view",15000);

  // 1) 专辑页搜索：应留在 albums
  await evalJs("Array.from(document.querySelectorAll('.nav')).find(e=>e.dataset.v==='albums').click()");
  await waitFor("S.view==='albums' && document.querySelectorAll('.card').length>0", 15000);
  const beforeAlbums = await evalJs("document.querySelectorAll('.card').length");
  await evalJs("(function(){var q=document.getElementById('q');q.value='盈月';q.dispatchEvent(new Event('input'));})()");
  await sleep(1600);
  const afterView = await evalJs("S.view");
  const afterAlbums = await evalJs("document.querySelectorAll('.card').length");
  ck(afterView === 'albums', '专辑页搜索后仍停留在专辑视图', {view: afterView, before: beforeAlbums, after: afterAlbums});
  ck(afterAlbums <= beforeAlbums && afterAlbums > 0, '专辑列表被过滤（条数减少但非空）', {before: beforeAlbums, after: afterAlbums});

  // 2) 艺术家页搜索：应留在 artists 且被过滤
  await evalJs("(function(){var q=document.getElementById('q');q.value='';q.dispatchEvent(new Event('input'));})()");
  await sleep(900);
  await evalJs("Array.from(document.querySelectorAll('.nav')).find(e=>e.dataset.v==='artists').click()");
  await waitFor("S.view==='artists'", 12000);
  const beforeArt = await evalJs("document.querySelectorAll('.card').length");
  await evalJs("(function(){var q=document.getElementById('q');q.value='A';q.dispatchEvent(new Event('input'));})()");
  await sleep(1600);
  const artView = await evalJs("S.view");
  const afterArt = await evalJs("document.querySelectorAll('.card').length");
  ck(artView === 'artists', '艺术家页搜索后仍停留在艺术家视图', {view: artView});
  ck(afterArt <= beforeArt, '艺术家列表被过滤', {before: beforeArt, after: afterArt});

  const shot = await send('Page.captureScreenshot',{format:'png'});
  fs.writeFileSync(path.join(__dirname,'..','build','search_fix.png'), Buffer.from(shot.data,'base64'));
  console.log('\n结果: '+passes+' 通过, '+fails.length+' 失败');
  try{child.kill();}catch(e){}
  process.exit(fails.length?1:0);
})().catch(e=>{console.error('FATAL '+e.message);try{child.kill();}catch(_){}process.exit(2);});
