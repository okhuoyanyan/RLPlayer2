'use strict';
// scenes/s2.js — bit-perfect 位元直通：音频路径 · 拒绝系统混音 · 六态徽标（21s）
scene(21, (ctx, t) => {
  bgGrad(ctx);
  stars(ctx, t, 120, 10, 0.3);
  bgGlow(ctx, W * 0.5, H * 0.62, 900, PAL.blue, 0.09);
  grid(ctx, 0.035);
  caption(ctx, t, {
    kicker: 'BIT-PERFECT DIRECT',
    title: '位元直通 · 不动一个比特',
    sub: 'WASAPI 独占 · ASIO · 绕开系统混音',
  });

  const PY = 600; // 主通路 y
  const nodes = [
    { x: 300,  icon: 'file',   tt: '音乐文件',   st: 'FLAC / DSD' },
    { x: 740,  icon: 'decode', tt: '解码',       st: '原始码流' },
    { x: 1180, icon: 'lock',   tt: '独占音频流', st: 'WASAPI 独占 / ASIO' },
    { x: 1620, icon: 'dac',    tt: 'DAC · 功放', st: '模拟输出' },
  ];
  const NW = 340, NH = 130;

  // ---- 节点间数据包流动 ----
  const flowT = seg(t, 2.0, 99);
  for (let k = 0; k < nodes.length - 1; k++) {
    const x0 = nodes[k].x + NW / 2 + 8, x1 = nodes[k + 1].x - NW / 2 - 8;
    flowLine(ctx, x0, PY, x1, PY, PAL.blue, 0.35 * segE(t, 1.4 + k * 0.5, 2.2 + k * 0.5), 2);
    if (t > 2.2 + k * 0.5) {
      const per = 1.6;
      for (let j = 0; j < 3; j++) {
        const ph = (flowT / per + j / 3 + k * 0.13) % 1;
        glowDot(ctx, lerp(x0, x1, ph), PY, 5.5, ph > 0.45 && ph < 0.55 ? PAL.white : PAL.blue, 0.9 * Math.sin(ph * Math.PI));
      }
    }
  }

  // ---- 节点本体（依次弹入）----
  const drawIcon = (kind, x, y, active) => {
    ctx.save();
    ctx.translate(x, y);
    ctx.strokeStyle = active ? PAL.blue : PAL.dim; ctx.fillStyle = ctx.strokeStyle;
    ctx.lineWidth = 2.5; ctx.lineJoin = 'round';
    if (kind === 'file') {
      ctx.strokeRect(-16, -20, 32, 40);
      ctx.beginPath(); ctx.moveTo(-16, -6); ctx.lineTo(0, -6); ctx.stroke();
      ctx.beginPath(); ctx.moveTo(-16, 4); ctx.lineTo(12, 4); ctx.stroke();
    } else if (kind === 'decode') {
      for (let i = 0; i < 3; i++) {
        ctx.beginPath(); ctx.moveTo(-18 + i * 14, -14); ctx.lineTo(-6 + i * 14, 0); ctx.lineTo(-18 + i * 14, 14); ctx.stroke();
      }
    } else if (kind === 'lock') {
      ctx.strokeRect(-16, -4, 32, 24);
      ctx.beginPath(); ctx.arc(0, -4, 10, Math.PI, 0); ctx.stroke();
    } else if (kind === 'dac') {
      ctx.beginPath(); ctx.moveTo(-14, -8); ctx.lineTo(-2, -8); ctx.lineTo(-2, -18); ctx.lineTo(14, 0); ctx.lineTo(-2, 18); ctx.lineTo(-2, 8); ctx.lineTo(-14, 8); ctx.closePath(); ctx.stroke();
    }
    ctx.restore();
  };
  nodes.forEach((n, k) => {
    const p = segE(t, 0.8 + k * 0.45, 1.8 + k * 0.45, eob);
    if (p <= 0.001) return;
    ctx.save();
    ctx.translate(n.x, PY);
    ctx.scale(lerp(0.7, 1, p), lerp(0.7, 1, p));
    alpha(ctx, clamp01(seg(t, 0.8 + k * 0.45, 1.4 + k * 0.45)));
    // 面板
    ctx.fillStyle = 'rgba(16,24,35,0.92)';
    ctx.strokeStyle = PAL.line + 'cc'; ctx.lineWidth = 1.5;
    rr(ctx, -NW / 2, -NH / 2, NW, NH, 20); ctx.fill(); ctx.stroke();
    // 激活描边
    const act = segE(t, 2.0 + k * 0.9, 2.6 + k * 0.9);
    if (act > 0.01) { ctx.strokeStyle = PAL.blue; ctx.globalAlpha *= 1; alpha(ctx, act * 0.9); ctx.lineWidth = 2; rr(ctx, -NW / 2, -NH / 2, NW, NH, 20); ctx.stroke(); ctx.globalAlpha = clamp01(seg(t, 0.8 + k * 0.45, 1.4 + k * 0.45)); }
    // 图标 + 文本
    const iact = t > 2.0 + k * 0.9;
    drawIcon(n.icon, -NW / 2 + 46, 0, iact);
    font(ctx, 700, 30); ctx.fillStyle = iact ? '#f0f4fb' : PAL.muted;
    ctx.fillText(n.tt, -NW / 2 + 82, -8);
    font(ctx, 500, 21); ctx.fillStyle = PAL.dim;
    ctx.fillText(n.st, -NW / 2 + 82, 26);
    // 比特标识（数据包抵达后常驻）
    const bp = segE(t, 2.6 + k * 0.9, 3.2 + k * 0.9, eo3);
    if (bp > 0.01) {
      alpha(ctx, bp);
      font(ctx, 600, 19);
      const s = '24 bit · 96 kHz';
      const w = textW(ctx, s, '1px');
      ctx.fillStyle = 'rgba(52,211,153,0.12)';
      rr(ctx, NW / 2 - w - 34, NH / 2 - 36, w + 24, 26, 13); ctx.fill();
      ctx.strokeStyle = 'rgba(52,211,153,0.5)'; ctx.lineWidth = 1; rr(ctx, NW / 2 - w - 34, NH / 2 - 36, w + 24, 26, 13); ctx.stroke();
      ctx.fillStyle = PAL.green; ctx.fillText(s, NW / 2 - w - 22, NH / 2 - 17);
    }
    ctx.restore();
  });

  // ---- 系统混音器：被拒之门外的幽灵节点 ----
  const mp = segE(t, 5.0, 6.2, eo3);
  if (mp > 0.001) {
    const reject = segE(t, 7.2, 8.8, eio);
    const mx = 960 + reject * 60, my = 380 + reject * 180;
    ctx.save();
    alpha(ctx, mp * (1 - segE(t, 8.6, 9.8, eio)));
    ctx.translate(mx, my);
    ctx.rotate(reject * 0.12);
    ctx.fillStyle = 'rgba(239,68,68,0.05)';
    ctx.setLineDash([7, 7]);
    ctx.strokeStyle = 'rgba(239,68,68,0.45)'; ctx.lineWidth = 1.5;
    rr(ctx, -170, -38, 340, 76, 16); ctx.fill(); ctx.stroke();
    ctx.setLineDash([]);
    font(ctx, 700, 26); ctx.fillStyle = 'rgba(239,68,68,0.75)';
    ctx.fillText('系统混音器 · 音量处理', -150, 9);
    // 虚线路径尝试接入 → 被划断
    const cp = segE(t, 6.0, 7.2, eio);
    ctx.setLineDash([5, 9]);
    ctx.strokeStyle = 'rgba(239,68,68,0.4)';
    ctx.beginPath(); ctx.moveTo(0, 38); ctx.lineTo(0, lerp(38, PY - 70, cp)); ctx.stroke();
    ctx.setLineDash([]);
    if (t > 7.4) {
      const xp = segE(t, 7.4, 8.2, eob);
      ctx.save();
      ctx.translate(0, lerp(38, PY - 70, cp) - 6);
      ctx.rotate(-Math.PI / 4 * xp);
      ctx.strokeStyle = PAL.red; ctx.lineWidth = 4; ctx.lineCap = 'round';
      ctx.beginPath(); ctx.moveTo(-16 * xp, 0); ctx.lineTo(16 * xp, 0); ctx.stroke();
      ctx.restore();
      font(ctx, 700, 22); ctx.fillStyle = PAL.red; alpha(ctx, segE(t, 7.8, 8.4));
      ctx.fillText('绕开', 30, lerp(38, PY - 70, cp));
    }
    ctx.restore();
  }

  // ---- 三零大数字（12.5–15s 弹入）----
  const stats = ['0 混音', '0 重采样', '0 音量处理'];
  stats.forEach((s, k) => {
    const p = segE(t, 12.5 + k * 0.5, 13.6 + k * 0.5, eob);
    if (p <= 0.001) return;
    ctx.save();
    const x = 560 + k * 400, y = 850;
    ctx.translate(x, y);
    ctx.scale(lerp(0.6, 1, p), lerp(0.6, 1, p));
    alpha(ctx, clamp01(seg(t, 12.5 + k * 0.5, 13.2 + k * 0.5)));
    font(ctx, 800, 58);
    const zero = '0', rest = s.slice(1);
    ctx.fillStyle = PAL.green;
    ctx.shadowColor = 'rgba(52,211,153,0.5)'; ctx.shadowBlur = 18;
    ctx.fillText(zero, -ctx.measureText(s).width / 2, 0);
    ctx.shadowBlur = 0;
    ctx.fillStyle = '#dfe5f0';
    ctx.fillText(rest, -ctx.measureText(s).width / 2 + ctx.measureText(zero).width, 0);
    ctx.restore();
  });

  // ---- 六态徽标轮播（底部）----
  const car = [
    { s: 'BIT-PERFECT', c: PAL.green, d: '位元完美直通' },
    { s: 'PCM → PCM 升频', c: PAL.blue, d: '无损升采样' },
    { s: 'PCM → DSD', c: PAL.purple, d: '调制到 1-bit' },
    { s: 'DSD → DSD', c: PAL.gold, d: '直通或变速率' },
  ];
  const cp2 = segE(t, 15.0, 16.0, eio);
  if (cp2 > 0.01) {
    const cur = car[Math.floor(Math.max(0, t - 16.0) / 2.2) % car.length];
    ctx.save();
    alpha(ctx, cp2);
    // 左侧说明
    font(ctx, 500, 22); ctx.fillStyle = PAL.dim;
    ctx.fillText('底栏状态徽标 · 实时可知', 560, 992);
    // 徽标本体
    font(ctx, 700, 26);
    const bw = textW(ctx, cur.s, '1px') + 76;
    const bx = 980, by = 984;
    ctx.fillStyle = 'rgba(255,255,255,0.04)';
    ctx.strokeStyle = cur.c; ctx.lineWidth = 1.8;
    rr(ctx, bx, by - 25, bw, 50, 25); ctx.fill(); ctx.stroke();
    glowDot(ctx, bx + 30, by, 5, cur.c, 0.6 + 0.4 * Math.sin(t * 5));
    ctx.fillStyle = cur.c; lsText(ctx, cur.s, bx + 48, by + 9, '1px');
    font(ctx, 500, 21); ctx.fillStyle = PAL.muted;
    ctx.fillText(cur.d, bx + bw + 22, by + 7);
    ctx.restore();
  }
});
