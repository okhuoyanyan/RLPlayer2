'use strict';
// scenes/s1.js — 曲库场景（18s）
// 节拍：0.3 封面墙透视飞入并缓慢左移漂移 → 2.5 中央封面+播放按钮 → 5.0 格式徽标交错弹出
//       → 8.0 搜索演示（打字机/结果条目/毫秒级搜索角标）→ 12 封面波浪涟漪+特性徽标 → 17 后构图稳定
// 确定性：画面是 t 的纯函数；随机只来自引擎 hash()/rng()，封面参数在加载期一次生成存常量。

const S1_ROWS = 4, S1_COLS = 6, S1_N = S1_ROWS * S1_COLS;
// 封面首字母池（刻意避开个别字母，防止任何拼读出品牌字样的可能）
const S1_LETTERS = 'NOCUMVKLPDGBFHJIWYSZ';
// 加载期一次性生成 24 张生成式封面（色相/构图/首字母），draw 内只读
const S1_ART = [];
{
  const r = rng(20261026);
  for (let i = 0; i < S1_N; i++) {
    S1_ART.push({
      h0: Math.floor(r() * 360),
      h1: (Math.floor(r() * 360) + 40 + Math.floor(r() * 120)) % 360,
      kind: r() < 0.55 ? 0 : 1, // 0=大字首字母 1=抽象色块
      letter: S1_LETTERS[Math.floor(r() * S1_LETTERS.length)],
      b: [r(), r(), r(), r(), r()],
    });
  }
}

// 生成式封面：渐变底 + 首字母/抽象色块，(x,y) 为左上角
function s1Cover(ctx, i, x, y, size, a) {
  const art = S1_ART[i], rad = Math.max(4, size * 0.07);
  ctx.save(); alpha(ctx, a);
  const g = ctx.createLinearGradient(x, y, x + size, y + size);
  g.addColorStop(0, `hsl(${art.h0},52%,28%)`);
  g.addColorStop(1, `hsl(${art.h1},60%,12%)`);
  rr(ctx, x, y, size, size, rad); ctx.fillStyle = g; ctx.fill();
  if (art.kind === 0) {
    font(ctx, 800, size * 0.46); ctx.fillStyle = 'rgba(255,255,255,0.92)';
    ctx.textAlign = 'center';
    ctx.fillText(art.letter, x + size / 2, y + size * 0.66);
    ctx.textAlign = 'left';
  } else {
    ctx.fillStyle = `hsla(${art.h1},72%,62%,0.5)`;
    ctx.fillRect(x + size * (0.12 + art.b[0] * 0.3), y + size * (0.12 + art.b[1] * 0.3), size * 0.42, size * 0.42);
    ctx.fillStyle = `hsla(${art.h0},70%,72%,0.35)`;
    ctx.beginPath();
    ctx.arc(x + size * (0.3 + art.b[2] * 0.4), y + size * (0.3 + art.b[3] * 0.4), size * (0.1 + art.b[4] * 0.12), 0, 7);
    ctx.fill();
  }
  ctx.restore();
  ctx.save(); alpha(ctx, a * 0.8);
  ctx.strokeStyle = 'rgba(255,255,255,0.09)'; ctx.lineWidth = 1;
  rr(ctx, x, y, size, size, rad); ctx.stroke();
  ctx.restore();
}

scene(18, (ctx, t) => {
  bgGrad(ctx);
  stars(ctx, t, 80, 9, 0.28);
  bgGlow(ctx, W * 0.62, H * 0.62, 780, PAL.blue, 0.10);

  // ---- 4×6 封面墙：简易透视投影（远小近大），飞入后缓慢左移 ----
  const cam = lerp(4200, 1100, segE(t, 0.3, 2.5, eo3));
  const drift = -70 * seg(t, 2.2, 18);
  const wallDim = 1 - 0.18 * seg(t, 2.6, 3.4);
  const rip = Math.sin(Math.PI * seg(t, 12, 17)); // 涟漪包络：起止归零
  for (let r = S1_ROWS - 1; r >= 0; r--) { // 远排先画
    for (let c = 0; c < S1_COLS; c++) {
      const i = r * S1_COLS + c;
      const delay = (c * 0.7 + r * 0.45) * 0.14;
      const ap = segE(t, 0.3 + delay, 1.5 + delay, eo3);
      if (ap <= 0.001) continue;
      const s = 1000 / (r * 520 + 700 + cam);
      const yo = Math.sin(t * 2.4 - c * 0.6) * 26 * rip // 涟漪：相位按列退
        + Math.sin(t * 0.9 + c * 0.5 + r * 0.8) * 4;    // 常驻微呼吸
      const size = 380 * s * (0.75 + 0.25 * eob(seg(t, 0.3 + delay, 1.9 + delay)));
      const sx = W / 2 + ((c - 2.5) * 420 + drift) * s;
      const sy = 700 + (r - 1.15) * 400 * s + yo;
      s1Cover(ctx, i, sx - size / 2, sy - size / 2, size, ap * 0.95 * wallDim);
    }
  }

  // ---- 中央封面 + 圆形播放按钮 + 脉冲光圈 ----
  const cz = segE(t, 2.5, 3.3, eob);
  if (cz > 0.001) {
    const size = lerp(230, 330, cz), cx0 = 960 - size / 2, cy0 = 620 - size / 2;
    ctx.save(); alpha(ctx, Math.min(1, cz * 1.4));
    ctx.shadowColor = 'rgba(0,0,0,0.6)'; ctx.shadowBlur = 44; ctx.shadowOffsetY = 16;
    ctx.fillStyle = '#0a0e14'; rr(ctx, cx0 - 2, cy0 - 2, size + 4, size + 4, 22); ctx.fill();
    ctx.restore();
    s1Cover(ctx, 2, cx0, cy0, size, Math.min(1, cz * 1.4));
    const kk = (t * 0.7) % 1;
    ctx.save(); alpha(ctx, (1 - kk) * 0.5 * cz);
    ctx.strokeStyle = PAL.blue; ctx.lineWidth = 2.5;
    ctx.beginPath(); ctx.arc(960, 620, 54 + 34 * kk, 0, 7); ctx.stroke();
    ctx.restore();
    ctx.save(); alpha(ctx, cz);
    ctx.fillStyle = 'rgba(8,12,18,0.78)';
    ctx.beginPath(); ctx.arc(960, 620, 52, 0, 7); ctx.fill();
    ctx.strokeStyle = 'rgba(255,255,255,0.25)'; ctx.lineWidth = 2; ctx.stroke();
    ctx.fillStyle = '#fff';
    ctx.beginPath();
    ctx.moveTo(948, 601); ctx.lineTo(982, 620); ctx.lineTo(948, 639);
    ctx.closePath(); ctx.fill();
    ctx.restore();
  }

  // ---- 格式徽标（底部一排，交错 0.15s 弹出）----
  const FMT = ['FLAC', 'DSD', 'WAV', 'APE', 'ALAC', 'WV', 'OGG', 'Opus', 'CUE'];
  let fx = 140;
  for (let k = 0; k < FMT.length; k++) {
    const p = segE(t, 5.0 + k * 0.15, 5.55 + k * 0.15, eob);
    ctx.save(); alpha(ctx, p); ctx.translate(0, (1 - p) * 26);
    fx += pill(ctx, fx, 950, FMT[k], { fs: 23, color: k % 2 ? PAL.purple : PAL.blue }) + 13;
    ctx.restore();
  }

  // ---- 特性徽标（涟漪期出现，常驻到结尾）----
  const FEATS = [['艺术家 · 专辑 · 古典作品', PAL.purple], ['CUE 整轨自动分轨', PAL.green]];
  let fpx = 140;
  for (let k = 0; k < FEATS.length; k++) {
    const p = segE(t, 12.0 + k * 0.18, 12.6 + k * 0.18, eob);
    ctx.save(); alpha(ctx, p); ctx.translate(0, (1 - p) * 24);
    fpx += pill(ctx, fpx, 1024, FEATS[k][0], { fs: 23, color: FEATS[k][1] }) + 14;
    ctx.restore();
  }

  // ---- 搜索演示：搜索框 + 打字机 + 3 行结果 + 角标 ----
  const sp = segE(t, 8.0, 8.6, eo3);
  if (sp > 0.001) {
    const bx = 140, by = lerp(432, 408, sp), bw = 560, bh = 58;
    ctx.save(); alpha(ctx, sp);
    ctx.fillStyle = 'rgba(13,19,29,0.94)'; rr(ctx, bx, by, bw, bh, 14); ctx.fill();
    ctx.strokeStyle = PAL.line; ctx.lineWidth = 1.5; rr(ctx, bx, by, bw, bh, 14); ctx.stroke();
    ctx.strokeStyle = PAL.muted; ctx.lineWidth = 2.2;
    ctx.beginPath(); ctx.arc(bx + 32, by + bh / 2 - 4, 9, 0, 7); ctx.stroke();
    ctx.beginPath(); ctx.moveTo(bx + 39, by + bh / 2 + 3); ctx.lineTo(bx + 45, by + bh / 2 + 9); ctx.stroke();
    const typed = typeSlice('深夜 粤语', t, 8.5, 5);
    font(ctx, 500, 26);
    let caretX = bx + 62;
    if (typed.txt) { ctx.fillStyle = PAL.text; ctx.fillText(typed.txt, bx + 62, by + 37); caretX += textW(ctx, typed.txt); }
    else if (t < 8.5) { font(ctx, 400, 24); ctx.fillStyle = PAL.dim; ctx.fillText('搜索曲库…', bx + 62, by + 36); }
    if (!typed.done && Math.sin(t * 8) > 0) { ctx.fillStyle = PAL.blue; ctx.fillRect(caretX + 5, by + 13, 2.5, 32); }
    // 结果条目（虚构曲名）
    const ROWS = [['夜色钢琴曲 · 现场版', '4:12', 12], ['维港之夜 · 深夜电台', '3:58', 47], ['月台下的告别 · 粤语', '5:07', 83]];
    for (let k = 0; k < 3; k++) {
      const p = segE(t, 9.5 + k * 0.22, 9.95 + k * 0.22, eo3);
      if (p <= 0.001) continue;
      const ry = 478 + k * 58 + (1 - p) * 14;
      ctx.save(); alpha(ctx, p);
      ctx.fillStyle = 'rgba(255,255,255,0.035)'; rr(ctx, bx, ry, bw, 50, 12); ctx.fill();
      ctx.strokeStyle = PAL.line; ctx.lineWidth = 1; rr(ctx, bx, ry, bw, 50, 12); ctx.stroke();
      ctx.fillStyle = `hsl(${ROWS[k][2]},55%,32%)`; rr(ctx, bx + 12, ry + 8, 34, 34, 8); ctx.fill();
      font(ctx, 500, 24); ctx.fillStyle = PAL.text; ctx.fillText(ROWS[k][0], bx + 62, ry + 32);
      font(ctx, 400, 20); ctx.fillStyle = PAL.muted; ctx.fillText(ROWS[k][1], bx + bw - 64, ry + 31);
      ctx.restore();
    }
    const gp = segE(t, 10.7, 11.25, eob);
    if (gp > 0.001) {
      ctx.save(); alpha(ctx, gp); ctx.translate(0, (1 - gp) * -20);
      pill(ctx, bx + 330, by, '毫秒级搜索', { fs: 22, color: PAL.green });
      ctx.restore();
    }
    ctx.restore();
  }

  caption(ctx, t, {
    kicker: 'YOUR LIBRARY',
    title: '你的整个曲库，一目了然',
    sub: 'FLAC · DSD · WAV · APE · ALAC · WV · OGG · Opus',
  });
});
