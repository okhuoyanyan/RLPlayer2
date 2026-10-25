// uicheck_fb02.js — 261025-02 第1条（徽标六态）与第4条（采样率/位深归 PCM）CDP 验收
const { spawn } = require('child_process');
const fs = require('fs'); const path = require('path'); const os = require('os');
const URL_ = process.argv[2] || 'http://127.0.0.1:8907/';
const OUT = path.join(__dirname, '..', 'build', 'ui_fb02'); fs.mkdirSync(OUT, { recursive: true });
const EDGE = ['C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\msedge.exe','C:\\Program Files\\Microsoft\\Edge\\Application\\msedge.exe'].find(p=>fs.existsSync(p));
const PORT = 9600 + Math.floor(Math.random()*300);
const udd = path.join(os.tmpdir(), 'edge_fb02_' + process.pid + '_' + Date.now());
const child = spawn(EDGE, ['--headless=new','--disable-gpu','--no-first-run','--user-data-dir='+udd,'--remote-debugging-port='+PORT,'--window-size=1600,1000','about:blank'], {stdio:'ignore'});
const sleep = ms => new Promise(r=>setTimeout(r,ms));
let ws, id=0; const pending=new Map(); const errors=[]; const fails=[]; let passes=0;
function send(m,p){return new Promise((res,rej)=>{const i=++id;pending.set(i,{res,rej});ws.send(JSON.stringify({id:i,method:m,params:p||{}}));});}
async function evalJs(e){const r=await send('Runtime.evaluate',{expression:e,awaitPromise:true,returnByValue:true});if(r.exceptionDetails)throw new Error('JS: '+((r.exceptionDetails.exception&&r.exceptionDetails.exception.description)||r.exceptionDetails.text));return r.result&&r.result.value;}
function ck(ok,n,i){ if(ok){passes++;console.log('  [PASS] '+n);} else {fails.push(n);console.log('  [FAIL] '+n+(i!==undefined?'  '+JSON.stringify(i):''));} }
async function waitFor(e,ms){const t0=Date.now();while(Date.now()-t0<(ms||8000)){if(await evalJs('!!('+e+')'))return true;await sleep(120);}return false;}
async function targets(){for(let i=0;i<60;i++){try{const r=await fetch('http://127.0.0.1:'+PORT+'/json/list');const j=await r.json();const p=j.filter(t=>t.type==='page');if(p.length)return p;}catch(e){}await sleep(250);}throw new Error('no devtools');}
(async()=>{
  const pages=await targets(); ws=new WebSocket(pages[0].webSocketDebuggerUrl);
  await new Promise((res,rej)=>{ws.onopen=res;ws.onerror=rej;});
  ws.onmessage=ev=>{const m=JSON.parse(ev.data);if(m.id&&pending.has(m.id)){const p=pending.get(m.id);pending.delete(m.id);m.error?p.rej(new Error(JSON.stringify(m.error))):p.res(m.result);}
    else if(m.method==='Runtime.exceptionThrown')errors.push(JSON.stringify(m.params.exceptionDetails).slice(0,200));
    else if(m.method==='Runtime.consoleAPICalled'&&m.params.type==='error')errors.push((m.params.args||[]).map(a=>a.value||a.description).join(' ').slice(0,200));};
  await send('Runtime.enable'); await send('Page.enable'); await send('Page.navigate',{url:URL_});
  await sleep(2500); await waitFor("window.S && S.view",15000);
  // ---- 第1条：徽标六态 ----
  const badge = async (st) => await evalJs("(function(){" +
    "S.bpState=" + JSON.stringify(st.bp) + ";S.bpResample=" + (st.up?'true':'false') + ";S.bpDsdOut=" + (st.dsd?'true':'false') + ";" +
    "S.cur=" + JSON.stringify(st.cur||'') + ";S.bpWhy='';S.bpFallback='';" +
    "if(D('bpBadge')){var b=bpBadgeInfo();D('bpBadge').textContent=b.text;D('bpBadge').className=b.cls;return {t:b.text,c:b.cls};}return null;})()");
  let r1 = await badge({bp:'yes', up:false});
  ck(r1 && r1.t==='bit-perfect' && r1.c==='bp-yes', '① 非升频 bit-perfect', r1);
  let r2 = await badge({bp:'no', up:false});
  ck(r2 && r2.t==='非 bit-perfect' && r2.c==='bp-no', '② 非升频 非 bit-perfect', r2);
  let r3 = await badge({bp:'', up:false});
  ck(r3 && r3.t==='bit', '③ 非升频 无结论 => bit', r3);
  let r4 = await badge({bp:'no', up:true, dsd:false, cur:'x.flac'});
  ck(r4 && r4.t==='PCM-PCM 升频' && r4.c==='bp-up', '④ PCM-PCM 升频', r4);
  let r5 = await badge({bp:'no', up:true, dsd:true, cur:'x.flac'});
  ck(r5 && r5.t==='PCM-DSD 升频', '⑤ PCM-DSD 升频', r5);
  let r6 = await badge({bp:'no', up:true, dsd:true, cur:'x.dsf'});
  ck(r6 && r6.t==='DSD-DSD 升频', '⑥ DSD-DSD 升频', r6);
  // ---- 第4条：采样率/位深归属 PCM ----
  await evalJs("Array.from(document.querySelectorAll('.nav')).find(e=>e.dataset.v==='settings').click()");
  await waitFor("!!document.getElementById('sp_rate')", 8000);
  const pane = await evalJs("(function(){var e=document.getElementById('sp_rate');if(!e)return null;var p=e.closest('.spane');return p?p.getAttribute('data-g'):'none';})()");
  ck(pane==='pcm', '采样率控件位于 PCM 面板(data-g=pcm)', pane);
  const pane2 = await evalJs("(function(){var e=document.getElementById('sp_bits');if(!e)return null;var p=e.closest('.spane');return p?p.getAttribute('data-g'):'none';})()");
  ck(pane2==='pcm', '位深控件位于 PCM 面板', pane2);
  const outHas = await evalJs("(function(){var g=document.querySelector('.spane[data-g=out]');return g?!!g.querySelector('#sp_rate'):null;})()");
  ck(outHas===false || outHas===null, '输出面板里不再有 sp_rate', outHas);
  const rs = await send('Page.captureScreenshot',{format:'png'});
  fs.writeFileSync(path.join(OUT,'fb02.png'), Buffer.from(rs.data,'base64'));
  ck(errors.length===0, '全页 0 JS 异常', errors.slice(0,2));
  console.log('\n结果: '+passes+' 通过, '+fails.length+' 失败');
  if(fails.length) console.log('失败: '+fails.join(' | '));
  try{child.kill();}catch(e){}
  process.exit(fails.length?1:0);
})().catch(e=>{console.error('FATAL '+e.message);try{child.kill();}catch(_){}process.exit(2);});
