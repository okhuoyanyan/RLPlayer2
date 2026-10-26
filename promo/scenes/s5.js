'use strict';
// scenes/s5.js — AI 场景（20s）
// 节拍：1.0 AI 简介卡（打字机正文 + 光标 + 来源 chips 打勾）→ 8.0 一句话检索卡（右侧滑入）
//       → 12.0 三张特性卡依次弹入（自动补全 / 防幻觉 / 古典作品页）→ 18 后稳定呼吸
// 确定性：画面是 t 的纯函数；图标全部几何绘制（无 emoji 字符）；来源名一律通用词。

scene(20, (ctx, t) => {
  bgGrad(ctx);
  stars(ctx, t, 80, 8, 0.26);
  bgGlow(ctx, W * 0.58, H * 0.5, 800, PAL.cyan, 0.09);
  bgGlow(ctx, W * 0.3, H * 0.72, 620, PAL.purple, 0.07);

  // ---- AI 简介卡（中央偏左，900×360）----
  const cp5 = segE(t, 1.0, 1.7, eo3);
  if (cp5 > 0.001) {
    const cx = 260, cy = lerp(460, 420, cp5), cw = 900, ch = 360;
    ctx.save(); alpha(ctx, cp5);
    ctx.fillStyle = 'rgba(16,24,35,0.93)'; rr(ctx, cx, cy, cw, ch, 20); ctx.fill();
    ctx.strokeStyle = PAL.line; ctx.lineWidth = 1.5; rr(ctx, cx, cy, cw, ch, 20); ctx.stroke();
    ctx.fillStyle = PAL.cyan;
    ctx.shadowColor = PAL.cyan; ctx.shadowBlur = 10;
    ctx.fillRect(cx, cy + 24, 4, ch - 48);
    ctx.shadowBlur = 0;
    font(ctx, 700, 30); ctx.fillStyle = PAL.text;
    ctx.fillText('AI 简介', cx + 40, cy + 58);
    // 角标 pill（橙色）
    const bp = segE(t, 1.6, 2.1, eob);
    if (bp > 0.001) {
      ctx.save(); alpha(ctx, bp); ctx.translate(0, (1 - bp) * -14);
      pill(ctx, cx + 720, cy + 44, 'AI 整理', { fs: 20, color: PAL.orange });
      ctx.restore();
    }
    ctx.strokeStyle = PAL.line; ctx.lineWidth = 1;
    ctx.beginPath(); ctx.moveTo(cx + 40, cy + 84); ctx.lineTo(cx + cw - 40, cy + 84); ctx.stroke();
    // 正文打字机（两行，光标闪烁）
    const L1 = '一张融合室内乐编制与现代电子音色的概念专辑，动态起伏宽广，';
    const L2 = '适合在深夜专注聆听。条目由 AI 整理，来源可在曲库中追溯。';
    const ts = typeSlice(L1 + L2, t, 2.3, 13);
    const la = ts.txt.slice(0, L1.length), lb = ts.txt.slice(L1.length);
    font(ctx, 400, 28); ctx.fillStyle = PAL.text;
    ctx.fillText(la, cx + 40, cy + 140);
    ctx.fillText(lb, cx + 40, cy + 192);
    if (!ts.done && Math.sin(t * 8) > 0) {
      const px = lb.length > 0 ? cx + 40 + textW(ctx, lb) : cx + 40 + textW(ctx, la);
      const py = lb.length > 0 ? cy + 192 : cy + 140;
      ctx.fillStyle = PAL.cyan; ctx.fillRect(px + 6, py - 24, 3, 32);
    }
    // 来源 chips（逐个点亮 + 几何勾，非 emoji）
    const CHIPS = [['在线曲库', 6.8], ['歌词库', 7.3], ['音乐数据库', 7.8]];
    let chx = cx + 40;
    for (let k = 0; k < 3; k++) {
      const p = segE(t, CHIPS[k][1], CHIPS[k][1] + 0.45, eob);
      ctx.save(); alpha(ctx, p); ctx.translate(0, (1 - p) * 14);
      font(ctx, 500, 22);
      const wch = textW(ctx, CHIPS[k][0]) + 60;
      ctx.fillStyle = 'rgba(52,211,153,0.08)'; rr(ctx, chx, cy + 268, wch, 42, 21); ctx.fill();
      ctx.strokeStyle = 'rgba(52,211,153,0.5)'; ctx.lineWidth = 1.5; rr(ctx, chx, cy + 268, wch, 42, 21); ctx.stroke();
      const ck = seg(t, CHIPS[k][1] + 0.2, CHIPS[k][1] + 0.55);
      ctx.strokeStyle = PAL.green; ctx.lineWidth = 2.5; ctx.lineCap = 'round';
      ctx.beginPath(); ctx.moveTo(chx + 16, cy + 289);
      if (ck > 0.35) ctx.lineTo(chx + 22, cy + 296);
      if (ck > 0.7) ctx.lineTo(chx + 34, cy + 281);
      ctx.stroke(); ctx.lineCap = 'butt';
      ctx.fillStyle = PAL.text;
      ctx.fillText(CHIPS[k][0], chx + 46, cy + 296);
      chx += wch + 14;
      ctx.restore();
    }
    ctx.restore();
  }

  // ---- 一句话检索卡（右侧滑入，600×220）----
  const sp5 = segE(t, 8.0, 8.7, eio);
  if (sp5 > 0.001) {
    const sx = lerp(1310, 1220, sp5), sy = 420, sw = 600, sh = 220;
    ctx.save(); alpha(ctx, sp5);
    ctx.fillStyle = 'rgba(16,24,35,0.93)'; rr(ctx, sx, sy, sw, sh, 18); ctx.fill();
    ctx.strokeStyle = PAL.line; ctx.lineWidth = 1.5; rr(ctx, sx, sy, sw, sh, 18); ctx.stroke();
    ctx.fillStyle = PAL.purple; ctx.fillRect(sx, sy + 20, 4, sh - 40);
    // 搜索框
    ctx.fillStyle = 'rgba(255,255,255,0.04)';
    rr(ctx, sx + 32, sy + 32, sw - 64, 54, 12); ctx.fill();
    ctx.strokeStyle = PAL.line; ctx.lineWidth = 1.5; rr(ctx, sx + 32, sy + 32, sw - 64, 54, 12); ctx.stroke();
    ctx.strokeStyle = PAL.muted; ctx.lineWidth = 2.2;
    ctx.beginPath(); ctx.arc(sx + 58, sy + 55, 8, 0, 7); ctx.stroke();
    ctx.beginPath(); ctx.moveTo(sx + 64, sy + 61); ctx.lineTo(sx + 70, sy + 67); ctx.stroke();
    const q = typeSlice('找一首适合深夜的慢歌', t, 8.8, 9);
    font(ctx, 500, 26);
    let qcx = sx + 84;
    if (q.txt) { ctx.fillStyle = PAL.text; ctx.fillText(q.txt, sx + 84, sy + 68); qcx += textW(ctx, q.txt); }
    if (!q.done && Math.sin(t * 8) > 0) { ctx.fillStyle = PAL.cyan; ctx.fillRect(qcx + 5, sy + 44, 2.5, 30); }
    // 结果两行（虚构曲名）
    const R5 = [['霓虹雨后 · 慢板钢琴', '4:47', 190], ['午夜电台 · Vol.3', '5:21', 265]];
    for (let k = 0; k < 2; k++) {
      const p = segE(t, 10.2 + k * 0.3, 10.65 + k * 0.3, eo3);
      if (p <= 0.001) continue;
      const ry = sy + 104 + k * 54 + (1 - p) * 12;
      ctx.save(); alpha(ctx, p);
      ctx.fillStyle = 'rgba(255,255,255,0.035)'; rr(ctx, sx + 32, ry, sw - 64, 44, 10); ctx.fill();
      ctx.strokeStyle = PAL.line; ctx.lineWidth = 1; rr(ctx, sx + 32, ry, sw - 64, 44, 10); ctx.stroke();
      ctx.fillStyle = `hsl(${R5[k][2]},55%,34%)`; rr(ctx, sx + 42, ry + 7, 30, 30, 7); ctx.fill();
      font(ctx, 500, 23); ctx.fillStyle = PAL.text; ctx.fillText(R5[k][0], sx + 84, ry + 29);
      font(ctx, 400, 19); ctx.fillStyle = PAL.muted; ctx.fillText(R5[k][1], sx + sw - 96, ry + 28);
      ctx.restore();
    }
    // 角标 pill（绿）
    const qp = segE(t, 11.2, 11.75, eob);
    if (qp > 0.001) {
      ctx.save(); alpha(ctx, qp); ctx.translate(0, (1 - qp) * -16);
      pill(ctx, sx + 400, sy - 2, '一句话检索', { fs: 22, color: PAL.green });
      ctx.restore();
    }
    ctx.restore();
  }

  // ---- 三张特性卡（依次弹入 eob 缩放，之后轻微悬浮呼吸）----
  const F5 = [
    { title: '自动补全', sub: '封面 · 标签 · 歌词', col: PAL.cyan },
    { title: '防幻觉', sub: '标注 AI 生成 · 来源强制引用', col: PAL.purple },
    { title: '古典作品页', sub: '作曲家 · 版本对比', col: PAL.blue },
  ];
  for (let k = 0; k < 3; k++) {
    const p = segE(t, 12.0 + k * 0.6, 12.6 + k * 0.6, eob);
    if (p <= 0.001) continue;
    const fw = 500, fh = 170;
    const fx = 140 + k * 570;
    const fy = 800 + Math.sin(t * 1.3 + k * 1.2) * 4;
    const mx = fx + fw / 2, my = fy + fh / 2;
    ctx.save(); alpha(ctx, Math.min(1, p * 1.2));
    ctx.translate(mx, my); ctx.scale(0.82 + 0.18 * p, 0.82 + 0.18 * p); ctx.translate(-mx, -my);
    ctx.fillStyle = 'rgba(255,255,255,0.03)'; rr(ctx, fx, fy, fw, fh, 18); ctx.fill();
    ctx.strokeStyle = PAL.line; ctx.lineWidth = 1.5; rr(ctx, fx, fy, fw, fh, 18); ctx.stroke();
    ctx.fillStyle = F5[k].col; ctx.fillRect(fx, fy + 24, 4, fh - 48);
    font(ctx, 700, 30); ctx.fillStyle = PAL.text;
    ctx.fillText(F5[k].title, fx + 108, fy + 74);
    font(ctx, 400, 22); ctx.fillStyle = PAL.muted;
    ctx.fillText(F5[k].sub, fx + 108, fy + 114);
    if (k === 0) { // 三格补全：虚线框轮流变实心图块
      const cols3 = [PAL.cyan, PAL.blue, PAL.purple];
      for (let m = 0; m < 3; m++) {
        const solid = ((t * 0.5 + m * 0.29) % 1) < 0.58;
        if (solid) { ctx.fillStyle = cols3[m]; rr(ctx, fx + 30 + m * 26, fy + 76, 18, 18, 5); ctx.fill(); }
        else {
          ctx.setLineDash([4, 3]); ctx.strokeStyle = PAL.dim; ctx.lineWidth = 1.5;
          rr(ctx, fx + 30 + m * 26, fy + 76, 18, 18, 5); ctx.stroke(); ctx.setLineDash([]);
        }
      }
    } else if (k === 1) { // 盾牌 + 勾（防幻觉）
      const scx = fx + 66, scy = fy + 85;
      ctx.fillStyle = 'rgba(123,92,255,0.16)';
      ctx.strokeStyle = PAL.purple; ctx.lineWidth = 2;
      ctx.beginPath();
      ctx.moveTo(scx, scy - 26);
      ctx.bezierCurveTo(scx + 16, scy - 20, scx + 22, scy - 14, scx + 22, scy - 6);
      ctx.bezierCurveTo(scx + 22, scy + 10, scx + 12, scy + 20, scx, scy + 26);
      ctx.bezierCurveTo(scx - 12, scy + 20, scx - 22, scy + 10, scx - 22, scy - 6);
      ctx.bezierCurveTo(scx - 22, scy - 14, scx - 16, scy - 20, scx, scy - 26);
      ctx.closePath(); ctx.fill(); ctx.stroke();
      ctx.strokeStyle = PAL.green; ctx.lineWidth = 2.5; ctx.lineCap = 'round';
      ctx.beginPath();
      ctx.moveTo(scx - 9, scy - 1); ctx.lineTo(scx - 2, scy + 7); ctx.lineTo(scx + 11, scy - 9);
      ctx.stroke(); ctx.lineCap = 'butt';
    } else { // 古典作品页：声部行（圆点 + 变长横线）
      const lens = [44, 30, 38], dcols = [PAL.gold, PAL.cyan, PAL.purple];
      for (let m = 0; m < 3; m++) {
        const ry = fy + 52 + m * 30;
        glowDot(ctx, fx + 44, ry, 4, dcols[m], 0.9);
        ctx.strokeStyle = 'rgba(230,233,239,0.35)'; ctx.lineWidth = 3; ctx.lineCap = 'round';
        ctx.beginPath(); ctx.moveTo(fx + 58, ry); ctx.lineTo(fx + 58 + lens[m], ry); ctx.stroke();
        ctx.lineCap = 'butt';
      }
    }
    ctx.restore();
  }

  caption(ctx, t, {
    kicker: 'AI-POWERED LIBRARY',
    title: 'AI 帮你打理曲库',
    sub: '简介 · 检索 · 补全，来源可溯',
  });
});
