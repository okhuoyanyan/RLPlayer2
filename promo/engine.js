'use strict';
// engine.js — RLPlayer 介绍短片动效引擎
// 核心约定：一切画面都是时间 t（秒）的纯函数，支持任意 seek，逐帧确定性渲染。
const W = 1920, H = 1080;
const PAL = {
  bg0: '#05070a', bg1: '#0a0e14', panel: '#101823', panel2: '#0d131d',
  line: '#1c2330', text: '#e6e9ef', muted: '#8a919e', dim: '#5a6270',
  blue: '#3b6ef6', purple: '#7b5cff', green: '#34d399', orange: '#f59e0b',
  red: '#ef4444', cyan: '#22d3ee', gold: '#eab308', white: '#ffffff',
};
const GRAD = (ctx, x0, y0, x1, y1) => {
  const g = ctx.createLinearGradient(x0, y0, x1, y1);
  g.addColorStop(0, PAL.blue); g.addColorStop(1, PAL.purple); return g;
};

// ---------- 缓动与工具 ----------
const clamp01 = x => x < 0 ? 0 : x > 1 ? 1 : x;
const clamp = (x, a, b) => x < a ? a : x > b ? b : x;
const lerp = (a, b, t) => a + (b - a) * t;
const smooth = x => { x = clamp01(x); return x * x * (3 - 2 * x); };
const eo3 = x => 1 - Math.pow(1 - clamp01(x), 3);
const ei3 = x => Math.pow(clamp01(x), 3);
const eio = x => { x = clamp01(x); return x < .5 ? 4 * x * x * x : 1 - Math.pow(-2 * x + 2, 3) / 2; };
const eob = x => { x = clamp01(x); const c1 = 1.70158, c3 = c1 + 1; return 1 + c3 * Math.pow(x - 1, 3) + c1 * Math.pow(x - 1, 2); };
// seg: t 在 [a,b] 内的归一化进度；segE 带缓动
const seg = (t, a, b) => clamp01((t - a) / (b - a));
const segE = (t, a, b, e = eio) => e(seg(t, a, b));
// 确定性哈希/随机（绘制期禁止 Math.random，只能用这两个）
const hash = i => { const s = Math.sin(i * 127.1 + 311.7) * 43758.5453; return s - Math.floor(s); };
function rng(seed) { let a = seed >>> 0; return () => { a |= 0; a = a + 0x6D2B79F5 | 0; let t = Math.imul(a ^ a >>> 15, 1 | a); t = t + Math.imul(t ^ t >>> 7, 61 | t) ^ t; return ((t ^ t >>> 14) >>> 0) / 4294967296; }; }

// ---------- 画笔助手 ----------
function font(ctx, w, s) { ctx.font = `${w} ${s}px "Segoe UI","Microsoft YaHei UI","Microsoft YaHei",sans-serif`; }
function alpha(ctx, a) { ctx.globalAlpha = clamp01(a); }
function rr(ctx, x, y, w, h, r) { ctx.beginPath(); ctx.roundRect(x, y, w, h, r); }
// 带字距的文字（Chromium 支持 ctx.letterSpacing）
function lsText(ctx, txt, x, y, spacing) {
  const old = ctx.letterSpacing; ctx.letterSpacing = spacing;
  ctx.fillText(txt, x, y); ctx.letterSpacing = old || '0px';
}
function textW(ctx, txt, spacing) {
  const old = ctx.letterSpacing; ctx.letterSpacing = spacing || '0px';
  const w = ctx.measureText(txt).width; ctx.letterSpacing = old || '0px'; return w;
}
// 药丸徽标：在 (x, y)（垂直中线）左起绘制，返回右边缘 x
function pill(ctx, x, y, txt, o = {}) {
  const fs = o.fs || 26, padX = o.padX || 20, h = o.h || fs + 22;
  const color = o.color || PAL.blue, fg = o.fg || '#fff';
  const dot = o.dot !== false;
  font(ctx, 600, fs);
  const tw = textW(ctx, txt, '0.5px');
  const w = tw + padX * 2 + (dot ? 14 : 0);
  const a = ctx.globalAlpha;
  ctx.fillStyle = 'rgba(255,255,255,0.03)'; rr(ctx, x, y - h / 2, w, h, h / 2); ctx.fill();
  ctx.strokeStyle = color + '88'; ctx.lineWidth = 1.5; rr(ctx, x, y - h / 2, w, h, h / 2); ctx.stroke();
  if (dot) { ctx.fillStyle = color; ctx.beginPath(); ctx.arc(x + padX + 4, y, 5, 0, 7); ctx.fill(); }
  ctx.fillStyle = fg; ctx.fillText(txt, x + padX + (dot ? 16 : 0), y + fs * 0.36);
  ctx.globalAlpha = a;
  return x + w;
}
// 背景与氛围
function bgGrad(ctx, c0 = PAL.bg0, c1 = PAL.bg1) {
  const g = ctx.createLinearGradient(0, 0, 0, H); g.addColorStop(0, c0); g.addColorStop(1, c1);
  ctx.fillStyle = g; ctx.fillRect(0, 0, W, H);
}
function bgGlow(ctx, x, y, r, color, a = 0.14) {
  const g = ctx.createRadialGradient(x, y, 0, x, y, r);
  g.addColorStop(0, color + Math.round(a * 255).toString(16).padStart(2, '0'));
  g.addColorStop(1, color + '00');
  ctx.fillStyle = g; ctx.fillRect(x - r, y - r, r * 2, r * 2);
}
function stars(ctx, t, n, spd, a = 0.5, seed = 7) {
  ctx.save();
  for (let i = 0; i < n; i++) {
    const hx = hash(i * 2.3 + seed), hy = hash(i * 5.7 + seed * 3);
    const x = ((hx * W + t * spd * (0.3 + hash(i) * 0.7)) % (W + 40)) - 20;
    const y = hy * H;
    const tw = 0.35 + 0.65 * (0.5 + 0.5 * Math.sin(t * (0.5 + hash(i + 99) * 1.5) + i));
    ctx.globalAlpha = a * tw * (0.3 + hash(i + 7) * 0.7);
    ctx.fillStyle = i % 9 === 0 ? PAL.blue : i % 13 === 0 ? PAL.purple : '#c9d4e8';
    const s = 0.8 + hash(i + 31) * 1.7;
    ctx.fillRect(x, y, s, s);
  }
  ctx.restore();
}
function vignette(ctx, a = 0.5) {
  const g = ctx.createRadialGradient(W / 2, H / 2, H * 0.42, W / 2, H / 2, H * 1.02);
  g.addColorStop(0, 'rgba(0,0,0,0)'); g.addColorStop(1, `rgba(0,0,0,${a})`);
  ctx.fillStyle = g; ctx.fillRect(0, 0, W, H);
}
// 细网格底纹（低透明度）
function grid(ctx, a = 0.05) {
  ctx.save(); ctx.strokeStyle = `rgba(120,150,220,${a})`; ctx.lineWidth = 1;
  for (let x = 0; x <= W; x += 96) { ctx.beginPath(); ctx.moveTo(x, 0); ctx.lineTo(x, H); ctx.stroke(); }
  for (let y = 0; y <= H; y += 96) { ctx.beginPath(); ctx.moveTo(0, y); ctx.lineTo(W, y); ctx.stroke(); }
  ctx.restore();
}

// ---------- 标准字卡（各场景统一风格） ----------
// 动画节拍：kicker [0.15,0.7]，标题 [0.3,1.0]，副标 [0.75,1.5]
function caption(ctx, t, o) {
  const x = o.x ?? 140, y = o.y ?? 170;
  if (o.kicker) {
    const p = segE(t, 0.15, 0.75, eo3);
    if (p > 0.001) {
      ctx.save(); alpha(ctx, p);
      ctx.translate(0, (1 - p) * 18);
      font(ctx, 600, 26); ctx.fillStyle = o.kickerColor || PAL.blue;
      lsText(ctx, o.kicker, x, y, '7px');
      // 前置短横线
      ctx.fillStyle = o.kickerColor || PAL.blue;
      ctx.fillRect(x - 44, y - 10, 28, 3);
      ctx.restore();
    }
  }
  if (o.title) {
    const p = segE(t, 0.3, 1.05, eo3);
    if (p > 0.001) {
      ctx.save(); alpha(ctx, p); ctx.translate(0, (1 - p) * 34);
      font(ctx, 800, o.titleSize || 88); ctx.fillStyle = '#f2f5fb';
      ctx.shadowColor = 'rgba(59,110,246,0.35)'; ctx.shadowBlur = 24;
      ctx.fillText(o.title, x, y + (o.titleSize || 88) * 0.95);
      ctx.restore();
    }
  }
  if (o.sub) {
    const p = segE(t, 0.75, 1.55, eo3);
    if (p > 0.001) {
      ctx.save(); alpha(ctx, p); ctx.translate(0, (1 - p) * 22);
      font(ctx, 500, 30); ctx.fillStyle = PAL.muted;
      ctx.fillText(o.sub, x, y + (o.titleSize || 88) * 0.95 + 56);
      ctx.restore();
    }
  }
}
// 打字机：返回 { txt 当前子串, done 是否完成 }
function typeSlice(str, t, startT, cps = 16) {
  const total = str.length / cps;
  const p = clamp01((t - startT) / total);
  return { txt: str.slice(0, Math.ceil(p * str.length)), done: p >= 1 };
}
// 发光连线（节点间数据流动画的基础件）
function flowLine(ctx, x0, y0, x1, y1, color, a = 0.5, wdt = 2) {
  ctx.save(); alpha(ctx, a);
  ctx.strokeStyle = color; ctx.lineWidth = wdt;
  ctx.shadowColor = color; ctx.shadowBlur = 10;
  ctx.beginPath(); ctx.moveTo(x0, y0); ctx.lineTo(x1, y1); ctx.stroke();
  ctx.restore();
}
function glowDot(ctx, x, y, r, color, a = 1) {
  ctx.save(); alpha(ctx, a);
  ctx.fillStyle = color; ctx.shadowColor = color; ctx.shadowBlur = 14;
  ctx.beginPath(); ctx.arc(x, y, r, 0, 7); ctx.fill();
  ctx.restore();
}

// ---------- 场景注册与时间轴 ----------
const SCENES = [];
function scene(dur, draw) { SCENES.push({ dur, draw }); }
let STARTS = [];
function buildTimeline() {
  STARTS = []; let acc = 0;
  for (const s of SCENES) { STARTS.push(acc); acc += s.dur; }
  return acc; // 总时长
}
const CF = 0.8; // 场景间交叉溶解秒数
function render(t) {
  const total = STARTS.length ? STARTS[STARTS.length - 1] + SCENES[SCENES.length - 1].dur : 0;
  t = clamp(t, 0, Math.max(0, total - 0.001));
  ctx.fillStyle = '#000'; ctx.fillRect(0, 0, W, H);
  let i = SCENES.length - 1;
  while (i > 0 && t < STARTS[i]) i--;
  const tl = t - STARTS[i];
  SCENES[i].draw(ctx, tl);
  // 与下一场景交叉溶解
  if (i + 1 < SCENES.length) {
    const nextStart = STARTS[i + 1];
    if (t >= nextStart - CF) {
      const p = smooth((t - (nextStart - CF)) / CF);
      ctx.save(); ctx.globalAlpha = p;
      SCENES[i + 1].draw(ctx, Math.max(0, t - nextStart));
      ctx.restore();
    }
  }
  watermark(ctx, t);
  vignette(ctx, 0.42);
}
// 常驻水印（片头结束后出现）
function watermark(ctx, t) {
  const a = segE(t, 10.5, 12, eio) * 0.85;
  if (a <= 0.01) return;
  ctx.save(); alpha(ctx, a);
  const x = W - 236, y = H - 44;
  const g = GRAD(ctx, x, y - 14, x + 28, y + 14);
  ctx.fillStyle = g; rr(ctx, x, y - 14, 28, 28, 8); ctx.fill();
  font(ctx, 800, 18); ctx.fillStyle = '#fff'; ctx.fillText('R', x + 9.5, y + 6);
  font(ctx, 600, 20); ctx.fillStyle = PAL.muted; lsText(ctx, 'RLPlayer', x + 40, y + 7, '1px');
  ctx.restore();
}
// 数字滚动
function countUp(t, a, b, t0, t1, fmt = v => Math.round(v).toString()) {
  const p = eio(seg(t, t0, t1));
  return fmt(lerp(a, b, p));
}

// ---------- 启动 ----------
const cv = document.getElementById('cv');
const ctx = cv.getContext('2d', { alpha: false });
ctx.textBaseline = 'alphabetic';
window.seek = t => render(t);
window.play = (from = 0, speed = 1) => {
  const t0 = performance.now(); let last = from;
  const loop = () => {
    const t = from + (performance.now() - t0) / 1000 * speed;
    if (t - last > 0.03) { render(t); last = t; }
    requestAnimationFrame(loop);
  }; loop();
};
