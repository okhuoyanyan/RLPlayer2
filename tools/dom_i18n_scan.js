// dom_i18n_scan.js — 从真实渲染的界面里采集所有中文文本（i18n 词条的权威来源）
// 为什么不用源码正则：源码里 6 万汉字是注释，正则分不清"注释"和"UI 文案"，
// 实测 236 条里只有 95 条是真的界面文字。渲染后的 DOM 才是用户真正看到的东西。
const { spawn } = require('child_process');
const fs = require('fs');
const path = require('path');
const os = require('os');

const URL_ = process.argv[2] || 'http://127.0.0.1:19020/';
const OUT = process.argv[3] || 'D:/RTA-hqplayer/build/dom_strings.json';
const EDGE = ['C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\msedge.exe',
              'C:\\Program Files\\Microsoft\\Edge\\Application\\msedge.exe'].find(p => fs.existsSync(p));
if (!EDGE) { console.error('找不到 Edge'); process.exit(2); }
const PORT = 9810 + Math.floor(Math.random() * 200);
const udd = path.join(os.tmpdir(), 'edge_i18n_' + process.pid + '_' + Date.now());
const child = spawn(EDGE, ['--headless=new', '--disable-gpu', '--no-first-run', '--no-default-browser-check',
  '--user-data-dir=' + udd, '--remote-debugging-port=' + PORT, '--window-size=1600,1000', 'about:blank'],
  { stdio: 'ignore' });
const sleep = ms => new Promise(r => setTimeout(r, ms));

async function targets() {
  for (let i = 0; i < 80; i++) {
    try {
      const r = await fetch('http://127.0.0.1:' + PORT + '/json/list');
      const j = await r.json();
      const p = j.filter(t => t.type === 'page');
      if (p.length) return p;
    } catch (e) {}
    await sleep(250);
  }
  throw new Error('DevTools 端口没起来');
}
let ws, msgId = 0;
const pending = new Map();
function send(method, params) {
  return new Promise((res, rej) => {
    const id = ++msgId;
    pending.set(id, { res, rej });
    ws.send(JSON.stringify({ id, method, params: params || {} }));
  });
}
const evalJs = async e => {
  const r = await send('Runtime.evaluate', { expression: e, awaitPromise: true, returnByValue: true });
  if (r.exceptionDetails) return null;
  return r.result && r.result.value;
};

const COLLECT = `
(function(){
  var out = [];
  var re = /[\\u4e00-\\u9fff]/;
  var w = document.createTreeWalker(document.body, NodeFilter.SHOW_TEXT, null);
  var n;
  while ((n = w.nextNode())) {
    var s = String(n.nodeValue || '').replace(/\\s+/g, ' ').trim();
    if (!s || !re.test(s) || s.length > 60) continue;
    if (n.parentElement && n.parentElement.closest('script,style')) continue;
    out.push(s);
  }
  var at = ['title','placeholder','aria-label','alt'];
  var els = document.body.querySelectorAll('*');
  for (var i = 0; i < els.length; i++) {
    for (var k = 0; k < at.length; k++) {
      var v = els[i].getAttribute(at[k]);
      if (!v) continue;
      var t = String(v).replace(/\\s+/g, ' ').trim();
      if (!t || !re.test(t) || t.length > 60) continue;
      out.push(t);
    }
  }
  return out;
})()`;

(async () => {
  const pages = await targets();
  ws = new WebSocket(pages[0].webSocketDebuggerUrl);
  await new Promise((res, rej) => { ws.onopen = res; ws.onerror = rej; });
  ws.onmessage = ev => {
    const m = JSON.parse(ev.data);
    if (m.id && pending.has(m.id)) {
      const p = pending.get(m.id); pending.delete(m.id);
      m.error ? p.rej(new Error(JSON.stringify(m.error))) : p.res(m.result);
    }
  };
  await send('Runtime.enable');
  await send('Page.enable');
  await send('Page.navigate', { url: URL_ });
  await sleep(3500);

  const all = new Map();
  const add = arr => { if (!arr) return; for (const s of arr) all.set(s, (all.get(s) || 0) + 1); };

  const navs = ['albums', 'artists', 'songs', 'queue', 'fav', 'recent', 'settings'];
  for (const v of navs) {
    await evalJs("(function(){var e=Array.from(document.querySelectorAll('.nav')).find(function(x){return x.dataset.v==='" + v + "';});if(e)e.click();})()");
    await sleep(1400);
    add(await evalJs(COLLECT));
    if (v === 'settings') {
      const stabs = await evalJs("Array.from(document.querySelectorAll('.stab')).map(function(x){return x.dataset.g;})");
      for (const g of (stabs || [])) {
        await evalJs("(function(){var e=Array.from(document.querySelectorAll('.stab')).find(function(x){return x.dataset.g==='" + g + "';});if(e)e.click();})()");
        await sleep(900);
        add(await evalJs(COLLECT));
      }
    }
  }
  await evalJs("(function(){var e=document.querySelector('.trow');if(e)e.click();})()");
  await sleep(1500);
  add(await evalJs(COLLECT));

  const list = [...all.keys()].sort((a, b) => b.length - a.length || a.localeCompare(b));
  fs.writeFileSync(OUT, JSON.stringify(list, null, 0), 'utf8');
  console.log('unique strings:', list.length);
  console.log('sample:', list.slice(0, 15).join(' | '));
  ws.close(); try { child.kill(); } catch (_) {}
  process.exit(0);
})().catch(e => { console.error('失败: ' + e.message); try { child.kill(); } catch (_) {} process.exit(2); });
