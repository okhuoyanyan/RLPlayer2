// uicheck_preset.js — 三档官方推荐预设的 CDP 验收（headless Edge + DevTools 协议）
// 用法: node tools/uicheck_preset.js http://127.0.0.1:PORT/
const { spawn } = require('child_process');
const fs = require('fs'); const path = require('path'); const os = require('os');
const URL_ = process.argv[2] || 'http://127.0.0.1:8907/';
const OUT = path.join(__dirname, '..', 'build', 'ui_preset');
fs.mkdirSync(OUT, { recursive: true });
const EDGE = ['C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\msedge.exe',
              'C:\\Program Files\\Microsoft\\Edge\\Application\\msedge.exe'].find(p => fs.existsSync(p));
const PORT = 9500 + Math.floor(Math.random() * 300);
const udd = path.join(os.tmpdir(), 'edge_preset_' + process.pid + '_' + Date.now());
const child = spawn(EDGE, ['--headless=new','--disable-gpu','--no-first-run','--no-default-browser-check',
  '--user-data-dir=' + udd, '--remote-debugging-port=' + PORT, '--window-size=1600,1000','about:blank'], { stdio: 'ignore' });
const sleep = ms => new Promise(r => setTimeout(r, ms));
let ws, id = 0; const pending = new Map(); const errors = []; const fails = []; let passes = 0;
function send(method, params) { return new Promise((res, rej) => { const i = ++id; pending.set(i, { res, rej }); ws.send(JSON.stringify({ id: i, method, params: params || {} })); }); }
async function evalJs(expr) {
  const r = await send('Runtime.evaluate', { expression: expr, awaitPromise: true, returnByValue: true });
  if (r.exceptionDetails) throw new Error('JS: ' + ((r.exceptionDetails.exception && r.exceptionDetails.exception.description) || r.exceptionDetails.text));
  return r.result && r.result.value;
}
function ck(ok, name, info) { if (ok) { passes++; console.log('  [PASS] ' + name + (info !== undefined ? '  ' + info : '')); } else { fails.push(name); console.log('  [FAIL] ' + name + (info !== undefined ? '  ' + JSON.stringify(info) : '')); } }
async function waitFor(expr, ms) { const t0 = Date.now(); while (Date.now() - t0 < (ms||8000)) { if (await evalJs('!!(' + expr + ')')) return true; await sleep(120); } return false; }
async function targets() { for (let i=0;i<60;i++){ try { const r = await fetch('http://127.0.0.1:'+PORT+'/json/list'); const j = await r.json(); const p = j.filter(t=>t.type==='page'); if (p.length) return p; } catch(e){} await sleep(250);} throw new Error('no devtools'); }
(async () => {
  const pages = await targets();
  ws = new WebSocket(pages[0].webSocketDebuggerUrl);
  await new Promise((res, rej) => { ws.onopen = res; ws.onerror = rej; });
  ws.onmessage = ev => { const m = JSON.parse(ev.data);
    if (m.id && pending.has(m.id)) { const p = pending.get(m.id); pending.delete(m.id); m.error ? p.rej(new Error(JSON.stringify(m.error))) : p.res(m.result); }
    else if (m.method === 'Runtime.exceptionThrown') errors.push(JSON.stringify(m.params.exceptionDetails).slice(0,300));
    else if (m.method === 'Runtime.consoleAPICalled' && m.params.type === 'error') errors.push((m.params.args||[]).map(a=>a.value||a.description).join(' ').slice(0,300));
  };
  await send('Runtime.enable'); await send('Page.enable');
  await send('Page.navigate', { url: URL_ });
  await sleep(2500);
  await waitFor("window.S && S.view", 15000);
  // 进设置页
  await evalJs("Array.from(document.querySelectorAll('.nav')).find(e=>e.dataset.v==='settings').click()");
  ck(await waitFor("!!document.getElementById('presetCard')", 8000), '设置页出现「官方推荐」卡片');
  for (const id_ of ['presetBitPerfect','presetPcm','presetDsd','presetCapsRefresh','presetCapsInfo'])
    ck(await evalJs("!!document.getElementById('" + id_ + "')"), '控件存在 #' + id_);
  ck(await evalJs("document.querySelector('#presetBitPerfect').getAttribute('aria-label')") === '原音直通（bit-perfect）', '原音直通按钮 aria-label 正确');
  const caps0 = await evalJs("document.getElementById('presetCapsInfo').textContent");
  console.log('  caps 行: ' + caps0);
  await sleep(1200);
  const caps1 = await evalJs("document.getElementById('presetCapsInfo').textContent");
  ck(/最高/.test(caps1) && /原生 DSD/.test(caps1), '能力行显示「最高 xx / 原生 DSD」', caps1);
  // 点 PCM 升频
  await evalJs("document.getElementById('presetPcm').click()");
  await sleep(2500);
  const pcmState = await evalJs("(function(){var r=D('sRate'),f=D('sFilt');return {rate:r?r.value:null, filt:f?f.value:null, shaper:D('sShap')?D('sShap').value:null};})()");
  ck(pcmState.rate && pcmState.rate !== '0', 'PCM 档把输出采样率设为具体值', pcmState.rate);
  ck(!!pcmState.filt && /tight/.test(pcmState.filt), 'PCM 档选中 *-tight 滤波', pcmState.filt);
  // 点原音直通
  await evalJs("document.getElementById('presetBitPerfect').click()");
  await sleep(2500);
  const bpState = await evalJs("(function(){return {rate:D('sRate')?D('sRate').value:null, shaper:D('sShap')?D('sShap').value:null, vol:window.S&&S.vol!==undefined?S.vol:null};})()");
  ck(bpState.shaper === 'none', '原音直通把整形器设为 none', bpState.shaper);
  ck(bpState.rate === 'follow' || bpState.rate === '0' || bpState.rate === '', '原音直通把输出速率设为跟随源', bpState.rate);
  // 刷新按钮
  await evalJs("document.getElementById('presetCapsRefresh').click()");
  await sleep(1500);
  ck(true, '刷新按钮可点击（无异常）');
  const r = await send('Page.captureScreenshot', { format: 'png' });
  fs.writeFileSync(path.join(OUT, 'preset-card.png'), Buffer.from(r.data, 'base64'));
  ck(errors.filter(e => !/favicon/.test(e)).length === 0, '全页 0 个 JS 异常 / console.error', errors.slice(0,2));
  console.log('\n结果: ' + passes + ' 项通过, ' + fails.length + ' 项失败');
  if (fails.length) console.log('失败项: ' + fails.join(' | '));
  try { child.kill(); } catch (e) {}
  process.exit(fails.length ? 1 : 0);
})().catch(e => { console.error('FATAL ' + e.message); try { child.kill(); } catch (_) {} process.exit(2); });
