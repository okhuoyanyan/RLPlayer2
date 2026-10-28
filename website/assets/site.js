/* RLPlayer 2.0 官网 —— 轻量交互（无框架、无依赖）
   1) 滚动淡入：进入视口一次性触发
   2) 卡片光晕：跟随鼠标位置
   3) 数字滚动：整数从 0 计到目标值
   4) 信号链：进入视口后点亮流向 */

(function () {
  'use strict';

  var reduce = window.matchMedia && window.matchMedia('(prefers-reduced-motion: reduce)').matches;

  /* ---------- 1. 滚动淡入 ---------- */
  var reveals = document.querySelectorAll('.reveal');
  if (!reduce && 'IntersectionObserver' in window) {
    var io = new IntersectionObserver(function (entries) {
      entries.forEach(function (e) {
        if (e.isIntersecting) {
          e.target.classList.add('in');
          io.unobserve(e.target);
        }
      });
    }, { rootMargin: '0px 0px -12% 0px', threshold: 0.06 });
    reveals.forEach(function (el) { io.observe(el); });
  } else {
    reveals.forEach(function (el) { el.classList.add('in'); });
  }

  /* ---------- 2. 卡片光晕跟随鼠标 ---------- */
  if (!reduce && window.matchMedia('(hover: hover)').matches) {
    document.querySelectorAll('.card').forEach(function (card) {
      card.addEventListener('mousemove', function (ev) {
        var r = card.getBoundingClientRect();
        card.style.setProperty('--mx', (ev.clientX - r.left) + 'px');
        card.style.setProperty('--my', (ev.clientY - r.top) + 'px');
      });
    });
  }

  /* ---------- 3. 数字滚动 ---------- */
  function countUp(el) {
    var target = parseFloat(el.getAttribute('data-to'));
    if (isNaN(target)) return;
    if (reduce) { el.textContent = String(target); return; }
    var dur = 1100, t0 = null;
    function step(ts) {
      if (!t0) t0 = ts;
      var p = Math.min((ts - t0) / dur, 1);
      // easeOutExpo
      var e = p === 1 ? 1 : 1 - Math.pow(2, -10 * p);
      el.textContent = String(Math.round(target * e));
      if (p < 1) requestAnimationFrame(step);
      else el.textContent = String(target);
    }
    requestAnimationFrame(step);
  }

  var nums = document.querySelectorAll('[data-to]');
  if (nums.length && 'IntersectionObserver' in window) {
    var jc = 0;
    var io2 = new IntersectionObserver(function (entries) {
      entries.forEach(function (e) {
        if (!e.isIntersecting) return;
        var el = e.target;
        io2.unobserve(el);
        jc = (jc + 1) % 4;
        setTimeout(function () { countUp(el); }, jc * 90);
      });
    }, { threshold: 0.4 });
    nums.forEach(function (el) { io2.observe(el); });
  }

  /* ---------- 4. 信号链点亮 ---------- */
  var chain = document.querySelector('.chain svg');
  if (chain && 'IntersectionObserver' in window) {
    var io3 = new IntersectionObserver(function (entries) {
      entries.forEach(function (e) {
        if (e.isIntersecting) { chain.classList.add('lit'); io3.unobserve(chain); }
      });
    }, { threshold: 0.25 });
    io3.observe(chain);
  }

  /* ---------- 5. 顶栏阴影（滚动后加深） ---------- */
  var nav = document.querySelector('header.nav');
  if (nav) {
    var onScroll = function () {
      if (window.scrollY > 8) nav.classList.add('scrolled');
      else nav.classList.remove('scrolled');
    };
    window.addEventListener('scroll', onScroll, { passive: true });
    onScroll();
  }
  /* FALLBACK：无论观察器是否触发，1.2 秒后一律显示，绝不留白页 */
  setTimeout(function () {
    document.querySelectorAll(".reveal:not(.in)").forEach(function (el) { el.classList.add("in"); });
  }, 1200);
})();