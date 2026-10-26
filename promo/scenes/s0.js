'use strict';
// scenes/s0.js — 片头：粒子汇聚 → R 徽标 + 波纹环 → 字标 → 标语（11s）
scene(11, (ctx, t) => {
  bgGrad(ctx, '#04060a', '#070a10');
  stars(ctx, t, 170, 9, 0.42);
  const cx = W / 2, cy = H / 2 - 60;

  // ---- 1) 粒子汇聚成环（0–5s）----
  const conv = eio(seg(t, 0.9, 5.0));
  const N = 240;
  for (let i = 0; i < N; i++) {
    const ang0 = hash(i * 3.13) * Math.PI * 2;
    const sx = hash(i * 7.71) * W, sy = hash(i * 9.37) * H;
    const ang = ang0 + t * 0.06 * (hash(i + 3) - 0.5) + t * 0.03;
    const R = 235 + Math.sin(t * 1.4 + i * 0.7) * 9;
    const x = lerp(sx, cx + Math.cos(ang) * R, conv);
    const y = lerp(sy, cy + Math.sin(ang) * R, conv);
    const tw = 0.5 + 0.5 * Math.sin(t * 2 + i * 1.7);
    alpha(ctx, lerp(0.28, 0.75, conv) * (0.45 + 0.55 * tw) * (1 - segE(t, 9.2, 10.6) * 0.7));
    ctx.fillStyle = i % 11 === 0 ? PAL.blue : i % 17 === 0 ? PAL.purple : '#b9c6de';
    const s = 1.2 + hash(i + 31) * 2.2;
    ctx.fillRect(x, y, s, s);
  }
  alpha(ctx, 1);

  // ---- 2) 环形波形线（3.6s 起，随 logo 出现变亮）----
  const wr = segE(t, 3.6, 5.4, eio);
  if (wr > 0.01) {
    ctx.save();
    alpha(ctx, wr * 0.8 * (1 - segE(t, 9.2, 10.6) * 0.6));
    ctx.strokeStyle = PAL.blue; ctx.lineWidth = 2;
    ctx.shadowColor = PAL.blue; ctx.shadowBlur = 12;
    ctx.beginPath();
    const R0 = 235;
    for (let a = 0; a <= 360; a += 2) {
      const rad = a * Math.PI / 180;
      const wave = Math.sin(rad * 14 + t * 5) * 10 + Math.sin(rad * 5 - t * 3) * 6;
      const r = R0 + 26 + wave;
      const x = cx + Math.cos(rad) * r, y = cy + Math.sin(rad) * r;
      a === 0 ? ctx.moveTo(x, y) : ctx.lineTo(x, y);
    }
    ctx.closePath(); ctx.stroke();
    ctx.restore();
  }

  // ---- 3) R 徽标（4.6s 起，backOut 弹入）----
  const lp = segE(t, 4.6, 6.0, eob);
  if (lp > 0.001) {
    const S = 190, lx = cx - 420, ly = cy - S / 2;
    ctx.save();
    ctx.translate(lx + S / 2, ly + S / 2);
    ctx.scale(lerp(0.55, 1, lp), lerp(0.55, 1, lp));
    ctx.globalAlpha = clamp01(seg(t, 4.6, 5.3));
    const g = GRAD(ctx, -S / 2, -S / 2, S / 2, S / 2);
    ctx.fillStyle = g;
    ctx.shadowColor = 'rgba(59,110,246,0.5)'; ctx.shadowBlur = 40;
    rr(ctx, -S / 2, -S / 2, S, S, 44); ctx.fill();
    ctx.shadowBlur = 0;
    font(ctx, 800, 122); ctx.fillStyle = '#fff';
    ctx.textAlign = 'center'; ctx.textBaseline = 'middle';
    ctx.fillText('R', 2, 10);
    ctx.textAlign = 'left'; ctx.textBaseline = 'alphabetic';
    ctx.restore();
  }

  // ---- 4) 字标（5.6s 起）----
  const wp = segE(t, 5.6, 7.2, eo3);
  if (wp > 0.001) {
    ctx.save();
    alpha(ctx, wp);
    ctx.translate((1 - wp) * 46, 0);
    font(ctx, 800, 96);
    ctx.fillStyle = '#f4f7fd';
    ctx.shadowColor = 'rgba(123,92,255,0.35)'; ctx.shadowBlur = 26;
    lsText(ctx, 'RLPlayer', cx - 260, cy + 34, '12px');
    ctx.shadowBlur = 0;
    font(ctx, 600, 24);
    ctx.fillStyle = PAL.blue;
    lsText(ctx, 'HIGH-FIDELITY ENGINE', cx - 256, cy + 86, '9px');
    ctx.restore();
  }

  // ---- 5) 标语（8.0s 起）----
  const tp = segE(t, 8.0, 9.6, eo3);
  if (tp > 0.001) {
    ctx.save();
    alpha(ctx, tp);
    ctx.translate(0, (1 - tp) * 26);
    font(ctx, 500, 34);
    const s1 = '为极致聆听而生', s2 = ' · Windows 高保真播放引擎';
    const w1 = ctx.measureText(s1).width, w2 = ctx.measureText(s2).width;
    const x0 = cx - (w1 + w2) / 2;
    ctx.fillStyle = '#c4cbdb'; ctx.fillText(s1, x0, cy + 250);
    ctx.fillStyle = PAL.muted; ctx.fillText(s2, x0 + w1, cy + 250);
    ctx.restore();
  }

  // 底部中央小提示（1.5–4s 引导期）
  const hp = segE(t, 1.6, 2.6, eio) * (1 - segE(t, 4.2, 5.0, eio));
  if (hp > 0.01) {
    ctx.save(); alpha(ctx, hp * 0.55);
    font(ctx, 500, 22); ctx.fillStyle = PAL.dim;
    lsText(ctx, 'A NEW KIND OF PLAYER', W / 2 - 130, H - 96, '6px');
    ctx.restore();
  }
});
