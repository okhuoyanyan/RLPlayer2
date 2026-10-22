// uiprobe.js — 在真实浏览器里执行一段脚本并打印结果（界面联调用）
// 用法: node tools/uiprobe.js <url> <expression-file>
const { spawn } = require('child_process');
const fs = require('fs');
const path = require('path');
const os = require('os');

const URL_ = process.argv[2];
const EXPR = fs.readFileSync(process.argv[3], 'utf8');
const OUT = path.join(os.tmpdir(), 'rta_probe');
fs.mkdirSync(OUT, { recursive: true });

const EDGE = ['C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\msedge.exe',
              'C:\\Program Files\\Microsoft\\Edge\\Application\\msedge.exe'].find(p => fs.existsSync(p));
if (!EDGE) { console.error('找不到 Edge'); process.exit(2); }

const PORT = 9900 + Math.floor(Math.random() * 90);
const udd = path.join(os.tmpdir(), 'edge_probe_' + process.pid + '_' + Date.now());
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
  await sleep(2200);
  const r = await send('Runtime.evaluate', { expression: EXPR, awaitPromise: true, returnByValue: true });
  if (r.exceptionDetails) { console.error('JS 异常: ' + JSON.stringify(r.exceptionDetails.text)); process.exit(3); }
  console.log(JSON.stringify(r.result && r.result.value, null, 1));
  ws.close(); child.kill(); process.exit(0);
})().catch(e => { console.error('失败: ' + e.message); try { child.kill(); } catch (_) {} process.exit(2); });
