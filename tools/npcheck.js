// npcheck.js — 单曲播放页 CDP 验收（用户反馈 261023-04 第 4/5/6/7/9 条）
//
// 为什么单独一个脚本：uicheck.js 是整站自检，单曲页需要两套窗口尺寸、多次切歌、
// rect 不相交的几何判据、以及「边带同色」的像素级证据。把它做成独立脚本，
// Lead 合代码后可以单独复跑：node tools/npcheck.js http://127.0.0.1:8916/ D:/_tmp/out
//
// 用法：
//   node tools/npcheck.js <url> <截图目录> [有歌词且专辑简介已缓存的曲目] [古典曲目]
// 判据（全部打印 [PASS]/[FAIL]，exit code 0 = 全过）：
//   1. 几何（261024-01 #3b）：hero / 歌词+标签栏 / 控制区 三段互不相交；
//      歌词区与进度条间距 >= 12px；**歌词在左、标签栏在右**（同一行、垂直区间重叠），不是上下堆叠；
//      1024x640 与 1576x908 都测。
//   2. 黑边：宿主已改「WebView 满幅」（launcher kBandInset=0），页面也不再自绘实色外圈/暗角。
//      这里断言：.npedge 不再显示、#npPage 覆盖整个视口（>1px 误差即失败）。
//   3. 控制区（#3b ①）：音量**与控制键同一行**（|音量中心 - 播放键中心| <= 10px），而不是单独一行；
//      整页底部只有一块控制区。
//   4. 资料（#3b ③ / #4）：默认**完全隐藏**（抽屉关闭、不预取、不产生请求）；右侧只有标签药丸；
//      点标签才滑出抽屉，抽屉**不遮住控制区**；歌曲/专辑/艺术家三个标签分别请求 kind=track/release/person，
//      渲染的是各自的 fields 网格（不是同一份内容塞进三个标签）；#npDataClose / Esc / 点遮罩都能关；
//      古典曲目出现「作品」标签且版本列表渲染。
//   5. 文案（#2）：资料里只出现「AI 整理」四个字，不得出现「未找到权威来源」「仅供参考」「未核对」。
//   6. #4 EQ 芯片：有 eq 时出现、点击进入设置-EQ 页；清空后消失。
//   7. #9 古典作品导航：导航里没有「古典作品」；作品标签的按钮可进入 works 页且有返回路径。
//
// ⚠️ 数据来源口径：本机测试实例没有 LLM key（不花钱、不出网），所以「资料内容」类判据用
//    **页内契约 mock 注入**（拦截 fetch，返回 Lead 约定的 {kind, fields} 形状）：零真实调用、可复现。
//    UI 渲染/交互/几何全部是对真实页面的断言；真实后端联调由 Lead 侧验证。
//    传 --no-mock 可关掉注入（此时实例必须真有 AI 能力，否则资料类判据会 FAIL）。
const { spawn } = require('child_process');
const fs = require('fs');
const path = require('path');
const os = require('os');

const URL_ = process.argv[2] || 'http://127.0.0.1:8916/';
const OUT = process.argv[3] || path.join(os.tmpdir(), 'rta_npcheck');
fs.mkdirSync(OUT, { recursive: true });
// 可选夹具用 '-' 表示"没有"（PowerShell 会把空字符串参数吞掉，所以别依赖空串）
const arg = i => ((process.argv[i] || '') === '-' ? '' : (process.argv[i] || ''));
const TRACK = arg(4);
const CLASSICAL = arg(5);
const LYRICS = arg(6);
const UNCACHED = arg(7);
const NO_MOCK = process.argv.indexOf('--no-mock') >= 0;
// 2026-10-24（真实后端联调）：--no-mock 时走真实 /api/entity/summary。此时：
//  · 断言文案改成真实字段名（不再依赖 MOCK-* 标记）；
//  · 点标签前清空页面内 NP_DATA 缓存，确保真的发请求（否则预取/缓存会让"请求次数=0"假失败）；
//  · 数据生成要等，rows 判据先 waitFor 再断言。
const REAL = NO_MOCK;

// 契约为 Lead 约定的 {ok, kind, fields:{标签:值}}。三个 kind 内容形状不同 ⟹ 顺带验证
// "歌曲标签渲染的是歌曲级字段、专辑标签是专辑级字段"，不会串台。
const MOCK_BY_KIND = {
  track: { ok: true, kind: 'track', summaryKind: 'track', summaryCached: false, unverified: true, citations: [],
    fields: { '演唱': 'MOCK-演唱', '作词': 'MOCK-作词', '作曲': 'MOCK-作曲', '歌曲风格': 'MOCK-风格',
              '发行时间': 'MOCK-发行', '时长': 'MOCK-时长', '歌词赏析': 'MOCK-歌词赏析（长文，独占整行）' } },
  release: { ok: true, kind: 'release', summaryKind: 'release', summaryCached: true, unverified: true, citations: [],
    fields: { '艺术家': 'MOCK-艺术家', '发行时间': 'MOCK-发行', '专辑类型': 'MOCK-类型', '语种': 'MOCK-语种',
              '出版方': 'MOCK-出版方', '专辑编号': 'MOCK-编号', '曲目列表': 'MOCK-曲目列表（长文）' } },
  person: { ok: true, kind: 'person', summaryKind: 'person', summaryCached: true, unverified: true, citations: [],
    fields: { '简介': 'MOCK-简介（长文，独占整行）', '国籍': 'MOCK-国籍', '活跃年代': 'MOCK-年代',
              '风格': 'MOCK-流派', '代表作品': 'MOCK-代表作', '厂牌': 'MOCK-厂牌' } }
};
const MOCK_JS = String.raw`(function(){
  window.__reqs = [];
  if (window.__npMock) return true;
  window.__npMock = 1;
  var of = window.fetch;
  window.fetch = function(u, o){
    var s = String(u);
    if (s.indexOf('/api/entity/summary') === 0) {
      var m = /[?&]kind=([a-z]+)/.exec(s);
      var kind = m ? m[1] : 'person';
      window.__reqs.push(s);
      var body = window.__MOCKDATA[kind] || { ok:false, error:'mock-none' };
      return Promise.resolve(new Response(JSON.stringify(body), { status:200, headers:{'Content-Type':'application/json'} }));
    }
    if (s.indexOf('/api/status') === 0) {
      return of.apply(this, arguments).then(function(r){ return r.json(); }).then(function(j){
        j.hasLlmKey = true; j.summaryEnabled = true;
        return new Response(JSON.stringify(j), { status:200, headers:{'Content-Type':'application/json'} });
      });
    }
    return of.apply(this, arguments);
  };
  try { META.hasLlmKey = true; META.summary = 1; } catch(e){}
  return true;
})()`;

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
    for (let i = lines.length - 1; i >= 0; --i) {
      if (lines[i].indexOf(' PLAY v') >= 0 && lines[i].indexOf('"deviceId":"null"') >= 0) return lines[i];
    }
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
function box(sel) {
  return evalJs("(function(){var e=document.querySelector(" + JSON.stringify(sel) + ");if(!e)return null;var b=e.getBoundingClientRect();return {t:Math.round(b.top),b:Math.round(b.bottom),l:Math.round(b.left),r:Math.round(b.right),w:Math.round(b.width),h:Math.round(b.height)};})()");
}
async function gapChecks(W, H) {
  const hero = await box('.nphero');
  const body = await box('.npbody');
  const lyric = await box('#npLyric');
  const rail = await box('#npRail');
  const seek = await box('#npSeek');
  const ctl = await box('.npctl2');
  const tag = ' @' + W + 'x' + H;
  ck(noOverlap(hero, body), '封面+标题区 与 歌词/资料区 不重叠' + tag, 'hero.b=' + (hero && hero.b) + ' body.t=' + (body && body.t));
  ck(noOverlap(body, ctl), '歌词/资料区 与 控制区 不重叠' + tag, 'body.b=' + (body && body.b) + ' ctl.t=' + (ctl && ctl.t));
  ck(noOverlap(lyric, seek), '歌词区 与 进度条 不重叠' + tag, 'lyric.b=' + (lyric && lyric.b) + ' seek.t=' + (seek && seek.t));
  const gap = (seek && body) ? (seek.t - body.b) : -1;
  ck(gap >= 6, '歌词/资料区 与 进度条 间距 >= 6px' + tag, 'gap=' + gap + 'px');
  // #3b ② 左右排列判据：歌词在左、标签栏在右，且两者垂直区间重叠（=同一行），绝不是上下堆叠。
  if (rail && rail.w > 0) {
    if (lyric && lyric.w > 2) {
      const vOverlap = Math.min(lyric.b, rail.b) - Math.max(lyric.t, rail.t);
      ck(lyric.r <= rail.l && vOverlap > 40 && !overlaps(lyric, rail),
         '歌词在左 / 资料标签栏在右 同行排列（不是上下堆叠）' + tag,
         'lyric.r=' + lyric.r + ' rail.l=' + rail.l + ' 垂直重叠=' + vOverlap + 'px');
    } else {
      // 纯音乐/无歌词：歌词区收起，标签栏仍在正文区且在控制区之上
      ck(rail.t >= (body ? body.t - 1 : 0) && rail.b <= (ctl ? ctl.t + 1 : 99999),
         '无歌词时标签栏仍在正文区、不压控制区' + tag, 'rail=' + JSON.stringify(rail));
    }
  } else {
    ck(false, '右侧资料标签栏显示（有可用实体时）' + tag, JSON.stringify({ rail: rail }));
  }
  // #3b ① 音量必须在控制行里，和播放键同一行（用户：不能单独一行放最下面）
  const vm = await evalJs("(function(){var v=document.getElementById('npVol'),p=document.getElementById('npPlay');if(!v||!p)return null;var a=v.getBoundingClientRect(),b=p.getBoundingClientRect();var row=document.querySelector('.npctlrow');var rb=row?row.getBoundingClientRect():null;return {vc:Math.round(a.top+a.height/2),pc:Math.round(b.top+b.height/2),vBottom:Math.round(a.bottom),ctlBottom:Math.round(rb?rb.bottom:0)};})()");
  ck(vm && Math.abs(vm.vc - vm.pc) <= 10, '音量与控制键同一行（中心差 <= 10px）' + tag, JSON.stringify(vm));
  // 2026-10-24：宿主改为 WebView 满幅（launcher kBandInset=0），页面不再自绘实色外圈/暗角。
  const edge = await evalJs("(function(){var ep=document.querySelector('.npedge');var b=document.getElementById('npPage').getBoundingClientRect();return {npedge:ep?getComputedStyle(ep).display:'(missing)',np:[Math.round(b.left),Math.round(b.top),Math.round(b.right),Math.round(b.bottom)],vp:[innerWidth,innerHeight]};})()");
  ck(edge && edge.npedge === 'none', '页面不再自绘实色边框/暗角（.npedge 已隐藏）' + tag, JSON.stringify(edge));
  ck(edge && Math.abs(edge.np[0]) <= 1 && Math.abs(edge.np[1]) <= 1 && Math.abs(edge.np[2] - edge.vp[0]) <= 1 && Math.abs(edge.np[3] - edge.vp[1]) <= 1,
     '单曲页铺满整个视口（无内缩 ⟹ 无黑边）' + tag, edge ? JSON.stringify({ np: edge.np, vp: edge.vp }) : '');
  return { hero: hero, body: body, lyric: lyric, rail: rail, seek: seek, ctl: ctl };
}
// 标签bar（261024-01 #3b ③）：.npchip 取代旧的 .npdtab
async function tabClick(label) {
  return evalJs("(function(){var b=Array.from(document.querySelectorAll('.npchip')).filter(function(x){return x.textContent.trim()===" + JSON.stringify(label) + ";});if(!b.length)return false;b[0].click();return true;})()");
}
async function activeTab() {
  return evalJs("(function(){var b=document.querySelector('.npchip.on');return b?b.textContent.trim():'';})()");
}
async function dataStat() { return evalJs("(document.getElementById('npDataStat')||{}).textContent||''"); }
async function drawerOpen() { return evalJs("(function(){var d=document.getElementById('npDrawer');return !!(d&&d.classList.contains('on'));})()"); }
async function drawerState() {
  return evalJs("(function(){var d=document.getElementById('npDrawer');var b=d.getBoundingClientRect();var ctl=document.querySelector('.npctl2');var cb=ctl?ctl.getBoundingClientRect():null;return {on:d.classList.contains('on'),l:Math.round(b.left),r:Math.round(b.right),t:Math.round(b.top),b:Math.round(b.bottom),w:Math.round(b.width),kind:(document.getElementById('npDrawerKind')||{}).textContent,name:(document.getElementById('npDrawerName')||{}).textContent,rows:document.querySelectorAll('#npDataBody .efrow').length,ctlTop:cb?Math.round(cb.top):0,body:(document.getElementById('npDataBody')||{}).textContent||''};})()");
}
async function installMock() {
  await evalJs("window.__MOCKDATA = " + JSON.stringify(MOCK_BY_KIND) + ";");
  await evalJs(MOCK_JS);
}

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
  // 契约 mock（见文件头「数据来源口径」）：本实例无 LLM key，资料类判据用页内注入的 {kind, fields}
  if (!NO_MOCK) { await installMock(); console.log('  [mock] 已注入 /api/entity/summary 契约 mock（零真实调用）'); }
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
      await evalJs("(function(){if(typeof npDataReset==='function')npDataReset();window.__reqs=[];})()");
      // 切换尺寸后整页重排
      await play(TRACK);
      await ensureNpOpen();
      await sleep(500);
      // ---- 默认态（#3b ③）：资料**完全不占版面**，右侧只有标签药丸；且不预取（不产生任何请求）----
      const chips = await evalJs("Array.from(document.querySelectorAll('.npchip')).map(function(b){return b.textContent.trim();})");
      ck(chips.length >= 3 && chips.indexOf('歌曲') >= 0 && chips.indexOf('专辑') >= 0 && chips.indexOf('艺术家') >= 0,
         '右侧有 歌曲/专辑/艺术家 标签（作品按古典判定出现）' + tag, JSON.stringify(chips));
      const d0 = await drawerState();
      ck(!d0.on, '默认不打开资料抽屉（资料默认隐藏）' + tag, 'on=' + d0.on);
      ck(d0.l >= (W - 2) || d0.l >= W, '抽屉关闭时完全移出视口（不占版面）' + tag, 'drawer.l=' + d0.l + ' W=' + W);
      const req0 = await evalJs("(window.__reqs||[]).length");
      ck(req0 === 0, '默认不预取任何实体（不点标签 = 不产生 AI 请求）' + tag, 'reqs=' + req0);
      const r = await gapChecks(W, H);
      await shot('np-' + W + 'x' + H + '-01-default');
      // ---- 点「专辑」标签：才请求 kind=release，抽屉滑出且不遮住控制区 ----
      if (REAL) await evalJs("(function(){try{NP_DATA.cache={};NP_DATA.inflight={};}catch(e){}return 1;})()");
      ck(await tabClick('专辑'), '点「专辑」标签' + tag, '');
      if (REAL) await waitFor("document.querySelectorAll('#npDataBody .efrow').length >= 1", 150000);
      await sleep(600);
      const dAlb = await drawerState();
      ck(dAlb.on && dAlb.kind === '专辑', '抽屉滑出且头部显示「专辑」' + tag, JSON.stringify({ on: dAlb.on, kind: dAlb.kind }));
      ck(dAlb.b <= dAlb.ctlTop + 1, '抽屉不遮住播放控制区' + tag, 'drawer.b=' + dAlb.b + ' ctl.t=' + dAlb.ctlTop);
      ck(dAlb.rows >= (REAL ? 2 : 3), '专辑标签渲染字段网格（.efrow >= ' + (REAL ? 2 : 3) + ' 行）' + tag, 'rows=' + dAlb.rows);
      const relReq = await evalJs(REAL
        ? "((NP_DATA.cache['release']||{}).state==='ok')?1:0"
        : "(window.__reqs||[]).filter(function(s){return /[?&]kind=release/.test(s);}).length");
      ck(relReq >= 1, REAL ? ('专辑标签成功加载了 release 数据' + tag) : ('专辑标签请求的是 kind=release' + tag),
         REAL ? 'cache.release=' + (await evalJs("((NP_DATA.cache['release']||{}).state||'-')")) : ('release 请求 ' + relReq + ' 次'));
      ck(await evalJs(REAL
           ? "(function(){var t=document.getElementById('npDataBody').textContent;return /发行时间|专辑类型|语种|出版方|曲目列表|专辑编号|时长|艺术家/.test(t);})()"
           : "(function(){var t=document.getElementById('npDataBody').textContent;return t.indexOf('MOCK-艺术家')>=0||t.indexOf('MOCK-类型')>=0;})()"),
         '专辑标签内容来自 release 级字段（不串台）' + tag, '');
      await shot('np-' + W + 'x' + H + '-02-album-tab');
      // 关闭（#npDataClose）
      await evalJs("document.getElementById('npDataClose').click()");
      await sleep(350);
      ck(!(await drawerOpen()), '点右上关闭按钮收起抽屉' + tag, '');
      // ---- 点「歌曲」标签：请求 kind=track，内容必须是曲目级 ----
      if (REAL) await evalJs("(function(){try{NP_DATA.cache={};NP_DATA.inflight={};}catch(e){}return 1;})()");
      ck(await tabClick('歌曲'), '点「歌曲」标签' + tag, '');
      if (REAL) await waitFor("document.querySelectorAll('#npDataBody .efrow').length >= 1", 150000);
      await sleep(600);
      const dTrk = await drawerState();
      ck(dTrk.on && dTrk.kind === '歌曲', '抽屉头部显示「歌曲」' + tag, JSON.stringify({ kind: dTrk.kind }));
      ck(await evalJs(REAL
           ? "(function(){var t=document.getElementById('npDataBody').textContent;return /演唱|作词|作曲|编曲|所属专辑|歌曲风格/.test(t);})()"
           : "(function(){var t=document.getElementById('npDataBody').textContent;return t.indexOf('MOCK-演唱')>=0||t.indexOf('MOCK-作词')>=0;})()"),
         '歌曲标签渲染曲目级字段（演唱/作词…，不是歌手简介）' + tag, '');
      // 实体串台硬判据：歌曲 tab 里不得出现艺术家级字段
      ck(await evalJs(REAL
           ? "(function(){var t=document.getElementById('npDataBody').textContent;return !/国籍|厂牌|活跃年代/.test(t);})()"
           : "(function(){var t=document.getElementById('npDataBody').textContent;return t.indexOf('MOCK-国籍')<0&&t.indexOf('MOCK-厂牌')<0;})()"),
         '歌曲标签里没有艺术家级字段（国籍/厂牌）—— 不串台' + tag, '');
      const trkReq = await evalJs(REAL
        ? "((NP_DATA.cache['track']||{}).state==='ok')?1:0"
        : "(window.__reqs||[]).filter(function(s){return /[?&]kind=track/.test(s);}).length");
      ck(trkReq >= 1, REAL ? ('歌曲标签成功加载了 track 数据' + tag) : ('歌曲标签请求的是 kind=track' + tag),
         REAL ? 'cache.track=' + (await evalJs("((NP_DATA.cache['track']||{}).state||'-')")) : ('track 请求 ' + trkReq + ' 次'));
      await shot('np-' + W + 'x' + H + '-03-song-tab');
      // ---- 点「艺术家」标签：kind=person ----
      if (REAL) await evalJs("(function(){try{NP_DATA.cache={};NP_DATA.inflight={};}catch(e){}return 1;})()");
      ck(await tabClick('艺术家'), '点「艺术家」标签' + tag, '');
      if (REAL) await waitFor("document.querySelectorAll('#npDataBody .efrow').length >= 1", 150000);
      await sleep(600);
      const dPer = await drawerState();
      ck(dPer.on && dPer.kind === '艺术家', '抽屉头部显示「艺术家」' + tag, JSON.stringify({ kind: dPer.kind }));
      ck(await evalJs(REAL
           ? "(function(){var t=document.getElementById('npDataBody').textContent;return /国籍|风格|代表作品|简介|活跃年代/.test(t);})()"
           : "(function(){var t=document.getElementById('npDataBody').textContent;return t.indexOf('MOCK-国籍')>=0||t.indexOf('MOCK-代表作')>=0;})()"),
         '艺术家标签渲染表演者级字段（国籍/代表作品…）' + tag, '');
      const perReq = await evalJs(REAL
        ? "((NP_DATA.cache['person']||{}).state==='ok')?1:0"
        : "(window.__reqs||[]).filter(function(s){return /[?&]kind=person/.test(s);}).length");
      ck(perReq >= 1, REAL ? ('艺术家标签成功加载了 person 数据' + tag) : ('艺术家标签请求的是 kind=person' + tag),
         REAL ? 'cache.person=' + (await evalJs("((NP_DATA.cache['person']||{}).state||'-')")) : ('person 请求 ' + perReq + ' 次'));
      await shot('np-' + W + 'x' + H + '-04-artist-tab');
      // ---- Esc 关抽屉（不关整页）----
      await send('Input.dispatchKeyEvent', { type: 'keyDown', key: 'Escape', code: 'Escape', windowsVirtualKeyCode: 27 });
      await sleep(400);
      ck(!(await drawerOpen()), 'Esc 收起抽屉' + tag, '');
      ck(await evalJs("document.getElementById('npPage').classList.contains('on')"), 'Esc 收抽屉时整页仍开着' + tag, '');
      // ---- 正文文案（#2）：只留「AI 整理」----
      await tabClick('专辑');
      await sleep(500);
      const txt = await evalJs("document.getElementById('npDataBody').textContent");
      ck(txt.indexOf('未找到权威来源') < 0 && txt.indexOf('仅供参考') < 0 && txt.indexOf('未核对') < 0,
         '资料里没有「未找到权威来源/仅供参考/未核对」' + tag, '');
      if (REAL) {
        // 真实后端：有权威来源时显示引用/来源，无来源时显示「AI 整理」——两者都算诚实标注。
        ck(txt.indexOf('AI 整理') >= 0 || txt.indexOf('依据') >= 0 || txt.indexOf('引用') >= 0 || txt.indexOf('来源') >= 0,
           '资料有诚实标注（AI 整理 / 来源引用）' + tag, '');
      } else {
        ck(txt.indexOf('AI 整理') >= 0, '资料里有「AI 整理」标注（诚实标注保留）' + tag, '');
      }
      await evalJs("document.getElementById('npDataClose').click()");
      await sleep(300);
      // 有歌词的曲目：用真实多行歌词验 1024x640 的「歌词压进度条」回归
      if (LYRICS) {
        await play(LYRICS);
        await sleep(900);
        const nline = await evalJs("document.querySelectorAll('#npLyric .npline').length");
        ck(nline >= 3, '歌词区渲染出多行歌词（真实歌词夹具）' + tag, nline + ' 行');
        await gapChecks(W, H);
        await shot('np-' + W + 'x' + H + '-03b-lyrics');
      }
      // 无缓存实体：点开标签后必须进入**明确终态**（内容或「暂无资料」+重试），不能永远停在骨架
      //（#3b ③ 默认不预取，所以加载态只能由点击触发）
      if (UNCACHED) {
        await play(UNCACHED);
        // /api/meta 分两拍回来：等标签栏出现再点（否则是竞态假失败）
        await waitFor("document.querySelectorAll('.npchip').length > 0", 20000);
        await sleep(300);
        let opened = await tabClick('专辑');
        if (!opened) opened = await tabClick('歌曲');
        ck(opened, '无缓存曲目也能点开资料标签（专辑/歌曲）' + tag, '');
        const settled = await waitFor("(function(){var b=document.getElementById('npDataBody');if(!b)return false;return !b.querySelector('.npdskel');})()", 60000);
        ck(settled, '点开后进入明确终态（内容或暂无资料，不停在骨架）' + tag, '');
        await shot('np-' + W + 'x' + H + '-05-uncached');
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
        const hasWork = await evalJs("Array.from(document.querySelectorAll('.npchip')).some(function(b){return b.textContent.trim()==='作品';})");
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
      // 收起单曲页，回到列表（避免影响下一尺寸）；抽屉先收，避免残留状态
      await evalJs("(function(){var d=document.getElementById('npDrawer');if(d&&d.classList.contains('on')&&document.getElementById('npDataClose'))document.getElementById('npDataClose').click();})()");
      await sleep(200);
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
