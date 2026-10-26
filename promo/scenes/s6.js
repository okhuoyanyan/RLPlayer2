'use strict';
// scenes/s6.js — 细节宫格：歌词滚动 / 20 段 PEQ / UPnP 投放 / 托盘与主题（18s）
scene(18, (ctx, t) => {
  bgGrad(ctx);
  stars(ctx, t, 100, 8, 0.25);
  bgGlow(ctx, W * 0.5, H * 0.62, 900, PAL.blue, 0.07);
  caption(ctx, t, {
    kicker: 'MORE TO LOVE',
    title: '细节，都在',
    sub: '界面即播放器 · 每一处都为聆听调校',
  });

  const PX = [170, 990], PYs = [392, 726], PW = 760, PH = 292;
  const panelBase = (x, y, k) => {
    ctx.fillStyle = 'rgba(255,255,255,0.03)';
    ctx.strokeStyle = PAL.line + 'aa'; ctx.lineWidth = 1.5;
    rr(ctx, x, y, PW, PH, 18); ctx.fill(); ctx.stroke();
    ctx.fillStyle = 'rgba(120,150,220,0.06)';
    ctx.fillRect(x + 1, y + 1, PW - 2, 44);
  };

  // ============ P1 歌词逐行滚动 ============
  {
    const x = PX[0], y = PYs[0];
    const p = segE(t, 0.5, 1.3, eob);
    if (p > 0.001) {
      ctx.save();
      ctx.translate(x + PW / 2, y + PH / 2);
      ctx.scale(lerp(0.75, 1, p), lerp(0.75, 1, p));
      ctx.translate(-(x + PW / 2), -(y + PH / 2));
      alpha(ctx, clamp01(seg(t, 0.5, 1.0)));
      panelBase(x, y);
      const lines = ['霓虹慢慢落在窗台', '旧磁带还在轻轻转', '夜色摊开像慢镜头', '我们把白昼唱完了', '剩下的交给低频', '和一支不肯停的鼓'];
      font(ctx, 500, 20); ctx.fillStyle = PAL.dim;
      lsText(ctx, '歌词逐行跟随', x + 26, y + 30, '2px');
      const lh = 44, top = y + 84;
      const idx = Math.max(0, (t - 1.6)) / 1.25;
      const cur = Math.floor(idx) % lines.length;
      const fr = eio(idx - Math.floor(idx));
      const off = -(cur * lh + fr * lh) + lh * 1.2;
      ctx.save();
      ctx.beginPath(); rr(ctx, x + 12, y + 52, PW - 24, PH - 66, 12); ctx.clip();
      lines.forEach((ln, i) => {
        const d = Math.min(1, Math.abs(i - cur) / 2.2);
        const yy = top + off + i * lh + 60;
        const active = i === cur;
        ctx.save();
        alpha(ctx, lerp(1, 0.28, d));
        if (active) {
          const g = GRAD(ctx, x + 30, 0, x + 30 + ctx.measureText(ln).width + 200, 0);
          font(ctx, 700, 32);
          ctx.fillStyle = '#ffffff';
          ctx.shadowColor = 'rgba(59,110,246,0.55)'; ctx.shadowBlur = 14;
          ctx.fillText(ln, x + 44, yy);
          ctx.shadowBlur = 0;
          ctx.fillStyle = PAL.blue;
          ctx.fillRect(x + 26, yy - 24, 4, 30);
        } else {
          font(ctx, 500, 26);
          ctx.fillStyle = PAL.muted;
          ctx.fillText(ln, x + 44, yy);
        }
        ctx.restore();
      });
      ctx.restore();
      // 进度线
      const prog = clamp01((t - 1.6) / 10);
      ctx.fillStyle = 'rgba(255,255,255,0.08)';
      ctx.fillRect(x + 26, y + PH - 22, PW - 52, 3);
      ctx.fillStyle = PAL.blue;
      ctx.fillRect(x + 26, y + PH - 22, (PW - 52) * prog, 3);
      ctx.restore();
    }
  }

  // ============ P2 20 段 PEQ ============
  {
    const x = PX[1], y = PYs[0];
    const p = segE(t, 1.3, 2.1, eob);
    if (p > 0.001) {
      ctx.save();
      ctx.translate(x + PW / 2, y + PH / 2);
      ctx.scale(lerp(0.75, 1, p), lerp(0.75, 1, p));
      ctx.translate(-(x + PW / 2), -(y + PH / 2));
      alpha(ctx, clamp01(seg(t, 1.3, 1.8)));
      panelBase(x, y);
      font(ctx, 500, 20); ctx.fillStyle = PAL.dim;
      lsText(ctx, '20 段参数 EQ · 拖拽即调', x + 26, y + 30, '2px');
      const gx0 = x + 40, gx1 = x + PW - 40, mid = y + 168, amp = 62;
      // 网格
      ctx.strokeStyle = 'rgba(120,150,220,0.12)'; ctx.lineWidth = 1;
      for (let gy = mid - amp; gy <= mid + amp + 1; gy += amp / 2) {
        ctx.beginPath(); ctx.moveTo(gx0, gy); ctx.lineTo(gx1, gy); ctx.stroke();
      }
      for (let i = 0; i <= 10; i++) {
        const gx = lerp(gx0, gx1, i / 10);
        ctx.beginPath(); ctx.moveTo(gx, mid - amp - 8); ctx.lineTo(gx, mid + amp + 8); ctx.stroke();
      }
      // 曲线：中心随时间游走的高斯凸起
      const cxN = (Math.sin(t * 0.55) + 1) / 2 * 19;
      const height = Math.sin(t * 0.8) * 0.8 + 0.4;
      const nodes = [];
      for (let i = 0; i < 20; i++) {
        const bump = Math.exp(-Math.pow(i - cxN, 2) / 5.5);
        nodes.push(mid - bump * amp * height);
      }
      ctx.strokeStyle = PAL.green; ctx.lineWidth = 3;
      ctx.shadowColor = PAL.green; ctx.shadowBlur = 12;
      ctx.beginPath();
      nodes.forEach((ny, i) => {
        const nx = lerp(gx0, gx1, i / 19);
        i === 0 ? ctx.moveTo(nx, ny) : ctx.lineTo(nx, ny);
      });
      ctx.stroke();
      ctx.shadowBlur = 0;
      // 节点
      nodes.forEach((ny, i) => {
        const nx = lerp(gx0, gx1, i / 19);
        const near = Math.exp(-Math.pow(i - cxN, 2) / 5.5) > 0.55;
        ctx.fillStyle = near ? '#fff' : PAL.green;
        ctx.beginPath(); ctx.arc(nx, ny, near ? 5.5 : 3.5, 0, 7); ctx.fill();
      });
      // 右上角 pill
      font(ctx, 600, 19);
      pill(ctx, x + PW - 218, y + 24, '耳机校准预设', { fs: 19, color: PAL.gold, h: 30 });
      ctx.restore();
    }
  }

  // ============ P3 UPnP 投放 ============
  {
    const x = PX[0], y = PYs[1];
    const p = segE(t, 2.1, 2.9, eob);
    if (p > 0.001) {
      ctx.save();
      ctx.translate(x + PW / 2, y + PH / 2);
      ctx.scale(lerp(0.75, 1, p), lerp(0.75, 1, p));
      ctx.translate(-(x + PW / 2), -(y + PH / 2));
      alpha(ctx, clamp01(seg(t, 2.1, 2.6)));
      panelBase(x, y);
      font(ctx, 500, 20); ctx.fillStyle = PAL.dim;
      lsText(ctx, 'UPnP / DLNA 一键投放', x + 26, y + 30, '2px');
      const src = { x: x + 130, y: y + 168 };
      const dev = [
        { n: '客厅数播', x: x + 560, y: y + 92 },
        { n: '卧室一体机', x: x + 560, y: y + 168 },
        { n: '书房 DAC', x: x + 560, y: y + 244 },
      ];
      const act = Math.floor(Math.max(0, t - 3.2) / 1.7) % dev.length;
      // 播放器节点
      const g = GRAD(ctx, src.x - 34, src.y - 34, src.x + 34, src.y + 34);
      ctx.fillStyle = g;
      rr(ctx, src.x - 34, src.y - 34, 68, 68, 16); ctx.fill();
      font(ctx, 800, 34); ctx.fillStyle = '#fff'; ctx.textAlign = 'center';
      ctx.fillText('R', src.x, src.y + 12);
      ctx.textAlign = 'left';
      font(ctx, 600, 19); ctx.fillStyle = PAL.muted;
      ctx.fillText('本机播放器', src.x - 46, src.y + 62);
      // 设备节点与流
      dev.forEach((d, i) => {
        const on = i === act;
        ctx.strokeStyle = on ? PAL.green : PAL.line; ctx.lineWidth = 1.8;
        ctx.fillStyle = on ? 'rgba(52,211,153,0.10)' : 'rgba(255,255,255,0.03)';
        rr(ctx, d.x - 96, d.y - 30, 220, 60, 12); ctx.fill(); ctx.stroke();
        // 喇叭图形
        ctx.strokeStyle = on ? PAL.green : PAL.dim; ctx.lineWidth = 2;
        ctx.beginPath();
        ctx.moveTo(d.x - 76, d.y - 8); ctx.lineTo(d.x - 64, d.y - 8); ctx.lineTo(d.x - 64, d.y - 16);
        ctx.lineTo(d.x - 50, d.y); ctx.lineTo(d.x - 64, d.y + 16); ctx.lineTo(d.x - 64, d.y + 8);
        ctx.lineTo(d.x - 76, d.y + 8); ctx.closePath(); ctx.stroke();
        font(ctx, 600, 20);
        ctx.fillStyle = on ? '#eaf6ef' : PAL.muted;
        ctx.fillText(d.n, d.x - 34, d.y + 7);
        if (on) glowDot(ctx, d.x + 100, d.y - 14, 4, PAL.green, 0.5 + 0.5 * Math.sin(t * 6));
        // 音频包
        if (on && t > 3.2) {
          const per2 = 0.9;
          for (let j = 0; j < 2; j++) {
            const ph = (t / per2 + j * 0.5) % 1;
            const px = lerp(src.x + 44, d.x - 102, ph);
            const py = lerp(src.y, d.y, ph) - Math.sin(ph * Math.PI) * 46;
            glowDot(ctx, px, py, 4.5, PAL.cyan, Math.sin(ph * Math.PI));
          }
        }
      });
      ctx.restore();
    }
  }

  // ============ P4 托盘常驻 + 三主题 ============
  {
    const x = PX[1], y = PYs[1];
    const p = segE(t, 2.9, 3.7, eob);
    if (p > 0.001) {
      ctx.save();
      ctx.translate(x + PW / 2, y + PH / 2);
      ctx.scale(lerp(0.75, 1, p), lerp(0.75, 1, p));
      ctx.translate(-(x + PW / 2), -(y + PH / 2));
      alpha(ctx, clamp01(seg(t, 2.9, 3.4)));
      panelBase(x, y);
      font(ctx, 500, 20); ctx.fillStyle = PAL.dim;
      lsText(ctx, '托盘常驻 · 三套主题', x + 26, y + 30, '2px');
      // 迷你窗口 → 收进托盘
      const shrink = segE(t, 3.8, 5.4, eio);
      const wx0 = x + 70, wy0 = y + 76;
      const wx = lerp(wx0, x + PW - 210, shrink), wy = lerp(wy0, y + PH - 64, shrink);
      const ww = lerp(330, 46, shrink), wh = lerp(200, 40, shrink);
      ctx.fillStyle = 'rgba(16,24,35,0.95)';
      ctx.strokeStyle = PAL.line; ctx.lineWidth = 1.5;
      rr(ctx, wx, wy, ww, wh, lerp(12, 10, shrink)); ctx.fill(); ctx.stroke();
      if (shrink < 0.95) {
        ctx.fillStyle = 'rgba(120,150,220,0.15)';
        ctx.fillRect(wx + 1, wy + 1, ww - 2, 26 * (1 - shrink) + 6);
        // 关闭按钮 → 托盘箭头
        const bx = wx + ww - 22, by = wy + 15;
        ctx.strokeStyle = PAL.muted; ctx.lineWidth = 2;
        ctx.beginPath(); ctx.moveTo(bx - 5, by - 5); ctx.lineTo(bx + 5, by + 5);
        ctx.moveTo(bx + 5, by - 5); ctx.lineTo(bx - 5, by + 5); ctx.stroke();
      }
      // 托盘图标（收起后脉冲）
      if (shrink > 0.35) {
        const ix = x + PW - 122, iy = y + PH - 44;
        const g = GRAD(ctx, ix - 14, iy - 14, ix + 14, iy + 14);
        alpha(ctx, segE(t, 4.6, 5.4));
        ctx.fillStyle = g;
        rr(ctx, ix - 14, iy - 14, 28, 28, 8); ctx.fill();
        font(ctx, 800, 17); ctx.fillStyle = '#fff'; ctx.textAlign = 'center';
        ctx.fillText('R', ix, iy + 6); ctx.textAlign = 'left';
        const pulse = 1 + 0.16 * Math.sin(t * 4);
        ctx.strokeStyle = 'rgba(59,110,246,0.5)'; ctx.lineWidth = 2;
        rr(ctx, ix - 14 * pulse, iy - 14 * pulse, 28 * pulse, 28 * pulse, 8); ctx.stroke();
        alpha(ctx, 1);
        font(ctx, 500, 18); ctx.fillStyle = PAL.muted;
        ctx.fillText('点 X 不退出 · 常驻后台', x + PW - 210, y + PH - 66);
      }
      // 三主题色卡
      const themes = [
        { n: '深色', c: '#0b0f16', b: '#2b3547' },
        { n: '浅色', c: '#eef1f6', b: '#c9d2e2' },
        { n: '蔚蓝', c: '#dceaf7', b: '#8fb8e8' },
      ];
      const thAct = Math.floor(Math.max(0, t - 5.6) / 1.4) % 3;
      themes.forEach((th, i) => {
        const tx = x + 70 + i * 130, ty = y + 96;
        const on = i === thAct;
        ctx.fillStyle = th.c;
        ctx.strokeStyle = on ? PAL.blue : th.b; ctx.lineWidth = on ? 2.5 : 1.5;
        rr(ctx, tx, ty, 104, 66, 10); ctx.fill(); ctx.stroke();
        font(ctx, 600, 18);
        ctx.fillStyle = i === 0 ? '#c4ccdd' : '#37424f';
        ctx.fillText(th.n, tx + 12, ty + 40);
        // 迷你频谱装饰
        for (let b = 0; b < 5; b++) {
          const bh = 6 + (hash(i * 9 + b) * 14 + Math.sin(t * 3 + b + i * 3) * 3);
          ctx.fillStyle = on ? PAL.blue : (i === 0 ? '#39445a' : '#9fb0c6');
          ctx.fillRect(tx + 62 + b * 7, ty + 48 - bh, 4, bh);
        }
      });
      ctx.restore();
    }
  }
});
