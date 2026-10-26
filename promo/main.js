'use strict';
// main.js — 时间轴组装与启动
// 调试：intro.html?t=12.5 渲染单帧；intro.html?play=1&t=0 实时预览
// 截帧管线：capture.js 通过 CDP 反复调用 window.seek(t)，不要自动播放
buildTimeline();
(() => {
  const p = new URLSearchParams(location.search);
  if (p.has('play')) { play(parseFloat(p.get('t') || '0')); }
  else { seek(p.has('t') ? parseFloat(p.get('t')) : 0); }
})();
