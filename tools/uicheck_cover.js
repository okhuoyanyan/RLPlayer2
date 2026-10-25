// uicheck_cover.js — 封面：进度可见 + 实际加载出图（真实网络/缓存）
const { spawn } = require('child_process');
const fs = require('fs'); const path = require('path'); const os = require('os');
const URL_ = process.argv[2];
const EDGE = ['C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\msedge.exe','C:\\Program Files\\Microsoft\\Edge\\Application\\msedge.exe'].find(p=>fs.existsSync(p));
const PORT = 9800 + Math.floor(Math.random()*150);
const udd = path.join(os.tmpdir(), 'edge_cov_' + process.pid + '_' + Date.now());
const child = spawn(EDGE, ['--headless=new','--disable-gpu','--no-first-run','--user-data-dir='+udd,'--remote-debugging-port='+PORT,'--window-size=1600,1000','about:blank'], {stdio:'ignore'});
const sleep = ms => new Promise(r=>setTimeout(r,ms));
let ws, id=0; const pending=new Map(); const errors=[]; const fails=[]; let passes=0;
function send(m,p){return new Promise((res,rej)=>{const i=++id;pending.set(i,{res,rej});ws.send(JSON.stringify({id:i,method:m,params:p||{}}));});}
async function evalJs(e){const r=await send('Runtime.evaluate',{expression:e,awaitPromise:true,returnByValue:true});if(r.exceptionDetails)throw new Error('JS: '+r.exceptionDetails.text);return r.result&&r.result.value;}
function ck(ok,n,i){if(ok){passes++;console.log('  [PASS] '+n+(i!==undefined?'  '+JSON.stringify(i):''));}else{fails.push(n);console.log('  [FAIL] '+n+(i!==undefined?'  '+JSON.stringify(i):''));}}
async function waitFor(e,ms){const t0=Date.now();while(Date.now()-t0<(ms||8000)){if(await evalJs('!!('+e+')'))return true;await sleep(200);}return false;}
async function targets(){for(let i=0;i<60;i++){try{const r=await fetch('http://127.0.0.1:'+PORT+'/json/list');const j=await r.json();const p=j.filter(t=>t.type==='page');if(p.length)return p;}catch(e){}await sleep(250);}throw new Error('no devtools');}
(async()=>{
  const pages=await targets(); ws=new WebSocket(pages[0].webSocketDebuggerUrl);
  await new Promise((res,rej)=>{ws.onopen=res;ws.onerror=rej;});
  ws.onmessage=ev=>{const m=JSON.parse(ev.data);if(m.id&&pending.has(m.id)){const p=pending.get(m.id);pending.delete(m.id);m.error?p.rej(new Error(JSON.stringify(m.error))):p.res(m.result);}
    else if(m.method==='Runtime.exceptionThrown')errors.push(JSON.stringify(m.params.exceptionDetails).slice(0,200));};
  await send('Runtime.enable'); await send('Page.enable'); await send('Page.navigate',{url:URL_});
  await sleep(2500); await waitFor("window.S && S.view",15000);
  // 进专辑页
  await evalJs("Array.from(document.querySelectorAll('.nav')).find(e=>e.dataset.v==='albums').click()");
  const okAlbums = await waitFor("document.querySelectorAll('.card').length > 0", 20000);
  ck(okAlbums, '专辑列表已渲染', await evalJs("document.querySelectorAll('.card').length"));
  // 进度条/进度元素
  await sleep(2500);
  const progVisible = await evalJs("(function(){var e=document.getElementById('covProg');return e?{display:getComputedStyle(e).display, text:e.textContent}:null;})()");
  console.log('  covProg = ' + JSON.stringify(progVisible));
  // 等 8 秒让图片加载，统计真实解码成功的图
  await sleep(8000);
  const stat = await evalJs("(function(){var imgs=Array.from(document.querySelectorAll('.card .cv img'));var ok=0,fail=0;imgs.forEach(function(i){if(i.complete&&i.naturalWidth>4)ok++;else if(i.complete)fail++;});return {total:imgs.length,loaded:ok,failed:fail};})()");
  ck(stat && stat.total > 0, '专辑卡片里有封面 <img>', stat);
  ck(stat && stat.loaded > 0, '至少有一张封面真的解码成功（naturalWidth>4）', stat);
  // 百分比
  const pct = stat && stat.total ? Math.round(100*stat.loaded/stat.total) : 0;
  console.log('  首屏封面加载率 = ' + pct + '%  (' + stat.loaded + '/' + stat.total + ')');
  // 打开第一张专辑，确认不再报「加载失败」
  await evalJs("document.querySelector('.card').click()");
  const detailOk = await waitFor("S.view==='album' && !!document.querySelector('.ahead')", 12000);
  ck(detailOk, '专辑详情页可打开（无「加载失败」）', await evalJs("document.body.innerText.indexOf('加载失败')>=0"));
  ck(errors.length===0, '全页 0 JS 异常', errors.slice(0,2));
  console.log('\n结果: '+passes+' 通过, '+fails.length+' 失败');
  try{child.kill();}catch(e){}
  process.exit(fails.length?1:0);
})().catch(e=>{console.error('FATAL '+e.message);try{child.kill();}catch(_){}process.exit(2);});
