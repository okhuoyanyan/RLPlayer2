// lyr_dpr.js — 歌词滚动在【真机档】视口下的验收（默认 893x587 / DPR 1.5）
// 背景：用户真机 WebView2 视口 893x587 CSS px（窗口 1340x880 @150%），DPR=1.5；
//       旧验收 tools/t25check.js 用 1576x908 / DPR 1 → 假 PASS，真机「完全不会滚动」。
// 数据全 mock（/api/status + /api/lyrics + /api/meta + /api/seek），不依赖曲库、不出声。
// 用法：node tools/lyr_dpr.js [url] [outdir] [W] [H] [DPR]
const { spawn } = require('child_process');
const fs = require('fs'); const path = require('path'); const os = require('os');
const URL_ = process.argv[2] || 'http://127.0.0.1:8931/';
const OUT = process.argv[3] || 'D:/_tmp_lyr/shots';
const W0 = +(process.argv[4] || 893), H0 = +(process.argv[5] || 587), DPR = +(process.argv[6] || 1.5);
fs.mkdirSync(OUT, { recursive: true });
const EDGE = ['C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe','C:/Program Files/Microsoft/Edge/Application/msedge.exe'].find(p=>fs.existsSync(p));
if (!EDGE) { console.error('找不到 Edge'); process.exit(2); }
const PORT = 9600 + Math.floor(Math.random()*300);
const udd = path.join(os.tmpdir(), 'edge_lyr_' + process.pid + '_' + Date.now());
const child = spawn(EDGE, ['--headless=new','--disable-gpu','--no-first-run','--no-default-browser-check','--user-data-dir='+udd,'--remote-debugging-port='+PORT,'--window-size='+W0+','+H0,'about:blank'], { stdio: 'ignore' });
const sleep = ms => new Promise(r=>setTimeout(r,ms));
let ws, msgId = 0; const pending = new Map(); const errors = []; const results = [];
function send(m,p){ return new Promise((res,rej)=>{ const id=++msgId; pending.set(id,{res,rej}); ws.send(JSON.stringify({id,method:m,params:p||{}})); }); }
async function evalJs(e){ const r = await send('Runtime.evaluate',{expression:e,awaitPromise:true,returnByValue:true}); if (r.exceptionDetails) throw new Error('JS 异常: ' + ((r.exceptionDetails.exception && (r.exceptionDetails.exception.description||r.exceptionDetails.exception.value)) || r.exceptionDetails.text)); return r.result && r.result.value; }
async function shot(n){ const r = await send('Page.captureScreenshot',{format:'png'}); fs.writeFileSync(path.join(OUT, n + '.png'), Buffer.from(r.data,'base64')); console.log('  [shot] ' + n); }
async function waitFor(expr, ms){ const t0=Date.now(); while (Date.now()-t0 < (ms||12000)) { try { if (await evalJs('!!('+expr+')')) return true; } catch(e){} await sleep(150); } return false; }
function ck(ok, what, detail){ const line='  ' + (ok?'[PASS] ':'[FAIL] ') + what + (detail!==undefined?('  '+JSON.stringify(detail)):''); results.push({ok:!!ok,what}); console.log(line); return ok; }

// 12 行带时间轴歌词（每 20 秒一行，铺满 dur=240 ⟹ 30%/60%/90% 落在不同行上）
const LYRIC_LINES = [];
for (let i = 0; i < 12; i++) {
  const t = i * 20;
  const mm = String(Math.floor(t/60)).padStart(2,'0'), ss = String(t%60).padStart(2,'0');
  LYRIC_LINES.push('[' + mm + ':' + ss + '.00]MOCK 歌词第 ' + (i+1) + ' 行');
}
const LRC = LYRIC_LINES.join('\n');

const MOCK = [
'(function(){',
'  window.__LY = { pos: 0, dur: 240, cur: "MOCK\\\\lyric-demo.flac", playing: true, lrc: __LRC__ };',
'  var of = window.fetch;',
'  function jr(o){ return new Response(JSON.stringify(o), { status:200, headers:{"Content-Type":"application/json"} }); }',
'  window.fetch = function(u, opt){',
'    var s = String(u);',
'    if (s.indexOf("/api/status") === 0) {',
'      var st = window.__LY;',
'      return Promise.resolve(jr({ ok:true, playing:st.playing, paused:false, dur:st.dur, pos:st.pos,',
'        current:st.cur, outRate:44100, outBits:16, volDb:-60, startDb:-60,',
'        hasLlmKey:false, summaryEnabled:false, bitPerfect:"no", bitPerfectWhy:"mock" }));',
'    }',
'    if (s.indexOf("/api/lyrics") === 0) return Promise.resolve(jr({ ok:true, lrc: window.__LY.lrc }));',
'    if (s.indexOf("/api/meta") === 0) return Promise.resolve(jr({ ok:true, title:"MOCK 歌词演示", artist:"MOCK", album:"MOCK", hasCover:false }));',
'    if (s.indexOf("/api/seek") === 0) {',
'      var m = /[?&]pos=([0-9.]+)/.exec(s);',
'      if (m) window.__LY.pos = parseFloat(m[1]) * (window.__LY.dur||0);',
'      return Promise.resolve(jr({ ok:true }));',
'    }',
'    if (s.indexOf("/api/play") === 0) return Promise.resolve(jr({ ok:true }));',
'    return of.apply(this, arguments);',
'  };',
'  return 1;',
'})()'
].join('\n').replace('__LRC__', JSON.stringify(LRC));

// 页内快照：滚动容器几何 + active 行相对容器中心的偏差
const GEO_FN = 'function(id){' +
  'var box=document.getElementById(id); if(!box) return {miss:true};' +
  'var act=box.querySelector(".npline.on, .ly.on");' +
  'var br=box.getBoundingClientRect();' +
  'var o={id:id, st:Math.round(box.scrollTop*100)/100, ch:box.clientHeight, sh:box.scrollHeight,' +
  '       max:Math.round((box.scrollHeight-box.clientHeight)*100)/100, top:Math.round(br.top),' +
  '       h:Math.round(br.height), kids:box.children.length, at:(act?+act.dataset.i:-1)};' +
  'if(act){var ar=act.getBoundingClientRect(); o.actTop=Math.round(ar.top); o.actH=Math.round(ar.height);' +
  '  o.off=Math.round(((ar.top+ar.height/2)-(br.top+br.height/2))*100)/100;}' +
  'return o;}';

async function geo(id){ return await evalJs('(' + GEO_FN + ')(' + JSON.stringify(id) + ')'); }
async function setPos(pct){
  await evalJs('(function(){var d=window.__LY.dur||240; window.__LY.pos=d*' + pct + '/100; S.pos=window.__LY.pos; return 1;})()');
  await sleep(900);   // 400ms 轮询 + 平滑滚动/rAF 收敛
}

(async () => {
  let pages = [];
  for (let i=0;i<80;i++){ try { const r=await fetch('http://127.0.0.1:'+PORT+'/json/list'); const j=await r.json(); pages=j.filter(t=>t.type==='page'); if(pages.length) break; } catch(e){} await sleep(250); }
  if (!pages.length) throw new Error('DevTools 端口没起来');
  ws = new WebSocket(pages[0].webSocketDebuggerUrl);
  await new Promise((res,rej)=>{ ws.onopen=res; ws.onerror=rej; });
  ws.onmessage = ev => { const m=JSON.parse(ev.data); if (m.id && pending.has(m.id)) { const p=pending.get(m.id); pending.delete(m.id); m.error?p.rej(new Error(JSON.stringify(m.error))):p.res(m.result); } else if (m.method==='Runtime.exceptionThrown') errors.push('exception: ' + ((m.params.exceptionDetails.exception&&m.params.exceptionDetails.exception.description)||m.params.exceptionDetails.text)); };
  await send('Runtime.enable'); await send('Page.enable');
  await send('Page.addScriptToEvaluateOnNewDocument', { source: 'window.chrome={webview:{postMessage:function(m){(window.__msgs=window.__msgs||[]).push(String(m));},addEventListener:function(){},removeEventListener:function(){}}};' });
  await send('Emulation.setDeviceMetricsOverride', { width:W0, height:H0, deviceScaleFactor:DPR, mobile:false });
  await send('Page.navigate', { url: URL_ });
  await sleep(1500);
  await waitFor("window.S && document.getElementById('npLyric')", 40000);
  await evalJs(MOCK);
  const kv = await evalJs("fetch('/api/settings').then(r=>r.json()).then(j=>j.kv||{})");
  ck(kv.backend === 'null' && String(kv.volume) === '-60', '测试实例 NullSink + -60dB（不出声）', {backend:kv.backend, volume:kv.volume});
  if (!(kv.backend === 'null' && String(kv.volume) === '-60')) throw new Error('不安全：拒绝继续');

  await evalJs("document.getElementById('npOpen').click()");
  await waitFor("document.getElementById('npPage').classList.contains('on')", 6000);
  await waitFor('LY.lines.length > 5', 12000);
  const nLines = await evalJs('LY.lines.length');
  ck(nLines === 12, '歌词夹具解析出 12 行', nLines);

  const vp = await evalJs('({w:innerWidth,h:innerHeight,dpr:devicePixelRatio})');
  ck(Math.round(vp.w) === W0 && Math.round(vp.h) === H0 && Math.abs(vp.dpr - DPR) < 0.01, '视口 = ' + W0 + 'x' + H0 + ' / DPR ' + DPR, vp);

  console.log('=== #npLyric 跟随滚动（' + W0 + 'x' + H0 + ' / DPR ' + DPR + '） ===');
  const g0 = await geo('npLyric');
  console.log('  几何: ' + JSON.stringify(g0));
  ck(g0.ch > 40, '#npLyric 有可用高度（clientHeight > 40）', {ch:g0.ch, sh:g0.sh});
  ck(g0.max > 100, '#npLyric 真的可滚动（scrollHeight-clientHeight > 100）', {ch:g0.ch, sh:g0.sh, max:g0.max});

  const seq = [];
  for (const pct of [30, 60, 90]) {
    await setPos(pct);
    const g = await geo('npLyric');
    seq.push(g);
    ck(g.at >= 0 && Math.abs(g.off) <= 8, pct + '% active 行居中（|off| <= 8px）', {at:g.at, off:g.off, st:g.st});
  }
  const sts = seq.map(x=>x.st);
  ck(sts[0] !== sts[1] && sts[1] !== sts[2], '30/60/90% 的 scrollTop 各不相同（真的在跟随滚动）', {seq:sts});
  ck(seq[2].st > seq[0].st, 'scrollTop 单调前进（90% > 30%）', {a:seq[0].st, b:seq[2].st});
  await shot('lyr-np-' + W0 + 'x' + H0 + '-90pct');

  await setPos(0);
  const gA = await geo('npLyric');
  ck(Math.abs(gA.off) <= 8, '0% 首行也能居中（|off| <= 8）', {at:gA.at, off:gA.off, st:gA.st});
  await setPos(100);
  const gB = await geo('npLyric');
  ck(Math.abs(gB.off) <= 8, '100% 末行也能居中（|off| <= 8）', {at:gB.at, off:gB.off, st:gB.st});

  await setPos(20);
  const man = await evalJs('(function(){var box=document.getElementById("npLyric");box.scrollTop=0;box.dispatchEvent(new WheelEvent("wheel",{deltaY:240,bubbles:true}));return {guard:LYMAN.until>Date.now(),before:Math.round(box.scrollTop)};})()');
  // 保护期内按真实播放节奏推进（每次 +1s，不会触发「进度跳变=seek」的重定位分支）
  for (let i = 0; i < 4; i++) {
    await evalJs('(function(){window.__LY.pos += 1; S.pos = window.__LY.pos; return 1;})()');
    await sleep(420);
  }
  const man2 = await geo('npLyric');
  ck(man.guard === true, '滚轮后进入 3 秒手动保护', man);
  ck(Math.abs(man2.st - man.before) <= 4, '保护期内按正常播放推进也不抢滚动（scrollTop 不变）', {before:man.before, after:man2.st, at:man2.at});

  await evalJs('LYMAN.until=Date.now()+3000; doSeek(0.9)');
  await sleep(1800);
  const gS = await geo('npLyric');
  ck(gS.at >= 0 && Math.abs(gS.off) <= 8, 'seek 到 90% 后立刻重定位并居中', {at:gS.at, off:gS.off});

  await evalJs('(function(){window.__LY.lrc="纯文本第一行\\n纯文本第二行\\n纯文本第三行";LYRETRY.file="";loadLyrics(window.__LY.cur);return 1;})()');
  await sleep(1000);
  await setPos(80);
  await sleep(600);
  const gp = await geo('npLyric');
  ck(gp.st === 0 && gp.at === -1, '纯文本歌词不自动滚、不高亮', {st:gp.st, at:gp.at, kids:gp.kids});
  // 纯文本必须给显式提示（否则"不滚"看起来就是坏了）
  const hint = await evalJs("(function(){var h=document.querySelector('#npLyric .ly-hint');return h?h.textContent.trim():'';})()");
  ck(/纯文本歌词/.test(hint), '纯文本歌词显示「无时间轴 · 不会自动跟随」提示', hint);
  await shot('lyr-plain-hint-' + W0 + 'x' + H0);
  await evalJs('(function(){window.__LY.lrc=' + JSON.stringify(LRC) + ';LYRETRY.file="";loadLyrics(window.__LY.cur);return 1;})()');
  await sleep(1000);

  console.log('=== #lyricbox 跟随滚动（先关掉整页，右栏才有布局） ===');
  await evalJs("document.getElementById('npClose').click()");
  await waitFor("!document.getElementById('npPage').classList.contains('on')", 5000);
  await sleep(500);
  await evalJs("setRTab('l')");
  await sleep(500);
  const lb0 = await geo('lyricbox');
  console.log('  几何: ' + JSON.stringify(lb0));
  // 893 宽时 @media(max-width:1240px) 把整个右栏 #right 收掉（设计如此）⟹ 该档不判右栏
  const railVisible = await evalJs("(function(){var r=document.getElementById('right');if(!r)return false;var cs=getComputedStyle(r);return cs.display!=='none' && r.getBoundingClientRect().height>0;})()");
  if (!railVisible) {
    console.log('  [SKIP] 本档视口 ' + W0 + 'x' + H0 + ' 下 #right 被 @media(max-width:1240px) 收起（用户在此档看不到右栏歌词，判据另行在 1340 宽覆盖）');
    await shot('lyr-rail-hidden-' + W0 + 'x' + H0);
  } else {
  ck(lb0.ch > 40, '#lyricbox 有可用高度', {ch:lb0.ch, sh:lb0.sh});
  const seq2 = [];
  for (const pct of [30, 60, 90]) {
    await setPos(pct);
    const g = await geo('lyricbox');
    seq2.push(g);
    ck(g.at >= 0 && Math.abs(g.off) <= 8, '#lyricbox ' + pct + '% active 行居中（|off| <= 8px）', {at:g.at, off:g.off, st:g.st});
  }
  const sts2 = seq2.map(x=>x.st);
  ck(sts2[0] !== sts2[1] && sts2[1] !== sts2[2], '#lyricbox 30/60/90% 的 scrollTop 各不相同', {seq:sts2});
  await shot('lyr-rail-' + W0 + 'x' + H0 + '-90pct');
  }

  const fail = results.filter(r=>!r.ok);
  console.log('');
  console.log('==== ' + (results.length - fail.length) + '/' + results.length + ' PASS, ' + fail.length + ' FAIL ====');
  fail.forEach(f=>console.log('  FAIL: ' + f.what));
  console.log('ERRORS: ' + JSON.stringify(errors.slice(0,5)));
  try { child.kill(); } catch(e){}
  process.exit(fail.length ? 1 : 0);
})().catch(e=>{ console.error('FATAL', e); try { child.kill(); } catch(_){} process.exit(2); });
