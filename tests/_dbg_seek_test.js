
const { spawn } = require('child_process');
const fs = require('fs');
const path = require('path');
const EXE = 'D:/RTA-hqplayer/build-streamdsd/rtaweb.exe';
const FILE = 'D:/RTA-hqplayer/build/testdata/user_ginshoku.dsf';
const PORT = 18899, BASE = 'http://127.0.0.1:' + PORT;
const sleep = ms => new Promise(r => setTimeout(r, ms));
const st = async () => { try { return await (await fetch(BASE + '/api/status')).json(); } catch (e) { return null; } };
(async () => {
  const child = spawn(EXE, ['--port', String(PORT)], { stdio: 'ignore' });
  for (let i = 0; i < 120; i++) { const s = await st(); if (s) break; await sleep(250); }
  await fetch(BASE + '/api/play', { method: 'POST', headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
    body: 'file=' + encodeURIComponent(FILE) + '&backend=null&rate=96000&volume=-60' });
  await sleep(2500);
  const out = {};
  const snap = async (tag) => { const s = await st(); out[tag] = { pos: +s.pos.toFixed(2), ws: s.wsMb, peak: s.wsPeakMb, err: s.error || '' }; };
  await snap('t2s');
  // forward seek to 5:00
  await fetch(BASE + '/api/seek?pos=' + (300 / 384)); await sleep(1800); await snap('after_seek_300s');
  await sleep(900); await snap('inc_after_300s');
  // backward seek to 0:30
  await fetch(BASE + '/api/seek?pos=' + (30 / 384)); await sleep(1800); await snap('after_back_seek_30s');
  await sleep(900); await snap('inc_after_30s');
  // pause / resume at 0:30
  await fetch(BASE + '/api/pause?on=1'); await sleep(500); const a = await st(); await sleep(1500); const b = await st();
  out.pause = { d1: +a.pos.toFixed(3), d2: +b.pos.toFixed(3), delta: +(b.pos - a.pos).toFixed(3), paused: b.paused };
  await fetch(BASE + '/api/pause?on=0'); await sleep(1500); const c = await st();
  out.resume = { pos: +c.pos.toFixed(3), delta: +(c.pos - b.pos).toFixed(3), paused: c.paused };
  const f = await st(); out.final = { peak: f.wsPeakMb, ws: f.wsMb, playing: f.playing, err: f.error || '' };
  try { await fetch(BASE + '/api/quit'); } catch (e) {}
  await sleep(1000); try { child.kill(); } catch (e) {}
  console.log(JSON.stringify(out, null, 1));
})();
