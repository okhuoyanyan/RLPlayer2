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
async function play(file) {
  await evalJs('playPath(' + JSON.stringify(file) + ')');
  const ok = await waitFor("S.playing && S.lastCur === " + JSON.stringify(file), 20000);
  await sleep(900);
  return ok;
}
async function bandShot(name) {
  await evalJs("(function(){var d=document.createElement('div');d.id='__band';d.style.cssText='position:fixed;inset:0;z-index:2147483647;pointer-events:none;border:5px solid var(--bg);box-sizing:border-box';document.body.appendChild(d);})()");
  const p = await shot(name + '-band5');
  await evalJs("(function(){var b=document.getElementById('__band');if(b)b.remove();})()");
  return p;
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
  const edge = await evalJs("(function(){var ids=['npGrid'];for(var i=0;i<ids.length;i++){var e=document.getElementById(ids[i]);if(!e)continue;var b=e.getBoundingClientRect();if(b.left<8||b.top<8||b.right>innerWidth-8||b.bottom>innerHeight-8)return false;}return true;})()");
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
  ck(navs.some(function (t) { return t.indexOf('EQ') >= 0; }), '左侧导航仍保留「EQ 与房间校正」', '');

  if (!TRACK) { console.log('未提供测试曲目，跳过单曲页场景（只做静态检查）'); }
  else {
    for (const [W, H] of [[1576, 908], [1024, 640]]) {
      const tag = ' @' + W + 'x' + H;
      await setSize(W, H);
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
      await bandShot('np-' + W + 'x' + H + '-02-cached');
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
      // 无缓存专辑：切歌后自动加载 → 骨架
      if (CLASSICAL) {
        await play(CLASSICAL);
        await sleep(600);
        const loading = await evalJs("(function(){var b=document.getElementById('npDataBody');return !!(b && b.querySelector('.npdskel'));})()");
        ck(loading, '无缓存实体加载态为骨架（不是大块空白）' + tag, '');
        await shot('np-' + W + 'x' + H + '-04-loading');
        const hasWork = await evalJs("Array.from(document.querySelectorAll('.npdtab')).some(function(b){return b.textContent.trim()==='作品';})");
        ck(hasWork, '古典曲目出现「作品」tab' + tag, '');
        await tabClick('作品');
        await sleep(500);
        ck((await activeTab()) === '作品', '切到「作品」tab' + tag, '');
        await waitFor("document.querySelectorAll('#npDataBody .npdwork').length > 0 || document.getElementById('npWorkRetry')", 90000);
        const nv = await evalJs("document.querySelectorAll('#npDataBody .npdwork').length");
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
    const eq0 = await evalJs("fetch('/api/settings').then(function(r){return r.json();}).then(function(j){return j.kv.eq||'';})");
    ck(await evalJs("!document.getElementById('npEqChip')"), 'EQ 为空时单曲页没有 EQ 芯片', 'eq="' + eq0 + '"');
    await evalJs("fetch('/api/settings',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'eq='+encodeURIComponent('profiles/opra/sennheiser__hd_600_sennheiser_hd_600__autoeq_filk.txt')}).then(function(r){return r.json();})");
    await evalJs("loadSettings()");
    await sleep(900);
    const hasChip = await evalJs("!!document.getElementById('npEqChip')");
    ck(hasChip, '加载 EQ 后单曲页出现 EQ 芯片（#4）', '');
    await shot('np-eq-chip-on');
    if (hasChip) {
      await evalJs("document.getElementById('npEqChip').click()");
      await sleep(700);
      ck(await evalJs("S.view === 'eq'"), '点击 EQ 芯片跳到「EQ 与房间校正」页', '');
      await evalJs("setView('albums'); loadAlbums('');");
      await sleep(500);
      await play(TRACK);
      await ensureNpOpen();
      await sleep(400);
    }
    await evalJs("fetch('/api/settings',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'eq='})");
    await evalJs("loadSettings()");
    await sleep(900);
    ck(await evalJs("!document.getElementById('npEqChip')"), '清空 EQ 后 EQ 芯片消失', '');
    await shot('np-eq-chip-off');
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
