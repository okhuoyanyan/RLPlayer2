// test_web_naa_discover.js — /api/naa/discover 自动发现的正/负路径验证（只读端口探测，无声音）
//
// 用法：node tools/test_web_naa_discover.js [http://127.0.0.1:8923]
//
// 判据：①负路径 —— 本机 43210 无监听且局域网 NAA 未开启时，如实返回空列表 + 说明原因；
//       ②正路径 —— 在本机 43210 起监听，自动发现必须报出 127.0.0.1:43210；
//       ③代理 TUN 的 fake-IP 段（198.18.0.0/15）不能被当成端点（本机实测踩到 255 个假端点）；
//       ④30 秒内复用缓存，不重复扫全网段。
const net = require('net');
const BASE = process.argv[2] || 'http://127.0.0.1:8923';
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
let fails = 0;
function ck(cond, what, detail) {
  if (cond) console.log('  [PASS] ' + what);
  else { console.log('  [FAIL] ' + what + (detail !== undefined ? '  ' + JSON.stringify(detail) : '')); fails++; }
}
async function discover(force) {
  let j = await (await fetch(BASE + '/api/naa/discover' + (force ? '?refresh=1' : ''))).json();
  const t0 = Date.now();
  while (j && j.scanning && Date.now() - t0 < 30000) {
    await sleep(400);
    j = await (await fetch(BASE + '/api/naa/discover')).json();
  }
  return j;
}
(async () => {
  console.log('=== 1) 负路径：本机 43210 无监听 + 局域网 NAA 未开启 ===');
  let j = await discover(true);
  ck(j && j.ok === true && j.scanning === false, '扫描完成', j && { ok: j.ok, scanning: j.scanning });
  const eps = (j && j.endpoints) || [];
  ck(eps.length === 0, '未发现 NAA 端点（如实返回空列表，不编造）', eps.slice(0, 5));
  ck(typeof j.note === 'string' && j.note.indexOf('未发现 NAA 端点') === 0, 'note 明确写"未发现 NAA 端点"', j && j.note);
  ck(typeof j.note === 'string' && j.note.indexOf('UPnP') >= 0, 'note 说明服务互斥（开 UPnP 会关掉 NAA）', j && j.note);
  ck(Array.isArray(j.subnets) && j.subnets.length >= 1, '返回扫描过的网段列表', j && j.subnets);
  ck(j.probed > 250, '探测主机数 > 250（整段 /24）', j && j.probed);

  console.log('\n=== 2) 正路径：本机 43210 起监听 → 自动发现 127.0.0.1 ===');
  const srv = net.createServer((s) => s.end());
  await new Promise((res) => srv.listen(43210, '127.0.0.1', res));
  j = await discover(true);
  const eps2 = (j && j.endpoints) || [];
  ck(eps2.some((e) => e.host === '127.0.0.1' && e.port === 43210), '自动发现本机端点 127.0.0.1:43210', eps2.slice(0, 5));
  ck(typeof j.note === 'string' && j.note.indexOf('发现') === 0, 'note 报告"发现 N 个开放 TCP 43210 的主机"', j && j.note);
  ck(eps2.every((e) => e.port === 43210), '所有端点端口均为 43210', eps2.slice(0, 5));
  ck(!eps2.some((e) => String(e.host).indexOf('198.18.') === 0 || String(e.host).indexOf('198.19.') === 0),
     '代理 TUN fake-IP 段（198.18/15）不会被当成端点', eps2.slice(0, 5));
  await new Promise((res) => srv.close(res));

  console.log('\n=== 3) 缓存：30 秒内不重复扫描 ===');
  const t1 = Date.now();
  const j2 = await (await fetch(BASE + '/api/naa/discover')).json();
  const dt = Date.now() - t1;
  ck(j2.scanning === false && dt < 1000, '直接复用缓存结果（' + dt + 'ms）', { scanning: j2.scanning, dt });

  console.log('\n=== 结果: ' + (fails ? ('FAIL (' + fails + ')') : 'PASS') + ' ===');
  process.exit(fails ? 1 : 0);
})().catch((e) => { console.error('EXCEPTION', e); process.exit(2); });
