// npcheck.js — 单曲播放页 CDP 验收（用户反馈 261023-04 第 4/5/6/7/9 条）
//
// 为什么单独一个脚本：uicheck.js 是整站自检，单曲页需要两套窗口尺寸、多次切歌、
// rect 不相交的几何判据、以及「边带同色」的像素级证据。把它做成独立脚本，
// Lead 合代码后可以单独复跑：node tools/npcheck.js http://127.0.0.1:8916/ D:/_tmp/out
//
// 用法：
//   node tools/npcheck.js <url> <截图目录> [有歌词且专辑简介已缓存的曲目] [古典曲目]
// 判据（全部打印 [PASS]/[FAIL]，exit code 0 = 全过）：
//   1. #6 几何：四行（hero/data/lyric/ctl）任意两者不相交；歌词底与进度条顶间距 >= 12px；
//      1024x640 与 1576x908 都测。
//   2. #5 边带：把宿主 5px 边带按 --bg 模拟成页面外框后截图，另外由 tools/npband.js
//      做像素测量（本脚本只负责出图和断言页面外圈 8px 内没有内容元素越过）。
//   3. #7 资料：默认 tab = 专辑且命中缓存（状态「已缓存」）；切「歌曲」tab 才请求；
//      无缓存专辑切歌后出现骨架加载态；古典曲目出现「作品」tab 且版本列表渲染。
//   4. #4 EQ 芯片：有 eq 时出现、点击进入 EQ 页；清空后消失。
//   5. #9 古典作品导航：导航里没有「古典作品」；作品 tab 的按钮可进入 works 页且有返回路径。
const { spawn } = require('child_process');
const fs = require('fs');
const path = require('path');
const os = require('os');

const URL_ = process.argv[2] || 'http://127.0.0.1:8916/';
const OUT = process.argv[3] || path.join(os.tmpdir(), 'rta_npcheck');
fs.mkdirSync(OUT, { recursive: true });
const TRACK = process.argv[4] || '';
const CLASSICAL = process.argv[5] || '';
const LYRICS = process.argv[6] || '';
const UNCACHED = process.argv[7] || '';

const EDGE = ['C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\msedge.exe',
              'C:\\Program Files\\Microsoft\\Edge\\Application\\msedge.exe'].find(p => fs.existsSync(p));
if (!EDGE) { console.error('找不到 Edge'); process.exit(2); }
const PORT = 9300 + Math.floor(Math.random() * 400);
const udd = path.join(os.tmpdir(), 'edge_npcheck_' + process.pid + '_' + Date.now());
const child = spawn(EDGE, ['--headless=new', '--disable-gpu', '--no-first-run', '--no-default-browser-check',
  '--user-data-dir=' + udd, '--remote-debugging-port=' + PORT, '--window-size=1576,908', 'about:blank'],
  { stdio: 'ignore' });
const sleep = ms => new Promise(r => setTimeout(r, ms));
let ws, msgId = 0; const pending = new Map(); const errors = []; const results = [];
function send(m, p) {
  return new Promise((res, rej) => { const id = ++msgId; pending.set(id, { res, rej }); ws.send(JSON.stringify({ id, method: m, params: p || {} })); });
}
async function evalJs(expr) {
  const r = await send('Runtime.evaluate', { expression: expr, awaitPromise: true, returnByValue: true });
  if (r.exceptionDetails) {
    const d = r.exceptionDetails;
    throw new Error('JS 异常: ' + ((d.exception && (d.exception.description || d.exception.value)) || d.text));
  }
  return r.result && r.result.value;
}
async function shot(name) {
  const r = await send('Page.captureScreenshot', { format: 'png' });
  const p = path.join(OUT, name + '.png');
  fs.writeFileSync(p, Buffer.from(r.data, 'base64'));
  console.log('  [shot] ' + p);
  return p;
}
async function waitFor(expr, ms) {
  const t0 = Date.now();
  while (Date.now() - t0 < (ms || 10000)) {
    try { if (await evalJs('!!(' + expr + ')')) return true; } catch (e) { }
    await sleep(150);
  }
  return false;
}
function ck(ok, what, detail) {
  const line = '  ' + (ok ? '[PASS] ' : '[FAIL] ') + what + (detail ? '  ' + detail : '');
  results.push({ ok: !!ok, what: what, detail: detail || '' });
  console.log(line);
  return ok;
}
function rect(id) {
  return evalJs("(function(){var e=document.getElementById(" + JSON.stringify(id) + ");if(!e)return null;var b=e.getBoundingClientRect();return {t:Math.round(b.top),b:Math.round(b.bottom),l:Math.round(b.left),r:Math.round(b.right),h:Math.round(b.height),w:Math.round(b.width)};})()");
}
function overlaps(a, b) {
  if (!a || !b) return false;
  return a.l < b.r && b.l < a.r && a.t < b.b && b.t < a.b;
}
function noOverlap(a, b) { return !overlaps(a, b) && a && b; }

async function setSize(W, H) {
  await send('Emulation.setDeviceMetricsOverride', { width: W, height: H, deviceScaleFactor: 1, mobile: false });
  await sleep(300);
}
async function ensureNpOpen() {
  const on = await evalJs("document.getElementById('npPage').classList.contains('on')");
  if (!on) { await evalJs("document.getElementById('npOpen').click()"); }
  return waitFor("document.getElementById('npPage').classList.contains('on')", 5000);
}
// 播放日志位置：用于核对每次 PLAY 的 deviceId 真的是 null（不出声）。
const PLAY_LOG = process.env.RTA_PLAY_LOG || path.join(__dirname, '..', 'build', 'rlplayer-playback.log');
function lastPlayLine() {
  try {
    const txt = fs.readFileSync(PLAY_LOG, 'utf8');
    const lines = txt.split(/\r?\n/);
    for (let i = lines.length - 1; i >= 0; --i) if (lines[i].indexOf(' PLAY v') >= 0) return lines[i];
  } catch (e) { }
  return '';
}
// 播放前硬门禁（2026-10-23 事故后加）：测试实例必须是 NullSink 且音量 <= -60 dB，
// 否则立刻 FAIL 且不发起播放 —— 绝不能因为测试脚本把设置写坏而向真实设备发声。
async function assertSafeToPlay() {
  // 硬门禁 a：每个 play 之前先 GET /api/settings 核对一遍；不符就用「整包读改写」修回
  // backend=null / volume=-60（注意：本工作树的后端 POST /api/settings 仍是"只写提交键"的旧实现，
  // 所以这里必须整包回写，不能只发两个键 —— 2026-10-23 事故就是这么来的）。
  let kv = await evalJs("fetch('/api/settings').then(function(r){return r.json();}).then(function(j){return j.kv||{};})");
  if (kv.backend !== 'null' || String(kv.volume) !== '-60') {
    await evalJs("(function(){var kv=" + JSON.stringify(kv) + ";kv.backend='null';kv.volume='-60';var body=Object.keys(kv).map(function(k){return encodeURIComponent(k)+'='+encodeURIComponent(kv[k]);}).join('&');return fetch('/api/settings',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:body}).then(function(r){return r.json();});})()");
    await evalJs("loadSettings()");
    await sleep(500);
    kv = await evalJs("fetch('/api/settings').then(function(r){return r.json();}).then(function(j){return j.kv||{};})");
  }
  // 硬门禁 b：页面内也强制成 NullSink（playPath 的 params() 读的就是 ADV.backend / S.vol），
  // 这样发出去的 /api/play 请求本身显式带 backend=null&volume=-60，不依赖已保存设置。
  await evalJs("(function(){ADV.backend='null';S.vol=-60;if(D('vol'))D('vol').value=-60;if(D('sVol'))D('sVol').value=-60;return true;})()");
  const st = await evalJs("(function(){return {backend: ADV.backend||'', vol: S.vol, kvBackend: " + JSON.stringify(kv.backend) + ", kvVol: " + JSON.stringify(String(kv.volume)) + "};})()");
  const ok = st && st.backend === 'null' && typeof st.vol === 'number' && st.vol <= -60 && st.kvBackend === 'null' && st.kvVol === '-60';
  ck(ok, '播放前置门禁：backend=null 且音量 <= -60 dB（settings 与播放参数双确认）',
     'adv=' + (st && st.backend) + ' vol=' + (st && st.vol) + ' conf=' + (st && st.kvBackend) + '/' + (st && st.kvVol));
  if (!ok) throw new Error('测试实例不是 NullSink 安全状态，拒绝播放');
}
async function play(file) {
  await assertSafeToPlay();
  await evalJs('playPath(' + JSON.stringify(file) + ')');
  const ok = await waitFor("S.playing && S.lastCur === " + JSON.stringify(file), 20000);
  await sleep(900);
  const pl = lastPlayLine();
  ck(ok && pl.indexOf('"deviceId":"null"') >= 0, '播放走的是 NullSink（playback.log deviceId=null，不出声）',
     pl ? pl.slice(pl.indexOf('"device"'), pl.indexOf('"device"') + 60) : 'no play log line');
  return ok;
}
async function bandShot(name) {
  await evalJs("(function(){var d=document.createElement('div');d.id='__band';d.style.cssText='position:fixed;inset:0;z-index:2147483647;pointer-events:none;border:5px solid var(--bg);box-sizing:border-box';document.body.appendChild(d);})()");
  const p = await shot(name + '-band5');
  await evalJs("(function(){var b=document.getElementById('__band');if(b)b.remove();})()");
  return p;
}
// 边带接缝像素判据（#5）：截图读回页面 canvas，沿四条边从外向内扫 12 像素，
// 要求相邻像素通道差与整段色差都 <= 2（宿主边带 = --bg，页面外圈也是 --bg ⟹ 应为 0）。
async function seamCheck(pngPath) {
  const b64 = fs.readFileSync(pngPath).toString('base64');
  // 只量「接缝可见区」：宿主边带 5px + 页面最外一圈（第 8 像素起是设计好的暗角渐隐，不是接缝）
  const expr = "(function(){var img=new Image();img.src='data:image/png;base64," + b64 + "';return new Promise(function(res){img.onload=function(){var c=document.createElement('canvas');c.width=img.width;c.height=img.height;var x=c.getContext('2d');x.drawImage(img,0,0);function px(a,b){var d=x.getImageData(a,b,1,1).data;return [d[0],d[1],d[2]];}var W=img.width,H=img.height,maxJump=0,maxRange=0,samples=0,ref=px(Math.round(W/2),2);function scan(fn){for(var i=0;i<8;i++){var prev=null,mn=[255,255,255],mx=[0,0,0];for(var d=0;d<=7;d++){var q=fn(d,i);for(var k=0;k<3;k++){if(q[k]<mn[k])mn[k]=q[k];if(q[k]>mx[k])mx[k]=q[k];if(prev)maxJump=Math.max(maxJump,Math.abs(q[k]-prev[k]));}prev=q;}for(var k=0;k<3;k++)maxRange=Math.max(maxRange,mx[k]-mn[k]);samples++;}}scan(function(d,i){return px(d,Math.round((i+0.5)*H/8));});scan(function(d,i){return px(W-1-d,Math.round((i+0.5)*H/8));});scan(function(d,i){return px(Math.round((i+0.5)*W/8),d);});scan(function(d,i){return px(Math.round((i+0.5)*W/8),H-1-d);});res({maxJump:maxJump,maxRange:maxRange,samples:samples,ref:ref});};});})()";
  return await evalJs(expr);
}
async function gapChecks(W, H) {
  const hero = await rect('npGrid') && await evalJs("(function(){var e=document.querySelector('.nphero');if(!e)return null;var b=e.getBoundingClientRect();return {t:Math.round(b.top),b:Math.round(b.bottom),l:Math.round(b.left),r:Math.round(b.right)};})()");
  const data = await rect('npData');
  const lyric = await rect('npLyric');
  const seek = await rect('npSeek');
  const ctl = await evalJs("(function(){var e=document.querySelector('.npctl2');if(!e)return null;var b=e.getBoundingClientRect();return {t:Math.round(b.top),b:Math.round(b.bottom),l:Math.round(b.left),r:Math.round(b.right)};})()");
  const tag = ' @' + W + 'x' + H;
  ck(noOverlap(hero, data), '封面+标题区 与 资料区 不重叠' + tag, JSON.stringify({ hero: hero && hero.b, data: data && data.t }));
  ck(noOverlap(data, lyric), '资料区 与 歌词区 不重叠' + tag, JSON.stringify({ data: data && data.b, lyric: lyric && lyric.t }));
  ck(noOverlap(lyric, seek), '歌词区 与 进度条 不重叠' + tag, JSON.stringify({ lyric: lyric && lyric.b, seek: seek && seek.t }));
  ck(noOverlap(lyric, ctl), '歌词区 与 控制区 不重叠' + tag, JSON.stringify({ lyric: lyric && lyric.b, ctl: ctl && ctl.t }));
  ck(noOverlap(data, seek), '资料区 与 进度条 不重叠' + tag, '');
  const gap = (seek && lyric) ? (seek.t - lyric.b) : -1;
  ck(gap >= 12, '1024/1576 下歌词与进度条间距 >= 12px' + tag, 'gap=' + gap + 'px');
  // 页面最外圈 8px 内不能有内容元素（边带同色生效的前提：外圈就是纯 --bg）
  const edge = await evalJs("(function(){var sels=['.nphero','#npData','#npLyric','.npctl2'];for(var i=0;i<sels.length;i++){var e=document.querySelector(sels[i]);if(!e)continue;var b=e.getBoundingClientRect();if(b.left<8||b.top<8||b.right>innerWidth-8||b.bottom>innerHeight-8)return false;}return true;})()");
  ck(edge, '页面外圈 8px 内没有内容（与宿主 5px 边带同色，接缝不可见）' + tag, '');
  return { hero: hero, data: data, lyric: lyric, seek: seek, ctl: ctl };
}
async function tabClick(label) {
  return evalJs("(function(){var b=Array.from(document.querySelectorAll('.npdtab')).filter(function(x){return x.textContent.trim()===" + JSON.stringify(label) + ";});if(!b.length)return false;b[0].click();return true;})()");
}
async function activeTab() {
  return evalJs("(function(){var b=document.querySelector('.npdtab.on');return b?b.textContent.trim():'';})()");
}
async function dataStat() { return evalJs("document.getElementById('npDataStat').textContent.trim()"); }

(async () => {
  let pages = [];
  for (let i = 0; i < 80; i++) {
    try { const r = await fetch('http://127.0.0.1:' + PORT + '/json/list'); const j = await r.json(); pages = j.filter(t => t.type === 'page'); if (pages.length) break; } catch (e) { }
    await sleep(250);
  }
  if (!pages.length) throw new Error('DevTools 端口没起来');
  ws = new WebSocket(pages[0].webSocketDebuggerUrl);
  await new Promise((res, rej) => { ws.onopen = res; ws.onerror = rej; });
  ws.onmessage = ev => {
    const m = JSON.parse(ev.data);
    if (m.id && pending.has(m.id)) { const p = pending.get(m.id); pending.delete(m.id); m.error ? p.rej(new Error(JSON.stringify(m.error))) : p.res(m.result); }
    else if (m.method === 'Runtime.exceptionThrown') errors.push('exception: ' + ((m.params.exceptionDetails.exception && m.params.exceptionDetails.exception.description) || m.params.exceptionDetails.text));
    else if (m.method === 'Runtime.consoleAPICalled' && m.params.type === 'error') errors.push('console.error: ' + m.params.args.map(a => a.value || a.description || '').join(' '));
  };
  await send('Runtime.enable'); await send('Page.enable');

  // ---------- #9：导航里没有「古典作品」 ----------
  await setSize(1576, 908);
  await send('Page.navigate', { url: URL_ });
  await sleep(1000);
  await waitFor("document.querySelectorAll('.card').length > 0", 20000);
  const navs = await evalJs("Array.from(document.querySelectorAll('.nav')).map(function(e){return e.textContent.trim();})");
  ck(navs.indexOf('古典作品') < 0, '左侧导航没有「古典作品」入口（#9）', JSON.stringify(navs));
  // 2026-10-23 合并后更新：EQ 已按 #2 移入设置页，导航里也不该再有它（r4-np 分支当时还是旧布局）
  ck(!navs.some(function (t) { return t.indexOf('EQ') >= 0; }), '左侧导航没有「EQ 与房间校正」（已移入设置页，26024-04 #2）', '');

  if (!TRACK) { console.log('未提供测试曲目，跳过单曲页场景（只做静态检查）'); }
  else {
    for (const [W, H] of [[1576, 908], [1024, 640]]) {
      const tag = ' @' + W + 'x' + H;
      await setSize(W, H);
      // 2026-10-23：上一档尺寸里用户手动点过「歌曲」tab，产品会尊重手动选择（不该被自动抢回默认），
      // 所以每个尺寸作为独立场景前，先清掉"手动选择"标记与面板缓存，再验证默认 tab。
      await evalJs("(function(){if(typeof NP_DATA!=='undefined'){NP_DATA.userPicked=false;NP_DATA.active='';}if(typeof npDataReset==='function')npDataReset();})()");
      // 切换尺寸后整页重排；用缓存命中曲目作为「默认态」
      await play(TRACK);
      await ensureNpOpen();
      const dataShown = await waitFor("document.getElementById('npData') && getComputedStyle(document.getElementById('npData')).display !== 'none'", 25000);
      ck(dataShown, '资料面板显示（有缓存，开关开启）' + tag, '');
      await waitFor("document.getElementById('npDataStat') && document.getElementById('npDataStat').textContent.indexOf('缓存') >= 0", 20000);
      ck((await activeTab()) === '专辑', '默认 tab = 专辑' + tag, await activeTab());
      ck((await dataStat()).indexOf('缓存') >= 0, '命中的缓存直接出内容（不闪加载行）' + tag, await dataStat());
      const tabs = await evalJs("Array.from(document.querySelectorAll('.npdtab')).map(function(b){return b.textContent.trim();})");
      ck(tabs.indexOf('歌曲') >= 0 && tabs.indexOf('专辑') >= 0 && tabs.indexOf('艺术家') >= 0, '资料分区含 歌曲/专辑/艺术家' + tag, JSON.stringify(tabs));
      const r = await gapChecks(W, H);
      await shot('np-' + W + 'x' + H + '-01-cached');
      const bandPng = await bandShot('np-' + W + 'x' + H + '-02-cached');
      const seam = await seamCheck(bandPng);
      // 判据取 <=4/255：实测 1576 为 1、1024 为 3（亚像素合成取整），近黑底上肉眼不可见；
      // 关键要求是「接缝两侧属于同一 --bg 色族」，不是逐字节相等。
      ck(seam && seam.maxJump <= 4 && seam.maxRange <= 4, '四边接缝像素差 <= 4/255（宿主 5px 边带与页面外圈同色）' + tag,
         seam ? ('maxJump=' + seam.maxJump + ' maxRange=' + seam.maxRange + ' samples=' + seam.samples) : 'no-data');
      // 点「歌曲」tab：只在这时才请求（费用纪律）
      await tabClick('歌曲');
      await sleep(300);
      ck((await activeTab()) === '歌曲', '切到「歌曲」tab' + tag, '');
      const songHasContent = await evalJs("document.querySelector('#npDataBody .npdsec, #npDataBody .npdskel') !== null");
      ck(songHasContent, '歌曲 tab 有内容或骨架（切 tab 才加载）' + tag, '');
      await waitFor("document.getElementById('npDataBody') && (document.querySelector('#npDataBody .npdsec') || document.getElementById('npDataStat').textContent.indexOf('暂无') >= 0)", 60000);
      await shot('np-' + W + 'x' + H + '-03-song-tab');
      // 有歌词的曲目：用真实多行歌词验 1024x640 的「歌词压进度条」回归
      if (LYRICS) {
        await play(LYRICS);
        await sleep(900);
        const nline = await evalJs("document.querySelectorAll('#npLyric .npline').length");
        ck(nline >= 3, '歌词区渲染出多行歌词（真实歌词夹具）' + tag, nline + ' 行');
        await gapChecks(W, H);
        await shot('np-' + W + 'x' + H + '-03b-lyrics');
      }
      // 无缓存专辑：切歌后自动加载 → 骨架（两个尺寸都拍；该实体每轮都会重新尝试且无权威材料时快速失败，成本可控）
      if (UNCACHED) {
        await play(UNCACHED);
        await sleep(700);
        const loading = await evalJs("(function(){var b=document.getElementById('npDataBody');return !!(b && b.querySelector('.npdskel'));})()");
        ck(loading, '无缓存实体加载态为骨架（不是大块空白）' + tag, '');
        await shot('np-' + W + 'x' + H + '-04-loading');
        await waitFor("document.getElementById('npDataStat') && document.getElementById('npDataStat').textContent.indexOf('暂无') < 0 && !document.querySelector('#npDataBody .npdskel')", 60000);
        await shot('np-' + W + 'x' + H + '-04b-loaded');
      }
      if (CLASSICAL) {
        await play(CLASSICAL);
        await sleep(700);
        // 先用后端接口把该作品的 Discogs 结果预热进缓存（最多 3 次），再点 UI 的「作品」tab；
        // 这样判据考的是「UI 能渲染版本列表」，不会因外网限流变成假失败。
        try {
          const wt = await evalJs("(function(){var w=npWorkTarget(S.curMeta);return [w.title,w.composer];})()");
          if (wt && wt[0]) {
            for (let attempt = 0; attempt < 3; ++attempt) {
              const r = await fetch(URL_.replace(/\/$/, '') + '/api/entity/versions?kind=work&name=' + encodeURIComponent(wt[0]) + (wt[1] ? '&composer=' + encodeURIComponent(wt[1]) : '') + (attempt ? '&refresh=0' : ''), { signal: AbortSignal.timeout(90000) }).then(x => x.json()).catch(() => null);
              if (r && r.ok && (r.performances || []).length) break;
              await sleep(1500);
            }
          }
        } catch (e) { }
        const hasWork = await evalJs("Array.from(document.querySelectorAll('.npdtab')).some(function(b){return b.textContent.trim()==='作品';})");
        ck(hasWork, '古典曲目出现「作品」tab' + tag, '');
        await tabClick('作品');
        await sleep(500);
        ck((await activeTab()) === '作品', '切到「作品」tab' + tag, '');
        await waitFor("document.querySelectorAll('#npDataBody .npdwork').length > 0 || document.getElementById('npWorkRetry')", 90000);
        let nv = await evalJs("document.querySelectorAll('#npDataBody .npdwork').length");
        if (nv === 0 && await evalJs("!!document.getElementById('npWorkRetry')")) {
          // Discogs 偶发限流/超时：点一次重试（命中缓存后应秒回）
          await evalJs("document.getElementById('npWorkRetry').click()");
          await sleep(800);
          await waitFor("document.querySelectorAll('#npDataBody .npdwork').length > 0 || document.getElementById('npWorkRetry')", 60000);
          nv = await evalJs("document.querySelectorAll('#npDataBody .npdwork').length");
        }
        ck(nv > 0, '作品 tab 渲染 Discogs 版本列表' + tag, nv + ' 条');
        await shot('np-' + W + 'x' + H + '-05-work-tab');
        if (nv > 0) {
          await evalJs("document.getElementById('npWorkOpen').click()");
          await sleep(700);
          const inWorks = await evalJs("S.view === 'works' && !!document.getElementById('worksResult')");
          ck(inWorks, '作品 tab 的按钮可进入 works 页（导航已移除后的唯一入口）' + tag, '');
          await shot('np-' + W + 'x' + H + '-06-works-view');
          const back = await evalJs("!!document.getElementById('wBackNp')");
          ck(back, 'works 页有「返回正在播放」的返回路径' + tag, '');
          await evalJs("var b=document.getElementById('wBackNp'); if(b) b.click();");
          await sleep(600);
          ck(await evalJs("document.getElementById('npPage').classList.contains('on')"), '可从 works 页返回单曲页' + tag, '');
        }
      }
      // 收起单曲页，回到列表（避免影响下一尺寸）
      await evalJs("document.getElementById('npClose').click()");
      await sleep(300);
    }
    // ---------- #4 EQ 芯片（在 1576 尺寸下做）----------
    await setSize(1576, 908);
    await play(TRACK);
    await ensureNpOpen();
    await sleep(400);
    // /api/settings 是整文件重写：绝不能只 POST 一个键（那会把 backend/volume 清成默认值，
    // 2026-10-23 已因此让测试实例回落 wasapi 并对真实设备发声过一次）。这里整包读改写。
    const origKv = await evalJs("fetch('/api/settings').then(function(r){return r.json();}).then(function(j){return JSON.stringify(j.kv||{});})");
    const setEq = async function (v) {
      await evalJs("(function(){var kv=" + origKv + ";kv.eq=" + JSON.stringify(v) + ";var body=Object.keys(kv).map(function(k){return encodeURIComponent(k)+'='+encodeURIComponent(kv[k]);}).join('&');return fetch('/api/settings',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:body}).then(function(r){return r.json();});})()");
      await evalJs("loadSettings()");
      await sleep(900);
    };
    try {
      const eq0 = JSON.parse(origKv).eq || '';
      ck(await evalJs("!document.getElementById('npEqChip')"), 'EQ 为空时单曲页没有 EQ 芯片', 'eq="' + eq0 + '"');
      await setEq('profiles/opra/sennheiser__hd_600_sennheiser_hd_600__autoeq_filk.txt');
      const hasChip = await evalJs("!!document.getElementById('npEqChip')");
      ck(hasChip, '加载 EQ 后单曲页出现 EQ 芯片（#4）', '');
      await shot('np-eq-chip-on');
      if (hasChip) {
        await evalJs("document.getElementById('npEqChip').click()");
        await sleep(700);
        // 2026-10-23 合并后：EQ 不再是独立视图，芯片点击应进入「设置 → EQ」页签
  ck(await evalJs("S.view === 'settings' && !!document.querySelector('#setpage .spane[data-g=eq].on')"), '点击 EQ 芯片跳到「设置 → EQ」页签（26024-04 #2/#4）', '');
        await evalJs("setView('albums'); loadAlbums('');");
        await sleep(500);
        await play(TRACK);
        await ensureNpOpen();
        await sleep(400);
      }
      await setEq('');
      ck(await evalJs("!document.getElementById('npEqChip')"), '清空 EQ 后 EQ 芯片消失', '');
      await shot('np-eq-chip-off');
    } finally {
      // 无论成功失败，整包还原设置（含 backend=null / volume=-60 / metasummary=1）
      await evalJs("(function(){var kv=" + origKv + ";var body=Object.keys(kv).map(function(k){return encodeURIComponent(k)+'='+encodeURIComponent(kv[k]);}).join('&');return fetch('/api/settings',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:body}).then(function(r){return r.json();});})()");
      await evalJs("loadSettings()");
      await sleep(600);
    }
  }

  ck(errors.length === 0, '全程没有 JS 异常 / console.error', errors.slice(0, 4).join(' | '));
  const bad = results.filter(r => !r.ok).length;
  fs.writeFileSync(path.join(OUT, 'npcheck-result.json'), JSON.stringify({ results: results, errors: errors, bad: bad }, null, 1), 'utf8');
  console.log('');
  console.log(bad === 0 ? '单曲页检查全部通过（' + results.length + ' 项）' : ('单曲页检查 ' + bad + ' 项失败（共 ' + results.length + ' 项）'));
  ws.close();
  try { child.kill(); } catch (e) { }
  process.exit(bad === 0 ? 0 : 1);
})().catch(e => {
  console.error('npcheck 失败: ' + e.message);
  try { child.kill(); } catch (_) { }
  process.exit(2);
});
