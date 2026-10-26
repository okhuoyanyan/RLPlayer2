'use strict';
// capture.js — Edge 无头 + CDP 逐帧截取
// 用法：
//   node capture.js                       # 全量：30fps × 150s → frames/
//   node capture.js --dur 150 --fps 30    # 同上
//   node capture.js --times 5.2,40,100    # 只截指定时刻（QA 用）→ frames/
//   node capture.js --start 60 --end 61   # 区间截帧
const { spawn } = require('child_process');
const fs = require('fs');
const path = require('path');
const http = require('http');

const EDGE = 'C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\msedge.exe';
const PORT = 9337;

const argv = process.argv.slice(2);
const opt = {};
for (let i = 0; i < argv.length; i++) {
  if (argv[i].startsWith('--')) {
    opt[argv[i].slice(2)] = (argv[i + 1] && !argv[i + 1].startsWith('--')) ? argv[++i] : true;
  }
}
const FPS = parseFloat(opt.fps || 30);
const DUR = parseFloat(opt.dur || 150);
const OUT = path.join(__dirname, opt.out || 'frames');
const FORMAT = opt.format || 'png';
const QUALITY = parseInt(opt.quality || 92, 10);
const TIMES = opt.times ? String(opt.times).split(',').map(Number) : null;
const T0 = opt.start !== undefined ? parseFloat(opt.start) : 0;
const T1 = opt.end !== undefined ? parseFloat(opt.end) : DUR;

const sleep = ms => new Promise(r => setTimeout(r, ms));
function getJSON(url) {
  return new Promise((resolve, reject) => {
    http.get(url, res => {
      let d = '';
      res.on('data', c => d += c);
      res.on('end', () => { try { resolve(JSON.parse(d)); } catch (e) { reject(e); } });
    }).on('error', reject);
  });
}

async function main() {
  fs.mkdirSync(OUT, { recursive: true });
  const edge = spawn(EDGE, [
    '--headless=new',
    `--remote-debugging-port=${PORT}`,
    '--window-size=1920,1200',
    '--user-data-dir=' + path.join(__dirname, 'edgeprof'),
    '--no-first-run', '--no-default-browser-check',
    '--disable-gpu', '--hide-scrollbars', '--mute-audio',
    '--force-device-scale-factor=1',
    'about:blank',
  ], { stdio: 'ignore' });

  let targets = null;
  for (let i = 0; i < 80; i++) {
    try { targets = await getJSON(`http://127.0.0.1:${PORT}/json/list`); break; } catch (e) { await sleep(500); }
  }
  if (!targets) { console.error('FATAL: Edge CDP 未就绪'); edge.kill(); process.exit(1); }
  const page = targets.find(t => t.type === 'page');
  const ws = new WebSocket(page.webSocketDebuggerUrl);
  await new Promise((res, rej) => { ws.onopen = res; ws.onerror = rej; });

  let mid = 0; const pending = new Map();
  ws.onmessage = ev => {
    const m = JSON.parse(ev.data);
    if (m.id && pending.has(m.id)) { pending.get(m.id)(m); pending.delete(m.id); }
  };
  const send = (method, params = {}) => new Promise(resolve => {
    const id = ++mid; pending.set(id, resolve);
    ws.send(JSON.stringify({ id, method, params }));
  });

  await send('Page.enable');
  await send('Emulation.setDeviceMetricsOverride', { width: 1920, height: 1080, deviceScaleFactor: 1, mobile: false });
  const url = 'file:///' + path.resolve(__dirname, 'intro.html').replace(/\\/g, '/');
  await send('Page.navigate', { url });
  await sleep(2600);
  const chk = await send('Runtime.evaluate', { expression: 'typeof window.seek', returnByValue: true });
  if (chk.result.result.value !== 'function') {
    console.error('FATAL: 引擎未加载', JSON.stringify(chk.result));
    edge.kill(); process.exit(1);
  }
  await send('Runtime.evaluate', { expression: 'window.seek(0)' });

  const ext = FORMAT === 'jpeg' ? 'jpg' : 'png';
  const frameList = TIMES
    ? TIMES.map(tt => ({ t: tt, name: `t${String(tt).replace('.', '_')}` }))
    : (() => { const a = []; for (let f = Math.round(T0 * FPS); f < Math.round(T1 * FPS); f++) a.push({ t: f / FPS, name: `f${String(f).padStart(5, '0')}` }); return a; })();

  const tStart = Date.now();
  for (let i = 0; i < frameList.length; i++) {
    const { t, name } = frameList[i];
    await send('Runtime.evaluate', { expression: `window.seek(${t.toFixed(5)})` });
    const shot = await send('Page.captureScreenshot', { format: FORMAT, quality: QUALITY });
    if (!shot.result || !shot.result.data) { console.error('截图失败 @', t, JSON.stringify(shot).slice(0, 300)); process.exit(1); }
    fs.writeFileSync(path.join(OUT, `${name}.${ext}`), Buffer.from(shot.result.data, 'base64'));
    if (i % 100 === 0 || i === frameList.length - 1) {
      const rate = (i + 1) / ((Date.now() - tStart) / 1000);
      const etaMin = TIMES ? 0 : ((frameList.length - i - 1) / rate / 60);
      console.log(`[${i + 1}/${frameList.length}] t=${t.toFixed(2)}s 捕获率 ${rate.toFixed(1)} 帧/s eta ${etaMin.toFixed(1)} min`);
    }
  }
  console.log('DONE:', frameList.length, '帧,', ((Date.now() - tStart) / 1000 / 60).toFixed(1), 'min, 输出目录', OUT);
  try { ws.close(); } catch (e) {}
  edge.kill();
  process.exit(0);
}
main().catch(e => { console.error(e); process.exit(1); });
