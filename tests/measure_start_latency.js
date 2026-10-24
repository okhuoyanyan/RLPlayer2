
const { spawn } = require('child_process');
const fs = require('fs');
const path = require('path');

const EXE = 'D:/RTA-hqplayer/build-streamdsd/rtaweb.exe';
const FILE = process.argv[2] || 'D:/RTA-hqplayer/build/testdata/user_ginshoku.dsf';
const EXTRA = process.argv[3] || 'backend=null&rate=96000&volume=-60';
const SEEKFRAC = process.argv[4] || '0.78125';
const PORT = 18899;
const BASE = 'http://127.0.0.1:' + PORT;
const logPath = path.join(path.dirname(EXE), 'rlplayer-playback.log');
const sleep = ms => new Promise(r => setTimeout(r, ms));

(async () => {
  try { fs.unlinkSync(logPath); } catch (e) {}
  const child = spawn(EXE, ['--port', String(PORT)], { stdio: 'ignore' });
  const out = { pid: child.pid };
  let ready = false;
  for (let i = 0; i < 120 && !ready; i++) {
    try { const r = await fetch(BASE + '/api/status'); if (r.ok) { out.ws0 = (await r.json()).wsMb; ready = true; } }
    catch (e) { await sleep(250); }
  }
  if (!ready) { console.log('SERVER NOT READY'); child.kill(); return; }

  await sleep(300);
  const t0 = Date.now();
  const body = 'file=' + encodeURIComponent(FILE) + '&' + EXTRA;
  const pr = await fetch(BASE + '/api/play', { method: 'POST', headers: { 'Content-Type': 'application/x-www-form-urlencoded' }, body });
  const pj = await pr.json();
  out.playOk = pj.ok; out.playErr = pj.error || '';

  const samples = [];
  let tFirst = null, peak = 0, lastPos = 0;
  const deadline = Date.now() + 20000;
  while (Date.now() < deadline) {
    let st = null;
    try { st = await (await fetch(BASE + '/api/status')).json(); } catch (e) {}
    if (st) {
      const t = Date.now() - t0;
      if (st.wsPeakMb > peak) peak = st.wsPeakMb;
      samples.push({ t, ws: st.wsMb, peak: st.wsPeakMb, pos: +st.pos, playing: st.playing, err: st.error || '' });
      if (tFirst === null && +st.pos > 0) tFirst = t;
      lastPos = +st.pos;
      if (tFirst !== null && t - tFirst > 4000) break;
      if (st.error) break;
    }
    await sleep(100);
  }
  out.tFirst = tFirst; out.peak = peak;

  // ---- seek to 5:00 (5*60/384 = 0.78125) ----
  const before = lastPos;
  await fetch(BASE + '/api/seek?pos=' + SEEKFRAC);
  await sleep(1200);
  let st1 = await (await fetch(BASE + '/api/status')).json();
  await sleep(1500);
  let st2 = await (await fetch(BASE + '/api/status')).json();
  out.seekFrom = before; out.seekPos1 = +st1.pos; out.seekPos2 = +st2.pos;
  out.seekErr = st1.error || '';

  // ---- pause / resume ----
  await fetch(BASE + '/api/pause?on=1');
  await sleep(600);
  const p1 = +(await (await fetch(BASE + '/api/status')).json()).pos;
  await sleep(1200);
  const p2 = +(await (await fetch(BASE + '/api/status')).json()).pos;
  await fetch(BASE + '/api/pause?on=0');
  await sleep(1500);
  const p3 = +(await (await fetch(BASE + '/api/status')).json()).pos;
  out.pausedDelta = +(p2 - p1).toFixed(3); out.resumeDelta = +(p3 - p2).toFixed(3);
  out.pauseFlag = (await (await fetch(BASE + '/api/status')).json()).paused;

  const final = await (await fetch(BASE + '/api/status')).json();
  out.finalPeak = final.wsPeakMb; out.outRate = final.outRate; out.signalInfo = final.info;

  try { await fetch(BASE + '/api/quit'); } catch (e) {}
  await sleep(1200);
  try { if (!child.killed) child.kill(); } catch (e) {}

  // log highlights
  let log = '';
  try { log = fs.readFileSync(logPath, 'utf8'); } catch (e) {}
  out.log = log.split(/\r?\n/).filter(l => /MEM |STREAM_|DECODE|PREFETCH_SKIP|PLAY |END |ISP|RENDERFAIL|NORENDER/.test(l)).slice(0, 40);
  out.timeline = samples.filter((s, i) => i % 5 === 0 || s.t < 1500).slice(0, 40);
  console.log(JSON.stringify(out, null, 1));
})();