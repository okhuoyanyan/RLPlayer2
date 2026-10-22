// uishots.js — 抓一组验收截图（主题 / 设置页 / 主界面）
const { spawn } = require('child_process');
const fs = require('fs');
const path = require('path');
const os = require('os');

const URL_ = process.argv[2] || 'http://127.0.0.1:8902/';
const OUT = process.argv[3] || path.join(os.tmpdir(), 'rta_shots');
fs.mkdirSync(OUT, { recursive: true });

const EDGE = ['C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\msedge.exe',
              'C:\\Program Files\\Microsoft\\Edge\\Application\\msedge.exe'].find(p => fs.existsSync(p));
if (!EDGE) { console.error('找不到 Edge'); process.exit(2); }

const PORT = 9800 + Math.floor(Math.random() * 300);
const udd = path.join(os.tmpdir(), 'edge_shots_' + process.pid + '_' + Date.now());
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
const evalJs = async e => {
  const r = await send('Runtime.evaluate', { expression: e, awaitPromise: true, returnByValue: true });
  return r.result && r.result.value;
};
async function shot(name) {
  const r = await send('Page.captureScreenshot', { format: 'png' });
  fs.writeFileSync(path.join(OUT, name + '.png'), Buffer.from(r.data, 'base64'));
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
  await sleep(2600);

  const themes = [['dark', '20-dark'], ['light', '21-light'], ['blue', '22-blue']];
  for (const t of themes) {
    await evalJs("document.querySelector('.sw[data-theme=" + t[0] + "]').click()");
    await sleep(500);
    await evalJs("Array.from(document.querySelectorAll('.nav')).find(e=>e.dataset.v==='albums').click()");
    await sleep(800);
    await shot(t[1] + '-albums');
    await evalJs("Array.from(document.querySelectorAll('.nav')).find(e=>e.dataset.v==='settings').click()");
    await sleep(800);
    await shot(t[1] + '-settings');
  }
  console.log('截图目录: ' + OUT);
  ws.close(); child.kill(); process.exit(0);
})().catch(e => { console.error('失败: ' + e.message); try { child.kill(); } catch (_) {} process.exit(2); });
