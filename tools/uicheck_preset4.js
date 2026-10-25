// uicheck_preset4.js — 261026-01：4 档预设 + 卡片在「输出」面板
const { spawn } = require('child_process');
const fs = require('fs'); const path = require('path'); const os = require('os');
const URL_ = process.argv[2] || 'http://127.0.0.1:8907/';
const EDGE = ['C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\msedge.exe','C:\\Program Files\\Microsoft\\Edge\\Application\\msedge.exe'].find(p=>fs.existsSync(p));
const PORT = 9700 + Math.floor(Math.random()*200);
const udd = path.join(os.tmpdir(), 'edge_p4_' + process.pid + '_' + Date.now());
const child = spawn(EDGE, ['--headless=new','--disable-gpu','--no-first-run','--user-data-dir='+udd,'--remote-debugging-port='+PORT,'--window-size=1600,1000','about:blank'], {stdio:'ignore'});
const sleep = ms => new Promise(r=>setTimeout(r,ms));
let ws, id=0; const pending=new Map(); const errors=[]; const fails=[]; let passes=0;
function send(m,p){return new Promise((res,rej)=>{const i=++id;pending.set(i,{res,rej});ws.send(JSON.stringify({id:i,method:m,params:p||{}}));});}
async function evalJs(e){const r=await send('Runtime.evaluate',{expression:e,awaitPromise:true,returnByValue:true});if(r.exceptionDetails)throw new Error('JS: '+r.exceptionDetails.text);return r.result&&r.result.value;}
function ck(ok,n,i){if(ok){passes++;console.log('  [PASS] '+n);}else{fails.push(n);console.log('  [FAIL] '+n+(i!==undefined?'  '+JSON.stringify(i):''));}}
async function waitFor(e,ms){const t0=Date.now();while(Date.now()-t0<(ms||8000)){if(await evalJs('!!('+e+')'))return true;await sleep(120);}return false;}
async function targets(){for(let i=0;i<60;i++){try{const r=await fetch('http://127.0.0.1:'+PORT+'/json/list');const j=await r.json();const p=j.filter(t=>t.type==='page');if(p.length)return p;}catch(e){}await sleep(250);}throw new Error('no devtools');}
(async()=>{
  const pages=await targets(); ws=new WebSocket(pages[0].webSocketDebuggerUrl);
  await new Promise((res,rej)=>{ws.onopen=res;ws.onerror=rej;});
  ws.onmessage=ev=>{const m=JSON.parse(ev.data);if(m.id&&pending.has(m.id)){const p=pending.get(m.id);pending.delete(m.id);m.error?p.rej(new Error(JSON.stringify(m.error))):p.res(m.result);}
    else if(m.method==='Runtime.exceptionThrown')errors.push(JSON.stringify(m.params.exceptionDetails).slice(0,200));};
  await send('Runtime.enable'); await send('Page.enable'); await send('Page.navigate',{url:URL_});
  await sleep(2500); await waitFor("window.S && S.view",15000);
  await evalJs("Array.from(document.querySelectorAll('.nav')).find(e=>e.dataset.v==='settings').click()");
  await waitFor("!!document.getElementById('presetCard')", 8000);
  for (const bid of ['presetBitPerfect','presetPcm','presetDsd','presetPcm2Dsd']) {
    ck(await evalJs("!!document.getElementById('"+bid+"')"), '按钮存在 #'+bid);
  }
  // 卡片必须在「输出」面板里（data-g=out），不能在所有页签外面
  const inOut = await evalJs("(function(){var c=document.getElementById('presetCard');if(!c)return null;var p=c.closest('.spane');return p?p.getAttribute('data-g'):'none';})()");
  ck(inOut === 'out', '预设卡片位于「输出」面板', inOut);
  // 点 PCM→DSD：应发出 profile=pcm2dsd
  await evalJs("document.getElementById('presetPcm2Dsd').click()");
  await sleep(2500);
  const st = await evalJs("(function(){return {rate:D('sRate')?D('sRate').value:null, filt:D('sFilt')?D('sFilt').value:null};})()");
  ck(st && st.filt && /tight/.test(st.filt), 'PCM→DSD 档写入 tight 滤波', st && st.filt);
  ck(errors.length===0, '全页 0 JS 异常', errors.slice(0,2));
  const shot = await send('Page.captureScreenshot',{format:'png'});
  fs.writeFileSync(path.join(__dirname,'..','build','ui_preset','preset4.png'), Buffer.from(shot.data,'base64'));
  console.log('\n结果: '+passes+' 通过, '+fails.length+' 失败');
  try{child.kill();}catch(e){}
  process.exit(fails.length?1:0);
})().catch(e=>{console.error('FATAL '+e.message);try{child.kill();}catch(_){}process.exit(2);});
