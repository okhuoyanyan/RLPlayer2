// peqcheck.js — 20 段 PEQ 验收（261024-03 #5：文字对齐 + HIFI 交互）
// 判据：
//   1) 对齐：表头每列中心 vs 行内控件中心 <= 2px（893x587/DPR1.5 与 1576x908/DPR1 两档都要过）；
//      数字列右对齐 + tabular-nums；控件高度统一 24px；行数 20；曲线节点 20 个。
//   2) 交互：拖拽节点 -> /api/peq 的 bands 参数确实变化（拦截 POST 断言）；Shift+拖/滚轮 -> Q 变；
//      双击 -> 该段复位 0dB/Q1.41；选中行 ↑ -> 增益 +0.1 并下发；自动前级 -> preamp = -ceil(峰值)。
// 不出声：只读设置页 + 后端 null/-60（本脚本不动播放）。
const { spawn } = require('child_process');
const fs = require('fs'); const path = require('path'); const os = require('os');
const URL_ = process.argv[2] || 'http://127.0.0.1:8931/';
const OUT = process.argv[3] || 'D:/_tmp_lyr/peqshots';
fs.mkdirSync(OUT, { recursive: true });
const EDGE = ['C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe','C:/Program Files/Microsoft/Edge/Application/msedge.exe'].find(p=>fs.existsSync(p));
if (!EDGE) { console.error('找不到 Edge'); process.exit(2); }
const PORT = 9700 + Math.floor(Math.random()*200);
const udd = path.join(os.tmpdir(), 'edge_peq_' + process.pid + '_' + Date.now());
const child = spawn(EDGE, ['--headless=new','--disable-gpu','--no-first-run','--no-default-browser-check','--user-data-dir='+udd,'--remote-debugging-port='+PORT,'--window-size=1576,908','about:blank'], { stdio: 'ignore' });
const sleep = ms => new Promise(r=>setTimeout(r,ms));
let ws, msgId = 0; const pending = new Map(); const errors = []; const results = [];
function send(m,p){ return new Promise((res,rej)=>{ const id=++msgId; pending.set(id,{res,rej}); ws.send(JSON.stringify({id,method:m,params:p||{}})); }); }
async function evalJs(e){ const r = await send('Runtime.evaluate',{expression:e,awaitPromise:true,returnByValue:true}); if (r.exceptionDetails) throw new Error('JS 异常: ' + ((r.exceptionDetails.exception && (r.exceptionDetails.exception.description||r.exceptionDetails.exception.value)) || r.exceptionDetails.text)); return r.result && r.result.value; }
async function shot(n){ const r = await send('Page.captureScreenshot',{format:'png'}); fs.writeFileSync(path.join(OUT, n + '.png'), Buffer.from(r.data,'base64')); console.log('  [shot] ' + n); }
async function waitFor(expr, ms){ const t0=Date.now(); while (Date.now()-t0 < (ms||12000)) { try { if (await evalJs('!!('+expr+')')) return true; } catch(e){} await sleep(150); } return false; }
function ck(ok, what, detail){ const line='  ' + (ok?'[PASS] ':'[FAIL] ') + what + (detail!==undefined?('  '+JSON.stringify(detail)):''); results.push({ok:!!ok,what}); console.log(line); return ok; }
async function setSize(W,H,dsf){ await send('Emulation.setDeviceMetricsOverride',{width:W,height:H,deviceScaleFactor:dsf||1,mobile:false}); await sleep(350); }
async function clickAt(x,y,mods){ await send('Input.dispatchMouseEvent',{type:'mouseMoved',x,y,modifiers:mods||0,button:'none'}); await send('Input.dispatchMouseEvent',{type:'mousePressed',x,y,button:'left',clickCount:1,modifiers:mods||0}); }
async function releaseAt(x,y,mods){ await send('Input.dispatchMouseEvent',{type:'mouseReleased',x,y,button:'left',clickCount:1,modifiers:mods||0}); }
async function dragAt(x,y,mods){ await send('Input.dispatchMouseEvent',{type:'mouseMoved',x,y,button:'left',modifiers:mods||0}); await sleep(60); }
async function wheelAt(x,y,dy){ await send('Input.dispatchMouseEvent',{type:'mouseWheel',x,y,deltaX:0,deltaY:dy,button:'none'}); }

(async () => {
  let pages = [];
  for (let i=0;i<80;i++){ try { const r=await fetch('http://127.0.0.1:'+PORT+'/json/list'); const j=await r.json(); pages=j.filter(t=>t.type==='page'); if(pages.length) break; } catch(e){} await sleep(250); }
  if (!pages.length) throw new Error('DevTools 端口没起来');
  ws = new WebSocket(pages[0].webSocketDebuggerUrl);
  await new Promise((res,rej)=>{ ws.onopen=res; ws.onerror=rej; });
  ws.onmessage = ev => { const m=JSON.parse(ev.data); if (m.id && pending.has(m.id)) { const p=pending.get(m.id); pending.delete(m.id); m.error?p.rej(new Error(JSON.stringify(m.error))):p.res(m.result); } else if (m.method==='Runtime.exceptionThrown') errors.push('exception: ' + ((m.params.exceptionDetails.exception&&m.params.exceptionDetails.exception.description)||m.params.exceptionDetails.text)); };
  await send('Runtime.enable'); await send('Page.enable');
  await send('Page.addScriptToEvaluateOnNewDocument', { source: 'window.chrome={webview:{postMessage:function(){},addEventListener:function(){},removeEventListener:function(){}}};' });
  await setSize(1576, 908, 1);
  await send('Page.navigate', { url: URL_ });
  await sleep(1500);
  await waitFor("window.S && document.getElementById('setpage')", 40000);
  const kv = await evalJs("fetch('/api/settings').then(r=>r.json()).then(j=>j.kv||{})");
  ck(kv.backend === 'null' && String(kv.volume) === '-60', '测试实例 NullSink + -60dB（不出声）', {backend:kv.backend, volume:kv.volume});
  if (!(kv.backend === 'null' && String(kv.volume) === '-60')) throw new Error('不安全：拒绝继续');

  // 拦截 /api/peq 的 POST，记录 body（判据要证明"拖拽后发给后端的 bands 真变了"）
  await evalJs('(function(){ if(window.__peqHooked) return 1; window.__peqHooked=1; window.__peqPosts=[];' +
    'var of=window.fetch; window.fetch=function(u,o){ var s=String(u);' +
    ' if(s.indexOf("/api/peq")===0 && o && o.body) window.__peqPosts.push({url:s, body:String(o.body), t:Date.now()});' +
    ' return of.apply(this,arguments); }; return 1;})()');

  // 归零，保证判据可复现
  await evalJs("fetch('/api/peq/preset',{method:'POST',body:'action=reset&name=Flat'}).then(r=>r.json()).then(j=>!!j.ok)");
  await evalJs("openSettingsTab('eq')");
  await waitFor("document.querySelectorAll('#peqRows .peqr').length === 20", 20000);
  await sleep(600);
  ck(await evalJs("document.querySelectorAll('#peqRows .peqr').length") === 20, '20 行频段渲染完成');
  ck(await evalJs("document.querySelectorAll('#peqCurve circle.peqnode').length") === 20, '曲线上 20 个频段节点');

  const ALIGN = 'function(){' +
    'var out={cols:[],rows:0,sizes:[],aligns:[]};' +
    'var hd=document.querySelector("#setpage .peqhd"); if(!hd) return {miss:true};' +
    'var hs=hd.querySelectorAll("span"); var r0=document.querySelector("#peqRows .peqr");' +
    'var cells=r0.querySelectorAll(".peqc");' +
    'for(var i=0;i<cells.length;i++){' +
    ' var ctl=cells[i].querySelector("input,select"); if(!ctl) continue;' +
    ' var hr=hs[i+1].getBoundingClientRect(), cr=ctl.getBoundingClientRect();' +
    ' out.cols.push({col:hs[i+1].textContent.trim(), dh:Math.round(((hr.left+hr.width/2)-(cr.left+cr.width/2))*100)/100, w:Math.round(cr.width), h:Math.round(cr.height)});' +
    ' if(ctl.type!=="checkbox") out.sizes.push(Math.round(cr.height));' +
    ' out.aligns.push(getComputedStyle(ctl).textAlign);' +
    '}' +
    'out.rows=document.querySelectorAll("#peqRows .peqr").length;' +
    'return out;}';

  const bands = [[893,587,1.5],[1576,908,1]];
  for (const [W,H,D] of bands) {
    await setSize(W,H,D);
    await evalJs("openSettingsTab('eq')");
    await sleep(700);
    const tag = ' @' + W + 'x' + H;
    const a = await evalJs('(' + ALIGN + ')()');
    const maxDev = Math.max(...a.cols.map(c=>Math.abs(c.dh)));
    console.log('  列偏差' + tag + ': ' + JSON.stringify(a.cols));
    ck(maxDev <= 2, '表头列中心与控件中心偏差 <= 2px' + tag, {max:maxDev});
    ck(a.sizes.every(h => h === 24), '控件高度统一 24px' + tag, a.sizes);
    ck(a.cols[0].col === '频率' && a.aligns[0] === 'right' && a.aligns[1] === 'right' && a.aligns[2] === 'right',
       '频率/增益/Q 三列数字右对齐' + tag, a.aligns);
    ck(a.rows === 20, '20 行频段' + tag, a.rows);
    // 截图前把表格滚到视口中间（量尺已经在上面做完，滚动不影响 rect 差值）
    await evalJs("(function(){var r=document.querySelector('#peqRows'); if(r&&r.scrollIntoView) r.scrollIntoView({block:'center'}); return 1;})()");
    await sleep(400);
    await shot('peq-' + W + 'x' + H);
  }

  // ===== 交互（在 1576x908 下做，坐标更宽裕） =====
  await setSize(1576, 908, 1);
  await evalJs("openSettingsTab('eq')");
  await sleep(700);
  const nodePos = (i) => evalJs('(function(){var c=document.querySelector("#peqCurve circle.peqnode[data-i=\'' + i + '\']");' +
    'if(!c)return null;var r=c.getBoundingClientRect();return {x:r.left+r.width/2,y:r.top+r.height/2};})()');
  const lastPost = () => evalJs('(function(){var a=window.__peqPosts||[];return a.length?a[a.length-1].body:null;})()');
  const bandOf = (body, i) => { if(!body) return null; const m = /bands=([^&]*)/.exec(body); if(!m) return null; const s = decodeURIComponent(m[1]); const parts = s.split(';'); return parts.find(p=>p.startsWith(i+',')) || null; };
  const P = await nodePos(5);
  ck(!!P, '第 5 段节点可定位', P);

  // (1) 拖拽：右移 + 下移（频率升、增益降）
  await evalJs('window.__peqPosts = []; 1');
  await clickAt(P.x, P.y); await dragAt(P.x + 45, P.y + 14); await dragAt(P.x + 90, P.y + 26); await releaseAt(P.x + 90, P.y + 26);
  await sleep(900);
  let body = await lastPost();
  let b5 = bandOf(body, 5);
  ck(!!b5, '拖拽后确实向 /api/peq 发了 bands' , b5);
  ck(b5 && /^5,?(PK|LS|HS),-?[0-9.]+,(-?[0-9.]+|off),-?[0-9.]+$/.test(b5), 'bands 里第 5 段格式正确', b5);
  const b5f = b5 ? parseFloat(b5.split(',')[2]) : 0;
  const b5g = b5 ? parseFloat(b5.split(',')[3]) : 0;
  ck(b5f > 30 && b5g < -0.2, '拖拽改变了该段频率(升)与增益(降)', {freq:b5f, gain:b5g});

  // (2) Shift+拖 = 改 Q
  await evalJs('window.__peqPosts = []; 1');
  const P2 = await nodePos(5);
  await clickAt(P2.x, P2.y, 8); await dragAt(P2.x, P2.y - 60, 8); await releaseAt(P2.x, P2.y - 60, 8);
  await sleep(900);
  body = await lastPost(); b5 = bandOf(body, 5);
  const q5 = b5 ? parseFloat(b5.split(',')[4]) : 1.41;
  ck(q5 > 1.5, 'Shift+拖 改的是 Q（升）', {q:q5});

  // (3) 滚轮 = 改 Q（往回转）
  await evalJs('window.__peqPosts = []; 1');
  const P3 = await nodePos(5);
  await wheelAt(P3.x, P3.y, 300); await sleep(700);
  body = await lastPost(); b5 = bandOf(body, 5);
  const q5b = b5 ? parseFloat(b5.split(',')[4]) : 0;
  ck(q5b > 0 && Math.abs(q5b - q5) > 0.01, '滚轮改 Q（与 Shift 拖不同值）', {before:q5, after:q5b});

  // (4) 双击复位
  await evalJs('window.__peqPosts = []; 1');
  await evalJs('window.__dbl=0; document.querySelector("#peqCurve svg").addEventListener("dblclick", function(){window.__dbl++;}); 1');
  const P4 = await nodePos(5);
  await send('Input.dispatchMouseEvent',{type:'mousePressed',x:P4.x,y:P4.y,button:'left',clickCount:1});
  await send('Input.dispatchMouseEvent',{type:'mouseReleased',x:P4.x,y:P4.y,button:'left',clickCount:1});
  await send('Input.dispatchMouseEvent',{type:'mousePressed',x:P4.x,y:P4.y,button:'left',clickCount:2});
  await send('Input.dispatchMouseEvent',{type:'mouseReleased',x:P4.x,y:P4.y,button:'left',clickCount:2});
  await sleep(900);
  const dblSeen = await evalJs('window.__dbl||0');
  ck(dblSeen > 0, '双击事件到达曲线（dblclick 派发正常）', dblSeen);
  body = await lastPost(); b5 = bandOf(body, 5);
  const g5 = b5 ? parseFloat(b5.split(',')[3]) : 9, q5c = b5 ? parseFloat(b5.split(',')[4]) : 0;
  ck(Math.abs(g5) < 0.05 && Math.abs(q5c - 1.41) < 0.02, '双击节点 = 该段复位（0.0 dB / Q 1.41）', {gain:g5, q:q5c});

  // (5) 键盘：选中第 3 行后 ↑ 增益 +0.1
  await evalJs('window.__peqPosts = []; (function(){var tr=document.querySelector("#peqRows .peqr[data-i=\'3\']");tr.click();tr.focus();return 1;})()');
  await sleep(200);
  await send('Input.dispatchKeyEvent',{type:'rawKeyDown',key:'ArrowUp',code:'ArrowUp',windowsVirtualKeyCode:38,nativeVirtualKeyCode:38});
  await send('Input.dispatchKeyEvent',{type:'keyUp',key:'ArrowUp',code:'ArrowUp',windowsVirtualKeyCode:38,nativeVirtualKeyCode:38});
  await sleep(800);
  body = await lastPost(); const b3 = bandOf(body, 3);
  const g3 = b3 ? parseFloat(b3.split(',')[3]) : 0;
  ck(Math.abs(g3 - 0.1) < 0.02, '选中行 ↑ = 增益 +0.1 dB 并热更新', {gain:g3});

  // (6) 自动前级：先把第 8 段设 +6 dB，再点按钮
  await evalJs('(function(){var tr=document.querySelector("#peqRows .peqr[data-i=\'8\']");var g=tr.querySelector(".peqG");' +
    'g.value="6.0"; g.dispatchEvent(new Event("input",{bubbles:true})); return 1;})()');
  await sleep(900);
  await evalJs('window.__peqPosts = []; document.getElementById("peqAutoPre").click(); 1');
  await sleep(900);
  body = await lastPost();
  const pre = body ? parseFloat((/preamp=([-0-9.]+)/.exec(body)||[0,0])[1]) : 0;
  ck(Math.abs(pre + 6) < 0.01, '自动前级 = -ceil(曲线峰值)（+6 dB → -6 dB）', {preamp:pre, body:(body||'').slice(0,80)});
  ck(await evalJs('String(document.getElementById("peqPreamp").value)') === '-6', '前级输入框显示 -6');
  await shot('peq-after-actions');

  const fail = results.filter(r=>!r.ok);
  console.log('');
  console.log('==== ' + (results.length - fail.length) + '/' + results.length + ' PASS, ' + fail.length + ' FAIL ====');
  fail.forEach(f=>console.log('  FAIL: ' + f.what));
  if (errors.length) console.log('ERRORS: ' + JSON.stringify(errors.slice(0,4)));
  try { child.kill(); } catch(e){}
  process.exit(fail.length ? 1 : 0);
})().catch(e=>{ console.error('FATAL', e); try { child.kill(); } catch(_){} process.exit(2); });
