'use strict';
// scenes/s4.js — 原生 DSD 全链路：PCM 阶梯 → 1-bit 脉冲密度 → DSD 512（19s）
// 顶层预生成一阶 SDM 比特流（确定性，绘制期不随机）
const DSD_BITS = (() => {
  const n = 4096, bits = new Uint8Array(n);
  let integ = 0;
  for (let i = 0; i < n; i++) {
    const x = 0.5 + 0.45 * Math.sin(i / n * Math.PI * 2 * 24); // 24 个周期
    const y = integ >= 0 ? 1 : 0;
    integ += x - y;
    bits[i] = y;
  }
  return bits;
})();

scene(19, (ctx, t) => {
  bgGrad(ctx);
  stars(ctx, t, 120, 10, 0.3);
  bgGlow(ctx, W * 0.55, H * 0.55, 880, PAL.purple, 0.10);
  // 巨大的 1-bit 底纹字
  ctx.save();
  alpha(ctx, 0.045 * segE(t, 2.0, 4.0));
  ctx.translate(1230, 540); ctx.rotate(-0.06);
  font(ctx, 800, 230); ctx.fillStyle = PAL.cyan;
  ctx.fillText('1-bit', -260, 80);
  ctx.restore();

  caption(ctx, t, {
    kicker: 'NATIVE DSD',
    title: '原生 DSD 全链路',
    sub: 'DSF · DFF 直读 · DirectSDM 逐位直通 · DoP 24-bit 载波',
  });

  const X0 = 260, X1 = 1660;
  const scrollPx = t * 340;           // 视觉滚动速度 px/s
  const per = 683;                    // 包络周期（px），与比特流 24 周期/4096 采样对齐

  // ---- 上层：PCM 阶梯波（y=430）----
  const pp = segE(t, 0.3, 1.8, eo3);
  if (pp > 0.001) {
    ctx.save();
    alpha(ctx, pp * (1 - segE(t, 8.5, 10.5, eio) * 0.55));
    ctx.strokeStyle = PAL.blue; ctx.lineWidth = 2.5;
    ctx.shadowColor = PAL.blue; ctx.shadowBlur = 10;
    ctx.beginPath();
    const step = 26, amp = 88, levels = 12;
    for (let x = X0; x <= X1 + step; x += step) {
      const raw = Math.sin((x + scrollPx) * Math.PI * 2 / per);
      const q = Math.round(raw * levels / 2) / (levels / 2);
      const y = 430 - q * amp;
      const xi = Math.min(x, X1);
      x === X0 ? ctx.moveTo(xi, y) : ctx.lineTo(xi, y);
      ctx.lineTo(Math.min(x + step, X1), y);
    }
    ctx.stroke();
    // 采样方块
    ctx.shadowBlur = 0; ctx.fillStyle = '#9db8f5';
    for (let x = X0; x <= X1; x += step * 2) {
      const raw = Math.sin((x + scrollPx) * Math.PI * 2 / per);
      const q = Math.round(raw * levels / 2) / (levels / 2);
      ctx.fillRect(x - 3, 430 - q * amp - 3, 6, 6);
    }
    ctx.restore();
    // 标签
    ctx.save(); alpha(ctx, pp);
    font(ctx, 600, 22);
    pill(ctx, X0, 350, 'PCM 24-bit · 阶梯量化', { fs: 22, color: PAL.blue });
    ctx.restore();
  }

  // ---- 转换箭头（3–8s，从 PCM 层下落到 DSD 层）----
  const arp = segE(t, 3.2, 5.0, eio);
  if (arp > 0.01 && t < 9.5) {
    ctx.save();
    alpha(ctx, arp * (1 - segE(t, 8.0, 9.5, eio)));
    for (let k = 0; k < 5; k++) {
      const ph = (t * 0.6 + k / 5) % 1;
      const x = X0 + 140 + k * 260, y = lerp(490, 640, ph);
      ctx.strokeStyle = `rgba(123,92,255,${0.85 * (1 - ph * 0.3)})`;
      ctx.lineWidth = 3; ctx.lineCap = 'round';
      ctx.beginPath(); ctx.moveTo(x - 9, y - 8); ctx.lineTo(x, y); ctx.lineTo(x + 9, y - 8); ctx.stroke();
    }
    font(ctx, 600, 24); ctx.fillStyle = PAL.purple;
    ctx.fillText('DirectSDM · 逐位直通', X1 - 250, 578);
    ctx.restore();
  }

  // ---- 下层：1-bit 脉冲密度流（y=720）----
  const dp = segE(t, 4.0, 6.0, eo3);
  if (dp > 0.001) {
    ctx.save();
    alpha(ctx, dp);
    // 包络正弦（密度可视化基准）
    ctx.strokeStyle = PAL.purple; ctx.lineWidth = 2.5;
    ctx.shadowColor = PAL.purple; ctx.shadowBlur = 12;
    ctx.beginPath();
    for (let x = X0; x <= X1; x += 5) {
      const y = 720 + Math.sin((x + scrollPx) * Math.PI * 2 / per) * 66;
      x === X0 ? ctx.moveTo(x, y) : ctx.lineTo(x, y);
    }
    ctx.stroke();
    // 比特刻线
    ctx.shadowBlur = 8; ctx.shadowColor = PAL.cyan;
    ctx.strokeStyle = 'rgba(34,211,238,0.75)'; ctx.lineWidth = 2;
    const midY = 720, hgt = 26;
    ctx.beginPath();
    for (let x = X0; x <= X1; x += 4) {
      const i = Math.floor((x - X0 + scrollPx) / 4) % 4096;
      if (DSD_BITS[((i % 4096) + 4096) % 4096]) {
        ctx.moveTo(x + 0.5, midY - hgt); ctx.lineTo(x + 0.5, midY + hgt);
      }
    }
    ctx.stroke();
    ctx.restore();
    // 标签
    ctx.save(); alpha(ctx, dp);
    font(ctx, 600, 22);
    pill(ctx, X0, 640 - 26, 'DSD 1-bit · 脉冲密度', { fs: 22, color: PAL.purple });
    ctx.restore();
  }

  // ---- 速率大字（8–11s 计数）----
  const rp = segE(t, 7.8, 9.0, eo3);
  if (rp > 0.001) {
    ctx.save();
    alpha(ctx, rp);
    ctx.translate(0, (1 - rp) * 20);
    font(ctx, 800, 64);
    const rate = countUp(t, 2.8, 22.6, 8.2, 10.6, v => v.toFixed(1));
    const s = `PCM → DSD 512 · ${rate} MHz`;
    const w = ctx.measureText(s).width;
    const grad = GRAD(ctx, 0, 0, w, 0);
    ctx.fillStyle = grad;
    ctx.shadowColor = 'rgba(123,92,255,0.45)'; ctx.shadowBlur = 20;
    ctx.fillText(s, W / 2 - w / 2, 880);
    ctx.restore();
  }

  // ---- 底部模式 pills（12s 起）----
  const bp = segE(t, 11.8, 12.8, eio);
  if (bp > 0.01) {
    ctx.save(); alpha(ctx, bp);
    font(ctx, 600, 24);
    let x = 300;
    x = pill(ctx, x, 975, '原生 DSD · ASIO', { fs: 24, color: PAL.green }) + 26;
    x = pill(ctx, x, 975, 'DoP 24-bit 载波', { fs: 24, color: PAL.blue }) + 26;
    pill(ctx, x, 975, 'DSD 64 / 128 / 256 / 512', { fs: 24, color: PAL.purple });
    ctx.restore();
  }
});
