// t25check.js — 261024-02 #2/#3/#4/#5 的 CDP 验收（无声：backend=null / volume=-60 / 假渲染器）
//
// 判据（全部打印 [PASS]/[FAIL]，exit 0 = 全过）：
//  #4 歌词：LRC 解析（[offset]/一行多时间戳/[mm:ss.xxx]）+ pos 0/30/60/90% 的 active 行与居中偏移
//            + 纯文本歌词不自动滚 + 手动滚动 3 秒不抢 + seek 立刻重定位 + 不遮进度条（两档尺寸）
//  #3 时长/曲目：专辑总时长 = Σ 本地曲目；页面曲目行数 = 本地曲目数、逐行时长正确；
//            AI 的「时长/曲目列表」不被采信；歌曲页时长 = 文件时长；抽屉「专辑」用本地曲目表
//  #2 外链：引用 chip / 关联字段里的 URL → postMessage('openurl:<url>')；javascript:/file: 一律拒绝
//  #5 推送：单曲页按钮 → backend=upnp 落盘 → /api/play 路由到 CastManager → 假渲染器收到 AVTransport；
//            三处 UI（单曲页/底栏/流媒体页）状态一致；再点停止 → 恢复推送前后端；输出页无投放块
//
// ⚠️ 数据来源口径：资料内容用页内契约 mock（拦截 fetch 返回 fields/citations），零真实 AI 调用。
//    UPnP 用 tools/mock_upnp_renderer.js（不出声、不碰真实数播）。真实后端联调由 Lead 侧做。
const { spawn } = require('child_process');
const fs = require('fs'); const path = require('path'); const os = require('os');
const URL_ = process.argv[2] || 'http://127.0.0.1:8931/';
const OUT = process.argv[3] || 'D:/_tmp_lead/t25-np/shots';
const LYRICS_TRACK = process.argv[4] || '';
fs.mkdirSync(OUT, { recursive: true });
const MOCK_PORT = 49199;
const MOCK = 'http://127.0.0.1:' + MOCK_PORT;
const MOCK_DEV = MOCK + '/description.xml';
const mockProc = spawn(process.execPath, [path.join(__dirname, 'mock_upnp_renderer.js'), String(MOCK_PORT)], { stdio: 'ignore' });
const EDGE = ['C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe','C:/Program Files/Microsoft/Edge/Application/msedge.exe'].find(p=>fs.existsSync(p));
if (!EDGE) { console.error('找不到 Edge'); process.exit(2); }
const PORT = 9500 + Math.floor(Math.random()*300);
const udd = path.join(os.tmpdir(), 'edge_t25_' + process.pid + '_' + Date.now());
const child = spawn(EDGE, ['--headless=new','--disable-gpu','--no-first-run','--no-default-browser-check','--user-data-dir='+udd,'--remote-debugging-port='+PORT,'--window-size=1576,908','about:blank'], { stdio: 'ignore' });
const sleep = ms => new Promise(r=>setTimeout(r,ms));
let ws, msgId = 0; const pending = new Map(); const errors = []; const results = [];
function send(m,p){ return new Promise((res,rej)=>{ const id=++msgId; pending.set(id,{res,rej}); ws.send(JSON.stringify({id,method:m,params:p||{}})); }); }
async function evalJs(e){ const r = await send('Runtime.evaluate',{expression:e,awaitPromise:true,returnByValue:true}); if (r.exceptionDetails) throw new Error('JS 异常: ' + ((r.exceptionDetails.exception && (r.exceptionDetails.exception.description||r.exceptionDetails.exception.value)) || r.exceptionDetails.text)); return r.result && r.result.value; }
async function shot(n){ const r = await send('Page.captureScreenshot',{format:'png'}); fs.writeFileSync(path.join(OUT, n + '.png'), Buffer.from(r.data,'base64')); console.log('  [shot] ' + n); }
async function waitFor(expr, ms){ const t0=Date.now(); while (Date.now()-t0 < (ms||10000)) { try { if (await evalJs('!!('+expr+')')) return true; } catch(e){} await sleep(150); } return false; }
function ck(ok, what, detail){ const line='  ' + (ok?'[PASS] ':'[FAIL] ') + what + (detail!==undefined?('  '+JSON.stringify(detail)):''); results.push({ok:!!ok,what}); console.log(line); return ok; }
async function setSize(W,H,dsf){ await send('Emulation.setDeviceMetricsOverride',{width:W,height:H,deviceScaleFactor:dsf||1,mobile:false}); await sleep(320); }
function fmtLike(x) {
  x = Math.max(0, Math.round(x || 0));
  var h = Math.floor(x/3600), m = Math.floor((x%3600)/60), s = x%60;
  var ss = (s<10?'0':'')+s;
  if (h > 0) return h + ':' + (m<10?'0':'')+m + ':' + ss;
  return m + ':' + ss;
}
// 页内契约 mock（fields/citations/设备列表）放在 tools/mock_t25.js，避免生成器里嵌套引号
const PAGE_MOCK = fs.readFileSync(path.join(__dirname, 'mock_t25.js'), 'utf8')
  .replace(/__MOCK_PORT__/g, String(MOCK_PORT))
  .replace(/__MOCK_DEV__/g, MOCK_DEV)
  .replace(/__MOCK_BASE__/g, MOCK);

(async () => {
  for (let i=0;i<50;i++){ try { if ((await fetch(MOCK + '/__state')).ok) break; } catch(e){} await sleep(200); }
  let pages = [];
  for (let i=0;i<80;i++){ try { const r=await fetch('http://127.0.0.1:'+PORT+'/json/list'); const j=await r.json(); pages=j.filter(t=>t.type==='page'); if(pages.length) break; } catch(e){} await sleep(250); }
  if (!pages.length) throw new Error('DevTools 端口没起来');
  ws = new WebSocket(pages[0].webSocketDebuggerUrl);
  await new Promise((res,rej)=>{ ws.onopen=res; ws.onerror=rej; });
  ws.onmessage = ev => { const m=JSON.parse(ev.data); if (m.id && pending.has(m.id)) { const p=pending.get(m.id); pending.delete(m.id); m.error?p.rej(new Error(JSON.stringify(m.error))):p.res(m.result); } else if (m.method==='Runtime.exceptionThrown') errors.push('exception: ' + ((m.params.exceptionDetails.exception&&m.params.exceptionDetails.exception.description)||m.params.exceptionDetails.text)); else if (m.method==='Runtime.consoleAPICalled' && m.params.type==='error') errors.push('console.error: ' + m.params.args.map(a=>a.value||a.description||'').join(' ')); };
  await send('Runtime.enable'); await send('Page.enable');
  // 桥桩：postMessage 记录到 __msgs；addEventListener 给个空实现（页面会监听宿主消息）
  await send('Page.addScriptToEvaluateOnNewDocument', { source: 'window.chrome={webview:{postMessage:function(m){(window.__msgs=window.__msgs||[]).push(String(m));},addEventListener:function(){},removeEventListener:function(){}}};' });
  await setSize(1576, 908);
  await send('Page.navigate', { url: URL_ });
  await sleep(1200);
  await waitFor("document.querySelectorAll('.card').length > 0", 60000);
  let kv = await evalJs("fetch('/api/settings').then(r=>r.json()).then(j=>j.kv||{})");
  ck(kv.backend === 'null' && String(kv.volume) === '-60', '测试实例是 NullSink 且音量 -60（不发声）', { backend: kv.backend, volume: kv.volume });
  if (!(kv.backend === 'null' && String(kv.volume) === '-60')) throw new Error('不安全：拒绝继续');
  await evalJs(PAGE_MOCK);
  await evalJs("loadSettings()"); await sleep(600);

  // ---------- 夹具 ----------
  const albs = await evalJs("fetch('/api/lib/albums').then(r=>r.json()).then(j=>j.albums||[])");
  const big = albs.slice().sort((a,b)=>b.n-a.n)[0];
  const alb = await evalJs("fetch('/api/lib/album?k=' + encodeURIComponent(" + JSON.stringify(big.k) + ")).then(r=>r.json())");
  console.log('  夹具专辑: ' + alb.name + ' / ' + alb.tracks.length + ' 首 / dur=' + alb.dur);
  let useAlb = alb, track = alb.tracks[0];
  for (const cand of albs.slice().sort((x, y) => y.n - x.n)) {
    const d = await evalJs("fetch('/api/lib/album?k=' + encodeURIComponent(" + JSON.stringify(cand.k) + ")).then(r=>r.json())");
    if (!d || !d.tracks || !d.tracks.length) continue;
    const mm = await evalJs("fetch('/api/meta?file=' + encodeURIComponent(" + JSON.stringify(d.tracks[0].path) + ")).then(r=>r.json())");
    if (mm && mm.album && mm.artist && d.tracks.length >= 4) { useAlb = d; track = d.tracks[0]; break; }
  }
  console.log('  #3 夹具专辑: ' + useAlb.name + ' / ' + useAlb.tracks.length + ' 首（首曲标签 album=' + useAlb.name + '）');

  // ================= #4 歌词 =================
  console.log('=== #4 歌词滚动（解析 + 同步 + 居中 + 手动保护） ===');
  const parseCase = await evalJs('(function(){var a=parseLrc("[offset:500]\\n[00:10.00]A\\n[00:20.50]B");var b=parseLrc("[00:01.00][00:05.00]重复行\\n[01:02.345]毫秒");var c=parseLrc("纯文本第一行\\n纯文本第二行");return {offsetA:a.length?a[0].t:-1,multi:b.length,ms:b.length>2?b[2].t:-1,plainTimed:c.length?c[0].t:null,plainN:c.length};})()');
  ck(Math.abs(parseCase.offsetA - 9.5) < 0.01, '[offset:500] 让时间戳提前 0.5s（10.0 → 9.5）', parseCase.offsetA);
  ck(parseCase.multi === 3, '一行多时间戳展开成多行（1+2=3）', parseCase.multi);
  ck(Math.abs(parseCase.ms - 62.345) < 0.01, '[mm:ss.xxx] 按毫秒解析（62.345）', parseCase.ms);
  ck(parseCase.multi === 3, '一行多时间戳 + 排序后共 3 行', parseCase.multi);
  ck(parseCase.plainN === 2 && parseCase.plainTimed === -1, '纯文本歌词保留行且 t=-1（不自动滚）', parseCase);

  const lyrPath = LYRICS_TRACK || track.path;
  await evalJs('playPath(' + JSON.stringify(lyrPath) + ')');
  await waitFor('S.playing', 20000);
  await sleep(1500);
  await evalJs("document.getElementById('npOpen').click()");
  await waitFor("document.getElementById('npPage').classList.contains('on')", 6000);
  await sleep(1200);
  const nLines = await evalJs('LY.lines.length');
  ck(nLines > 5, '歌词夹具解析出多行（' + nLines + ' 行）', nLines);
  ck(await evalJs('LY.timed === true'), '有时间戳 ⟹ LY.timed=true');

  for (const [W,H] of [[1576,908],[1024,640]]) {
    const tag = ' @' + W + 'x' + H;
    await setSize(W,H); await sleep(400);
    for (const pct of [0, 30, 60, 90]) {
      // 冻结进度：/api/status 每 400ms 会把 S.pos 覆盖回真实播放位置，不冻结就测不到我们设的 pos
      const r = await evalJs('(function(){var dur=S.dur||1;var p=dur*' + pct + '/100;window.__freezePos=p;window.__freezeDur=dur;S.pos=p;LYMAN.until=0;LY.at=-1;renderNpLyric();var box=document.getElementById("npLyric");var exp=0;for(var i=0;i<LY.lines.length;i++){if(LY.lines[i].t<=p+0.05)exp=i;else break;}var on=box.querySelector(".npline.on");return {pos:Math.round(p),idx:on?+on.dataset.i:-1,exp:exp,lines:LY.lines.length};})()');
      await sleep(700);
      // 偏移在 Node 侧算（避免把 NaN 之类"不可序列化值"从 CDP 传回来时变成 undefined）
      let after = null;
      for (let t = 0; t < 8; t++) {
        const geo2 = await evalJs('(function(){var box=document.getElementById("npLyric");var on=box.querySelector(".npline.on");if(!on)return {has:false};var br=box.getBoundingClientRect(),er=on.getBoundingClientRect();return {has:true,et:er.top,eh:er.height,bt:br.top,bh:br.height};})()');
        if (geo2 && geo2.has) {
          const off = (geo2.et + geo2.eh / 2) - (geo2.bt + geo2.bh / 2);
          if (isFinite(off)) { after = Math.round(off); if (Math.abs(after) <= 12) break; }
        }
        await sleep(250);
      }
      ck(r.idx === r.exp, '#4 ' + pct + '% 时高亮行 = 时间戳对应的行' + tag, { idx: r.idx, exp: r.exp, pos: r.pos });
      ck(after !== null && Math.abs(after) <= 12, '#4 ' + pct + '% 时 active 行垂直居中（偏移 ≤ 12px）' + tag, { off: after });
    }
    const geo = await evalJs('(function(){var a=document.getElementById("npLyric").getBoundingClientRect(),b=document.getElementById("npSeek").getBoundingClientRect();return {gap:Math.round(b.top-a.bottom)};})()');
    ck(geo.gap >= 6, '#4 歌词区不遮进度条（间距 >= 6px）' + tag, geo);
    await shot('t25-lyrics-' + W + 'x' + H);
  }

  await evalJs('(function(){ delete window.__freezePos; delete window.__freezeDur; return 1; })()');
  const manual = await evalJs('(function(){var box=document.getElementById("npLyric");var p=(S.dur||1)*0.2;window.__freezePos=p;S.pos=p;LY.at=-1;renderNpLyric();box.dispatchEvent(new WheelEvent("wheel",{deltaY:300}));var guard=LYMAN.until>Date.now();var before=box.scrollTop;var p2=(S.pos||0)+0.5;window.__freezePos=p2;S.pos=p2;LY.at=-1;renderNpLyric();return {guard:guard,before:Math.round(before),after:Math.round(box.scrollTop)};})()');
  await sleep(400);
  ck(manual.guard === true, '#4 滚轮后进入 3 秒手动保护', manual);
  ck(Math.abs(manual.after - manual.before) <= 4, '#4 保护期内不抢滚动（scrollTop 基本不变）', manual);
  await evalJs('(function(){LYMAN.until=0;var p=(S.dur||1)*0.1;window.__freezePos=p;S.pos=p;LY.at=-1;renderNpLyric();return 1;})()');
  await sleep(600);
  ck(await evalJs('LYMAN.until === 0'), '#4 解除保护后恢复自动跟随');
  // seek 端到端：真发 /api/seek，然后看高亮行是否跟到 90% 附近、并且仍在居中（seek 立刻重定位）
  await evalJs('(function(){ delete window.__freezePos; delete window.__freezeDur; LYMAN.until=Date.now()+3000; doSeek(0.9); return 1; })()');
  await sleep(1800);
  let seekChk = null;
  for (let t = 0; t < 8; t++) {
    seekChk = await evalJs('(function(){var box=document.getElementById("npLyric");var on=box.querySelector(".npline.on");var br=box.getBoundingClientRect();var geo=on?{has:true,et:on.getBoundingClientRect().top,eh:on.getBoundingClientRect().height,bt:br.top,bh:br.height}:{has:false};var exp=0;for(var i=0;i<LY.lines.length;i++){if(LY.lines[i].t<=S.pos+0.05)exp=i;else break;}return {pos:Math.round(S.pos),idx:on?+on.dataset.i:-1,exp:exp,geo:geo,guard:LYMAN.until};})()');
    if (seekChk && seekChk.geo && seekChk.geo.has) {
      const off = (seekChk.geo.et + seekChk.geo.eh / 2) - (seekChk.geo.bt + seekChk.geo.bh / 2);
      seekChk.off = isFinite(off) ? Math.round(off) : null;
      if (seekChk.idx === seekChk.exp && Math.abs(seekChk.off) <= 12) break;
    }
    await sleep(250);
  }
  ck(seekChk && seekChk.idx === seekChk.exp && Math.abs(seekChk.off) <= 12, '#4 seek 后高亮行跟到新位置并立刻重定位（居中）', seekChk);
  ck(seekChk.guard === 0, '#4 seek 会清掉手动滚动保护', seekChk.guard);

  // ===== #4b 真机档视口 893x587 / DPR 1.5（261024-03 #3 用户报「完全不会滚动」的就是这一档） =====
  // 为什么必须单列一档：旧验收只有 1576x908/DPR1（1576 宽 > 1240 ⟹ 右栏可见、行高 34px），
  // 而真机 WebView2 的 CSS 视口是 893x587（窗口 1340x880 物理 @150%）或 1340x880。
  // 两档都要过；判据与 tools/lyr_dpr.js 同源。
  // 夹具：这一档自己注入一份 16 行、铺满全曲的 LRC（拦截 /api/lyrics），
  //       不依赖被测曲目是否带歌词 —— 曲库里的曲目大多没有 .lrc，用真实歌词会让这一档静默空转。
  console.log('=== #4b 真机档 893x587 / DPR 1.5 ===');
  await setSize(893, 587, 1.5);
  const vp2 = await evalJs('({w:innerWidth,h:innerHeight,dpr:devicePixelRatio})');
  ck(Math.round(vp2.w) === 893 && Math.round(vp2.h) === 587 && Math.abs(vp2.dpr - 1.5) < 0.01,
     '#4b 视口 = 893x587 / DPR 1.5', vp2);
  if (!(await evalJs("document.getElementById('npPage').classList.contains('on')"))) {
    await evalJs("document.getElementById('npOpen').click()");
    await waitFor("document.getElementById('npPage').classList.contains('on')", 6000);
  }
  // 注入 16 行、时间戳均布于全曲的 LRC（只拦 /api/lyrics；其余请求照旧）
  await evalJs('(function(){' +
    'if(!window.__lyMockInstalled){window.__lyMockInstalled=1;var of=window.fetch;' +
    'window.fetch=function(u,o){var s=String(u);' +
    'if(s.indexOf("/api/lyrics")===0 && window.__LYMOCK && window.__LYMOCK.on){' +
    'return Promise.resolve(new Response(JSON.stringify({ok:true,lrc:window.__LYMOCK.lrc}),{status:200,headers:{"Content-Type":"application/json"}}));}' +
    'return of.apply(this,arguments);};}' +
    'var dur=S.dur||240,n=16,a=[];for(var i=0;i<n;i++){var t=dur*i/n,m=Math.floor(t/60),q=Math.floor(t%60);' +
    'a.push("["+(m<10?"0":"")+m+":"+(q<10?"0":"")+q+".00]MOCK 歌词第 "+(i+1)+" 行");}' +
    'window.__LYMOCK={on:true,lrc:a.join(String.fromCharCode(10))};LYRETRY.file="";loadLyrics(S.lastCur);return 1;})()');
  await waitFor('LY.lines.length === 16', 12000);
  const nL2 = await evalJs('LY.lines.length');
  ck(nL2 === 16 && (await evalJs('LY.timed === true')), '#4b 夹具就位：16 行带时间轴歌词', nL2);
  const g2 = await evalJs('(function(){var b=document.getElementById("npLyric");return {ch:b.clientHeight,sh:b.scrollHeight};})()');
  console.log('  #4b 夹具: ' + nL2 + ' 行 / 容器 ' + JSON.stringify(g2));
  ck(g2.ch > 40 && g2.sh > g2.ch, '#4b 歌词容器有高度且真的可滚（893x587 下不塌陷）', g2);

  // (a) 用【行时间戳】驱动（不依赖这首歌的歌词是否铺满全曲）：首行 / 中间 / 末行
  const probeRows = [['首行', 0], ['中间', Math.floor(nL2 / 2)], ['末行', nL2 - 1]];
  const rowGeo = [];
  for (const [nm, idx] of probeRows) {
    await evalJs('(function(){var t=LY.lines[' + idx + '].t+0.2; window.__freezePos=t; window.__freezeDur=S.dur||1;' +
                 'S.pos=t; LYMAN.until=0; LY.at=-1; var b=document.getElementById("npLyric"); b.dataset.at=""; renderNpLyric(); return 1;})()');
    await sleep(700);
    const r3 = await evalJs('(function(){var box=document.getElementById("npLyric");var on=box.querySelector(".npline.on");' +
      'if(!on)return {has:false};var br=box.getBoundingClientRect(),er=on.getBoundingClientRect();' +
      'return {has:true,st:Math.round(box.scrollTop*100)/100,idx:+on.dataset.i,' +
      'off:Math.round(((er.top+er.height/2)-(br.top+br.height/2))*100)/100,ch:box.clientHeight,sh:box.scrollHeight};})()');
    rowGeo.push({ nm, idx, ...r3 });
    ck(r3.has && r3.idx === idx && Math.abs(r3.off) <= 8,
       '#4b ' + nm + '（第 ' + (idx + 1) + ' 行）滚到容器正中（|off| <= 8px）', r3);
  }
  const stA = rowGeo[0] && rowGeo[0].st, stB = rowGeo[1] && rowGeo[1].st, stC = rowGeo[2] && rowGeo[2].st;
  ck(stA !== stB || stB !== stC, '#4b scrollTop 随行变化（不是钉死不动）', {st首行: stA, st中: stB, st末: stC});
  ck(Math.abs(rowGeo[0].off) <= 4 && Math.abs(rowGeo[2].off) <= 4,
     '#4b 首行/末行也能滚到正中（|off| <= 4px；旧 calc(50% - 22px) 在此档差 8.7/9.4px）',
     {首行: rowGeo[0].off, 末行: rowGeo[2].off});

  // (b) 30/60/90% 播放进度：居中 + scrollTop 有变化
  const seq3 = [];
  for (const pct of [30, 60, 90]) {
    await evalJs('(function(){var d=S.dur||1;var p=d*' + pct + '/100; window.__freezePos=p; window.__freezeDur=d;' +
                 'S.pos=p; LYMAN.until=0; LY.at=-1; document.getElementById("npLyric").dataset.at=""; renderNpLyric(); return 1;})()');
    await sleep(700);
    const g3 = await evalJs('(function(){var box=document.getElementById("npLyric");var on=box.querySelector(".npline.on");' +
      'if(!on)return {has:false};var br=box.getBoundingClientRect(),er=on.getBoundingClientRect();' +
      'return {has:true,st:Math.round(box.scrollTop*100)/100,off:Math.round(((er.top+er.height/2)-(br.top+br.height/2))*100)/100};})()');
    seq3.push({ pct, ...g3 });
    ck(g3.has && Math.abs(g3.off) <= 8, '#4b ' + pct + '% 时 active 行居中（|off| <= 8px）', {off:g3.off, st:g3.st});
  }
  ck(new Set(seq3.map(x => x.st)).size > 1 || stA !== stC,
     '#4b 30/60/90% 期间 scrollTop 发生过变化', {seq: seq3.map(x => x.st), 行驱动: [stA, stB, stC]});

  // (c) 893 宽下右栏被 media query 收起 —— 明确记录，不当失败
  const railHidden = await evalJs("(function(){var r=document.getElementById('right');if(!r)return true;return getComputedStyle(r).display==='none';})()");
  ck(railHidden === true, '#4b 893 宽下右栏 #right 按 @media(max-width:1240px) 收起（右栏歌词判据由 1576 档覆盖）');
  await shot('t25-lyrics-dpr893');
  await evalJs('(function(){delete window.__freezePos; delete window.__freezeDur;' +
               'if(window.__LYMOCK) window.__LYMOCK.on=false; LYRETRY.file=""; if(S.lastCur) loadLyrics(S.lastCur); return 1;})()');
  await setSize(1576, 908);

  // ================= #3 时长 / 曲目列表 =================
  console.log('=== #3 时长与曲目列表（本地曲库为唯一真相源） ===');
  const sumTracks = useAlb.tracks.reduce((s,t)=>s+(t.dur||0),0);
  ck(Math.abs(sumTracks - (useAlb.dur||0)) <= 2, '#3 后端 album.dur = Σ 本地曲目时长', { sum: sumTracks, dur: useAlb.dur });
  await setSize(1576,908);
  await evalJs("setView('albums'); loadAlbums('');"); await sleep(1200);
  await evalJs('openAlbum(' + JSON.stringify(useAlb.k) + ')');
  await sleep(1800);
  const albumUi = await evalJs('(function(){var rows=document.querySelectorAll("#list .trow-track");var head=document.querySelector("#list .trlist-head .sum");var du=[].map.call(rows,function(r){return r.querySelector(".du").textContent.trim();});var sum=document.getElementById("albumSummary");return {n:rows.length,head:head?head.textContent.trim():"",du:du,sumTxt:sum?sum.textContent:""};})()');
  ck(albumUi.n === useAlb.tracks.length, '#3 专辑页曲目行数 = 本地曲目数', { rows: albumUi.n, local: useAlb.tracks.length });
  ck(albumUi.head.indexOf(useAlb.tracks.length + ' 首') >= 0, '#3 曲目列表表头写了「N 首」', albumUi.head);
  ck(albumUi.head.indexOf('总时长') >= 0 && albumUi.head.indexOf(fmtLike(sumTracks)) >= 0, '#3 表头总时长 = Σ 本地曲目（' + fmtLike(sumTracks) + '）', albumUi.head);
  const badDur = albumUi.du.filter(function(s){ return !/^[0-9]+:[0-9][0-9]$/.test(s); });
  ck(badDur.length === 0, '#3 每行时长都是本地文件时长（m:ss）', badDur.slice(0,3));
  await waitFor("document.querySelector('#albumSummary .efrow')", 8000);
  const albSum = await evalJs("(document.getElementById('albumSummary')||{}).textContent||''");
  ck(albSum.indexOf('02:42（Track 01）') < 0, '#3 AI 猜的时长不被采信（页面无「02:42（Track 01）」）', albSum.slice(0,80));
  ck(albSum.indexOf('MOCK 单曲') < 0, '#3 AI 的单行曲目列表不被渲染（页面上是本地对齐表格）');
  await shot('t25-album-1576x908');
  await setSize(1024, 640); await sleep(700);
  await shot('t25-album-1024x640');
  await setSize(1576, 908); await sleep(400);

  await evalJs('playPath(' + JSON.stringify(track.path) + ')');
  await waitFor('S.playing && S.lastCur === ' + JSON.stringify(track.path) + ' && S.dur > 0', 25000);
  await sleep(1200);
  await evalJs("document.getElementById('npOpen').click()"); await sleep(900);
  await waitFor("document.querySelectorAll('.npchip').length > 0", 15000);
  await evalJs('(function(){var b=[].filter.call(document.querySelectorAll(".npchip"),function(x){return x.textContent.trim()==="歌曲";});if(b.length)b[0].click();})()');
  await sleep(1200);
  const songTxt = await evalJs("(document.getElementById('npDataBody')||{}).textContent||''");
  const fileDur = await evalJs('S.dur');
  ck(songTxt.indexOf('02:42（Track 01）') < 0, '#3 歌曲标签不显示 AI 猜的时长', songTxt.slice(0,60));
  ck(songTxt.indexOf(fmtLike(fileDur)) >= 0, '#3 歌曲标签显示文件真实时长 ' + fmtLike(fileDur), { dur: fileDur });
  await evalJs('(function(){var b=[].filter.call(document.querySelectorAll(".npchip"),function(x){return x.textContent.trim()==="专辑";});if(b.length)b[0].click();})()');
  await sleep(2000);
  const relRows = await evalJs("document.querySelectorAll('#npDataBody .eft-row').length");
  ck(relRows === useAlb.tracks.length, '#3 抽屉「专辑」标签用本地曲目列表（行数=本地曲目数）', { rows: relRows, local: useAlb.tracks.length });
  await shot('t25-np-album-tab');
  await evalJs("(function(){var c=document.getElementById('npDataClose');if(c)c.click();})()");

  // ================= #2 外链 =================
  console.log('=== #2 资料链接 → 系统浏览器（postMessage openurl） ===');
  const ext = await evalJs('(function(){window.__msgs=[];var ok1=openExternal("https://monster-siren.hypergryph.com/music/779484");var ok2=openExternal("javascript:alert(1)");var ok3=openExternal("file:///C:/Windows/win.ini");var ok4=openExternal("https://ok.example/a");return {ok1:ok1,ok2:ok2,ok3:ok3,ok4:ok4,msgs:(window.__msgs||[]).slice()};})()');
  ck(ext.ok1 === true && ext.msgs.length >= 1 && ext.msgs[0] === 'openurl:https://monster-siren.hypergryph.com/music/779484', '#2 https 链接 → postMessage openurl:<url>（URL 原样）', ext.msgs[0]);
  ck(ext.ok2 === false && ext.ok3 === false, '#2 javascript: / file: 被拒绝（返回 false）', { js: ext.ok2, file: ext.ok3 });
  ck(ext.msgs.length === 2 && ext.msgs[1] === 'openurl:https://ok.example/a', '#2 非法协议不产生 postMessage（只发两条合法消息）', ext.msgs);
  const linkHit = await evalJs('(function(){var el=document.querySelector(".eflink");if(!el)return {found:false};window.__msgs=[];el.click();return {found:true,url:el.dataset.url,msgs:(window.__msgs||[]).slice()};})()');
  ck(linkHit.found && linkHit.msgs.length === 1 && linkHit.msgs[0].indexOf('openurl:https://') === 0, '#2 「关联」字段里的 URL 可点击并交给宿主打开', linkHit);
  // 引用 chip 在「艺术家」标签的资料里（mock person 带 citations）
  await evalJs('(function(){var b=[].filter.call(document.querySelectorAll(".npchip"),function(x){return x.textContent.trim()==="艺术家";});if(b.length)b[0].click();})()');
  await sleep(1500);
  const chipHit = await evalJs('(function(){var open=document.querySelector(".chip-open"),copy=document.querySelector(".chip-copy");if(!open||!copy)return {found:false};window.__msgs=[];open.click();return {found:true,url:open.dataset.url,msgs:(window.__msgs||[]).slice(),hasCopy:!!copy};})()');
  ck(chipHit.found && chipHit.hasCopy && chipHit.msgs.length === 1, '#2 引用 chip：点本体=打开浏览器，旁边仍有复制按钮', chipHit);

  // ================= #5 推送到 UPnP =================
  console.log('=== #5 一键推送到 UPnP（假渲染器，不出声） ===');
  await evalJs("setView('settings'); openSettingsTab('out');");
  await sleep(1000);
  const outTab = await evalJs('(function(){var txt=(document.querySelector("#setpage .spane[data-g=out]")||{}).textContent||"";var opts=[].map.call(document.querySelectorAll("#sp_backend option"),function(o){return o.value;});return {hasCastBlock:txt.indexOf("投放（UPnP）")>=0,hasCastStat:txt.indexOf("投放实时状态")>=0,hasStart:!!document.getElementById("sp_cast_start"),hasFile:!!document.getElementById("sp_cast_file"),backendOpts:opts};})()');
  ck(!outTab.hasCastBlock && !outTab.hasCastStat, '#5 输出页已无「投放（UPnP）」「投放实时状态」两块', outTab);
  ck(!outTab.hasStart && !outTab.hasFile, '#5 输出页已无开始投放/源文件控件');
  ck(outTab.backendOpts.indexOf('upnp') >= 0, '#5 输出后端下拉仍保留 UPnP 选项', outTab.backendOpts);
  await evalJs("openSettingsTab('stream')");
  await sleep(1200);
  const streamTab = await evalJs('(function(){var p=document.querySelector("#setpage .spane[data-g=stream]");return {hasPush:!!document.getElementById("sp_cast_push"),hasStop:!!document.getElementById("sp_cast_stop"),hasDev:!!document.getElementById("sp_cast_dev"),hasStat:!!document.getElementById("sp_cast_stat_phase"),txt:(p?p.textContent:"").indexOf("推送到设备")>=0};})()');
  ck(streamTab.hasPush && streamTab.hasStop && streamTab.hasDev && streamTab.hasStat, '#5 流媒体页有「推送当前曲/停止投放/设备下拉/实时状态」', streamTab);
  await shot('t25-stream-tab');

  await evalJs("setView('albums'); loadAlbums('');"); await sleep(900);
  await evalJs('playPath(' + JSON.stringify(track.path) + ')');
  await waitFor('S.playing', 20000); await sleep(1200);
  await evalJs("document.getElementById('npOpen').click()"); await sleep(800);
  await evalJs("(function(){ADV.upnpDev='';ADV.backend='null';saveSettings();return 1;})()");
  await sleep(700);
  const btn0 = await evalJs("(document.getElementById('npCastBtn')||{}).textContent||''");
  ck(btn0.indexOf('推送到设备') >= 0, '#5 单曲页有「推送到设备」按钮', btn0);
  await evalJs("document.getElementById('npCastBtn').click()");
  await sleep(1800);
  const picker = await evalJs('(function(){var m=document.getElementById("modal");return {on:m.classList.contains("on"),rows:document.querySelectorAll("#mbody .castpick-row").length,title:(document.getElementById("mtitle")||{}).textContent};})()');
  ck(picker.on && picker.rows >= 1, '#5 未选设备时先弹出设备列表', picker);
  await shot('t25-cast-picker');
  await evalJs("document.querySelector('#mbody .castpick-row').click()");
  await sleep(5000);
  const st1 = await evalJs("fetch('/api/cast/state').then(r=>r.json())");
  ck(st1.running === true, '#5 推送后投放会话已启动（running=true）', { running: st1.running, phase: st1.phase, err: st1.error });
  ck(st1.device === MOCK_DEV || st1.device === 'Mock-N1000-Renderer', '#5 目标设备 = 假渲染器（friendlyName 或描述 URL）', { device: st1.device });
  const mockState = await (await fetch(MOCK + '/__state')).json();
  const calls = (mockState.calls||[]).map(c=>c.action);
  ck(calls.indexOf('SetAVTransportURI') >= 0 && calls.indexOf('Play') >= 0, '#5 假渲染器收到 SetAVTransportURI + Play（所有播放走 UPnP）', calls.slice(0,8));
  const uriCall = (mockState.calls||[]).filter(c=>c.action==='SetAVTransportURI')[0]||{};
  ck(String(uriCall.uri||'').indexOf('http://127.0.0.1:') === 0, '#5 渲染器取流地址指向本机供源端口', uriCall.uri);
  const kv2 = await evalJs("fetch('/api/settings').then(r=>r.json()).then(j=>j.kv||{})");
  ck(kv2.backend === 'upnp' && kv2.upnpdev === MOCK_DEV, '#5 backend=upnp / upnpdev 已落盘', { backend: kv2.backend });
  const ui3 = await evalJs('(function(){var nb=document.getElementById("npCastBtn");var badge=document.getElementById("castBadge");var stop=document.getElementById("sp_cast_stop");return {np:nb?nb.textContent.trim():"",npOn:nb?nb.classList.contains("on"):false,badge:badge?getComputedStyle(badge).display:"none",stopDisabled:stop?stop.disabled:null};})()');
  ck(ui3.npOn && ui3.np.indexOf('已推送到') >= 0, '#5 单曲页按钮变成「已推送到 <设备>」', ui3.np);
  ck(ui3.badge !== 'none', '#5 底栏出现 UPnP 徽标（与单曲页同源）', ui3.badge);
  ck(ui3.stopDisabled === false, '#5 流媒体页「停止投放」已可用（三处状态一致）', ui3.stopDisabled);
  await shot('t25-cast-on-1576');
  await setSize(1024,640); await sleep(600); await shot('t25-cast-on-1024');
  await setSize(1576,908); await sleep(400);
  await evalJs("document.getElementById('npCastBtn').click()");
  await sleep(4000);
  const st2 = await evalJs("fetch('/api/cast/state').then(r=>r.json())");
  ck(st2.running === false, '#5 再点一次 → 投放停止', st2.running);
  const kv3 = await evalJs("fetch('/api/settings').then(r=>r.json()).then(j=>j.kv||{})");
  ck(kv3.backend === 'null', '#5 停止后输出后端恢复为推送前的 null（测试实例不发声）', kv3.backend);
  const ui4 = await evalJs('(function(){var nb=document.getElementById("npCastBtn");var b=document.getElementById("castBadge");return {np:nb?nb.textContent.trim():"",badge:b?getComputedStyle(b).display:"none"};})()');
  ck(ui4.np.indexOf('推送到设备') >= 0 && ui4.badge === 'none', '#5 停止后按钮/徽标回到未投放状态', ui4);
  await shot('t25-cast-off-1576x908');
  await setSize(1024, 640); await sleep(600);
  await shot('t25-np-default-1024x640');
  await setSize(1576, 908); await sleep(300);
  await shot('t25-np-default-1576x908');

  ck(errors.length === 0, '全程没有 JS 异常 / console.error', errors.slice(0,3).join(' | '));
  const bad = results.filter(r=>!r.ok).length;
  console.log('');
  console.log(bad === 0 ? ('t25 检查全部通过（' + results.length + ' 项）') : ('t25 检查 ' + bad + ' 项失败（共 ' + results.length + ' 项）'));
  try { await fetch(URL_.replace(/\/$/,'') + '/api/cast/stop', { method:'POST' }); } catch(e){}
  try { await evalJs("fetch('/api/settings',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'backend=null&volume=-60'}).then(function(){loadSettings();return 1;})"); } catch(e){}
  ws.close(); try { child.kill(); } catch(e){} try { mockProc.kill(); } catch(e){}
  process.exit(bad === 0 ? 0 : 1);
})().catch(e => { console.error('t25check 失败: ' + e.message); console.error(errors.slice(0,5).join(' | ')); try { child.kill(); } catch(_){} try { mockProc.kill(); } catch(_){} process.exit(2); });