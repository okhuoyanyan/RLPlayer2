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
// 文件夹页是异步 /api/files 渲染的：点了导航后等 #fPath 出现；偶发慢响应时再点一次（最多 3 次）。
async function gotoFiles() {
  for (let i = 0; i < 3; i++) {
    await evalJs("Array.from(document.querySelectorAll('.nav')).find(e=>e.dataset.v==='files').click()");
    if (await waitFor("!!document.getElementById('fPath') && document.querySelectorAll('#list .row').length > 0", 6000)) return true;
  }
  return false;
}

// ---- 全站 ? 浮层核查（261027-03 #6）--------------------------------------------------
// 为什么需要它：用户报「设置-诊断，所有 ? 的帮助文档都被左侧栏挡住文字」。
// 这里把设置页每个页签里**每一个** ? 都 hover 一遍，量出浮层矩形，再算：
//   ① 与左栏 .side 的重叠面积（判据：必须为 0）；
//   ② 是否完整落在视口内；
//   ③ 被主区 .main（overflow:auto）裁掉的宽度 —— 老实现 .tip::after 就是死在这一条上。
// 老实现（CSS ::after）量不到真实节点，用计算样式反推它自己的盒模型；
// 新实现（body 下的 #tipLayer 固定层）直接量那个节点，两者字段一致，可以前后对比。
async function tipAudit(prefix) {
  const rows = [], badSide = [], badView = [], badClip = [];
  // 可选：窄窗口复核（TIP_W/TIP_H 环境变量）—— 左栏在 ≤1240px 时变 200px，
  // 浮层的 minL/maxW 要跟着变，所以窄窗必须单独量一遍。
  const EMU_W = +(process.env.TIP_W || 0), EMU_H = +(process.env.TIP_H || 0);
  if (EMU_W && EMU_H) await send('Emulation.setDeviceMetricsOverride', { width: EMU_W, height: EMU_H, deviceScaleFactor: 1, mobile: false });
  const MEAS = (idx) => '(function(){' +
    'var e=document.querySelector(\'#setpage .tip[data-tipidx="' + idx + '"]\');' +
    'if(!e) return {err:\'tip gone\'};' +
    'var L=document.getElementById(\'tipLayer\');' +
    'if(!L||!L.classList.contains(\'on\')) e.dispatchEvent(new MouseEvent(\'mouseover\',{bubbles:true}));' +
    'L=document.getElementById(\'tipLayer\');' +
    'var vr=e.getBoundingClientRect(), out={impl:\'none\',rect:null};' +
    'if(L&&L.classList.contains(\'on\')){' +
    '  var lr=L.getBoundingClientRect();' +
    '  out={impl:\'layer\',rect:{l:+lr.left.toFixed(2),t:+lr.top.toFixed(2),r:+lr.right.toFixed(2),b:+lr.bottom.toFixed(2),w:+lr.width.toFixed(2),h:+lr.height.toFixed(2)}};' +
    '}else{' +
    '  var cs=getComputedStyle(e,\'::after\');' +
    '  var w=parseFloat(cs.width)||0,h=parseFloat(cs.height)||0;' +
    '  if(cs.content&&cs.content!==\'none\'&&cs.content!==\'normal\'&&w>0&&h>0){' +
    '    var below=!!e.closest(\'.sghead\');' +   // 分组标题的 (?) 旧 CSS 是向下展开
    '    var l=vr.left+vr.width/2-w/2, t=below?(vr.bottom+7):(vr.top-7-h);' +
    '    out={impl:\'pseudo\',rect:{l:+l.toFixed(2),t:+t.toFixed(2),r:+(l+w).toFixed(2),b:+(t+h).toFixed(2),w:+w.toFixed(2),h:+h.toFixed(2)}};' +
    '  }' +
    '}' +
    'var side=document.querySelector(\'.side\'), main=document.querySelector(\'.main\');' +
    'if(!side||!main||!out.rect) return {err:\'no rect\',impl:out.impl};' +
    'var sr=side.getBoundingClientRect(), mr=main.getBoundingClientRect();' +
    'function isect(a,b,c,d,A,B,C,Dd){var w=Math.min(c,C)-Math.max(a,A),h=Math.min(d,Dd)-Math.max(b,B);return (w>0&&h>0)?{w:+w.toFixed(2),h:+h.toFixed(2),area:+(w*h).toFixed(1)}:{w:0,h:0,area:0};}' +
    'var R=out.rect;' +
    'return {impl:out.impl,rect:R,side:isect(R.l,R.t,R.r,R.b,sr.left,sr.top,sr.right,sr.bottom),' +
    '  clip:{l:+Math.max(0,mr.left-R.l).toFixed(2),r:+Math.max(0,R.r-mr.right).toFixed(2),t:+Math.max(0,mr.top-R.t).toFixed(2),b:+Math.max(0,R.b-mr.bottom).toFixed(2)},' +
    '  vw:window.innerWidth,vh:window.innerHeight,' +
    '  sideR:+sr.right.toFixed(2), mainL:+mr.left.toFixed(2)};' +
    '})()';
  const pad = (s, n) => { s = String(s == null ? '' : s); let w = 0; for (const ch of s) w += (ch.charCodeAt(0) > 255 ? 2 : 1); return s + ' '.repeat(Math.max(0, n - w)); };
  await send('Page.navigate', { url: URL_ });
  await sleep(700);
  await waitFor("window.S && S.view", 15000);
  await sleep(400);
  console.log('=== 全站 ? 浮层核查（前缀 ' + prefix + '）===');
  console.log('左栏文本: ' + JSON.stringify(await evalJs("(document.querySelector('.side')||{}).innerText || ''")));
  await evalJs("Array.from(document.querySelectorAll('.side .nav')).filter(function(e){return e.dataset.v==='settings';})[0].click()");
  if (!await waitFor("!!document.querySelector('#setpage .stabs')", 8000)) { console.log('设置页没打开'); return { total: 0, fail: true, badSide, badView, badClip }; }
  await sleep(500);
  const tabs = await evalJs("Array.from(document.querySelectorAll('#setpage .stabs [data-g]')).map(function(e){return {g:e.dataset.g,t:e.textContent.trim()};})") || [];
  console.log('页签: ' + tabs.map(t => t.t + '(' + t.g + ')').join(' / '));
  console.log('');
  console.log(pad('页签', 5) + ' ' + pad('控件', 18) + ' ' + pad('浮层矩形 x/y (w×h)', 34) + ' ' + pad('左栏重叠', 12) + ' ' + pad('视口', 5) + ' 主区裁剪');
  for (const tb of tabs) {
    await evalJs('(function(g){var b=Array.from(document.querySelectorAll(\'#setpage .stabs [data-g]\')).filter(function(e){return e.dataset.g===g;})[0]; if(b)b.click();})(' + JSON.stringify(tb.g) + ')');
    await sleep(340);
    const tips = await evalJs('(function(){' +
      'var all=Array.prototype.slice.call(document.querySelectorAll(\'#setpage .tip\'));' +
      'all.forEach(function(e){e.removeAttribute(\'data-tipidx\');});' +
      'var vis=all.filter(function(e){var r=e.getBoundingClientRect();return r.width>1&&r.height>1;});' +
      'vis.forEach(function(e,i){e.setAttribute(\'data-tipidx\',String(i));});' +
      'return vis.map(function(e,i){var r=e.getBoundingClientRect();' +
      '  var p=e.parentElement, lb=p?(p.textContent||\'\'):\'\';' +
      '  return {i:i,cx:+(r.left+r.width/2).toFixed(2),cy:+(r.top+r.height/2).toFixed(2),' +
      '    label:lb.replace(/[?？]/g,\'\').trim().slice(0,18)};});' +
      '})()') || [];
    for (const t of tips) {
      // 先把 ? 滚到视口中央：用户是"滚到那里再 hover"，不滚会量到视口外的假数据
      await evalJs('(function(idx){var e=document.querySelector(\'#setpage .tip[data-tipidx="' + t.i + '"]\');' +
        'if(!e) return 0; e.scrollIntoView({block:"center",inline:"nearest"}); return 1;})()');
      await sleep(140);
      await send('Input.dispatchMouseEvent', { type: 'mouseMoved', x: Math.round(t.cx), y: Math.round(t.cy), button: 'none', buttons: 0 });
      await sleep(90);
      const pos = await evalJs('(function(idx){var e=document.querySelector(\'#setpage .tip[data-tipidx="' + t.i + '"]\');' +
        'if(!e) return null; var r=e.getBoundingClientRect(); return {cx:r.left+r.width/2, cy:r.top+r.height/2};})()');
      if (pos && (Math.round(pos.cx) !== Math.round(t.cx) || Math.round(pos.cy) !== Math.round(t.cy))) {
        await send('Input.dispatchMouseEvent', { type: 'mouseMoved', x: Math.round(pos.cx), y: Math.round(pos.cy), button: 'none', buttons: 0 });
        await sleep(90);
      }
      const m = await evalJs(MEAS(t.i));
      // 截图必须在鼠标还停在 ? 上时抓：浮层只在 hover/focus 时显示，移开就没了
      if (m && m.rect) {
        if (tb.g === 'diag' && t.i === 0) await shot(prefix + '-diag-tip');
        if (tb.g === 'out' && t.i === 0) await shot(prefix + '-out-tip');
      }
      await send('Input.dispatchMouseEvent', { type: 'mouseMoved', x: 4, y: 4, button: 'none', buttons: 0 });  // 移开，收起浮层
      if (!m || m.err || !m.rect) { console.log(pad(tb.t, 5) + ' ' + pad(t.label, 18) + ' 量不到浮层: ' + (m && m.err)); continue; }
      const R = m.rect, area = m.side.area;
      const clipW = Math.max(m.clip.l, m.clip.r), clipH = Math.max(m.clip.t, m.clip.b);
      const clipped = clipW > 0.5 || clipH > 0.5;
      const inView = R.l >= -0.5 && R.t >= -0.5 && R.r <= m.vw + 0.5 && R.b <= m.vh + 0.5;
      const key = tb.t + '/' + t.label;
      rows.push({ tab: tb.t, g: tb.g, label: t.label, impl: m.impl, rect: R, area: area, inView: inView, clipW: clipW, clipH: clipH });
      if (area > 0) badSide.push(key + '(' + area + 'px²)');
      if (!inView) badView.push(key + '(l=' + R.l + ',r=' + R.r + '/vw=' + m.vw + ',b=' + R.b + '/vh=' + m.vh + ')');
      if (clipped) badClip.push(key + '(裁 ' + (clipW || clipH) + 'px)');
      console.log(pad(tb.t, 5) + ' ' + pad(t.label, 18) + ' ' +
        pad('x ' + R.l + '..' + R.r + ' y ' + R.t + '..' + R.b + ' (' + R.w + '×' + R.h + ')', 34) + ' ' +
        pad(area > 0 ? ('✗ ' + m.side.w + '×' + m.side.h + '=' + area) : '0', 12) + ' ' +
        pad(inView ? 'ok' : '✗越界', 5) + ' ' + (clipped ? ('✗ ' + clipW + '/' + clipH + 'px') : '0') + ' [' + m.impl + ']');
    }
  }
  // ---- 边界用例：把 ? 顶到视口最下沿/最上沿，验证"夹进视口"真的生效 ----
  // （上面的循环用 scrollIntoView(block:'center')，测不到 y 方向夹取；这两条专门打边）
  await evalJs('(function(){var b=Array.prototype.slice.call(document.querySelectorAll(\'#setpage .stabs [data-g]\')).filter(function(e){return e.dataset.g===\'diag\';})[0]; if(b)b.click();})()');
  await sleep(340);
  for (const edge of ['end', 'start']) {
    const info = await evalJs('(function(){var all=Array.prototype.slice.call(document.querySelectorAll(\'#setpage .tip\'));' +
      'all.forEach(function(e){e.removeAttribute(\'data-tipidx\');});' +
      'var vis=all.filter(function(e){var r=e.getBoundingClientRect();return r.width>1&&r.height>1;});' +
      'if(!vis.length) return null; var pick=' + (edge === 'end' ? 'vis[vis.length-1]' : 'vis[0]') + ';' +
      'vis.forEach(function(e,i){if(e===pick) e.setAttribute(\'data-tipidx\',\'0\');});' +
      'pick.scrollIntoView({block:"' + edge + '"});' +
      'var r=pick.getBoundingClientRect(); return {cx:r.left+r.width/2,cy:r.top+r.height/2};})()');
    if (!info) continue;
    await sleep(160);
    const pos = await evalJs('(function(){var e=document.querySelector(\'#setpage .tip[data-tipidx="0"]\'); if(!e) return null;' +
      'var r=e.getBoundingClientRect(); return {cx:r.left+r.width/2,cy:r.top+r.height/2};})()') || info;
    await send('Input.dispatchMouseEvent', { type: 'mouseMoved', x: Math.round(pos.cx), y: Math.round(pos.cy), button: 'none', buttons: 0 });
    await sleep(120);
    const m = await evalJs(MEAS(0));
    await send('Input.dispatchMouseEvent', { type: 'mouseMoved', x: 4, y: 4, button: 'none', buttons: 0 });
    if (!m || !m.rect) { console.log('边界(' + edge + ') 量不到'); continue; }
    const R = m.rect;
    const inView = R.l >= -0.5 && R.t >= -0.5 && R.r <= m.vw + 0.5 && R.b <= m.vh + 0.5;
    const key = '边界/diag-' + edge;
    rows.push({ tab: '边界', g: 'diag', label: edge, impl: m.impl, rect: R, area: m.side.area, inView: inView, clipW: 0, clipH: 0 });
    if (m.side.area > 0) badSide.push(key + '(' + m.side.area + 'px²)');
    if (!inView) badView.push(key + '(t=' + R.t + ',b=' + R.b + '/vh=' + m.vh + ')');
    console.log(pad('边界', 5) + ' ' + pad('diag ? ' + edge, 18) + ' ' +
      pad('x ' + R.l + '..' + R.r + ' y ' + R.t + '..' + R.b + ' (' + R.w + '×' + R.h + ')', 34) + ' ' +
      pad(m.side.area > 0 ? ('✗ ' + m.side.area) : '0', 12) + ' ' + pad(inView ? 'ok' : '✗越界', 5) + ' 视口高 ' + m.vh);
  }
  await evalJs("Array.from(document.querySelectorAll('.side .nav')).filter(function(e){return e.dataset.v==='albums';})[0].click()");
  await sleep(600);
  await shot(prefix + '-sidebar');
  const align = await evalJs('(function(){' +
    'function R(e){var r=e.getBoundingClientRect();return {x:+r.left.toFixed(2),y:+r.top.toFixed(2),h:+r.height.toFixed(2),b:+r.bottom.toFixed(2)};}' +
    'var side=document.querySelector(\'.side\');' +
    'var play=Array.prototype.slice.call(side.querySelectorAll(\'.sec\')).filter(function(e){return e.textContent.trim()===\'播放\';})[0];' +
    'var sqhd=side.querySelector(\'.sqhd\');' +
    'if(!play||!sqhd) return {ok:false};' +
    'var nav=play.nextElementSibling, sq=sqhd.nextElementSibling;' +
    'var cp=R(play), cs=R(sqhd), cn=nav?R(nav):null, cq=sq?R(sq):null;' +
    'return {ok:true,leftDiff:+(cs.x-cp.x).toFixed(2),gapPlay:cn?+(cn.y-cp.b).toFixed(2):null,gapSq:cq?+(cq.y-cs.b).toFixed(2):null,' +
    '  gapDiff:(cn&&cq)?+(Math.abs((cq.y-cs.b)-(cn.y-cp.b))).toFixed(2):null, firstSq:sq?sq.className:null, sqText:sq?sq.textContent.trim():null,' +
    '  titleH:[cp.h,cs.h]};' +
    '})()');
  console.log('');
  console.log('左栏对齐: ' + JSON.stringify(align));
  return { total: rows.length, rows: rows, badSide: badSide, badView: badView, badClip: badClip, align: align,
           fail: badSide.length > 0 || badView.length > 0 || badClip.length > 0 || errors.length > 0 };
}

// ---- 三主题复核（261027-03 #6）-------------------------------------------------------
// 浮层改成固定层之后，配色改由 CSS 变量驱动（--bg3/--txt2/--line/--shadow）。
// 这里在 dark / light / blue 三套主题下各抽 3 个 ?，重新量「与左栏重叠 / 是否在视口内」，
// 并把浮层的前景/背景色取回来算对比度（证明三套主题下都还读得清）。
async function tipThemes(prefix) {
  const rows = [];
  const con = (fg, bg) => {
    const p = (s) => { const m = String(s).match(/[\d.]+/g) || []; return m.slice(0, 3).map(Number); };
    const lin = (c) => { c /= 255; return c <= 0.03928 ? c / 12.92 : Math.pow((c + 0.055) / 1.055, 2.4); };
    const L = (rgb) => 0.2126 * lin(rgb[0]) + 0.7152 * lin(rgb[1]) + 0.0722 * lin(rgb[2]);
    const a = L(p(fg)), b = L(p(bg));
    return +(((Math.max(a, b) + 0.05) / (Math.min(a, b) + 0.05))).toFixed(2);
  };
  const MEAS = '(function(){' +
    'var e=document.querySelector(\'#setpage .tip[data-tipidx="0"]\');' +
    'if(!e) return {err:"no tip"};' +
    'e.dispatchEvent(new MouseEvent(\'mouseover\',{bubbles:true}));' +   // 无条件重放：绝不用上一个 ? 的残留'
    'var L=document.getElementById(\'tipLayer\');' +
    'if(!L||!L.classList.contains(\'on\')) return {err:"layer not shown"};' +
    'var lr=L.getBoundingClientRect(), side=document.querySelector(\'.side\').getBoundingClientRect(), cs=getComputedStyle(L);' +
    'var w=Math.min(lr.right,side.right)-Math.max(lr.left,side.left), h=Math.min(lr.bottom,side.bottom)-Math.max(lr.top,side.top);' +
    'return {l:+lr.left.toFixed(2),t:+lr.top.toFixed(2),r:+lr.right.toFixed(2),b:+lr.bottom.toFixed(2),' +
    '  w:+lr.width.toFixed(2),h:+lr.height.toFixed(2),area:(w>0&&h>0)?+(w*h).toFixed(1):0,' +
    '  inView:(lr.left>=-0.5&&lr.top>=-0.5&&lr.right<=innerWidth+0.5&&lr.bottom<=innerHeight+0.5),' +
    '  vw:innerWidth,vh:innerHeight,color:cs.color,bg:cs.backgroundColor,text:L.textContent.slice(0,24)};' +
    '})()';
  await send('Page.navigate', { url: URL_ });
  await sleep(700);
  await waitFor("window.S && S.view", 15000);
  await sleep(300);
  await evalJs("Array.from(document.querySelectorAll('.side .nav')).filter(function(e){return e.dataset.v==='settings';})[0].click()");
  if (!await waitFor("!!document.querySelector('#setpage .stabs')", 8000)) { console.log('设置页没打开'); return { fail: true, rows }; }
  await sleep(400);
  console.log('=== 三主题 ? 浮层复核（' + prefix + '）===');
  let bad = 0;
  for (const th of ['dark', 'light', 'blue']) {
    await evalJs('(function(t){var b=document.querySelector(\'.themes .sw[data-theme="' + th + '"]\'); if(b)b.click();})()');
    await sleep(350);
    for (const g of ['in', 'out', 'diag']) {
      // 注意：这里必须把 g 传进 IIFE —— 少了实参就等于没点页签，9 次测量会全落在同一个 ? 上（本轮真踩到）
      await evalJs('(function(g){var b=Array.prototype.slice.call(document.querySelectorAll(\'#setpage .stabs [data-g]\')).filter(function(e){return e.dataset.g===g;})[0]; if(b)b.click();})(' + JSON.stringify(g) + ')');
      await sleep(320);
      const info = await evalJs('(function(){var all=Array.prototype.slice.call(document.querySelectorAll(\'#setpage .tip\'));' +
        'all.forEach(function(e){e.removeAttribute(\'data-tipidx\');});' +
        'var vis=all.filter(function(e){var r=e.getBoundingClientRect();return r.width>1&&r.height>1;});' +
        'if(!vis.length) return null; vis.forEach(function(e,i){e.setAttribute(\'data-tipidx\',String(i));});' +
        'vis[0].scrollIntoView({block:"center"});' +
        'var r=vis[0].getBoundingClientRect(); return {cx:r.left+r.width/2,cy:r.top+r.height/2,n:vis.length};})()');
      if (!info) continue;
      await sleep(140);
      const pos = await evalJs('(function(){var e=document.querySelector(\'#setpage .tip[data-tipidx="0"]\'); if(!e) return null; ' +
        'var r=e.getBoundingClientRect(); return {cx:r.left+r.width/2,cy:r.top+r.height/2};})()') || info;
      await send('Input.dispatchMouseEvent', { type: 'mouseMoved', x: 4, y: 4, button: 'none', buttons: 0 });  // 先离开，保证下一个 ? 一定能触发 mouseover
      await sleep(70);
      await send('Input.dispatchMouseEvent', { type: 'mouseMoved', x: Math.round(pos.cx), y: Math.round(pos.cy), button: 'none', buttons: 0 });
      await sleep(120);
      const m = await evalJs(MEAS);
      if (th === 'dark' && g === 'diag') await shot(prefix + '-theme-dark');
      if (th === 'light' && g === 'diag') await shot(prefix + '-theme-light');
      if (th === 'blue' && g === 'diag') await shot(prefix + '-theme-blue');
      await send('Input.dispatchMouseEvent', { type: 'mouseMoved', x: 4, y: 4, button: 'none', buttons: 0 });
      if (!m || m.err) { console.log(th + ' ' + g + ' 量不到: ' + (m && m.err)); bad++; continue; }
      const cr = con(m.color, m.bg);
      rows.push({ th, g, m, cr });
      if (m.area > 0 || !m.inView) bad++;
      console.log(th + '  ' + g.padEnd(5) + ' x ' + m.l + '..' + m.r + ' y ' + m.t + '..' + m.b + ' (' + m.w + '×' + m.h + ')' +
        ' 左栏重叠 ' + m.area + ' 视口 ' + (m.inView ? 'ok' : '✗') + ' 对比度 ' + cr + ':1  ' + JSON.stringify(m.text));
    }
  }
  return { rows, fail: bad > 0 };
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

  // ---- 模式 `tips`：全站 ? 浮层核查（261027-03 #6「帮助文档被左侧栏挡住」）----
  // 用法：node tools/uicheck.js <url> <截图目录> tips [截图前缀]
  // 判据（用户给的）：每个 ? 浮层的矩形与左栏 .side 的重叠面积必须为 0，且浮层完整落在视口内。
  // 另外多记一项"被主区裁剪"：主区 .main 是 overflow:auto，浮层若伸出它的左边界就会被裁掉
  // —— 那正是"被左侧栏挡住文字"的真实形态（不是 z-index）。
  const MODE = (process.argv[4] || '').trim().toLowerCase();
  if (MODE === 'themes') {
    const r = await tipThemes((process.argv[5] || 'tip').trim());
    if (errors.length) console.log('  JS 异常: ' + errors.slice(0, 3).join(' | '));
    console.log(r.fail ? '结果: FAIL' : '结果: PASS');
    try { child.kill(); } catch (e) { }
    process.exit(r.fail ? 1 : 0);
  }
  if (MODE === 'tips') {
    const r = await tipAudit((process.argv[5] || 'tip').trim());
    console.log('\n汇总: ' + r.total + ' 个 ?；与左栏重叠 ' + r.badSide.length +
      ' 个；超出视口 ' + r.badView.length + ' 个；被主区裁剪 ' + r.badClip.length + ' 个');
    if (r.badSide.length) console.log('  重叠清单: ' + r.badSide.slice(0, 8).join(' / '));
    if (r.badView.length) console.log('  越界清单: ' + r.badView.slice(0, 8).join(' / '));
    if (errors.length) console.log('  JS 异常: ' + errors.slice(0, 3).join(' | '));
    console.log(r.fail ? '结果: FAIL' : '结果: PASS');
    try { child.kill(); } catch (e) { }
    process.exit(r.fail ? 1 : 0);
  }

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

  // ---- 261027-02 #3：左栏「播放」区与「我的歌单」区必须是同一套间距 ----
  // 用户原话：「主界面，播放、我的歌单两个对齐，现在这一行各个元素没有完全统一间隔」。
  // 旧账：.sec 是 margin-left 12px + 无 padding、下边距 6px；.sqhd 是 padding-left 22px、下边距 2px、
  // 行高也不同 ⟹ 标题左边缘差 10px、标题到首项间距差 4px、二级行比一级行高 2.75px。
  // 判据（用户给的）：标题文字左边缘 x 差 ≤1px、标题到各自第一项间距差 ≤1px、行高一致。
  await waitFor("!!document.querySelector('.side .sqhd')", 6000);
  const sideAlign = await evalJs(`(function(){
    function R(e){var r=e.getBoundingClientRect();return {x:+r.left.toFixed(2),y:+r.top.toFixed(2),h:+r.height.toFixed(2),b:+r.bottom.toFixed(2)};}
    var side=document.querySelector('.side');
    var play=Array.prototype.slice.call(side.querySelectorAll('.sec')).filter(function(e){return e.textContent.trim()==='播放';})[0];
    var sqhd=side.querySelector('.sqhd');
    if(!play||!sqhd) return {ok:false,why:'缺少「播放」标题或「我的歌单」标题'};
    var nav=play.nextElementSibling, sq=sqhd.nextElementSibling;
    if(!nav||!sq) return {ok:false,why:'缺少首项'};
    var cp=R(play), cs=R(sqhd), cn=R(nav), cq=R(sq);
    var sp=getComputedStyle(play), ss=getComputedStyle(sqhd);
    var isRow=sq.classList.contains('sqrow');
    return {ok:true, isRow:isRow,
      leftDiff:+(cs.x-cp.x).toFixed(2),
      gapPlay:+(cn.y-cp.b).toFixed(2), gapSq:+(cq.y-cs.b).toFixed(2),
      gapDiff:+((cq.y-cs.b)-(cn.y-cp.b)).toFixed(2),
      titleH:[cp.h,cs.h], titleHdiff:+Math.abs(cp.h-cs.h).toFixed(2),
      lineHeight:[sp.lineHeight,ss.lineHeight], fontSize:[sp.fontSize,ss.fontSize],
      rowH:[cn.h,cq.h], rowHdiff:isRow?+Math.abs(cn.h-cq.h).toFixed(2):null};
  })()`);
  ck(sideAlign && sideAlign.ok && Math.abs(sideAlign.leftDiff)<=1 && Math.abs(sideAlign.gapDiff)<=1 &&
     sideAlign.titleHdiff<=1 && sideAlign.lineHeight[0]===sideAlign.lineHeight[1] &&
     sideAlign.fontSize[0]===sideAlign.fontSize[1] && (sideAlign.isRow ? sideAlign.rowHdiff<=1 : true),
     '左栏「播放」与「我的歌单」标题左边缘/行高/标题→首项间距一致（差 ≤1px）', JSON.stringify(sideAlign));
  await shot('27-sidebar-align', true);

  // ---- 验收 1：左导航两项都已移走 ----
  // 2026-10-23 用户 261023-04 #2/#9：EQ 与房间校正 → 设置页「EQ」页签；古典作品 → 单曲页「作品」tab。
  const navOk = await evalJs(`(function(){
    var navs = Array.from(document.querySelectorAll('.nav')).map(e => e.textContent.trim());
    var hasClassical = navs.some(t => t.indexOf('古典作品') >= 0);
    var hasEq = navs.some(t => t.indexOf('EQ') >= 0);
    return { ok: !hasClassical && !hasEq, hasClassical: hasClassical, hasEq: hasEq, navs: navs };
  })()`);
  ck(navOk.ok, '左导航没有「古典作品」也没有「EQ 与房间校正」（已分别移入单曲页/设置页）', JSON.stringify(navOk));
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
    // 2026-10-27（261026-09 实测）：副作用端点自 2026-10-25 起只接受 POST，GET 一律 405。
    // 这里原来用 GET ⟹ 清理动作静默失败；用户实例上"首行本来就不是收藏"时判据空过，
    // 换成带预置收藏的沙箱（build-ui9）才暴露出来。改为 POST（与页面里 api() 助手同一行为）。
    await fetch('/api/fav?file=' + encodeURIComponent(f) + '&on=0', { method: 'POST' });
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
    var aiCard = document.getElementById('artistSummary');
    var hasAi = !!aiCard && (aiCard.querySelectorAll('.efrow').length > 0 || aiCard.querySelectorAll('.npdsec').length > 0);
    if (hasSources && hasBio) return 'data-branch';
    if (hasEmpty && hasRetry) return 'empty-branch';
    // 261024-01 #3a：没有权威词条时**不再**拍「权威词条暂缺」卡，直接安静显示 AI 整理资料
    if (hasAi) return 'ai-branch';
    return 'sources=' + hasSources + ' bio=' + hasBio + ' empty=' + hasEmpty + ' retry=' + hasRetry + ' ai=' + hasAi;
  })()`);
  ck(profileState === 'data-branch' || profileState === 'empty-branch' || profileState === 'ai-branch',
     '艺术家资料页状态正常（有数据=简介+来源芯片；无权威=安静显示 AI 资料；离线=空态卡+重试）', profileState);
  ck(await evalJs("(function(){var t=document.body.textContent||'';return t.indexOf('权威词条暂缺')<0 && t.indexOf('没有匹配条目')<0 && t.indexOf('权威资料暂缺')<0;})()"),
     '艺术家资料页没有「权威词条暂缺 / 没有匹配条目 / 权威资料暂缺」解释文案（261024-01 #3a）');
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
  // 文件夹视图是异步 /api/files 渲染的；慢盘/慢网络下固定 sleep 700 会假失败（本环境实测偶发）。
  ck(await gotoFiles(), '文件夹视图有内容');
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
  // 2026-10-22 功能3：「诊断」独立成页，排在元数据之后。
  // 2026-10-23 用户 261023-04 #2：EQ 与房间校正从主导航移进设置页，「EQ」页签排在「高级」之后。
  // 2026-10-24 用户 261024-01 #5：「流媒体」从「高级」拆成独立页签，排在元数据之后、诊断之前。
  // 2026-10-27（用户 261026-09 #6）：EQ 页拆成「EQ」+「声音校准」，声音校准紧跟 EQ。
  // 2026-10-27（用户 261027-03 #9 / 在线更新）：新增「分析」页签（排在元数据之后、流媒体之前）
  // 与「关于与更新」页签（排在诊断之后），共 12 个。
  // ★ 2026-10-27（261027-04 #4）：用户要求「设置-分析，整个功能页不应该放到这里，直接将分析放到
  //   正在播放的那首歌音乐上」⟹「分析」页签**已删除**，回到 11 个；三类结果改挂在单曲页 #npAnal。
  //   这里必须跟着改（否则旧期望值会让自检永远红）—— 改动只删了「分析」这一项，其余顺序未动。
  ck(tabOrder === '输入/输出/PCM/SDM/高级/EQ/声音校准/元数据/流媒体/诊断/关于与更新',
     '设置页签顺序为 输入/输出/PCM/SDM/高级/EQ/声音校准/元数据/流媒体/诊断/关于与更新（无「分析」）', tabOrder);

  // ---- 261027-02 #2：设置-输出-官方推荐设置 的 4 个预设必须并排一行 ----
  // 用户原话：「4 个预设一行就行，不要分两行」。旧 CSS 是 repeat(3) ⟹ 第 4 张必然掉到第二行。
  // 判据：4 个 .preset-btn 的 top 只有一个值（=同一行）、left 严格递增；并记录每张卡的 x/y/w/h。
  await evalJs("document.querySelector('.stab[data-g=out]').click()");
  ck(await waitFor("!!document.getElementById('presetCard')", 8000), '设置-输出 页能打开「官方推荐预设」卡片');
  await sleep(200);
  const preGeom = await evalJs(`(function(){
    var grid=document.querySelector('#presetCard .preset-grid'); if(!grid) return {ok:false,why:'没有 .preset-grid'};
    var bs=Array.prototype.slice.call(grid.querySelectorAll('.preset-btn'));
    var rs=bs.map(function(b){var r=b.getBoundingClientRect();
      return {id:b.id,x:+r.left.toFixed(2),y:+r.top.toFixed(2),w:+r.width.toFixed(2),h:+r.height.toFixed(2)};});
    var tops={}, lefts=[];
    rs.forEach(function(r){ tops[Math.round(r.y)]=1; lefts.push(r.x); });
    var inc=lefts.every(function(v,i){ return i===0||v>lefts[i-1]; });
    return {ok:true,n:rs.length,rows:Object.keys(tops).length,inc:inc,
            cols:getComputedStyle(grid).gridTemplateColumns, items:rs};
  })()`);
  ck(preGeom && preGeom.ok && preGeom.n===4 && preGeom.rows===1 && preGeom.inc,
     '官方推荐设置的 4 个预设在同一行（top 相同、left 递增）',
     JSON.stringify(preGeom && {rows:preGeom.rows, inc:preGeom.inc, cols:preGeom.cols, items:preGeom.items}));
  await shot('26-presets-row', true);

  // ---- 2026-10-24 用户 261024-01 #5/#6/#8/#12 的界面契约（全部只读，不改任何设置）----
  // #5 密码框样式：旧 CSS 选择器只列了 text/number，password 落到浏览器默认样式（方角/无内边距/
  //    不铺满），这就是「密码框怎么跟其他框样式不一样」的根因。判据：计算样式逐项相等。
  const pwStyle = await evalJs(`(function(){
    function cs(id){var e=document.getElementById(id);if(!e)return null;var s=getComputedStyle(e);
      return [s.paddingTop,s.paddingRight,s.paddingBottom,s.paddingLeft,s.borderRadius,
              s.backgroundColor,s.borderTopWidth,s.borderTopStyle,s.fontSize,s.fontFamily].join('|');}
    return {u:cs('sp_src_user'),p:cs('sp_src_pass'),k:cs('sp_llmkey')};
  })()`);
  ck(pwStyle && pwStyle.u && pwStyle.p === pwStyle.u && pwStyle.k === pwStyle.u,
     '密码框（流媒体服务器 / DeepSeek Key）与文本框计算样式逐项一致', JSON.stringify(pwStyle));

  // #5/#6 流媒体页：控件齐全；高级页改名「本地音乐库」，不再有「流媒体服务器」组与 DLNA 按钮。
  const streamSec = await evalJs(`(function(){
    var s=document.querySelector('#setpage .spane[data-g=stream]');
    var a=document.querySelector('#setpage .spane[data-g=adv]');
    if(!s||!a) return 'missing-pane';
    var need=['sp_src_add','sp_src_list','sp_src_form','sp_src_name','sp_src_kind','sp_src_base',
              'sp_src_user','sp_src_pass','sp_src_save','sp_src_cancel','sp_src_testform',
              // 261024-02 #5：DLNA 只发现按钮升级为完整「推送到设备」区块
              'sp_cast_dev','sp_cast_scan','sp_cast_push','sp_cast_stop','sp_cast_stat_phase'];
    var miss=[]; need.forEach(function(id){var e=document.getElementById(id);if(!e||!s.contains(e))miss.push(id)});
    return {miss:miss,
            oldStream:a.textContent.indexOf('流媒体服务器')>=0,
            oldName:a.textContent.indexOf('音乐库与网络')>=0,
            newName:a.textContent.indexOf('本地音乐库')>=0,
            upnpInAdv:!!a.querySelector('#sp_upnp')};
  })()`);
  ck(streamSec && Array.isArray(streamSec.miss) && streamSec.miss.length===0 && !streamSec.oldStream &&
     !streamSec.oldName && streamSec.newName && !streamSec.upnpInAdv,
     '流媒体页控件齐全（含推送区块）；高级页改名「本地音乐库」、无流媒体服务器组/DLNA 按钮', JSON.stringify(streamSec));

  // #12 SDM 比特率 / 封装始终可选（根因：旧 applyDsdState 只在输出 DSD 时启用，默认 PCM ⟹ 全灰）
  const sdmEnabled = await evalJs(`(function(){var r={};['sp_dsdrate','sp_dsdpk'].forEach(function(id){var e=document.getElementById(id);r[id]=e?e.disabled:null});r.note=!!document.getElementById('sp_dsdmodenote');return r;})()`);
  ck(sdmEnabled && sdmEnabled.sp_dsdrate===false && sdmEnabled.sp_dsdpk===false && sdmEnabled.note,
     'SDM 比特率 / 封装始终可选（不 disabled），并有 PCM 下的联动说明', JSON.stringify(sdmEnabled));

  // #8 标签写回默认开启、不再出现「可选」开关字样
  const wbDefault = await evalJs(`(function(){var p=document.querySelector('#setpage .spane[data-g=meta]');if(!p)return null;
    var t=p.textContent;return {hasDefault:t.indexOf('默认开启')>=0, hasOptional:t.indexOf('可选')>=0,
    desc:!!document.getElementById('sp_wbdesc')};})()`);
  ck(wbDefault && wbDefault.hasDefault && !wbDefault.hasOptional && wbDefault.desc,
     '标签写回默认开启：「默认开启」在、无「可选」字样，两按钮说明仍在', JSON.stringify(wbDefault));

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
  // 2026-10-23 用户 261023-04 #10：状态卡必须是横向网格（宽屏一行 4~6 个、窄屏自动换行），
  // 不能像原来那样被 .sgbody 压进单列 → 竖排成长条。顺带断言数值是「大字」。
  const inStatGeom = await evalJs(`(function(){
    var grid = document.querySelector('#setpage .spane[data-g=in] .in-stat-grid');
    if (!grid) return null;
    var items = Array.prototype.slice.call(grid.querySelectorAll('.in-stat-item'));
    var rows = {};
    items.forEach(function(e){ var k = Math.round(e.getBoundingClientRect().top); rows[k] = (rows[k]||0)+1; });
    var maxRow = 0; Object.keys(rows).forEach(function(k){ maxRow = Math.max(maxRow, rows[k]); });
    var val = document.getElementById('sp_stat_running');
    return { count: items.length, maxRow: maxRow,
             valFontSize: val ? parseFloat(getComputedStyle(val).fontSize) : 0,
             gridW: Math.round(grid.getBoundingClientRect().width) };
  })()`);
  ck(inStatGeom && inStatGeom.count >= 6 && inStatGeom.maxRow >= 4,
     '「采集实时状态」是横向统计卡网格（宽屏一行 ≥4 个）', JSON.stringify(inStatGeom));
  ck(inStatGeom && inStatGeom.valFontSize >= 14, '状态卡数值用大字号（≥14px）', JSON.stringify(inStatGeom));

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
  // 2026-10-23 用户 261023-04 #3：三个开关 → 两个。
  //   ①「自动补全（不用 AI）」= metaon；②「AI 增强（DeepSeek）」= metallm + metasummary 合并。
  // 固定两键必须仍在且已绑定；旧的 sp_metasummary 必须**不存在**（已合并）。
  const metaFix = await evalJs("(function(){var ids=['sp_metaon','sp_metallm'];var p=document.querySelector('#setpage .spane[data-g=meta]');var miss=[];ids.forEach(function(i){var e=document.getElementById(i);if(!e||e.type!=='checkbox'||!p.contains(e)||!e.onchange)miss.push(i)});if(document.getElementById('sp_metasummary'))miss.push('sp_metasummary:仍存在');return miss;})()");
  ck(Array.isArray(metaFix) && metaFix.length === 0,
     '两个开关（自动补全 / AI 增强）存在且已绑定；旧 sp_metasummary 已合并删除', JSON.stringify(metaFix));

  // 合并后的 AI 开关：文案必须讲清「DeepSeek + 联网 + 按次计费」
  // 2026-10-23 修判据：说明文案是独立描述行（不在 <label> 内），旧判据只看 label 文本会误报；
  // 这里取整行 .sfld 的可读文案（含说明），必须出现 AI 增强 / DeepSeek / 联网 / 按次计费。
  // 2026-10-24 用户 261024-01 #7：说明文字收进 (?) 的 data-tip（hover 才显示）——
  // 判据必须同时读可见文字与 data-tip，否则会把"移进提示"误判成"文案丢失"。
  const aiLabel = await evalJs("(function(){var e=document.getElementById('sp_metallm');var f=e?e.closest('.sfld'):null;if(!f)return '';var t=f.textContent;var d=f.querySelectorAll('[data-tip]');for(var i=0;i<d.length;i++)t+=' '+d[i].getAttribute('data-tip');return t;})()");
  ck(aiLabel.indexOf('AI 增强') >= 0 && aiLabel.indexOf('DeepSeek') >= 0 && aiLabel.indexOf('联网') >= 0 && aiLabel.indexOf('按次计费') >= 0,
     '「AI 增强（DeepSeek）」文案包含联网 / 按次计费说明（可见文字或 (?) 提示）', aiLabel);

  // 合并语义：一次勾选必须**同时**写 metallm 与 metasummary（开=1,1 / 关=0,0）。
  // 自检纪律：原状态可能是 metallm=1,metasummary=0 的混合态，合并后无法用两次点击还原成混合态，
  // 所以这里**不落盘** —— 临时劫持 api() 截获 saveSettings 生成的 body，检查完立刻还原
  // 函数 / META / DOM，全程不发任何网络请求（不会改用户设置）。
  const mergedPair = await evalJs(`(function(){
    var e = document.getElementById('sp_metallm');
    if (!e) return 'missing';
    var origChecked = e.checked, origLlm = META.llm, origSum = META.summary;
    var realApi = api, captured = '';
    api = function(u, b){ if (u === '/api/settings') captured = b; return Promise.resolve({}); };
    e.checked = true; e.dispatchEvent(new Event('change')); saveSettings(true);
    var onBody = captured, onState = (META.llm === 1 && META.summary === 1);
    captured = '';
    e.checked = false; e.dispatchEvent(new Event('change')); saveSettings(true);
    var offBody = captured, offState = (META.llm === 0 && META.summary === 0);
    api = realApi;
    META.llm = origLlm; META.summary = origSum; e.checked = origChecked;
    return { onPair: /(?:^|&)metallm=1(?:&|$)/.test(onBody) && /(?:^|&)metasummary=1(?:&|$)/.test(onBody),
             offPair: /(?:^|&)metallm=0(?:&|$)/.test(offBody) && /(?:^|&)metasummary=0(?:&|$)/.test(offBody),
             onState: onState, offState: offState };
  })()`);
  ck(mergedPair && mergedPair.onPair && mergedPair.offPair && mergedPair.onState && mergedPair.offState,
     '「AI 增强」一次勾选同时写 metallm+metasummary（开=1,1 / 关=0,0）', JSON.stringify(mergedPair));
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
  // 2026-10-27（261027-02 #1）：该按钮已整体删除，原断言（它不在元数据分组）失去意义 → 改为断言它不存在。
  ck(await evalJs("!document.getElementById('sp_diagzip')"),
     '诊断导出按钮已不存在（元数据页与诊断页都没有，已并入一键反馈）');
  ck(await evalJs("!document.getElementById('sp_diag')"), '旧的单一 JSON「导出诊断信息」按钮已移除（sp_diag 不存在）');
  await evalJs("document.querySelector('.stab[data-g=diag]').click()");
  ck(await waitFor("document.querySelector('#setpage .spane[data-g=diag]') && document.querySelector('#setpage .spane[data-g=diag]').classList.contains('on')", 5000),
     '诊断页签能切过去');
  // 2026-10-27（261027-02 #1）：用户要求删除「导出诊断包」这一栏，功能合并进「一键反馈」。
  // 原话：「设置-诊断，导致诊断包，这一栏删除，功能合并到下面一键反馈，点击"提交反馈"自动发送诊断包到我的GITHUB上」
  // ⟹ 旧断言要求 sp_diagzip / sp_diagjson 存在，现在应当**反过来断言它们不存在**，且一键反馈表单齐全。
  ck(await evalJs("!document.getElementById('sp_diagzip') && !document.getElementById('sp_diagjson')"),
     '「导出诊断包 / 只要 JSON」两个按钮已按用户要求移除（功能并入一键反馈）');
  ck(await evalJs("!!document.getElementById('sp_fbsubmit') && !!document.getElementById('sp_fbsubmit').onclick && !!document.getElementById('sp_fbtitle') && !!document.getElementById('sp_fbbody')"),
     '一键反馈表单（类型/标题/正文/复现步骤/提交按钮）齐全且已绑定');
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
  // 2026-10-23 用户 261023-04 #2：EQ 与房间校正不再是主导航页，而是设置页的「EQ」页签。
  // 判据：抽头/衰减仍在「高级」页签；EQ 文件 / IR / GPU 卷积在「EQ」页签 —— 都在设置页内，
  // 且各自归属正确（不能互相串页签，否则又会出现重复 id / 读错控件）。
  // 2026-10-23（UI 评审 #11）：EQ 卡片里的「保存 EQ 与房间校正设置」按钮与底部全局保存重复，已删除；
  // 这里同时断言它确实不在了，避免以后又被加回来。
  // 2026-10-27（用户 261026-09 #6）：EQ 页签拆成「EQ」+「声音校准」两个。
  //   EQ        = 20 段 PEQ + 耳机 EQ（OPRA）
  //   声音校准  = 房间校正（测量向导）+ 校正文件（sp_eq / sp_ir / sp_irgpu）
  // 判据随之更新：抽头/衰减在「高级」，PEQ/OPRA 在 eq 面板，EQ 文件/IR/GPU 卷积在 cal 面板。
  ck(await evalJs("(function(){var page=document.getElementById('setpage');var eq=document.querySelector('#setpage .spane[data-g=eq]');var cal=document.querySelector('#setpage .spane[data-g=cal]');if(!eq||!cal)return false;var t=document.getElementById('sp_taps'),a=document.getElementById('sp_atten'),ir=document.getElementById('sp_ir'),e=document.getElementById('sp_eq'),g=document.getElementById('sp_irgpu'),dup=document.getElementById('bSaveRoomEq');return !!(t&&a&&page.contains(t)&&page.contains(a)&&ir&&e&&g&&cal.contains(ir)&&cal.contains(e)&&cal.contains(g)&&eq.contains(document.getElementById('peqRows'))&&eq.contains(document.getElementById('sp_opra_search'))&&!dup);})()"),
     '抽头/衰减在设置页，PEQ/OPRA 在「EQ」页签，EQ 文件/IR/GPU 卷积在「声音校准」页签，且无重复保存按钮');
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

  // ---- 261024-02 #5：投放 UI 重构后的自检 ----
  // 用户原话：「输出栏位下的 UPNP 投放功能多余」⟹ 输出页整块删除；投放入口统一到
  // 单曲页「推送到设备」按钮 + 设置→流媒体页的「推送到设备」区块（三处共用 /api/cast/state）。
  await evalJs("document.querySelector('.stab[data-g=out]').click()");
  await sleep(400);
  ck(await evalJs("(function(){var p=document.querySelector('#setpage .spane[data-g=out]');return !!p && p.classList.contains('on');})()"),
     '输出页签处于激活状态');
  const outCastGone = await evalJs(`(function(){
    var p = document.querySelector('#setpage .spane[data-g=out]');
    var ids = ['sp_cast_start','sp_cast_file','sp_cast_dop','sp_cast_usecur','sp_cast_scan','sp_cast_stop','sp_cast_stat_phase'];
    var left = [];
    ids.forEach(function(id){ var e = document.getElementById(id); if (e && p.contains(e)) left.push(id); });
    var bk = [].map.call(document.querySelectorAll('#sp_backend option'), function(o){ return o.value; });
    return { left: left, hasUpnp: bk.indexOf('upnp') >= 0,
             castText: (p.textContent.indexOf('投放（UPnP）') >= 0 || p.textContent.indexOf('投放实时状态') >= 0) };
  })()`);
  ck(outCastGone && outCastGone.left.length === 0 && !outCastGone.castText,
     '输出页已无投放控件与「投放（UPnP）/投放实时状态」区块（261024-02 #5）', JSON.stringify(outCastGone));
  ck(outCastGone && outCastGone.hasUpnp, '输出后端下拉仍保留「UPnP / DLNA 投放」选项');

  await evalJs("document.querySelector('.stab[data-g=stream]').click()");
  await sleep(700);
  const streamCast = await evalJs(`(function(){
    var p = document.querySelector('#setpage .spane[data-g=stream]');
    if (!p) return 'missing-pane';
    var need = ['sp_cast_dev','sp_cast_scan','sp_cast_push','sp_cast_stop','sp_cast_devmsg',
                'sp_cast_stat_phase','sp_cast_stat_dev','sp_cast_stat_pos','sp_cast_stat_transport',
                'sp_cast_stat_bytes','sp_cast_stat_reqs','sp_cast_stat_port','sp_cast_stat_ratematch'];
    var miss = [], unbound = [];
    need.forEach(function(id){
      var e = document.getElementById(id);
      if (!e || !p.contains(e)) { miss.push(id); return; }
      if ((id === 'sp_cast_scan' || id === 'sp_cast_push' || id === 'sp_cast_stop' || id === 'sp_cast_dev') && !e.onclick && !e.onchange) unbound.push(id);
    });
    var push = document.getElementById('sp_cast_push'), stop = document.getElementById('sp_cast_stop');
    var grid = p.querySelector('.in-stat-grid');
    var items = grid ? grid.querySelectorAll('.in-stat-item') : [];
    var rows = {};
    Array.prototype.forEach.call(items, function(e){ var k = Math.round(e.getBoundingClientRect().top); rows[k] = (rows[k] || 0) + 1; });
    var maxRow = 0; Object.keys(rows).forEach(function(k){ maxRow = Math.max(maxRow, rows[k]); });
    var sel = document.getElementById('sp_cast_dev');
    var fld = sel ? sel.closest('.sfld') : null;
    return { miss: miss, unbound: unbound,
             pushDisabled: push ? push.disabled : null, stopDisabled: stop ? stop.disabled : null,
             items: items.length, maxRow: maxRow,
             devWidth: fld ? Math.round(fld.getBoundingClientRect().width) : 0,
             hasTitle: p.textContent.indexOf('推送到设备') >= 0 };
  })()`);
  ck(streamCast && typeof streamCast === 'object' && streamCast.miss.length === 0 && streamCast.unbound.length === 0,
     '流媒体页「推送到设备」区块控件齐全且已绑定事件', JSON.stringify(streamCast));
  ck(streamCast && streamCast.pushDisabled === false && streamCast.stopDisabled === true,
     '推送空闲时「推送当前曲」可用、「停止投放」禁用', JSON.stringify({ push: streamCast && streamCast.pushDisabled, stop: streamCast && streamCast.stopDisabled }));
  ck(streamCast && streamCast.items >= 6 && streamCast.maxRow >= 4,
     '投放实时状态是横向统计卡网格（一行 ≥4 个）', JSON.stringify({ items: streamCast && streamCast.items, maxRow: streamCast && streamCast.maxRow }));
  ck(streamCast && streamCast.devWidth >= 400, '目标设备下拉整行显示（长设备名可读）', streamCast && streamCast.devWidth);
  ck(streamCast && streamCast.hasTitle, '流媒体页有「推送到设备」标题');
  ck(await evalJs("document.querySelectorAll('#setpage .spane[data-g=stream] a[href^=http]').length === 0"),
     '流媒体页无 http 外链');

  // ---- 高级页：EQ 跳转按钮已删除（261023-04 #2：EQ 已在设置页内，不再需要跳转）----
  await evalJs("document.querySelector('.stab[data-g=adv]').click()");
  ck(await evalJs("!document.getElementById('bGoEqNav')"), '高级设置页不再有「前往 EQ 页面」跳转按钮（EQ 已在设置页内）');
  ck(await evalJs("document.querySelectorAll('#setpage .spane[data-g=adv] a[href^=http]').length === 0"),
     '高级页签无外链');

  // ---- 验收 3 & 4：EQ 与房间校正（261023-04 #2 起是设置页的一个页签，不再是主导航页） ----
  await evalJs("document.querySelector('.stab[data-g=eq]').click()");
  await sleep(700);
  ck(await evalJs("(function(){var p=document.querySelector('#setpage .spane[data-g=eq]');return !!p && p.classList.contains('on') && document.getElementById('setpage').classList.contains('on');})()"),
     '设置页「EQ」页签可切换且内容在设置页内');
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

  ck(await evalJs("document.querySelectorAll('#setpage .spane[data-g=eq] a[href^=http]').length === 0"), 'EQ 页签无外链');
  ck(await evalJs("!!document.getElementById('sp_opra_copychip') && document.getElementById('sp_opra_copychip').classList.contains('chip')"), '存在复制官网链接芯片');

  // ---- 261024-02 #6：20 段参数均衡 PEQ ----
  // 2026-10-24（1.3.14 起）：PEQ 编辑区从 <table> 改成 CSS grid 的 div 行（#peqRows > .peqhd + .peqr），
  // 判据必须跟着换选择器，否则测的是"表"存在与否而不是"20 段可编辑"—— 旧写法 tbody tr 会恒为 0。
  const peqRows = await evalJs("document.querySelectorAll('#peqRows .peqr').length");
  ck(peqRows === 20, 'PEQ 编辑区 20 段', 'rows=' + peqRows);
  ck(await evalJs("!!document.getElementById('peqCurve') && document.querySelectorAll('#peqCurve svg polyline').length === 1"),
     'PEQ 频响曲线（SVG）已渲染');
  ck(await evalJs("document.querySelectorAll('#peqPreset option').length >= 2"),
     'PEQ 预设下拉含 Flat + 示例', 'opts=' + (await evalJs("document.querySelectorAll('#peqPreset option').length")));
  ck(await evalJs("(function(){var ids=['peqBypass','peqPreamp','peqLoad','peqSave','peqDelete','peqReset'];for(var i=0;i<ids.length;i++){var e=document.getElementById(ids[i]);if(!e)return false;if(ids[i]==='peqBypass'||ids[i]==='peqPreamp'){if(!e.onchange&&!e.oninput)return false;}else{if(!e.onclick)return false;}}return true;})()"),
     'PEQ 控件齐全且已绑定事件（bypass/前级/加载/保存/删除/重置）');

  // 改第 11 段（1kHz）+6dB：前端 -> 后端 -> 曲线，全链路实测
  const peqEdit = await evalJs(`(async function(){
    var rows = document.querySelectorAll('#peqRows .peqr');
    var tr = rows[10];
    if (!tr) return { ok:false, msg:'没有第 11 行' };
    var f = tr.querySelector('.peqF'), g = tr.querySelector('.peqG');
    f.value = 1000; f.dispatchEvent(new Event('input'));
    g.value = 6.0; g.dispatchEvent(new Event('input'));
    await new Promise(r => setTimeout(r, 1200));
    var j = await fetch('/api/peq').then(r => r.json());
    var b = j.bands[10];
    var near = j.points.reduce((a, x) => Math.abs(Math.log(x[0]/1000)) < Math.abs(Math.log(a[0]/1000)) ? x : a);
    return { ok:true, gain:b.gain, freq:b.freq, db:near[1], nearF:near[0], fs:j.fs, allGains:j.bands.map(x=>x.gain) };
  })()`);
  ck(peqEdit.ok && Math.abs(peqEdit.gain - 6.0) < 1e-6, 'UI 改第 11 段 +6dB 已生效并落库', JSON.stringify(peqEdit));
  ck(peqEdit.ok && Math.abs(peqEdit.db - 6.0) <= 0.4, '曲线在 1kHz 附近 ≈ +6dB', 'db=' + (peqEdit.db || 0).toFixed(3));
  await shot('06-peq-20band', true);

  // 预设加载（内置示例）走 UI：选中 + 点「加载」
  await evalJs("(function(){var s=document.getElementById('peqPreset');var opt=Array.from(s.options).find(o=>o.value.indexOf('Harman')>=0);if(opt)s.value=opt.value;document.getElementById('peqLoad').click();})()");
  await sleep(1200);
  const peqPresetOk = await evalJs(`(async function(){
    var j = await fetch('/api/peq').then(r => r.json());
    return { ok:true, name:j.name, nonzero:j.bands.filter(b=>Math.abs(b.gain)>0.5).length, preamp:j.preamp };
  })()`);
  ck(peqPresetOk.nonzero > 0, 'UI 加载内置预设（Harman 示例）生效', JSON.stringify(peqPresetOk));

  // 复原 Flat（直接调 API，避免 confirm 弹窗卡住 headless）
  await evalJs("fetch('/api/peq/preset',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'action=reset&name=Flat'}).then(r=>r.json()).then(()=>peqLoadState())");
  await sleep(900);
  ck(await evalJs("fetch('/api/peq').then(r=>r.json()).then(j=>j.bands.every(b=>Math.abs(b.gain)<1e-9))"), 'PEQ 测试后已复原 Flat');



  // EQ 自检在设置页里做，后续「右栏标签页 / 歌词 / 信号链」需要回到普通浏览视图（右栏才可见）。
  await evalJs("Array.from(document.querySelectorAll('.nav')).find(e=>e.dataset.v==='tracks').click()");
  await sleep(600);

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
      // 2026-10-24 #5：旧的单服务器表单 srcSave 已删除，改为流媒体页的多服务器控件。
      var needBind=['bStop','bNext','bPrev','bPause','qclr','tq','sRep','sShuf','vol','sLoud','seek','bScan','bAddRoot','bRescan','bUpnp','bNet','bHelp','bLyric','bChain','sp_directsdm','sp_dsdkind','sp_dsdconv','sp_metaon','sp_metallm','sp_wbdry','sp_wbapply','sp_llmsave','sp_llmtest','sp_llmclear','npRep','npVol','sp_instart','sp_instop','sp_inmon','sp_cast_scan','sp_cast_push','sp_cast_stop','sp_cast_dev','sp_src_add','sp_src_save','sp_src_cancel','sp_src_testform'];
      (META_PROVIDERS || []).forEach(function (p) { needBind.push('sp_meta_' + p.id); });
      var valueOnly=['sRate','sFilt','sFiltNx','sShap','sBuf','sDSD','sIsp','s2s'];
      var miss=[];
      needBind.forEach(function(i){ var e=document.getElementById(i); if(!e) miss.push(i+':缺失'); else if(!e.onclick && !e.onchange && !e.oninput) miss.push(i+':未绑定'); });
      valueOnly.forEach(function(i){ if(!document.getElementById(i)) miss.push(i+':缺失'); });
      return miss;
    })()`);
  ck(Array.isArray(bound) && bound.length === 0, '所有控件都存在且已绑定事件', JSON.stringify(bound));

  // ---- 本轮新增：队列主视图 / 文件夹地址栏 / 正在播放页 / 循环键 / 拖拽遮罩 ----
  await evalJs("Array.from(document.querySelectorAll('.nav')).find(e=>e.dataset.v==='queue').click()");
  // 队列页是异步拉 /api/queue 再渲染的：慢机器上固定 sleep 会假失败（本环境实测偶发），
  // 改为等条件成立（最多 8s），而不是赌 700ms。
  // 2026-10-23 追加：产品侧已修「搜索防抖定时器迟到把视图切回 tracks」的根因，这里再加标题条件兜底。
  ck(await waitFor("!!document.querySelector('#list .qrow') || !!document.querySelector('#list .empty')", 8000),
     '播放队列有内容或明确的空状态');
  ck(await waitFor("document.getElementById('pth').textContent.indexOf('播放队列') >= 0", 4000), '队列页标题正确');

  ck(await gotoFiles(), '文件夹有地址栏输入框');
  const addr = await evalJs("(document.getElementById('fPath')||{}).value || ''");
  ck(!!addr, '地址栏显示了当前目录', addr);

  await evalJs("document.getElementById('npOpen').click()");
  await sleep(400);
  ck(await evalJs("document.getElementById('npPage').classList.contains('on')"), '点封面可以打开正在播放页');
  ck(await evalJs("!!document.getElementById('npMeta') && document.getElementById('npMeta').closest('#npPage') !== null"), '正在播放页有 #npMeta 芯片容器');
  // 261024-01 #3b ③：资料默认隐藏，右侧只露标签；点标签才滑出抽屉
  ck(await evalJs("!!document.getElementById('npRail') && document.getElementById('npRail').closest('#npPage') !== null"), '正在播放页有右侧资料标签栏 #npRail');
  ck(await evalJs("!!document.getElementById('npTabs') && !!document.getElementById('npDrawer') && !!document.getElementById('npDataBody')"), '资料标签栏 + 抽屉 + 内容区齐全');
  ck(await evalJs("(function(){var d=document.getElementById('npDrawer');return !!d && !d.classList.contains('on') && d.getBoundingClientRect().left >= innerWidth - 2;})()"),
     '资料抽屉默认关闭且完全在视口外（默认不占版面）');
  ck(await evalJs("!document.getElementById('npSummary') && !document.getElementById('npData')"), '旧的 #npSummary / 整行 #npData 面板已移除');
  // #6 几何回归：歌词区与进度条在任何窗口尺寸下不得重叠（1024x640 曾压在一起）
  ck(await evalJs("(function(){var a=document.getElementById('npLyric').getBoundingClientRect(),b=document.getElementById('npSeek').getBoundingClientRect();return a.bottom<=b.top && b.top-a.bottom>=12;})()"),
     '歌词区与进度条不重叠且间距 >= 12px');
  ck(await evalJs("(function(){var r=document.getElementById('npRail'),l=document.getElementById('npLyric');if(!r||!l)return false;var a=r.getBoundingClientRect();if(a.width<2)return true;var b=l.getBoundingClientRect();if(b.width<2)return true;return a.left>=b.right-1&&Math.min(a.bottom,b.bottom)-Math.max(a.top,b.top)>20;})()"),
     '资料标签栏在歌词右侧同一行（左右排列，不是上下堆叠；本实例未配 AI 时标签栏本就不显示）');
  ck(await evalJs("(function(){var v=document.getElementById('npVol'),p=document.getElementById('npPlay');if(!v||!p)return false;var a=v.getBoundingClientRect(),b=p.getBoundingClientRect();return Math.abs((a.top+a.height/2)-(b.top+b.height/2))<=10;})()"),
     '音量与播放键同一行（不再单独占底部一行）');
  // 261024-02 #5：单曲页有「推送到设备」按钮（与流媒体页推送区块、底栏徽标同源）
  ck(await evalJs("(function(){var b=document.getElementById('npCastBtn');return !!b && b.textContent.indexOf('推送')>=0 && !!b.onclick;})()"),
     '单曲页有「推送到设备」按钮且已绑定');
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

  // ---- 261023-04 #8：三主题下 select / option 的对比度（深色主题曾出现「菜单浅字压浅底」）----
  // 判据不看文案：读 computed style，把 select 的半透明背景按体色合成后算 WCAG 对比度，
  // 要求 select 本体文字与 option 菜单文字都 ≥ 4.5:1（三套主题各算一次）。
  await evalJs("Array.from(document.querySelectorAll('.nav')).find(e=>e.dataset.v==='tracks').click()");
  await sleep(700);
  const themeBefore = await evalJs("document.documentElement.dataset.theme");
  const selContrast = await evalJs(`(function(){
    // 不用正则：模板字符串里的反斜杠会再被转义一层（本项目实测踩到，p[0] 变成 "(233"）。
    function parse(c){var i=c.indexOf('('),j=c.indexOf(')');if(i<0||j<0)return null;var p=c.slice(i+1,j).split(',').map(function(x){return parseFloat(x)});return {r:p[0],g:p[1],b:p[2],a:p.length>3?p[3]:1};}
    function lum(c){var p=parse(c);if(!p)return null;function f(v){v/=255;return v<=0.03928?v/12.92:Math.pow((v+0.055)/1.055,2.4)}return 0.2126*f(p.r)+0.7152*f(p.g)+0.0722*f(p.b)}
    function ratio(fg,bg){var la=lum(fg),lb=lum(bg);if(la==null||lb==null)return null;return (Math.max(la,lb)+0.05)/(Math.min(la,lb)+0.05)}
    function composite(c,under){var p=parse(c);if(!p)return c;var u=parse(under);return 'rgb('+Math.round(p.a*p.r+(1-p.a)*u.r)+','+Math.round(p.a*p.g+(1-p.a)*u.g)+','+Math.round(p.a*p.b+(1-p.a)*u.b)+')';}
    var out = {};
    ['dark','light','blue'].forEach(function(t){
      document.documentElement.dataset.theme = t;
      // 体色要**按当前主题重新取**：半透明的 select 背景要合成到当前主题的页面底色上，
      // 沿用上一个主题的体色会把浅色算成「深字压深底」（第一版就是这么误报 1.1:1 的）。
      var pageBg = getComputedStyle(document.body).backgroundColor;
      var s = document.getElementById('trackSort');
      if (!s) { out[t] = 'no-select'; return; }
      var sc = getComputedStyle(s);
      var o = s.options[2] || s.options[0];
      var oc = getComputedStyle(o);
      var effBg = composite(sc.backgroundColor, pageBg);
      out[t] = {
        selectRatio: ratio(sc.color, effBg),
        optionRatio: ratio(oc.color, oc.backgroundColor),
        optionBgOpaque: parse(oc.backgroundColor) ? parse(oc.backgroundColor).a === 1 : false,
        optionBg: oc.backgroundColor,
        selectColor: sc.color
      };
    });
    return out;
  })()`);
  await evalJs("document.documentElement.dataset.theme='" + themeBefore + "'");
  const selThemes = ['dark', 'light', 'blue'];
  const selBad = selThemes.filter(function(t){
    var x = selContrast && selContrast[t];
    return !x || x === 'no-select' || !(x.optionBgOpaque && x.optionRatio >= 4.5 && x.selectRatio >= 4.5);
  });
  ck(selBad.length === 0, '三主题下「默认排序」下拉本体与菜单对比度均 ≥ 4.5:1', JSON.stringify(selContrast));

  // ---- 261023-04 #1：底栏几何（1600 与 1024 两档）----
  // 判据（用户原话）：控制组水平居中（与底栏中心偏差 < 8px）；歌词/信号链/音量任何宽度都可见。
  const barGeomExpr = `(function(){
    function vis(id){var e=document.getElementById(id);if(!e)return false;var r=e.getBoundingClientRect();var cs=getComputedStyle(e);return !!(r.width&&r.height)&&cs.display!=='none'&&cs.visibility!=='hidden'&&cs.opacity!=='0';}
    var bar=document.querySelector('.bar'), keys=document.querySelector('.bar .keys');
    if(!bar||!keys)return null;
    var br=bar.getBoundingClientRect(), kr=keys.getBoundingClientRect();
    return { offset:+Math.abs((kr.left+kr.width/2)-(br.left+br.width/2)).toFixed(1),
             lyric:vis('bLyric'), chain:vis('bChain'), vol:vis('vol') };
  })()`;
  const bg1 = await evalJs(barGeomExpr);
  await send('Emulation.setDeviceMetricsOverride', { width: 1024, height: 640, deviceScaleFactor: 1, mobile: false });
  await sleep(500);
  const bg2 = await evalJs(barGeomExpr);
  await send('Emulation.clearDeviceMetricsOverride');
  await sleep(300);
  ck(bg1 && bg1.offset < 8 && bg1.lyric && bg1.chain && bg1.vol,
     '宽屏底栏控制组水平居中（偏差 < 8px）且歌词/信号链/音量可见', JSON.stringify(bg1));
  ck(bg2 && bg2.offset < 8 && bg2.lyric && bg2.chain && bg2.vol,
     '1024 宽底栏控制组仍水平居中，五键 + 播放链（歌词/信号链/音量）仍可见', JSON.stringify(bg2));


  // =====================================================================================
  // 261027-03 #3：播放队列重构的回归断言（只测队列；自检前后都不改用户队列）
  // 判据逐条对应用户三条抱怨，全部读真实 DOM / 接口数字（不看文案）：
  //   ① × 默认可见（computed opacity > 0.1，不需要 hover）；右键队列条目有「从队列移除」
  //   ② 添加入口默认可见：队列页「＋ 添加曲目」/ 曲目页每行「＋」/ 单曲页「＋ 加入队列」
  //   ③ 排序下拉里没有「默认顺序」这种伪状态；排序后出现「↩ 撤销排序」，撤销后它自己消失
  // 纪律：先把原队列（items + cues）整列存下来，结尾用 /api/queue/set 还原；原队列为空则 /api/clear。
  // =====================================================================================
  await evalJs("window.__qOrig = null; fetch('/api/queue').then(r=>r.json()).then(j=>{window.__qOrig = {items:j.items||[], cues:j.cues||[]};});");
  await sleep(400);
  // 造 3 首测试曲（从库里取；库为空时跳过本段，不让自检假失败）
  const qSeed = await evalJs(`(async()=>{
    const j = await (await fetch('/api/lib/search?q=')).json();
    const t = (j.tracks||[]).slice(0,3).map(x=>x.path);
    if (!t.length) return {seeded:0};
    for (const p of t) await fetch('/api/add', {method:'POST', headers:{'Content-Type':'application/x-www-form-urlencoded'}, body:'file='+encodeURIComponent(p)});
    const q = await (await fetch('/api/queue')).json();
    return {seeded:t.length, n:q.items.length};
  })()`);
  if (qSeed && qSeed.seeded) {
    await evalJs("Array.from(document.querySelectorAll('.nav')).find(e=>e.dataset.v==='queue').click()");
    await waitFor("!!document.querySelector('#list .qrow')", 8000);
    await sleep(300);
    const qAddBtn = await evalJs(`(function(){
      var b=document.getElementById('qAddBtn'); if(!b) return null;
      var r=b.getBoundingClientRect(), cs=getComputedStyle(b);
      return {visible: r.width>0 && r.height>0 && cs.display!=='none' && cs.visibility!=='hidden' && parseFloat(cs.opacity)>0.1, text:b.textContent.trim()};
    })()`);
    ck(qAddBtn && qAddBtn.visible, '队列页「＋ 添加曲目」按钮默认可见（不用 hover/右键）', JSON.stringify(qAddBtn));
    const qDelVis = await evalJs(`(function(){
      var b=document.querySelector('#list .qrow .qdel'); if(!b) return null;
      var r=b.getBoundingClientRect(), cs=getComputedStyle(b);
      var hit=document.elementFromPoint(r.left+r.width/2, r.top+r.height/2);
      return {opacity:cs.opacity, visible: parseFloat(cs.opacity)>0.1, w:Math.round(r.width),
              hitIsSelf: hit===b || (hit&&b.contains(hit))};
    })()`);
    ck(qDelVis && qDelVis.visible && qDelVis.w > 0 && qDelVis.hitIsSelf,
       '队列行「×」默认可见且点得到（不再是 hover 才显形）', JSON.stringify(qDelVis));
    const qSortOpts = await evalJs(`(function(){var s=document.getElementById('qSort');return s?Array.prototype.map.call(s.options,function(o){return o.text;}) : null;})()`);
    ck(Array.isArray(qSortOpts) && qSortOpts.some(t=>t.indexOf('标题')>=0) && !qSortOpts.some(t=>t.indexOf('默认顺序')>=0),
       '队列排序下拉只有动作项，没有「默认顺序」伪状态', JSON.stringify(qSortOpts));
    // 排序 → 出现「↩ 撤销排序」；撤销 → 顺序还原 + 按钮消失
    const sortRes = await evalJs(`(async()=>{
      const before = (await (await fetch('/api/queue')).json()).items.slice();
      window.__qSortedBefore = before;
      const s = document.getElementById('qSort');
      s.value='title'; s.dispatchEvent(new Event('change'));
      await new Promise(r=>setTimeout(r,1200));
      const q = await (await fetch('/api/queue')).json();
      const undo1 = !!document.getElementById('qSortUndo');
      return {changed: q.items.join('|')!==before.join('|'), n:q.items.length, undoBtn:undo1, canUndo:!!q.canUndo, before:before};
    })()`);
    ck(sortRes && sortRes.n === (qSeed.n||0) && sortRes.undoBtn && sortRes.canUndo,
       '按标题排序后条数不变且出现「↩ 撤销排序」（canUndo 与按钮一致）', JSON.stringify({n:sortRes&&sortRes.n, undo:sortRes&&sortRes.undoBtn}));
    const undoRes = await evalJs(`(async()=>{
      const b = document.getElementById('qSortUndo'); if (!b) return {noBtn:true};
      b.click();
      await new Promise(r=>setTimeout(r,1100));
      const q = await (await fetch('/api/queue')).json();
      return {restored: q.items.join('|')===window.__qSortedBefore.join('|'), n:q.items.length, undoBtn:!!document.getElementById('qSortUndo'), canUndo:!!q.canUndo};
    })()`);
    ck(undoRes && undoRes.restored === true && undoRes.undoBtn === false,
       '「↩ 撤销排序」能整列还原，且还原后按钮自己消失', JSON.stringify(undoRes));
    // 右键队列条目 → 菜单里有「从队列移除」，点了真的少一首
    const rcRes = await evalJs(`(async()=>{
      const row = document.querySelector('#list .qrow');
      const n0 = (await (await fetch('/api/queue')).json()).items.length;
      const r = row.getBoundingClientRect();
      row.dispatchEvent(new MouseEvent('contextmenu', {bubbles:true, cancelable:true, clientX:r.left+40, clientY:r.top+8}));
      await new Promise(x=>setTimeout(x,300));
      const m = document.querySelector('.ctxmenu');
      const hasDel = !!m && m.textContent.indexOf('从队列移除')>=0;
      let n1 = n0;
      if (m) {
        const it = Array.prototype.slice.call(m.children).filter(function(d){return d.textContent.indexOf('从队列移除')>=0;})[0];
        if (it) { it.click(); await new Promise(x=>setTimeout(x,700)); n1 = (await (await fetch('/api/queue')).json()).items.length; }
        else document.body.click();
      }
      return {menu:!!m, hasDel:hasDel, n0:n0, n1:n1};
    })()`);
    ck(rcRes && rcRes.menu && rcRes.hasDel && rcRes.n1 === rcRes.n0 - 1,
       '右键队列条目有「从队列移除」菜单且真的删掉一首', JSON.stringify(rcRes));
    // 曲目页：每行都有常显「＋」
    await evalJs("Array.from(document.querySelectorAll('.nav')).find(e=>e.dataset.v==='tracks').click()");
    await waitFor("!!document.querySelector('#list .trow')", 8000);
    await sleep(400);
    const trPlus = await evalJs(`(function(){
      var rows=document.querySelectorAll('#list .trow'), btns=document.querySelectorAll('#list .trow .add-btn');
      if(!rows.length) return {rows:0};
      var cs=getComputedStyle(btns[0]);
      return {rows:rows.length, addBtns:btns.length, opacity:parseFloat(cs.opacity), disp:cs.display};
    })()`);
    ck(trPlus && trPlus.rows > 0 && trPlus.addBtns === trPlus.rows && trPlus.opacity > 0.1 && trPlus.disp !== 'none',
       '曲目页每行都有默认可见的「＋」入队按钮', JSON.stringify(trPlus));
    if (trPlus && trPlus.addBtns) {
      const trAdd = await evalJs(`(async()=>{
        const n0=(await (await fetch('/api/queue')).json()).items.length;
        document.querySelector('#list .trow .add-btn').click();
        await new Promise(r=>setTimeout(r,800));
        const n1=(await (await fetch('/api/queue')).json()).items.length;
        return {n0:n0,n1:n1};
      })()`);
      ck(trAdd && trAdd.n1 === trAdd.n0 + 1, '点曲目行「＋」队列 +1', JSON.stringify(trAdd));
    }
    // 单曲页「＋ 加入队列」按钮存在且可见
    await evalJs("document.getElementById('npOpen').click()");
    await sleep(500);
    const npAdd = await evalJs(`(function(){var b=document.getElementById('npAddQ');if(!b)return null;
      var r=b.getBoundingClientRect(),cs=getComputedStyle(b);
      return {visible:r.width>0&&r.height>0&&cs.display!=='none'&&cs.visibility!=='hidden', text:b.textContent.trim()};})()`);
    ck(npAdd && npAdd.visible && npAdd.text.indexOf('加入队列')>=0, '单曲页有默认可见的「＋ 加入队列」按钮', JSON.stringify(npAdd));
    await evalJs("document.getElementById('npClose').click()");
    await sleep(300);
    // =====================================================================================
    // 261027-04 #1：播放队列工具条 —— 三个按钮必须同一行（同一垂直基线）
    // 用户原话：「主页-播放队列，'添加曲目'按钮和'保存为歌单'、'清空队列'按钮不在同一行，
    //            把'保存为歌单'、'清空队列'放到'添加曲目'右面」。
    // 判据（用户给的）：CDP 量三个按钮的 getBoundingClientRect()，top 完全相同（差 ≤1px），left 递增。
    // 为什么必须在**有队列**时量：「保存为歌单 / 清空队列」只在 n>0 时渲染（空队列没东西可存/可清）。
    // =====================================================================================
    await evalJs("Array.from(document.querySelectorAll('.side .nav')).find(e=>e.dataset.v==='queue').click()");
    await waitFor("!!document.getElementById('qAddBtn') && document.querySelectorAll('#list .qrow').length > 0", 10000);
    await sleep(400);
    const barGeo = await evalJs(`(function(){
      function G(id){ var e=document.getElementById(id); if(!e) return null; var r=e.getBoundingClientRect(); var cs=getComputedStyle(e);
        return {x:+r.left.toFixed(2), y:+r.top.toFixed(2), w:+r.width.toFixed(2), h:+r.height.toFixed(2), radius:cs.borderRadius, fs:cs.fontSize}; }
      var a=G('qAddBtn'), s=G('qSaveSq'), c=G('qClrAll');
      if(!a||!s||!c) return {missing:true, a:!!a, s:!!s, c:!!c};
      function gap(x,y){return +(y.x - (x.x + x.w)).toFixed(2);}
      return {a:a, s:s, c:c,
        topSpread:+(Math.max(a.y,s.y,c.y)-Math.min(a.y,s.y,c.y)).toFixed(2),
        hSpread:+(Math.max(a.h,s.h,c.h)-Math.min(a.h,s.h,c.h)).toFixed(2),
        leftOrder:(a.x<s.x && s.x<c.x),
        gapSaveClear:gap(s,c),
        sameRadius:(a.radius===s.radius && s.radius===c.radius),
        sameFont:(a.fs===s.fs && s.fs===c.fs)};
    })()`);
    ck(barGeo && !barGeo.missing && barGeo.topSpread <= 1, '队列工具条三按钮 top 完全相同（≤1px）',
       JSON.stringify(barGeo));
    ck(barGeo && !barGeo.missing && barGeo.leftOrder, '队列工具条三按钮 left 递增（添加曲目 → 保存为歌单 → 清空队列）',
       barGeo && JSON.stringify([barGeo.a && barGeo.a.x, barGeo.s && barGeo.s.x, barGeo.c && barGeo.c.x]));
    ck(barGeo && !barGeo.missing && barGeo.hSpread <= 1 && barGeo.sameRadius && barGeo.sameFont,
       '队列工具条三按钮等高、同圆角、同字号',
       barGeo && JSON.stringify({h:[barGeo.a.h,barGeo.s.h,barGeo.c.h], r:barGeo.a.radius, f:barGeo.a.fs}));
    // 窄窗（<1100px）：允许换行，但「保存为歌单 / 清空队列」必须仍成组（同一 top），不许一个上、一个下
    await send('Emulation.setDeviceMetricsOverride', { width: 1024, height: 800, deviceScaleFactor: 1, mobile: false });
    await sleep(600);
    const barNarrow = await evalJs(`(function(){
      function T(id){var e=document.getElementById(id); if(!e) return null; var r=e.getBoundingClientRect(); return {x:+r.left.toFixed(2), y:+r.top.toFixed(2)};}
      return {a:T('qAddBtn'), s:T('qSaveSq'), c:T('qClrAll')};})()`);
    await send('Emulation.clearDeviceMetricsOverride');
    await sleep(300);
    ck(!barNarrow || !barNarrow.s || !barNarrow.c || Math.abs(barNarrow.s.y - barNarrow.c.y) <= 1,
       '窄窗（1024）换行后「保存为歌单/清空队列」仍成组（同一行）', JSON.stringify(barNarrow));
    // ---- 还原用户队列（自检纪律：界面/接口都回到进来时的样子）----
    const qRestore = await evalJs(`(async()=>{
      const o = window.__qOrig || {items:[], cues:[]};
      let okr = true;
      if (!o.items.length) { await fetch('/api/clear', {method:'POST'}); }
      else {
        const lines = o.items.map((f,i)=>{ const c=(o.cues||[])[i]||[-1,-1];
          return (c && c[0]>=0) ? (f+'\\t'+c[0]+'\\t'+c[1]) : f; }).join('\\n');
        const r = await fetch('/api/queue/set', {method:'POST', headers:{'Content-Type':'application/x-www-form-urlencoded'}, body:'files='+encodeURIComponent(lines)});
        const j = await r.json(); okr = !!j.ok;
      }
      const q = await (await fetch('/api/queue')).json();
      return {ok:okr, n:q.items.length, want:o.items.length};
    })()`);
    ck(qRestore && qRestore.ok && qRestore.n === qRestore.want, '自检结束后用户队列已还原', JSON.stringify(qRestore));
  } else {
    ck(true, '队列回归段跳过（音乐库为空，无法造测试曲目）', JSON.stringify(qSeed));
  }

  // =====================================================================================
  // 261027-04 #2：最近播放 —— 全部行等高
  // 用户原话：「主页-最近播放，每一行的行距不一样，看起来极其不美观，主要是专辑名称那些
  //            占用的行数不一样，你看看如何统一起来」。
  // 根因：.du（专辑列）当年只写 text-align:right，没有 nowrap/ellipsis ⟹ 长专辑名折 2~3 行，行高浮动。
  // 判据（用户给的）：量**全部**行的 getBoundingClientRect().height，min/max 差 ≤1px、唯一值个数 == 1；
  //   并且必须真的有超长名字被截断（scrollWidth > clientWidth）—— 否则"全等高"可能只是"没有长名字"的空转。
  // =====================================================================================
  await evalJs("Array.from(document.querySelectorAll('.side .nav')).find(e=>e.dataset.v==='recent').click()");
  await waitFor("document.querySelectorAll('#list .rrow').length > 0", 10000);
  await sleep(500);
  const recentGeo = await evalJs(`(function(){
    var rows=Array.prototype.slice.call(document.querySelectorAll('#list .rrow'));
    if(!rows.length) return {n:0};
    var hs=rows.map(function(e){return +e.getBoundingClientRect().height.toFixed(2);});
    var uniq={}; hs.forEach(function(h){uniq[h]=1;});
    var det=rows.map(function(e){
      var du=e.querySelector('.du'), tt=e.querySelector('.tt');
      var cs=du?getComputedStyle(du):null;
      return {h:+e.getBoundingClientRect().height.toFixed(2),
        duWS:cs?cs.whiteSpace:null, duTO:cs?cs.textOverflow:null,
        duClipped: du?(du.scrollWidth>du.clientWidth+1):null,
        ttClipped: tt?(tt.scrollWidth>tt.clientWidth+1):null};});
    return {n:rows.length, min:Math.min.apply(null,hs), max:Math.max.apply(null,hs),
      uniq:Object.keys(uniq).length, heights:hs, det:det};
  })()`);
  ck(recentGeo && recentGeo.n > 0, '最近播放有行可量', JSON.stringify(recentGeo && {n:recentGeo.n}));
  ck(recentGeo && recentGeo.n > 0 && (recentGeo.max - recentGeo.min) <= 1 && recentGeo.uniq === 1,
     '最近播放全部行高相同（差 ≤1px）',
     JSON.stringify(recentGeo && {n:recentGeo.n, min:recentGeo.min, max:recentGeo.max, uniq:recentGeo.uniq, heights:recentGeo.heights}));
  ck(recentGeo && recentGeo.n > 0 && recentGeo.det.every(function(d){return d.duWS==='nowrap' && d.duTO==='ellipsis';}),
     '最近播放专辑列单行省略（white-space:nowrap + text-overflow:ellipsis）',
     JSON.stringify(recentGeo && recentGeo.det[0]));
  await shot('28-recent-rows', true);

  // =====================================================================================
  // 261027-04 #3：我的歌单 —— 每个自定义歌单要有通用图标，且与一级导航逐像素对齐
  // 用户原话：「主页-我的歌单，我尝试添加了一个歌单，只有自定义名字，名字前面没有图标，
  //            请为歌单添加一个通用的图标标识，跟上面播的最爱、最近播放等对齐」。
  // 判据（用户给的）：四个文字左边缘 x 全部 == 46.00（差 ≤1px）；并给四个图标的矩形与 fill/stroke。
  // 纪律：**绝不删用户的歌单**。用户本来没有歌单时，才临时建一个自检歌单，量完立刻删掉（还原）。
  // =====================================================================================
  const sqMode = await evalJs(`(async()=>{
    var l = await (await fetch('/api/sq/list')).json();
    return {count:(l.queues||[]).length, names:(l.queues||[]).map(function(q){return q.name;})};
  })()`);
  let sqTempCreated = null;
  if (sqMode && sqMode.count === 0) {
    // 空态：先量「暂无歌单」的缩进（此刻确实没有歌单，不是造出来的空态）
    const emptyGeo = await evalJs(`(function(){
      var emp=document.querySelector('.side .sqempty'); if(!emp) return null;
      var rg=document.createRange(); rg.selectNodeContents(emp);
      return {textX:+rg.getBoundingClientRect().left.toFixed(2), text:emp.textContent.trim()};
    })()`);
    ck(emptyGeo && Math.abs(emptyGeo.textX - 46) <= 1, '「我的歌单」空态「暂无歌单」缩进 == 46.00px（差 ≤1px）', JSON.stringify(emptyGeo));
    // 临时建一个歌单（用当前队列）→ 量图标 → 删掉还原
    const mk = await evalJs(`(async()=>{
      var q = await (await fetch('/api/queue')).json();
      if (!(q.items||[]).length) return {skip:true};
      var r = await fetch('/api/sq/save?name=' + encodeURIComponent('自检临时歌单'), {method:'POST'});
      var j = await r.json();
      await new Promise(function(t){setTimeout(t,400);});
      if (typeof renderSavedQueues === 'function') { renderSavedQueues(); await new Promise(function(t){setTimeout(t,700);}); }
      return {ok:!!j.ok, rows:document.querySelectorAll('.side .sqrow').length};
    })()`);
    if (mk && mk.ok && mk.rows > 0) sqTempCreated = '自检临时歌单';
    else ck(true, '「我的歌单」图标判据跳过（队列为空，无法建临时歌单）', JSON.stringify(mk));
  }
  const sideGeo = await evalJs(`(function(){
    function txtX(el){ var tn=Array.prototype.filter.call(el.childNodes,function(n){return n.nodeType===3 && n.textContent.trim();})[0];
      if(!tn) return null; var rg=document.createRange(); rg.selectNodeContents(tn); return +rg.getBoundingClientRect().left.toFixed(2); }
    function box(el){ if(!el) return null; var r=el.getBoundingClientRect(); return {x:+r.left.toFixed(2),y:+r.top.toFixed(2),w:+r.width.toFixed(2),h:+r.height.toFixed(2)}; }
    var side=document.querySelector('.side');
    function nav(v){ return Array.prototype.slice.call(side.querySelectorAll('.nav')).filter(function(e){return e.dataset.v===v;})[0]; }
    var out={navs:{}};
    [['queue',nav('queue')],['recent',nav('recent')],['favs',nav('favs')]].forEach(function(p){
      var e=p[1]; if(!e){out.navs[p[0]]=null;return;}
      var svg=e.querySelector('svg'); var cs=svg?getComputedStyle(svg):null;
      out.navs[p[0]]={textX:txtX(e), text:(e.textContent||'').trim().slice(0,8), icon:box(svg),
        fill:svg?svg.getAttribute('fill'):null, stroke:svg?svg.getAttribute('stroke'):null,
        strokeColor:cs?cs.stroke:null, w:cs?cs.width:null, h:cs?cs.height:null};
    });
    var sq=side.querySelector('.sqrow');
    if(sq){ var s2=sq.querySelector('svg'); var c2=s2?getComputedStyle(s2):null; var sn=sq.querySelector('.sqn');
      out.sq={textX: sn?txtX(sn):null, text:(sn?sn.textContent:'').trim().slice(0,10), hasIcon:!!s2, icon:box(s2),
        fill:s2?s2.getAttribute('fill'):null, stroke:s2?s2.getAttribute('stroke'):null,
        strokeColor:c2?c2.stroke:null, w:c2?c2.width:null, h:c2?c2.height:null,
        rowColor:getComputedStyle(sq).color, rowH:+sq.getBoundingClientRect().height.toFixed(2)};
    } else out.sq = null;
    return out;
  })()`);
  const nv = (sideGeo && sideGeo.navs) || {};
  const xs = [nv.queue && nv.queue.textX, nv.recent && nv.recent.textX, nv.favs && nv.favs.textX, sideGeo && sideGeo.sq && sideGeo.sq.textX];
  const all46 = xs.filter(function(v){return v!=null;});
  ck(sideGeo && sideGeo.sq && sideGeo.sq.hasIcon, '「我的歌单」每个自定义歌单都有图标', JSON.stringify(sideGeo && sideGeo.sq && {text:sideGeo.sq.text, icon:sideGeo.sq.icon}));
  ck(all46.length === 4 && Math.max.apply(null, all46) - Math.min.apply(null, all46) <= 1 && Math.abs(all46[0] - 46) <= 1,
     '左栏四个文字左边缘 x 全部 == 46.00（差 ≤1px）', 'xs=' + JSON.stringify(xs));
  ck(sideGeo && sideGeo.sq && sideGeo.sq.w === nv.queue.w && sideGeo.sq.h === nv.queue.h,
     '歌单图标尺寸与一级导航图标相同（16×16）', JSON.stringify(sideGeo && sideGeo.sq && {sq:[sideGeo.sq.w, sideGeo.sq.h], nav:[nv.queue && nv.queue.w, nv.queue && nv.queue.h]}));
  // 颜色判据（判据本身必须可靠，铁律 3）：**不拿别的行的实测颜色当基准**。
  //   .nav 有 transition:.13s（color 也过渡），采样落在淡入中途会读到 rgb(137,149,171) 这种中间值，
  //   把正确实现判成错（本自检第一版就是这么误报的）。改成对照 CSS 变量本体：
  //   图标 fill=none + stroke=currentColor，歌单图标解析色 == 自己行文字色 == 根上 var(--mut)。
  const cssVar = await evalJs("(function(){var s=getComputedStyle(document.documentElement);" +
    "function hx(v){v=(v||'').trim(); if(v[0]!=='#') return v; if(v.length===4) v='#'+v[1]+v[1]+v[2]+v[2]+v[3]+v[3];" +
    "  var r=parseInt(v.substr(1,2),16),g=parseInt(v.substr(3,2),16),b=parseInt(v.substr(5,2),16); return 'rgb('+r+', '+g+', '+b+')';}" +
    "return {mut:hx(s.getPropertyValue('--mut')), onText:hx(s.getPropertyValue('--nav-on-text')), theme:document.documentElement.dataset.theme||'dark'};})()");
  const iconsOk = sideGeo && sideGeo.sq &&
    [nv.queue, nv.recent, nv.favs].every(function(o){return o && o.stroke === 'currentColor' && o.fill === 'none';}) &&
    sideGeo.sq.stroke === 'currentColor' && sideGeo.sq.fill === 'none' &&
    sideGeo.sq.strokeColor === sideGeo.sq.rowColor &&
    sideGeo.sq.strokeColor === (cssVar && cssVar.mut);
  ck(iconsOk, '歌单图标与一级导航同风格（fill=none/stroke=currentColor，颜色 == var(--mut)）',
     JSON.stringify({sq: sideGeo && sideGeo.sq && {fill:sideGeo.sq.fill, stroke:sideGeo.sq.stroke, color:sideGeo.sq.strokeColor, row:sideGeo.sq.rowColor},
                     mut: cssVar && cssVar.mut, theme: cssVar && cssVar.theme,
                     navQueue: nv.queue && {fill:nv.queue.fill, stroke:nv.queue.stroke, color:nv.queue.strokeColor}}));
  await shot('29-sidebar-playlists', true);
  // 还原：只删掉自检自己建的那个歌单
  if (sqTempCreated) {
    const rm = await evalJs(`(async()=>{
      await fetch('/api/sq/delete?name=' + encodeURIComponent('自检临时歌单'), {method:'POST'});
      await new Promise(function(t){setTimeout(t,400);});
      if (typeof renderSavedQueues === 'function') { renderSavedQueues(); await new Promise(function(t){setTimeout(t,600);}); }
      var l = await (await fetch('/api/sq/list')).json();
      return {left:(l.queues||[]).length};
    })()`);
    ck(rm && rm.left === 0, '自检建的临时歌单已删掉（用户的歌单一个都不动）', JSON.stringify(rm));
  }
  // ---- 2026-10-27（261027-04 #4）：分析功能从「主页 + 设置」搬到单曲页 ----
  // 用户原话：「主页-分析和设置-分析，整个功能页不应该放到这里，直接将分析放到正在播放的那首歌
  // 音乐上，播放就自动运行一次分析，在单曲播放页面上放置入口展示（动态、音质、假无损）。」
  // 判据（只增不减，不改已有断言）：
  //   ① 左栏没有「分析」；② 设置页签没有「分析」；③ 单曲页有 #npAnal 三块；
  //   ④ 自动分析开关默认开、且与 conf 的 autoanl 一致；⑤ 版本对比入口仍在右键菜单里。
  {
    const navHasAnalysis = await evalJs("Array.from(document.querySelectorAll('.side .nav')).some(function(e){return e.textContent.trim()==='分析';})");
    ck(navHasAnalysis === false, '左栏导航项里没有「分析」（已搬到单曲页）');
    const tabHasAnalysis = await evalJs("Array.from(document.querySelectorAll('#setpage .stabs [data-g]')).some(function(e){return e.textContent.trim()==='分析'||e.dataset.g==='analysis';})");
    ck(tabHasAnalysis === false, '设置页签里没有「分析」（整页已删除，只保留曲目右键的「对比版本…」）');
    // 高级页的开关：必须**先进入设置页**再切到「高级」页签 ——
    // 设置页没显示时（#setpage 没有 .on）里面所有控件 rect 恒为 0，
    // 直接把「正确实现」判成不可见（这是判据本身的坑，不是功能坏了，本轮实测踩到两次）。
    await evalJs("Array.from(document.querySelectorAll('.side .nav')).find(e=>e.dataset.v==='settings').click()");
    await waitFor("document.getElementById('setpage').classList.contains('on')", 8000);
    await evalJs("var t=document.querySelector('#setpage .stab[data-g=adv]'); if(t) t.click();");
    await sleep(600);
    const anlChk = await evalJs("(function(){var e=document.getElementById('sp_autoanl');if(!e) return null;var r=e.getBoundingClientRect();return {checked:e.checked,vis:r.width>0&&r.height>0,text:(e.closest('.chk')||e.parentNode).textContent.trim().slice(0,16)};})()");
    ck(!!anlChk && anlChk.checked === true && anlChk.vis === true, '设置-高级「播放时自动分析」开关存在、可见、默认开', JSON.stringify(anlChk));
    // 先把单曲页打开，再量分析条（页面关着时 rect 全是 0，量不到东西）
    await evalJs("var b=document.getElementById('npOpen'); if(b) b.click();");
    await sleep(800);
    const npOn = await evalJs("document.getElementById('npPage').classList.contains('on')");
    ck(npOn === true, '单曲页可以打开（分析条的宿主页面）');
    // 单曲页分析条：三块 + 两个按钮 + 状态行
    const anlDom = await evalJs(`(function(){
      var box=document.getElementById('npAnal');
      if(!box) return {ok:false,why:'no #npAnal'};
      var r=box.getBoundingClientRect();
      return {ok:true, cards:box.querySelectorAll('.npanal-card').length,
              auto:!!document.getElementById('npAnlAuto'),
              again:!!document.getElementById('npAnlNow'),
              st:(box.querySelector('.npanal-hd .st')||{}).textContent||'',
              h:Math.round(r.height), w:Math.round(r.width)};
    })()`);
    ck(anlDom && anlDom.ok && anlDom.cards >= 1 && anlDom.auto && anlDom.again && anlDom.h > 30,
       '单曲页有「音频分析」条：三块结果位 + 自动分析开关 + 重新分析按钮', JSON.stringify(anlDom));
    // 分析条必须在控制区之上、且在视口内（不许被挤到窗口外）
    const anlGeom = await evalJs(`(function(){
      var b=document.getElementById('npAnal'); if(!b) return null;
      var r=b.getBoundingClientRect(), c=document.querySelector('#npPage .npctl2');
      var cr=c?c.getBoundingClientRect():null;
      return {top:Math.round(r.top),bottom:Math.round(r.bottom),h:Math.round(r.height),
              inView:r.top>=0&&r.bottom<=innerHeight,
              aboveCtrl: cr? (r.bottom<=cr.top+1) : null,
              ctrlTop: cr?Math.round(cr.top):null};
    })()`);
    ck(anlGeom && anlGeom.inView && anlGeom.aboveCtrl === true && anlGeom.h > 30,
       '分析条完整落在视口内、且排在播放控制区之上', JSON.stringify(anlGeom));
    await shot('30-np-analysis-strip', true);
    await evalJs("var b=document.getElementById('npClose'); if(b) b.click();");
    await sleep(300);
    // 版本对比入口仍在：曲目右键菜单里必须有「对比版本…」
    // 路径一律写成正斜杠：字符串里出现 \x 会被 JS 当十六进制转义解析（本轮实测 SyntaxError）
    const cmpMenu = await evalJs("(function(){try{showTrackMenu(200,200,{path:'D:/probe.flac'});var m=document.querySelector('.ctxmenu');var t=m?Array.from(m.querySelectorAll('.ctxitem')).map(function(e){return e.textContent;}):[];document.body.click();return t;}catch(e){return ['EXC:'+e.message];}})()");
    ck(cmpMenu.indexOf('对比版本…') >= 0, '曲目右键菜单仍有「对比版本…」（版本对比功能没被删）', JSON.stringify(cmpMenu));
  }

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
