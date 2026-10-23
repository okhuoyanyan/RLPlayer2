// uicheck.js — 用 CDP 真跑一遍网页界面（点专辑、看曲目、抓 JS 异常）
//
// 为什么需要它：界面里任何一个 null 元素都会让后续所有事件绑定静默失效
// （本项目真踩过：D('bStop') 为 null，导致下一首/暂停/拖动进度全部失灵，
//   而页面本身看起来完全正常）。所以必须真的在浏览器里跑一遍并收集异常。
//
// 用法：node tools/uicheck.js <url> [截图输出目录]
const { spawn } = require('child_process');
const fs = require('fs');
const path = require('path');
const os = require('os');

const URL_ = process.argv[2] || 'http://127.0.0.1:8902/';
const OUT = process.argv[3] || path.join(os.tmpdir(), 'rta_uicheck');
fs.mkdirSync(OUT, { recursive: true });
const OUT_UI3 = path.join(__dirname, '..', 'build', 'ui3');
fs.mkdirSync(OUT_UI3, { recursive: true });

const EDGE = ['C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\msedge.exe',
              'C:\\Program Files\\Microsoft\\Edge\\Application\\msedge.exe'].find(p => fs.existsSync(p));
if (!EDGE) { console.error('找不到 Edge'); process.exit(2); }

// 端口和用户目录都带随机后缀：否则上一次没退干净的 headless Edge 会占着 9333，
// 新实例连不上，自检就假失败（本项目刚踩过）
const PORT = 9400 + Math.floor(Math.random() * 400);
const udd = path.join(os.tmpdir(), 'edge_uicheck_' + process.pid + '_' + Date.now());
const child = spawn(EDGE, ['--headless=new', '--disable-gpu', '--no-first-run', '--no-default-browser-check',
  '--user-data-dir=' + udd, '--remote-debugging-port=' + PORT, '--window-size=1600,1000', 'about:blank'],
  { stdio: 'ignore' });

const sleep = ms => new Promise(r => setTimeout(r, ms));

async function targets() {
  for (let i = 0; i < 60; i++) {
    try {
      const r = await fetch('http://127.0.0.1:' + PORT + '/json/list');
      const j = await r.json();
      const p = j.filter(t => t.type === 'page');
      if (p.length) return p;
    } catch (e) { /* 还没起来 */ }
    await sleep(250);
  }
  throw new Error('DevTools 端口没起来');
}

let ws, msgId = 0;
const pending = new Map();
const errors = [];

function send(method, params) {
  return new Promise((res, rej) => {
    const id = ++msgId;
    pending.set(id, { res, rej });
    ws.send(JSON.stringify({ id, method, params: params || {} }));
  });
}
async function evalJs(expr) {
  const r = await send('Runtime.evaluate', { expression: expr, awaitPromise: true, returnByValue: true });
  if (r.exceptionDetails) {
    const d = r.exceptionDetails;
    const msg = (d.exception && (d.exception.description || d.exception.value)) || d.text || JSON.stringify(d);
    throw new Error('JS 异常: ' + msg + '\n  表达式: ' + String(expr).slice(0, 160));
  }
  return r.result && r.result.value;
}
async function shot(name, toUi3) {
  const r = await send('Page.captureScreenshot', { format: 'png' });
  const p = path.join(OUT, name + '.png');
  fs.writeFileSync(p, Buffer.from(r.data, 'base64'));
  if (toUi3) {
    const p3 = path.join(OUT_UI3, name + '.png');
    fs.writeFileSync(p3, Buffer.from(r.data, 'base64'));
  }
  return p;
}
async function waitFor(expr, ms = 8000) {
  const t0 = Date.now();
  while (Date.now() - t0 < ms) {
    if (await evalJs('!!(' + expr + ')')) return true;
    await sleep(120);
  }
  return false;
}

(async () => {
  const pages = await targets();
  ws = new WebSocket(pages[0].webSocketDebuggerUrl);
  await new Promise((res, rej) => { ws.onopen = res; ws.onerror = rej; });
  ws.onmessage = ev => {
    const m = JSON.parse(ev.data);
    if (m.id && pending.has(m.id)) {
      const p = pending.get(m.id); pending.delete(m.id);
      m.error ? p.rej(new Error(JSON.stringify(m.error))) : p.res(m.result);
    } else if (m.method === 'Runtime.exceptionThrown') {
      errors.push('exception: ' + (m.params.exceptionDetails.exception && m.params.exceptionDetails.exception.description || m.params.exceptionDetails.text));
    } else if (m.method === 'Runtime.consoleAPICalled' && m.params.type === 'error') {
      errors.push('console.error: ' + m.params.args.map(a => a.value || a.description || '').join(' '));
    }
  };
  await send('Runtime.enable');
  await send('Page.enable');
  await send('Log.enable');

  const results = [];
  const ck = (ok, what, detail) => {
    const line = (ok ? '  [PASS] ' : '  [FAIL] ') + what + (detail ? '  ' + detail : '');
    results.push(line);
    console.log(line);
    return ok;
  };

  await send('Page.navigate', { url: URL_ });
  await sleep(400);
  ck(await waitFor("document.querySelector('#list') && document.querySelector('#list').children.length"), '首屏渲染出内容');
  ck(await waitFor("document.querySelectorAll('.card').length > 0", 8000), '首屏专辑卡片已就绪');
  const cards = await evalJs("document.querySelectorAll('.card').length");
  ck(cards > 0, '专辑卡片渲染', cards + ' 张');
  // 注意：判据要限定在 #list 内 —— 队列为空时右侧也会有一个 .empty，
  // 用 document.querySelector 会误判（这个判据本身错过一次）
  ck(await evalJs("!document.querySelector('#list .empty')"), '主内容区没有落到空状态');
  const p1 = await shot('01-albums');

  // ---- 验收 1：导航里有「EQ 与房间校正」；「古典作品」已按 261023-04 #9 移出导航 ----
  const navOk = await evalJs(`(function(){
    var navs = Array.from(document.querySelectorAll('.nav')).map(e => e.textContent.trim());
    var hasClassical = navs.some(t => t.indexOf('古典作品') >= 0);
    var hasEq = navs.some(t => t.indexOf('EQ 与房间校正') >= 0);
    return { ok: !hasClassical && hasEq, hasClassical: hasClassical, hasEq: hasEq, navs: navs };
  })()`);
  ck(navOk.ok, '导航里没有「古典作品」、仍有「EQ 与房间校正」', JSON.stringify(navOk));
  await shot('01-nav-classical-eq', true);

  // 点第一张专辑
  await evalJs("document.querySelector('.card').click()");
  ck(await waitFor("document.querySelector('#list .ahead') || document.querySelector('#list .empty')", 8000), '专辑页打开');
  await sleep(500);
  const tr = await evalJs("document.querySelectorAll('.trow').length");
  ck(tr > 0, '曲目列表非空', tr + ' 首');
  // 自检不能假设用户的「在线资料整理」开关状态：关 → 不能有板块；开 → 允许出现（加载行或内容）
  ck(await evalJs("(META.summary === 0) ? !document.querySelector('.summary-sec') : true"),
     '开关关闭时专辑页不出现简介板块（开关开启时允许出现，由用户设置决定）');
  const p2 = await shot('02-album');

  // 曲目视图
  await evalJs("Array.from(document.querySelectorAll('.nav')).find(e=>e.dataset.v==='tracks').click()");
  await sleep(700);
  ck(await evalJs("document.querySelectorAll('.trow').length > 0"), '曲目视图有内容');
  const p3 = await shot('03-tracks');

  // ---- 验收 2：曲目页有排序控件，点收藏优先后顺序变化；点心形后 /api/fav/list 出现该文件，取消后消失 ----
  ck(await evalJs("!!document.getElementById('trackSort')"), '曲目页有排序控件');
  const favCycleOk = await evalJs(`(async function(){
    var rows = document.querySelectorAll('#list .trow');
    if (!rows.length) return { ok: false, msg: '没有曲目行' };
    var btn = rows[0].querySelector('.fav-btn');
    if (!btn) return { ok: false, msg: '没有心形按钮' };
    var f = btn.dataset.file;
    await fetch('/api/fav?file=' + encodeURIComponent(f) + '&on=0');
    var l0 = await fetch('/api/fav/list').then(r => r.json());
    if (l0.files.indexOf(f) >= 0) return { ok: false, msg: '初始清理失败' };
    btn.click();
    await new Promise(r => setTimeout(r, 450));
    var l1 = await fetch('/api/fav/list').then(r => r.json());
    if (l1.files.indexOf(f) < 0) return { ok: false, msg: '点心形后未在 /api/fav/list 出现' };
    btn.click();
    await new Promise(r => setTimeout(r, 450));
    var l2 = await fetch('/api/fav/list').then(r => r.json());
    if (l2.files.indexOf(f) >= 0) return { ok: false, msg: '再次点击未在 /api/fav/list 消失' };
    return { ok: true, file: f };
  })()`);
  ck(favCycleOk.ok, '点心形后 /api/fav/list 出现该文件，取消后消失', JSON.stringify(favCycleOk));

  const favSortOk = await evalJs(`(async function(){
    var rows = document.querySelectorAll('#list .trow');
    if (rows.length < 2) return { ok: false, msg: '曲目不足2首' };
    var titlesBefore = Array.from(document.querySelectorAll('#list .trow .tt')).map(e => e.textContent);
    var lastRow = rows[rows.length - 1];
    var lastBtn = lastRow.querySelector('.fav-btn');
    lastBtn.click();
    await new Promise(r => setTimeout(r, 400));
    var sel = document.getElementById('trackSort');
    sel.value = 'fav';
    sel.dispatchEvent(new Event('change'));
    await new Promise(r => setTimeout(r, 400));
    var titlesAfter = Array.from(document.querySelectorAll('#list .trow .tt')).map(e => e.textContent);
    var changed = (titlesBefore.join('||') !== titlesAfter.join('||'));
    var firstIsFav = document.querySelectorAll('#list .trow')[0].querySelector('.fav-btn').classList.contains('on');
    var activeFav = document.querySelector('#list .trow .fav-btn.on');
    if (activeFav) activeFav.click();
    sel.value = 'default';
    sel.dispatchEvent(new Event('change'));
    return { ok: changed && firstIsFav, changed: changed, firstIsFav: firstIsFav };
  })()`);
  ck(favSortOk.ok, '点收藏优先后顺序变化', JSON.stringify(favSortOk));
  await shot('02-tracks-sort-fav', true);

  // 艺术家
  await evalJs("Array.from(document.querySelectorAll('.nav')).find(e=>e.dataset.v==='artists').click()");
  await sleep(700);
  const arts = await evalJs("document.querySelectorAll('.card').length");
  ck(arts > 0, '艺术家视图有内容', arts + ' 位');

  // ---- P3④a：艺术家资料页（L2 权威文本 + 来源芯片 + 空态/离线）----
  await evalJs("document.querySelector('.card').click()");
  await sleep(800);
  ck(await waitFor("(function(){ return S.view === 'artist' && !!document.querySelector('.artist-head'); })()", 8000), '艺术家资料页可进入');
  ck(await evalJs("!!document.querySelector('.artist-head') && !!document.querySelector('.artist-head h1')"), '艺术家资料页头部区块存在');
  // 区块检查：有数据时生平与来源芯片存在，无数据时空态卡与重试按钮存在（避免强依赖外网连接成功）
  const profileState = await evalJs(`(function(){
    var hasSources = document.querySelectorAll('.chip-copy').length > 0;
    var hasBio = !!document.querySelector('.bio-sec');
    var hasEmpty = !!document.querySelector('.empty-card');
    var hasRetry = !!document.getElementById('bRetryKnowledge');
    if (hasSources && hasBio) return 'data-branch';
    if (hasEmpty && hasRetry) return 'empty-branch';
    return 'sources=' + hasSources + ' bio=' + hasBio + ' empty=' + hasEmpty + ' retry=' + hasRetry;
  })()`);
  ck(profileState === 'data-branch' || profileState === 'empty-branch',
     '艺术家资料页状态正常（有数据时有简介与来源芯片，无数据时有空态卡与重试按钮）', profileState);
  ck(await evalJs("document.querySelectorAll('a[href^=http]').length === 0"),
     '艺术家资料页无 http 外链（来源一律使用复制链接芯片）');
  if (profileState === 'data-branch') {
    await evalJs("document.querySelector('.chip-copy').click()");
    ck(await waitFor("(function(){ var t=document.getElementById('toast'); return t && t.classList.contains('on') && t.textContent.indexOf('已复制来源链接') >= 0; })()", 4000),
       '点击来源芯片弹出「已复制来源链接」toast');
  }
  const pArtist = await shot('20-artist-profile');
  await evalJs("document.getElementById('bBackArtists').click()");
  await sleep(500);
  ck(await waitFor("S.view === 'artists' && document.querySelectorAll('.card').length > 0"), '可从资料页返回艺术家列表');

  // 兼测有资料与缓存的艺术家（ZARD：测试 L2 权威文本、来源芯片与复制交互）
  await evalJs("openArtistProfile('ZARD')");
  await sleep(600);
  ck(await waitFor("(function(){ return S.view === 'artist' && document.querySelectorAll('.chip-copy').length > 0 && !!document.querySelector('.artist-head .sub') && document.querySelector('.artist-head .sub').textContent.indexOf('缓存') >= 0; })()", 12000),
     '打开有权威资料的艺术家（ZARD）渲染出生平与来源芯片');
  ck(await evalJs("document.querySelectorAll('.chip-copy').length >= 1"), '来源芯片存在（≥1 个）');
  ck(await evalJs("!!document.querySelector('.artist-head .sub') && document.querySelector('.artist-head .sub').textContent.indexOf('缓存') >= 0"),
     '离线/缓存标注显示正确（含「缓存 · YYYY-MM-DD 取回」）');
  await evalJs("document.querySelector('.chip-copy').click()");
  ck(await waitFor("(function(){ var t=document.getElementById('toast'); return t && t.classList.contains('on') && t.textContent.indexOf('已复制来源链接') >= 0; })()", 4000),
     '点击来源芯片弹出「已复制来源链接」toast');
  ck(await evalJs("document.querySelectorAll('a[href^=http]').length === 0"),
     '带资料页面无 http 外链（永不跳转外部浏览器）');
  // 2026-10-23 261023-03 #2/#3：不再有「AI 综合 / AI 生成 / 生成 AI 简介」字样与按钮。
  // 设置开关默认关闭 → 艺术家页不应出现任何简介板块（不请求、不留空板块、不闪加载行）。
  ck(await evalJs("!document.querySelector('.ai-sec') && ((META.summary === 0) ? !document.querySelector('.summary-sec') : true)"),
     '艺术家资料页不出现旧「AI 简介」板块（开关关闭时也不留空板块）');
  ck(await evalJs("(function(){var t=document.body.textContent||'';return t.indexOf('AI 综合')<0 && t.indexOf('AI 生成')<0 && t.indexOf('生成 AI 简介')<0;})()"),
     '艺术家资料页无「AI 综合 / AI 生成 / 生成 AI 简介」字样');
  const pZard = await shot('21-artist-profile-zard');
  await evalJs("document.getElementById('bBackArtists').click()");
  await sleep(500);

  // ---- P3④b：作品与 Discogs 版本推荐页（261023-04 #9：已无导航入口，从单曲页「作品」tab 进入）----
  await evalJs("loadWorksView()");
  await sleep(600);
  ck(await evalJs("S.view === 'works' && !!document.getElementById('wTitle')"), '作品页仍可进入（入口已移到单曲页「作品」tab）');
  ck(await evalJs("!!document.getElementById('wTitle') && !!document.getElementById('wComposer') && !!document.getElementById('bSearchVersions')"),
     '作品页搜索控件完整（作品名、作曲家输入框与查询按钮存在）');
  ck(await evalJs("document.querySelectorAll('.chip-work').length >= 3"),
     '常用古典作品快速芯片存在（≥3 个）');
  ck(await waitFor("document.querySelectorAll('.version-row').length >= 5 || !!document.querySelector('.empty-card')", 15000),
     'Discogs 版本列表成功渲染或显示明确空态卡');
  const nVers = await evalJs("document.querySelectorAll('.version-row').length");
  if (nVers > 0) {
    ck(nVers >= 5, 'Boléro 权威版本列表条数 >= 5', nVers + ' 条');
    ck(await evalJs("document.querySelectorAll('.version-row .chip-copy').length === " + nVers),
       '每个版本行均有复制链接芯片');
    await evalJs("document.querySelector('.version-row .chip-copy').click()");
    ck(await waitFor("(function(){ var t=document.getElementById('toast'); return t && t.classList.contains('on') && t.textContent.indexOf('已复制来源链接') >= 0; })()", 4000),
       '点击版本复制芯片弹出「已复制来源链接」toast');
  } else {
    ck(await evalJs("!!document.querySelector('.empty-card') && !!document.getElementById('bRetryVersions')"),
       '网络离线时显示空态卡与重试按钮');
  }
  ck(await evalJs("document.querySelectorAll('a[href^=http]').length === 0"),
     '作品版本推荐页无 http 外链（永不跳转外部浏览器）');
  const pWorks = await shot('22-work-versions');

  // 文件夹
  await evalJs("Array.from(document.querySelectorAll('.nav')).find(e=>e.dataset.v==='files').click()");
  await sleep(700);
  ck(await evalJs("document.querySelectorAll('.row').length > 0"), '文件夹视图有内容');
  const p4 = await shot('04-files');

  // 设置面板 + 队列面板
  await evalJs("Array.from(document.querySelectorAll('.nav')).find(e=>e.dataset.v==='settings').click()");
  await sleep(400);
  ck(await evalJs("document.getElementById('setpage').classList.contains('on')"), '设置页可打开');
  ck(await evalJs("document.getElementById('app').classList.contains('noq')"), '设置页隐藏右侧队列栏');
  ck(await evalJs("document.getElementById('roots2').children.length >= 0"), '库目录列表已渲染');
  const p5 = await shot('05-settings');

  // ---- 新增：主题 / 完整设置页 ----
  ck(await evalJs("document.querySelectorAll('.sw').length === 3"), '三个主题切换按钮存在');
  await evalJs("document.querySelector('.sw[data-theme=light]').click()");
  await sleep(300);
  ck(await evalJs("document.documentElement.dataset.theme === 'light'"), '可以切到白色主题');
  const th1 = await shot('17-theme-light');
  await evalJs("document.querySelector('.sw[data-theme=blue]').click()");
  await sleep(300);
  ck(await evalJs("document.documentElement.dataset.theme === 'blue'"), '可以切到浅蓝主题');
  const th2 = await shot('18-theme-blue');
  await evalJs("document.querySelector('.sw[data-theme=dark]').click()");
  await sleep(250);
  ck(await evalJs("document.documentElement.dataset.theme === 'dark'"), '可以切回深色主题');

  await evalJs("Array.from(document.querySelectorAll('.nav')).find(e=>e.dataset.v==='settings').click()");
  await sleep(600);
  ck(await evalJs("document.getElementById('setpage').classList.contains('on')"), '设置页可打开');
  const tabOrder = await evalJs("Array.from(document.querySelectorAll('.stab')).map(e=>e.textContent).join('/')");
  // 2026-10-22 功能3：「诊断」独立成页，排在元数据之后
  ck(tabOrder === '输入/输出/PCM/SDM/高级/元数据/诊断',
     '设置页签顺序为 输入/输出/PCM/SDM/高级/元数据/诊断', tabOrder);

  // ---- P4b：设置页输入面板自检（控件绑定/设备下拉/起停状态/状态渲染/无外链）----
  await evalJs("document.querySelector('.stab[data-g=in]').click()");
  await sleep(400);
  ck(await evalJs("(function(){var p=document.querySelector('#setpage .spane[data-g=in]');return !!p && p.classList.contains('on');})()"),
     '输入页签可切换且当前处于激活状态');

  const inControlsOk = await evalJs(`(function(){
    var ids = ['sp_inbackend', 'sp_indev', 'sp_inbuf', 'sp_inch', 'sp_inmon', 'sp_inseconds', 'sp_inout', 'sp_instart', 'sp_instop'];
    var pane = document.querySelector('#setpage .spane[data-g=in]');
    var miss = [];
    ids.forEach(function(id) {
      var e = document.getElementById(id);
      if (!e || !pane.contains(e)) miss.push(id + ':缺失');
      else if (!e.onclick && !e.onchange && !e.oninput) miss.push(id + ':未绑定事件');
    });
    return miss;
  })()`);
  ck(Array.isArray(inControlsOk) && inControlsOk.length === 0, '输入页签控件全部存在且已绑定事件', JSON.stringify(inControlsOk));

  const devState = await evalJs(`(function(){
    var s = document.getElementById('sp_indev');
    if (!s) return 'no-select';
    var opts = s.querySelectorAll('option');
    var msg = (document.getElementById('sp_inmsg') || {}).textContent || '';
    if (opts.length > 1) return 'has-options:' + opts.length;
    if (opts.length === 1 && (opts[0].value === '' || msg.indexOf('未检测到') >= 0 || msg.indexOf('没有') >= 0)) return 'empty-state';
    return 'opts=' + opts.length + ' msg=' + msg;
  })()`);
  ck(devState.indexOf('has-options') === 0 || devState === 'empty-state',
     '输入设备下拉有选项或明确空态', devState);

  const inBtnState = await evalJs(`(function(){
    var start = document.getElementById('sp_instart');
    var stop = document.getElementById('sp_instop');
    if (!start || !stop) return null;
    return { startDisabled: start.disabled, stopDisabled: stop.disabled };
  })()`);
  ck(inBtnState && inBtnState.startDisabled === false && inBtnState.stopDisabled === true,
     '空闲时开始按钮可用且停止按钮禁用', JSON.stringify(inBtnState));

  const monTextOk = await evalJs(`(function(){
    var mon = document.getElementById('sp_inmon');
    if (!mon) return false;
    var parent = mon.closest('.sfld');
    var txt = parent ? parent.textContent : '';
    return txt.indexOf('直通监听（非 bit-perfect）') >= 0;
  })()`);
  ck(monTextOk, '监听开关包含「直通监听（非 bit-perfect）」文案');

  const statRenderOk = await evalJs(`(function(){
    var pane = document.querySelector('#setpage .spane[data-g=in]');
    var ids = ['sp_stat_running', 'sp_stat_frames', 'sp_stat_spec', 'sp_stat_water', 'sp_stat_xrun', 'sp_stat_clips', 'sp_stat_rms', 'sp_stat_peak', 'sp_stat_out', 'sp_meter_water', 'sp_meter_rms', 'sp_meter_peak'];
    var miss = [];
    ids.forEach(function(id) {
      var e = document.getElementById(id);
      if (!e || !pane.contains(e)) miss.push(id);
    });
    return miss;
  })()`);
  ck(Array.isArray(statRenderOk) && statRenderOk.length === 0,
     '输入状态面板指标与电平条完整渲染', JSON.stringify(statRenderOk));

  ck(await evalJs("document.querySelectorAll('#setpage .spane[data-g=in] a[href^=http]').length === 0"),
     '输入页签无 http 外链');
  const pInput = await shot('24-input-panel');

  await evalJs("document.querySelector('.stab[data-g=pcm]').click()");
  await sleep(200);
  ck(await evalJs("!document.getElementById('sp_trim0') || !document.getElementById('sp_trim0').offsetParent"), '声道微调只出现在输出页');
  ck(await evalJs("!!document.getElementById('sp_sdmo1') && !!document.getElementById('sp_dsdrate')"), 'SDM 页有 Oversampling 与比特率');
  ck(await evalJs("!!document.getElementById('sp_dsdkind') && !!document.getElementById('sp_dsdconv') && !!document.getElementById('sp_dsdkind').onchange && !!document.getElementById('sp_dsdconv').onchange"), 'SDM 页有 DSD 源抽取器 / Conversion 下拉且已绑定');
  // ---- DirectSDM 开关（本轮新增）：存在 / 在 SDM 页 / 能落盘 / 跑完还原 ----
  const origDm = await evalJs("fetch('/api/settings').then(r=>r.json()).then(j=>({dm:j.kv.directsdm||'0', vol:j.kv.volume||'0'}))");
  await evalJs("document.querySelector('.stab[data-g=sdm]').click()");
  ck(await waitFor("(function(){var e=document.getElementById('sp_directsdm');return !!e&&e.type==='checkbox'&&e.closest('.spane')&&e.closest('.spane').dataset.g==='sdm';})()", 5000),
     'SDM 页有 DirectSDM 开关（且只在 SDM 页）');
  ck(await evalJs("!!document.getElementById('sp_directsdm').onchange"), 'DirectSDM 开关已绑定事件');
  await evalJs("var e=document.getElementById('sp_directsdm'); e.checked=true; if (e.onchange) e.onchange(); else e.dispatchEvent(new Event('change'));");
  ck(await waitFor("fetch('/api/settings').then(r=>r.json()).then(j=>String(j.kv.directsdm)==='1')", 6000), 'DirectSDM 开关能保存到后端');
  await evalJs("var e=document.getElementById('sp_directsdm'); e.checked=" + (origDm.dm === '1') + "; if (e.onchange) e.onchange(); else e.dispatchEvent(new Event('change'));");
  ck(await waitFor("fetch('/api/settings').then(r=>r.json()).then(j=>String(j.kv.directsdm)==='" + String(origDm.dm) + "')", 6000), 'DirectSDM 开关还原');
  // 开关会把音量压到 0 dB，必须一起还原（自检不许改用户设置）
  await evalJs("S.vol=" + Number(origDm.vol) + "; var v=document.getElementById('vol'); if(v) v.value=" + Number(origDm.vol) + "; saveSettings(true);");
  ck(await waitFor("fetch('/api/settings').then(r=>r.json()).then(j=>String(j.kv.volume)==='" + String(origDm.vol) + "')", 6000), 'DirectSDM 自检后音量还原');
  // ---- 本轮新增：元数据补全页（分组 / 动态源开关 / metaon 往返 / 诊断按钮）----
  await evalJs("document.querySelector('.stab[data-g=meta]').click()");
  ck(await waitFor("(function(){var p=document.querySelector('#setpage .spane[data-g=meta]');if(!p||!p.classList.contains('on'))return false;var g=false;Array.prototype.forEach.call(p.querySelectorAll('.sghead'),function(e){if(e.textContent.indexOf('元数据补全')>=0)g=true});return g;})()", 5000),
     '元数据页有「元数据补全」分组（且页签能切过去）');
  // P1.5：源开关动态渲染 —— /api/providers 返回注册表，页面必须按 items 逐源渲染出
  // sp_meta_<id> 开关（旧的六开关硬编码断言已改为动态版）。
  const provs = await evalJs("fetch('/api/providers').then(r=>r.json())");
  // 注册表项数随「新增数据源」增长（P1.5 四源 → 2026-10-22 dizzylab 接入后五源）。
  // 判据：至少 4 项 + 下面按 id 逐项核对能力位；**不写死总数**，否则每次加源都要改测试
  //（这与「加源=一个描述符+两函数，零接线」的设计目标相悖）。
  ck(provs && provs.ok === true && Array.isArray(provs.items) && provs.items.length >= 4,
     '/api/providers 返回 ≥4 项注册表', JSON.stringify(provs && provs.items && provs.items.map(i => i.id)));
  if (provs && provs.ok && Array.isArray(provs.items)) {
    const caps = provs.items.map(i => i.id + ':' + (i.canSearch ? 'S' : '-') + (i.canLyric ? 'L' : '-') + ':' + i.name).join(' ');
    ck(provs.items.some(i => i.id === 'qq' && i.canSearch && i.canLyric) &&
       provs.items.some(i => i.id === 'netease' && i.canSearch && i.canLyric) &&
       provs.items.some(i => i.id === 'itunes' && i.canSearch && !i.canLyric) &&
       provs.items.some(i => i.id === 'lrclib' && !i.canSearch && i.canLyric),
       '注册表能力位正确(qq/netease=搜索+歌词, itunes=仅搜索, lrclib=仅歌词)', caps);
    // dizzylab（2026-10-22 接入）：同人第一优先源，只搜不取词 —— 如实标注 canLyric=false
    ck(provs.items.some(i => i.id === 'dizzylab' && i.canSearch && !i.canLyric),
       'dizzylab 已注册且 canLyric=false（无歌词接口，如实）', caps);
    // 顺序也断言：dizzylab 必须排在 qq 之前（同人优先），但采纳门槛不变
    ck(provs.items.findIndex(i => i.id === 'dizzylab') >= 0 &&
       provs.items.findIndex(i => i.id === 'dizzylab') < provs.items.findIndex(i => i.id === 'qq'),
       '注册表顺序：dizzylab 排在 qq 之前（同人优先）', caps);
    const metaMiss = await evalJs("(function(){var p=document.querySelector('#setpage .spane[data-g=meta]');var miss=[];var items=" + JSON.stringify(provs.items.map(i => i.id)) + ";items.forEach(function(id){var i='sp_meta_'+id;var e=document.getElementById(i);if(!e||e.type!=='checkbox'||!p.contains(e)||!e.onchange)miss.push(i)});var box=document.getElementById('sp_metasrc');if(!box||!p.contains(box))miss.push('sp_metasrc:容器缺失');return miss;})()");
    ck(Array.isArray(metaMiss) && metaMiss.length === 0,
       '页面按 /api/providers 渲染出对应源开关且已绑定事件', JSON.stringify(metaMiss));
    // 动态开关往返：关 metasrc_qq → 落盘 → 还原（不裸 POST，走 UI onchange → saveSettings）
    const qqItem = provs.items.find(i => i.id === 'qq');
    const origQq = String(await evalJs("fetch('/api/settings').then(r=>r.json()).then(j=>j.kv.metasrc_qq||j.kv.metaqq||'1')"));
    await evalJs("var e=document.getElementById('sp_meta_qq'); e.checked=false; e.dispatchEvent(new Event('change'));");
    ck(await waitFor("fetch('/api/settings').then(r=>r.json()).then(j=>String(j.kv.metasrc_qq)==='0')", 6000) && String(qqItem.on) === String(origQq === '1'),
       '关「QQ音乐」源开关能保存到后端(metasrc_qq)');
    await evalJs("var e=document.getElementById('sp_meta_qq'); e.checked=" + (origQq === '1') + "; e.dispatchEvent(new Event('change'));");
    ck(await waitFor("fetch('/api/settings').then(r=>r.json()).then(j=>String(j.kv.metasrc_qq)==='" + origQq + "')", 6000),
       '「QQ音乐」源开关往返还原');
  }
  // 总开关、DeepSeek 开关与在线资料整理开关保持固定
  const metaFix = await evalJs("(function(){var ids=['sp_metaon','sp_metallm','sp_metasummary'];var p=document.querySelector('#setpage .spane[data-g=meta]');var miss=[];ids.forEach(function(i){var e=document.getElementById(i);if(!e||e.type!=='checkbox'||!p.contains(e)||!e.onchange)miss.push(i)});return miss;})()");
  ck(Array.isArray(metaFix) && metaFix.length === 0, '总开关、DeepSeek 开关与在线资料整理开关存在且已绑定', JSON.stringify(metaFix));

  // 2026-10-23 261023-03 #2：开关文案必须去掉「AI」字眼，并说明「自动整理 + 缓存」
  const summLabel = await evalJs("(function(){var e=document.getElementById('sp_metasummary');return (e&&e.parentNode)?e.parentNode.textContent.trim():'';})()");
  ck(summLabel.indexOf('在线整理音乐资料简介') >= 0 && summLabel.indexOf('AI') < 0,
     '资料简介开关文案已去掉 AI 字眼', summLabel);

  // metasummary 默认关(0)，可保存并往返还原。
  // 注意：验收单曲页资料面板的测试实例会显式把它设为 1，所以这里不能硬断言 conf 值；
  // 改为证明「UI 勾选态与后端值严格一致、没有被隐式打开」（默认代码值是 0）。
  const origSummary = String(await evalJs("fetch('/api/settings').then(r=>r.json()).then(j=>j.kv.metasummary||'0')"));
  ck(await evalJs("(function(){var e=document.getElementById('sp_metasummary');return !!e && (e.checked === (" + JSON.stringify(origSummary === '1') + "));})()"),
     'metasummary 开关状态与后端一致（默认 0，未被隐式打开）', 'backend=' + origSummary);
  await evalJs("var e=document.getElementById('sp_metasummary'); e.checked=true; e.dispatchEvent(new Event('change'));");
  ck(await waitFor("fetch('/api/settings').then(r=>r.json()).then(j=>String(j.kv.metasummary)==='1')", 6000),
     '打开「在线整理音乐资料简介」能保存到后端');
  await evalJs("var e=document.getElementById('sp_metasummary'); e.checked=" + (origSummary === '1') + "; e.dispatchEvent(new Event('change'));");
  ck(await waitFor("fetch('/api/settings').then(r=>r.json()).then(j=>String(j.kv.metasummary)==='" + origSummary + "')", 6000),
     '「在线整理音乐资料简介」往返还原');
  // metaon 关→开：走 UI 开关（onchange → saveSettings → POST /api/settings），往返一致。
  // 注意不能裸 POST 单键：/api/settings 的 POST 是整文件重写，只发一个键会把其余设置全抹掉。
  const origMetaon = String(await evalJs("fetch('/api/settings').then(r=>r.json()).then(j=>j.kv.metaon||'1')"));
  await evalJs("var e=document.getElementById('sp_metaon'); e.checked=false; e.dispatchEvent(new Event('change'));");
  ck(await waitFor("fetch('/api/settings').then(r=>r.json()).then(j=>String(j.kv.metaon)==='0')", 6000),
     '关闭「自动补全」能保存到后端');
  await evalJs("var e=document.getElementById('sp_metaon'); e.checked=" + (origMetaon === '1') + "; e.dispatchEvent(new Event('change'));");
  ck(await waitFor("fetch('/api/settings').then(r=>r.json()).then(j=>String(j.kv.metaon)==='" + origMetaon + "')", 6000),
     '「自动补全」往返还原');
  // ---- 功能4（2026-10-22）：DeepSeek API Key 的界面入口 ----
  // 判据是可判定的三条红线，不看文案：
  //   ① 输入框在「元数据补全」分组里、type=password、初始为空（不回填明文）；
  //   ② GET /api/llm 只回「是否已配置 + 掩码」，响应文本里不得有任何完整 sk- key；
  //   ③ 空 POST 必须被拒绝 —— 否则 GET/POST 撞车或前端 bug 一次就能清空用户 key。
  // 本自检【绝不写真实 key】：不点保存、不清除，只验存在性与掩码契约。
  ck(await evalJs("(function(){var p=document.querySelector('#setpage .spane[data-g=meta]');var e=document.getElementById('sp_llmkey');if(!p||!e||!p.contains(e))return 'missing-or-not-in-meta';if(e.type!=='password')return 'type='+e.type;if(String(e.value)!=='')return 'value-not-empty';return 'ok';})()") === 'ok',
     '元数据页有 API Key 输入框（password 类型、在元数据分组内、初始为空）');
  ck(await evalJs("!!document.getElementById('sp_llmsave') && !!document.getElementById('sp_llmsave').onclick && !!document.getElementById('sp_llmtest') && !!document.getElementById('sp_llmtest').onclick && !!document.getElementById('sp_llmclear') && !!document.getElementById('sp_llmclear').onclick"),
     '保存 Key / 测试连接 / 清除 Key 三个按钮存在且已绑定');
  const llm1 = await evalJs("fetch('/api/llm').then(function(r){return r.json()})");
  const llmMaskOk = await evalJs("fetch('/api/llm').then(r=>r.json()).then(function(j){if(!j||j.ok!==true)return 'no-ok';if(typeof j.configured!=='boolean')return 'no-configured';if(!j.path||!/rlplayer-llm\\.conf$/.test(j.path))return 'path='+j.path;if(!j.configured)return j.masked===''?'ok':'masked-nonempty';return (/^\\*{4}$/.test(j.masked)||/^.{3}\\*{4}.{4}$/.test(j.masked))?'ok':'masked='+j.masked;})");
  ck(llmMaskOk === 'ok', 'GET /api/llm 只返回「已配置 + 掩码」且路径指向 rlplayer-llm.conf', String(llmMaskOk));
  ck(llm1 && llm1.ok === true && llm1.apiKey === undefined && llm1.key === undefined,
     'GET /api/llm 没有 apiKey/key 明文字段', JSON.stringify(Object.keys(llm1 || {})));
  ck(await evalJs("fetch('/api/llm').then(r=>r.text()).then(function(t){return !/sk-[A-Za-z0-9_-]{16,}/.test(t);})"),
     'GET /api/llm 响应文本中没有任何完整 key（无 sk- 长串）');
  ck(await evalJs("(function(){var m=document.getElementById('sp_llmmsg');if(!m)return false;var t=m.textContent||'';return t.length>0 && !/sk-[A-Za-z0-9_-]{16,}/.test(t);})()"),
     '状态提示只显示掩码，不含明文 key');
  ck(await evalJs("!/sk-[A-Za-z0-9_-]{16,}/.test(document.documentElement.outerHTML)"),
     '整个页面 DOM 不含任何 key 明文');
  const emptyPost = await evalJs("fetch('/api/llm',{method:'POST',body:''}).then(r=>r.json())");
  ck(!!emptyPost && emptyPost.ok === false, '空 POST 被拒绝（不会误清空已配置的 key）', JSON.stringify(emptyPost));
  ck(await evalJs("document.querySelectorAll('#setpage .spane[data-g=meta] a[href^=http]').length === 0"), 'API Key 小节无外链');
  // ---- 诊断页签（2026-10-22 功能3：从元数据组独立出来，且改为导出 ZIP）----
  // 判据按用户原话来：
  //   ① 独立选项卡（在 .stabs 里，与输入/输出/PCM/SDM/高级/元数据平级）；
  //   ② 不再藏在元数据分组里；
  //   ③ 导出的是【一个 ZIP】，不是三份文件 —— 用端点真实返回的魔数验证，不看文案。
  ck(await evalJs("Array.from(document.querySelectorAll('.stab')).map(function(e){return e.textContent}).indexOf('诊断') >= 0"),
     '设置页有独立的「诊断」页签');
  ck(await evalJs("(function(){var p=document.querySelector('#setpage .spane[data-g=diag]');return !!p && p.dataset.g==='diag';})()"),
     '「诊断」是独立分页（.spane[data-g=diag]），不是塞在别的页里');
  ck(await evalJs("(function(){var m=document.querySelector('#setpage .spane[data-g=meta]');return m ? !m.contains(document.getElementById('sp_diagzip')) : false;})()"),
     '诊断导出按钮不在元数据分组里（已移出）');
  ck(await evalJs("!document.getElementById('sp_diag')"), '旧的单一 JSON「导出诊断信息」按钮已移除（sp_diag 不存在）');
  await evalJs("document.querySelector('.stab[data-g=diag]').click()");
  ck(await waitFor("document.querySelector('#setpage .spane[data-g=diag]') && document.querySelector('#setpage .spane[data-g=diag]').classList.contains('on')", 5000),
     '诊断页签能切过去');
  ck(await evalJs("!!document.getElementById('sp_diagzip') && !!document.getElementById('sp_diagzip').onclick && document.getElementById('sp_diagzip').textContent.indexOf('zip') >= 0"),
     '「导出诊断包（.zip）」按钮存在且已绑定');
  ck(await evalJs("!!document.getElementById('sp_diagjson') && document.getElementById('sp_diagjson').onclick"),
     '「只要 JSON」按钮保留（有人只想贴文本）');
  ck(await evalJs("document.querySelectorAll('#setpage .spane[data-g=diag] a[href^=http]').length === 0"),
     '诊断页无外链');
  // 端点必须真的返回 ZIP（PK\3\4）。只看文案会被"接口挂了但按钮还在"骗过。
  const zipSig = await evalJs("fetch('/api/diagzip').then(function(r){return r.arrayBuffer()}).then(function(b){var v=new Uint8Array(b);return b.byteLength+':'+v[0]+','+v[1]+','+v[2]+','+v[3]}).catch(function(){return 'fail'})");
  const zipOk = typeof zipSig === 'string' && zipSig.indexOf(':80,75,3,4') > 0;
  ck(zipOk, '/api/diagzip 返回真 ZIP（魔数 PK\\3\\4）', 'sig=' + zipSig);
  // 打诊断包会读日志（最大 2MB×3），HTTP 服务是阻塞式单线程（webui.h 头注）——
  // 不等它排空，后面的设置/搜索断言会被排队拖慢而假失败。
  await sleep(900);
  // ---- P2：标签写回小节（说明 / 预览 / 执行按钮）----
  // 注意：这里【绝不点「执行写回」】—— 自检可能跑在有 MetaStore 记录的实例上，
  // 点了会真的写标签。只验元素存在、预览（纯只读扫描）出 toast、按钮绑定齐全。
  const wbSec = await evalJs("(function(){var p=document.querySelector('#setpage .spane[data-g=meta]');var d=document.getElementById('sp_wbdesc');var a=document.getElementById('sp_wbdry');var b=document.getElementById('sp_wbapply');if(!p)return 'no-meta-pane';if(!d||!a||!b)return 'missing';if(!p.contains(d)||!p.contains(a)||!p.contains(b))return 'not-in-meta';if(d.textContent.indexOf('写回')<0||d.textContent.indexOf('逐位不变')<0||d.textContent.indexOf('backups')<0)return 'desc-text';return 'ok';})()");
  ck(wbSec === 'ok', '写回小节：说明（逐位不变 + 备份到 backups）与两按钮存在', String(wbSec));
  ck(await evalJs("!!document.getElementById('sp_wbdry').onclick && !!document.getElementById('sp_wbapply').onclick && document.getElementById('sp_wbdry').classList.contains('btn') && document.getElementById('sp_wbapply').classList.contains('btn')"), '预览/执行按钮是 .btn 且已绑定事件');
  ck(await evalJs("document.querySelectorAll('#setpage .spane[data-g=meta] a[href^=http]').length === 0"), '写回小节无外链');
  await evalJs("document.getElementById('sp_wbdry').click()");
  ck(await waitFor("(function(){var t=document.getElementById('toast');return t&&t.classList.contains('on')&&t.textContent.indexOf('首可写')>=0&&t.textContent.indexOf('首跳过')>=0})()", 10000), '预览写回出现「N 首可写，M 首跳过」toast');
  const wbToast = await evalJs("(function(){var t=document.getElementById('toast');return (t&&t.classList.contains('on'))?t.textContent:''})()");
  ck(await evalJs("fetch('/api/writeback/status').then(function(r){return r.json()}).then(function(j){return j.ok===true&&typeof j.running==='boolean'&&typeof j.done==='number'&&typeof j.total==='number'&&typeof j.written==='number'&&typeof j.skipped==='number'&&typeof j.failed==='number'&&Array.isArray(j.lastFailures)})"), 'status 端点字段齐全(running/done/total/written/skipped/failed/lastFailures)');
  ck(await evalJs("!document.getElementById('sp_wbdry').disabled && !document.getElementById('sp_wbapply').disabled"), '预览完成后两按钮恢复可用', 'toast=' + wbToast);
  await evalJs("document.querySelector('.stab[data-g=out]').click()");
  await waitFor("document.querySelector('#setpage .spane[data-g=out]') && document.querySelector('#setpage .spane[data-g=out]').classList.contains('on')", 5000);
  const nGroups = await evalJs("document.querySelectorAll('#setpage .sgroup').length");
  ck(nGroups >= 6, '设置页分了多组', nGroups + ' 组');
  const nFields = await evalJs("document.querySelectorAll('#setpage select, #setpage input').length");
  ck(nFields >= 15, '设置项数量足够', nFields + ' 个控件');
  // 261023-03 第6条：EQ 文件 / 房间校正 IR 已从设置页搬到独立「EQ 与房间校正」页，
  // 设置页只保留跳转入口（bGoEqNav）。所以这里接受「跳转入口」或「旧控件」任一形态，
  // 但抽头/衰减必须还在高级页 —— 否则就是高级项真的丢了。
  ck(await evalJs("!!document.getElementById('sp_taps') && !!document.getElementById('sp_atten') && (!!document.getElementById('bGoEqNav') || (!!document.getElementById('sp_ir') && !!document.getElementById('sp_eq')))"), '包含抽头/衰减/卷积/EQ 等高级项（EQ/IR 已独立成页，设置页保留入口）');
  ck(await evalJs("document.querySelectorAll('#sp_device option').length > 1"), '输出设备下拉有内容', (await evalJs("document.querySelectorAll('#sp_device option').length")) + ' 项');
  // ---- 本轮新增：WASAPI 共享后端（选项存在 / 说明文案 / 能保存；!! 跑完必须还原，
  //      与 taps 同一原则 —— 自检不许改用户的输出后端）----
  const origBackend = String(await evalJs("fetch('/api/settings').then(r=>r.json()).then(j=>j.kv.backend||'wasapi')"));
  ck(await evalJs("(function(){var s=document.getElementById('sp_backend');if(!s)return false;for(var i=0;i<s.options.length;++i)if(s.options[i].value==='wasapi-shared')return true;return false})()"), '后端下拉含「WASAPI 共享」选项');
  await evalJs("var s=document.getElementById('sp_backend'); s.value='wasapi-shared'; if (s.onchange) s.onchange(); else s.dispatchEvent(new Event('change'));");
  ck(await waitFor("(function(){var t=(document.getElementById('sp_backendmsg')||{textContent:''}).textContent; return t.indexOf('共享')>=0 && t.indexOf('bit-perfect')>=0 && ADV.backend==='wasapi-shared';})()", 6000), '选共享后端出现说明文案且 ADV.backend 更新');
  ck(await waitFor("fetch('/api/settings').then(r=>r.json()).then(j=>j.kv.backend==='wasapi-shared')", 6000), '共享后端能保存到后端');
  await evalJs("var s=document.getElementById('sp_backend'); s.value='" + origBackend + "'; if (s.onchange) s.onchange(); else s.dispatchEvent(new Event('change'));");
  ck(await waitFor("fetch('/api/settings').then(r=>r.json()).then(j=>j.kv.backend==='" + origBackend + "')", 6000), '自检跑完后把输出后端还原');
  // 改一个参数并保存，验证后端确实收到。
  // !! 必须还原：自检跑在用户真实实例上时，曾经把 taps 写成 131072 并落盘，
  //    用户打开设置页就看到一个莫名其妙的"原型抽头数"。自检不许改用户的设置。
  const origTaps = String(await evalJs("fetch('/api/settings').then(r=>r.json()).then(j=>j.kv.taps)"));
  await evalJs("document.getElementById('sp_taps').value='131072'");
  await evalJs("document.getElementById('sp_save').click()");
  ck(await waitFor("fetch('/api/settings').then(r=>r.json()).then(j=>String(j.kv.taps)==='131072')", 6000), '设置能保存到后端');
  await evalJs("document.getElementById('sp_taps').value='" + origTaps + "'");
  await evalJs("document.getElementById('sp_save').click()");
  ck(await waitFor("fetch('/api/settings').then(r=>r.json()).then(j=>String(j.kv.taps)==='" + origTaps + "')", 6000), '自检跑完后把设置还原');
  // 1x / Nx 分开选：改 Nx 必须真的落到 filterNx，且不能连 1x 一起改掉
  const before = await evalJs("fetch('/api/settings').then(r=>r.json()).then(j=>({f:j.kv.filter||'',nx:j.kv.filterNx||''}))");
  await evalJs("document.getElementById('sFiltNx').value='poly-sinc-short-mp'; if (D('sp_filtNx')) D('sp_filtNx').value='poly-sinc-short-mp'; saveSettings(true);");
  ck(await waitFor("fetch('/api/settings').then(r=>r.json()).then(j=>j.kv.filterNx==='poly-sinc-short-mp')", 6000), 'Nx 滤波器单独保存到 filterNx');
  const after = await evalJs("fetch('/api/settings').then(r=>r.json()).then(j=>({f:j.kv.filter||'',nx:j.kv.filterNx||''}))");
  ck(after.f === before.f, '改 Nx 不影响 1x', 'filter=' + after.f + '（原值 ' + before.f + '）');
  await evalJs("document.getElementById('sFiltNx').value='" + before.nx + "'; if (D('sp_filtNx')) D('sp_filtNx').value='" + before.nx + "'; saveSettings(true);");
  ck(await waitFor("fetch('/api/settings').then(r=>r.json()).then(j=>String(j.kv.filterNx||'')==='" + before.nx + "')", 6000), 'Nx 还原');
  const p10 = await shot('19-settings-page');

  // ---- P5b：设置页投放（UPnP）小节自检（控件绑定/渲染器下拉/DoP说明/状态渲染/无外链）----
  await evalJs("document.querySelector('.stab[data-g=out]').click()");
  await sleep(400);
  ck(await evalJs("(function(){var p=document.querySelector('#setpage .spane[data-g=out]');return !!p && p.classList.contains('on');})()"),
     '输出页签处于激活状态');

  const castControlsOk = await evalJs(`(function(){
    var ids = ['sp_cast_dev', 'sp_cast_scan', 'sp_cast_file', 'sp_cast_usecur', 'sp_cast_dop', 'sp_cast_start', 'sp_cast_stop'];
    var pane = document.querySelector('#setpage .spane[data-g=out]');
    var miss = [];
    ids.forEach(function(id) {
      var e = document.getElementById(id);
      if (!e || !pane.contains(e)) miss.push(id + ':缺失');
      else if (!e.onclick && !e.onchange && !e.oninput) miss.push(id + ':未绑定事件');
    });
    return miss;
  })()`);
  ck(Array.isArray(castControlsOk) && castControlsOk.length === 0, '投放小节控件全部存在且已绑定事件', JSON.stringify(castControlsOk));

  const castDevState = await evalJs(`(function(){
    var s = document.getElementById('sp_cast_dev');
    if (!s) return 'no-select';
    var opts = s.querySelectorAll('option');
    var msg = (document.getElementById('sp_cast_devmsg') || {}).textContent || '';
    if (opts.length > 1) return 'has-options:' + opts.length;
    if (opts.length === 1 && (opts[0].value === '' || msg.indexOf('未发现') >= 0 || opts[0].textContent.indexOf('未发现') >= 0)) return 'empty-state';
    return 'opts=' + opts.length + ' msg=' + msg;
  })()`);
  ck(castDevState.indexOf('has-options') === 0 || castDevState === 'empty-state',
     '渲染器下拉有内容或明确空态（无渲染器时）', castDevState);

  const castBtnState = await evalJs(`(function(){
    var start = document.getElementById('sp_cast_start');
    var stop = document.getElementById('sp_cast_stop');
    if (!start || !stop) return null;
    return { startDisabled: start.disabled, stopDisabled: stop.disabled };
  })()`);
  ck(castBtnState && castBtnState.startDisabled === false && castBtnState.stopDisabled === true,
     '投放空闲时开始按钮可用且停止按钮禁用', JSON.stringify(castBtnState));

  const dopTextOk = await evalJs(`(function(){
    var dop = document.getElementById('sp_cast_dop');
    if (!dop) return false;
    var parent = dop.closest('.sfld') || dop.parentElement;
    var txt = parent ? parent.textContent : '';
    return txt.indexOf('DoP') >= 0 && (txt.indexOf('DSD') >= 0 || txt.indexOf('拒绝') >= 0);
  })()`);
  ck(dopTextOk, 'DoP 开关有说明文案（DSD 源说明与拒绝提示）');

  const castStatOk = await evalJs(`(function(){
    var pane = document.querySelector('#setpage .spane[data-g=out]');
    var ids = ['sp_cast_stat_phase', 'sp_cast_stat_dev', 'sp_cast_stat_pos', 'sp_cast_stat_transport', 'sp_cast_stat_bytes', 'sp_cast_stat_reqs', 'sp_cast_stat_port', 'sp_cast_stat_ratematch'];
    var miss = [];
    ids.forEach(function(id) {
      var e = document.getElementById(id);
      if (!e || !pane.contains(e)) miss.push(id);
    });
    return miss;
  })()`);
  ck(Array.isArray(castStatOk) && castStatOk.length === 0,
     '状态面板字段完整渲染（phase/渲染器/位置/传输状态/供源字节/请求数/采样率等）', JSON.stringify(castStatOk));

  ck(await evalJs("document.querySelectorAll('#setpage .spane[data-g=out] a[href^=http]').length === 0"),
     '投放小节与输出页签无 http 外链');

  // ---- 高级设置页包含前往 EQ 页面跳转按钮 ----
  await evalJs("document.querySelector('.stab[data-g=adv]').click()");
  ck(await evalJs("!!document.getElementById('bGoEqNav')"), '高级设置页包含前往 EQ 与房间校正页面的跳转按钮');
  ck(await evalJs("document.querySelectorAll('#setpage .spane[data-g=adv] a[href^=http]').length === 0"),
     '高级页签无外链');

  // ---- 验收 3 & 4：EQ 与房间校正独立页面自检（261023-03 第6/7条） ----
  await evalJs("Array.from(document.querySelectorAll('.nav')).find(e=>e.dataset.v==='eq').click()");
  await sleep(700);
  ck(await evalJs("S.view === 'eq'"), '可进入独立「EQ 与房间校正」页面');
  ck(await evalJs("!!document.getElementById('sp_opra_search')"), '包含合并后的品牌/型号搜索框');

  // 判据 3a：EQ 页空输入时型号卡片数 == 0（最关键判据）
  const emptyCards = await evalJs("document.querySelectorAll('#eqModelCards .model-card').length");
  ck(emptyCards === 0, 'EQ 页空输入时型号卡片数 == 0（最关键判据）', 'cards=' + emptyCards);
  ck(await evalJs("document.querySelectorAll('#eqBrandChips .brand-chip').length >= 8"), '空输入显示引导与热门品牌芯片');
  await shot('03-eq-empty-guide', true);

  // 判据 3b：输入 HD 600 后有结果且 <=30 条
  await evalJs("var s=document.getElementById('sp_opra_search'); s.value='HD 600'; s.dispatchEvent(new Event('input'));");
  await waitFor("document.querySelectorAll('#eqModelCards .model-card').length > 0", 6000);
  const hdCards = await evalJs("document.querySelectorAll('#eqModelCards .model-card').length");
  ck(hdCards > 0 && hdCards <= 30, '输入 HD 600 后有结果且 <=30 条', 'cards=' + hdCards);
  await shot('04-eq-hd600-results', true);

  // 判据 4：调音预设是卡片布局（至少 3 张卡）
  await evalJs("Array.from(document.querySelectorAll('#eqModelCards .model-card')).find(c => c.dataset.name.indexOf('HD 600') >= 0 || c.dataset.id.indexOf('hd_600') >= 0).click()");
  await waitFor("document.querySelectorAll('#eqPresetCards .preset-card').length >= 3", 8000);
  const nPresets = await evalJs("document.querySelectorAll('#eqPresetCards .preset-card').length");
  ck(nPresets >= 3, '调音预设是卡片布局（至少 3 张卡）', 'count=' + nPresets);
  ck(await evalJs("document.querySelectorAll('#eqPresetCards .preset-card .opra-apply-btn').length === " + nPresets), '每张预设卡片均有应用按钮');
  ck(await evalJs("!!document.getElementById('eqTargetCurveInfo') && document.getElementById('eqTargetCurveInfo').style.display !== 'none'"), '选中后显示目标曲线说明');
  await shot('05-eq-presets-cards', true);

  ck(await evalJs("document.querySelectorAll('#list a[href^=http]').length === 0"), 'EQ 与房间校正页面无外链');
  ck(await evalJs("!!document.getElementById('sp_opra_copychip') && document.getElementById('sp_opra_copychip').classList.contains('chip')"), '存在复制官网链接芯片');

  // ---- 右栏标签页 / 歌词 / 播放列表 / 信号链 ----
  await evalJs("document.querySelector('.rtab[data-t=l]').click()");
  await sleep(500);
  ck(await evalJs("document.getElementById('lyricbox').style.display !== 'none'"), '歌词标签页可切换');
  ck(await evalJs("document.getElementById('lyricbox').innerHTML.length > 0"), '歌词区有内容（无歌词时也有提示）');
  const p7 = await shot('07-lyrics');
  await evalJs("document.querySelector('.rtab[data-t=q]').click()");
  await sleep(400);
  ck(await evalJs("document.getElementById('qlist').style.display !== 'none'"), '右栏队列标签页可切换');
  await evalJs("document.getElementById('bChain').click()");
  await sleep(600);
  ck(await evalJs("document.getElementById('modal').classList.contains('on')"), '信号链弹层可打开');
  ck(await evalJs("document.getElementById('mbody').innerHTML.length > 10"), '信号链有内容');
  const p9 = await shot('09-chain');
  await evalJs("document.getElementById('mclose').click()");
  await sleep(200);
  ck(await evalJs("!document.getElementById('modal').classList.contains('on')"), '信号链弹层可关闭');

  // 搜索：自适应已有曲库（用户库有 Isobel 则搜 Isobel，测试库自选已有曲目）
  await evalJs(`(function(){
    var q = document.getElementById('q');
    fetch('/api/lib/search?q=').then(r=>r.json()).then(j=>{
      var kw = 'Isobel';
      if (j.tracks && j.tracks.length) {
        var hasIso = j.tracks.some(t => (t.title||'').indexOf('Isobel') >= 0);
        if (!hasIso) kw = j.tracks[0].title || 'test';
      }
      q.value = kw;
      q.dispatchEvent(new Event('input'));
    });
  })()`);
  await waitFor("document.querySelectorAll('#list .trow').length > 0", 8000);
  const hits = await evalJs("document.querySelectorAll('#list .trow').length");
  ck(hits > 0, '全局搜索有结果', hits + ' 条');
  const p6 = await shot('06-search');

  // 交互元素是否都绑定上了（这是本项目真踩过的坑）
  const bound = await evalJs(`
    (function(){
      // 两类要分开看：
      //  needBind：点了/改了就要有反应，必须有事件处理
      //  valueOnly：参数类控件，值在"下一次播放"时被 params() 读取，本来就不该有处理函数
      //            （早期把两类混在一起判，导致把正常控件误报成"未绑定"）
      //  P1.5：元数据源开关是动态的（/api/providers 注册表）—— 在静态清单之外逐源补查
      //  P2：标签写回的预览/执行按钮也是要点了有反应的
      var needBind=['bStop','bNext','bPrev','bPause','qclr','tq','sRep','sShuf','vol','sLoud','srcSave','seek','bScan','bAddRoot','bRescan','bUpnp','bNet','bHelp','bLyric','bChain','sp_directsdm','sp_dsdkind','sp_dsdconv','sp_metaon','sp_metallm','sp_metasummary','sp_diagzip','sp_diagjson','sp_wbdry','sp_wbapply','sp_llmsave','sp_llmtest','sp_llmclear','npRep','npVol','bGoEqNav','sp_instart','sp_instop','sp_inmon','sp_cast_scan','sp_cast_usecur','sp_cast_start','sp_cast_stop','sp_cast_dop','sp_cast_dev'];
      (META_PROVIDERS || []).forEach(function (p) { needBind.push('sp_meta_' + p.id); });
      var valueOnly=['sRate','sFilt','sFiltNx','sShap','sBuf','sDSD','sIsp','s2s','sp_cast_file'];
      var miss=[];
      needBind.forEach(function(i){ var e=document.getElementById(i); if(!e) miss.push(i+':缺失'); else if(!e.onclick && !e.onchange && !e.oninput) miss.push(i+':未绑定'); });
      valueOnly.forEach(function(i){ if(!document.getElementById(i)) miss.push(i+':缺失'); });
      return miss;
    })()`);
  ck(Array.isArray(bound) && bound.length === 0, '所有控件都存在且已绑定事件', JSON.stringify(bound));

  // ---- 本轮新增：队列主视图 / 文件夹地址栏 / 正在播放页 / 循环键 / 拖拽遮罩 ----
  await evalJs("Array.from(document.querySelectorAll('.nav')).find(e=>e.dataset.v==='queue').click()");
  await sleep(700);
  ck(await evalJs("!!document.querySelector('#list .qrow') || !!document.querySelector('#list .empty')"), '播放队列有内容或明确的空状态');
  ck(await evalJs("document.getElementById('pth').textContent.indexOf('播放队列') >= 0"), '队列页标题正确');

  await evalJs("Array.from(document.querySelectorAll('.nav')).find(e=>e.dataset.v==='files').click()");
  await sleep(700);
  ck(await evalJs("!!document.getElementById('fPath')"), '文件夹有地址栏输入框');
  const addr = await evalJs("document.getElementById('fPath').value");
  ck(!!addr, '地址栏显示了当前目录', addr);

  await evalJs("document.getElementById('npOpen').click()");
  await sleep(400);
  ck(await evalJs("document.getElementById('npPage').classList.contains('on')"), '点封面可以打开正在播放页');
  ck(await evalJs("!!document.getElementById('npMeta') && document.getElementById('npMeta').closest('#npPage') !== null"), '正在播放页有 #npMeta 芯片容器');
  // 261023-04 #7：单曲页资料区改为多实体分区面板（歌曲/专辑/艺术家/作品），#npSummary 已移除
  ck(await evalJs("!!document.getElementById('npData') && document.getElementById('npData').closest('#npPage') !== null"), '正在播放页有 #npData 资料面板容器');
  ck(await evalJs("!!document.getElementById('npTabs') && !!document.getElementById('npDataBody')"), '资料面板有 tab 条与内容区');
  ck(await evalJs("!document.getElementById('npSummary')"), '旧的单一 #npSummary 简介容器已移除');
  // #6 几何回归：歌词区与进度条在任何窗口尺寸下不得重叠（1024x640 曾压在一起）
  ck(await evalJs("(function(){var a=document.getElementById('npLyric').getBoundingClientRect(),b=document.getElementById('npSeek').getBoundingClientRect();return a.bottom<=b.top && b.top-a.bottom>=12;})()"),
     '歌词区与进度条不重叠且间距 >= 12px');
  ck(await evalJs("(function(){var a=document.getElementById('npData').getBoundingClientRect(),b=document.getElementById('npSeek').getBoundingClientRect();return a.bottom<=b.top;})()"),
     '资料区与进度条不重叠');
  // 空态文案要用「清掉歌词后的 renderNpLyric」来验：实例上可能正停着一首带在线歌词的曲子
  // （LY.lines 非空 → 显示的是歌词而不是空态），那是正常业务态，不是文案缺失。
  ck(await evalJs("(function(){var keep=LY.lines;LY.lines=[];renderNpLyric();var ok=document.getElementById('npLyric').innerHTML.indexOf('在线补全')>=0;LY.lines=keep;var b=document.getElementById('npLyric');if(b){b.dataset.n='';}renderNpLyric();return ok;})()"), '歌词空态提示「播放时自动在线补全」');
  await evalJs("document.getElementById('npClose').click()");
  await sleep(250);
  ck(await evalJs("!!document.getElementById('npPlay') && !!document.getElementById('npSeek') && !!document.getElementById('npPrev')"), '正在播放页有播放控件');
  ck(await evalJs("!document.getElementById('npPage').classList.contains('on')"), '正在播放页可以关闭');

  // 自检纪律：自检绝不污染用户实例状态。任何触发状态变更的控件测试，
  // 测试前必须记录原值，测完必须通过界面/设置保存还原并断言后端值已恢复。
  const origRepSetting = await evalJs("fetch('/api/settings').then(r=>r.json()).then(j=>j.kv&&j.kv.repeat!==undefined?String(j.kv.repeat):null)");
  const rep0 = await evalJs("document.getElementById('sRep').value");
  await evalJs("document.getElementById('bRep').click()");
  await sleep(200);
  const rep1 = await evalJs("document.getElementById('sRep').value");
  ck(rep0 !== rep1, '循环键能切换循环方式', rep0 + ' -> ' + rep1);

  // 还原循环状态：通过界面点击还原，恢复至自检前原值（自检必须还原纪律）
  await evalJs(`(function() {
    var want = ${JSON.stringify(rep0)};
    var cur = document.getElementById('sRep') ? document.getElementById('sRep').value : '0';
    var tries = 0;
    while (cur !== want && tries < 3) {
      var btn = document.getElementById('bRep');
      if (btn) btn.click();
      cur = document.getElementById('sRep') ? document.getElementById('sRep').value : '0';
      tries++;
    }
  })()`);
  await sleep(200);
  const repRestored = await evalJs("document.getElementById('sRep').value");
  ck(repRestored === rep0, '循环键自检后界面状态还原', repRestored + ' === ' + rep0);
  ck(await waitFor("fetch('/api/queue').then(r=>r.json()).then(j=>String(j.repeat)==='" + String(rep0) + "')", 4000), '自检跑完后循环模式恢复后端状态');

  ck(await evalJs("!!document.getElementById('dropmask')"), '拖拽提示层已就绪');

  // 设计契约：UI 永不出现外部页面 —— 全页面不允许有任何 http(s) 外链锚点
  ck(await evalJs("document.querySelectorAll('a[href^=http]').length === 0"), '全页面无 http 外链（永不跳转外部页面）');

  ck(errors.length === 0, '没有 JS 异常 / console.error', errors.slice(0, 4).join(' | '));

  console.log(results.join(String.fromCharCode(10)));
  console.log('');
  console.log('截图目录: ' + OUT);
  ['01-albums', '02-album', '03-tracks', '04-files', '05-settings', '06-search', '20-artist-profile', '21-artist-profile-zard', '24-input-panel'].forEach(n => {
    const p = path.join(OUT, n + '.png');
    if (fs.existsSync(p)) console.log('  ' + n + ': ' + fs.statSync(p).size + ' 字节');
  });
  const bad = results.filter(r => r.includes('[FAIL]')).length;
  console.log('');
  console.log(bad === 0 ? '界面检查全部通过' : (bad + ' 项失败'));
  ws.close(); child.kill();
  process.exit(bad === 0 ? 0 : 1);
})().catch(e => { console.error('检查失败: ' + e.message); try { child.kill(); } catch (_) {} process.exit(2); });
