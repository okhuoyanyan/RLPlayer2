#!/usr/bin/env node
'use strict';
// tools/ui_scan.js —— 在【真实 WebView2 窗口】里扫描 UI：越界 / 文字被裁 / 可点性 / ? 浮层。
//
// 为什么不用 headless（铁律 11）：headless 视口比真窗口宽，量不出"按钮被挤出可视区"。
// 为什么不用 CDP：1.5.12 的启动器不读 WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS（实测）⟹ 真窗口没有调试端口。
// 本脚本走"注入代理"：
//   1) build-uis/proxy.js 监听 8952，把后端 8900 的 HTML 注入 /__probe.js 后转发；
//   2) /__probe.js 在真窗口里轮询 /__cmd 执行 JS，结果 POST 回 /__report；
//   3) 本脚本用 /__send 把 JS 投进去并拿回 JSON —— 等价于在真窗口里 eval。
//   launcher 的导航字面量已改成 http://127.0.0.1:8952/?t=（只改 build-uis 下的副本）。
//
// 用法：
//   node tools/ui_scan.js check           # 探活：打印窗口 innerWidth/innerHeight/dpr/theme
//   node tools/ui_scan.js all             # 9 视图 + 11 设置页签：越界 + 文字 + 可点性
//   node tools/ui_scan.js tips            # 逐个 hover 所有 ? 浮层，量浮层矩形是否完整可见
//   node tools/ui_scan.js eval "<js>"     # 在真窗口里执行一段 JS 并打印返回值
const fs = require('fs');
const path = require('path');
const PROXY = process.env.RLPLAYER_PROXY || 'http://127.0.0.1:8952';
const BUILD = path.join(__dirname, '..', 'build-uis');

async function send(code, timeout = 30000) {
  const r = await fetch(PROXY + '/__send', {
    method: 'POST', headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ code, timeout }),
  });
  const j = await r.json();
  if (j.err) throw new Error('page error: ' + j.err);
  return j.val;
}
const sleep = ms => new Promise(r => setTimeout(r, ms));
const lib = n => fs.readFileSync(path.join(BUILD, n), 'utf8');
const VIEWS = ['albums', 'artists', 'tracks', 'files', 'remote', 'queue', 'recent', 'favs'];
const TABS = ['in', 'out', 'pcm', 'sdm', 'adv', 'eq', 'cal', 'meta', 'stream', 'diag', 'upd'];

async function install() {
  await send(lib('audit.js'));
  await send(lib('textscan.js'));
}

async function scanView(v) {
  await send("document.querySelector('.side .nav[data-v=" + v + "]').click(); 0;");
  await sleep(1200);
  return await send('return {view:(window.S&&S.view), audit:window.__audit(), text:window.__textscan()};');
}
async function scanTab(g) {
  await send("openSettingsTab('" + g + "'); 0;");
  await sleep(800);
  return await send('return {tab:"' + g + '", audit:window.__audit(), text:window.__textscan()};');
}
async function scanTips() {
  const out = [];
  for (const g of TABS) {
    await send("openSettingsTab('" + g + "'); 0;");
    await sleep(600);
    const r = await send(`
const L=document.getElementById('tipLayer'), vw=innerWidth, vh=innerHeight;
const sideR=document.querySelector('.side').getBoundingClientRect();
const tips=[...document.querySelectorAll('span.tip')].filter(e=>{const r=e.getBoundingClientRect();return r.width>0&&r.top>=0&&r.bottom<=vh;});
const rows=[];
for(const t of tips){ t.dispatchEvent(new MouseEvent('mouseover',{bubbles:true})); await new Promise(r=>setTimeout(r,55));
  const r=L.getBoundingClientRect();
  rows.push({tip:(t.getAttribute('data-tip')||'').slice(0,40), on:L.classList.contains('on'),
    rect:[Math.round(r.left),Math.round(r.top),Math.round(r.width),Math.round(r.height)],
    overRight:Math.round(r.right)-vw, overBottom:Math.round(r.bottom)-vh, underSide:Math.round(sideR.right)-Math.round(r.left)});
  t.dispatchEvent(new MouseEvent('mouseout',{bubbles:true})); }
return {tab:'` + g + `', n:tips.length, rows};`);
    out.push(r);
  }
  return out;
}

(async () => {
  const cmd = process.argv[2] || 'check';
  await send('1');
  if (cmd === 'eval') { console.log(JSON.stringify(await send(process.argv[3] || '1'))); return; }
  if (cmd === 'check') {
    console.log(JSON.stringify(await send('return [innerWidth,innerHeight,devicePixelRatio,document.documentElement.getAttribute("data-theme"),(S&&S.view)]')));
    return;
  }
  if (cmd === 'all') {
    await install();
    const res = { views: [], tabs: [] };
    for (const v of VIEWS) res.views.push(await scanView(v));
    for (const g of TABS) res.tabs.push(await scanTab(g));
    const bad = [];
    for (const x of res.views.concat(res.tabs)) {
      const k = x.view || x.tab;
      if (x.audit.offenders.length) bad.push({ page: k, offenders: x.audit.offenders });
      if (x.audit.clicks.length) bad.push({ page: k, clicks: x.audit.clicks });
      if (x.text.length) bad.push({ page: k, text: x.text });
    }
    console.log(JSON.stringify({ summary: { pages: res.views.length + res.tabs.length, bad: bad.length }, bad, raw: res }, null, 1));
    return;
  }
  if (cmd === 'tips') {
    const t = await scanTips();
    const bad = [];
    for (const tab of t) for (const r of tab.rows)
      if (!r.on || r.overRight > 0 || r.overBottom > 0 || r.underSide > 1) bad.push({ tab: tab.tab, ...r });
    console.log(JSON.stringify({ tabs: t.length, tips: t.reduce((s, x) => s + x.n, 0), bad }, null, 1));
    return;
  }
  console.log('usage: node tools/ui_scan.js check|all|tips|eval "<js>"');
})().catch(e => { console.error('ERR ' + e.message); process.exit(2); });
