'use strict';
// scenes/s7.js — 结尾：卖点级联 → 定版 logo → 黑场（17s）
scene(17, (ctx, t) => {
  bgGrad(ctx, '#04060a', '#080b12');
  stars(ctx, t, 150, 6, 0.4);
  bgGlow(ctx, W / 2, H * 0.45, 800, PAL.blue, 0.08 * (0.7 + 0.3 * Math.sin(t * 1.2)));

  // ---- 卖点 chips 级联（4×2）----
  const chips = ['位元直通', '升频引擎', 'GPU 加速', '原生 DSD', 'AI 整理', '20 段 EQ', '歌词跟随', 'UPnP 投放'];
  const CW = 386, CH = 84, GX = 26, GY = 22;
  const totalW = CW * 4 + GX * 3, x0 = (W - totalW) / 2, y0 = 300;
  chips.forEach((s, i) => {
    const col = i % 4, row = Math.floor(i / 4);
    const p = segE(t, 0.7 + i * 0.13, 1.5 + i * 0.13, eob);
    if (p <= 0.001) return;
    const x = x0 + col * (CW + GX), y = y0 + row * (CH + GY);
    ctx.save();
    ctx.translate(x + CW / 2, y + CH / 2);
    ctx.scale(lerp(0.65, 1, p), lerp(0.65, 1, p));
    alpha(ctx, clamp01(seg(t, 0.7 + i * 0.13, 1.2 + i * 0.13)) * (1 - segE(t, 13.8, 15.0, eio) * 0.55));
    ctx.fillStyle = 'rgba(255,255,255,0.035)';
    ctx.strokeStyle = PAL.line; ctx.lineWidth = 1.5;
    rr(ctx, -CW / 2, -CH / 2, CW, CH, 18); ctx.fill(); ctx.stroke();
    const g = GRAD(ctx, -18, -9, 0, 9);
    ctx.fillStyle = g;
    rr(ctx, -CW / 2 + 26, -9, 18, 18, 5); ctx.fill();
    font(ctx, 700, 32); ctx.fillStyle = '#e8edf7';
    ctx.fillText(s, -CW / 2 + 64, 11);
    ctx.restore();
  });

  // ---- 定版 logo 组（5.2–7.4s）----
  const lp = segE(t, 5.2, 6.6, eob);
  if (lp > 0.001) {
    const cy = 700, a = clamp01(seg(t, 5.2, 5.9)) * (1 - segE(t, 14.6, 15.8, eio));
    ctx.save();
    alpha(ctx, a);
    // 扩散光环
    const ringT = (t - 5.4) % 2.2;
    const rp2 = ringT / 2.2;
    if (t > 5.4 && rp2 < 1) {
      ctx.strokeStyle = `rgba(59,110,246,${0.35 * (1 - rp2)})`;
      ctx.lineWidth = 2;
      ctx.beginPath(); ctx.arc(W / 2, cy, 90 + rp2 * 150, 0, 7); ctx.stroke();
    }
    // R 徽标
    const S = lerp(120, 150, lp);
    ctx.save();
    ctx.translate(W / 2 - 320, cy - S / 2);
    const g = GRAD(ctx, 0, 0, S, S);
    ctx.fillStyle = g;
    ctx.shadowColor = 'rgba(59,110,246,0.5)'; ctx.shadowBlur = 44;
    rr(ctx, 0, 0, S, S, 34); ctx.fill();
    ctx.shadowBlur = 0;
    font(ctx, 800, S * 0.62); ctx.fillStyle = '#fff';
    ctx.textAlign = 'center'; ctx.textBaseline = 'middle';
    ctx.fillText('R', S / 2 + 1, S / 2 + 8);
    ctx.textAlign = 'left'; ctx.textBaseline = 'alphabetic';
    ctx.restore();
    // 字标
    const wp = segE(t, 5.8, 7.2, eo3);
    if (wp > 0.001) {
      alpha(ctx, a * wp);
      ctx.translate((1 - wp) * 30, 0);
      font(ctx, 800, 88);
      ctx.fillStyle = '#f4f7fd';
      ctx.shadowColor = 'rgba(123,92,255,0.35)'; ctx.shadowBlur = 24;
      lsText(ctx, 'RLPlayer', W / 2 - 200, cy + 30, '10px');
      ctx.shadowBlur = 0;
      font(ctx, 600, 22);
      ctx.fillStyle = PAL.blue;
      lsText(ctx, 'HIGH-FIDELITY ENGINE', W / 2 - 196, cy + 76, '8px');
    }
    ctx.restore();
  }

  // ---- 发行语（11.5s 起）----
  const dp2 = segE(t, 11.5, 13.0, eo3);
  if (dp2 > 0.001) {
    ctx.save();
    alpha(ctx, dp2 * (1 - segE(t, 14.8, 15.9, eio)));
    ctx.translate(0, (1 - dp2) * 18);
    font(ctx, 500, 32);
    const s = '单文件安装包 · 免依赖 · Windows 10 / 11';
    ctx.fillStyle = '#c4cbdb';
    ctx.fillText(s, W / 2 - ctx.measureText(s).width / 2, 878);
    font(ctx, 500, 22); ctx.fillStyle = PAL.dim;
    lsText(ctx, 'BUILT FOR LISTENING', W / 2 - 130, 930, '6px');
    ctx.restore();
  }

  // ---- 黑场收尾 ----
  const fadeB = segE(t, 15.2, 16.9, eio);
  if (fadeB > 0.001) {
    ctx.fillStyle = `rgba(0,0,0,${fadeB})`;
    ctx.fillRect(0, 0, W, H);
  }
});
