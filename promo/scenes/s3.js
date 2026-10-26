'use strict';
// scenes/s3.js — 升频引擎场景（26s）
// 节拍：0.5 波形展开 → 2.0 采样杆 24px→3.2px 加密 → 5.5 计数滚动到 705.6 kHz
//       → 6.5 频响曲线绘制（绿）→ 10.3 通带/阻带标注 → 12 GPU 面板（262,144 抽头 · 核阵列扫描）
//       → 15 实时频谱条（常驻到结尾）→ 20 Dither 徽标 → 构图稳定
// 确定性：全部图形是 t 的纯函数，随机只来自引擎 hash()。
// 布局：波形 y 382–538；频响绘图右下 x 940–1800 / y 704–908（避开左下 GPU 面板 140–860 / 660–960）；
//       频谱条 y 948–1014；左上 caption 与右下水印区域均留空。

scene(26, (ctx, t) => {
  bgGrad(ctx);
  stars(ctx, t, 70, 8, 0.25);
  bgGlow(ctx, W * 0.42, H * 0.5, 820, PAL.purple, 0.10);

  // ---- 波形加密（y=460，振幅 78）----
  const WX0 = 140, WX1 = 1800, WY = 460, AMP = 78;
  const yAt = x => WY + Math.sin(x * 0.02 + t * 1.4) * AMP;
  const cp = segE(t, 0.5, 1.8, eo3); // 平滑曲线从左到右展开
  if (cp > 0.001) {
    ctx.save(); alpha(ctx, 0.9);
    ctx.strokeStyle = PAL.blue; ctx.lineWidth = 2.5;
    ctx.shadowColor = PAL.blue; ctx.shadowBlur = 12;
    ctx.beginPath();
    const xe = lerp(WX0, WX1, cp);
    for (let x = WX0; x <= xe; x += 5) { const y = yAt(x); x === WX0 ? ctx.moveTo(x, y) : ctx.lineTo(x, y); }
    ctx.stroke(); ctx.restore();
  }
  // 采样杆：单路径一次描边（省 draw call），密度与颜色随进度（蓝→紫）
  const sd = segE(t, 2.0, 5.5, eio);
  if (sd > 0.001) {
    const spacing = lerp(24, 3.2, sd);
    ctx.save(); alpha(ctx, 0.55 + 0.35 * sd);
    ctx.strokeStyle = `rgb(${Math.round(lerp(59, 123, sd))},${Math.round(lerp(110, 92, sd))},${Math.round(lerp(246, 255, sd))})`;
    ctx.lineWidth = 1.6;
    ctx.beginPath();
    for (let x = WX0; x <= WX1; x += spacing) { ctx.moveTo(x, WY); ctx.lineTo(x, yAt(x)); }
    ctx.stroke(); ctx.restore();
  }
  // 采样率标签（左）
  const lp = segE(t, 1.0, 1.6, eob);
  if (lp > 0.001) {
    ctx.save(); alpha(ctx, lp); ctx.translate(0, (1 - lp) * 18);
    pill(ctx, 150, 356, '44.1 kHz', { fs: 22, color: PAL.blue });
    ctx.restore();
  }
  // 计数滚动（右）
  if (t > 5.5) {
    const p = segE(t, 5.5, 6.0, eob);
    const txt = countUp(t, 44.1, 705.6, 5.5, 6.6, v => v.toFixed(1)) + ' kHz';
    ctx.save(); alpha(ctx, p); ctx.translate(0, (1 - p) * 18);
    pill(ctx, 1640, 356, txt, { fs: 22, color: PAL.purple });
    ctx.restore();
  }

  // ---- 频响曲线（对数频率轴 10Hz–96kHz，纵轴 0/−100/−200 dB）----
  const PX0 = 940, PX1 = 1800, PY0 = 704, PYS = 204;
  const lx = f => PX0 + (Math.log10(f) - 1) / (Math.log10(96000) - 1) * (PX1 - PX0);
  const dbY = db => PY0 + (-db) / 200 * PYS;
  const fp = segE(t, 6.5, 10.5, eio);
  if (fp > 0.001) {
    // 轴与网格
    ctx.save(); alpha(ctx, Math.min(1, fp * 2));
    ctx.strokeStyle = PAL.line; ctx.lineWidth = 1;
    ctx.beginPath();
    for (let k = 0; k < 3; k++) { const y = PY0 + k * PYS / 2; ctx.moveTo(PX0, y); ctx.lineTo(PX1, y); }
    for (const f of [100, 1000, 10000]) { const x = lx(f); ctx.moveTo(x, PY0); ctx.lineTo(x, PY0 + PYS); }
    ctx.stroke();
    font(ctx, 400, 18); ctx.fillStyle = PAL.dim; ctx.textAlign = 'right';
    ctx.fillText('0', PX0 - 10, PY0 + 6);
    ctx.fillText('−100', PX0 - 10, PY0 + PYS / 2 + 6);
    ctx.fillText('−200', PX0 - 10, PY0 + PYS + 6);
    ctx.fillText('10Hz', PX0 + 4, PY0 + PYS + 26);
    ctx.textAlign = 'center';
    ctx.fillText('100', lx(100), PY0 + PYS + 26);
    ctx.fillText('1k', lx(1000), PY0 + PYS + 26);
    ctx.fillText('10k', lx(10000), PY0 + PYS + 26);
    ctx.textAlign = 'right';
    ctx.fillText('96kHz', PX1, PY0 + PYS + 26);
    ctx.textAlign = 'left';
    ctx.restore();
    // 曲线：通带平直 → 20 kHz 拐点后跌至 −189 dB（绿色发光）
    const knee = lx(20000), knee2 = knee + 42;
    ctx.save();
    ctx.strokeStyle = PAL.green; ctx.lineWidth = 3;
    ctx.shadowColor = PAL.green; ctx.shadowBlur = 12;
    ctx.beginPath();
    const xe = lerp(PX0, PX1, fp);
    for (let x = PX0; x <= xe; x += 6) {
      let y;
      if (x <= knee) y = PY0;
      else if (x <= knee2) y = lerp(PY0, dbY(-189), smooth((x - knee) / (knee2 - knee)));
      else y = dbY(-189);
      x === PX0 ? ctx.moveTo(x, y) : ctx.lineTo(x, y);
    }
    ctx.stroke(); ctx.restore();
  }
  // 通带标注
  const an1 = segE(t, 10.3, 11.0, eo3);
  if (an1 > 0.001) {
    ctx.save(); alpha(ctx, an1);
    glowDot(ctx, 1250, PY0, 6, PAL.green, an1);
    font(ctx, 500, 22); ctx.fillStyle = PAL.text;
    ctx.fillText('通带纹波 ±0.0000 dB', 1220, 676);
    ctx.restore();
  }
  // 阻带标注
  const an2 = segE(t, 10.7, 11.4, eo3);
  if (an2 > 0.001) {
    ctx.save(); alpha(ctx, an2);
    glowDot(ctx, lx(20000) + 15, dbY(-189), 6, PAL.green, an2);
    font(ctx, 500, 22); ctx.fillStyle = PAL.text; ctx.textAlign = 'right';
    ctx.fillText('阻带 −189 dB', 1800, 872);
    ctx.textAlign = 'left';
    ctx.restore();
  }

  // ---- 实时频谱条（最底部 64 根，15s 起常驻到结尾）----
  const sp3 = segE(t, 15, 16.2, eo3);
  if (sp3 > 0.001) {
    const N = 64, bx0 = 140, bx1 = 1780, base = 1014, bwd = (bx1 - bx0) / N;
    const sg = ctx.createLinearGradient(bx0, 0, bx1, 0);
    sg.addColorStop(0, PAL.blue); sg.addColorStop(1, PAL.purple);
    ctx.save(); alpha(ctx, sp3 * 0.85);
    ctx.fillStyle = sg;
    for (let i = 0; i < N; i++) {
      const env = (0.25 + 0.75 * hash(i * 1.7 + 3)) * (1 - (i / N) * 0.72); // 低频高、高频衰减
      const h = Math.max(2, env * (0.6 + 0.4 * Math.sin(t * 2 + i * 0.37)) * 66 * sp3);
      ctx.fillRect(bx0 + i * bwd + 4, base - h, bwd - 8, h);
    }
    ctx.restore();
    ctx.save(); alpha(ctx, sp3 * 0.6);
    ctx.strokeStyle = 'rgba(255,255,255,0.10)'; ctx.lineWidth = 1;
    ctx.beginPath(); ctx.moveTo(bx0, base + 2); ctx.lineTo(bx1, base + 2); ctx.stroke();
    ctx.restore();
  }

  // ---- GPU 面板（左下浮出，画在频谱之上）----
  const gp = segE(t, 12, 12.7, eo3);
  if (gp > 0.001) {
    const gx = 140, gy = lerp(700, 660, gp), gw = 720, gh = 300;
    ctx.save(); alpha(ctx, gp);
    ctx.fillStyle = 'rgba(16,24,35,0.94)'; rr(ctx, gx, gy, gw, gh, 18); ctx.fill();
    ctx.strokeStyle = PAL.line; ctx.lineWidth = 1.5; rr(ctx, gx, gy, gw, gh, 18); ctx.stroke();
    ctx.fillStyle = GRAD(ctx, gx, gy, gx + gw, gy); ctx.fillRect(gx + 2, gy + 2, gw - 4, 3);
    font(ctx, 700, 30); ctx.fillStyle = PAL.text;
    ctx.fillText('GPU 卷积加速', gx + 30, gy + 48);
    let px2 = gx + 30;
    const bp2 = segE(t, 12.4, 12.9, eob);
    ctx.save(); alpha(ctx, bp2); ctx.translate(0, (1 - bp2) * 14);
    px2 += pill(ctx, px2, gy + 88, 'NVIDIA CUDA', { fs: 21, color: PAL.green }) + 12;
    pill(ctx, px2, gy + 88, 'AMD GPU', { fs: 21, color: PAL.green });
    ctx.restore();
    font(ctx, 800, 42);
    const taps = countUp(t, 0, 262144, 13, 15.5, v => Math.round(v).toString().replace(/\B(?=(\d{3})+(?!\d))/g, ','));
    const twTaps = textW(ctx, taps);
    ctx.fillStyle = GRAD(ctx, gx + 30, 0, gx + 360, 0);
    ctx.fillText(taps, gx + 30, gy + 152);
    font(ctx, 500, 24); ctx.fillStyle = PAL.muted;
    ctx.fillText('抽头卷积', gx + 30 + twTaps + 14, gy + 152);
    font(ctx, 800, 30);
    const ms = countUp(t, 0, 2.0, 13.5, 15.5, v => v.toFixed(1));
    const twMs = textW(ctx, ms + ' ms');
    ctx.fillStyle = PAL.green; ctx.fillText(ms + ' ms', gx + 30, gy + 196);
    font(ctx, 500, 22); ctx.fillStyle = PAL.muted;
    ctx.fillText('/ 秒音频', gx + 30 + twMs + 12, gy + 196);
    // 核阵列 6×3：波浪扫描轮流点亮
    const cw = 52, chh = 18, gx0 = gx + 30, gy0 = gy + 218;
    for (let r2 = 0; r2 < 3; r2++) for (let c2 = 0; c2 < 6; c2++) {
      const pulse = Math.max(0, Math.sin(t * 2.0 - c2 * 0.7 + r2 * 0.5));
      const lit = pulse * pulse * pulse;
      const cxx = gx0 + c2 * 60, cyy = gy0 + r2 * 25;
      ctx.fillStyle = `rgba(52,211,153,${(0.10 + lit * 0.72).toFixed(3)})`;
      rr(ctx, cxx, cyy, cw, chh, 5); ctx.fill();
      ctx.strokeStyle = `rgba(52,211,153,${(0.18 + lit * 0.5).toFixed(3)})`;
      ctx.lineWidth = 1; rr(ctx, cxx, cyy, cw, chh, 5); ctx.stroke();
    }
    ctx.restore();
  }

  // ---- Dither 徽标（右上，常驻到结尾）----
  const dp = segE(t, 20, 20.6, eob);
  if (dp > 0.001) {
    ctx.save(); alpha(ctx, dp); ctx.translate(0, (1 - dp) * 16);
    pill(ctx, 1620, 176, 'Dither · 噪声整形', { fs: 22, color: PAL.cyan });
    ctx.restore();
  }

  caption(ctx, t, {
    kicker: 'UPSAMPLING ENGINE',
    title: '可测量的升频引擎',
    sub: '多相 sinc · 阻带 −189 dB · 通带纹波 0.0000 dB',
  });
});
