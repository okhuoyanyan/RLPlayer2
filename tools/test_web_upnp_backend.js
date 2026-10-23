// test_web_upnp_backend.js — 「输出后端 = UPnP / DLNA」路由的无声端到端验证
//
// 用法：node tools/test_web_upnp_backend.js [http://127.0.0.1:8923]
//
// 判据：后端 upnp 时 /api/play、/api/skip 转交 CastManager；/api/status 与投放会话同一口径
// （进度/时长/格式/暂停）；/api/pause、/api/seek、/api/stop 映射到 AVTransport；本地播放会先停
// 投放（一条会话）；未选渲染器时如实报错；upnpdev 设置能落盘。
//
// 为什么需要它：UPnP 后端一旦接错，要么不出声、要么本机声卡与网络渲染器同时出声。
// 全靠真机测不现实 —— 所以自起一个**本地假渲染器**（tools/mock_upnp_renderer.js），
// 全程不碰真实设备、不打开任何音频设备、不出声。真实设备联调由 Lead 在用户授权下做。
const { spawn } = require('child_process');
const fsn = require('fs');
const osn = require('os');
const pathn = require('path');
const BASE = process.argv[2] || 'http://127.0.0.1:8923';
const MOCK_PORT = parseInt(process.env.MOCK_PORT || '49199', 10);
const MOCK = 'http://127.0.0.1:' + MOCK_PORT;
const MOCK_DEV = MOCK + '/description.xml';   // 用设备描述 URL（与 SSDP 发现结果里的 location 一致）
const mockProc = spawn(process.execPath, [pathn.join(__dirname, 'mock_upnp_renderer.js'), String(MOCK_PORT)], { stdio: 'ignore' });
// 自造 2 秒静音 WAV（44.1 kHz / 16 bit / 2ch）：测试不依赖仓库里的任何素材
const FILE = pathn.join(osn.tmpdir(), 'rlplayer_cast_test_2s.wav');
(function makeWav() {
  const frames = 44100 * 2, ch = 2, bytes = frames * ch * 2;
  const buf = Buffer.alloc(44 + bytes);
  buf.write('RIFF', 0); buf.writeUInt32LE(36 + bytes, 4); buf.write('WAVE', 8);
  buf.write('fmt ', 12); buf.writeUInt32LE(16, 16); buf.writeUInt16LE(1, 20);
  buf.writeUInt16LE(ch, 22); buf.writeUInt32LE(44100, 24); buf.writeUInt32LE(44100 * ch * 2, 28);
  buf.writeUInt16LE(ch * 2, 32); buf.writeUInt16LE(16, 34);
  buf.write('data', 36); buf.writeUInt32LE(bytes, 40);
  fsn.writeFileSync(FILE, buf);
})();
function cleanup() {
  try { mockProc.kill(); } catch (e) { }
  try { fsn.unlinkSync(FILE); } catch (e) { }
}
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
let fails = 0;
function ck(cond, what, detail) {
  if (cond) console.log('  [PASS] ' + what);
  else { console.log('  [FAIL] ' + what + (detail !== undefined ? '  ' + JSON.stringify(detail) : '')); fails++; }
}
async function post(path, body) {
  const r = await fetch(BASE + path, { method: 'POST', body: body || '' });
  return r.json();
}
async function get(path) {
  const r = await fetch(BASE + path);
  return r.json();
}
async function mockState() { return (await fetch(MOCK + '/__state')).json(); }
async function waitFor(fn, ms) {
  const t0 = Date.now();
  let last = null;
  while (Date.now() - t0 < ms) {
    last = await fn();
    if (last && last.ok === true) return last;   // 只在条件真正满足时提前返回
    await sleep(150);
  }
  return last;
}
(async () => {
  // 等假渲染器起来（最多 10s）
  for (let i = 0; i < 50; i++) {
    try { if ((await fetch(MOCK + '/__state')).ok) break; } catch (e) { }
    await sleep(200);
  }
  console.log('=== 1) 前置：确保 backend=null、volume=-60（铁律：不发声） ===');
  let j = await post('/api/settings', 'backend=null&volume=-60');
  ck(j.ok === true, '设置写入成功', j);
  const st0 = await get('/api/cast/state');
  ck(st0.running === false, '起始无投放会话', st0);

  console.log('\n=== 2) 后端 = UPnP + 假渲染器 → /api/play 应转交 CastManager ===');
  const body = 'file=' + encodeURIComponent(FILE) + '&backend=upnp&upnpdev=' + encodeURIComponent(MOCK_DEV);
  j = await post('/api/play?x=1', body);
  ck(j.ok === true && j.upnp === true, '/api/play 返回 ok+upnp', j);
  const cs = await waitFor(async () => {
    const s = await get('/api/cast/state');
    // 等到 worker 真正走完发现 + 供源（phase=playing）再断言，避免抢跑
    return (s.running && s.phase === 'playing') ? { ok: true, s } : { ok: false, s };
  }, 10000, 'cast playing');
  ck(cs && cs.s && cs.s.running === true, '投放会话已建立', cs && cs.s);
  ck(cs && cs.s && cs.s.phase === 'playing', '投放会话进入 playing 相位', cs && cs.s && cs.s.phase);
  ck(cs && cs.s && cs.s.device === 'Mock-N1000-Renderer', '目标渲染器 = 假渲染器', cs && cs.s && cs.s.device);
  ck(cs && cs.s && Math.abs((cs.s.durationSec || 0) - 2.0) < 0.1, '投放会话时长 ≈ 2s', cs && cs.s && cs.s.durationSec);
  ck(cs && cs.s && cs.s.sampleRate === 44100 && cs.s.bits === 16, '供源格式 44100/16', cs && cs.s && [cs.s.sampleRate, cs.s.bits]);
  const ms1 = await mockState();
  ck(ms1.transport === 'PLAYING', '假渲染器收到 Play → PLAYING', ms1.transport);
  ck(ms1.calls.some((c) => c.action === 'SetAVTransportURI' && c.uri.indexOf('/stream/') > 0),
     'SetAVTransportURI 的 URI 指向本机供源 /stream/', ms1.calls.filter((c) => c.action === 'SetAVTransportURI'));

  console.log('\n=== 3) /api/status 统一到投放会话 ===');
  const st = await get('/api/status');
  ck(st.playing === true, 'status.playing = true', st.playing);
  ck(String(st.current).endsWith(FILE.split(/[\\/]/).pop()), 'status.current = 投放曲目', st.current);
  ck(st.outRate === 44100 && st.outBits === 16, 'status.outRate/outBits 来自投放供源', [st.outRate, st.outBits]);
  ck(Math.abs((st.dur || 0) - 2.0) < 0.1, 'status.dur ≈ 2s（不再恒为 0）', st.dur);

  console.log('\n=== 4) 暂停 / 继续（映射到 AVTransport Pause/Play） ===');
  await post('/api/pause?on=1', '');
  await sleep(1600);
  const mp = await mockState();
  ck(mp.transport === 'PAUSED_PLAYBACK', '假渲染器进入 PAUSED_PLAYBACK', mp.transport);
  const stP = await get('/api/status');
  ck(stP.paused === true, 'status.paused = true', stP.paused);
  await post('/api/pause?on=0', '');
  await sleep(1600);
  const mr = await mockState();
  ck(mr.transport === 'PLAYING', '继续后渲染器回到 PLAYING', mr.transport);
  ck((await get('/api/status')).paused === false, 'status.paused 回到 false');

  console.log('\n=== 5) 拖动进度（映射到 AVTransport Seek REL_TIME） ===');
  await post('/api/seek?pos=0.5', '');
  await sleep(1600);
  const msk = await mockState();
  const seekCall = msk.calls.filter((c) => c.action === 'Seek').pop();
  ck(Math.abs((msk.positionSec || 0) - 1.0) < 0.3, '假渲染器侧 Seek 到 ≈1s（2s × 0.5）', msk.positionSec);
  ck(!!seekCall, 'Seek SOAP 已发出', seekCall && seekCall.action);
  const stS = await get('/api/status');
  ck(Math.abs((stS.pos || 0) - 1.0) < 0.3, 'status.pos ≈ 1s', stS.pos);

  console.log('\n=== 6) 切歌（/api/skip → 停旧会话 + 起新会话） ===');
  await post('/api/add', 'file=' + encodeURIComponent(FILE));
  const n0 = (await mockState()).calls.filter((c) => c.action === 'SetAVTransportURI').length;
  j = await post('/api/skip?dir=1', '');
  ck(j.ok === true, '/api/skip 在 UPnP 后端下成功', j);
  await sleep(2500);
  const msk2 = await mockState();
  const n1 = msk2.calls.filter((c) => c.action === 'SetAVTransportURI').length;
  ck(n1 > n0, '切歌重新下发了 SetAVTransportURI（新会话）', { n0, n1 });
  ck(msk2.transport === 'PLAYING', '切歌后渲染器仍在播放', msk2.transport);

  console.log('\n=== 7) 停止（/api/stop 同时结束投放会话） ===');
  await post('/api/stop', '');
  await sleep(1800);
  const mst = await mockState();
  ck(mst.transport === 'STOPPED', '假渲染器收到 Stop', mst.transport);
  const cst = await get('/api/cast/state');
  ck(cst.running === false, '/api/cast/state.running = false', cst);
  const stTop = await get('/api/status');
  ck(stTop.playing === false, '/api/status.playing = false', stTop.playing);

  console.log('\n=== 8) 失败必须如实报错（未选渲染器） ===');
  j = await post('/api/play?x=1', 'file=' + encodeURIComponent(FILE) + '&backend=upnp');
  ck(j.ok === false, '未选渲染器时 /api/play 返回 ok=false', j);
  ck(String(j.error || '').indexOf('渲染器') >= 0, '错误文案说明缺渲染器', j.error);

  console.log('\n=== 9) 本地播放会先停投放（一条会话原则） ===');
  await post('/api/play?x=1', 'file=' + encodeURIComponent(FILE) + '&backend=upnp&upnpdev=' + encodeURIComponent(MOCK_DEV));
  await sleep(2500);
  ck((await get('/api/cast/state')).running === true, '投放会话运行中');
  await post('/api/play?x=1', 'file=' + encodeURIComponent(FILE) + '&backend=null&volume=-60');
  await sleep(2200);
  ck((await get('/api/cast/state')).running === false, '切回本地播放时投放会话被停止（无双声源）');
  ck((await mockState()).transport === 'STOPPED', '假渲染器已停止');
  await post('/api/stop', '');

  console.log('\n=== 10) 设置持久化：upnpdev / naahost ===');
  await post('/api/settings', 'upnpdev=' + encodeURIComponent(MOCK_DEV) + '&naahost=' + encodeURIComponent('127.0.0.1:43210'));
  const kv = (await get('/api/settings')).kv || {};
  ck(kv.upnpdev === MOCK_DEV, 'kv.upnpdev 已落盘', kv.upnpdev);
  ck(kv.naahost === '127.0.0.1:43210', 'kv.naahost 仍在', kv.naahost);
  // 收尾：清掉测试用 upnpdev，避免影响后续 UI 截图
  await post('/api/settings', 'upnpdev=');

  // 收尾：绝不留下会出声的配置（铁律 1：测试默认不可听）
  await post('/api/settings', 'backend=null&volume=-60');
  cleanup();
  console.log('\n=== 结果: ' + (fails ? ('FAIL (' + fails + ')') : 'PASS') + ' ===');
  process.exit(fails ? 1 : 0);
})().catch((e) => { console.error('EXCEPTION', e); cleanup(); process.exit(2); });
