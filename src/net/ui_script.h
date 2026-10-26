// ui_script.h — 界面脚本（以 C++ 原始字符串承载，零转义）
//
// 视图：专辑 / 艺术家 / 曲目 / 文件夹 / 流媒体 / 队列 / 设置
// 原则：以专辑为中心；搜索是全局的；任何一次点击都不应该让界面失去上下文。
#pragma once

namespace rta::net {

inline const char* rta_ui_script() {
    return R"JSEOF(
var D = function (i) { return document.getElementById(i); };
var WV = (window.chrome && window.chrome.webview) ? window.chrome.webview : null;
if (WV) document.documentElement.classList.add('webview2');
function wcmd(c) { try { if (WV) WV.postMessage(c); } catch (e) { } }
// 261024-02 #2：资料里的 URL 一律交给**系统浏览器**打开（绝不把 WebView 自己导航走）。
// 协议白名单只放 http/https（file: / javascript: / data: 一律拒绝，避免本机文件被页面打开）。
// 宿主（launcher）收到 'openurl:<url>' 后 ShellExecuteW 打开；浏览器直开（没有 WebView2）时 window.open 兜底。
function extUrlAllowed(u) {
  return /^https?:\/\/[^\s]+$/i.test(String(u || ''));
}
function openExternal(u) {
  u = String(u || '').trim();
  if (!extUrlAllowed(u)) { toast('不支持的链接（只允许 http / https）'); return false; }
  if (WV) { wcmd('openurl:' + u); toast('已在系统浏览器打开'); }
  else {
    try { window.open(u, '_blank', 'noopener'); toast('已在浏览器新标签打开'); }
    catch (e) { toast('打开失败：' + u); }
  }
  return true;
}
var S = {
  view: 'albums', albums: [], album: null, files: [], remote: [],
  q: { items: [], index: 0 }, cur: '', playing: false, paused: false,
  // 261026-08 #5/#6/#3：我的最爱 / 保存的队列 / 播放列表 的视图状态
  favs: [], sqs: [], sqName: '', pls: [], plOpen: '', plItems: {},
  vol: 0, loud: false, lastCur: '', roots: [], scanning: false, dur: 0, pos: 0,
  volDb: 0, startDb: 0, theme: 'dark', queueHidden: false,
  // 261024-04 #8：GUP 相关的两个运行时状态（gpuOn = 有 GPU；gpuUsed = 本次播放真的用了 GPU）
  gpuOn: false, gpuUsed: false
};
// 高级参数（设置页里改，params() 统一带上）。默认值与后端一致。
var ADV = {
  device: '', bits: 0, taps: 0, atten: 0, transition: 0, phase: '',
  modcut: 0, moddith: -1, ir: '', irgpu: 0, ramp: 0, theme: 'dark',
  eq: '', modorder: 5, modfamily: 'dsd', gpuvendor: 'auto', directsdm: 0, dsdKind: 'fir', dsdConv: 'narrow',
  // 输出后端与多声道
  // 261024-01 #9/#11：upnpDev = UPnP/DLNA 渲染器（设备描述 URL）；naaHost 自动发现填，
  // 手动框只是折叠兜底（用户原话：NAA 端点这个选项不应该出现，应该自动获取）。
  backend: 'wasapi', channels: 0, naaHost: '127.0.0.1:43210', upnpDev: '',
  trim0: 0, trim1: 0, trim2: 0, trim3: 0, trim4: 0, trim5: 0, trim6: 0, trim7: 0,
  // 输入
  inBackend: 'none', inDev: '', inOffset: 0, inBuf: 100, inMonitor: 0, inSeconds: 0, inOut: '',
  // SDM（DSD 输出）专用滤波器：空 = 沿用 PCM 页那一组
  sdmFilt: '', sdmFiltNx: '',
  // ReplayGain（261027-01 #2）：默认 off = 逐位直通；rgclip 默认开（防削波）
  rgmode: 'off', rgclip: 1
};
// 元数据补全开关（P1.5 注册表化）：总开关 metaon 与 DeepSeek metallm 固定两键；
// 数据源开关按 /api/providers（后端注册表）动态渲染，写 metasrc_<id> ——
// 后端新增数据源 = 注册一个描述符，前端零改动。缺省全开，与后端缺省一致。
// summary = 设置开关（/api/settings 的 metasummary，改动即时生效）；
// hasLlmKey / summaryEnabled = /api/status 回报的权威状态（是否配了 key、后端是否开着开关）。
// 简介板块的显示门控用这两个：没 key 又没缓存时连加载行都不显示（用户 261023-03 #5）。
var META = { on: 1, llm: 1, summary: 0, hasLlmKey: null, summaryEnabled: null };
var META_PROVIDERS = null;   // /api/providers 的 items（启动时拉取；null = 还没到）
var META_SRC = {};           // id → 0/1（源开关状态，随 /api/providers 到达而填充）
var SEP = String.fromCharCode(92);

// 先按本地记忆应用主题（不等 /api/settings 返回，避免启动瞬间闪一下）
(function () {
  var t = null;
  try { t = localStorage.getItem('rta-theme'); } catch (e) { }
  if (t === 'light' || t === 'blue') document.documentElement.dataset.theme = t;
})();

function toast(m) {
  var t = D('toast'); if (!t) return;
  t.textContent = m; t.classList.add('on');
  clearTimeout(window._tt); window._tt = setTimeout(function () { t.classList.remove('on'); }, 2000);
}
// 261024-02 #3：长时长格式（专辑总时长用 1:05:24，单曲用 4:24；都用本地曲库真实数据，不用 AI 猜）
function fmtDurLong(x) {
  x = Math.max(0, Math.round(x || 0));
  var h = Math.floor(x / 3600), m = Math.floor((x % 3600) / 60), s = x % 60;
  var ss = (s < 10 ? '0' : '') + s;
  if (h > 0) return h + ':' + (m < 10 ? '0' : '') + m + ':' + ss;
  return m + ':' + ss;
}
function fmt(x) {
  x = Math.max(0, Math.floor(x || 0));
  var h = Math.floor(x / 3600), m = Math.floor((x % 3600) / 60), s = x % 60;
  if (h > 0) return h + ':' + (m < 10 ? '0' : '') + m + ':' + (s < 10 ? '0' : '') + s;
  return m + ':' + (s < 10 ? '0' : '') + s;
}
function esc(s) {
  return String(s === undefined || s === null ? '' : s)
    .replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;')
    .replace(/"/g, '&quot;').replace(/'/g, '&#39;');
}
function fmtDate(sec) {
  if (!sec) return '';
  var d = new Date(sec * 1000);
  if (isNaN(d.getTime())) return '';
  var y = d.getFullYear();
  var m = ('0' + (d.getMonth() + 1)).slice(-2);
  var day = ('0' + d.getDate()).slice(-2);
  return y + '-' + m + '-' + day;
}
// 从完整路径取「所在文件夹名」（队列列表用，比整条路径可读；评审 #8）
function parentFolder(p) {
  var s = String(p || '').replace(/\\\\/g, '/');
  var segs = s.split('/').filter(function (x) { return !!x; });
  return segs.length >= 2 ? segs[segs.length - 2] : '';
}
function nam(p) {
  var s = String(p || ''), i = Math.max(s.lastIndexOf('/'), s.lastIndexOf(SEP));
  return i >= 0 ? s.slice(i + 1) : s;
}
function base(p) { return String(p || '').replace(/[/\\]+$/, ''); }
function join(a, b) { return base(a) + SEP + b; }
window.addEventListener('unhandledrejection', function (ev) {
  if (ev && ev.preventDefault) ev.preventDefault();
});

function apiRaw(u, body) {
  // 2026-10-25（安全加固后）：/api/ 下的一律用 POST —— 后端现在把副作用端点限制为 POST
  // （GET 一律 405，防 <img src="/api/quit"> 这类 CSRF）。读端点 POST 也照常工作（handler 不看 method），
  // 所以这里统一 POST 既修好「无 body 的 api('/api/fav?...') 变成 GET 被拒」，也不用逐个调用点改。
  var isApi = u.indexOf('/api/') === 0;
  var opt = (body === undefined && !isApi) ? {} : { method: 'POST' };
  if (body !== undefined) opt.body = body;
  var ctrl = (typeof AbortController !== 'undefined') ? new AbortController() : null;
  if (ctrl) opt.signal = ctrl.signal;
  var timer = setTimeout(function () { if (ctrl) ctrl.abort(); }, 8000);
  return fetch(u, opt).then(function (r) {
    clearTimeout(timer);
    if (!r.ok) {
      return r.text().then(function (txt) {
        var msg = '';
        try {
          var j = JSON.parse(txt);
          if (j && j.error) msg = j.error;
          else if (j && j.message) msg = j.message;
        } catch (e) {
          msg = txt;
        }
        if (!msg) msg = 'HTTP ' + r.status;
        var err = new Error(msg);
        err.status = r.status;
        throw err;
      }, function () {
        var err = new Error('HTTP ' + r.status);
        err.status = r.status;
        throw err;
      });
    }
    return r.json();
  }, function (e) {
    clearTimeout(timer);
    throw e;
  });
}

function api(u, body) {
  return apiRaw(u, body).catch(function (err) {
    if (err && err.status && err.status >= 400 && err.status < 500) {
      throw err;
    }
    return new Promise(function (res) { setTimeout(res, 200); }).then(function () {
      return apiRaw(u, body);
    });
  });
}

function renderPanelError(viewName, retryFn, err) {
  if (S.view !== viewName) return;
  var h = '<div class="card" style="max-width:480px;width:100%;margin:clamp(24px,10vh,90px) auto 0;padding:24px 26px;background:var(--phbg);border:1px solid var(--line);border-radius:12px;text-align:center">' +
          '<div style="font-size:14px;font-weight:650;margin-bottom:6px">加载失败</div>' +
          '<div style="color:var(--mut);font-size:12.5px;line-height:1.6;margin-bottom:14px">网络异常或服务响应超时，点击下方按钮重试</div>' +
          '<button class="btn pri" id=panelRetryBtn>点击重试</button></div>';
  D('list').innerHTML = h;
  if (D('panelRetryBtn')) {
    D('panelRetryBtn').onclick = function () {
      if (typeof retryFn === 'function') retryFn();
    };
  }
}
function params() {
  var dv = D('sDSD').value;              // 0 | dop64 | dop128 | dop256 | nat64 | nat128 | nat256
  var dsdN = 0, native = 0;
  if (dv.indexOf('nat') === 0) { native = 1; dsdN = parseInt(dv.slice(3), 10) || 0; }
  else if (dv.indexOf('dop') === 0) { dsdN = parseInt(dv.slice(3), 10) || 0; }
  else dsdN = parseInt(dv, 10) || 0;
  return 'dsd=' + dsdN + '&dsdnative=' + native +
         '&gpuvendor=' + ((D('sGpu') && D('sGpu').value) || 'auto') +
         '&rate=' + D('sRate').value +
         '&filter=' + encodeURIComponent(D('sFilt').value) +
         '&filterNx=' + encodeURIComponent(D('sFiltNx').value) +
         '&shaper=' + encodeURIComponent(D('sShap').value) +
         '&volume=' + S.vol +
         '&loudness=' + (S.loud ? 1 : 0) +
         // ReplayGain：off / track / album + 防削波（后端 decide() → cfg.replayGainDb）
         '&rgmode=' + (ADV.rgmode || 'off') + '&rgclip=' + (ADV.rgclip ? 1 : 0) +
         '&isp=' + (D('sIsp').checked ? 1 : 0) +
         '&twostage=' + (D('s2s').checked ? 1 : 0) +
         '&buffer=' + D('sBuf').value +
         '&modorder=' + (ADV.modorder || 5) +
          '&modfamily=' + encodeURIComponent(ADV.modfamily || 'dsd') +
         '&device=' + encodeURIComponent(ADV.device || '') +
         '&bits=' + (ADV.bits || 0) +
         // 2026-10-24（261024-02 #8）：源文件真实位深（bit-perfect 判据用；解码器容器位深不可靠）
         '&srcbits=' + ((S.curMeta && S.curMeta.bits) ? S.curMeta.bits : 0) +
         '&taps=' + (ADV.taps || 0) +
         '&atten=' + (ADV.atten || 0) +
         '&transition=' + (ADV.transition || 0) +
         '&phase=' + encodeURIComponent(ADV.phase || '') +
         '&modcut=' + (ADV.modcut || 0) +
         '&moddith=' + (ADV.moddith === undefined ? -1 : ADV.moddith) +
         '&eq=' + encodeURIComponent(ADV.eq || '') +
         '&ir=' + encodeURIComponent(ADV.ir || '') +
         '&irgpu=' + (ADV.irgpu || 0) +
         '&ramp=' + (ADV.ramp || 0) +
         '&backend=' + encodeURIComponent(ADV.backend || 'wasapi') +
         '&channels=' + (ADV.channels || 0) +
         '&naahost=' + encodeURIComponent(ADV.naaHost || '') +
         '&upnpdev=' + encodeURIComponent(ADV.upnpDev || '') +
         '&trim=' + [0, 1, 2, 3, 4, 5, 6, 7].map(function (i) { return ADV['trim' + i] || 0; }).join(',') +
         '&sdmfilt=' + encodeURIComponent(ADV.sdmFilt || '') +
         '&sdmfiltnx=' + encodeURIComponent(ADV.sdmFiltNx || '') +
         '&directsdm=' + (ADV.directsdm ? 1 : 0) +
         '&dsdkind=' + (ADV.dsdKind || 'fir') + '&dsdconv=' + (ADV.dsdConv || 'narrow');
}
/* ---------------- 主题 ---------------- */
function setTheme(t, persist) {
  if (t !== 'dark' && t !== 'light' && t !== 'blue') t = 'dark';
  document.documentElement.dataset.theme = t;
  S.theme = t; ADV.theme = t;
  Array.prototype.forEach.call(document.querySelectorAll('.sw'), function (e) {
    e.classList.toggle('on', e.dataset.theme === t);
  });
  if (D('sp_theme')) D('sp_theme').value = t;   // 设置页里的下拉框跟着同步
  // 标题栏配色跟着主题走（Edge 应用窗口用 theme-color 画标题栏）
  var tc = { dark: '#0c0e13', light: '#ffffff', blue: '#eef4fb' }[t] || '#0c0e13';
  var meta = document.querySelector('meta[name=theme-color]');
  if (!meta) { meta = document.createElement('meta'); meta.setAttribute('name', 'theme-color'); document.head.appendChild(meta); }
  meta.setAttribute('content', tc);
  wcmd('theme:' + t);                 // 原生边框/圆角立即跟随，不再等 1.5 秒轮询
  try { localStorage.setItem('rta-theme', t); } catch (e) { }
  if (persist !== false) saveSettings();
}
Array.prototype.forEach.call(document.querySelectorAll('.sw'), function (e) {
  e.onclick = function () { setTheme(e.dataset.theme); };
});

// ---------- 无边框窗口：拖动 / 最小化 / 最大化 / 关闭 ----------
(function bindWindowChrome() {
  if (!WV) return;
  var ICON_MAX = "<svg viewBox='0 0 24 24' fill='none' stroke='currentColor' stroke-width='2'><rect x='6' y='6' width='12' height='12' rx='1.5'/></svg>";
  var ICON_REST = "<svg viewBox='0 0 24 24' fill='none' stroke='currentColor' stroke-width='2'><rect x='8' y='8' width='10' height='10' rx='1.5'/><path d='M5 5h10v10'/></svg>";
  function setMaxIcon(isMax) {
    var b = D('wMax'); if (!b) return;
    b.innerHTML = isMax ? ICON_REST : ICON_MAX;
    b.title = isMax ? '还原' : '最大化';
  }
  WV.addEventListener('message', function (ev) {
    if (ev.data === 'max') setMaxIcon(true);
    else if (ev.data === 'normal') setMaxIcon(false);
  });
  var top = D('topbar');
  if (top) {
    top.addEventListener('mousedown', function (e) {
      if (e.button !== 0) return;
      if (e.target.closest('button,input,select,a,.search,.themes,.wctl')) return;
      wcmd('drag');
    });
    top.addEventListener('dblclick', function (e) {
      if (e.target.closest('button,input,select,a,.search,.themes,.wctl')) return;
      wcmd('max');
    });
  }
  // 2026-10-23 用户反馈「单曲播放页面无法拖动窗口」：npPage 是全屏覆盖层，把 topbar 的拖拽热区盖住了。
  // 这里给覆盖层自己的拖拽条（#npGrip）绑同一套行为：左键按下 → launcher 走 HTCAPTION 拖窗；双击 → 最大化切换。
  if (D('npGrip')) {
    D('npGrip').addEventListener('mousedown', function (e) {
      if (e.button !== 0) return;
      wcmd('drag');
    });
    D('npGrip').addEventListener('dblclick', function (e) {
      if (e.button !== 0) return;
      wcmd('max');
    });
  }
  if (D('wMin')) D('wMin').onclick = function () { wcmd('min'); };
  if (D('wMax')) D('wMax').onclick = function () { wcmd('max'); };
  if (D('wClose')) D('wClose').onclick = function () { wcmd('close'); };
})();

/* ---------------- 261024-04 #1：窗口边缘拉伸（页面发起 resize） ----------------
   为什么是页面发起：WebView2 的 3 个子窗口属于别的进程，宿主无法 subclass，鼠标事件全被它们吃掉，
   原生边缘 hit-test 收不到鼠标（10 轮都没修好）。所以改成：页面在窗口最外 5px 铺 8 条不可见热区，
   mousedown（左键）时 postMessage('resize:<方向>')，宿主走 WM_NCLBUTTONDOWN / HTLEFT 等。
   消息格式必须**逐字**一致（launcher.cpp 按前缀匹配）：
     resize:left | resize:right | resize:top | resize:bottom |
     resize:topleft | resize:topright | resize:bottomleft | resize:bottomright
   实现要点：
     ① 8 条 <i> 只负责光标形状与命中区域，判定统一在 document 的**捕获阶段**按坐标做——
        这样 CDP 里 dispatchEvent(new MouseEvent('mousedown',{clientX,clientY,bubbles:true}))（派给
        document，不经过元素命中）也能触发，与真实鼠标一致；
     ② 只在 html.webview2 下创建（浏览器直开时没有 chrome.webview，也不该有窗口边缘）；
     ③ 角落 10x10 优先于边（四条边各 5px），区域永远只在最外 5px ⟹ 顶栏按钮/单曲页控件不受影响；
     ④ stopPropagation：否则顶栏的拖拽处理（bindWindowChrome 的 'drag'）会跟着发一条，宿主会收到两条命令。 */
// 边缘热区带宽（逻辑 px）：判定带、光标带、以及"贴右缘滚动容器让出的宽度"必须是同一个值。
// CSS 侧走 var(--rzband,5px)（ui_page.h 的 #rzHot .rz-r / .rz-edge-inset），JS 启动时把它写到 :root，
// 这样三处不会再各写一个 5（历史上的 bug 之一就是判定 5px、光标带 3px 不一致）。
var RZ_BAND = 5;
var RZ_EDGES = [
  ['topleft', 'rz-tl'], ['topright', 'rz-tr'], ['bottomleft', 'rz-bl'], ['bottomright', 'rz-br'],
  ['left', 'rz-l'], ['right', 'rz-r'], ['top', 'rz-t'], ['bottom', 'rz-b']
];
function rzDirAt(x, y) {
  var W = window.innerWidth || document.documentElement.clientWidth || 0;
  var H = window.innerHeight || document.documentElement.clientHeight || 0;
  if (!W || !H) return '';
  var E = RZ_BAND, C = 10;                 // 边 = RZ_BAND（5px）、角落 10x10
  var CL = x <= C, CR = x >= W - C, CT = y <= C, CB = y >= H - C;
  if (CT && CL) return 'topleft';
  if (CT && CR) return 'topright';
  if (CB && CL) return 'bottomleft';
  if (CB && CR) return 'bottomright';
  if (x <= E) return 'left';
  if (x >= W - E) return 'right';
  if (y <= E) return 'top';
  if (y >= H - E) return 'bottom';
  return '';
}
(function bindResizeHotspots() {
  if (!WV) return;
  var box = document.createElement('div');
  box.id = 'rzHot';
  RZ_EDGES.forEach(function (d) {
    var i = document.createElement('i');
    i.className = d[1];
    i.setAttribute('data-rz', d[0]);
    box.appendChild(i);
  });
  document.body.appendChild(box);
  document.addEventListener('mousedown', function (e) {
    if (!e || (e.button !== 0 && e.button !== undefined && e.button !== null)) return;
    var d = rzDirAt(e.clientX, e.clientY);
    if (!d) return;
    wcmd('resize:' + d);      // ← 逐字格式，宿主按 'resize:' 前缀 + 方向解析
    e.preventDefault();       // 热区内不做拖拽 / 文本选择
    e.stopPropagation();      // 别让顶栏再发一条 'drag'
  }, true);
})();

/* ---------------- 261025 窗口右边缘：先让滚动条退出最外 5px ----------------
   为什么需要：右边缘的拉伸热区在最外 5px，而**滚动条也在这 5px 上**。原生滚动条的命中在浏览器
   内部（Chromium 的滚动条不是 DOM），鼠标落在滚动条上时：轻则"按下先滚一格再改窗口大小"（实测
   .main / .rbody 都如此），重则在某些运行时（overlay/fluent 滚动条）完全不给页面派发 mousedown ⟹
   窄窗（<1240px，主区滚动条正好贴右边）右边缘就拖不动，而左/上/下三边正常。
   修法：只对**贴住窗口右边缘的滚动容器**加 .rz-edge-inset（右侧 5px 透明边框，见 ui_page.h），
   滚动条整体退到 5px 内侧，最外 5px 永远是页面 ⟹ 永远是热区。不隐藏滚动条、不改外观、
   不动左/上/下三边（左侧容器的滚动条在自己右缘，不在窗口左缘）。
   探测口径：
     * 候选 = 计算样式 overflow-y 为 auto/scroll 的元素（跑一次全量扫描，之后靠 DOM/resize 事件复测）；
     * "贴右缘" = getBoundingClientRect().right >= innerWidth-1（加的是边框不是 margin，
       box-sizing:border-box 下外框不动 ⟹ 该判据不会自我反馈、不会抖动）；
     * 文档序处理（外层先于内层），getBoundingClientRect 强制布局，嵌套滚动容器不会重复加。
   触发点：加载、window.resize、click（视图切换多由点击驱动）、#app class 变化（noq 开关）。
   ⚠️ 已知边界：元素若本来就有**可见的**右边框，会被这条规则改成透明（目前贴右缘的只有
      .main / .rbody，两者都没有右边框；新增带右边框的贴边滚动容器时需要另想办法）。 */
var rzScrollers = [];                  // 候选滚动容器（扫描结果缓存）
function rzScanScrollers() {
  var out = [];
  var all = document.querySelectorAll('*');
  for (var i = 0; i < all.length; i++) {
    var el = all[i];
    if (el.id === 'rzHot' || (el.parentNode && el.parentNode.id === 'rzHot')) continue;
    var oy = '';
    try { oy = getComputedStyle(el).overflowY; } catch (e) { oy = ''; }
    if (oy !== 'auto' && oy !== 'scroll') continue;
    out.push(el);
  }
  rzScrollers = out;
}
function rzApplyEdgeInset() {
  var W = window.innerWidth || 0;
  if (!W) return;
  for (var i = 0; i < rzScrollers.length; i++) {
    var el = rzScrollers[i];
    if (!el.isConnected) continue;
    var r = el.getBoundingClientRect();
    var on = el.classList.contains('rz-edge-inset');
    var atEdge = (r.right >= W - 1) && r.width > 8 && r.height > 8;
    if (atEdge && !on) el.classList.add('rz-edge-inset');
    else if (!atEdge && on) el.classList.remove('rz-edge-inset');
  }
}
(function rzBindEdgeInset() {
  if (!WV) return;
  try { document.documentElement.style.setProperty('--rzband', RZ_BAND + 'px'); } catch (e) { }
  var tmr = 0;
  function later(rescan) {
    clearTimeout(tmr);
    tmr = setTimeout(function () {
      if (rescan) rzScanScrollers();
      rzApplyEdgeInset();
    }, 160);
  }
  window.addEventListener('resize', function () { later(false); });
  document.addEventListener('click', function () { later(false); }, true);
  // 结构变化（换视图/重渲染）才重扫候选；纯 class 变化用缓存即可（但 noq 会改变谁贴右缘，所以也复测）
  if (window.MutationObserver) {
    var mo = new MutationObserver(function (recs) {
      var structural = false;
      for (var i = 0; i < recs.length; i++) { if (recs[i].type !== 'attributes') { structural = true; break; } }
      later(structural);
    });
    mo.observe(document.body, { childList: true, subtree: true, attributes: true, attributeFilter: ['class'] });
  }
  rzScanScrollers();
  rzApplyEdgeInset();
  setTimeout(function () { rzScanScrollers(); rzApplyEdgeInset(); }, 300);
})();

/* ---------------- 261025 右边缘几何诊断（一行/几何变化，走宿主日志） ----------------
   「窄窗右边缘拖不动」这个 bug 已经错过两轮（261024-05 把热区收窄到 3px 也没用），
   原因都是**没人能量出滚动条到底占了哪几像素**。所以留下这一行诊断，字段口径：
     main / rbody = [left, right, offsetWidth-clientWidth, 是否真滚动, scrollTop, sbR]
       - offsetWidth-clientWidth = 右侧边框 + 竖向滚动条宽度（9px 来自全局 ::-webkit-scrollbar）
       - sbR = 滚动条自身的右缘（right 减去右边框）⟹ 修复生效时它应当 = 窗口宽 - 5
     hotR  = 右热区的左右边界（应覆盖 [W-5, W]）
     atR2  = (W-2, H/2) 处 elementFromPoint 命中谁
   只在几何/视图变化时发一条（去重），不刷屏；宿主对所有页面消息都会先打一行 'wvmsg: ...'。 */
(function rzDiag() {
  if (!WV) return;
  var last = '';
  function box(el) {
    if (!el) return '-';
    var r = el.getBoundingClientRect();
    var bw = 0;
    try { bw = parseFloat(getComputedStyle(el).borderRightWidth) || 0; } catch (e) { bw = 0; }
    return '[' + Math.round(r.left) + ',' + Math.round(r.right) + ',' + (el.offsetWidth - el.clientWidth) +
           ',' + (el.scrollHeight > el.clientHeight + 1 ? 1 : 0) + ',' + Math.round(el.scrollTop) +
           ',sbR=' + Math.round(r.right - bw) + ']';
  }
  function who() {
    var W = window.innerWidth, H = window.innerHeight, e = null;
    try { e = document.elementFromPoint(W - 2, Math.round(H / 2)); } catch (x) { return '-'; }
    if (!e) return 'null';
    var cn = (typeof e.className === 'string' && e.className) ? '.' + e.className.split(/\s+/)[0] : '';
    return e.tagName + (e.id ? '#' + e.id : '') + cn;
  }
  function tick() {
    var W = window.innerWidth, H = window.innerHeight;
    var app = D('app');
    var hot = document.querySelector('#rzHot .rz-r');
    var hr = hot ? (Math.round(hot.getBoundingClientRect().left) + '..' + Math.round(hot.getBoundingClientRect().right)) : '-';
    var s = 'rzdiag: W=' + W + ' H=' + H + ' dpr=' + (window.devicePixelRatio || 1) +
            ' view=' + (S.view || '-') + ' noq=' + ((app && app.classList.contains('noq')) ? 1 : 0) +
            ' main[l,r,sbw,ov]=' + box(document.querySelector('.main')) +
            ' rbody=' + box(document.querySelector('.rbody')) +
            ' hotR=' + hr + ' atR2=' + who();
    if (s === last) return;
    last = s;
    wcmd(s);
  }
  var tmr = 0;
  // 先套用一次内缩再量：报告里必须是**修复后**的几何（滚动条右缘 = W-5），否则日志会误导后来人
  function later() {
    clearTimeout(tmr);
    tmr = setTimeout(function () { rzApplyEdgeInset(); tick(); }, 260);
  }
  window.addEventListener('resize', later);
  document.addEventListener('click', later, true);
  setTimeout(function () { rzApplyEdgeInset(); tick(); }, 700);
})();

function fmtTag(sr, bits) {
  if (!sr) return '';
  return (sr % 1000 === 0 ? (sr / 1000) + 'k' : (Math.round(sr / 100) / 10) + 'k') + '/' + bits;
}

/* ---------------- 顶栏 / 工具区 ---------------- */
function head(title, subtitle, toolsHtml) {
  D('pth').textContent = title;
  D('pth').title = title;
  D('cnt').textContent = subtitle || '';
  D('cnt').title = subtitle || '';
  D('tools').innerHTML = toolsHtml || '';
}

/* ---------------- 专辑视图 ---------------- */
// 封面一律尝试加载：拿不到就让 img 自己消失，后面的首字母占位露出来。
// （早期用 cv 标记决定是否请求，结果「扫描时没识别出封面」就永远不显示了）
function albumCardHtml(a, i) {
  return '<div class=card data-i=' + i + '>' +
    // 2026-10-26（261026-02 #3）：封面端点现在**找不到就 404**（不再回 1x1 占位图），
    // 所以 onerror 必须是"隐藏 <img>、露出底下的首字母"，而不是让格子空着。
    '<div class=cv>' +
      '<div class=ph>' + esc((a.name || '?').slice(0, 1).toUpperCase()) + '</div>' +
      '<img src="' + (a.cvUrl || ('/api/lib/cover?k=' + a.k)) + '" loading=lazy alt="" onerror="this.style.display=\'none\'">' +
      '<div class=pl><svg width=13 height=13 viewBox="0 0 24 24" fill=#fff><path d="M8 5v14l11-7z"/></svg></div>' +
    '</div>' +
    '<div class=nm title="' + esc(a.name) + '">' + esc(a.name) + '</div>' +
    '<div class=ar title="' + esc(a.artist) + '">' + esc(a.artist || '未知艺术家') +
      (a.year ? '  \u00b7  ' + a.year : '') + (a.sr ? '  \u00b7  ' + fmtTag(a.sr, a.bits) : '') + '</div>' +
    '</div>';
}
// 2026-10-26（261026-02 #3）：封面抓取要有**看得见的进度**，并且进库页自动开跑。
// 后端：POST /api/lib/cover/prefetch（副作用）· GET /api/lib/cover/progress → {running,done,total,hit,miss,fail}
var COVP = { timer: 0 };
function startCoverPrefetch() {
  api('/api/lib/cover/prefetch', '').then(function (j) {
    if (j && j.ok && j.queued) toast('正在抓取封面：共 ' + j.queued + ' 张缺图');
    // 2026-10-26 (261026-05): always poll while the backend is running.
    // Previously polling only started when THIS call queued something, so a job started
    // elsewhere (prefetch?all=1, or a still-running batch) finished without repainting the grid,
    // leaving covers blank even though the server had them.
    if (j && j.ok && j.running) pollCoverProgress();
  }).catch(function () { });
}
function pollCoverProgress() {
  if (COVP.timer) return;
  COVP.timer = setInterval(function () {
    api('/api/lib/cover/progress').then(function (g) {
      if (!g || !g.ok) return;
      var el = D('covProg');
      if (el) { el.style.display = 'flex'; el.textContent = '封面 ' + (g.done || 0) + '/' + (g.total || 0) + '（命中 ' + (g.hit || 0) + '）'; }
      if (!g.running) {
        clearInterval(COVP.timer); COVP.timer = 0;
        if (el && g.done > 0) setTimeout(function () { el.style.display = 'none'; }, 2500);
        if (g.hit > 0) { toast('封面已补齐 ' + g.hit + ' 张'); loadAlbums(); }
      }
    }).catch(function () { });
  }, 700);
}

function loadAlbums(q, title) {
  S.view = 'albums';
  startCoverPrefetch();     // 进库页就开跑（没有缺图时后端立即返回 queued=0）
  head(title || '专辑', '加载中…', '');
  api('/api/lib/albums?q=' + encodeURIComponent(q || '')).then(function (j) {
    S.albums = j.albums || [];
    if (!S.albums.length) {
      head(title || '专辑', '0 张', '');
      var hasRoots = (S.roots && S.roots.length > 0);
      var emptyH = '<div class="empty-guide-card" id=emptyGuideCard style="max-width:520px;margin:clamp(30px,8vh,70px) auto;padding:32px 28px;background:var(--card);border:1px solid var(--line);border-radius:12px;text-align:center">' +
        '<div style="font-size:36px;margin-bottom:12px">&#127925;</div>' +
        '<div style="font-size:15px;font-weight:650;margin-bottom:8px">' +
        (hasRoots ? '已添加音乐目录，但未找到音频文件' : '还没有添加音乐目录') + '</div>' +
        '<div style="color:var(--mut2);font-size:12.5px;line-height:1.7;margin-bottom:20px">' +
        (hasRoots ? '当前目录中未发现可识别的音频文件，请确认音频格式或更换目录。' : 'RLPlayer 会自动为您建立高保真音乐库并整理专辑与元数据。<br>支持 FLAC、DSD (DSF/DFF)、WAV、ALAC、CUE 分轨等格式。') + '</div>' +
        '<div style="display:flex;justify-content:center;gap:12px">' +
        '<button class="btn pri" id=bEmptyAddRoot>📁 ' + (hasRoots ? '管理音乐目录' : '添加音乐目录') + '</button>' +
        '<button class="btn" id=bEmptyScan>🔄 ' + (hasRoots ? '重新扫描' : '扫描目录') + '</button>' +
        '</div></div>';
      D('list').innerHTML = emptyH;
      if (D('bEmptyAddRoot')) D('bEmptyAddRoot').onclick = function () { openSettingsTab('adv'); };
      if (D('bEmptyScan')) D('bEmptyScan').onclick = function () { D('bScan').click(); };
      return;
    }
    head(title || '专辑', S.albums.length + ' 张专辑', '');
    var h = '<div class=grid>';
    S.albums.forEach(function (a, i) {
      h += albumCardHtml(a, i);
    });
    h += '</div>';
    D('list').innerHTML = h;
    Array.prototype.forEach.call(D('list').querySelectorAll('.card'), function (e) {
      var a = S.albums[+e.dataset.i];
      e.onclick = function (ev) {
        if (ev.target.closest && ev.target.closest('.pl')) { playAlbum(a.k, 0); ev.stopPropagation(); return; }
        openAlbum(a.k);
      };
      e.oncontextmenu = function (ev) { ev.preventDefault(); enqueueAlbum(a.k); };
    });
  }).catch(function (e) {
    renderPanelError('albums', function () { loadAlbums(q, title); }, e);
  });
}

/* ---------------- 收藏（心形喜好标记 / 261023-03 第5条） ---------------- */
var FAV_SET = new Set();

function initFavs() {
  api('/api/fav/list').then(function (j) {
    if (j && j.ok && Array.isArray(j.files)) {
      FAV_SET = new Set(j.files);
      updateFavUi();
    }
  }).catch(function () {});
}

function favIconHtml(filePath, isFav, extraClass) {
  var onCls = isFav ? ' on' : '';
  var extCls = extraClass ? (' ' + extraClass) : '';
  return '<button class="fav-btn' + onCls + extCls + '" data-file="' + esc(filePath) + '" title="' + (isFav ? '取消收藏' : '收藏') + '">' +
         '<svg width=14 height=14 viewBox="0 0 24 24" fill="' + (isFav ? '#e05666' : 'none') + '" stroke="' + (isFav ? '#e05666' : 'currentColor') + '" stroke-width="2">' +
         '<path d="M20.84 4.61a5.5 5.5 0 00-7.78 0L12 5.67l-1.06-1.06a5.5 5.5 0 00-7.78 7.78l1.06 1.06L12 21.23l7.78-7.78 1.06-1.06a5.5 5.5 0 000-7.78z"/>' +
         '</svg></button>';
}

function toggleFav(filePath, ev) {
  if (ev) { ev.stopPropagation(); ev.preventDefault(); }
  if (!filePath) return;
  var isFav = FAV_SET.has(filePath);
  var nextOn = isFav ? 0 : 1;
  api('/api/fav?file=' + encodeURIComponent(filePath) + '&on=' + nextOn).then(function (j) {
    if (j && j.ok) {
      if (nextOn) {
        FAV_SET.add(filePath);
        toast('已添加到收藏');
      } else {
        FAV_SET.delete(filePath);
        toast('已取消收藏');
      }
      updateFavUi();
      if (S.view === 'tracks' && S.trackSort === 'fav') {
        renderTracksView();
      }
    }
  }).catch(function (e) {
    toast('收藏操作失败: ' + e);
  });
}

function updateFavUi() {
  Array.prototype.forEach.call(document.querySelectorAll('.fav-btn'), function (btn) {
    if (btn.id === 'npFavBtn') return;
    var f = btn.dataset.file;
    if (!f) return;
    var on = FAV_SET.has(f);
    btn.classList.toggle('on', on);
    var svg = btn.querySelector('svg');
    if (svg) {
      svg.setAttribute('fill', on ? '#e05666' : 'none');
      svg.setAttribute('stroke', on ? '#e05666' : 'currentColor');
    }
    btn.title = on ? '取消收藏' : '收藏';
  });
  // 同步单曲整页心形
  var npb = D('npFavBtn');
  if (npb) {
    if (S.lastCur) {
      npb.style.display = 'inline-flex';
      var curOn = FAV_SET.has(S.lastCur);
      npb.classList.toggle('on', curOn);
      var npsvg = npb.querySelector('svg');
      if (npsvg) {
        npsvg.setAttribute('fill', curOn ? '#e05666' : 'none');
        npsvg.setAttribute('stroke', curOn ? '#e05666' : 'currentColor');
      }
      npb.title = curOn ? '取消收藏' : '收藏';
    } else {
      npb.style.display = 'none';
    }
  }
}

/* ---- 结构化标签值网格（261024-01 #4）----
   后端契约（Lead）：/api/entity/summary 与 /api/entity/knowledge 的返回里带
     kind: 'track' | 'release' | 'person' | 'work'
     fields: { 标签: 值, ... }        // AI 只知道它能确认的；不知道的标签**根本不出现**
   前端职责：按实体类型排好序、长文本独占整行、空值一律不渲染（不占位、不写"未知"）。
   没有 fields（旧缓存/后端未升级）时自动退回 sections 段落渲染，绝不白屏。 */
// 每类实体「应该出现」的标签顺序（用户 #4 的思路 + 补全）。顺序 = 渲染顺序。
var EF_ORDER = {
  track: ['演唱', '作词', '作曲', '编曲', '歌曲风格', '发行时间', '所属专辑', '时长', '版本说明', '衍生作品', '歌词赏析'],
  person: ['简介', '国籍', '活跃年代', '风格', '代表作品', '厂牌', '合作'],
  release: ['艺术家', '发行时间', '专辑类型', '语种', '出版方', '专辑编号', '时长', '专辑介绍', '曲目列表', '制作人员', '关联'],
  work: ['作曲家', '创作年代', '体裁']
};
// 长文本标签：独占整行并保留换行（这些一定不是一格能装下的）
// 只有"一定是一整段/一整列"的标签才独占整行；代表作品/合作这类是短列表，留在网格里更紧凑（信息密度）。
var EF_WIDE = { '简介': 1, '专辑介绍': 1, '曲目列表': 1, '制作人员': 1, '歌词赏析': 1, '关联': 1, '衍生作品': 1, '版本说明': 1 };
function efValueText(v) {
  if (v === null || v === undefined) return '';
  if (typeof v === 'string') return v.replace(/\s+$/, '').replace(/^\s+/, '');
  if (typeof v === 'number' || typeof v === 'boolean') return String(v);
  if (Object.prototype.toString.call(v) === '[object Array]') {
    var out = [];
    for (var i = 0; i < v.length; ++i) { var s = efValueText(v[i]); if (s) out.push(s); }
    return out.join('\n');
  }
  try { return JSON.stringify(v); } catch (e) { return ''; }
}
// 261024-02 #3：本地曲库真实数据优先 —— opts.local = {标签: 值}（本地有就用本地的），
// opts.drop = {标签:1}（本地数据在页面上已经有专门的展示位置时，AI 那一项直接不渲染）。
// 值里的 http/https 自动变成可点链接（261024-02 #2：「关联」字段里的网址以前完全点不动）
function linkifyEscaped(v) {
  v = String(v == null ? '' : v);
  var re = /https?:\/\/[^\s<>"'）)】]+/g, out = '', last = 0, m;
  while ((m = re.exec(v))) {
    out += esc(v.slice(last, m.index));
    out += '<span class=eflink data-url="' + esc(m[0]) + '" role=link tabindex=0>' + esc(m[0]) + '</span>';
    last = m.index + m[0].length;
  }
  out += esc(v.slice(last));
  return out;
}
// 外链点击（事件委托，重绘后不需要重新绑定）
document.addEventListener('click', function (ev) {
  var t = ev.target;
  while (t && t !== document) {
    if (t.classList && t.classList.contains('eflink')) { ev.preventDefault(); openExternal(t.dataset.url); return; }
    t = t.parentNode;
  }
});
function renderFieldGrid(fields, kind, opts) {
  if (!fields || typeof fields !== 'object') return '';
  var skip = {};
  var sk = (opts && opts.skip) || {}, dr = (opts && opts.drop) || {};
  Object.keys(sk).forEach(function (k) { skip[k] = 1; });
  Object.keys(dr).forEach(function (k) { skip[k] = 1; });
  var local = (opts && opts.local) || {};
  var labels = [], seen = {};
  var order = EF_ORDER[kind] || [];
  function hasVal(lb) {
    if (local[lb] !== undefined && local[lb] !== null && local[lb] !== '') return true;
    return fields[lb] !== undefined && fields[lb] !== null;
  }
  order.forEach(function (lb) {
    if (hasVal(lb) && !skip[lb]) { labels.push(lb); seen[lb] = 1; }
  });
  Object.keys(fields).forEach(function (lb) {   // 契约外的标签也照渲染（前端不许丢后端给的内容）
    if (!seen[lb] && !skip[lb] && labels.indexOf(lb) < 0) labels.push(lb);
  });
  Object.keys(local).forEach(function (lb) {    // 本地独有（AI 没给的）标签也要出现
    if (!seen[lb] && !skip[lb] && labels.indexOf(lb) < 0) labels.push(lb);
  });
  var rows = '';
  labels.forEach(function (lb) {
    var v = efValueText(fields[lb]);
    if (local[lb] !== undefined && local[lb] !== null && local[lb] !== '') v = efValueText(local[lb]);
    if (!v) return;                              // 空值 = 后端不知道 ⟹ 不占位
    var wide = !!EF_WIDE[lb] || v.length > 46 || v.indexOf('\n') >= 0;
    rows += '<div class="efrow' + (wide ? ' wide' : '') + '">' +
            '<div class=lb>' + esc(lb) + '</div><div class=vl>' + linkifyEscaped(v) + '</div></div>';
  });
  // 261024-02 #3：曲目列表用本地曲库逐行渲染（序号 / 曲名 / 时长三列对齐），不再用 AI 的一行文本。
  // 只有调用方真的传了本地曲目（opts.tracklist）才渲染；否则保留 AI 的文本版（并在末尾标注 AI 推测）。
  var tl = (opts && opts.tracklist) || null;
  if (tl && tl.length) rows += efTracklistHtml(tl);
  return rows ? ('<div class=efields>' + rows + '</div>') : '';
}
// 261024-05 A12：曲目列表块单独成函数 —— 抽屉「专辑」标签要**先渲染本地曲目**，
// 不能让 AI 的 sections/文字内容挡住列表（旧实现只在有 fields 时才顺带渲染）。
function efTracklistHtml(tl) {
  if (!tl || !tl.length) return '';
  var tlh = '<div class="efrow wide"><div class=lb>曲目列表</div><div class=vl><div class=ef-tracklist>';
  tl.forEach(function (t, i) {
    var n = (typeof t.n === 'number' && t.n > 0) ? t.n : (i + 1);
    tlh += '<div class=eft-row><span class=eft-n>' + (n < 10 ? '0' : '') + n + '</span>' +
           '<span class=eft-t title="' + esc(t.title || '') + '">' + esc(t.title || '') + '</span>' +
           '<span class=eft-d>' + fmtDurLong(t.dur || 0) + '</span></div>';
  });
  tlh += '</div></div></div>';
  return tlh;
}
// 简介类区块的统一渲染：优先 fields 网格，兜底 sections 段落。
// title 非空时作为小标题（内容全是 AI 整理的板块一律传「AI 整理」，全站同一措辞 —— 261024-01 #2）。
function renderSummaryContent(summary, title, kind, opts) {
  if (!summary) return '';
  var sections = (summary.sections || []);
  var body = renderFieldGrid(summary.fields, (summary.kind || kind || ''), opts);
  if (!body) {
    if (!(summary.ok && sections.length)) return '';
    var citMap0 = {};
    (summary.citations || []).forEach(function (c, idx) { citMap0[c.id] = idx + 1; });
    var b0 = '';
    sections.forEach(function (sec) {
      b0 += '<div style="margin-bottom:10px">';
      if (sec.heading && sec.heading !== '概述' && sec.heading !== '简介') {
        b0 += '<div style="font-size:12.5px;font-weight:600;color:var(--txt);margin:6px 0 4px">' + esc(sec.heading) + '</div>';
      }
      (sec.paragraphs || []).forEach(function (p) {
        var sups = '';
        (p.citations || []).forEach(function (cid) {
          var found = citMap0[cid];
          if (found) sups += '<sup style="color:var(--mut);font-size:10px;margin-left:2px">[' + found + ']</sup>';
        });
        b0 += '<div style="font-size:12.5px;line-height:1.7;color:var(--txt);margin-bottom:6px">' + esc(p.text) + sups + '</div>';
      });
      b0 += '</div>';
    });
    body = b0;
  }
  if (!body) return '';
  var h = '';
  if (title) h += '<div style="font-size:12px;font-weight:600;letter-spacing:1px;color:var(--mut2);margin-bottom:8px">' + esc(title) + '</div>';
  h += body;
  if (summary.citations && summary.citations.length > 0) {
    h += '<div style="display:flex;gap:6px;flex-wrap:wrap;margin-top:10px">';
    summary.citations.forEach(function (c, idx) {
      var label = '[' + (idx + 1) + '] ' + (c.title || c.source || '来源');
      h += citationChipHtml(c.url, label, false);
    });
    h += '</div>';
  }
  // 说明行：不写「AI 生成」这类字眼（用户 261023-03 #2）。
  // 261024-01 #2：用户明确要求**只显示「AI 整理」四个字** ⟹ 旧的
  // 「AI 整理（未找到权威来源，仅供参考）」/「未核对 · 依据来源整理…」一律删掉；
  // 有标题的板块（标题就是「AI 整理」）不再重复；有真实来源时才留一行来源提示。
  if (!summary.unverified) {
    if (summary.citations && summary.citations.length > 0) {
      h += '<div style="font-size:11px;color:var(--mut2);margin-top:8px">依据上面的来源整理，可能不完整，请以来源为准</div>';
    }
  } else if (!title) {
    h += '<div style="font-size:10.5px;color:var(--mut2);letter-spacing:.6px;margin-top:10px">AI 整理</div>';
  }
  return h;
}

// 简介请求（2026-10-23 DEBUG）：后端把「未命中缓存」的生成放到了后台线程，首次请求立即回
// {"pending":true}，这里按 2s 轮询同一个 URL 直到出结果/超时。这样自动生成期间
// /api/status（400ms 轮询）与播放控制不会被单线程 HTTP 后端堵住（以前会整站卡死）。
// 261024-05 #T2：加 isWanted() —— 切歌/离开专辑页后**下一次轮询不再发出**，已发出的回来也丢弃
// （旧实现会把旧 key 的 pending 每 2s 继续打 60 次，正是"旧专辑任务带到下一张"的一部分）。
var SUMM_ABORT = { aborted: true };
function fetchSummary(url, onDone, tries, isWanted, onProg) {
  tries = tries || 0;
  if (isWanted && !isWanted()) { onDone(SUMM_ABORT); return; }
  api(url).then(function (j) {
    if (isWanted && !isWanted()) { onDone(SUMM_ABORT); return; }
    if (j && j.pending) {
      if (j.progress && onProg) {
        try { onProg(j.progress); } catch (e) { }
      }
      if (tries >= 60) { onDone({ ok: false, error: '资料整理超时' }); return; }
      setTimeout(function () { fetchSummary(url, onDone, tries + 1, isWanted, onProg); }, 2000);
      return;
    }
    onDone(j);
  }).catch(function (e) {
    if (isWanted && !isWanted()) { onDone(SUMM_ABORT); return; }
    onDone({ ok: false, error: String(e) });
  });
}

/* ---------------- 261024-04 #7：summary 的页面级缓存 + 单飞去重 ----------------
   用户原话：「只要打开了 AI 整理，进入页面就自动加载整理，不再需要点击」。
   要做到"自动"又不烧钱，必须保证**同一实体在页面生命周期内只请求一次**：
     * 同一 URL（kind + name + artist + lang）命中缓存 → 直接回调（异步回调，避免调用方在同步栈里重入）；
     * 正在飞行 → 挂到同一批回调上（不重复发请求）；
     * 成功/失败都缓存（失败不弹错，由调用方决定是否留一行"点击重试"）；
     * refresh=true 绕过缓存重新请求（用户明确点"重试/刷新"时才用）。 */
var SUMC = { cache: {}, inflight: {} };
function summHas(s) {
  return !!(s && ((s.fields && Object.keys(s.fields).length > 0) ||
                  (s.ok && s.sections && s.sections.length > 0)));
}
function summaryCached(url) {
  return Object.prototype.hasOwnProperty.call(SUMC.cache, url) ? SUMC.cache[url] : null;
}
function summaryOnce(url, cb, refresh, isWanted, onProg) {
  cb = cb || function () { };
  if (refresh) delete SUMC.cache[url];
  else if (Object.prototype.hasOwnProperty.call(SUMC.cache, url)) {
    var hit = SUMC.cache[url];
    setTimeout(function () { cb(hit); }, 0);
    return;
  }
  var q = SUMC.inflight[url];
  if (q) {
    q.push({ fn: cb, want: isWanted, prog: onProg });
    if (q.lastProgress && onProg) {
      var p0 = q.lastProgress;
      setTimeout(function () {
        if (!isWanted || isWanted()) {
          try { onProg(p0); } catch (e) { }
        }
      }, 0);
    }
    return;
  }
  q = SUMC.inflight[url] = [{ fn: cb, want: isWanted, prog: onProg }];
  // 261024-05 #T2：同一实体可能有多个等待者（抽屉 + 预取）。只要还有一个想要，轮询就继续；
  // 全部取消 → 立刻停（下一次 2s 轮询不再发出），结果也不进缓存。
  function stillWanted() {
    for (var i = 0; i < q.length; ++i) if (!q[i].want || q[i].want()) return true;
    return false;
  }
  fetchSummary(url, function (s) {
    delete SUMC.inflight[url];
    try { albPrefPump(); } catch (e) { }
    if (s === SUMM_ABORT) return;
    SUMC.cache[url] = s;
    for (var i = 0; i < q.length; ++i) {
      if (q[i].want && !q[i].want()) continue;
      try { q[i].fn(s); } catch (e) { }
    }
  }, 0, stillWanted, function (prog) {
    q.lastProgress = prog;
    for (var i = 0; i < q.length; ++i) {
      if (q[i].want && !q[i].want()) continue;
      if (q[i].prog) {
        try { q[i].prog(prog); } catch (e) { }
      }
    }
  });
}

// 261024-02 #2：引用 chip 拆成两个动作 —— 点 chip 本体 = 用系统浏览器打开；点右侧「复制」= 复制链接。
function citationChipHtml(url, label, small) {
  var fs = small ? 'font-size:10.5px;padding:1px 7px' : 'font-size:11px;padding:2px 8px';
  return '<span class="cite-chip" style="display:inline-flex;align-items:center;gap:0;' + fs + '">' +
         '<button class="btn chip-open" data-url="' + esc(url) + '" title="在系统浏览器打开：' + esc(url) + '" ' +
         'style="' + fs + ';border-top-right-radius:0;border-bottom-right-radius:0">' +
         '<svg width=11 height=11 viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" style="vertical-align:-1px;margin-right:4px">' +
         '<path d="M18 13v6a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2V8a2 2 0 0 1 2-2h6"/><path d="M15 3h6v6"/><path d="M10 14L21 3"/></svg>' +
         esc(label) + '</button>' +
         '<button class="btn chip-copy" data-url="' + esc(url) + '" title="复制链接" ' +
         'style="' + fs + ';border-top-left-radius:0;border-bottom-left-radius:0;border-left:0">复制</button>' +
         '</span>';
}
function bindCopyChips(container) {
  if (!container) return;
  Array.prototype.forEach.call(container.querySelectorAll('.chip-open'), function (btn) {
    btn.onclick = function (ev) { ev.stopPropagation(); openExternal(btn.dataset.url); };
  });
  Array.prototype.forEach.call(container.querySelectorAll('.chip-copy'), function (btn) {
    btn.onclick = function (ev) {
      ev.stopPropagation();
      var u = btn.dataset.url;
      if (!u) return;
      if (navigator.clipboard && navigator.clipboard.writeText) {
        navigator.clipboard.writeText(u).then(function () {
          toast('已复制来源链接');
        }).catch(function () {
          toast('已复制来源链接');
        });
      } else {
        toast('已复制来源链接');
      }
    };
  });
}

function syncAlbPlayAllBtn() {
  var b = D('albPlayAll');
  if (!b || S.view !== 'album' || !S.album) return;
  var alb = S.album;
  var playing = (S.playing || S.paused) && S.lastCur && (alb.tracks || []).some(function (t) { return t.path === S.lastCur; });
  if (playing) {
    if (S.playing && !S.paused) {
      b.textContent = '❚❚ 暂停';
      b.title = '暂停播放';
    } else {
      b.textContent = '▶ 继续播放';
      b.title = '继续播放专辑';
    }
  } else {
    b.textContent = '▶ 播放整张';
    b.title = '播放整张专辑';
  }
}

function openAlbum(k) {
  // 261024-05 #T2：打开另一张专辑 = 离开上一张 → 先停掉它的曲目预取队列（含在飞轮询）
  albPrefStop();
  S.view = 'album';
  api('/api/lib/album?k=' + encodeURIComponent(k)).then(function (j) {
    if (S.view !== 'album') return;   // 已切走 → 丢弃（否则会把别页内容覆写成专辑页）
    if (!j.ok) { toast(j.error || '打不开专辑'); return; }
    S.album = j;
    // 2026-10-26：封面 404 时不要留一块纯色空框 —— 用 <img> + 首字母底层（与列表页一致）
    var cv = "";
    var sub = [];
    if (j.artist) sub.push(esc(j.artist));
    if (j.year) sub.push(j.year);
    // 261024-02 #3：总时长 = 本地曲目时长之和（后端 /api/lib/album 的 dur 就是 Σ 本地曲目），
    // 用 1:05:24 形式；AI 不再参与时长计算。
    var localTotal = 0;
    (j.tracks || []).forEach(function (t) { localTotal += (t.dur || 0); });
    if (!localTotal) localTotal = j.dur || 0;
    sub.push(j.tracks.length + ' 首');
    sub.push(fmtDurLong(localTotal));
    var isThisPlaying = (S.playing || S.paused) && S.lastCur && (j.tracks || []).some(function (t) { return t.path === S.lastCur; });
    var albPlayText = isThisPlaying ? (S.paused ? '▶ 继续播放' : '❚❚ 暂停') : '▶ 播放整张';
    var albPlayTitle = isThisPlaying ? (S.paused ? '继续播放专辑' : '暂停播放') : '播放整张专辑';
    var h = '<div class=ahead><div class=cv style="position:relative">' +
            '<div class=ph>' + esc((j.name || '?').slice(0, 1).toUpperCase()) + '</div>' +
            '<img src="' + (j.cvUrl || ('/api/lib/cover?k=' + j.k)) + '" alt="" onerror="this.style.display=\'none\'" style="position:absolute;inset:0;width:100%;height:100%;object-fit:cover;border-radius:inherit">' +
            '</div><div class=info>' +
            '<div class=kind>ALBUM</div><h1 title="' + esc(j.name) + '">' + esc(j.name) + '</h1>' +
            '<div class=sub>' + sub.join(' \u00b7 ') + '</div>' +
            '<button class="btn pri" id=albPlayAll aria-label="播放整张专辑" title="' + albPlayTitle + '">' + albPlayText + '</button> ' +
            '<button class=btn id=aAdd>加入队列</button> ' +
            // 2026-10-26（261026-02 #3）：找不到封面的专辑（同人自出版，全网没有）允许用户自己挑一张图，
            // 存进 covercache（**不动音乐文件**）——这是把「99%」补到 100% 的人工通道。
            '<button class=btn id=aSetCover title="为这张专辑指定一张封面图（存到本地缓存，不改动音乐文件）">设置封面</button> ' +
            '<input type=file id=aCoverFile accept="image/*" style="display:none"> ' +
            '<button class=btn id=aShuf>随机播放</button>' +
            '</div></div>';

    // 2026-10-23（UI 评审 #2）：曲目列表必须在首屏可见 —— 以前简介（有时是很长的维基消歧义文）
    // 排在曲目之前，1024 下用户一首歌都看不到。现在先曲目、后简介。
    // 261024-02 #3：这张表就是**本地曲库的曲目列表**（行数 = 本地曲目数、逐行「曲名 + 时长」），
    // 加一行表头把"几首 / 总时长"讲清楚，避免和 AI 资料里的「时长」打架。
    // 261024-04 #7：专辑页顶部的「后台整理曲目资料」细进度条（不打扰；没开 AI 整理时是空 div）
    h += '<div id=albProgWrap></div>';
    // 261024-04 #4b：曲目列表左右各 22px，与 .ahead 对齐（以前 0 ⟹ 表头「曲目列表」贴着卡片左边）
    h += '<div style="padding:4px 22px 24px">';
    h += '<div class="trlist-head"><span class=lb>曲目列表</span><span class=sp></span>' +
         '<span class=sum>' + j.tracks.length + ' 首 · 总时长 ' + fmtDurLong(localTotal) + '</span></div>';
    j.tracks.forEach(function (t, i) {
      var isFav = FAV_SET.has(t.path);
      h += '<div class="trow trow-track" data-i=' + i + ' style="grid-template-columns:28px 24px minmax(120px,1.6fr) minmax(0,1fr) 92px 58px 26px;gap:8px">' +
           '<div class=n>' + (t.n || (i + 1)) + '</div>' +
           favIconHtml(t.path, isFav) +
           '<div class=tt title="' + esc(t.title) + '">' + esc(t.title) + '</div>' +
           '<div class=ar title="' + esc(t.artist || j.artist) + '">' + esc(t.artist || j.artist) + '</div>' +
           '<div class=bd>' + fmtTag(t.sr, t.bits) + '</div>' +
           '<div class=du>' + fmtDurLong(t.dur) + '</div>' +
           // 2026-10-26（261025-02 #3）：曲目行加一个**看得见**的「+」——
           // 以前专辑页只能"点行=播放整张"，没有任何单曲入队入口（右键也没处理），
           // 用户想在专辑里把某一首加进播放列表是做不到的。
           '<button class="add-btn" title="加入播放队列" aria-label="加入播放队列">+</button>' +
           '</div>';
    });
    h += '</div>';
    // 简介放在曲目之后（评审 #2）。
    // 261024-04 #4b：资料块左右各 22px（与 .ahead 对齐），正文内边距由 .sec-card（18px）提供。
    // 261024-04 #7：打开 AI 整理后**进入即自动加载**，不再需要点任何按钮。
    if (META.summary == 1) {
      h += '<div class="sec-card summary-sec" id="albumSummary" data-k="' + esc(j.k) + '"' +
           ' style="margin:8px 22px 24px">' +
           '<div style="font-size:12px;color:var(--mut2)">正在整理资料…</div>' +
           '</div>';
    }
    D('list').innerHTML = h;
    head(j.name, j.tracks.length + ' 首', '<button class=btn id=bBackAlbums title="返回专辑列表" aria-label="返回专辑列表">← 返回专辑</button>');
    if (D('bBackAlbums')) D('bBackAlbums').onclick = function () { loadAlbums(); };

    if (META.summary == 1) {
      var albK = String(j.k);
      // 261024-01 #4：后端现在回结构化 fields（可能没有 sections）⟹ 判据用"能渲染出东西"
      // #3：本地曲库是时长的唯一真相源；AI 的「时长」「曲目列表」一律不采信
      var albOpts = { local: { '时长': fmtDurLong(localTotal) }, drop: { '曲目列表': 1 } };
      var albUrl = '/api/entity/summary?kind=release&name=' + encodeURIComponent(j.name) +
                   '&artist=' + encodeURIComponent(j.artist || '') + '&lang=zh';
      var renderAlbumSummary = function (s) {
        var el = D('albumSummary');
        if (!el || el.dataset.k !== albK) return;   // 异步回来时已切到别的专辑 → 丢弃
        var albHtml = summHas(s) ? renderSummaryContent(s, 'AI 整理', 'release', albOpts) : '';
        if (albHtml) {
          el.innerHTML = albHtml;
          bindCopyChips(el);
        } else if (s && (s.error || s.pending)) {
          // 261024-04 #7：失败不弹错，只留一行可点击重试
          el.innerHTML = '<div class="summ-retry" id=albumSummRetry role=button tabindex=0>' +
                         '资料整理失败（' + esc(s.error || '超时') + '）· 点击重试</div>';
          var rb = D('albumSummRetry');
          if (rb) rb.onclick = function () { summaryOnce(albUrl, renderAlbumSummary, true); };
        } else {
          el.remove();   // 无资料 / 无 key 又无缓存 → 干脆不显示
        }
      };
      // 261024-05 #T2：离开这张专辑页后不再轮询它的 release 摘要（旧 key 的请求不再发出）
      summaryOnce(albUrl, function (s) {
        renderAlbumSummary(s);
        albPrefPump();
      }, false, function () {
        return S.view === 'album' && !!S.album && String(S.album.k) === albK;
      });
      // 261024-04 #7：整张专辑 = 专辑本身（上面）+ 每首曲目（后台，并发 ≤2）
      albumTrackPrefetch(j);
    }

    var albBtn = D('albPlayAll');
    if (albBtn) {
      albBtn.onclick = function () {
        var playing = (S.playing || S.paused) && S.lastCur && (j.tracks || []).some(function (t) { return t.path === S.lastCur; });
        if (playing) {
          playOrPause();
          syncAlbPlayAllBtn();
        } else {
          playAlbum(j.k, 0);
          albBtn.textContent = '❚❚ 暂停';
          albBtn.title = '暂停播放';
        }
      };
    }
    if (D('aPlay')) D('aPlay').onclick = function () { playAlbum(j.k, 0); };
    D('aAdd').onclick = function () { enqueueAlbum(j.k); };
    D('aShuf').onclick = function () { playAlbum(j.k, Math.floor(Math.random() * j.tracks.length)); };
    // 2026-10-26（261026-02 #3）：人工补封面（同人自出版专辑全网无图时的唯一出路；存 covercache，不动音乐文件）
    if (D('aSetCover')) D('aSetCover').onclick = function () { D('aCoverFile').click(); };
    if (D('aCoverFile')) D('aCoverFile').onchange = function (ev) {
      var f = ev.target.files && ev.target.files[0];
      if (!f) return;
      var x = new XMLHttpRequest();
      x.open('POST', '/api/lib/cover/set?k=' + encodeURIComponent(j.k));
      x.onload = function () {
        try {
          var r = JSON.parse(x.responseText);
          if (r && r.ok) { toast('封面已设置'); openAlbum(j.k); }
          else { toast('设置失败：' + ((r && r.error) || '未知原因')); }
        } catch (e) { toast('设置失败：响应无法解析'); }
      };
      x.onerror = function () { toast('设置失败：网络错误'); };
      x.send(f);
    };
    Array.prototype.forEach.call(D('list').querySelectorAll('.fav-btn'), function (btn) {
      btn.onclick = function (ev) {
        ev.stopPropagation();
        toggleFav(btn.dataset.file, ev);
      };
    });
    Array.prototype.forEach.call(D('list').querySelectorAll('.trow'), function (e) {
      e.onclick = function (ev) {
        if (ev.target.closest && ev.target.closest('.fav-btn')) return;
        if (ev.target.closest && ev.target.closest('.add-btn')) {
          // 2026-10-26（261025-02 #3）：专辑页曲目行此前**没有单曲入队入口**
          // （左键=播放整张，右键无处理）⟹ 用户在专辑页根本无法把某一首加进播放列表。
          ev.stopPropagation();
          var tk0 = (j.tracks || [])[+e.dataset.i] || {};
          addPathCue(tk0.path, tk0.cs, tk0.ce);
          return;
        }
        var ti = +e.dataset.i;
        var tk = (j.tracks || [])[ti] || {};
        playAlbum(j.k, ti, tk.cs, tk.ce);      // cue 曲目：把本轨起止秒一起带上
      };
      // 右键菜单：立即播放 / 加入队列（同样是单曲入口）
      e.oncontextmenu = function (ev) {
        ev.preventDefault();
        ev.stopPropagation();
        var tk = (j.tracks || [])[+e.dataset.i] || {};
        showTrackMenu(ev.clientX, ev.clientY, tk);
      };
    });
  }).catch(function (e) {
    renderPanelError('album', function () { openAlbum(k); }, e);
  });
}

/* ---------------- 261024-04 #7：专辑页后台整理（曲目级 summary 预取） ----------------
   规则（用户原话）：「只要打开了 AI 整理……如果是一整张专辑，则自动整理整个专辑」。
     * 进入专辑页立即自动整理：专辑本身（release，见 openAlbum）+ **专辑内每一首**（track）；
     * 曲目级是纯后台：并发 ≤2、最多 40 首；超过 40 首只整理前 40 首并提示「已整理 40/N（其余可按需点开）」；
     * 同一实体只请求一次（summaryOnce 的缓存 + 单飞）；同一张专辑只排一次队；
     * 失败也计入进度（不是一直转），不留错误提示（曲目级本来就没人盯着看）。 */
var ALBPREF = { key: '', gen: 0, total: 0, allTotal: 0, done: 0, active: 0, queue: [], timer: 0, live: { v: 0 } };
function albPrefKey(a) { return String((a && a.k) || '') + '|' + String((a && a.name) || ''); }
// 曲目级 URL 的拼法与单曲页抽屉（npDataEnsure）保持一致，才能命中同一份缓存
function albumTrackUrl(t, alb) {
  var ar = (t && t.artist) || (alb && alb.artist) || '';
  return '/api/entity/summary?kind=track&name=' + encodeURIComponent((t && t.title) || '') +
         (ar ? ('&artist=' + encodeURIComponent(ar)) : '') + '&lang=zh';
}
// 261024-05 #T2：离开专辑页 / 打开另一张专辑 → 立刻停掉剩余队列（含还在 2s 轮询的旧 key）。
// 用户原话：「只在单曲播放页面才预取本曲所属的专辑，不是全部专辑」——专辑页的"整张"只能指
// **当前打开的这张**，绝不能把上一张的待办带进下一张。
function albPrefStop() {
  ALBPREF.live.v = 0;                    // 在飞请求的轮询/回调全部失效
  ALBPREF.gen++;
  ALBPREF.key = ''; ALBPREF.total = 0; ALBPREF.allTotal = 0; ALBPREF.done = 0; ALBPREF.active = 0;
  ALBPREF.queue = [];
  if (ALBPREF.timer) { clearTimeout(ALBPREF.timer); ALBPREF.timer = 0; }
  ALBPREF.live = { v: 0 };               // 给下一次预取一个全新的令牌
  var box = D('albProgWrap'); if (box) box.innerHTML = '';
}
// 每次派发/回调前都重新确认「页面仍是这张专辑」：视图变了、专辑换了、被 stop 了 → false
function albPrefAlive(gen) {
  return gen === ALBPREF.gen && ALBPREF.live.v === 1 && S.view === 'album' &&
         !!S.album && albPrefKey(S.album) === ALBPREF.key;
}
function albPrefRender() {
  var box = D('albProgWrap');
  if (!box) return;
  if (!ALBPREF.total) { box.innerHTML = ''; return; }
  var pct = ALBPREF.total ? Math.round(ALBPREF.done / ALBPREF.total * 100) : 0;
  var isOver = ALBPREF.allTotal > ALBPREF.total;
  var txt = (ALBPREF.done >= ALBPREF.total)
    ? (isOver ? ('已整理 ' + ALBPREF.done + '/' + ALBPREF.allTotal + '（其余可按需点开）')
              : ('曲目资料已后台整理 ' + ALBPREF.total + ' 首'))
    : ('正在后台整理曲目资料 ' + ALBPREF.done + ' / ' + ALBPREF.total);
  box.innerHTML = '<div class="albprog"><span class=t>' + esc(txt) + '</span>' +
                  '<span class=bar><i style="width:' + pct + '%"></i></span></div>';
}
function albPrefPump() {
  var gen = ALBPREF.gen;
  // 并发 ≤2，且与在飞 summary 请求共享上限，保证 summary 并发峰值 ≤ 2
  while (ALBPREF.active < 2 && Object.keys(SUMC.inflight).length < 2 && ALBPREF.queue.length) {
    if (!albPrefAlive(gen)) return;      // 已切页/换专辑：队列里剩下的一条也不再发
    var it = ALBPREF.queue.shift();
    ALBPREF.active++;
    summaryOnce(it.url, function () {
      if (!albPrefAlive(gen)) return;    // 旧专辑的结果绝不改新专辑的进度/并发数
      ALBPREF.active--;
      ALBPREF.done++;
      albPrefRender();
      if (ALBPREF.done >= ALBPREF.total) {
        clearTimeout(ALBPREF.timer);
        ALBPREF.timer = setTimeout(function () {
          var b = D('albProgWrap');
          if (b && albPrefAlive(gen) && ALBPREF.done >= ALBPREF.total) b.innerHTML = '';
        }, 2600);
      }
      albPrefPump();
    }, false, function () { return albPrefAlive(gen); });
  }
}
function albumTrackPrefetch(alb) {
  if (META.summary != 1) return;
  if (META.hasLlmKey !== true && META.summaryEnabled !== true) return;
  var key = albPrefKey(alb);
  if (ALBPREF.key === key && ALBPREF.live.v === 1) return;   // 同一张专辑只排一次
  var tracks = (alb && alb.tracks) || [];
  var allCount = tracks.length;
  var q = [];
  // 最多 40 首；超过 40 首只整理前 40 首
  tracks.forEach(function (t) {
    if (t && t.title) {
      if (q.length < 40) q.push({ url: albumTrackUrl(t, alb) });
    }
  });
  if (!q.length) return;
  var gen = ALBPREF.gen + 1;
  ALBPREF = { key: key, gen: gen, total: q.length, allTotal: allCount, done: 0, active: 0, queue: q, timer: 0, live: { v: 1 } };
  albPrefRender();
  albPrefPump();
}

/* cs/ce = 被点那一轨的 CUE 区间（来自 /api/lib/album 每首的 cs/ce）。
   服务端本来就从库里读同一份区间（双保险：索引是旧版时前端这份仍然能把起点带对），
   两者一致时行为完全相同 —— 判据"点第 2 轨从 0 开始计 pos"两边都成立。 */
function playAlbum(k, i, cs, ce) {
  api('/api/lib/play', 'k=' + encodeURIComponent(k) + '&i=' + (i || 0) + cueQuery(cs, ce) + '&' + params()).then(function (j) {
    if (!j.ok) { toast(j.error || '播放失败'); return; }
    toast('开始播放专辑'); loadQ();
  });
}
function enqueueAlbum(k) {
  api('/api/lib/enqueue?k=' + encodeURIComponent(k)).then(function (j) {
    if (!j.ok) { toast(j.error || '加入失败'); return; }
    toast('整张专辑已加入队列'); loadQ();
  });
}

/* ---------------- 艺术家头像（2026-10-26 261026-01 #1） ----------------
   后端现状（tools/rtaweb.cpp）：
     GET  /api/artist/image?name=<名>   只读磁盘缓存（artistimg\<fnv16>.jpg），**零网络**；未命中 404。
     POST /api/artist/prefetch          进后台预热（返回 {ok,queued,requested,running}）。
   ★ prefetch **没有 progress 端点** ⟹ 「头像补齐中 X/Y」里的 X/Y 只能由前端自己数：
     X = 已成功加载的 <img> 数，Y = 本页艺术家数。失败的图按批重放（服务器只查磁盘缓存，
     重放=本地零网络，只是让刚预热好的图顶上来）。这与封面那套 /api/lib/cover/progress
     轮询同构，只是数据源换成了 img 的 load 事件。 */
function artistImgUrl(name, round) {
  return '/api/artist/image?name=' + encodeURIComponent(name) + (round ? ('&r=' + round) : '');
}
// 低饱和圆形占位里的图标：音符（24px；详情页头像更大，传 44）
function artistNoteSvg(size) {
  return '<svg width=' + size + ' height=' + size + ' viewBox="0 0 24 24" fill=none stroke=currentColor' +
         ' stroke-width=1.6 stroke-linecap=round stroke-linejoin=round aria-hidden=true>' +
         '<path d="M9 17.5V5.6l9-1.7v11.6"/><circle cx="6.6" cy="17.6" r="2.5"/><circle cx="15.6" cy="15.6" r="2.5"/></svg>';
}
var ARTAV = { timer: 0, round: 0, total: 0, done: 0, queued: 0, stopped: false };
function artaStop() {
  if (ARTAV.timer) { clearInterval(ARTAV.timer); ARTAV.timer = 0; }
  ARTAV.round = 0; ARTAV.total = 0; ARTAV.done = 0; ARTAV.queued = 0; ARTAV.stopped = false;
}
function artaRender() {
  var box = D('artProgWrap');
  if (!box) return;
  if (!ARTAV.total) { box.innerHTML = ''; return; }
  var pct = Math.round(ARTAV.done / ARTAV.total * 100);
  // 三种措辞：还在轮询 → 补齐中；全部到位 → 已补齐；轮询停了但仍有缺图 → 如实说"其余暂无图源"
  //（绝不写"已补齐 X/Y"来掩盖缺的图 —— 铁律 4）
  var txt = (ARTAV.done >= ARTAV.total)
    ? ('头像已补齐 ' + ARTAV.done + ' / ' + ARTAV.total + ' 位')
    : (ARTAV.stopped
        ? ('头像已有 ' + ARTAV.done + ' / ' + ARTAV.total + ' 位（其余暂无图源）')
        : ('头像补齐中 ' + ARTAV.done + ' / ' + ARTAV.total + ' 位'));
  box.innerHTML = '<div class=albprog><span class=t>' + esc(txt) + '</span>' +
                  '<span class=bar><i style="width:' + pct + '%"></i></span></div>';
}
function artaBind() {
  var imgs = document.querySelectorAll('#list .card .acv img');
  ARTAV.total = imgs.length;
  ARTAV.done = 0;
  Array.prototype.forEach.call(imgs, function (im) {
    im.onload = function () {
      if (im.classList.contains('ok')) return;
      im.classList.add('ok');
      im.classList.remove('err');
      ARTAV.done++;
      artaRender();
      if (ARTAV.done >= ARTAV.total && ARTAV.timer) { clearInterval(ARTAV.timer); ARTAV.timer = 0; }
    };
    im.onerror = function () { im.classList.add('err'); };
    // 命中磁盘缓存时 load 事件可能早于我挂回调（图片在 innerHTML 解析时就开始了）⟹ 补一次
    if (im.complete && im.naturalWidth > 0) im.onload();
  });
  artaRender();
}
// 一轮 1.8 s；每轮只重放 ≤48 张（WebUi 单线程 accept，246 张同时重放会把它挤住）
function artaPoll() {
  if (ARTAV.timer) return;
  ARTAV.timer = setInterval(function () {
    if (S.view !== 'artists' || !D('artProgWrap')) { artaStop(); return; }
    var pend = [];
    Array.prototype.forEach.call(document.querySelectorAll('#list .card .acv img'), function (im) {
      if (!im.classList.contains('ok')) pend.push(im);
    });
    if (!pend.length) { artaStop(); artaRender(); return; }
    ARTAV.round++;
    var batch = 48, off = ((ARTAV.round - 1) * batch) % pend.length;
    for (var i = 0; i < pend.length && i < batch; i++) {
      var im = pend[(off + i) % pend.length];
      im.src = artistImgUrl(im.getAttribute('data-n'), ARTAV.round);
    }
    artaRender();
    if (ARTAV.round >= 20) {          // 约 36 s 后停手，但**保留真实进度**（缺的图不冒充补齐）
      clearInterval(ARTAV.timer); ARTAV.timer = 0;
      ARTAV.stopped = true; ARTAV.round = 0;
      artaRender();
    }
  }, 1800);
}

/* ---------------- 艺术家 ---------------- */
// 2026-10-26（261026-05 #1）：支持按关键词过滤（搜索栏在艺术家页搜索时用）。
// q 为空 = 全部；非空 = 只保留名字包含关键词的艺术家（不区分大小写）。
function loadArtists(q) {
  S.view = 'artists';
  var kw = String(q || '').trim().toLowerCase();
  artaStop();
  head('艺术家', '加载中…', '');
  // 2026-10-26（261026-01 #1）：艺术家头像走外部源，但**抓取必须后台做** ——
  // WebUi 是单线程 accept，如果在图片请求里同步抓，246 个头像会把整站串行阻塞掉。
  // 这里只做一件事：告诉后端"开始预热这些名字"，图片端点本身只读缓存（未命中立刻 404）。
  api('/api/artist/prefetch').then(function (j) {
    if (j && j.ok && j.queued) ARTAV.queued = j.queued;
    artaPoll();            // 预热一开跑就开始"补齐中 X/Y"轮询
  }).catch(function () { artaPoll(); });
  api('/api/lib/albums').then(function (j) {
    var m = {}, order = [];
    (j.albums || []).forEach(function (a) {
      var k = a.artist || '未知艺术家';
      if (!m[k]) { m[k] = { n: 0, d: 0, cv: false, key: a.k }; order.push(k); }
      m[k].n++; m[k].d += a.dur;
      if (a.cv) m[k].cv = true, m[k].key = a.k;
    });
    order.sort(function (x, y) { return x.localeCompare(y, 'zh'); });
    if (kw) {
      order = order.filter(function (k) { return String(k).toLowerCase().indexOf(kw) >= 0; });
      head('艺术家', order.length + ' 位（搜索：' + q + '）', '');
    } else {
      head('艺术家', order.length + ' 位', '');
    }
    if (!order.length) { D('list').innerHTML = '<div class=empty>库里还没有音乐</div>'; return; }
    var h = '<div id=artProgWrap></div>';
    h += '<div class=grid style="grid-template-columns:repeat(auto-fill,minmax(150px,1fr))">';
    order.forEach(function (k) {
      var v = m[k];
      // 2026-10-26（261026-01 #1）：头像来自**外部艺术家图源**（/api/artist/image）。
      // 用户钦定「取不到就留空、不许用专辑封面顶替」⟹ 这里三件事同时成立：
      //   ① 不渲染 .ph 首字母灰块（旧版那排灰字母就是它）；② 不借任何专辑封面；
      //   ③ 取不到图时露一个**低饱和圆形占位**（浅描边 + 音符图标 opacity .35），页面不至于满屏空洞。
      // <img> 永远留在 DOM 里（失败只是 opacity:0，不删不摘）⟹「img 数 === 艺术家数」可被断言。
      h += '<div class=card data-a="' + esc(k) + '">' +
           '<div class="cv acv">' +
             '<div class=aph>' + artistNoteSvg(24) + '</div>' +
             // ★ 不用 loading=lazy：惰性加载的图在视口外永远不会发请求 ⟹ 重放（换 src）也发不出去，
             //   「补齐中 X/Y」会永远停在首屏那几张上（实测 35/127 卡死 30 s）。头像端点只读磁盘缓存、
             //   零网络，127~246 张一起请求的代价就是几百毫秒本地读盘，值得换一个真的会收敛的进度。
             '<img data-n="' + esc(k) + '" src="' + artistImgUrl(k) + '" alt=""' +
             ' onerror="this.classList.add(\'err\')">' +
           '</div>' +
           '<div class=nm title="' + esc(k) + '">' + esc(k) + '</div>' +
           '<div class=ar>' + v.n + ' 张专辑</div></div>';
    });
    h += '</div>';
    D('list').innerHTML = h;
    // 渲染完立刻挂 load/error 统计 + 开始轮询（后端没有 progress 端点，X/Y 只能自己数）
    artaBind();
    artaPoll();
    Array.prototype.forEach.call(D('list').querySelectorAll('.card'), function (e) {
      e.onclick = function () {
        var who = e.dataset.a;
        openArtistProfile(who);
      };
    });
  }).catch(function (e) {
    renderPanelError('artists', function () { loadArtists(); }, e);
  });
}
function renderAlbumSubset(who) {
  S.view = 'albums';
  head(who, S.albums.length + ' 张专辑', '<button class=btn id=bArtistBio>艺术家资料</button> <button class=btn id=bAll>返回全部专辑</button>');
  var h = '<div class=grid>';
  S.albums.forEach(function (a, i) { h += albumCardHtml(a, i); });
  D('list').innerHTML = h + '</div>';
  D('bArtistBio').onclick = function () { openArtistProfile(who); };
  D('bAll').onclick = function () { loadAlbums(''); };
  Array.prototype.forEach.call(D('list').querySelectorAll('.card'), function (e) {
    var a = S.albums[+e.dataset.i];
    e.onclick = function (ev) {
      if (ev.target.closest && ev.target.closest('.pl')) { playAlbum(a.k, 0); ev.stopPropagation(); return; }
      openAlbum(a.k);
    };
  });
}

/* ---------------- 艺术家资料页（P3④a/④b） ----------------
   261024-04 #5：以前 openArtistProfile 先把整页写成「正在获取权威资料…」，等 /api/entity/knowledge
   回来才渲染 ⟹ 慢网下整页空白好几秒。现在：
     ① **立刻**渲染基础页（名字 + 返回按钮 + 头像 + 资料区块），「正在获取资料…」只出现在**资料区块**里；
     ② 资料回来后只重绘这几块；③ 切走再回来：同 who 的请求 in-flight ⟹ 挂回调复用，
        已有结果 ⟹ 直接渲染，绝不重复请求（费用纪律）。 */
var ARTIST_KNOW = {};    // who -> { state: 'loading'|'done', res, waiters: [] }
// 已废弃：以前用「该艺术家第一张有封面的专辑封面」当头像。
// 2026-10-26 用户钦定「不要用封面来替换」⟹ 保留函数只为兼容旧调用，返回空。
function artistCoverKey(who) { return ''; }
function artistCoverKeyUnused(who) {
  if (!(S.albums && S.albums.length)) return '';
  for (var i = 0; i < S.albums.length; i++) {
    if ((S.albums[i].artist || '') === who && S.albums[i].cv) return S.albums[i].k;
  }
  return '';
}
// 2026-10-26（261026-01 #1）：艺术家头像改用**外部艺术家图源**（/api/artist/image）。
// 用户明确：艺术家确实需要接外部源；**取不到就留空，绝不能用专辑封面顶替**。
// 详情页与列表同一套处理：真 <img>（404 → opacity:0）+ 底下低饱和圆形占位，不再用 background-image
// （background-image 加载失败时页面什么都不显示，只能看到 .cv 的灰底 = 用户说的"几乎全是空白/灰块"）。
function artistHeroHtml(who, subText) {
  return '<div class="ahead artist-head">' +
       '<div class="cv acv">' +
         '<div class=aph>' + artistNoteSvg(44) + '</div>' +
         '<img data-n="' + esc(who) + '" src="' + artistImgUrl(who) + '" alt=""' +
         ' onerror="this.classList.add(\'err\')">' +
       '</div>' +
       '<div class=info>' +
       '<div class=kind>ARTIST · 艺术家资料</div>' +
       '<h1>' + esc(who) + '</h1>' +
       '<div class=sub>' + esc(subText || '') + '</div>' +
       '<div style="display:flex;gap:8px;margin-top:10px;flex-wrap:wrap">' +
       '<button class=btn id=bArtistAlbums>查看专辑</button> ' +
       '<button class=btn id=bArtistRefresh>刷新资料</button> ' +
       '<button class=btn id=bBackArtists>返回列表</button>' +
       '</div></div></div>';
}
// 基础页（骨架）：hero + 资料区块。「正在获取资料…」**只出现在资料区块里**
//（用户 261024-04 #5 的要求），hero 与返回按钮立刻可点、副标题留空不占位。
function artistBaseHtml(who) {
  var h = artistHeroHtml(who, '');
  h += '<div class=artist-body style="padding:0 22px 30px">';
  if (META.summary == 1) {
    h += '<div class="sec-card summary-sec" id=artistSummary style="margin-bottom:20px">' +
         '<div style="font-size:12px;color:var(--mut2)">正在获取资料…</div></div>';
  }
  h += '<div class="sec-card" id=artistKnowCard style="margin-bottom:24px">' +
       '<div style="font-size:12.5px;color:var(--mut)">正在获取资料…</div></div>';
  h += '</div>';
  return h;
}
function bindArtistButtons(who) {
  if (D('bArtistAlbums')) {
    D('bArtistAlbums').onclick = function () {
      api('/api/lib/albums').then(function (jj) {
        S.albums = (jj.albums || []).filter(function (a) { return (a.artist || '未知艺术家') === who; });
        renderAlbumSubset(who);
      });
    };
  }
  if (D('bArtistRefresh')) D('bArtistRefresh').onclick = function () { openArtistProfile(who, true); };
  if (D('bBackArtists')) D('bBackArtists').onclick = function () { loadArtists(); };
  if (D('bRetryKnowledge')) D('bRetryKnowledge').onclick = function () { openArtistProfile(who, true); };
  if (D('bBioToggle')) {
    D('bBioToggle').onclick = function () {
      var box = D('bioBox');
      if (!box) return;
      var isCol = box.classList.toggle('collapsed');
      this.textContent = isCol ? '展开全部' : '收起简介';
    };
  }
}
function openArtistProfile(who, refresh) {
  S.view = 'artist';
  setView('artists');
  S.view = 'artist';
  S.artistWho = who;
  head(who, '', '<button class=btn id=bBackArtistsTop>返回艺术家列表</button>');
  if (D('bBackArtistsTop')) D('bBackArtistsTop').onclick = function () { loadArtists(); };
  // ① 立刻出基础页（绝不整页空白等接口；加载文案只进资料区块）
  D('list').innerHTML = artistBaseHtml(who);
  bindArtistButtons(who);

  var wantSumm = (META.summary == 1);
  var summState = { done: false, res: null, enabled: wantSumm, skipBio: false };
  var uSumm = '/api/entity/summary?kind=person&name=' + encodeURIComponent(who) + '&lang=zh' +
              (refresh ? '&refresh=1' : '');
  var onSumm = function (s) {
    summState.done = true; summState.res = s;
    applyArtistSummary();
  };
  function applyArtistSummary() {
    if (!wantSumm) return;
    if (S.view !== 'artist' || S.artistWho !== who) return;    // 已切走 → 丢弃
    var el = D('artistSummary');
    if (!el || !summState.done) return;
    var ss = summState.res;
    // 261024-01 #4：AI 资料优先按后端结构化 fields 渲染成标签值网格；旧缓存只有 sections 也能显示。
    if (summHas(ss)) {
      el.style.display = '';
      el.innerHTML = renderSummaryContent(ss, 'AI 整理', 'person', { skip: (summState.skipBio ? { '简介': 1 } : {}) });
      bindCopyChips(el);
    } else if (ss && (ss.error || ss.pending)) {
      // 261024-04 #7：失败不弹错，只留一行可点击重试
      el.style.display = '';
      el.innerHTML = '<div class="summ-retry" id=artistSummRetry role=button tabindex=0>' +
                     '资料整理失败（' + esc(ss.error || '超时') + '）· 点击重试</div>';
      var rb = D('artistSummRetry');
      if (rb) rb.onclick = function () { summaryOnce(uSumm, onSumm, true); };
    } else {
      el.remove();   // 无资料 / 无 key 又无缓存 → 不留空白板块
    }
  }
  function paintKnow(res) {
    if (S.view !== 'artist' || S.artistWho !== who) return;
    renderArtistProfile(who, res, summState);
    applyArtistSummary();
    bindArtistButtons(who);
  }

  // ② 知识/资料：同 who 的在飞请求复用，已有结果直接渲染（不重复请求）
  var kc = ARTIST_KNOW[who];
  if (!refresh && kc && kc.state === 'done') {
    paintKnow(kc.res);
  } else if (!refresh && kc && kc.state === 'loading') {
    kc.waiters.push(paintKnow);
  } else {
    var rec = ARTIST_KNOW[who] = { state: 'loading', res: null, waiters: [paintKnow] };
    var uKnow = '/api/entity/knowledge?kind=person&name=' + encodeURIComponent(who) + (refresh ? '&refresh=1' : '');
    // D1（261024-05 主控追加）：后端把 knowledge 改成「先回 pending、后台再跑」，避免一次堵 1.5s。
    // pending ⟹ 700ms 后用**同一 URL** 重试（最多 40 次），期间资料区块保持「正在获取资料…」；
    // 仅当仍停在这个艺术家页时才重试（切走即停，不再发请求）。ok:false 不重试。
    var kTries = 0;
    var knowFetch = function () {
      if (S.view !== 'artist' || S.artistWho !== who) {
        rec.state = 'idle'; rec.waiters = [];     // 已切走：停掉重试链（下次进来重新取）
        return;
      }
      api(uKnow).catch(function (err) { return { ok: false, error: String(err), materials: [] }; })
        .then(function (res) {
          if (res && res.pending === true) {
            if (kTries < 40) {
              kTries++;
              setTimeout(knowFetch, 700);
              return;                              // 保持加载态：不 paint 空结果
            }
            res = { ok: false, error: '资料获取超时', materials: [] };
          }
          rec.state = 'done'; rec.res = res;
          var ws = rec.waiters; rec.waiters = [];
          ws.forEach(function (f) { try { f(res); } catch (e) { } });
        });
    };
    knowFetch();
  }

  // ③ 简介（AI 整理）：与知识并行，谁先到都不丢；summaryOnce 保证同一实体只请求一次
  if (wantSumm) summaryOnce(uSumm, onSumm, refresh);
}

function renderArtistProfile(who, j, summary) {
  S.view = 'artist';
  setView('artists');
  S.view = 'artist';

  var mats = (j && j.materials) || [];
  var isOk = j && j.ok && mats.length > 0;
  // 资料解析提前到这里：AI 资料网格要知道"简介"是否已由词条给出（避免同一页两段简介）。
  var bioMat = null;
  var stylesMats = [];
  mats.forEach(function (m) {
    if (!bioMat && (m.kind === 'biography' || m.kind === 'disambiguation' || m.text)) bioMat = m;
    if (m.kind === 'genre' || m.kind === 'similar') stylesMats.push(m);
  });
  var bioText = (isOk && bioMat) ? (bioMat.text || '') : '';

  var subText = '';
  if (isOk) {
    var firstDate = mats[0].fetched_at ? fmtDate(mats[0].fetched_at) : '';
    if (j.from_cache) {
      subText = '缓存 · ' + (firstDate || '近期') + ' 取回' + (j.offline ? ' · 离线' : '');
    } else {
      // 261024-04 #5：去掉「权威」二字（用户要求全文案不再出现「权威资料 / 权威词条」）
      subText = '资料' + (firstDate ? (' · ' + firstDate + ' 取回') : '');
    }
  }
  // 261024-01 #3a：没有资料时**不显示**「资料暂缺」这类状态小字 —— 顶部只留名字，
  // 下方安静地给 AI 整理资料；没有资料就什么都不显示（用户原话：那句说明"纯粹多余"）。

  head(who, subText, '<button class=btn id=bTopBack>返回艺术家列表</button>');
  if (D('bTopBack')) D('bTopBack').onclick = function () { loadArtists(); };

  var h = '';
  // 头像/名称（261024-04 #5：与基础骨架共用同一份 markup，避免两处漂移）
  h += artistHeroHtml(who, subText);

  h += '<div class=artist-body style="padding:0 22px 30px">';

  // AI 整理资料（261024-01 #3a/#4）：这是本页**主内容**，紧跟 hero 之后；
  //   词条资料缺失时不再有任何"资料暂缺 / 维基百科没有匹配条目…"的解释卡（用户：那句纯粹多余）。
  //   结构化 fields → 标签值网格；旧缓存只有 sections → 段落兜底；两者都没有就整卡不渲染。
  var summHtml = '';
  if (META.summary == 1) {
    var sumObj = (summary && summary.enabled && summary.done) ? summary.res : null;
    var hasSummary = summHas(sumObj);
    var sumLoading = (META.hasLlmKey === true && !hasSummary);
    if (summary) summary.skipBio = !!bioText;   // 词条简介已在下面展示 → AI 网格里不重复「简介」
    summHtml = '<div class="sec-card summary-sec" id="artistSummary" style="margin-bottom:20px' + (sumLoading ? '' : ';display:none') + '">';
    if (hasSummary) {
      summHtml += renderSummaryContent(sumObj, 'AI 整理', 'person', { skip: (bioText ? { '简介': 1 } : {}) });
    } else if (sumLoading) {
      summHtml += '<div style="font-size:12px;color:var(--mut2)">正在获取资料…</div>';
    }
    summHtml += '</div>';
  }
  h += summHtml;

  if (!isOk) {
    // 261024-01 #3a：**删除整个「资料暂缺」说明块**。没有词条资料 → 上面 AI 资料安静显示，这里什么都不放。
    // 只有"确实无内容可显示"的情况，才给一行极简空态 + 重试（绝不写一大段解释）。
    // 2026-10-25（uicheck 抓到的真空白）：在线整理**开着但没配 LLM key**时，上面那张 AI 卡永远填不满，
    // 旧条件只判 META.summary != 1 ⟹ 整页空白（三个分支一个都不命中）。补上 hasLlmKey 判断。
    if (META.summary != 1 || META.hasLlmKey !== true) {
      h += '<div class="card empty-card" style="padding:22px 20px;text-align:center;background:var(--phbg);border:1px solid var(--line);border-radius:12px;margin-top:14px">' +
           '<div style="color:var(--mut);font-size:12.5px">暂无资料</div>' +
           '<button class="btn" id=bRetryKnowledge style="margin-top:12px">重试获取</button>' +
           '</div>';
    }
  } else {
    // 1) 词条简介（原文，长文折叠「展开」）—— 排在 AI 资料之后，是"佐证"而不是主内容
    var isLong = bioText.length > 280 || (bioText.split('\n').length > 5);

    h += '<div class="sec-card bio-sec" style="margin-bottom:24px">' +
         '<div class="sec-title" style="font-size:12px;font-weight:600;letter-spacing:1px;color:var(--mut2);margin-bottom:10px">简介</div>' +
         '<div class="bio-box ' + (isLong ? 'collapsed' : '') + '" id=bioBox>' +
         esc(bioText || '暂无详细文字介绍') +
         '</div>' +
         (isLong ? '<button class=btn id=bBioToggle style="margin-top:10px">展开全部</button>' : '') +
         '</div>';

    // 2) 风格与相似（有则显示）
    if (stylesMats.length > 0) {
      h += '<div class="sec-card style-sec" style="margin-bottom:24px">' +
           '<div style="font-size:12px;font-weight:600;letter-spacing:1px;color:var(--mut2);margin-bottom:10px">风格与相似</div>' +
           '<div style="display:flex;gap:8px;flex-wrap:wrap">';
      stylesMats.forEach(function (sm) {
        h += '<span class=badge>' + esc(sm.title || sm.kind) + '</span>';
      });
      h += '</div></div>';
    }

    // 3) 来源：来源徽章、标题、取回日期、「复制链接」芯片（绝不 window.open / <a target=_blank>）
    h += '<div class="sec-card sources-sec" style="margin-bottom:24px">' +
         '<div style="font-size:12px;font-weight:600;letter-spacing:1px;color:var(--mut2);margin-bottom:12px">资料来源</div>' +
         '<div style="display:flex;flex-direction:column;gap:8px">';

    mats.forEach(function (m) {
      var srcName = '来源';
      if (m.source.indexOf('wikipedia') >= 0) srcName = '维基百科';
      else if (m.source.indexOf('moegirl') >= 0) srcName = '萌娘百科';
      else if (m.source.indexOf('discogs') >= 0) srcName = 'Discogs';
      else if (m.source.indexOf('lastfm') >= 0) srcName = 'Last.fm';
      else if (m.source) srcName = m.source;

      var fDate = fmtDate(m.fetched_at);
      h += '<div class="trow source-row" style="cursor:default;display:flex;align-items:center;gap:12px;padding:9px 14px;border-radius:9px;background:var(--hov)">' +
           '<span class=badge>' + esc(srcName) + '</span>' +
           '<span style="font-weight:600;font-size:12.5px;color:var(--txt)">' + esc(m.title) + '</span>' +
           '<span style="font-size:11.5px;color:var(--mut2);margin-right:auto">' + (fDate ? ('取回于 ' + fDate) : '') + '</span>' +
           citationChipHtml(m.url, '复制链接', false) +
           '</div>';
    });
    h += '</div></div>';
  }

  h += '</div>';
  D('list').innerHTML = h;

  // 绑定事件（261024-04 #5：与基础骨架共用 bindArtistButtons，两处不再各写一遍）
  bindArtistButtons(who);

  // 复制链接芯片：navigator.clipboard.writeText(url) + toast「已复制来源链接」
  bindCopyChips(D('list'));
}

/* ---------------- 古典作品与版本推荐（P3④b Discogs L4 / 261023-03 第4条） ---------------- */
function loadWorksView(initialTitle, initialComposer, fromNp) {
  S.view = 'works';
  S.worksFrom = fromNp ? 'np' : '';
  setView('works');
  S.view = 'works';
  // 用户 261023-04 #9：左导航不再有「古典作品」入口，作品页只从单曲页「作品」tab 进入；
  // 从单曲页进来时给一个明确的返回路径（回到正在播放页），不是无路可退。
  head('古典作品 · 演绎版本', '按作品名查不同指挥/乐团/年代的录音版本',
       (fromNp && S.lastCur) ? '<button class=btn id=wBackNp title="返回单曲播放页">← 返回正在播放</button>' : '');

  var defaultTitle = initialTitle || 'Boléro';
  var defaultComposer = (initialComposer !== undefined) ? initialComposer : 'Maurice Ravel';

  var h = '<div style="padding:10px 22px 30px">';

  // 首次进入顶部说明文案（261023-03 第4条）
  h += '<div style="font-size:12.5px;color:var(--mut2);margin-bottom:12px;line-height:1.6">' +
       '古典作品页：按「作品名 + 作曲家」检索 Discogs 上的不同录音版本（指挥 / 乐团 / 年代 / 厂牌），' +
       '帮你在同一首作品的不同演绎里挑到更对味的一张。可以自己填作品名查询，也可以直接点下方「示例作品」芯片一键检索。</div>';

  // 搜索卡片
  h += '<div class=card style="padding:16px 20px;margin-bottom:20px;background:var(--hov);border:1px solid var(--line);border-radius:10px">';
  h += '<div style="display:flex;gap:12px;flex-wrap:wrap;align-items:center">';
  h += '<div style="flex:1;min-width:200px"><label style="display:block;font-size:11.5px;color:var(--mut2);margin-bottom:4px">作品名称</label>' +
       '<input id=wTitle value="' + esc(defaultTitle) + '" placeholder="作品名称，例如 Boléro / Symphony No. 5" style="width:100%;height:32px;padding:0 10px;background:var(--card);border:1px solid var(--line);border-radius:6px;color:var(--txt);box-sizing:border-box"></div>';
  h += '<div style="flex:1;min-width:200px"><label style="display:block;font-size:11.5px;color:var(--mut2);margin-bottom:4px">作曲家（可选）</label>' +
       '<input id=wComposer value="' + esc(defaultComposer) + '" placeholder="作曲家，例如 Maurice Ravel / Beethoven" style="width:100%;height:32px;padding:0 10px;background:var(--card);border:1px solid var(--line);border-radius:6px;color:var(--txt);box-sizing:border-box"></div>';
  h += '<div style="align-self:flex-end">' +
       '<button class="btn pri" id=bSearchVersions style="height:32px;padding:0 16px">查询版本</button> ' +
       '<button class=btn id=bRefreshVersions style="height:32px;padding:0 12px" title="强制刷新">刷新</button>' +
       '</div>';
  h += '</div>';

  // 示例作品芯片（261023-03 第4条：点击直接检索）
  h += '<div style="display:flex;gap:8px;align-items:center;margin-top:12px;flex-wrap:wrap">' +
       '<span style="font-size:12px;color:var(--mut2)">示例作品：</span>' +
       '<button class="btn chip-work" data-title="Boléro" data-composer="Maurice Ravel" style="font-size:11.5px;padding:2px 8px" title="Maurice Ravel — Boléro">Boléro（拉威尔）</button>' +
       '<button class="btn chip-work" data-title="Symphony No. 5" data-composer="Ludwig van Beethoven" style="font-size:11.5px;padding:2px 8px" title="Ludwig van Beethoven — Symphony No. 5">贝多芬第五交响曲</button>' +
       '<button class="btn chip-work" data-title="Brandenburg Concerto No. 3" data-composer="Johann Sebastian Bach" style="font-size:11.5px;padding:2px 8px" title="J.S. Bach — Brandenburg Concerto No. 3">勃兰登堡协奏曲</button>' +
       '<button class="btn chip-work" data-title="The Four Seasons" data-composer="Antonio Vivaldi" style="font-size:11.5px;padding:2px 8px" title="Antonio Vivaldi — The Four Seasons">四季（维瓦尔第）</button>' +
       '<button class="btn chip-work" data-title="Symphony No. 5" data-composer="Gustav Mahler" style="font-size:11.5px;padding:2px 8px" title="Gustav Mahler — Symphony No. 5">马勒第五交响曲</button>' +
       '</div>';
  h += '</div>';

  // 结果呈现容器
  h += '<div id=worksResult></div>';
  h += '</div>';

  D('list').innerHTML = h;
  if (D('wBackNp')) D('wBackNp').onclick = function () { openNowPlaying(); };

  function doQuery(refresh) {
    var t = D('wTitle') ? D('wTitle').value.trim() : '';
    var c = D('wComposer') ? D('wComposer').value.trim() : '';
    if (!t) { toast('请输入作品名称'); return; }
    openWorkVersions(t, c, refresh);
  }

  if (D('bSearchVersions')) D('bSearchVersions').onclick = function () { doQuery(false); };
  if (D('bRefreshVersions')) D('bRefreshVersions').onclick = function () { doQuery(true); };
  if (D('wTitle')) {
    D('wTitle').onkeydown = function (ev) { if (ev.key === 'Enter') doQuery(false); };
  }
  if (D('wComposer')) {
    D('wComposer').onkeydown = function (ev) { if (ev.key === 'Enter') doQuery(false); };
  }

  Array.prototype.forEach.call(D('list').querySelectorAll('.chip-work'), function (btn) {
    btn.onclick = function () {
      var t = btn.dataset.title;
      var c = btn.dataset.composer;
      if (D('wTitle')) D('wTitle').value = t;
      if (D('wComposer')) D('wComposer').value = c;
      openWorkVersions(t, c, false);
    };
  });

  if (defaultTitle) {
    openWorkVersions(defaultTitle, defaultComposer, false);
  }
}

function openWorkVersions(workTitle, composer, refresh) {
  // 261023-03 第4条：切走后再点别的栏目时，异步回调必须能识别自己已经过期。
  // 两道保险：① S.view 守卫（离开 works 直接丢弃）；② 请求序号（同一视图内连点只认最后一次）。
  if (S.view !== 'works') return;
  var box = D('worksResult');
  if (!box) return;
  S.worksReq = (S.worksReq || 0) + 1;
  var req = S.worksReq;
  box.innerHTML = '<div class=empty style="padding:60px 0">正在从 Discogs 检索版本…</div>';

  var u = '/api/entity/versions?kind=work&name=' + encodeURIComponent(workTitle) +
          (composer ? ('&composer=' + encodeURIComponent(composer)) : '') +
          (refresh ? '&refresh=1' : '');

  // 2026-10-23（用户 261023-01 #4「切到作品栏后无法切回其它栏位」）真凶：
  // 这里没有「当前是否还在作品页」的判断。用户点了作品栏、Discogs 还没返回时切到专辑/曲目，
  // 回调仍把 #worksResult 的内容写回 → 看起来就是「卡在作品页/切不过去」。
  // 修法：回调先校验 S.view 与容器是否仍属于作品页，不满足就整段丢弃（不写 DOM、不报错）。
  var stillWorks = function () {
    return S.view === 'works' && !!D('worksResult');
  };
  api(u).then(function (j) {
    // 双守卫：视图已切走 → 丢弃；同一视图内连点 → 只认最后一次请求（S.worksReq 序号）
    if (!stillWorks() || req !== S.worksReq) return;
    renderWorkVersions(workTitle, composer, j);
  }).catch(function (err) {
    if (!stillWorks() || req !== S.worksReq) return;
    renderWorkVersions(workTitle, composer, { ok: false, errors: [String(err)], performances: [] });
  });
}

function renderWorkVersions(workTitle, composer, j) {
  if (S.view !== 'works') return;   // 261023-03 第4条：已离开作品页，丢弃过期结果
  var box = D('worksResult');
  if (!box) return;
  // 同 openWorkVersions 的守卫：视图已经切走就别再动 DOM（防「切不回去」的残留内容）
  if (S.view !== 'works') return;

  var perfs = (j && j.performances) || [];
  var isOk = j && j.ok && perfs.length > 0;

  var h = '';

  if (!isOk) {
    var errMsg = (j && j.errors && j.errors[0]) || (j && j.error) || '未在 Discogs 检索到该作品的演绎版本，或当前网络不可达。';
    h += '<div class="card empty-card" style="padding:36px 20px;text-align:center;background:var(--phbg);border:1px solid var(--line);border-radius:12px">' +
         '<div style="font-size:15px;font-weight:600;margin-bottom:8px;color:var(--txt)">未找到版本或检索失败</div>' +
         '<div style="color:var(--mut);font-size:12.5px;margin-bottom:18px;max-width:520px;margin-left:auto;margin-right:auto;line-height:1.6">' +
         esc(errMsg) + '</div>' +
         '<button class="btn pri" id=bRetryVersions>重试查询</button>' +
         '</div>';
    box.innerHTML = h;
    if (D('bRetryVersions')) {
      D('bRetryVersions').onclick = function () { openWorkVersions(workTitle, composer, true); };
    }
    return;
  }

  // 统计数值（不同演绎 X / 发行版本 Y）
  var distinctCount = (typeof j.distinctCount === 'number') ? j.distinctCount : ((typeof j.distinct_count === 'number') ? j.distinct_count : perfs.length);
  var releaseCount = (typeof j.releaseCount === 'number') ? j.releaseCount : ((typeof j.release_count === 'number') ? j.release_count : perfs.length);

  // 头部概要卡片
  var subStr = (composer ? ('作曲家: ' + esc(composer) + ' · ') : '') +
               (j.from_cache ? ('缓存 · ' + fmtDate(j.generated_at) + (j.offline ? ' · 离线' : '')) : 'Discogs 实时') +
               ' · 不同演绎 ' + distinctCount + ' / 发行版本 ' + releaseCount;
  h += '<div class="ahead work-head" style="margin-bottom:16px;padding:16px 20px;background:var(--hov);border-radius:10px">' +
       '<div class=kind style="font-size:11px;font-weight:600;letter-spacing:1px;color:var(--mut2)">CLASSICAL WORK · 古典作品</div>' +
       '<h1 style="margin:4px 0 6px;font-size:20px;font-weight:700;color:var(--txt)">' + esc(workTitle) + '</h1>' +
       '<div class=sub style="font-size:12px;color:var(--mut)">' + subStr + '</div>' +
       '</div>';

  // 不足 5 条演绎提示
  if (distinctCount < 5) {
    h += '<div class="card count-hint" style="padding:10px 14px;margin-bottom:14px;background:var(--phbg);border:1px solid var(--line);border-radius:8px;font-size:12px;color:var(--mut)">' +
         '不同演绎仅 ' + distinctCount + ' 个（目标 ≥5，未做臆造）' +
         '</div>';
  }

  // 版本列表 表头
  h += '<div class="trow trow-head" style="cursor:default;display:flex;align-items:center;gap:12px;padding:6px 16px;margin-bottom:4px;border-radius:6px;font-size:11px;font-weight:600;color:var(--mut2);text-transform:uppercase;letter-spacing:0.5px">' +
       '<span style="font-family:monospace;min-width:38px">年份</span>' +
       '<span style="flex:1.3;min-width:0">标题 / 发行</span>' +
       '<span style="flex:0.9;min-width:0">指挥</span>' +
       '<span style="flex:1.2;min-width:0">乐团</span>' +
       '<span style="flex:0.9;min-width:0">独奏</span>' +
       '<span style="min-width:75px">厂牌</span>' +
       '<span style="min-width:55px">格式</span>' +
       '<span style="min-width:80px;text-align:right">操作</span>' +
       '</div>';

  // 版本列表
  h += '<div class=version-list style="display:flex;flex-direction:column;gap:8px">';
  perfs.forEach(function (p) {
    var yr = p.year > 0 ? String(p.year) : '—';
    var conductorDisplay = p.conductor || '–';
    var ensembleDisplay = p.ensemble || p.orchestra || '–';
    var soloistsDisplay = (p.soloists && p.soloists.length > 0) ? p.soloists.join(', ') : '–';

    var badges = '';
    if (p.otherEditions && p.otherEditions > 0) {
      badges += '<span class=badge style="margin-left:6px;font-size:10px;background:var(--hov2);color:var(--mut)" title="同演绎其他发行 +' + p.otherEditions + ' 版">+' + p.otherEditions + ' 版</span>';
    }
    if (p.dedupUnknown) {
      badges += '<span class=badge style="margin-left:6px;font-size:10px;background:var(--phbg);border:1px dashed var(--line);color:var(--mut2)" title="演绎信息未明，未做去重合并">未明演绎</span>';
    }

    h += '<div class="trow version-row" style="cursor:default;display:flex;align-items:center;gap:12px;padding:10px 16px;border-radius:9px;background:var(--hov)">' +
         '<span style="font-family:monospace;font-size:12px;color:var(--mut);min-width:38px">' + yr + '</span>' +
         '<div style="flex:1.3;min-width:0">' +
           '<div style="display:flex;align-items:center">' +
             '<span style="font-weight:600;font-size:13px;color:var(--txt);overflow:hidden;text-overflow:ellipsis;white-space:nowrap" title="' + esc(p.title) + '">' + esc(p.title) + '</span>' +
             badges +
           '</div>' +
           '<div style="font-size:11px;color:var(--mut2);overflow:hidden;text-overflow:ellipsis;white-space:nowrap;margin-top:2px" title="' + esc(p.performers || '—') + '">' + esc(p.performers || '—') + '</div>' +
         '</div>' +
         '<div style="flex:0.9;min-width:0;font-size:12px;color:var(--txt);overflow:hidden;text-overflow:ellipsis;white-space:nowrap" title="' + esc(conductorDisplay) + '">' + esc(conductorDisplay) + '</div>' +
         '<div style="flex:1.2;min-width:0;font-size:12px;color:var(--txt);overflow:hidden;text-overflow:ellipsis;white-space:nowrap" title="' + esc(ensembleDisplay) + '">' + esc(ensembleDisplay) + '</div>' +
         '<div style="flex:0.9;min-width:0;font-size:12px;color:var(--mut);overflow:hidden;text-overflow:ellipsis;white-space:nowrap" title="' + esc(soloistsDisplay) + '">' + esc(soloistsDisplay) + '</div>' +
         '<div style="font-size:11.5px;color:var(--mut);min-width:75px;max-width:110px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap" title="' + esc(p.label || '—') + '">' + esc(p.label || '—') + '</div>' +
         '<span class=badge style="font-size:11px;min-width:45px;text-align:center">' + esc(p.format || 'CD') + '</span>' +
         '<button class="btn chip-copy" data-url="' + esc(p.url) + '" title="复制版本链接" style="margin-left:auto">' +
           '<svg width=12 height=12 viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" style="vertical-align:-1px;margin-right:4px"><rect x="9" y="9" width="13" height="13" rx="2" ry="2"/><path d="M5 15H4a2 2 0 0 1-2-2V4a2 2 0 0 1 2-2h9a2 2 0 0 1 2 2v1"/></svg>' +
           '复制链接' +
         '</button>' +
         '</div>';
  });
  h += '</div>';

  box.innerHTML = h;

  Array.prototype.forEach.call(box.querySelectorAll('.chip-copy'), function (btn) {
    btn.onclick = function (ev) {
      ev.stopPropagation();
      var u = btn.dataset.url;
      if (!u) return;
      if (navigator.clipboard && navigator.clipboard.writeText) {
        navigator.clipboard.writeText(u).then(function () {
          toast('已复制来源链接');
        }).catch(function () {
          toast('已复制来源链接');
        });
      } else {
        toast('已复制来源链接');
      }
    };
  });
}
function loadTracks(q) {
  S.view = 'tracks';
  S.tracksQuery = q || '';
  // 评审：搜索结果页顶栏搜索框不回显当前关键词 → 这里同步（用户正在输入时值本来就相同，不会打断输入）
  if (D('q') && String(D('q').value) !== String(q || '')) D('q').value = q || '';
  head(q ? ('搜索：' + q) : '曲目', '搜索中…', '');
  api('/api/lib/search?q=' + encodeURIComponent(q || '')).then(function (j) {
    if (S.view !== 'tracks') return;   // 已切走（261023-01 #4 同类问题）→ 丢弃，别覆写当前页面
    var t = j.tracks || [];
    S.currentTracks = t;
    renderTracksView();
  }).catch(function (e) {
    renderPanelError('tracks', function () { loadTracks(q); }, e);
  });
}

function bindTrackRow(e, sorted) {
  var favBtn = e.querySelector('.fav-btn');
  if (favBtn) {
    favBtn.onclick = function (ev) {
      ev.stopPropagation();
      toggleFav(favBtn.dataset.file, ev);
    };
  }
  e.onclick = function (ev) {
    if (ev.target.closest && ev.target.closest('.fav-btn')) return;
    playPath(sorted[+e.dataset.i].path);
  };
  e.oncontextmenu = function (ev) {
    ev.preventDefault();
    addPath(sorted[+e.dataset.i].path);
  };
}

function renderTracksView() {
  if (S.view !== 'tracks') return;
  var t0 = performance.now();
  var t = S.currentTracks || [];
  var q = S.tracksQuery || '';
  var totalCount = t.length;
  head(q ? ('搜索：' + q) : '曲目', totalCount + ' 首', '');
  if (!totalCount) {
    if (q) {
      var emptyH = '<div class=empty id=searchEmpty style="padding:48px 20px;text-align:center">' +
                   '<div style="font-size:15px;font-weight:600;margin-bottom:6px">未找到与 “' + esc(q) + '” 相关的曲目</div>' +
                   '<div style="color:var(--mut2);font-size:12px;margin-bottom:16px">请尝试缩短搜索词，或检查是否有错别字</div>' +
                   '<button class="btn" id=bSearchClear>清空搜索词</button></div>';
      D('list').innerHTML = emptyH;
      if (D('bSearchClear')) D('bSearchClear').onclick = function () {
        if (D('qClear')) D('qClear').click();
        else { D('q').value = ''; loadAlbums(''); }
      };
    } else {
      D('list').innerHTML = '<div class=empty>没有匹配的曲目</div>';
    }
    return;
  }

  var sortMode = S.trackSort || 'default';
  var sorted = t.slice();
  if (sortMode === 'title') {
    sorted.sort(function (a, b) { return (a.title || '').localeCompare(b.title || '', 'zh'); });
  } else if (sortMode === 'artist') {
    sorted.sort(function (a, b) { return (a.artist || '').localeCompare(b.artist || '', 'zh'); });
  } else if (sortMode === 'album') {
    sorted.sort(function (a, b) { return (a.album || '').localeCompare(b.album || '', 'zh'); });
  } else if (sortMode === 'dur') {
    sorted.sort(function (a, b) { return (a.dur || 0) - (b.dur || 0); });
  } else if (sortMode === 'year') {
    sorted.sort(function (a, b) { return (b.year || 0) - (a.year || 0); });
  } else if (sortMode === 'fav') {
    sorted.sort(function (a, b) {
      var fa = FAV_SET.has(a.path) ? 1 : 0;
      var fb = FAV_SET.has(b.path) ? 1 : 0;
      if (fa !== fb) return fb - fa; // 收藏优先排前面
      return (a.title || '').localeCompare(b.title || '', 'zh');
    });
  }

  // 排序控件放列表右上，紧凑（261023-03 第5条）
  var h = '<div style="display:flex;justify-content:flex-end;align-items:center;padding:10px 20px 4px;gap:8px">' +
          '<span style="font-size:12px;color:var(--mut2)">排序：</span>' +
          '<select id=trackSort class="track-sort sort-select">' +
          '<option value=default' + (sortMode === 'default' ? ' selected' : '') + '>默认排序</option>' +
          '<option value=title' + (sortMode === 'title' ? ' selected' : '') + '>标题</option>' +
          '<option value=artist' + (sortMode === 'artist' ? ' selected' : '') + '>艺术家</option>' +
          '<option value=album' + (sortMode === 'album' ? ' selected' : '') + '>专辑</option>' +
          '<option value=dur' + (sortMode === 'dur' ? ' selected' : '') + '>时长</option>' +
          '<option value=year' + (sortMode === 'year' ? ' selected' : '') + '>年份</option>' +
          '<option value=fav' + (sortMode === 'fav' ? ' selected' : '') + '>收藏优先</option>' +
          '</select></div>';

  h += '<div id=trackListRows style="padding:4px 0 24px"></div>';
  D('list').innerHTML = h;

  if (D('trackSort')) {
    D('trackSort').onchange = function () {
      S.trackSort = this.value;
      renderTracksView();
    };
  }

  var BATCH = 300;
  var rendered = 0;
  var rowsContainer = D('trackListRows');

  function appendNextBatch() {
    if (!rowsContainer || S.view !== 'tracks') return;
    if (rendered >= sorted.length) return;
    var next = Math.min(rendered + BATCH, sorted.length);
    var chunkHtml = '';
    for (var i = rendered; i < next; i++) {
      var x = sorted[i];
      var isFav = FAV_SET.has(x.path);
      chunkHtml += '<div class="trow trow-track" data-i=' + i + ' style="grid-template-columns:28px 24px minmax(120px,1.6fr) minmax(0,1fr) 92px 58px;gap:8px">' +
                   '<div class=n>' + (i + 1) + '</div>' +
                   favIconHtml(x.path, isFav) +
                   '<div class=tt title="' + esc(x.title) + '">' + esc(x.title) + '</div>' +
                   '<div class=ar title="' + esc(x.artist + ' \u2014 ' + x.album) + '">' + esc(x.artist) + ' \u2014 ' + esc(x.album) + '</div>' +
                   '<div class=bd>' + fmtTag(x.sr, x.bits) + '</div>' +
                   '<div class=du>' + fmt(x.dur) + '</div></div>';
    }
    var temp = document.createElement('div');
    temp.innerHTML = chunkHtml;
    while (temp.firstChild) {
      var node = temp.firstChild;
      bindTrackRow(node, sorted);
      rowsContainer.appendChild(node);
    }
    rendered = next;
    var sub = sorted.length > BATCH ? (sorted.length + ' 首 · 已显示 ' + rendered + ' 首') : (sorted.length + ' 首');
    head(q ? ('搜索：' + q) : '曲目', sub, '');
  }

  appendNextBatch();
  window._trackFirstRenderMs = performance.now() - t0;

  var mainEl = document.querySelector('.main');
  if (mainEl) {
    mainEl.onscroll = function () {
      if (S.view !== 'tracks' || rendered >= sorted.length) return;
      if (mainEl.scrollTop + mainEl.clientHeight >= mainEl.scrollHeight - 600) {
        appendNextBatch();
      }
    };
  }
}

/* ---------------- 文件夹 ---------------- */
function loadFiles(d) {
  S.view = 'files';
  api('/api/files?dir=' + encodeURIComponent(d || '')).then(function (j) {
    // 与 loadTracks 同一类问题（261023-01 #4）：请求在路上时用户可能已经切走，
    // 迟到的响应不许覆写当前页面 —— 否则「文件夹」偶发空白/闪回别的页（本轮自检就踩到）。
    if (S.view !== 'files') return;
    if (!j.ok) { D('list').innerHTML = '<div class=empty>' + esc(j.error) + '</div>'; return; }
    S.dir = j.dir;
    head('文件夹', '', '');
    var rows = [];
    if (base(j.dir).length > 2) rows.push({ k: 'up', n: '返回上一级', m: '' });
    (j.dirs || []).forEach(function (n) { rows.push({ k: 'dir', n: n, m: '文件夹' }); });
    (j.files || []).forEach(function (n) { rows.push({ k: 'file', n: n, m: '' }); });
    S.files = rows;
    D('cnt').textContent = rows.filter(function (r) { return r.k !== 'up'; }).length + ' 项';

    // 地址栏：可以直接输入路径，也可以从音乐库根目录跳
    var roots = S.roots || [];
    var opts = '';
    roots.forEach(function (r) { opts += '<option value="' + esc(r) + '">' + esc(nam(r)) + '</option>'; });
    var h = '<div class=pathbar>' +
            '<button class=pb id=fUp title="上一级">\u2191</button>' +
            (opts ? '<select class=psel id=fRoot><option value="">音乐库…</option>' + opts + '</select>' : '') +
            '<input class=pin id=fPath value="' + esc(j.dir) + '" spellcheck=false placeholder="输入目录后回车，例如 D:\\Music">' +
            '<button class=pb id=fGo title="转到">\u21b5</button></div>';
    h += '<div class=frows>';
    rows.forEach(function (r, i) {
      var isd = (r.k === 'dir' || r.k === 'up');
      var ic = (r.k === 'up') ? '\u2191' : (isd ? '\u{1F4C1}' : '\u266B');
      h += '<div class="row' + (isd ? ' rowdir' : '') + '" data-i=' + i + '>' +
           '<span class=ico>' + ic + '</span>' +
           '<span class="nm' + (isd ? ' dir' : '') + '">' + esc(r.n) + '</span>' +
           '<span class=meta>' + esc(r.m) + '</span></div>';
    });
    h += '</div>';
    D('list').innerHTML = rows.length ? h : h + '<div class=empty>此目录没有音频文件</div>';

    D('fGo').onclick = function () {
      var v = (D('fPath').value || '').trim();
      if (v) loadFiles(v);
    };
    D('fPath').onkeydown = function (e) { if (e.key === 'Enter') D('fGo').click(); };
    if (D('fUp')) D('fUp').onclick = function () {
      var parts = base(S.dir).split(SEP); parts.pop();
      loadFiles(parts.join(SEP) + SEP);
    };
    if (D('fRoot')) D('fRoot').onchange = function () { if (this.value) loadFiles(this.value); };

    Array.prototype.forEach.call(D('list').querySelectorAll('.row'), function (e) {
      if (e.dataset.i === undefined) return;
      var r = S.files[+e.dataset.i];
      e.onclick = function () {
        if (r.k === 'up') {
          var parts = base(S.dir).split(SEP); parts.pop();
          loadFiles(parts.join(SEP) + SEP);
        } else if (r.k === 'dir') loadFiles(join(S.dir, r.n) + SEP);
        else playPath(join(S.dir, r.n));
      };
      e.oncontextmenu = function (ev) { if (r.k === 'file') { ev.preventDefault(); addPath(join(S.dir, r.n)); } };
    });
  }).catch(function (e) {
    renderPanelError('files', function () { loadFiles(d); }, e);
  });
}

/* ---------------- 最近播放 ---------------- */
function loadRecent() {
  S.view = 'recent';
  head('最近播放', '加载中…', '');
  api('/api/recent').then(function (j) {
    var it = j.items || [];
    head('最近播放', it.length + ' 首', '');
    if (!it.length) { D('list').innerHTML = '<div class=empty>还没有播放记录</div>'; return; }
    var h = '<div style="padding:6px 0 24px">';
    it.forEach(function (x, i) {
      h += '<div class="trow" data-i=' + i + ' style="grid-template-columns:34px 1fr 160px 92px">' +
           '<div class=n>' + (i + 1) + '</div>' +
           '<div class=tt title="' + esc(x.path) + '">' + esc(x.title || nam(x.path)) + '</div>' +
           '<div class=ar>' + esc(x.artist) + '</div>' +
           '<div class=du>' + esc(x.album) + '</div></div>';
    });
    D('list').innerHTML = h + '</div>';
    Array.prototype.forEach.call(D('list').querySelectorAll('.trow'), function (e) {
      e.onclick = function () { playPath(it[+e.dataset.i].path); };
      e.oncontextmenu = function (ev) { ev.preventDefault(); addPath(it[+e.dataset.i].path); };
    });
  }).catch(function (e) {
    renderPanelError('recent', loadRecent, e);
  });
}

/* ---------------- 我的最爱 / 保存的队列 / 播放列表（用户 261026-08 #3/#5/#6）----------------
   数据源：
     #5 我的最爱   = 既有收藏机制（/api/fav 红心，落盘 rlplayer-favs.json）
                     + meta=1 从库索引补标题/艺术家/专辑（库里没有就退回文件名）
     #6 保存的队列 = rlplayer-savedqueues.conf（原子写，重启后仍在）
     #3 播放列表   = playlists\*.m3u8（/api/pl，本轮补 create/rename/add/addmany/remove/info）
   为什么三个东西分行放：它们各自的后端与持久化形态不同，混在一起以后必然又变成"两处真相"。 */

// 通用文本输入弹窗。为什么不用 window.prompt：WebView2 里 JS 对话框要宿主实现
// ScriptDialogOpening，且 CDP 自检时对话框会把页面挂住（拿不到 DOM 就无法断言）。统一走 #modal。
function askText(o) {
  o = o || {};
  var m = D('modal'); if (!m) return;
  D('mtitle').textContent = o.title || '输入';
  D('mbody').innerHTML =
    '<div style="padding:2px 0 14px"><input id=askIn style="width:100%;padding:9px 11px;border-radius:9px;' +
    'border:1px solid var(--line);background:var(--field);color:var(--txt);font-size:13px" value="' +
    esc(o.value || '') + '" placeholder="' + esc(o.placeholder || '') + '"></div>' +
    (o.hint ? '<div class=hint style="margin:-8px 0 12px">' + esc(o.hint) + '</div>' : '') +
    '<div style="display:flex;gap:8px;justify-content:flex-end">' +
    '<button class=btn id=askCancel>取消</button>' +
    '<button class="btn pri" id=askOk>' + esc(o.ok || '确定') + '</button></div>';
  m.classList.add('on');
  var inp = D('askIn');
  D('askOk').onclick = function () {
    var v = (D('askIn').value || '').trim();
    if (!v) { toast('名字不能为空'); return; }
    m.classList.remove('on');
    if (o.onOk) o.onOk(v);
  };
  D('askCancel').onclick = function () { m.classList.remove('on'); };
  if (inp) {
    inp.onkeydown = function (ev) {
      if (ev.key === 'Enter') { ev.preventDefault(); D('askOk').click(); }
      else if (ev.key === 'Escape') { ev.preventDefault(); D('askCancel').click(); }
    };
    setTimeout(function () { try { inp.focus(); inp.select(); } catch (e) {} }, 30);
  }
}

// 路径里的上级目录名（本地实现，不走 parentFolder：那个函数是给 Windows 单反斜杠路径写的旧实现）
function folderOf(p) {
  var s = String(p || '');
  var i = Math.max(s.lastIndexOf(String.fromCharCode(92)), s.lastIndexOf('/'));
  if (i <= 0) return '';
  var head = s.slice(0, i);
  var j = Math.max(head.lastIndexOf(String.fromCharCode(92)), head.lastIndexOf('/'));
  return head.slice(j + 1);
}

/* ---------------- 整列起播（#6 队列播放 / #3 播放整列 / #1 我的最爱双击共用） ----------------
   语义（用户原话）：从选中的那一首开始，顺序播放整个列表。
   实现：一次性替换队列（/api/queue/set）→ 播第 i 首（/api/play 会在队列里就地定位 index）。
   关键点：单曲循环会让"顺序播放"只播第一首 ⟹ 先退到专辑循环。**这是行为保证，不是只改图标**。
   2026-10-27（261026-09 #1）：抽出带起播下标的版本 —— 我的最爱要求「双击任何一首，
   从这首开始顺序播放整个列表」，而原来的 playWholeList 只会从第 1 首开始。 */
function ensureSequentialMode(cb) {
  var cur = parseInt(D('sRep') ? D('sRep').value : 0, 10) || 0;
  if (cur !== 2) { cb(); return; }
  D('sRep').value = '1';
  api('/api/playmode?mode=1').then(function () { applyRep(1); cb(); }).catch(function () { cb(); });
}
function playWholeListFrom(files, start, label) {
  files = (files || []).filter(function (f) { return !!f; });
  if (!files.length) { toast('这个列表是空的'); return; }
  var i = parseInt(start, 10) || 0;
  if (i < 0) i = 0;
  if (i > files.length - 1) i = files.length - 1;
  ensureSequentialMode(function () {
    api('/api/queue/set', 'files=' + encodeURIComponent(files.join(String.fromCharCode(10)))).then(function (j) {
      if (!j || j.ok === false) { toast((j && j.error) || '建立队列失败'); return; }
      playPath(files[i]);
      toast('从第 ' + (i + 1) + ' 首开始顺序播放' + (label ? ('：' + label) : '') + '（共 ' + files.length + ' 首）');
      if (S.view === 'queue') renderQueuePage();
    }).catch(function (e) { toast('播放失败: ' + e); });
  });
}
function playWholeList(files, label) { playWholeListFrom(files, 0, label); }

/* ---------------- 我的最爱（#1：2026-10-27 用户 261026-09 重做） ----------------
   用户原话：「我的最爱设计好丑，我的最爱不需要下面出现保存的队列，只需要展示所有点心的
   歌曲即可，也不需要上面的播放全部按钮，双击任何一首歌，然后顺序播放即可」。
   因此这一页现在只有一张列表，交互只有一条规则：
     ① 列表 = 所有点过红心的曲目（序号 / 标题 / 艺术家 / 专辑 / 红心可取消）；
     ② **双击任一首 ⟹ 整个最爱列表进队列、从这首开始顺序播放**（不是只播这一首）；
     ③ 单击只选中（不播放）—— 否则双击会先播单曲再播整列，听起来像"卡了一下又从头"。
   底部不再挂「我的歌单」区块（那是左栏的事），顶部不再有「▶ 播放全部」。 */
function loadFavs() {
  S.view = 'favs';
  head('我的最爱', '加载中…', '');
  api('/api/fav/list?meta=1').then(function (j) {
    if (S.view !== 'favs') return;
    var raw = (j && j.files) || [];
    S.favs = raw.map(function (x) { return (typeof x === 'string') ? { file: x } : x; });
    FAV_SET = new Set(S.favs.map(function (x) { return x.file; }));
    var n = S.favs.length;
    head('我的最爱', n ? (n + ' 首') : '', '');   // #1：不要「▶ 播放全部」
    if (!n) {
      D('list').innerHTML = '<div class=empty>还没有收藏<br><span style="font-size:11.5px">在曲目行或单曲页点心形，就会出现在这里</span></div>';
      return;
    }
    var h = '<div class=favhint><b>双击</b>任意一首，从这首开始<b>顺序播放</b>整个最爱列表（' + n + ' 首）</div>';
    // 列标题（序号/标题/艺术家/专辑）—— 与 .trow 同一套网格参数，逐列对齐
    h += '<div class=favhead style="grid-template-columns:34px 1fr 180px 200px 34px">' +
         '<div class=n>#</div><div>标题</div><div>艺术家</div><div class=du>专辑</div><div></div></div>';
    h += '<div style="padding:2px 0 6px">';
    S.favs.forEach(function (x, i) {
      var f = x.file || '';
      var t = x.title || nam(f);
      var ar = x.artist || '';
      var al = x.album || folderOf(f);
      h += '<div class="trow favrow" data-i=' + i + ' style="grid-template-columns:34px 1fr 180px 200px 34px">' +
           '<div class=n>' + (i + 1) + '</div>' +
           '<div class=tt title="' + esc(f) + '">' + esc(t) + '</div>' +
           '<div class=ar title="' + esc(ar) + '">' + esc(ar) + '</div>' +
           '<div class=du title="' + esc(al) + '" style="text-align:left;overflow:hidden;text-overflow:ellipsis;white-space:nowrap">' + esc(al) + '</div>' +
           '<div style="display:flex;justify-content:flex-end">' + favIconHtml(f, true) + '</div></div>';
    });
    h += '</div>';
    D('list').innerHTML = h;
    Array.prototype.forEach.call(D('list').querySelectorAll('.favrow'), function (e) {
      var i = +e.dataset.i, x = S.favs[i]; if (!x) return;
      // 单击 = 选中；双击 = 从这首起顺序播放整个最爱列表（#1 的核心行为）
      e.onclick = function (ev) {
        if (ev.target.closest && ev.target.closest('.fav-btn')) return;
        Array.prototype.forEach.call(D('list').querySelectorAll('.favrow'), function (y) {
          y.classList.toggle('fsel', y === e);
        });
      };
      e.ondblclick = function (ev) {
        if (ev.target.closest && ev.target.closest('.fav-btn')) return;
        playWholeListFrom((S.favs || []).map(function (y) { return y.file; }), i, '我的最爱');
      };
      e.oncontextmenu = function (ev) { ev.preventDefault(); showTrackMenu(ev.clientX, ev.clientY, { path: x.file }); };
      var b = e.querySelector('.fav-btn');
      if (b) b.onclick = function (ev) { unfavFromFavs(x.file, ev); };
    });
  }).catch(function (e) { renderPanelError('favs', loadFavs, e); });
}
function unfavFromFavs(file, ev) {
  if (ev) { ev.stopPropagation(); ev.preventDefault(); }
  api('/api/fav?file=' + encodeURIComponent(file) + '&on=0').then(function (j) {
    if (!j || !j.ok) { toast((j && j.error) || '取消收藏失败'); return; }
    FAV_SET.delete(file);
    toast('已取消收藏');
    if (S.view === 'favs') loadFavs();
  }).catch(function (e) { toast('取消收藏失败: ' + e); });
}

// 确认弹窗（同样不用 window.confirm：WebView2/CDP 里原生对话框会挂住页面，测不了也点不动）
function askConfirm(o) {
  o = o || {};
  var m = D('modal'); if (!m) return;
  D('mtitle').textContent = o.title || '确认';
  D('mbody').innerHTML =
    '<div style="padding:2px 0 16px;font-size:12.5px;line-height:1.7;color:var(--txt2)">' + esc(o.text || '') + '</div>' +
    '<div style="display:flex;gap:8px;justify-content:flex-end">' +
    '<button class=btn id=cfNo>取消</button>' +
    '<button class="btn pri" id=cfYes>' + esc(o.ok || '确定') + '</button></div>';
  m.classList.add('on');
  D('cfNo').onclick = function () { m.classList.remove('on'); };
  D('cfYes').onclick = function () { m.classList.remove('on'); if (o.onOk) o.onOk(); };
}

/* ---------------- 我的歌单（原「保存的队列」，#2：只改名，功能不变） ----------------
   后端仍是 /api/sq/*，落盘 rlplayer-savedqueues.conf（原子写）。
   展示位置：**只**在左栏「我的最爱」下面（renderSavedQueues / paintSavedQueues）。
   2026-10-27（261026-09 #2）：用户「下面保存的队列改为『我的歌单』，功能不变；
   从播放队列那里直接保存为歌单即可」。所以：
     * 本页（我的最爱）不再挂这个区块 —— 原来那份 savedQueuesSectionHtml/bindSavedQueueSection
       已删除（用户 #1 明确「不需要下面出现保存的队列」）；
     * 保存入口在**播放队列页**的「保存为歌单」按钮（id=qSaveSq，见 renderQueuePage）。 */
// 左栏列表（我的最爱下面 = 「我的歌单」）：两段式 —— 取数（renderSavedQueues）/ 上屏（paintSavedQueues）。
// 拆开是为了保存/改名/删除后能只刷左栏、不动当前打开的页面。
function renderSavedQueues() {
  api('/api/sq/list').then(function (j) {
    S.sqs = (j && j.queues) || [];
    paintSavedQueues();
  }).catch(function () {});
}
function paintSavedQueues() {
  {
    var box = D('sqList'); if (!box) return;
    // #2：标题固定为「我的歌单」（原「保存的队列」）。空列表也给一行说明，
    // 否则用户不知道左栏这一块是干什么的、也不知道去哪儿存。
    var h = '<div class=sqhd>我的歌单</div>';
    if (!S.sqs.length) {
      h += '<div class=sqempty>队列页「保存为歌单」可新建</div>';
    }
    S.sqs.forEach(function (q, i) {
      h += '<div class="nav sqrow" data-v=sq data-sq="' + esc(q.name) + '" data-i=' + i + '>' +
           '<span class=sqn title="' + esc(q.name) + '">' + esc(q.name) + '</span>' +
           '<span class="sqb ren" title="重命名">✎</span>' +
           '<span class="sqb del" title="删除">×</span></div>';
    });
    box.innerHTML = h;
    Array.prototype.forEach.call(box.querySelectorAll('.sqrow'), function (e) {
      var q = S.sqs[+e.dataset.i]; if (!q) return;
      e.onclick = function (ev) {
        if (ev.target.classList.contains('sqb')) return;
        renderSavedQueuePage(q.name);
      };
      var r1 = e.querySelector('.ren'); if (r1) r1.onclick = function (ev) { ev.stopPropagation(); renameQueueDialog(q.name); };
      var r2 = e.querySelector('.del'); if (r2) r2.onclick = function (ev) { ev.stopPropagation(); deleteSavedQueue(q.name); };
    });
  }
}
function renderSavedQueuePage(name) {
  if (!name) return;
  S.view = 'sq'; S.sqName = name;
  head(name, '加载中…', '');
  api('/api/sq/info?name=' + encodeURIComponent(name)).then(function (j) {
    if (S.view !== 'sq' || S.sqName !== name) return;
    if (!j || !j.ok) {
      head(name, '', '');
      D('list').innerHTML = '<div class=empty>队列不存在（可能已被删除）</div>';
      return;
    }
    var it = j.items || [];
    S.sqItems = it;
    head(name, it.length + ' 首',
      '<button class="btn pri" id=sqPlayAll>▶ 播放整列</button>' +
      '<button class=btn id=sqRen>重命名</button>' +
      '<button class=btn id=sqDel>删除</button>');
    Array.prototype.forEach.call(document.querySelectorAll('.sqlist .sqrow'), function (e) {
      e.classList.toggle('on', e.dataset.sq === name);
    });
    if (!it.length) { D('list').innerHTML = '<div class=empty>这个队列是空的</div>'; return; }
    var h = '<div style="padding:6px 0 24px">';
    it.forEach(function (x, i) {
      h += '<div class="trow sqtrow" data-i=' + i + ' style="grid-template-columns:34px 1fr 240px">' +
           '<div class=n>' + (i + 1) + '</div>' +
           '<div class=tt title="' + esc(x.file) + '">' + esc(nam(x.file)) + '</div>' +
           '<div class=ar style="text-align:left;overflow:hidden;text-overflow:ellipsis;white-space:nowrap">' + esc(folderOf(x.file)) + '</div></div>';
    });
    D('list').innerHTML = h + '</div>';
    if (D('sqPlayAll')) D('sqPlayAll').onclick = function () { playSavedQueue(name, 0); };
    if (D('sqRen')) D('sqRen').onclick = function () { renameQueueDialog(name); };
    if (D('sqDel')) D('sqDel').onclick = function () { deleteSavedQueue(name); };
    Array.prototype.forEach.call(D('list').querySelectorAll('.sqtrow'), function (e) {
      var i = +e.dataset.i;
      e.onclick = function () { playSavedQueue(name, i); };
    });
  }).catch(function (e) { renderPanelError('sq', function () { renderSavedQueuePage(name); }, e); });
}
function playSavedQueue(name, i) {
  i = i || 0;
  ensureSequentialMode(function () {
    api('/api/sq/play?name=' + encodeURIComponent(name) + '&i=' + i).then(function (j) {
      if (!j || !j.ok) { toast((j && j.error) || '载入失败'); return; }
      api('/api/sq/info?name=' + encodeURIComponent(name)).then(function (info) {
        var it = (info && info.items) || [];
        var k = Math.min(Math.max(0, i), Math.max(0, it.length - 1));
        if (it[k]) playPath(it[k].file, it[k].cs, it[k].ce);
        toast('播放队列：' + name + '（' + it.length + ' 首）');
        loadQ();
      });
    }).catch(function (e) { toast('播放失败: ' + e); });
  });
}
function saveQueueDialog() {
  var n = (S.q && S.q.items) ? S.q.items.length : 0;
  if (!n) { toast('队列是空的，先加几首歌再保存'); return; }
  askText({ title: '保存为歌单', ok: '保存', value: S.sqName || '',
    placeholder: '例如 夜深人静',
    hint: '把现在这 ' + n + ' 首（含顺序与 CUE 区间）存成一个歌单；写入 rlplayer-savedqueues.conf，重启后仍在',
    onOk: function (name) {
      api('/api/sq/save?name=' + encodeURIComponent(name)).then(function (j) {
        if (!j || !j.ok) { toast((j && j.error) || '保存失败'); return; }
        toast('已存为歌单：' + j.name + '（' + j.count + ' 首）');
        renderSavedQueues();
        if (S.view === 'favs') loadFavs();
        else if (S.view === 'sq') renderSavedQueuePage(j.name);
      }).catch(function (e) { toast('保存失败: ' + e); });
    } });
}
function renameQueueDialog(name) {
  askText({ title: '重命名歌单', ok: '改名', value: name,
    hint: '改名后立刻写入 rlplayer-savedqueues.conf（重启后是新名字）',
    onOk: function (to) {
      api('/api/sq/rename?from=' + encodeURIComponent(name) + '&to=' + encodeURIComponent(to)).then(function (j) {
        if (!j || !j.ok) { toast((j && j.error) || '改名失败'); return; }
        toast('已改名为：' + j.to);
        if (S.sqName === name) S.sqName = j.to;
        renderSavedQueues();
        if (S.view === 'sq') renderSavedQueuePage(j.to);
        else if (S.view === 'favs') loadFavs();
      }).catch(function (e) { toast('改名失败: ' + e); });
    } });
}
function deleteSavedQueue(name) {
  askConfirm({ title: '删除歌单', ok: '删除',
    text: '删除歌单「' + name + '」？只删这份歌单，不动当前播放队列。',
    onOk: function () {
      api('/api/sq/delete?name=' + encodeURIComponent(name)).then(function (j) {
        if (!j || !j.ok) { toast((j && j.error) || '删除失败'); return; }
        toast('已删除：' + name);
        if (S.sqName === name) S.sqName = '';
        renderSavedQueues();
        if (S.view === 'sq') { setView('favs'); loadFavs(); }
        else if (S.view === 'favs') loadFavs();
      }).catch(function (e) { toast('删除失败: ' + e); });
    } });
}

/* ---------------- 播放列表 UI 已撤（#2，2026-10-27 用户 261026-09） ----------------
   用户原话：「播放队列和播放列表，这两个是重复设计，只需要播放队列、最近播放、我的最爱这三个」。
   ⟹ 261026-08 #3 刚加的这套 /api/pl 前端（列表页 / 新建 / 加曲 / 改名 / 删除 / 右键「加入播放列表…」）
   全部移除；左栏导航项与 setView('pl') 分支也一起去掉。
   **后端端点保留**（/api/pl、/api/pl/save|rename|delete|add|addmany|remove|info|load 仍在 rtaweb.cpp），
   只是不再有前端入口 —— 想恢复只需把这块 UI 接回来，不必重新设计接口。
   「保存队列」这件事统一走「我的歌单」（/api/sq/*，落 rlplayer-savedqueues.conf，能存 CUE 区间）。 */

/* ---------------- 播放控制 ---------------- */
/* CUE 分轨（2026-10-25）：cue 把整轨切成 N 首，这 N 首的 path 完全相同，
   只有 cue_start/cue_end（秒）能区分。cs < 0 = 整首（旧行为，负向对照就走这条）。
   为什么不传"时长"：播放侧需要的是起点，只给时长等于少一半信息。 */
function cueQuery(cs, ce) {
  if (!(cs >= 0)) return '';
  return '&cue_start=' + encodeURIComponent(cs) +
         (ce >= 0 ? ('&cue_end=' + encodeURIComponent(ce)) : '');
}
function playPath(f, cs, ce) {
  api('/api/play', 'file=' + encodeURIComponent(f) + cueQuery(cs, ce) + '&' + params()).then(function (j) {
    if (j && j.ok === false) { toast(j.error || '播放失败'); return; }
    toast('开始播放'); loadQ();
  });
}
function addPath(f) {
  api('/api/add', 'file=' + encodeURIComponent(f)).then(function () { toast('已加入队列'); loadQ(); });
}
function playRemote(it) {
  api('/api/play', 'rid=' + encodeURIComponent(it.id) + '&rsuffix=' + encodeURIComponent(it.sf || '') + '&' + params())
    .then(function (j) { if (!j.ok) toast(j.error || '播放失败'); else loadQ(); });
}

/* 2026-10-26（261025-02 #3）：通用的「单曲操作菜单」。
   专辑页曲目行、曲目列表、文件夹行都用它，避免每个列表各写一套（以前只有队列项有菜单）。 */
function showTrackMenu(x, y, tk) {
  hideContextMenu();
  var m = document.createElement('div');
  m.className = 'ctxmenu';
  m.style.left = Math.max(4, x) + 'px';
  m.style.top = Math.max(4, y) + 'px';
  function item(label, fn) {
    var d = document.createElement('div');
    d.className = 'ctxitem';
    d.textContent = label;
    d.onclick = function (ev) { ev.stopPropagation(); hideContextMenu(); fn(); };
    m.appendChild(d);
  }
  item('立即播放', function () { playPath(tk.path, tk.cs, tk.ce); });
  item('加入播放队列', function () { addPathCue(tk.path, tk.cs, tk.ce); });
  // 2026-10-27（B2/B3 接线）：音质分析（19 项客观指标）与多版本对比。
  // 两项都只读文件（/api/health 流式解码、/api/compare 对齐后分析），不开音频设备。
  item('分析音质（19 项）', function () { openQualityModal(tk.path); });
  item('对比版本…', function () { openCompareModal(tk.path); });
  // #2（2026-10-27 用户 261026-09）：原来的「加入播放列表…」菜单项已随播放列表 UI 一起撤掉
  //（播放队列 / 播放列表重复，只留队列；要留存就用队列页的「保存为歌单」）。
  document.body.appendChild(m);
  _activeCtxMenu = m;
  // 贴边回推（菜单比窗口大时也不会跑到屏幕外）
  var r = m.getBoundingClientRect();
  if (r.right > innerWidth - 4) m.style.left = Math.max(4, innerWidth - r.width - 4) + 'px';
  if (r.bottom > innerHeight - 4) m.style.top = Math.max(4, innerHeight - r.height - 4) + 'px';
}
// cue 曲目的入队：必须把区间一起带上，否则同一整轨的多首只会入队"整首"
function addPathCue(f, cs, ce) {
  api('/api/add', 'file=' + encodeURIComponent(f) + cueQuery(cs, ce)).then(function (j) {
    if (j && j.ok === false) { toast(j.error || '加入失败'); return; }
    toast('已加入队列'); loadQ();
  });
}

/* ---------------- 队列右键菜单 ---------------- */
var _activeCtxMenu = null;
function hideContextMenu() {
  if (_activeCtxMenu && _activeCtxMenu.parentNode) {
    _activeCtxMenu.parentNode.removeChild(_activeCtxMenu);
  }
  _activeCtxMenu = null;
}
document.addEventListener('click', hideContextMenu);
document.addEventListener('contextmenu', function (ev) {
  if (!ev.target.closest || !ev.target.closest('.qi')) hideContextMenu();
});

function showQueueContextMenu(x, y, idx) {
  hideContextMenu();
  var menu = document.createElement('div');
  menu.className = 'ctx-menu';
  menu.style.left = Math.min(x, window.innerWidth - 150) + 'px';
  menu.style.top = Math.min(y, window.innerHeight - 100) + 'px';

  var itemPlay = document.createElement('div');
  itemPlay.className = 'ctx-item';
  itemPlay.innerHTML = '<span>\u25b6</span><span>立即播放</span>';
  itemPlay.onclick = function (ev) {
    ev.stopPropagation();
    hideContextMenu();
    if (S.q && S.q.items && S.q.items[idx]) {
      var c = (S.q.cues || [])[idx] || [-1, -1];
      playPath(S.q.items[idx], c[0], c[1]);
    }
  };

  var itemDel = document.createElement('div');
  itemDel.className = 'ctx-item danger';
  itemDel.innerHTML = '<span>\u00d7</span><span>从队列移除</span>';
  itemDel.onclick = function (ev) {
    ev.stopPropagation();
    hideContextMenu();
    api('/api/qdel?i=' + idx, '').then(function () { toast('已移除'); loadQ(); });
  };

  menu.appendChild(itemPlay);
  menu.appendChild(itemDel);
  document.body.appendChild(menu);
  _activeCtxMenu = menu;
}

function updateNextTrackInfo() {
  var el = D('npNextTrack');
  var heroEl = D('npNextTrackHero');
  var tqBadge = D('tqBadge');
  var qLen = (S.q && S.q.items) ? S.q.items.length : 0;
  if (tqBadge) {
    if (qLen > 0) {
      tqBadge.textContent = qLen;
      tqBadge.style.display = '';
    } else {
      tqBadge.style.display = 'none';
    }
  }
  if (!S.playing || !qLen) {
    if (el) el.style.display = 'none';
    if (heroEl) heroEl.style.display = 'none';
    return;
  }
  var curIdx = (S.q.index !== undefined ? S.q.index : -1);
  if (curIdx < 0 && S.lastCur) {
    curIdx = S.q.items.indexOf(S.lastCur);
  }
  var repMode = (D('sRep') && D('sRep').value) || '0';
  var nextIdx = curIdx + 1;
  var hasNext = true;
  if (nextIdx >= qLen) {
    if (repMode === '1') {
      nextIdx = 0;
    } else {
      hasNext = false;
    }
  }
  var remain = Math.max(0, qLen - 1 - (curIdx >= 0 ? curIdx : 0));
  var text = '';
  var tip = '';
  if (hasNext && qLen > 0) {
    var nextPath = S.q.items[nextIdx] || '';
    var nextTitle = nam(nextPath);
    text = '下一首: ' + nextTitle + (remain > 0 ? (' · 剩 ' + remain + ' 首') : '');
    tip = '下一首：' + nextTitle + '（队列共 ' + qLen + ' 首，剩余 ' + remain + ' 首）';
  } else {
    text = '队列末尾' + (qLen > 0 ? (' · 共 ' + qLen + ' 首') : '');
    tip = '当前已是队列最后一首（共 ' + qLen + ' 首）';
  }
  if (el) {
    el.textContent = text;
    el.title = tip;
    el.style.display = '';
  }
  if (heroEl) {
    heroEl.textContent = text;
    heroEl.title = tip;
    heroEl.style.display = '';
  }
}

/* ---------------- 队列 ---------------- */
var PM_PENDING = 0;   // 最近一次播放模式写请求的时间戳（毫秒）；loadQ 用它避开"回写旧值"竞态
function loadQ() {
  api('/api/queue').then(function (j) {
    // cues 与 items 逐项对齐的 [起,止] 秒数组（后端 /api/queue；[-1,-1] = 整首）
    S.q = { items: j.items || [], cues: j.cues || [], index: j.index || 0 };
    if (D('sShuf')) D('sShuf').checked = !!j.shuffle;
    // #2：mode 与 repeat 是同一个值（0关/1列表/2单曲/3随机）；老后端没有 mode 时退回 repeat
    var pmd = (j.mode !== undefined) ? j.mode : j.repeat;
    // 2026-10-27（261026-09 实测发现）：点底栏模式按钮会先改 #sRep、再 POST /api/playmode；
    // 这条 3 秒轮询如果恰好在这两步之间返回，就会把用户刚点的值**覆盖回旧值**
    //（表现：点一下没反应；tools/uicheck.js 的「循环键能切换循环方式」因此偶发 0 -> 0）。
    // 写请求后 2 秒内不采信轮询结果，让"最后一次用户操作"胜出。
    var pmFresh = (Date.now() - PM_PENDING) < 2000;
    if (D('sRep') && pmd !== undefined && !pmFresh) { D('sRep').value = String(pmd); applyRep(pmd); }
    D('rcnt').textContent = '共 ' + S.q.items.length + ' 首';
    updateNextTrackInfo();
    if (!S.q.items.length) {
      D('qlist').innerHTML = '<div class=empty style="padding:26px 14px">队列为空<br><span style="font-size:11px">右键专辑或曲目可加入</span></div>';
      return;
    }
    var h = '';
    S.q.items.forEach(function (f, i) {
      h += '<div class="qi' + (i === S.q.index ? ' on' : '') + '" data-i=' + i + ' draggable=true>' +
           '<span class=n>' + (i + 1) + '</span><span class=t title="' + esc(f) + '">' + esc(nam(f)) + '</span>' +
           '<button class=qdel data-i=' + i + ' title="从队列移除">\u00d7</button></div>';
    });
    D('qlist').innerHTML = h;
    Array.prototype.forEach.call(D('qlist').children, function (e) {
      if (e.dataset.i === undefined) return;
      var i = +e.dataset.i;
      e.onclick = function (ev) {
        if (ev.target.classList.contains('qdel')) return;
        var c = (S.q.cues || [])[i] || [-1, -1];
        playPath(S.q.items[i], c[0], c[1]);
      };
      e.oncontextmenu = function (ev) {
        ev.preventDefault();
        showQueueContextMenu(ev.clientX, ev.clientY, i);
      };
      var d = e.querySelector('.qdel');
      if (d) d.onclick = function (ev) {
        ev.stopPropagation();
        api('/api/qdel?i=' + i, '').then(function () { toast('已移除'); loadQ(); });
      };
      e.ondragstart = function (ev) { ev.dataTransfer.setData('text/plain', e.dataset.i); };
      e.ondragover = function (ev) { ev.preventDefault(); };
      e.ondrop = function (ev) {                  // 拖动排序
        ev.preventDefault();
        var from = parseInt(ev.dataTransfer.getData('text/plain'), 10);
        var to = parseInt(e.dataset.i, 10);
        if (isNaN(from) || isNaN(to) || from === to) return;
        api('/api/qmove?from=' + from + '&to=' + to, '').then(loadQ);
      };
    });
  }).catch(function () { });
}

/* ---------------- 元数据 / 封面 ---------------- */
// 正在播放整页的补全信息芯片（底栏不加，避免拥挤）。
// enriched=false 或无 meta → 返回空串（容器清空）；值芯片带「来源」提示，来自 meta.sources 溯源表。
function buildNpMetaHtml(m) {
  var mt = m && m.meta;
  if (!mt || mt.enriched !== true) return '';
  var src = mt.sources || {};
  var chips = '';
  [['year', '年份'], ['genre', '流派'], ['albumArtist', '专辑艺术家']].forEach(function (p) {
    var v = m[p[0]];
    if (!v) return;
    chips += '<span class=badge title="' + esc('来源: ' + (src[p[0]] || '本地标签')) + '">' +
             esc(p[1] + '  ' + v) + '</span>';
  });
  chips += '<span class="badge g" title="标签/封面/歌词已由在线来源补全，可在设置-元数据补全关闭">已补全</span>';
  return chips;
}
function loadMeta(f) {
  if (!f) return;
  api('/api/meta?file=' + encodeURIComponent(f)).then(function (m) {
    S.curMeta = m;
    var tName = m.title || nam(f);
    D('npt').textContent = tName;
    D('npt').title = tName;
    var sub = [];
    if (m.artist) sub.push(m.artist);
    if (m.album) sub.push(m.album);
    var subStr = sub.join(' \u00b7 ');
    D('npart').textContent = subStr;
    D('npart').title = subStr;
    D('cv').style.backgroundImage = m.hasCover ? "url('/api/cover?file=" + encodeURIComponent(f) + "')" : '';
    // 补全芯片：存全局，renderNp 统一填进 #npMeta；整页开着就立刻刷一次（meta 是异步回来的）
    S.npMetaHtml = buildNpMetaHtml(m);
    if (D('npPage') && D('npPage').classList.contains('on')) {
      renderNp();   // renderNp 内部已调 updateNpSummary(S.curMeta)，不再重复请求
    }
  }).catch(function () {
    var tName = nam(f);
    D('npt').textContent = tName;
    D('npt').title = tName;
  });
}

/* ---------------- 状态 ---------------- */
// 2026-10-25（261025-02 #1）：底栏徽标从两态扩成六态 —— 非升频的 bit-perfect / 非 bit-perfect / bit，
// 以及三种升频方向（PCM→PCM、PCM→DSD、DSD→DSD）。判据优先用后端权威字段，缺字段才退回推导：
//   direct            = DirectSDM 1-bit 直通（最严格）
//   resample=false    = 没有重采样 ⟹ 落在「非升频」两态
//   resample=true     = 升频；再看输出是不是 DSD（dsd 字段非 0），源是不是 DSD（按当前文件扩展名）
function srcIsDsdPath(p) { return /\.(dsf|dff)$/i.test(String(p || '')); }
function bpBadgeInfo() {
  var bpv = S.bpState || '';
  var cur = S.cur || (D('npt') && D('npt').title) || '';
  var srcDsd = srcIsDsdPath(cur);
  var up = !!S.bpResample;
  var dsdOut = !!S.bpDsdOut || S.bpOutMode === 'dop' || S.bpOutMode === 'native';
  var t = '', c = '', title = S.bpWhy || '';
  if (bpv === 'direct') { t = 'BIT-PERFECT·1bit'; c = 'bp-direct'; title = title || 'DirectSDM：DSD 1-bit 直通，跳过 PCM 与调制'; }
  else if (!up) {
    if (bpv === 'yes') { t = 'bit-perfect'; c = 'bp-yes'; title = title || '不升频 · 0 dB · 无抖动/整形 · 无 EQ/IR'; }
    else if (bpv === 'no') { t = '非 bit-perfect'; c = 'bp-no'; title = title || '没有升频，但链路里有改动（原因见信号链）'; }
    else { t = 'bit'; c = 'bp-no'; title = title || '直通，但没有拿到后端的 bit-perfect 结论'; }
  } else if (dsdOut) {
    t = srcDsd ? 'DSD-DSD 升频' : 'PCM-DSD 升频';
    c = 'bp-up'; title = title || (srcDsd ? 'DSD 源 → 更高 DSD 档（经解码 + 重新调制）' : 'PCM 源 → DSD（SDM 调制）');
  } else {
    t = 'PCM-PCM 升频'; c = 'bp-up'; title = title || 'PCM 源 → 更高 PCM 采样率';
  }
  if (S.bpFallback) title = (title ? title + '；' : '') + '降级原因：' + S.bpFallback;
  return { text: t, cls: c, title: title };
}

function tick() {
  api('/api/status').then(function (j) {
    S.playing = !!j.playing; S.paused = !!j.paused; S.dur = j.dur; S.pos = j.pos;
    // 2026-10-23 #1：底栏 bit-perfect 徽标的数据源（后端权威判定，见 /api/status 的 bitPerfect 字段）
    S.bpState = j.bitPerfect || ""; S.bpWhy = j.bitPerfectWhy || "";
    // 2026-10-25（261025-02 #1）：徽标要分「升频方向」，需要这次播放的真实链路口径。
    // resample=是否重采样；dsd=输出 DSD 档（0=不是 DSD 输出）；outMode 是后端新加的权威字段（没有就退回推导）。
    S.bpResample = !!j.resample;
    S.bpDsdOut = !!(j.dsd && String(j.dsd) !== '0');
    S.bpOutMode = j.outMode || '';
    S.bpFallback = j.fallbackWhy || '';
    // 2026-10-23 261023-03 #2/#5：资料简介的显示门控只用后端权威状态（400ms 轮询顺带更新）
    if (typeof j.hasLlmKey === 'boolean') META.hasLlmKey = j.hasLlmKey;
    if (typeof j.summaryEnabled === 'boolean') META.summaryEnabled = j.summaryEnabled;
    // 正在播放整页：进度条 / 时间 / 播放图标
    if (D('npPage') && D('npPage').classList.contains('on')) {
      if (D('npBar') && S.dur > 0) D('npBar').style.width = Math.min(100, S.pos / S.dur * 100) + '%';
      if (D('npPos')) D('npPos').textContent = fmt(S.pos);
      if (D('npDur')) D('npDur').textContent = fmt(S.dur);
      if (D('npIcP')) D('npIcP').setAttribute('d', (S.playing && !S.paused)
        ? 'M7 5h4v14H7zM13 5h4v14h-4z' : 'M8 5l12 7-12 7z');
      syncVolUi();     // 音量可能在别处改（键盘 ↑↓ / 底栏滑块），整页跟着走
      renderNpLyric();
    }
    S.volDb = (typeof j.volDb === 'number') ? j.volDb : S.vol;
    S.startDb = (typeof j.startDb === 'number') ? j.startDb : S.vol;
    // 信号链弹层开着时，音量节点要跟着实时变（否则就是在显示过期数据）
    if (D('chainVol') && D('modal').classList.contains('on')) D('chainVol').textContent = volText(S.volDb);
    if (D('chainTiming') && D('modal').classList.contains('on')) {
      var dspB = (typeof j.dspBlockMs === 'number' && j.dspBlockMs > 0) ? j.dspBlockMs.toFixed(2) + ' ms/块' : '';
      var dspT = (typeof j.dspTotalMs === 'number' && j.dspTotalMs > 0) ? '（累计 ' + j.dspTotalMs.toFixed(1) + ' ms）' : '';
      var dspStr = (dspB || dspT) ? (dspB + dspT) : '–';
      var decStr = (typeof j.decodeMs === 'number' && j.decodeMs > 0) ? (j.decodeMs >= 1000 ? (j.decodeMs/1000).toFixed(2) + ' s' : j.decodeMs.toFixed(1) + ' ms') : '–';
      var wavStr = '–';
      if (typeof j.ffmpegMs === 'number' && j.ffmpegMs > 0) {
        wavStr = '转码写 ' + j.ffmpegMs.toFixed(1) + ' ms' + (typeof j.wavReadMs === 'number' && j.wavReadMs > 0 ? ' / 读 ' + j.wavReadMs.toFixed(1) + ' ms' : '');
      } else if (typeof j.wavReadMs === 'number' && j.wavReadMs > 0) {
        wavStr = '读取 ' + j.wavReadMs.toFixed(1) + ' ms';
      }
      var scanStr = (typeof j.scanMs === 'number' && j.scanMs > 0) ? (j.scanMs >= 1000 ? (j.scanMs/1000).toFixed(2) + ' s' : j.scanMs.toFixed(0) + ' ms') : '–';
      D('chainTiming').textContent = '解码 ' + decStr + '  \u00b7  临时WAV ' + wavStr + '  \u00b7  DSP ' + dspStr + '  \u00b7  库扫描 ' + scanStr;
    }
    var pct = (j.dur > 0 ? Math.min(100, j.pos / j.dur * 100) : 0) + '%';
    if (D('bar')) D('bar').style.width = pct;
    D('tpos').textContent = fmt(j.pos);
    D('tdur').textContent = fmt(j.dur);
    D('icP').setAttribute('d', S.playing && !S.paused
      ? 'M7 5h4v14H7zM13 5h4v14h-4z'                     /* 暂停图标 */
      : 'M8 5v14l11-7z');                                 /* 播放图标 */
    if (j.current) {
      if (S.lastCur !== j.current) { S.lastCur = j.current; loadMeta(j.current); loadLyrics(j.current); }
      syncLyrics(j.pos);
      D('nps').textContent = (j.playing ? (j.paused ? '已暂停' : '播放中') : '已停止') +
        (j.outRate ? ' \u00b7 ' + j.outRate + ' Hz / ' + j.outBits + ' bit' : '');
      D('fmt').textContent = j.outRate ? (Math.round(j.outRate / 100) / 10) + 'k/' + j.outBits : '--';
      updateNextTrackInfo();
      // 261024-04 #8：底栏 GPU 徽标只表示「GPU **真的参与**了本次播放」。
      // 后端新字段 gpuUsed（本次播放 GPU 是否实际参与）；字段还没上线时回退旧的 gpu 字段。
      // 选择"不参与就不亮"而不是"显示灰色 GPU 可用·未参与"：底栏右侧本来就挤，
      // 常驻一个灰 chip 会把"绿点 = GPU 参与"这个唯一的强信号稀释掉；可用性在设置→高级与信号链里能查到。
      S.gpuUsed = (typeof j.gpuUsed === 'boolean') ? j.gpuUsed : !!j.gpu;
      S.gpuOn = !!j.gpu;
      D('gpub').style.display = S.gpuUsed ? '' : 'none';
      D('gpub').title = S.gpuUsed ? 'GPU 参与本次播放'
        : (S.gpuOn ? 'GPU 可用 · 本次未参与（信号链里绿色节点才会亮）' : 'GPU 未参与本次播放');
    } else {
      S.gpuUsed = false; S.gpuOn = false;
      D('fmt').textContent = '--';
      D('gpub').style.display = 'none';
      if (S.lastCur) { S.lastCur = ''; D('npt').textContent = '未播放'; D('npart').textContent = ''; D('nps').textContent = '选择一个专辑开始'; D('cv').style.backgroundImage = ''; S.npMetaHtml = ''; if (D('npMeta')) D('npMeta').innerHTML = ''; }
      updateNextTrackInfo();
    }
    // 2026-10-23 #1：底栏常驻一个「b-p」徽标，不打开信号链也能知道当前是否 bit-perfect。
    // 2026-10-23 用户 261023-04 #1：改成紧凑 chip（class=bp-yes/bp-no/bp-direct），
    // 配色与显隐交给 CSS（三套主题各有一套颜色），这里不写死颜色、也不占中间列。
    if (D('bpBadge')) {
      var bi = bpBadgeInfo();
      D('bpBadge').textContent = bi.text;
      D('bpBadge').className = bi.cls;
      D('bpBadge').title = bi.title;
    }
    if (j.error) D('nps').textContent = j.error;
    syncAlbPlayAllBtn();
  }).catch(function () { });
}

/* ---------------- 播放设置持久化 ---------------- */
var SET_KEYS = [['sRate','rate'], ['sFilt','filter'], ['sFiltNx','filterNx'], ['sShap','shaper'],
                ['sDSD','dsd'], ['sBuf','buffer'], ['sIsp','isp'], ['s2s','twostage'], ['sGpu','gpuvendor']];
function saveSettings(immediate) {
  var body = '';
  SET_KEYS.forEach(function (p) {
    var e = D(p[0]); if (!e) return;
    var v = (e.type === 'checkbox') ? (e.checked ? '1' : '0') : e.value;
    if (body) body += '&';
    body += p[1] + '=' + encodeURIComponent(v);
  });
  body += '&volume=' + S.vol + '&loudness=' + (D('sLoud').checked ? '1' : '0');
  body += '&rgmode=' + (ADV.rgmode || 'off') + '&rgclip=' + (ADV.rgclip ? 1 : 0);
  body += '&theme=' + encodeURIComponent(S.theme || 'dark');
  body += '&device=' + encodeURIComponent(ADV.device || '') + '&bits=' + (ADV.bits || 0)
        + '&taps=' + (ADV.taps || 0) + '&atten=' + (ADV.atten || 0)
        + '&transition=' + (ADV.transition || 0) + '&phase=' + encodeURIComponent(ADV.phase || '')
        + '&modorder=' + (ADV.modorder || 5) + '&modfamily=' + encodeURIComponent(ADV.modfamily || 'dsd') + '&modcut=' + (ADV.modcut || 0)
        + '&moddith=' + (ADV.moddith === undefined ? -1 : ADV.moddith)
        + '&eq=' + encodeURIComponent(ADV.eq || '')
        + '&ir=' + encodeURIComponent(ADV.ir || '') + '&irgpu=' + (ADV.irgpu || 0)
        + '&ramp=' + (ADV.ramp || 0)
        + '&backend=' + encodeURIComponent(ADV.backend || 'wasapi')
        + '&channels=' + (ADV.channels || 0)
        + '&naahost=' + encodeURIComponent(ADV.naaHost || '')
        + '&upnpdev=' + encodeURIComponent(ADV.upnpDev || '')
        + '&trim=' + [0, 1, 2, 3, 4, 5, 6, 7].map(function (i) { return ADV['trim' + i] || 0; }).join(',')
        + '&inbackend=' + encodeURIComponent(ADV.inBackend || 'none')
        + '&indev=' + encodeURIComponent(ADV.inDev || '')
        + '&inoffset=' + (ADV.inOffset || 0)
        + '&inbuf=' + (ADV.inBuf || 100)
        + '&inmon=' + (ADV.inMonitor || 0)
        + '&inseconds=' + (ADV.inSeconds || 0)
        + '&inout=' + encodeURIComponent(ADV.inOut || '')
        + '&sdmfilt=' + encodeURIComponent(ADV.sdmFilt || '')
        + '&sdmfiltnx=' + encodeURIComponent(ADV.sdmFiltNx || '')
        + '&directsdm=' + (ADV.directsdm ? 1 : 0)
        + '&dsdkind=' + (ADV.dsdKind || 'fir') + '&dsdconv=' + (ADV.dsdConv || 'narrow')
        + '&scanthreads=' + (ADV.scanThreads !== undefined ? ADV.scanThreads : 0)
        + '&opraproduct=' + encodeURIComponent(ADV.opraproduct || '')
        + '&oprapreset=' + encodeURIComponent(ADV.oprapreset || '')
        // 元数据补全（P1.5）：metaon/metallm 固定两键；源开关按注册表逐源写 metasrc_<id>; metasummary 默认0
        + '&metaon=' + (META.on ? 1 : 0)
        + '&metallm=' + (META.llm ? 1 : 0)
        + '&metasummary=' + (META.summary ? 1 : 0)
        + (META_PROVIDERS || []).map(function (p) {
            return '&metasrc_' + p.id + '=' + (META_SRC[p.id] === 0 ? 0 : 1);
          }).join('');
  clearTimeout(window._setT);
  if (immediate) {
    api('/api/settings', body);
  } else {
    window._setT = setTimeout(function () { api('/api/settings', body); }, 300);
  }
}
function loadSettings() {
  api('/api/settings').then(function (j) {
    var kv = j.kv || {};
    if (kv.scanThreads !== undefined || kv.scanthreads !== undefined) {
      ADV.scanThreads = parseInt(kv.scanThreads || kv.scanthreads, 10) || 0;
    }
    SET_KEYS.forEach(function (p) {
      var e = D(p[0]); if (!e || kv[p[1]] === undefined) return;
      if (e.type === 'checkbox') e.checked = (kv[p[1]] === '1');
      else e.value = kv[p[1]];
    });
    // ---- 老用户配置迁移（功能1 的红线：不许悄悄改老用户行为）----
    // 只有 conf 里【完全没有 rate 键】时才用新默认（follow）：
    //   老用户 conf 通常有 rate=max → kv.rate 有值 → 上面 SET_KEYS 已把它写回 <select>，
    //   所以 TA 的行为一点没变（继续按"设备最高"播，直到自己改）。
    //   全新安装没有 conf → kv.rate 缺失 → 保持 HTML 第一项 follow。
    // 注意：这里【只改 DOM 初值，不立刻 saveSettings】，避免读到旧 conf 就回写。
    if (kv.rate === undefined) {
      if (D('sRate')) D('sRate').value = 'follow';
    }
    if (kv.volume !== undefined) { S.vol = parseFloat(kv.volume) || 0; D('vol').value = S.vol; syncVolUi(); }
    if (kv.loudness !== undefined) { D('sLoud').checked = (kv.loudness === '1'); S.loud = D('sLoud').checked; }
    // 高级参数
    if (kv.device !== undefined) ADV.device = kv.device;
    if (kv.bits !== undefined) ADV.bits = parseInt(kv.bits, 10) || 0;
    if (kv.taps !== undefined) ADV.taps = parseInt(kv.taps, 10) || 0;
    if (kv.atten !== undefined) ADV.atten = parseFloat(kv.atten) || 0;
    if (kv.transition !== undefined) ADV.transition = parseFloat(kv.transition) || 0;
    if (kv.phase !== undefined) ADV.phase = kv.phase;
    if (kv.modorder !== undefined) ADV.modorder = parseInt(kv.modorder, 10) || 5;
    if (kv.modfamily !== undefined) ADV.modfamily = kv.modfamily || 'dsd';
    if (kv.modcut !== undefined) ADV.modcut = parseFloat(kv.modcut) || 0;
    if (kv.moddith !== undefined) ADV.moddith = parseFloat(kv.moddith);
    if (kv.eq !== undefined) ADV.eq = kv.eq;
    if (kv.ir !== undefined) ADV.ir = kv.ir;
    if (kv.irgpu !== undefined) ADV.irgpu = (kv.irgpu === '1') ? 1 : 0;
    if (kv.ramp !== undefined) ADV.ramp = parseFloat(kv.ramp) || 0;
    if (kv.gpuvendor !== undefined) { ADV.gpuvendor = kv.gpuvendor || 'auto'; if (D('sGpu')) D('sGpu').value = ADV.gpuvendor; }
    if (kv.backend !== undefined) ADV.backend = kv.backend || 'wasapi';
    if (kv.channels !== undefined) ADV.channels = parseInt(kv.channels, 10) || 0;
    if (kv.naahost !== undefined) ADV.naaHost = kv.naahost || '127.0.0.1:43210';
    if (kv.upnpdev !== undefined) ADV.upnpDev = kv.upnpdev || '';
    if (kv.trim !== undefined) {
      var tv = String(kv.trim).split(',');
      for (var ti = 0; ti < 8; ti++) ADV['trim' + ti] = parseFloat(tv[ti]) || 0;
    }
    if (kv.inbackend !== undefined) ADV.inBackend = kv.inbackend || 'none';
    if (kv.indev !== undefined) ADV.inDev = kv.indev || '';
    if (kv.inoffset !== undefined) ADV.inOffset = parseInt(kv.inoffset, 10) || 0;
    if (kv.inbuf !== undefined) ADV.inBuf = parseInt(kv.inbuf, 10) || 100;
    if (kv.inmon !== undefined) ADV.inMonitor = (kv.inmon === '1') ? 1 : 0;
    if (kv.inseconds !== undefined) ADV.inSeconds = parseFloat(kv.inseconds) || 0;
    if (kv.inout !== undefined) ADV.inOut = kv.inout || '';
    if (kv.sdmfilt !== undefined) ADV.sdmFilt = kv.sdmfilt || '';
    if (kv.sdmfiltnx !== undefined) ADV.sdmFiltNx = kv.sdmfiltnx || '';
    if (kv.directsdm !== undefined) ADV.directsdm = (kv.directsdm === '1') ? 1 : 0;
    // ReplayGain（261027-01 #2）：老配置没有这两个键 ⟹ 回落 off + 防削波开（与后端默认一致）
    if (kv.rgmode !== undefined) ADV.rgmode = (kv.rgmode === 'track' || kv.rgmode === 'album') ? kv.rgmode : 'off';
    if (kv.rgclip !== undefined) ADV.rgclip = (kv.rgclip === '0' || kv.rgclip === 'false') ? 0 : 1;
    if (kv.dsdkind !== undefined) ADV.dsdKind = kv.dsdkind || 'fir';
    if (kv.dsdconv !== undefined) ADV.dsdConv = kv.dsdconv || 'narrow';
    if (kv.opraproduct !== undefined) ADV.opraproduct = kv.opraproduct;
    if (kv.oprapreset !== undefined) ADV.oprapreset = kv.oprapreset;
    // 元数据补全（P1.5）：metaon/metallm 从设置键读；源开关不在这里读 ——
    // 由 fillMetaProviders 走 /api/providers（同一 conf 的注册表视图，含旧键迁移）
    if (kv.metaon !== undefined) META.on = (kv.metaon === '1') ? 1 : 0;
    if (kv.metallm !== undefined) META.llm = (kv.metallm === '1') ? 1 : 0;
    if (kv.metasummary !== undefined) META.summary = (kv.metasummary === '1') ? 1 : 0;
    // 主题：本地优先（避免闪一下），否则用服务端保存的
    var t = null;
    try { t = localStorage.getItem('rta-theme'); } catch (e) { }
    setTheme(t || kv.theme || 'dark', false);
    // 用户 261023-04 #4：EQ/IR 芯片读的是上面刚填好的 ADV.eq / ADV.ir ——
    // 设置页保存后立刻重绘单曲页（清空 EQ 后芯片要马上消失，不用重开页面）。
    if (D('npPage') && D('npPage').classList.contains('on')) renderNp();
  }).catch(function () { });
}

/* ---------------- 元数据源开关（P1.5：/api/providers 注册表驱动） ---------------- */
// 源开关的 HTML 由注册表条目生成：能力位决定提示文案（搜索/歌词）。渲染进设置页
// 元数据分组的 #sp_metasrc 占位容器；/api/providers 晚到也会在到达后补填。
function metaSrcHtml() {
  return (META_PROVIDERS || []).map(function (p) {
    var cap = (p.canSearch && p.canLyric) ? '标签 / 封面 / 歌词'
            : p.canSearch ? '标签 / 封面'
            : '歌词（独立歌词源）';
    return chk('sp_meta_' + p.id, p.name, META_SRC[p.id] !== 0, cap + '；按注册表顺序参与补全');
  }).join('');
}
function renderMetaSrcRows() {
  var box = D('sp_metasrc'); if (!box) return;
  box.innerHTML = metaSrcHtml();
  (META_PROVIDERS || []).forEach(function (p) {
    var e = D('sp_meta_' + p.id); if (!e) return;
    e.onchange = function () { META_SRC[p.id] = this.checked ? 1 : 0; saveSettings(); };
  });
}
// 启动时拉一次注册表（含当前开关状态，旧键已由后端迁移）；设置页开着时立即重填，
// 没开着则下次 renderSettingsPage 用缓存渲染。
function fillMetaProviders() {
  api('/api/providers').then(function (j) {
    if (!j || !j.ok || !j.items) return;
    META_PROVIDERS = j.items;
    META_PROVIDERS.forEach(function (p) { META_SRC[p.id] = p.on ? 1 : 0; });
    renderMetaSrcRows();
  }).catch(function () { });
}

/* ---------------- 标签写回（P2：/api/writeback 预览 / 执行 / 轮询） ----------------
   预览=GET dry（纯只读扫描，toast「N 首可写，M 首跳过」）；
   执行=POST {"apply":true} 起后台批，轮询 status 到 done，toast「写入 X，跳过 Y，失败 Z」。
   执行期间两按钮禁用（WB_BUSY）；写回前服务端自动做 PCM 逐位校验，
   原文件备份到 MetaBase 目录的 backups 子目录。 */
var WB_BUSY = 0;
function wbButtons(dis) {
  var a = D('sp_wbdry'), b = D('sp_wbapply');
  if (a) a.disabled = dis ? true : false;
  if (b) b.disabled = dis ? true : false;
}
function wbMsg(m) { var e = D('sp_wbmsg'); if (e) e.textContent = m || ''; }
function wbPoll() {
  api('/api/writeback/status').then(function (j) {
    if (!j || !j.ok) { WB_BUSY = 0; wbButtons(0); wbMsg(''); return; }
    if (j.running) {
      wbMsg('写回进行中 ' + (j.done || 0) + '/' + (j.total || 0) + '…');
      setTimeout(wbPoll, 800);
      return;
    }
    WB_BUSY = 0; wbButtons(0); wbMsg('');
    if (j.error) { toast('写回未执行：' + j.error); return; }
    toast('写入 ' + (j.written || 0) + '，跳过 ' + (j.skipped || 0) + '，失败 ' + (j.failed || 0) +
          '；失败清单见 rlplayer-meta' + SEP + 'writeback-failures.json');
  }).catch(function () { WB_BUSY = 0; wbButtons(0); wbMsg(''); });
}
// 页面（重）打开时同步一次在跑状态：中途刷新页面也能恢复禁用并接上轮询
function wbSyncBusy() {
  api('/api/writeback/status').then(function (j) {
    if (!j || !j.ok || !j.running) return;
    WB_BUSY = 1; wbButtons(1); wbMsg('写回批次进行中…');
    wbPoll();
  }).catch(function () { });
}
function wbPreview() {
  if (WB_BUSY) return;
  WB_BUSY = 1; wbButtons(1); wbMsg('正在扫描 MetaBase…');
  api('/api/writeback?dry=1').then(function (j) {
    WB_BUSY = 0; wbButtons(0); wbMsg('');
    if (!j || !j.ok) { toast('预览失败：' + ((j && j.error) || '未知错误')); return; }
    toast((j.writable || 0) + ' 首可写，' + (j.skipped || 0) + ' 首跳过');
  }).catch(function () { WB_BUSY = 0; wbButtons(0); wbMsg(''); toast('预览失败：网络错误'); });
}
function wbApply() {
  if (WB_BUSY) return;
  WB_BUSY = 1; wbButtons(1); wbMsg('写回批次已启动…');
  api('/api/writeback', '{"apply":true}').then(function (j) {
    if (!j || !j.ok || !j.started) {
      WB_BUSY = 0; wbButtons(0); wbMsg('');
      toast('无法启动写回：' + ((j && j.error) || '未知错误'));
      return;
    }
    wbPoll();
  }).catch(function () { WB_BUSY = 0; wbButtons(0); wbMsg(''); toast('无法启动写回：网络错误'); });
}

/* ---------------- 设备能力 ---------------- */
// Nx 滤波器下拉默认值：跟随 1x
(function () {
  var nx = D('sFiltNx');
  if (nx) {
    var o = document.createElement('option');
    o.value = ''; o.textContent = '\u8ddf\u968f 1x \u6ee4\u6ce2\u5668';
    nx.insertBefore(o, nx.firstChild);
    nx.value = '';
  }
})();

/* #4（2026-10-27 用户 261026-09）：「输出设备选 VBMatrix In 2，为什么下面声道音量微调还是 2.0？
   我这个输出在 WIN 里面应该是设置成为 5.1 的」——根因是微调行数来自固定值（跟随源时写死 2），
   从来不问设备。现在 /api/devices 每台设备都带 channels/mixRate（GetMixFormat 只读探测，
   不开流不出声），这里缓存成 DEVMIX，微调按 **设备实际混音声道数** 生成。 */
var DEVMIX = {};       // deviceId -> {channels, mixRate, mixBits, fmt}
function devMixOf(id) { return DEVMIX[id || ''] || null; }
function devMixChannels(id) { var d = devMixOf(id); return (d && d.channels) || 0; }
// 微调列表用的"有效声道数"（按后端分档，语义必须与播放链路一致）：
//   wasapi-shared：共享模式输出恒等于**设备混音声道数**（系统混音器说了算），忽略设置项；
//   wasapi（独占）  ：显式选了 2/6/8 就用它；「跟随源」(0) 用设备混音声道数做预览；
//   upnp / null 等  ：没有本地混音格式可问，按设置项，未设置则 2。
// 拿不到设备信息（探测失败 / 没选设备）时退回 2 —— 绝不编造数字，也不回到"永远 2"的老 bug。
function effectiveChannels() {
  var bk = (D('sp_backend') && D('sp_backend').value) || ADV.backend || 'wasapi';
  var setCh = parseInt(ADV.channels, 10) || 0;
  var devCh = devMixChannels(ADV.device);
  // 共享模式：输出恒等于设备混音声道数（系统混音器说了算），设置项不参与。
  if (bk === 'wasapi-shared') return devCh || setCh || 2;
  // 独占 / Null / UPnP：显式设置优先；「跟随源」时用设备混音声道数做**预览**
  //（拿不到设备信息才退回 2 —— 老代码无条件 2 就是用户看到的那个 bug）。
  return setCh > 0 ? setCh : (devCh || 2);
}
function devMixText(id) {
  var d = devMixOf(id);
  if (!d || !d.channels) return '';
  return d.channels + ' 声道 · ' + (d.mixRate ? (d.mixRate + ' Hz') : '采样率未知') +
         (d.mixBits ? (' · ' + d.mixBits + ' bit ' + (d.fmt || '')) : '');
}
// 微调行数的说明行（id=sp_trimchmsg，常驻在微调网格下面）：行数与依据一起可见，
// 免得以后再出现"设备是 5.1、界面只有两行、用户不知道看哪个"。
function updateDevMixHint() {
  var el = D('sp_trimchmsg'); if (!el) return;
  var n = effectiveChannels();
  var devCh = devMixChannels(ADV.device);
  var t = devMixText(ADV.device);
  var bk = (D('sp_backend') && D('sp_backend').value) || ADV.backend || 'wasapi';
  var tail;
  if (parseInt(ADV.channels, 10) > 0) tail = '；「输出声道数」= ' + parseInt(ADV.channels, 10) + '，以此为准';
  else if (bk === 'wasapi-shared') tail = '；共享模式输出恒等于设备混音声道数';
  else if (bk === 'wasapi') tail = '；「输出声道数」= 跟随源，独占播放时实际声道随源文件变化';
  else tail = '；「输出声道数」= 跟随源';
  el.innerHTML = '按 <b>' + n + ' 声道</b> 显示' +
    (devCh ? ('（设备混音 ' + devCh + ' ch' + (t ? (' · ' + esc(t.replace(/^(\d+) 声道 · /, ''))) : '') + '）') : '') + tail;
}
// 重建微调网格（设备切换 / 声道数切换 / 后端切换都走它，保证标签与实际声道一一对应）
// 注意：只重画 UI，**不改 ADV.channels** —— 设备报 4ch 时 sp_ch 里没有 4 这个选项，
// 若顺手把值写回去会存成 0（用户显式设的 6 声道就被悄悄改掉了）。
function repaintTrims() {
  var g = document.querySelector('#setpage .trimgrid');
  if (g) { g.innerHTML = trimFields(effectiveChannels()); bindTrims(); }
  updateDevMixHint();
}
// 兼容旧调用名（parent 侧脚本/其它分支可能按这个名字找）
function applyTrimChannels() { repaintTrims(); }
function loadDeviceList() {
  var bk = ADV.backend || 'wasapi';
  var url = (bk === 'asio') ? '/api/asiodevices' : '/api/devices';
  api(url).then(function (j) {
    var list = j.devices || j.drivers || [];
    // 先缓存每台设备的混音格式（即使设置页还没渲染也要有，否则首屏微调行数会先错一次）
    list.forEach(function (d) { if (d && d.id) DEVMIX[d.id] = { channels: d.channels || 0,
      mixRate: d.mixRate || 0, mixBits: d.mixBits || 0, fmt: d.mixFmt || '' }; });
    var sel2 = D('sp_device');
    if (!sel2) { updateDevMixHint(); return; }
    var h = '<option value="">' +
      (bk === 'asio' ? '（第一个可用驱动）'
                     : (bk === 'wasapi' || bk === 'wasapi-shared' ? '默认设备' : '（此后端没有本地设备）')) +
      '</option>';
    list.forEach(function (d) {
      var dm = DEVMIX[d.id];
      h += '<option value="' + esc(d.id) + '">' + esc(d.name) +
           (d.default ? '（默认）' : '') +
           (dm && dm.channels ? ('  ·  ' + dm.channels + 'ch' + (dm.mixRate ? ('/' + (dm.mixRate / 1000) + 'k') : '')) : '') +
           '</option>';
    });
    sel2.innerHTML = h;
    sel2.value = ADV.device || '';
    sel2.onchange = function () {
      ADV.device = this.value;
      if (bk === 'wasapi') loadRates();   // 只有 WASAPI 能查设备支持的采样率
      // #4 核心：换设备 ⟹ 按该设备的 channels 重建声道微调（2 → L/R；6 → L/R/C/LFE/Ls/Rs；8 → 7.1）
      repaintTrims();
      saveSettings();
      var dm = devMixOf(this.value);
      toast(dm && dm.channels ? ('输出设备已选择（' + dm.channels + ' 声道），下次播放生效')
                             : '输出设备已选择，下次播放生效');
    };
    repaintTrims();      // 首屏：此时 DEVMIX 已有值，微调按设备声道数渲染
  }).catch(function () { });
}

function loadRates() {
  api('/api/rates' + (ADV.device ? ('?device=' + encodeURIComponent(ADV.device)) : '')).then(function (j) {
    var list = j.rates || [];
    if (!list.length) return;
    var max = list[list.length - 1];
    var sel = D('sRate');
    Array.prototype.forEach.call(sel.options, function (o) {
      if (o.value === 'max') { o.textContent = '\u81ea\u52a8\uff08\u8bbe\u5907\u6700\u9ad8 ' + max + '\uff09'; return; }
      // ★ 'follow' 必须在这里 return：它是枚举值、不是数字。漏掉这一条会让它
      //   掉进下面的 parseInt 分支（parseInt('follow') = NaN）⟹ 判为"设备不支持"
      //   ⟹ o.disabled=true 且 sel.value 被强制改成 'max'，于是"跟随系统"永远选不上。
      //   2026-10-22 功能1 实测踩到：全新安装本该是 follow，页面显示的却是 max。
      if (o.value === 'follow') {
        o.textContent = j.sysRate ? ('\u8ddf\u968f\u7cfb\u7edf\uff08' + j.sysRate + ' Hz\uff09')
                                  : '\u8ddf\u968f\u7cfb\u7edf';
        return;
      }
      if (o.value === 'src') return;
      var r = parseInt(o.value, 10);
      var ok = list.indexOf(r) >= 0;
      o.disabled = !ok;
      if (!ok && sel.value === o.value) sel.value = 'max';
      o.textContent = String(r) + (ok ? '' : '  \uff08\u8bbe\u5907\u4e0d\u652f\u6301\uff09');
    });
    var nm = j.device ? ('\u8f93\u51fa\u8bbe\u5907\uff1a' + j.device) : '';
    if (nm && D('devName')) D('devName').textContent = nm;
  }).catch(function () { });
}

/* ---------------- 媒体库状态 ---------------- */
// 2026-10-24（用户 261024-01 #6/#7）：本地音乐库页用「数字」代替一段说明文字 ——
// 曲目 / 专辑 / 扫描线程 / 上次扫描耗时 一眼可见，说明文字全部收进 (?)。
function renderLibStats(j) {
  var box = D('sp_libstats'); if (!box) return;
  var th = parseInt(j.scanThreads, 10) || 0;
  var ms = parseFloat(j.lastScanMs) || 0;
  function it(lbl, val) {
    return '<div class=it><span class=lbl>' + lbl + '</span><span class=val>' + val + '</span></div>';
  }
  // 输入框里的 0 = 自动；统计卡显示的是后端【实际生效】的线程数（例如 8），
  // 标签写成「生效」避免用户以为「输入 0 / 统计 8」是两套不一致的值。
  var h = it('曲目', (j.tracks || 0)) + it('专辑', (j.albums || 0)) +
          it('扫描线程（生效）', th ? th : '自动') + it('上次扫描', ms > 0 ? (ms / 1000).toFixed(1) + ' s' : '—');
  if (j.scanning) h += it('进度', (j.scanned || 0) + ' / ' + (j.total || 0));
  box.innerHTML = h;
}
function loadLibStats() {
  api('/api/lib/stats').then(function (j) {
    var wasScanning = S.scanning;
    S.scanning = !!j.scanning;
    S.roots = j.roots || [];
    renderLibStats(j);
    var bScanEl = D('bScan');
    if (S.scanning) {
      if (bScanEl) {
        bScanEl.classList.add('spinning');
        bScanEl.title = '正在扫描音乐库 (' + (j.scanned || 0) + '/' + (j.total || 0) + ')';
      }
      D('scanbar').style.display = '';
      var pct = j.total > 0 ? Math.min(100, j.scanned / j.total * 100) : 0;
      D('scanfill').style.width = pct + '%';
      D('cnt').textContent = '扫描中 ' + j.scanned + '/' + j.total;
    } else {
      if (bScanEl) {
        bScanEl.classList.remove('spinning');
        bScanEl.title = '重新扫描音乐库';
      }
      D('scanbar').style.display = 'none';
      if (wasScanning) {
        var msg = '扫描完成：共 ' + (j.tracks || 0) + ' 首曲目，' + (j.albums || 0) + ' 张专辑';
        toast(msg);
        window._lastScanSummary = { tracks: j.tracks, albums: j.albums };
        if (S.view === 'albums') loadAlbums('');
        else if (S.view === 'tracks') loadTracks(S.tracksQuery || '');
      } else if (j.tracks === 0 && S.view === 'albums' && !D('list').children.length) {
        loadAlbums('');
      }
    }
    if (D('roots2')) {
      var h = '';
      S.roots.forEach(function (r) {
        h += '<div class=rootline><span title="' + esc(r) + '">' + esc(r) + '</span>' +
             '<button data-r="' + esc(r) + '" title="移除">\u00d7</button></div>';
      });
      D('roots2').innerHTML = h || '<div style="font-size:11.5px;color:#5b6478">还没有添加目录</div>';
      Array.prototype.forEach.call(D('roots2').querySelectorAll('button'), function (b) {
        b.onclick = function () {
          api('/api/lib/roots', 'remove=' + encodeURIComponent(b.dataset.r)).then(function () { loadLibStats(); toast('已移除'); });
        };
      });
    }
    if (S.view === 'settings' && D('sp_roots')) drawRoots();
  }).catch(function () { });
}

/* ---------------- 流媒体 ---------------- */
function loadRemote(id) {
  S.view = 'remote';
  api('/api/src/browse?id=' + encodeURIComponent(id || '')).then(function (j) {
    if (j.err && !(j.items || []).length) {
      // 评审 #7：以前错误态整屏只留一行裸文本，像"页面坏了"。改成带说明 + 去配置入口的空态卡。
      head('流媒体', '', '');
      D('list').innerHTML = '<div class="card" style="max-width:560px;width:100%;margin:clamp(24px,10vh,90px) auto 0;padding:24px 26px;background:var(--phbg);border:1px solid var(--line);border-radius:12px">' +
        '<div style="font-size:14px;font-weight:650;margin-bottom:6px">还没有可浏览的流媒体服务器</div>' +
        '<div style="color:var(--mut);font-size:12.5px;line-height:1.7;margin-bottom:14px">' +
        esc(j.err) + '<br>支持 Navidrome / Subsonic / Emby（只取原始文件，不转码）。先在「设置 → 流媒体」添加并启用一台服务器，保存后回到这里即可浏览。</div>' +
        '<button class="btn pri" id=remoteGoSet>去设置里配置</button>' +
        '<button class="btn" id=remoteRetry style="margin-left:8px">重试</button></div>';
      if (D('remoteGoSet')) D('remoteGoSet').onclick = function () { openSettingsTab('stream'); };
      if (D('remoteRetry')) D('remoteRetry').onclick = function () { loadRemote(id || ''); };
      return;
    }
    S.remote = j.items || []; S.rpath = id || '';
    head('流媒体', S.remote.length + ' 项', '');
    var h = '';
    if (id) h += '<div class="row rowdir" data-r=-1><span class=ico>&#128193;</span><span class=nm>返回上一级</span></div>';
    S.remote.forEach(function (it, i) {
      h += '<div class="row' + (it.dir ? ' rowdir' : '') + '" data-r=' + i + '>' +
           '<span class=ico>' + (it.dir ? '&#128193;' : '&#127925;') + '</span>' +
           '<span class=nm>' + esc(it.t) + '</span><span class=meta>' + esc(it.dir ? '' : (it.ar || '')) + '</span></div>';
    });
    if (!S.remote.length) h = '<div class=empty>此目录为空<br><span style="font-size:11px">先在右侧「设置」里填服务器地址并保存</span></div>';
    D('list').innerHTML = h;
    Array.prototype.forEach.call(D('list').children, function (e) {
      if (e.dataset.r === undefined) return;
      var i = +e.dataset.r;
      e.onclick = function () {
        if (i < 0) loadRemote(S.rpath.split('/').slice(0, -1).join('/'));
        else { var it = S.remote[i]; if (it.dir) loadRemote(it.id); else playRemote(it); }
      };
    });
  }).catch(function (e) { D('list').innerHTML = '<div class=empty>' + esc(e) + '</div>'; });
}

/* ---------------- 流媒体服务器（多台；用户 261024-01 #5） ----------------
   后端契约（tools/rtaweb.cpp）：
     GET  /api/src/config  → {servers:[{id,name,kind,base,user,hasPass,active}], active}
     POST /api/src/config   action=add|update|remove|active（update 时 password 留空 = 保持原密码）
     POST /api/src/ping     表单里还没保存的配置也能先测；?id=N 测已保存的某台
   密码红线：GET 不回明文，界面也不回填；编辑时留空即不改。 */
var SRC = { servers: [], active: '', editing: '' };
var SRC_FORM_OPEN = false;

function srcKindName(k) { return k === 'emby' ? 'Emby' : 'Navidrome / Subsonic'; }
function srcFormMsg(t, ok) {
  var e = D('sp_src_fmsg'); if (!e) return;
  e.textContent = t || '';
  e.style.color = (ok === undefined) ? '' : (ok ? 'var(--ok-text)' : 'var(--danger)');
}
function srcShowForm(open, s) {
  var box = D('sp_src_form'); if (!box) return;
  SRC_FORM_OPEN = !!open;
  box.style.display = open ? '' : 'none';
  if (!open) { SRC.editing = ''; return; }
  SRC.editing = s ? String(s.id) : '';
  var ft = D('sp_src_ftitle');
  if (ft) ft.textContent = s ? ('编辑服务器 · ' + (s.name || s.base || '')) : '添加服务器';
  D('sp_src_name').value = s ? (s.name || '') : '';
  D('sp_src_kind').value = s ? (s.kind || 'subsonic') : 'subsonic';
  D('sp_src_base').value = s ? (s.base || '') : '';
  D('sp_src_user').value = s ? (s.user || '') : '';
  D('sp_src_pass').value = '';
  D('sp_src_pass').placeholder = s ? '留空 = 保持原密码' : '密码';
  srcFormMsg(s ? ('正在编辑「' + (s.name || s.base) + '」') : '');
  var f = D('sp_src_name'); if (f) f.focus();
}
function srcFormBody() {
  return 'kind=' + encodeURIComponent(D('sp_src_kind').value) +
         '&name=' + encodeURIComponent((D('sp_src_name').value || '').trim()) +
         '&base=' + encodeURIComponent((D('sp_src_base').value || '').trim()) +
         '&user=' + encodeURIComponent((D('sp_src_user').value || '').trim()) +
         '&pass=' + encodeURIComponent(D('sp_src_pass').value || '');
}
function renderSrcList() {
  var box = D('sp_src_list'); if (!box) return;
  var list = SRC.servers || [];
  if (!list.length) {
    box.innerHTML = '<div class=srvempty>还没有流媒体服务器 —— 点上面的「添加服务器」</div>';
    return;
  }
  var h = '';
  list.forEach(function (s) {
    var act = !!s.active;
    h += '<div class="srvrow' + (act ? ' on' : '') + '" data-id="' + esc(s.id) + '">'
       + '<span class=srvdots></span>'
       + '<div class=srvmain><div class=srvname><span class=nm>' + esc(s.name || s.base) + '</span>'
       + '<span class="srvtag kind">' + esc(srcKindName(s.kind)) + '</span>'
       + (act ? '<span class="srvtag on">已启用</span>' : '') + '</div>'
       + '<div class=srvurl>' + esc(s.base || '') + (s.user ? ' · ' + esc(s.user) : '') + '</div></div>'
       + '<div class=srvacts>'
       + (act ? '' : '<button class="srvbtn pri" data-a=on>启用</button>')
       + '<button class=srvbtn data-a=test>测试</button>'
       + '<button class=srvbtn data-a=edit>编辑</button>'
       + '<button class="srvbtn del" data-a=del>删除</button>'
       + '</div></div>';
  });
  box.innerHTML = h;
  Array.prototype.forEach.call(box.querySelectorAll('.srvbtn'), function (b) {
    b.onclick = function () {
      var row = b.closest('.srvrow'); if (!row) return;
      var id = row.dataset.id, s = null;
      (SRC.servers || []).forEach(function (x) { if (String(x.id) === id) s = x; });
      var a = b.dataset.a;
      if (a === 'edit') { srcShowForm(true, s); return; }
      if (a === 'test') { srcTestSaved(id, b); return; }
      if (a === 'on') {
        api('/api/src/config', 'action=active&id=' + encodeURIComponent(id)).then(function (j) {
          if (!j || !j.ok) { toast('启用失败：' + ((j && j.error) || '未知错误')); return; }
          toast('已启用「' + (s ? (s.name || s.base) : id) + '」');
          loadSrcConfig();
        }).catch(function () { toast('启用失败：服务没响应'); });
        return;
      }
      if (a === 'del') {
        if (!confirm('删除服务器「' + (s ? (s.name || s.base) : id) + '」？只删配置，不动已下载缓存。')) return;
        api('/api/src/config', 'action=remove&id=' + encodeURIComponent(id)).then(function (j) {
          if (!j || !j.ok) { toast('删除失败：' + ((j && j.error) || '未知错误')); return; }
          if (String(SRC.editing) === String(id)) srcShowForm(false);
          toast('已删除');
          loadSrcConfig();
        }).catch(function () { toast('删除失败：服务没响应'); });
      }
    };
  });
}
function loadSrcConfig() {
  if (!D('sp_src_list')) return;    // 流媒体页没渲染就不取
  return api('/api/src/config').then(function (j) {
    if (!j || !j.ok) return;
    SRC.servers = j.servers || [];
    SRC.active = j.active || '';
    renderSrcList();
  }).catch(function () { });
}
function bindSrcPage() {
  if (D('sp_src_add')) D('sp_src_add').onclick = function () { srcShowForm(true, null); };
  if (D('sp_src_cancel')) D('sp_src_cancel').onclick = function () { srcShowForm(false); };
  if (D('sp_src_save')) D('sp_src_save').onclick = srcSave;
  if (D('sp_src_testform')) D('sp_src_testform').onclick = srcTestForm;
  if (D('sp_upnp')) D('sp_upnp').onclick = discoverDlna;
  ['sp_src_base', 'sp_src_user'].forEach(function (id) {
    var e = D(id);
    if (e) e.addEventListener('keydown', function (ev) { if (ev.key === 'Enter') srcSave(); });
  });
}
function srcSave() {
  if (!SRC_FORM_OPEN) return;
  var base = (D('sp_src_base').value || '').trim();
  var user = (D('sp_src_user').value || '').trim();
  if (!base) { srcFormMsg('请填写服务器地址', false); return; }
  if (base.indexOf('http://') !== 0 && base.indexOf('https://') !== 0) {
    srcFormMsg('地址要以 http:// 或 https:// 开头', false); return;
  }
  if (!user) { srcFormMsg('请填写用户名', false); return; }
  var body = 'action=' + (SRC.editing ? 'update' : 'add') +
             (SRC.editing ? ('&id=' + encodeURIComponent(SRC.editing)) : '') + '&' + srcFormBody();
  srcFormMsg('保存中…');
  api('/api/src/config', body).then(function (j) {
    if (!j || !j.ok) { srcFormMsg('保存失败：' + ((j && j.error) || '未知错误'), false); return; }
    var wasEdit = !!SRC.editing;
    srcShowForm(false);
    toast(wasEdit ? '服务器已更新' : '服务器已添加');
    loadSrcConfig();
  }).catch(function () { srcFormMsg('保存失败：服务没响应', false); });
}
function srcTestForm() {
  srcFormMsg('测试中…');
  api('/api/src/ping', srcFormBody()).then(function (j) {
    srcFormMsg(j && j.ok ? '\u2713 连接成功' : ('\u2717 ' + ((j && j.error) || '失败')), !!(j && j.ok));
  }).catch(function () { srcFormMsg('\u2717 服务没响应', false); });
}
function srcTestSaved(id, btn) {
  if (btn) { btn.disabled = true; btn.textContent = '测试中…'; }
  api('/api/src/ping?id=' + encodeURIComponent(id)).then(function (j) {
    if (btn) { btn.disabled = false; btn.textContent = '测试'; }
    toast(j && j.ok ? '连接成功' : ('连接失败：' + ((j && j.error) || '未知错误')));
  }).catch(function () {
    if (btn) { btn.disabled = false; btn.textContent = '测试'; }
    toast('连接失败：服务没响应');
  });
}
function discoverDlna() {
  var m = D('sp_upnpmsg'); if (!m) return;
  m.textContent = '正在搜索…';
  api('/api/upnp').then(function (j) {
    var devs = (j && j.devices) || [];
    if (!devs.length) { m.textContent = '没发现 DLNA 渲染器'; return; }
    m.innerHTML = devs.map(function (d) { return esc(d.name) + ' — ' + esc(d.host) + ':' + esc(d.port); }).join('<br>');
  }).catch(function () { m.textContent = '搜索失败：服务没响应'; });
}

/* ---------------- 视图切换 ---------------- */
// 2026-10-23（合并 r4-shell 的"EQ 移入设置页" + r4-np 的"单曲页 EQ 芯片"）：
// 以前 EQ 是一级视图（setView('eq') + loadEqView()），现在它是设置页里的一个页签。
// 统一走这个入口，别再直接 setView('eq')（那个视图已经不存在）。
function openSettingsTab(g) {
  setView('settings');
  var b = document.querySelector('#setpage .stab[data-g=' + (g || 'eq') + ']');
  if (b) b.click();
  var sp = document.getElementById('setpage');
  if (sp && sp.scrollTo) sp.scrollTo(0, 0);
}
function setView(v) {
  // 261024-05 #T2：离开专辑页 → 立即停掉整张专辑的曲目预取队列，切页面后不再发旧 key 的请求
  if (v !== 'album') albPrefStop();
  S.view = v;
  // 2026-10-23（uicheck 复现的真实 bug）：全局搜索框有 260ms 防抖，输入后立刻点左侧别的导航，
  // 定时器仍会触发 setView('tracks')+loadTracks，把用户刚切的页面顶掉（队列页就是这么被换掉的）。
  // 规则：切换视图即取消未触发的搜索，"最后一次用户操作"胜出。
  if (typeof searchTimer !== 'undefined' && searchTimer) { clearTimeout(searchTimer); searchTimer = 0; }
  // 作品页入口收进单曲页后，仍在内部使用 works 视图；离开 works 时清掉"从正在播放返回"的标记
  if (v !== 'works') S.worksFrom = '';
  Array.prototype.forEach.call(document.querySelectorAll('.nav'), function (e) {
    var on = e.dataset.v === v || (v === 'artist' && e.dataset.v === 'artists') || (v === 'work' && e.dataset.v === 'works');
    // #6：保存的队列是左栏的**二级条目**，只能亮当前那一个（否则所有 sqrow 会一起高亮）
    if (e.dataset.sq) on = (v === 'sq' && e.dataset.sq === S.sqName);
    e.classList.toggle('on', on);
  });
  var isSet = (v === 'settings');
  // 设置页占满整屏：右栏（队列/歌词）在这里没有意义，直接藏掉。
  // 走 #app.noq 这套已有的网格规则，否则网格会留一个空列。
  // 2026-10-23（UI 评审 #8）：播放队列页自己就是队列，右侧抽屉再显示一份同样的队列是重复信息
  D('app').classList.toggle('noq', isSet || S.queueHidden || v === 'queue');
  D('app').classList.toggle('setmode', isSet);   // 设置页隐藏 crumb，页签自己吸顶
  applyRTab(isSet);
  if (D('setpage')) D('setpage').classList.toggle('on', isSet);
  // 2026-10-23 用户反馈「切到作品栏后无法切回其它栏位」：干净环境下三种路径都复现不出来，
  // 但这里确实存在一类残留风险 —— 只要有任何路径把 #list 置为 display:none（设置页就会），
  // 离开设置页时若不显式恢复，视图就会「看起来卡住/空白」。此处按目标视图**强制**设定，
  // 不再依赖「上一个视图是什么」。
  if (D('list')) D('list').style.display = isSet ? 'none' : '';
  if (!isSet && D('scanbar')) D('scanbar').style.display = S.scanning ? '' : 'none';
  if (isSet) { head('设置', '所有参数都在这里，改完点「保存」', ''); renderSettingsPage(); }
  else if (D('scanbar')) D('scanbar').style.display = S.scanning ? '' : 'none';
}

Array.prototype.forEach.call(document.querySelectorAll('.nav'), function (e) {
  if (e.dataset.d) return;
  e.onclick = function () {
    var v = e.dataset.v;
    if (!v) return;
    setView(v);
    if (v === 'albums') loadAlbums('');
    else if (v === 'artists') loadArtists();
    else if (v === 'tracks') loadTracks('');
    else if (v === 'files') loadFiles(S.dir || '');
    else if (v === 'remote') loadRemote('');
    else if (v === 'queue') { renderQueuePage(); }
    else if (v === 'recent') loadRecent();
    // 261026-08：#5 我的最爱 / #6 歌单详情；#2（261026-09）播放列表入口已撤
    else if (v === 'favs') { loadFavs(); }
    else if (v === 'sq') { renderSavedQueuePage(S.sqName || ''); }
  };
});

/* ---------------- 搜索（全局） ---------------- */
var searchTimer = 0;
function updateSearchClearBtn() {
  var btn = D('qClear');
  if (!btn) return;
  btn.style.display = (D('q') && D('q').value) ? '' : 'none';
}
if (D('qClear')) {
  D('qClear').onclick = function () {
    D('q').value = '';
    updateSearchClearBtn();
    D('q').focus();
    loadAlbums('');
  };
}
D('q').oninput = function () {
  var v = this.value.trim();
  updateSearchClearBtn();
  clearTimeout(searchTimer);
  searchTimer = setTimeout(function () {
    // ★ 2026-10-26（261026-05 #1）：**按当前视图分派**搜索。
    // 旧实现无条件 setView('tracks') + loadTracks(v) ⟹ 在专辑页/艺术家页搜索任何东西都会
    // 被强行跳到曲目页，表现为"搜不到专辑和艺术家"。
    if (!v) {
      if (S.view === 'tracks') loadAlbums('');
      else if (S.view === 'albums') loadAlbums('');
      else if (S.view === 'artists') loadArtists();
      return;
    }
    if (S.view === 'artists') { loadArtists(v); return; }
    if (S.view === 'albums' || S.view === 'folder') { loadAlbums(v); return; }
    // tracks 及其它视图：保持原行为（全局搜曲目是合理的默认）
    setView('tracks');
    loadTracks(v);
  }, 260);
};
D('q').onkeydown = function (e) {
  if (e.key === 'Escape') {
    this.value = '';
    updateSearchClearBtn();
    loadAlbums('');
  }
  if (e.key === 'Enter' && S.view === 'tracks') {
    var first = document.querySelector('#list .trow');
    if (first) first.click();
  }
};

/* ---------------- 控件 ---------------- */
D('bStop').onclick = function () { api('/api/stop', '').then(function(){ loadQ(); }); };
D('bNext').onclick = function () { api('/api/skip?dir=1', '').then(loadQ); };
D('bPrev').onclick = function () { api('/api/skip?dir=-1', '').then(loadQ); };
function playOrPause() {
  if (!S.playing) {
    // 停止之后点播放：必须重新起播并把最新参数（采样率/滤波器/位深…）一起下发，
    // 只发 /api/pause 是不会有任何反应的（本轮用户报的就是这个）。
    var f = (S.q && S.q.items && S.q.items.length) ? S.q.items[S.q.index || 0] : S.lastCur;
    if (f) { playPath(f); return; }
    toast('队列是空的，先拖一首歌进来');
    return;
  }
  var nextPaused = !S.paused;
  S.paused = nextPaused;
  api('/api/pause?on=' + (nextPaused ? 1 : 0), '');
  if (D('npIcP')) D('npIcP').setAttribute('d', nextPaused ? 'M8 5l12 7-12 7z' : 'M7 5h4v14H7zM13 5h4v14h-4z');
}
D('bPause').onclick = playOrPause;
D('qclr').onclick = function () { api('/api/clear', '').then(function () { loadQ(); if (S.view === 'queue') renderQueuePage(); }); };
D('npOpen').onclick = openNowPlaying;
D('npClose').onclick = closeNowPlaying;
// 资料抽屉（261024-01 #3b ③）：关闭按钮 / 点遮罩 / 再点同一标签 都能收起
if (D('npDataClose')) D('npDataClose').onclick = function () { npDrawerClose(); npDataRenderBody(); };
if (D('npDrawerBg')) D('npDrawerBg').onclick = function () { npDrawerClose(); npDataRenderBody(); };
// 整页里的播放控件：直接复用底栏那套动作，避免两处逻辑漂移
D('npPrev').onclick = function () { api('/api/skip?dir=-1', '').then(loadQ); };
D('npNext').onclick = function () { api('/api/skip?dir=1', '').then(loadQ); };
D('npPlay').onclick = playOrPause;
D('npStop').onclick = function () { api('/api/stop', '').then(loadQ); };
// ---- 播放模式（四态）：底栏 #bRep、整页 #npRep、设置页 #sp_rep 共用 #sRep 的值（唯一真相源）----
// 2026-10-26（用户 261026-08 #2）：**循环与随机合并成同一个按钮**，共 4 态：
//   0 = 关闭循环（顺序播完停止）   1 = 专辑/列表循环
//   2 = 单曲循环                  3 = 随机播放（打乱后循环）
// 点击顺序（用户钦定）：单曲循环 → 专辑循环 → 关闭循环 → 随机播放 → 回单曲循环
// 为什么用一个字段而不是两个开关：两个开关能组合出 6 态，图标根本没法诚实表达，
// 而且"图标说随机、行为却是单曲"这类假修历史上真的发生过。
var PM_ORDER = [2, 1, 0, 3];
var PM_LABEL = ['关闭循环（顺序播完停止）', '专辑/列表循环', '单曲循环', '随机播放'];
// 四种图标各画各的（内联 SVG，风格与既有图标一致：24x24 视框 + stroke=currentColor）：
//   单曲 = 循环箭头 + 数字 1；专辑循环 = 纯循环箭头；关闭 = 循环箭头 + 斜杠划掉；随机 = 交叉双箭头
function repIconHtml(mode, size) {
  mode = parseInt(mode, 10); if (isNaN(mode)) mode = 0;
  var sz = size || 17;
  var head = '<svg width="' + sz + '" height="' + sz + '" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">';
  if (mode === 3) {
    return head + '<path d="M3 6.5h3.5l10 11H21"/><path d="M18.2 3.5L21 6l-2.8 2.5"/>' +
                  '<path d="M3 17.5h3.5l10-11H21"/><path d="M18.2 14.5L21 17.5l-2.8 2.5"/></svg>';
  }
  var body = '<path d="M4 9V7a2 2 0 012-2h11"/><path d="M14 2.5L17.5 5 14 7.5"/>' +
             '<path d="M20 15v2a2 2 0 01-2 2H7"/><path d="M10 21.5L6.5 19 10 16.5"/>';
  if (mode === 2) return head + body + '<text x="12" y="15.6" font-size="9.5" font-weight="700" fill="currentColor" stroke="none" text-anchor="middle">1</text></svg>';
  if (mode === 0) return head + body + '<path d="M3.2 3.2l17.6 17.6"/></svg>';
  return head + body + '</svg>';
}
function applyRep(v) {
  v = parseInt(v, 10) || 0;
  ['repOne', 'npRepOne'].forEach(function (id) {
    var e = D(id); if (e) e.style.display = (v === 2) ? '' : 'none';
  });
  var i1 = D('repIco'); if (i1) i1.innerHTML = repIconHtml(v, 17);
  var i2 = D('npIco');  if (i2) i2.innerHTML = repIconHtml(v, 18);
  ['bRep', 'npRep'].forEach(function (id) {
    var e = D(id); if (!e) return;
    e.style.color = (v === 0) ? '' : 'var(--acc-text)';
    e.dataset.mode = String(v);                       // 自检/脚本可读的机器可判据
    e.title = '播放模式：' + PM_LABEL[v] + '（点击切换）';
    e.setAttribute('aria-label', '播放模式：' + PM_LABEL[v]);
  });
}
function cycleRep() {
  var cur = parseInt(D('sRep').value, 10) || 0;
  var i = PM_ORDER.indexOf(cur); if (i < 0) i = 0;
  var next = PM_ORDER[(i + 1) % 4];
  D('sRep').value = String(next);
  // 单入口：后端 /api/playmode 把 repeat + shuffle 一次收敛 ⟹ 图标与行为不可能分叉。
  PM_PENDING = Date.now();
  api('/api/playmode?mode=' + next);
  applyRep(next);
  // #5（261026-09）：设置页的「播放模式」下拉（sp_rep）与隐藏镜像（sp_shuf）已删除 ——
  // 底栏 #bRep / 单曲页 #npRep 就是唯一入口，不再需要往设置页同步镜像值。
  toast('播放模式：' + PM_LABEL[next]);
  return next;
}
// ---- 音量：整页滑块是 #vol 的镜像，两边都只改 S.vol + 下发 /api/volume ----
// setVol() 是唯一入口（键盘 ↑↓ 也用它）；syncVolUi() 把状态刷到所有镜像控件。
function syncVolUi() {
  ['vol', 'npVol'].forEach(function (id) {
    var e = D(id);
    if (e && Math.abs(parseFloat(e.value) - S.vol) > 1e-9) e.value = S.vol;
  });
  var lab = D('npVolLab');
  if (lab) lab.textContent = (S.vol > 0 ? '+' : '') + S.vol.toFixed(1) + ' dB';
}
if (D('npRep')) D('npRep').onclick = cycleRep;
if (D('npVol')) {
  D('npVol').oninput = function () {
    S.vol = parseFloat(this.value);
    var v = D('vol'); if (v) v.value = S.vol;
    syncVolUi();
    api('/api/volume?db=' + S.vol.toFixed(1), '');
  };
  D('npVol').addEventListener('change', function () { saveSettings(); });
}
D('npSeek').onclick = function (ev) {
  if (!S.dur) return;
  var r = this.getBoundingClientRect();
  doSeek((ev.clientX - r.left) / Math.max(1, r.width));
};
D('tq').onclick = function () {
  S.queueHidden = !S.queueHidden;
  D('app').classList.toggle('noq', S.queueHidden || S.view === 'settings');
};
// #2：唯一写入口 = /api/playmode（0关/1专辑循环/2单曲循环/3随机）。
D('sRep').onchange = function () {
  var v = parseInt(this.value, 10) || 0;
  D('sShuf').checked = (v === 3);
  PM_PENDING = Date.now();
  api('/api/playmode?mode=' + v);
  applyRep(v);
};
// sShuf 现在只是"模式=随机"的镜像复选框（保留它是为了不破坏既有调用点/自检脚本），
// 勾上 = 随机，取消 = 回到专辑循环（不再是"第五种组合"）。
D('sShuf').onchange = function () {
  var m = this.checked ? 3 : 1;
  D('sRep').value = String(m);
  PM_PENDING = Date.now();
  api('/api/playmode?mode=' + m).then(loadQ);
  applyRep(m);
};
D('vol').oninput = function () {
  S.vol = parseFloat(this.value);
  if (typeof syncVolUi === 'function') syncVolUi();   // 整页滑块是镜像，必须一起动
  api('/api/volume?db=' + S.vol.toFixed(1), '');   // 播放中即时生效，不必重开
};
D('sLoud').onchange = function () { S.loud = this.checked; saveSettings(); };
// 参数类控件改动后自动落盘（下次启动还在）
['sRate','sFilt','sFiltNx','sShap','sDSD','sIsp','s2s','sBuf'].forEach(function (id) {
  var e = D(id); if (!e) return;
  e.addEventListener('change', saveSettings);
});
D('vol').addEventListener('change', saveSettings);
// 2026-10-24 #5：旧的「流媒体服务器」单表单（srcSave/saveSrc）已删除，改由设置页「流媒体」页签的
// bindSrcPage() 绑定（多服务器列表）。这里的隐藏快捷面板不保留副本。
D('bUpnp').onclick = function () {
  D('upnpList').innerHTML = '<div style="font-size:11px;color:#5b6478">正在搜索（约 2 秒）…</div>';
  api('/api/upnp').then(function (j) {
    if (!j.devices || !j.devices.length) {
      D('upnpList').innerHTML = '<div style="font-size:11px;color:#5b6478">没发现 DLNA 渲染器</div>';
      return;
    }
    var h = '';
    j.devices.forEach(function (d) {
      h += '<div class=rootline><span title="' + esc(d.loc) + '">' + esc(d.name) +
           (d.model ? '  ·  ' + esc(d.model) : '') + '</span><span style="color:#5b6478;font-size:10.5px">' + esc(d.host) + ':' + esc(d.port) + '</span></div>';
    });
    h += '<div style="font-size:10.5px;color:#5b6478;margin-top:6px;line-height:1.7">只做发现。DLNA 投放会经过对方的重采样，' +
         '无法保证逐位，所以不作为主链路。</div>';
    D('upnpList').innerHTML = h;
  }).catch(function () { D('upnpList').innerHTML = '<div style="font-size:11px;color:#e8654b">搜索失败</div>'; });
};
D('bNet').onclick = function () {
  api('/api/net').then(function (j) {
    D('netMsg').innerHTML = '本机：http://127.0.0.1:' + j.port + '/<br>' +
      (j.lan ? ('局域网：http://' + esc(j.ip) + ':' + j.port + '/（已开启）')
             : ('局域网访问：未开启（用 <code>--lan</code> 启动即可以 http://' + esc(j.ip) + ':' + j.port + '/ 访问）'));
  });
};
D('bLyric').onclick = function () { setRTab(RTAB === 'l' ? 'q' : 'l'); };
D('bChain').onclick = showChain;
D('bHelp').onclick = function () {
  D('mtitle').textContent = '快捷键与提示';
  D('modal').classList.add('on');
  var rows = [
    ['空格', '暂停 / 继续'], ['← →', '后退 / 前进 5 秒'], ['↑ ↓', '音量增减 1 dB'],
    ['n / p', '下一首 / 上一首'], ['s', '停止'], ['l', '歌词'], ['i', '信号链'],
    ['右键专辑', '整张加入队列'], ['右键曲目', '加入队列'], ['右键队列条目', '从队列移除'],
    ['拖动队列条目', '调整播放顺序'], ['Esc（搜索框内）', '清空搜索']
  ];
  var h = '<div style="display:grid;grid-template-columns:120px 1fr;gap:9px 14px;font-size:12.5px">';
  rows.forEach(function (r) {
    h += '<div style="color:#9dc0ff;font-family:ui-monospace,Consolas,monospace">' + esc(r[0]) + '</div><div>' + esc(r[1]) + '</div>';
  });
  h += '</div>';
  h += '<div style="margin-top:16px;font-size:11px;color:#5b6478;line-height:1.8">' +
       '独占模式下播放会接管音频设备，其他程序的声音会被静音 —— 这是正常的。<br>' +
       '想要 bit-perfect：输出采样率选「保持源采样率」、抖动选「none」、音量 0 dB。</div>';
  D('mbody').innerHTML = h;
};
D('mclose').onclick = function () { D('modal').classList.remove('on'); };
D('modal').onclick = function (e) { if (e.target === D('modal')) D('modal').classList.remove('on'); };
D('seek').onclick = function (e) {
  var r = this.getBoundingClientRect();
  doSeek((e.clientX - r.left) / r.width);
};
D('bScan').onclick = function () {
  api('/api/lib/scan', '').then(function () { toast('开始扫描音乐库'); loadLibStats(); });
};
D('bRescan').onclick = function () {
  api('/api/lib/scan', 'full=1').then(function () { toast('重新读取全部标签'); loadLibStats(); });
};
D('bAddRoot').onclick = function () {
  var d = D('sRoot').value.trim();
  if (!d) { toast('请填写目录'); return; }
  api('/api/lib/roots', 'add=' + encodeURIComponent(d)).then(function (j) {
    if (!j.ok) { toast(j.error || '添加失败'); return; }
    D('sRoot').value = ''; toast('已添加，正在建立索引'); loadLibStats();
    api('/api/lib/scan', '');
  });
};

// 261024-02 #4：seek 统一走这里 —— 发完请求立刻清掉歌词的手动滚动保护并重定位，
// 不等下一次 400ms tick（否则跳转后歌词要"愣一下"才跟上）。
function doSeek(frac) {
  var f = Math.max(0, Math.min(0.9999, frac));
  api('/api/seek?pos=' + f.toFixed(4), '');
  lyricReposition();
  if (D('npPage') && D('npPage').classList.contains('on')) renderNpLyric();
  if (RTAB === 'l') syncLyrics(f * (S.dur || 0));
}
function seekRel(d) {
  if (!S.dur || S.dur <= 0) return;
  doSeek((S.pos + d) / S.dur);
}
function setVol(v) {
  S.vol = v;
  D('vol').value = v;
  if (typeof syncVolUi === 'function') syncVolUi();   // 整页滑块跟随（同一个 S.vol）
  api('/api/volume?db=' + v.toFixed(1), '');
}


/* ---------------- 右栏标签页 ---------------- */
var RTAB = 'q';
function applyRTab(forceHide) {
  D('qlist').style.display = (!forceHide && RTAB === 'q') ? '' : 'none';
  D('lyricbox').style.display = (!forceHide && RTAB === 'l') ? '' : 'none';
  D('qclr').style.display = (RTAB === 'q') ? '' : 'none';
  Array.prototype.forEach.call(document.querySelectorAll('.rtab'), function (e) {
    e.classList.toggle('on', e.dataset.t === RTAB);
  });
}
function setRTab(t) {
  RTAB = t;
  applyRTab(false);
  if (t === 'l') {
    // 页签刚显示时容器这一刻才有布局：先按当前进度强制重定位（不等下一次 /api/status 轮询）
    lyricReposition();
    syncLyrics(S.pos || 0, true);
    if (!LY.lines.length) loadLyrics(S.lastCur);
  }
}
Array.prototype.forEach.call(document.querySelectorAll('.rtab'), function (e) {
  e.onclick = function () { setRTab(e.dataset.t); };
});

/* ---------------- 歌词（LRC）---------------- */
// 261024-02 #4：歌词滚动"全错"的根因有三处，一起重做 ——
//   ① parseLrc 不支持 [offset:±ms]，且把 [00:44.26] 的百分秒/毫秒换算只做了长度判断；
//   ② 滚动用 el.offsetTop 计算，但 offsetParent 不是滚动容器（.nplyric2 无 position），
//      offsetTop 带着整页偏移 ⟹ scrollTop 被算到一个荒唐的位置（这就是"全错"）；
//   ③ 纯文本歌词（t=-1）也被当成有轴歌词，pos 一到就把高亮钉在最后一行。
// 现在：时间戳解析走标准 LRC（[mm:ss] / [mm:ss.x] / [mm:ss.xx] / [mm:ss.xxx] / [mm:ss:xx]、一行多时间戳、
// [offset:±ms]）；滚动一律用 getBoundingClientRect 相对滚动容器算目标位置（与 offsetParent 无关）。
var LY = { lines: [], at: -1, raw: '', timed: false, lastPos: -1 };
var LYMAN = { until: 0 };            // 手动滚动保护：这段时间内不自动抢滚动
function lyricManual() {
  LYMAN.until = Date.now() + 3000;
  if (LYFOLLOW.raf) { cancelAnimationFrame(LYFOLLOW.raf); LYFOLLOW.raf = 0; }
}
function lyricTimed() { return !!(LY.lines.length && LY.lines[0].t >= 0); }
function parseLrc(s) {
  var out = [];
  if (!s) return out;
  var rows = String(s).replace(/\r/g, '').split('\n');
  // [offset:±ms]：LRC 规范，正值表示歌词整体**提前**（时间戳减掉）
  var offsetMs = 0;
  for (var i = 0; i < rows.length; i++) {
    var om = rows[i].match(/^\s*\[offset:\s*([+-]?\d+)\s*\]/i);
    if (om) offsetMs = parseInt(om[1], 10) || 0;
  }
  var STAMP = /\[(\d{1,3}):(\d{1,2})(?:[.:](\d{1,3}))?\]/g;
  var timed = false;
  for (var r = 0; r < rows.length; r++) {
    var row = rows[r], stamps = [], m;
    STAMP.lastIndex = 0;
    while ((m = STAMP.exec(row))) {
      var mm = parseInt(m[1], 10), ss = parseInt(m[2], 10);
      var frac = m[3] ? parseInt(m[3], 10) : 0, ms = frac;
      if (m[3] && m[3].length === 1) ms = frac * 100;        // [mm:ss.x]  → 十分之一秒
      else if (m[3] && m[3].length === 2) ms = frac * 10;    // [mm:ss.xx] → 百分之一秒
      // 三位就是毫秒（ms = frac）
      stamps.push(mm * 60 + ss + ms / 1000);
    }
    var text = row.replace(/\[[^\]]*\]/g, '').trim();
    if (!stamps.length || !text) continue;   // [ti:]/[ar:]/[by:] 元信息行与空行跳过
    timed = true;
    for (var k = 0; k < stamps.length; k++) {
      out.push({ t: Math.max(0, stamps[k] - offsetMs / 1000), s: text });
    }
  }
  out.sort(function (a, b) { return a.t - b.t; });
  if (!timed) {
    // 纯文本歌词（没有时间轴）：保留行，但 t=-1 ⟹ 不自动滚动、不高亮（见 renderNpLyric）
    rows.forEach(function (x) {
      x = x.trim();
      if (!x || /^\[[a-z]+:/i.test(x)) return;
      out.push({ t: -1, s: x });
    });
  }
  return out;
}
// ---- 跟随滚动（261024-03 #3 重做） ----
// 旧实现用 scrollTo({behavior:'smooth'})。真机 WebView2（Chromium 155）实测：smooth 的
// 滚动动画在容器刚从 display:none 变可见 / 连续改目标时会被合并或停滞 —— 日志里
//   scrollTo lyricbox want=120 before=0 immediate=0
//   （1.5s 后）scrollTo lyricbox want=211 before=0 immediate=0
// 目标写了两次，scrollTop 一动不动；到第三次才动。视口尺寸无关（1340x880 / 893x587 都出现过）。
// 现在：
//   * 一律用 scrollTop 直接赋值（同步生效，可验证），完全不依赖 smooth；
//   * 赋值后下一帧复测，未到位就续跑（最多 1.5s）——覆盖「留白还没生效 / 布局还没稳定」；
//   * 容器 clientHeight==0（面板隐藏、网格行塌陷）时不再静默失败：在 1.5s 内逐帧重试；
//   * 行元素一律按 data-i 取，不再用 children 下标（提示条会混进 children）。
var LYFOLLOW = { raf: 0, deadline: 0 };
function lyricRows(box) {
  var out = [];
  if (!box) return out;
  for (var i = 0; i < box.children.length; i++) {
    var e = box.children[i];
    if (e && e.dataset && e.dataset.i !== undefined) out.push(e);
  }
  return out;
}
function lyricLinePx(box) {
  var rows = lyricRows(box);
  for (var i = 0; i < rows.length; i++) {
    var h = rows[i].getBoundingClientRect().height;
    if (h > 0) return h;
  }
  return 0;
}
// 上下留白 = (容器高 - 行高)/2，写进 CSS 变量（::before/::after 用它）⟹ 首行/末行也能滚到正中
function lyricPad(box) {
  if (!box) return;
  var ch = box.clientHeight, lh = lyricLinePx(box);
  if (ch <= 0 || lh <= 0) return;
  var pad = Math.max(0, (ch - lh) / 2);
  var name = (box.id === 'lyricbox') ? '--lyrpad' : '--nppad';
  var cur = box.style.getPropertyValue(name);
  if (cur && Math.abs(parseFloat(cur) - pad) < 0.3) return;
  box.style.setProperty(name, pad.toFixed(2) + 'px');
}
function lyricTarget(box, el) {
  var br = box.getBoundingClientRect(), er = el.getBoundingClientRect();
  var t = box.scrollTop + (er.top - br.top) - (box.clientHeight - er.height) / 2;
  var max = Math.max(0, box.scrollHeight - box.clientHeight);
  return Math.max(0, Math.min(t, max));
}
function lyricScrollTo(box, el) {
  if (!box || !el) return;
  lyricPad(box);
  if (LYFOLLOW.raf) { cancelAnimationFrame(LYFOLLOW.raf); LYFOLLOW.raf = 0; }
  LYFOLLOW.deadline = Date.now() + 1500;
  var step = function () {
    LYFOLLOW.raf = 0;
    if (!box || !el || !box.isConnected) return;
    lyricPad(box);
    if (box.clientHeight <= 0) {                     // 没布局：逐帧重试，别静默放弃
      if (Date.now() < LYFOLLOW.deadline) LYFOLLOW.raf = requestAnimationFrame(step);
      return;
    }
    var t = lyricTarget(box, el);
    if (Math.abs(t - box.scrollTop) >= 0.6) box.scrollTop = t;
    if (Math.abs(t - box.scrollTop) >= 0.6 && Date.now() < LYFOLLOW.deadline)
      LYFOLLOW.raf = requestAnimationFrame(step);
  };
  step();
}

// seek / 切歌：解除手动保护并允许立刻重定位
function lyricReposition() { LYMAN.until = 0; LY.lastPos = -1; LY.at = -1; }
// 2026-10-23 用户反馈「歌词要等约 10 秒才出现」：
// 根因是时序 —— 播放开始时后端才在后台线程触发元数据/歌词补全（meta_on_play），
// 而这里的 loadLyrics 只请求**一次**：补全还没写回 metabase 时拿到空结果，之后永不重试，
// 于是表现为「等很久才出现」或「必须切走再切回才出现」。
// 处理：歌词为空时做**有限次退避重试**（1s/2s/3s/5s），并在切到歌词页时也触发一次。
// 重试仅在前端，不增加后端负担；一旦拿到内容就停止。
var LYRETRY = { file: "", n: 0, timer: 0 };
function loadLyrics(file, isRetry) {
  if (!file) { D('lyricbox').innerHTML = '<div class="ly none">还没有播放曲目</div>'; return; }
  if (/^https?:/.test(file)) { D('lyricbox').innerHTML = '<div class="ly none">网络曲目暂无歌词</div>'; return; }
  if (!isRetry) {
    LYRETRY.file = file; LYRETRY.n = 0;
    if (LYRETRY.timer) { clearTimeout(LYRETRY.timer); LYRETRY.timer = 0; }
  }
  api('/api/lyrics?file=' + encodeURIComponent(file)).then(function (j) {
    LY.lines = j.ok ? parseLrc(j.lrc) : [];
    LY.raw = (j && j.lrc) || '';
    LY.timed = lyricTimed();
    LY.at = -1; LY.lastPos = -1; LYMAN.until = 0;   // 换歌：清掉上一首的高亮/手动保护
    // 纯文本歌词：给一条显式提示（否则"不滚"看起来就是坏了 —— 261024-03 #3 用户反馈）
    var lyHint = (LY.lines.length && !LY.timed)
      ? '<div class=ly-hint title="这份歌词没有 [mm:ss] 时间轴，无法按播放位置高亮/跟随">纯文本歌词（无时间轴）· 不会自动跟随滚动</div>'
      : '';
    D('lyricbox').innerHTML = LY.lines.length
      ? lyHint + LY.lines.map(function (l, i) {
          return '<div class="ly" data-i="' + i + '">' + esc(l.s) + '</div>';
        }).join('')
      : '<div class="ly none">没有歌词<br><span style="font-size:11px">同名 .lrc 文件、内嵌歌词，或播放时自动在线补全（可在设置-元数据补全关闭）</span></div>';
    // 空结果 → 退避重试（后端可能正在后台补全；最多 4 次：1s/2s/3s/5s）
    if (!LY.lines.length && LYRETRY.file === file && LYRETRY.n < 4) {
      var waits = [1000, 2000, 3000, 5000];
      var w = waits[LYRETRY.n] || 5000;
      LYRETRY.n++;
      if (LYRETRY.timer) clearTimeout(LYRETRY.timer);
      LYRETRY.timer = setTimeout(function () {
        if (LYRETRY.file === file && S.lastCur === file) loadLyrics(file, true);
      }, w);
      D("lyricbox").innerHTML = '<div class="ly none">歌词加载中…<br><span style="font-size:11px">正在等后台补全（首次播放新曲目时可能要几秒）</span></div>';
      return;
    }
    Array.prototype.forEach.call(D('lyricbox').children, function (e) {
      if (e.dataset.i === undefined) return;
      e.onclick = function () {
        var l = LY.lines[+e.dataset.i];
        if (l.t >= 0 && S.dur > 0) doSeek(l.t / S.dur);
      };
    });
    syncLyrics(S.pos);
    if (D('npPage') && D('npPage').classList.contains('on')) renderNpLyric();
  }).catch(function () { D('lyricbox').innerHTML = '<div class="ly none">歌词加载失败</div>'; });
}
// 右栏歌词（#lyricbox）：与整页歌词共用同一套解析/居中逻辑（261024-02 #4）
function syncLyrics(pos, force) {
  if (!lyricTimed()) return;
  var idx = -1;
  for (var i = 0; i < LY.lines.length; i++) { if (LY.lines[i].t <= pos + 0.05) idx = i; else break; }
  var box = D('lyricbox');
  if (!box) return;
  if (idx === LY.at && !force) return;
  LY.at = idx;
  var rows = lyricRows(box);
  for (var r = 0; r < rows.length; r++) rows[r].classList.toggle('on', +rows[r].dataset.i === idx);
  var el = rows[idx];
  // 右栏歌词页签没打开时不要滚（clientHeight=0，滚了也没意义）；切到该页签时会 force 重定位
  if (el && RTAB === 'l' && Date.now() >= LYMAN.until) lyricScrollTo(box, el);
}

/* ---------------- 播放列表（UI 已并入播放队列） ---------------- */

/* ---------------- 信号链（Roon 式）---------------- */
// 音量文案必须反映【现在真正生效的】值，而不是起播那一刻的快照。
// 之前这里用的是快照，用户拖动音量后信号链还显示旧值 —— 这是会被当成"撒谎"的 bug。
function volText(liveDb) {
  var v = (typeof liveDb === 'number') ? liveDb : 0;
  if (Math.abs(v) < 0.05) return '0.0 dB（逐位无损）';
  var s = v.toFixed(1) + ' dB';
  if (typeof S.startDb === 'number' && Math.abs(S.startDb - v) >= 0.05)
    s += '（起播 ' + S.startDb.toFixed(1) + ' dB，实时微调 ' + (v - S.startDb).toFixed(1) + ' dB）';
  return s;
}
function showChain() {
  D('mtitle').textContent = '信号链';
  D('modal').classList.add('on');
  api('/api/signal').then(function (j) {
    if (!j.ok) { D('mbody').innerHTML = '<div class=empty>还没有开始播放</div>'; return; }
    var p = j.path;
    var nodes = [];
    // 2026-10-23（用户 261023-02 #1）：把「本次是否 bit-perfect」放在**最显眼的位置**（第一条），
    // 并且结论来自后端的权威判定（/api/signal 的 bitPerfect 字段），不再让用户自己从一堆字段里推。
    // 三态配色：direct=紫（DirectSDM 1-bit 直通）/ yes=绿 / no=黄（附具体原因）。
    (function () {
      var bp = p.bitPerfect || '';
      var why = p.bitPerfectWhy || '';
      var label, color, icon;
      if (bp === 'direct') { label = 'BIT-PERFECT · 1-bit 直通'; color = '#a78bfa'; icon = '\u25c6'; }
      else if (bp === 'yes') { label = 'BIT-PERFECT'; color = '#34d399'; icon = '\u2713'; }
      else if (bp === 'no') { label = '非 bit-perfect'; color = '#fbbf24'; icon = '!'; }
      else { label = 'BIT-PERFECT 状态未知'; color = '#94a3b8'; icon = '?'; }
      nodes.push({ k: icon + ' ' + label, v: why || '（本次未记录）', _bp: bp, _color: color });
    }());
    nodes.push({ k: '源文件', v: nam(p.src) + (p.codec ? '  \u00b7  ' + p.codec.toUpperCase() : '') });
    nodes.push({ k: '解码', v: (p.note || '原生无损解码') + '  \u00b7  ' + p.inRate + ' Hz / ' + p.inBits + ' bit / ' + p.inCh + ' ch' });
    // 261024-04 #8：绿色节点 = 本次**真的**由 GPU 参与计算的那一环。
    // 旧实现给「采样率转换」无条件加 gpu:true ⟹ 只要开了升频，信号链就永远有一个绿色节点，
    // 用户看到的"GPU"其实和本次播放是否用 GPU 无关。
    // 后端字段：/api/signal 的 p.gpuUsed（本次播放 GPU 是否参与）/ p.convGpu（卷积是否走 GPU）。
    // ⚠️ 未解决项：这两个字段后端还没加进 JSON（现在卷积开关叫 irGpu），所以先用
    //    p.convGpu ?? p.irGpu 兜底；后端加好后本兜底自动失效（新字段优先），到时候可以删掉兜底。
    var convGpu = (typeof p.convGpu === 'boolean') ? p.convGpu : (p.irGpu === true);
    var sigGpuUsed = (typeof p.gpuUsed === 'boolean') ? p.gpuUsed : !!S.gpuUsed;
    if (p.resample) nodes.push({ k: '采样率转换', v: (p.filter || 'Kaiser sinc') + '  \u00b7  ' + p.phase + '  \u00b7  ' + p.taps + ' 抽头' + (p.twoStage ? '  \u00b7  两段式' : '') + '  \u2192  ' + p.outRate + ' Hz' + (sigGpuUsed ? '  \u00b7  GPU' : ''), gpu: !!sigGpuUsed });
    else nodes.push({ k: '采样率转换', v: '旁路（源采样率与输出一致，逐位直通）' });
    // 261024-04 #8：EQ / 卷积单独成一环，并且**只有卷积真的走 GPU 时**才标绿（后端 convGpu 字段）
    if (p.eq || p.conv) {
      var eqParts = [];
      if (p.eq) eqParts.push('参数 EQ' + (ADV.eq ? (' ' + nam(ADV.eq)) : ''));
      if (p.conv) eqParts.push('FIR 卷积' + (ADV.ir ? (' ' + nam(ADV.ir)) : ''));
      nodes.push({ k: 'EQ / 卷积', v: eqParts.join('  \u00b7  ') + '  \u00b7  ' + (convGpu ? 'GPU 计算' : 'CPU 计算'), gpu: convGpu });
    }
    // 2026-10-24（原生 DSD 接入）：ASIO 原生 DSD 既不调制也不装 DoP 载波 —— 单独一环。
    // 字段由 rtaweb 的 play_one_native_dsd() 写进 /api/signal：
    //   dsdNative=true / dsdNativeType=MSB1|LSB1 / dsdNativeTypeCode=33|32 / dsdNativeRate=DSD 位率
    //   （注意是 2822400 这类位率，不是 DoP 的 176400 载波率）
    if (p.dsdNative) {
      nodes.push({ k: '原生 DSD', v: '类型 ' + (p.dsdNativeType || '?') +
        (p.dsdNativeTypeCode ? '（' + p.dsdNativeTypeCode + '）' : '') +
        '  \u00b7  ' + (p.dsdNativeRate || p.outRate) + ' Hz  \u00b7  1-bit 直送驱动（不经 PCM / DoP）' });
    } else if (p.dsd) {
      nodes.push({ k: 'DSD 调制', v: 'DSD' + p.dsd + '（' + (p.dsdRate / 1000000).toFixed(4) + ' MHz）\u00b7 1-bit \u00b7 ' + (p.modOrder || 5) + ' 阶 SDM' });
      // 载波率必须用 p.carrier：DirectSDM / 回落的信号链里 outRate 记的是 DSD 位率（2822400），
      // DoP 真正开给设备的率是 carrier（DSD64 -> 176400）。旧写法会把 2822400 说成"24-bit 载波"。
      nodes.push({ k: 'DoP 封装', v: '24-bit 载波 ' + (p.carrier || p.outRate) + ' Hz \u00b7 标记 0x05/0xFA 交替' });
    } else {
      nodes.push({ k: '抖动 / 噪声整形', v: (p.shaper && p.shaper !== 'none') ? p.shaper : '无（逐位）' });
    }
    // 原生 DSD 失败回落 DoP：原因原样显示，绝不让用户以为这一曲是原生 DSD
    if (p.dsdFallback) nodes.push({ k: 'DoP 回落', v: p.dsdFallback });
    if (p.preAtten && p.preAtten < 0) {
      nodes.push({ k: '峰值保护', v: '检测到过采样峰值 ' + (p.ispPeak * 100).toFixed(1) + '%（' + p.ispOver +
        ' 个点超出满幅）→ 预衰减 ' + p.preAtten.toFixed(1) + ' dB' });
    }
    if (p.loudness) nodes.push({ k: '等响度补偿', v: '开启' });
    nodes.push({ k: '音量', v: volText(p.volume), id: 'chainVol' });
    nodes.push({ k: '输出', v: p.device + '  \u00b7  ' + p.outRate + ' Hz / ' + p.outBits + ' bit / ' + p.outCh +
      ' ch  \u00b7  ' + ((ADV.backend === 'wasapi-shared') ? '共享模式（非 bit-perfect）' : '独占模式') +
      '  \u00b7  缓冲 ' + p.buffer + ' ms' });

    var tDec = (typeof p.decodeMs === 'number' && p.decodeMs > 0)
      ? (p.decodeMs >= 1000 ? (p.decodeMs / 1000).toFixed(2) + ' s' : p.decodeMs.toFixed(1) + ' ms')
      : '–';
    var tWav = '–';
    if (typeof p.ffmpegMs === 'number' && p.ffmpegMs > 0) {
      tWav = '转码写 ' + p.ffmpegMs.toFixed(1) + ' ms' +
             (typeof p.wavReadMs === 'number' && p.wavReadMs > 0 ? ' / 读 ' + p.wavReadMs.toFixed(1) + ' ms' : '');
    } else if (typeof p.wavReadMs === 'number' && p.wavReadMs > 0) {
      tWav = '读取 ' + p.wavReadMs.toFixed(1) + ' ms';
    }
    var tDsp = '–';
    if (typeof p.dspBlockMs === 'number' && p.dspBlockMs > 0) {
      tDsp = p.dspBlockMs.toFixed(2) + ' ms/块' +
             (typeof p.dspTotalMs === 'number' && p.dspTotalMs > 0 ? '（累计 ' + p.dspTotalMs.toFixed(1) + ' ms）' : '');
    } else if (typeof p.dspTotalMs === 'number' && p.dspTotalMs > 0) {
      tDsp = '累计 ' + p.dspTotalMs.toFixed(1) + ' ms';
    }
    var tScan = (typeof p.scanMs === 'number' && p.scanMs > 0)
      ? (p.scanMs >= 1000 ? (p.scanMs / 1000).toFixed(2) + ' s' : p.scanMs.toFixed(0) + ' ms')
      : '–';

    nodes.push({
      k: '耗时统计',
      v: '解码 ' + tDec + '  \u00b7  临时WAV ' + tWav + '  \u00b7  DSP ' + tDsp + '  \u00b7  库扫描 ' + tScan,
      id: 'chainTiming'
    });
    var h = '<div class=chain>';
    nodes.forEach(function (n, i) {
      // _bp 节点是「本次是否 bit-perfect」横幅：整块高亮 + 左侧色条，要一眼能看到
      var isBp = !!n._bp;
      var style = isBp
        ? ' style="border-left:3px solid ' + n._color + ';background:rgba(255,255,255,.04);padding-left:10px;border-radius:6px"'
        : '';
      var kStyle = isBp ? ' style="color:' + n._color + ';font-weight:700;letter-spacing:.3px"' : '';
      h += '<div class="cnode' + (n.gpu ? ' gpu' : '') + '"' + style + '><div class=dot></div><div><div class=k' + kStyle + '>' + esc(n.k) + '</div>' +
           '<div class=v' + (n.id ? ' id=' + n.id : '') + '>' + esc(n.v) + '</div></div></div>';
      if (i !== nodes.length - 1) h += '<div class=cline></div>';
    });
    h += '</div>';
    h += '<div style="margin-top:14px;font-size:11px;color:#5b6478;line-height:1.7">绿色节点表示该环节本次由 GPU 实际参与计算（没参与就不标绿）。<br>只要每一环都不做多余处理，输出就是逐位（bit-perfect）的。</div>';
    D('mbody').innerHTML = h;
  }).catch(function () { D('mbody').innerHTML = '<div class=empty>读取失败</div>'; });
}


/* ---------------- 完整设置页 ---------------- */
var SET_GROUPS = null;
function opt(v, label, cur) {
  return '<option value="' + esc(v) + '"' + (String(cur) === String(v) ? ' selected' : '') + '>' + esc(label) + '</option>';
}
// 2026-10-24（用户 261024-01 #7）：「文字量太大……做成鼠标移上去的提示文本，要么就给我去掉」。
// 约定：
//   tip(说明)     → label 右侧的 (?) 图标，hover / 键盘 focus 才显示（长说明一律用这个）；
//   inlineHint()  → 真正需要常驻的安全警告，压成一句放控件下面；
//   sgroup(title, tipText, body) → 分组标题右侧同样是一个 (?)。
// 开发者注释（"实测 THD+N…""顺序挑第一个能跑的后端"这类）不进入界面。
function tip(t) {
  return t ? '<span class=tip tabindex=0 role=note data-tip="' + esc(t) + '">?</span>' : '';
}
function inlineHint(t) {
  return t ? '<div class=hint>' + t + '</div>' : '';
}
function num(id, val, min, max, step, hint) {
  return '<div class=sfld><label>' + id.label + tip(hint) + '</label><input type=number id=' + id.id +
         ' value="' + val + '" min=' + min + ' max=' + max + ' step=' + step + '></div>';
}
function sel(id, label, optsHtml, hint) {
  return '<div class=sfld><label>' + label + tip(hint) + '</label><select id=' + id + '>' + optsHtml + '</select></div>';
}
// wide=true 时整行显示（用于说明较长的开关，避免被 .sgbody 的多列网格压成窄条）
function chk(id, label, checked, hint, wide) {
  return '<div class="sfld' + (wide ? ' wide' : '') + '"><label class=chk><input type=checkbox id=' + id + (checked ? ' checked' : '') + '>' +
         label + (hint ? '<span class=tip tabindex=0 role=note data-tip="' + esc(hint) +
           '" onclick="event.preventDefault();event.stopPropagation()">?</span>' : '') + '</label></div>';
}
function txt(id, label, val, hint, ph) {
  return '<div class="sfld wide"><label>' + label + tip(hint) + '</label><input type=text id=' + id + ' value="' + esc(val || '') +
         '" placeholder="' + esc(ph || '') + '"></div>';
}

/* ---------------- 输出后端 / 输入设备 ---------------- */
var ASIO_DRIVERS = null;   // 由 /api/asio 填充
function backendHint() {
  var m = D('sp_backendmsg'); if (!m) return;
  var bk = (D('sp_backend') && D('sp_backend').value) || 'wasapi';
  if (bk === 'wasapi') {
    m.textContent = '独占 · 事件驱动 · bit-perfect；播放期间会静音系统其他声音。';
  } else if (bk === 'wasapi-shared') {
    m.textContent = '共享：经系统混音，非 bit-perfect；不支持 DoP / DSD。';
  } else if (bk === 'null') {
    m.textContent = 'Null output：跑完整 DSP，不出声。';
  } else if (bk === 'naa') {
    // 261024-01 #11：端点来自自动发现，手动框只是折叠兜底。
    // 注意：自动发现没找到时**不能**回落到手动框里的历史默认值（127.0.0.1:43210），
    // 那会让界面宣称"推到 127.0.0.1"而其实根本没发现端点（本轮截图自检抓到）。
    var ep = (epSel && epSel.value) || '';
    // 未发现时的原因已经写在黄色结论行（sp_naa_msg）里，这里不再重复，避免同一屏两遍同样的话。
    m.textContent = 'NAA：把 PCM 按 RTANet 协议推到 ' + (ep || '端点（尚未发现）') + '（端点机需运行 rtanet_rx）。';
  } else if (bk === 'upnp') {
    // 261024-01 #9：后端下拉新增「UPnP / DLNA 投放」。提示用渲染器**名字**，不回显整条描述 URL。
    var devSel = D('sp_upnp_dev');
    var devName = '';
    if (devSel && devSel.selectedIndex >= 0 && devSel.value) {
      devName = devSel.options[devSel.selectedIndex].textContent || '';
    }
    m.textContent = 'UPnP / DLNA 投放：播放 / 切歌 / 暂停 / 拖动进度都会作用到渲染器 ' +
      (devName || '（未选择渲染器）') + '。按源文件原采样率供源，不经本机 DSP。';
  } else if (bk === 'asio') {
    if (ASIO_DRIVERS === null) m.textContent = 'ASIO：正在检测本机驱动…';
    else if (!ASIO_DRIVERS.length) m.textContent = 'ASIO：本机未检测到 ASIO 驱动。';
    else m.textContent = 'ASIO：本机驱动 ' + ASIO_DRIVERS.join('、') + '。';
  }
}

/* ---------------- 输入采集与直通监听（P4b） ---------------- */
var _inPollTimer = null;

function loadInputDevices() {
  return api('/api/input/devices').then(function (j) {
    var s = D('sp_indev'); if (!s) return;
    var list = (j && j.devices) || [];
    var hh = '<option value="">（未选择 / 默认设备）</option>';
    list.forEach(function (d) {
      var name = d.name || '';
      if (d.isLoopback && name.indexOf('[Loopback]') !== 0 && name.indexOf('[回环]') !== 0) {
        name = '[回环] ' + name;
      }
      if (d.isDefault) {
        name += '（默认）';
      }
      hh += '<option value="' + esc(d.id) + '">' + esc(name) + '</option>';
    });
    s.innerHTML = hh;
    if (ADV.inDev) s.value = ADV.inDev;
    s.onchange = function () { ADV.inDev = this.value; saveSettings(); };
    if (D('sp_inmsg')) {
      D('sp_inmsg').textContent = list.length ? '' : '本机未检测到可用输入设备';
    }
  }).catch(function () {
    if (D('sp_inmsg')) D('sp_inmsg').textContent = '输入设备列表读取失败';
  });
}

function updateInputStateUi(st) {
  if (!st) return;
  var running = !!st.running;
  var btnStart = D('sp_instart');
  var btnStop = D('sp_instop');
  if (btnStart) btnStart.disabled = running;
  if (btnStop) btnStop.disabled = !running;

  var rEl = D('sp_stat_running');
  if (rEl) {
    if (running) {
      rEl.textContent = st.mode === 'monitor' ? '直通监听中' : (st.mode === 'null' ? '模拟源运行中' : '录制中');
      rEl.style.color = 'var(--acc)';
    } else {
      rEl.textContent = '空闲';
      rEl.style.color = '';
    }
  }

  var fEl = D('sp_stat_frames');
  if (fEl) {
    var sec = (typeof st.seconds === 'number') ? st.seconds.toFixed(1) : '0.0';
    fEl.textContent = (st.frames || 0) + ' 帧 (' + sec + 's)';
  }

  var sEl = D('sp_stat_spec');
  if (sEl) {
    if (st.rate && st.channels) {
      sEl.textContent = st.rate + ' Hz / ' + st.channels + ' 声道' + (st.device ? (' · ' + st.device) : '');
    } else {
      sEl.textContent = st.device ? st.device : '-';
    }
  }

  var wEl = D('sp_stat_water');
  var wBar = D('sp_meter_water');
  var waterVal = typeof st.water === 'number' ? Math.max(0, Math.min(1, st.water)) : 0;
  var waterPct = (waterVal * 100).toFixed(1) + '%';
  if (wEl) wEl.textContent = waterPct;
  if (wBar) wBar.style.width = waterPct;

  var xEl = D('sp_stat_xrun');
  if (xEl) {
    xEl.textContent = '溢出 ' + (st.overflow || 0) + ' / 欠载 ' + (st.underflow || 0);
    xEl.style.color = ((st.overflow || 0) > 0 || (st.underflow || 0) > 0) ? 'var(--err)' : '';
  }

  var cEl = D('sp_stat_clips');
  if (cEl) {
    var pos = st.posClips || 0;
    var neg = st.negClips || 0;
    cEl.textContent = '+' + pos + ' / -' + neg;
    cEl.style.color = (pos > 0 || neg > 0) ? 'var(--err)' : '';
  }

  var rmsEl = D('sp_stat_rms');
  var rmsBar = D('sp_meter_rms');
  var rms = typeof st.rmsDb === 'number' ? st.rmsDb : -999;
  if (rmsEl) rmsEl.textContent = (rms > -900) ? (rms.toFixed(1) + ' dBFS') : '-∞ dBFS';
  if (rmsBar) {
    var rPct = Math.max(0, Math.min(100, ((rms + 60) / 60) * 100));
    rmsBar.style.width = (rms > -900 ? rPct : 0).toFixed(0) + '%';
  }

  var pkEl = D('sp_stat_peak');
  var pkBar = D('sp_meter_peak');
  var pk = typeof st.peakDb === 'number' ? st.peakDb : -999;
  if (pkEl) pkEl.textContent = (pk > -900) ? (pk.toFixed(1) + ' dBFS') : '-∞ dBFS';
  if (pkBar) {
    var pPct = Math.max(0, Math.min(100, ((pk + 60) / 60) * 100));
    pkBar.style.width = (pk > -900 ? pPct : 0).toFixed(0) + '%';
    pkBar.classList.toggle('clip', pk >= -0.1);
  }

  var oEl = D('sp_stat_out');
  if (oEl) {
    oEl.textContent = st.outFile || (st.mode === 'monitor' ? '（直通监听无输出文件）' : '-');
  }

  var errBox = D('sp_stat_err_box');
  var errEl = D('sp_stat_err');
  if (st.error) {
    if (errEl) errEl.textContent = st.error;
    if (errBox) errBox.style.display = 'block';
    if (D('sp_inmsg')) D('sp_inmsg').textContent = st.error;
  } else {
    if (errBox) errBox.style.display = 'none';
  }
}

function pollInputState() {
  clearTimeout(_inPollTimer);
  var inPane = document.querySelector('#setpage .spane[data-g=in]');
  var isTabOn = inPane && inPane.classList.contains('on');
  var isSetOpen = D('setpage') && D('setpage').classList.contains('on');

  api('/api/input/state').then(function (st) {
    updateInputStateUi(st);
    if ((isSetOpen && isTabOn) || (st && st.running)) {
      _inPollTimer = setTimeout(pollInputState, 1000);
    }
  }).catch(function () {
    if (isSetOpen && isTabOn) {
      _inPollTimer = setTimeout(pollInputState, 1500);
    }
  });
}

function startInputCapture() {
  var btnStart = D('sp_instart');
  var btnStop = D('sp_instop');
  if (btnStart) btnStart.disabled = true;
  if (btnStop) btnStop.disabled = true;

  var isMon = D('sp_inmon') && D('sp_inmon').checked;
  var mode = isMon ? 'monitor' : 'record';
  var dev = (D('sp_indev') && D('sp_indev').value) || '';
  var sec = D('sp_inseconds') ? parseFloat(D('sp_inseconds').value) || 0 : 0;
  var out = D('sp_inout') ? D('sp_inout').value.trim() : '';

  var body = 'mode=' + encodeURIComponent(mode);
  if (dev) body += '&dev=' + encodeURIComponent(dev);
  if (sec > 0) body += '&seconds=' + sec;
  if (out) body += '&out=' + encodeURIComponent(out);

  api('/api/input/start', body).then(function (j) {
    if (j && j.ok) {
      toast(isMon ? '直通监听已启动' : '录制采集已启动');
      if (D('sp_inmsg')) D('sp_inmsg').textContent = '';
      pollInputState();
    } else {
      var err = (j && j.error) || '启动失败';
      if (D('sp_inmsg')) D('sp_inmsg').textContent = err;
      toast('启动失败: ' + err);
      pollInputState();
    }
  }).catch(function (e) {
    var err = (e && e.message) || '网络或接口错误';
    if (D('sp_inmsg')) D('sp_inmsg').textContent = err;
    toast('启动失败: ' + err);
    pollInputState();
  });
}

function stopInputCapture() {
  var btnStart = D('sp_instart');
  var btnStop = D('sp_instop');
  if (btnStart) btnStart.disabled = true;
  if (btnStop) btnStop.disabled = true;

  api('/api/input/stop', '').then(function (j) {
    if (j && j.ok) {
      toast('采集已停止');
      pollInputState();
    } else {
      var err = (j && j.error) || '停止失败';
      toast('停止失败: ' + err);
      pollInputState();
    }
  }).catch(function (e) {
    toast('停止失败');
    pollInputState();
  });
}

/* ---------------- UPnP/DLNA 媒体投放 ---------------- */
var _castPollTimer = null;
var _castDevices = [];

function fmtBytes(b) {
  if (!b || b <= 0) return '0 B';
  if (b < 1024) return b + ' B';
  if (b < 1024 * 1024) return (b / 1024).toFixed(1) + ' KB';
  if (b < 1024 * 1024 * 1024) return (b / (1024 * 1024)).toFixed(1) + ' MB';
  return (b / (1024 * 1024 * 1024)).toFixed(2) + ' GB';
}

function updateCastStateUi(st) {
  if (!st) return;
  var running = !!st.running;
  var btnStart = D('sp_cast_start');
  var btnStop = D('sp_cast_stop');
  if (btnStart) btnStart.disabled = running;
  if (btnStop) btnStop.disabled = !running;

  var pEl = D('sp_cast_stat_phase');
  if (pEl) {
    var pMap = {
      'discovering': '设备发现中',
      'serving': '本地供源就绪',
      'playing': '投放播放中',
      'stopped': '已停止',
      'error': '发生错误'
    };
    pEl.textContent = pMap[st.phase] || (st.phase || '已停止');
    if (st.phase === 'playing') pEl.style.color = 'var(--ok)';
    else if (st.phase === 'error') pEl.style.color = 'var(--err)';
    else if (running) pEl.style.color = 'var(--acc)';
    else pEl.style.color = '';
  }

  var devEl = D('sp_cast_stat_dev');
  if (devEl) {
    devEl.textContent = st.device || '-';
  }

  var posEl = D('sp_cast_stat_pos');
  if (posEl) {
    posEl.textContent = fmt(st.positionSec || 0);
  }

  var trEl = D('sp_cast_stat_transport');
  if (trEl) {
    trEl.textContent = st.transportState || 'STOPPED';
    if (st.transportState === 'PLAYING') trEl.style.color = 'var(--ok)';
    else if (st.transportState === 'PAUSED_PLAYBACK') trEl.style.color = 'var(--warn, #eab308)';
    else trEl.style.color = '';
  }

  var bEl = D('sp_cast_stat_bytes');
  if (bEl) {
    bEl.textContent = fmtBytes(st.servedBytes || 0);
  }

  var reqEl = D('sp_cast_stat_reqs');
  if (reqEl) {
    reqEl.textContent = (st.requestCount || 0) + ' 次';
  }

  var portEl = D('sp_cast_stat_port');
  if (portEl) {
    portEl.textContent = String(st.servePort || 8912);
  }

  var rmEl = D('sp_cast_stat_ratematch');
  if (rmEl) {
    if (st.rateMatch === 'match') {
      rmEl.textContent = '匹配（Bit-Perfect）';
      rmEl.style.color = 'var(--ok)';
    } else if (st.rateMatch === 'mismatch') {
      rmEl.textContent = '失配（可能重采样）';
      rmEl.style.color = 'var(--err)';
    } else {
      rmEl.textContent = '未声明（需硬件确证）';
      rmEl.style.color = 'var(--mut2)';
    }
  }

  var warnBox = D('sp_cast_stat_warn_box');
  var warnEl = D('sp_cast_stat_warn');
  if (st.warning) {
    if (warnEl) warnEl.textContent = st.warning;
    if (warnBox) warnBox.style.display = 'block';
  } else {
    if (warnBox) warnBox.style.display = 'none';
  }

  var errBox = D('sp_cast_stat_err_box');
  var errEl = D('sp_cast_stat_err');
  if (st.error) {
    if (errEl) errEl.textContent = st.error;
    if (errBox) errBox.style.display = 'block';
    if (D('sp_cast_msg')) D('sp_cast_msg').textContent = st.error;
  } else {
    if (errBox) errBox.style.display = 'none';
  }
}


// 261024-01 #9：两个渲染器下拉（「输出后端」里的 sp_upnp_dev 与「投放」区块里的
// sp_cast_dev）共用同一份发现结果与同一个选择。value 一律用设备描述 URL ——
// 直连描述比按名字匹配可靠（本机 SSDP 被拦时 CastManager 仍可用 http://IP:PORT/ 直连）。
function castDevLabel(d) {
  return (d.friendlyName || d.name || d.host || '') + (d.model ? (' · ' + d.model) : '');
}
function castDevValue(d) {
  return d.location || d.friendlyName || d.name || d.host || '';
}
function populateCastDevices() {
  var opts = '';
  _castDevices.forEach(function (d) {
    opts += '<option value="' + esc(castDevValue(d)) + '">' + esc(castDevLabel(d)) + '</option>';
  });
  if (!opts) opts = '<option value="">（未发现 UPnP 渲染器）</option>';
  // 已保存的渲染器若这次没扫到（设备关机 / 换网段）也保留一项，别让用户的选择悄悄丢
  var saved = (ADV.upnpDev || '').trim();
  var haveSaved = false;
  if (saved) {
    for (var i = 0; i < _castDevices.length; i++) {
      if (castDevValue(_castDevices[i]) === saved) { haveSaved = true; break; }
    }
    if (!haveSaved) opts += '<option value="' + esc(saved) + '">（上次选择）' + esc(saved) + '</option>';
  }
  ['sp_cast_dev', 'sp_upnp_dev'].forEach(function (id) {
    var s = D(id);
    if (!s) return;
    s.innerHTML = opts;
    var want = saved;
    if (!want && _castDevices.length) want = castDevValue(_castDevices[0]);
    if (!want) return;
    for (var k = 0; k < s.options.length; k++) {
      if (s.options[k].value === want) { s.value = want; break; }
    }
  });
  // 第一次发现有设备时自动选中并写回设置（用户不用手填设备地址）
  if (!saved && _castDevices.length) syncUpnpDev(castDevValue(_castDevices[0]));
  backendHint();   // 下拉刚填好，后端提示里的渲染器名字要跟着刷新（否则会显示"未选择渲染器"）
}
function syncUpnpDev(value) {
  ADV.upnpDev = value || '';
  if (D('sp_cast_dev') && D('sp_cast_dev').value !== value) D('sp_cast_dev').value = value;
  if (D('sp_upnp_dev') && D('sp_upnp_dev').value !== value) D('sp_upnp_dev').value = value;
  saveSettings();
}
function scanCastDevices(force) {
  var msg = D('sp_cast_devmsg');
  var msg2 = D('sp_upnp_msg');
  var btns = [D('sp_cast_scan'), D('sp_upnp_scan')];
  btns.forEach(function (b) { if (b) b.disabled = true; });
  if (msg) msg.textContent = '正在发现渲染器…';
  if (msg2) msg2.textContent = '正在发现渲染器…';

  var url = '/api/cast/devices' + (force ? '?scan=1' : '');
  api(url).then(function (j) {
    btns.forEach(function (b) { if (b) b.disabled = false; });
    _castDevices = (j && j.devices) || [];
    populateCastDevices();
    if (msg) {
      msg.textContent = _castDevices.length
        ? ('共发现 ' + _castDevices.length + ' 个渲染器')
        : '未发现 UPnP/DLNA 渲染器（可能未开机、不在同一子网，或 SSDP 组播被代理/防火墙拦截）';
    }
    if (msg2) {
      msg2.textContent = _castDevices.length
        ? ('共发现 ' + _castDevices.length + ' 个渲染器')
        : '未发现 UPnP/DLNA 渲染器：确认数播已开启 UPnP/DLNA 且与本机同一子网';
    }
  }).catch(function () {
    btns.forEach(function (b) { if (b) b.disabled = false; });
    if (msg) msg.textContent = '扫描失败（/api/cast/devices 无响应）';
    if (msg2) msg2.textContent = '扫描失败（/api/cast/devices 无响应）';
  });
}

/* 261024-05：NAA 端点自动发现整段已删除（后端选项一起去掉，没有任何调用点）。 */
/* 261024-05：NAA 端点发现已删除（用户「NAA 放弃，彻底删除这一功能」）。
   数播的 NAA 只认 HQPlayer 私有握手，实测 8 种握手变体全部无回应；界面已不再提供该后端。 */
var _naaPollTimer = null;
function renderNaaEndpoints() { /* 已删除 */ }
function discoverNaa() { clearTimeout(_naaPollTimer); /* 已删除 */ }

/* ---------------- 后端相关控件的显隐 / 启用状态（261024-01 #9/#11） ---------------- */
// 261024-01 #9/#11：后端 = UPnP 时，本机设备 / 采样率 / 位深 / 声道 / 缓冲 / 渐变 / 输出模式 /
// 声道微调**全都不参与**（投放按源文件原采样率供源，不经本机 DSP）。这些字段直接隐藏，而不是
// 置灰摆着 —— 用户已反馈设置页控件/文本太多（261024-01 #7），没用就别占地方。
// 注意：字段仍保留在 DOM 里（display:none），collectSettings / saveSettings 照旧读得到值。
var _UPNP_IRRELEVANT = ['sp_device', 'sp_rate', 'sp_bits', 'sp_ch', 'sp_buffer', 'sp_ramp', 'sp_outmode'];
function _setFieldVisible(id, on) {
  var e = D(id);
  if (!e) return;
  var f = e.closest ? e.closest('.sfld') : null;
  if (f) f.style.display = on ? '' : 'none';
}
function applyBackendState() {
  var bk = (D('sp_backend') && D('sp_backend').value) || 'wasapi';
  var upnpBox = D('sp_upnpwrap'); if (upnpBox) upnpBox.style.display = (bk === 'upnp') ? '' : 'none';
  _UPNP_IRRELEVANT.forEach(function (id) { _setFieldVisible(id, bk !== 'upnp'); });
  // NAA 下"输出设备"没有意义（声音走 RTANet），但采样率/位深/声道/缓冲仍影响 DSP 输出，保留。
  _setFieldVisible('sp_device', bk !== 'naa' && bk !== 'upnp');
  var tw = D('sp_trimwrap'); if (tw) tw.style.display = (bk === 'upnp') ? 'none' : '';
  // 261024-02 #5：投放区块已整体搬到「流媒体」页（输出页不再有它），这里不再联动隐藏。
  // #4（261026-09）：切换后端会改变"有效声道数"（共享 = 设备混音声道数；独占 = 设置项），
  // 所以这里也要重画一次微调网格，否则切到共享后行数还停留在 2。
  if (typeof repaintTrims === 'function' && D('sp_trimchmsg')) repaintTrims();
}

// 261024-02 #5：推送区块的控件绑定（旧版"填文件路径 + DoP 勾选 + 开始投放"已按用户要求删掉，
// 换成 QQ 音乐式的一键推送：选设备 → 推送当前曲 / 停止投放）。
function initCastControls() {
  var devSel = D('sp_cast_dev');
  var btnScan = D('sp_cast_scan');
  var btnPush = D('sp_cast_push');
  var btnStop = D('sp_cast_stop');

  if (devSel) {
    devSel.onchange = function () {
      var v = devSel.value || '';
      syncUpnpDev(v);
      var d = null;
      for (var i = 0; i < _castDevices.length; i++) if (castDevValue(_castDevices[i]) === v) d = _castDevices[i];
      var msg = D('sp_cast_devmsg');
      if (msg) {
        msg.textContent = d ? ((d.model ? ('型号: ' + d.model + ' · ') : '') + (d.hasAVTransport ? '支持 AVTransport' : '未知传输服务'))
                            : (v ? ('已选择：' + v) : '点「重新发现」扫描局域网里的 MediaRenderer');
      }
    };
  }
  if (btnScan) btnScan.onclick = function () { scanCastDevices(true); };
  if (btnPush) {
    btnPush.onclick = function () {
      if (D('sp_cast_msg')) D('sp_cast_msg').textContent = '';
      // 下拉里选的设备优先（用户可能刚换了一台），否则用已保存的
      if (devSel && devSel.value && devSel.value !== ADV.upnpDev) syncUpnpDev(devSel.value);
      castPush(S.lastCur);
    };
  }
  if (btnStop) btnStop.onclick = function () { castStop(); };
}

/* ---------------- 261024-02 #5：一键推送到 UPnP（学 QQ 音乐） ----------------
   用户原话：「你应该学习的是 QQ 音乐的逻辑，单曲播放页面下有一个将本曲一键推送到 UPNP 的按钮，
   然后所有播放都走 UPNP。」「输出栏位下的 UPNP 投放功能多余。」
   设计：
     * 单一真相源 = /api/cast/state（后端 CastManager）。三处 UI（单曲页按钮 / 底栏徽标 /
       流媒体页推送区块）全部由 applyCastUi(st) 从同一份状态刷新，不各存一份。
     * 推送 = ADV.backend='upnp' + ADV.upnpDev=<描述URL> 落盘，然后按新参数重放当前曲目
       ⟹ 之后的切歌/暂停/seek 全部由后端路由到同一条投放会话（"所有播放都走 UPNP"）。
     * 再点一次 = 停止投放 + 回到推送前的输出后端（默认 wasapi），并恢复本地播放。
       （刻意记 prevBackend 而不是硬编码 wasapi：测试实例是 null，硬编码会把安全设置冲掉。） */
var CAST = { state: null, prevBackend: '' };
function castRunning() { return !!(CAST.state && CAST.state.running); }
function castDevNameOf(loc) {
  loc = String(loc || '');
  for (var i = 0; i < _castDevices.length; i++) {
    if (castDevValue(_castDevices[i]) === loc) return castDevLabel(_castDevices[i]);
  }
  var m = /^https?:\/\/([^/:]+)/i.exec(loc);
  return m ? m[1] : (loc || '设备');
}
function castIconSvg() {
  return '<svg width=12 height=12 viewBox="0 0 24 24" fill=none stroke=currentColor stroke-width="2" ' +
         'style="vertical-align:-1px;margin-right:4px"><path d="M2 16.1A5 5 0 0 1 5.9 20"/><path d="M2 12.05A9 9 0 0 1 9.95 20"/>' +
         '<path d="M2 8V6a2 2 0 0 1 2-2h16a2 2 0 0 1 2 2v12a2 2 0 0 1-2 2h-6"/><path d="M2 20h.01"/></svg>';
}
// 三处 UI 的唯一刷新入口
function applyCastUi(st) {
  if (st && typeof st.running === 'boolean') CAST.state = st;
  st = CAST.state || {};
  var running = !!st.running;
  var nm = castDevNameOf(st.device);
  var nb = D('npCastBtn');
  if (nb) {
    nb.innerHTML = castIconSvg() + (running ? ('已推送到 ' + esc(nm)) : '推送到设备');
    nb.classList.toggle('on', running);
    nb.title = running ? ('点击停止投放并恢复本机输出（阶段：' + (st.phase || '-') + '）')
                       : '把当前曲目推送到 UPnP / DLNA 设备，之后所有播放都走它';
  }
  var badge = D('castBadge');
  if (badge) {
    badge.style.display = running ? 'inline-flex' : 'none';
    badge.title = running ? ('正在投放到 ' + nm) : '';
  }
  var push = D('sp_cast_push'), stop = D('sp_cast_stop');
  if (push) { push.textContent = running ? '重新推送当前曲' : '推送当前曲'; }
  if (stop) stop.disabled = !running;
  updateCastStateUi(st);
}
function pollCastState(once) {
  clearTimeout(_castPollTimer);
  var outPane = document.querySelector('#setpage .spane[data-g=out]');
  var streamPane = document.querySelector('#setpage .spane[data-g=stream]');
  var wantLoop = (streamPane && streamPane.classList.contains('on') && D('setpage') && D('setpage').classList.contains('on'))
              || (outPane && outPane.classList.contains('on') && D('setpage') && D('setpage').classList.contains('on'))
              || (D('npPage') && D('npPage').classList.contains('on'))
              || (CAST.state && CAST.state.running);
  api('/api/cast/state').then(function (st) {
    applyCastUi(st);
    if (!once && wantLoop) _castPollTimer = setTimeout(function () { pollCastState(); }, 1000);
  }).catch(function () {
    if (!once && wantLoop) _castPollTimer = setTimeout(function () { pollCastState(); }, 1500);
  });
}
function openModal(title, html) {
  var m = D('modal'); if (!m) return;
  D('mtitle').textContent = title;
  D('mbody').innerHTML = html;
  m.classList.add('on');
}
// 未选设备时的设备选择浮层
function npPickCastDevice(cb) {
  api('/api/cast/devices?scan=1').then(function (j) {
    _castDevices = (j && j.devices) || [];
    populateCastDevices();
    if (!_castDevices.length) {
      toast('未发现 UPnP / DLNA 渲染器（确认数播已开启 DLNA 且与本机同网段）');
      cb(null);
      return;
    }
    var h = '<div class=castpick>';
    _castDevices.forEach(function (d, i) {
      h += '<button class=castpick-row data-i=' + i + '>' +
           '<span class=cpn>' + esc(d.friendlyName || d.name || d.host) + '</span>' +
           '<span class=cph>' + esc((d.model ? d.model + ' · ' : '') + (d.host || '')) + '</span></button>';
    });
    h += '</div><div class=hint style="margin-top:10px">选中后会把当前曲目推送到该设备，之后的切歌/暂停/进度都作用在它上面。</div>';
    openModal('推送到设备', h);
    Array.prototype.forEach.call(D('mbody').querySelectorAll('.castpick-row'), function (b) {
      b.onclick = function () {
        var d = _castDevices[+b.dataset.i];
        D('modal').classList.remove('on');
        cb(castDevValue(d));
      };
    });
  }).catch(function () { toast('设备发现失败（/api/cast/devices 无响应）'); cb(null); });
}
// 推送：落盘 backend=upnp + upnpdev，再按新参数重放当前曲
function castPush(file) {
  file = file || S.lastCur || '';
  if (!file) { toast('当前没有可推送的曲目'); return; }
  if (/^https?:/i.test(file)) { toast('网络曲目暂不支持投放'); return; }
  var go = function (dev) {
    if (!dev) return;
    CAST.prevBackend = (ADV.backend && ADV.backend !== 'upnp') ? ADV.backend : (CAST.prevBackend || 'wasapi');
    ADV.backend = 'upnp';
    ADV.upnpDev = dev;
    saveSettings();
    playPath(file);
    toast('已推送到 ' + castDevNameOf(dev));
    pollCastState(true);
  };
  var dev = (ADV.upnpDev || '').trim();
  if (dev) go(dev);
  else npPickCastDevice(function (d) { if (d) go(d); });
}
// 停止：停会话 → 回到推送前的输出后端 → 恢复本地播放
function castStop() {
  api('/api/cast/stop', '').then(function () {
    var back = CAST.prevBackend || 'wasapi';
    if (back === 'upnp') back = 'wasapi';
    ADV.backend = back;
    saveSettings();
    toast('已停止投放，输出回到 ' + back);
    if (S.lastCur) playPath(S.lastCur);
    pollCastState(true);
  }).catch(function () { toast('停止投放失败'); });
}
function castToggle() {
  if (castRunning()) castStop();
  else castPush(S.lastCur);
}
function npCastBtnHtml() {
  var running = castRunning();
  return '<button class="chip npcast' + (running ? ' on' : '') + '" id=npCastBtn title="' +
         (running ? '点击停止投放并恢复本机输出' : '把当前曲目推送到 UPnP / DLNA 设备') + '">' +
         castIconSvg() + (running ? ('已推送到 ' + esc(castDevNameOf((CAST.state || {}).device))) : '推送到设备') +
         '</button>';
}

/* ---------------- 输出模式 / DSD 比特率 / 封装 ---------------- */
function outModeOf() { return (D('sDSD') && D('sDSD').value !== '0') ? 'dsd' : 'pcm'; }
function dsdRateOf() {
  var v = (D('sDSD') && D('sDSD').value) || '0';
  if (v === '0') return '256';                 // PCM 时给个默认值，别让下拉空着
  var m = /([0-9]+)$/.exec(v);
  return m ? m[1] : '256';
}
function dsdPackOf() { return ((D('sDSD') && D('sDSD').value) || '').indexOf('nat') === 0 ? 'nat' : 'dop'; }
// 三个控件合成隐藏的 sDSD（params()/saveSettings 仍然只认这一个状态）
// 2026-10-24（用户 261024-01 #12）：这里的 disabled 逻辑整个删掉。
// 「SDM 的比特率和封装为什么灰了无法选？」—— 根因就是这一句：只在输出模式已经是 DSD 时才允许改，
// 而默认是 PCM ⟹ 两个下拉永远灰着。现在它们始终可选；在 PCM 下改动会自动把输出切到 DSD
//（联动在控件 onchange 里），并有一句常驻说明指出这件事。
function applyDsdState() {
  var isDsd = D('sp_outmode') && D('sp_outmode').value === 'dsd';
  ['sp_dsdrate', 'sp_dsdpk'].forEach(function (id) {
    var e = D(id);
    if (!e) return;
    e.disabled = false;
    e.style.opacity = '1';
    e.removeAttribute('aria-disabled');
    e.title = isDsd ? '' : '改这里会把输出模式切到 DSD（SDM）';
  });
  var note = D('sp_dsdmodenote');
  if (note) note.style.display = isDsd ? 'none' : '';
}
function applyDsd() {
  var out = D('sp_outmode') ? D('sp_outmode').value : 'pcm';
  if (out !== 'dsd') { D('sDSD').value = '0'; return; }
  var pk = D('sp_dsdpk') ? D('sp_dsdpk').value : 'dop';
  var rt = D('sp_dsdrate') ? D('sp_dsdrate').value : '256';
  D('sDSD').value = pk + rt;
}

// 声道名（WAVEFORMATEXTENSIBLE 的典型顺序）
function channelNames(n) {
  if (n <= 1) return ['单声道'];
  if (n === 2) return ['左', '右'];
  if (n === 4) return ['左', '右', '左后', '右后'];
  if (n === 6) return ['左', '右', '中置', '低音', '左环绕', '右环绕'];
  if (n === 8) return ['左', '右', '中置', '低音', '左环绕', '右环绕', '左后', '右后'];
  var out = [];
  for (var i = 0; i < n; ++i) out.push('声道 ' + (i + 1));
  return out;
}
function bindTrims() {
  for (var i = 0; i < 8; ++i) {
    (function (k) {
      var e = D('sp_trim' + k);
      if (e) e.oninput = function () { ADV['trim' + k] = parseFloat(this.value) || 0; saveSettings(); };
    })(i);
  }
}
function trimFields(chSel) {
  var n = parseInt(chSel, 10) || 0;
  if (!n) n = 2;                       // 跟随源时先按立体声显示
  var names = channelNames(n);
  var h = '';
  for (var i = 0; i < names.length && i < 8; ++i) {
    h += '<div class=sfld><label>' + names[i] + '（声道 ' + (i + 1) + '，dB）</label>' +
         '<input type=number id=sp_trim' + i + ' value="' + (ADV['trim' + i] || 0) +
         '" min=-24 max=12 step=0.1></div>';
  }
  return h;
}

function sgroup(title, hint, body) {
  if (body === undefined) { body = hint; hint = ''; }
  return '<div class=sgroup><div class=sghead>' + title + tip(hint) +
         '</div><div class=sgbody>' + (body || '') + '</div></div>';
}

// ---- 功能2（用户 2026-10-22）：把「跟随预设」跟的是什么相位实时说清楚 ----
// 数据来自 /api/filters（后端 filter_presets() 的真相源），前端不抄第二份相位表。
var FILTER_PHASES = null;      // name -> {phase, phaseName, taps}
var FILTER_PHASES_WANT = 0;    // 还没到就记下，到了再补渲染
function loadFilterPhases() {
  return api('/api/filters').then(function (j) {
    if (!j || !j.ok || !j.items) return;
    FILTER_PHASES = {};
    j.items.forEach(function (it) { FILTER_PHASES[it.name] = it; });
    bindPhaseHint();
  }).catch(function () { });
}
// 把"当前 1x 滤波器的预设相位"和"强制覆盖后的实际相位"一起写进 #sp_phasemsg。
function bindPhaseHint() {
  var msg = D('sp_phasemsg');
  if (!msg) return;
  var fname = (D('sp_filt') && D('sp_filt').value) || '';
  var override = (D('sp_phase') && D('sp_phase').value) || '';
  if (!fname) { msg.textContent = ''; return; }
  var it = FILTER_PHASES ? FILTER_PHASES[fname] : null;
  if (!it) {
    // 相位表还没回来（或该滤波器不在表里）：只说清"预设"是什么，不编造具体相位
    msg.textContent = '预设相位：读取中…';
    return;
  }
  var actual = override
    ? ({ linear: '线性相位', minimum: '最小相位', intermediate: '混合相位' }[override] || override)
    : it.phaseName;
  // 2026-10-24 #7：一行说清「预设 / 实际」，不再展开解释什么叫覆盖。
  msg.textContent = '预设 ' + it.phaseName + '（抽头 ' + it.taps + '）· 实际输出 ' + actual + (override ? '（强制覆盖）' : '');
}

// ---- 功能4（用户 2026-10-22）：DeepSeek API Key 的界面入口 ----
// 三条硬约束（对应后端 /api/llm）：
//   ① 界面只显示掩码（sk-****abcd）：GET 只回 mask，输入框永不回填；
//   ② 保存后立刻清空输入框 —— 明文不在 DOM 里多停留一秒；
//   ③ 与 /api/settings 完全分离：key 进 rlplayer-llm.conf，普通设置进另一个文件。
var LLM = { configured: false, masked: '', model: 'deepseek-flash', baseUrl: '', proxy: false, loaded: false };
function llmRender(j) {
  if (j && j.ok) {
    LLM.configured = !!j.configured;
    LLM.masked = j.masked || '';
    LLM.model = j.model || 'deepseek-flash';
    LLM.baseUrl = j.baseUrl || '';
    LLM.proxy = !!j.proxy;
    LLM.loaded = true;
  }
  var m = D('sp_llmmsg');
  if (!m) return;
  if (!LLM.loaded) { m.textContent = '读取配置中…'; return; }
  var s = LLM.configured
    ? ('已配置：' + LLM.masked + '（模型 ' + LLM.model + '）')
    : '未配置 —— 填入 DeepSeek API Key 后点「保存 Key」';
  m.textContent = s + '；key 只存本机 rlplayer-llm.conf，不进安装包、界面从不回显明文' +
    (LLM.proxy ? '；配置里的 proxy= 行已保留' : '');
}
function llmRefresh() {
  return fetch('/api/llm').then(function (r) { return r.json(); }).then(llmRender)
    .catch(function () { var m = D('sp_llmmsg'); if (m) m.textContent = '读取配置失败（服务没响应）'; });
}
function llmPost(body, okMsg) {
  return fetch('/api/llm', { method: 'POST', body: body })
    .then(function (r) { return r.json(); })
    .then(function (j) {
      if (!j || !j.ok) { toast('操作失败：' + ((j && j.error) || '未知错误')); return j; }
      var e = D('sp_llmkey');
      if (e) e.value = '';                       // 明文绝不在 DOM 里停留
      llmRender(j);
      if (okMsg) toast(okMsg);
      return j;
    });
}
function llmSave() {
  var e = D('sp_llmkey');
  var k = e ? String(e.value || '').replace(/^\s+|\s+$/g, '') : '';
  if (!k) { toast('先填入 API Key；留空保存不会改动现有配置'); return; }
  llmPost('key=' + encodeURIComponent(k), 'API Key 已保存（界面只显示掩码）');
}
function llmClear() { llmPost('clear=1', '已清除 API Key（可随时重新填入）'); }
function llmTest() {
  var b = D('sp_llmtest'), m = D('sp_llmmsg');
  if (b) { b.disabled = true; b.textContent = '测试中…'; }
  fetch('/api/llm/test').then(function (r) { return r.json(); }).then(function (j) {
    if (b) { b.disabled = false; b.textContent = '测试连接'; }
    if (!m) return;
    if (j && j.ok) m.textContent = '连接成功：模型 ' + j.model + '，耗时 ' + j.latencyMs + ' ms（key 有效）';
    else m.textContent = '连接失败：' + ((j && j.error) || '未知错误');
  }).catch(function () {
    if (b) { b.disabled = false; b.textContent = '测试连接'; }
    if (m) m.textContent = '连接失败：服务没响应';
  });
}

/* ---------------- 官方推荐预设与硬件能力 ---------------- */
var _capsCache = null;
var _capsTime = 0;

function setSelectValue(sel, val) {
  if (!sel || val === undefined || val === null) return;
  var strVal = String(val);
  var found = false;
  for (var i = 0; i < sel.options.length; i++) {
    if (sel.options[i].value === strVal) { found = true; break; }
  }
  if (!found) {
    var opt = document.createElement('option');
    opt.value = strVal;
    opt.textContent = strVal;
    sel.appendChild(opt);
  }
  sel.value = strVal;
}

function presetCardHtml() {
  var dev = (_capsCache && _capsCache.device) || {};
  var name = dev.name || '检测中…';
  var maxRate = dev.maxRate ? (dev.maxRate + ' Hz') : '- Hz';
  var dsdStr = '-';
  if (dev.dsdNative !== undefined) {
    dsdStr = dev.dsdNative ? ('是（最高 ' + (dev.dsdNativeMax || 'dsd256') + '）') : '否';
  }
  var pcmTargetRate = dev.maxRate ? dev.maxRate : '384000';
  var dsdTarget = (dev.dsdNative && dev.dsdNativeMax) ? dev.dsdNativeMax : (dev.dopMax || dev.dsdNativeMax || 'dsd256');

  var dsdBadge = '';
  if (dev.dsdNative === false) {
    dsdBadge = dev.dop ? '★ 降级为 DoP' : '★ 硬件不支持 DSD';
  }
  var pcmBadge = '';
  if (dev.maxRate && dev.maxRate <= 192000) {
    pcmBadge = '★ 仅到 ' + (dev.maxRate >= 1000 ? (dev.maxRate / 1000 + 'k') : dev.maxRate);
  }

  var h = '<div class="preset-card" id=presetCard>'
        + '<div class=preset-card-head>'
        + '<span class=preset-card-title>'
        + '<svg width=15 height=15 viewBox="0 0 24 24" fill=none stroke=currentColor stroke-width=2 style="color:var(--acc)"><path d="M12 2l3.09 6.26L22 9.27l-5 4.87 1.18 6.88L12 17.77l-6.18 3.25L7 14.14 2 9.27l6.91-1.01L12 2z"/></svg>'
        + '官方推荐预设'
        + '</span>'
        + '<button class="btn" id=presetCapsRefresh title="刷新硬件能力" aria-label="刷新硬件能力" style="font-size:11.5px;padding:3px 9px;display:flex;align-items:center;gap:4px">'
        + '<svg width=12 height=12 viewBox="0 0 24 24" fill=none stroke=currentColor stroke-width=2><path d="M21 12a9 9 0 1 1-3-6.7"/><path d="M21 4v5h-5"/></svg>'
        + '刷新</button>'
        + '</div>'
        + '<div class=preset-caps-line id=presetCapsInfo>'
        + '当前设备：' + esc(name) + ' · 最高 ' + esc(maxRate) + ' · 原生 DSD ' + esc(dsdStr)
        + '</div>'
        + '<div class=preset-grid>'
        + '<button class=preset-btn id=presetBitPerfect aria-label="原音直通（bit-perfect）">'
        + '<div class=preset-btn-top>'
        + '<span class=preset-btn-title>原音直通（bit-perfect）</span>'
        + '<span class=preset-btn-badge id=presetBpBadge style="' + (dev.exclusive === false ? '' : 'display:none') + '">' + (dev.exclusive === false ? '★ 仅共享模式' : '') + '</span>'
        + '</div>'
        + '<div class=preset-btn-sub>不升频、音量 0 dB、无 EQ/IR；只保证不动数据，不等于最好听</div>'
        + '</button>'
        + '<button class=preset-btn id=presetPcm aria-label="PCM 升频">'
        + '<div class=preset-btn-top>'
        + '<span class=preset-btn-title>PCM 升频</span>'
        + '<span class=preset-btn-badge id=presetPcmBadge style="' + (pcmBadge ? '' : 'display:none') + '">' + pcmBadge + '</span>'
        + '</div>'
        + '<div class=preset-btn-sub id=presetPcmSub>升到设备最高档（当前 ' + esc(pcmTargetRate) + '），tight 滤波 + 每相位 512 抽头</div>'
        + '</button>'
        + '<button class=preset-btn id=presetPcm2Dsd aria-label="PCM 升频到 DSD">'
        + '<div class=preset-btn-top>'
        + '<span class=preset-btn-title>PCM → DSD 升频</span>'
        + '</div>'
        + '<div class=preset-btn-sub>PCM 源经 SDM 调制直接出 DSD（' + esc(dsdTarget) + '，EC-7）</div>'
        + '</button>'
        + '<button class=preset-btn id=presetDsd aria-label="DSD 升频">'
        + '<div class=preset-btn-top>'
        + '<span class=preset-btn-title>DSD 升频</span>'
        + '<span class=preset-btn-badge id=presetDsdBadge style="' + (dsdBadge ? '' : 'display:none') + '">' + dsdBadge + '</span>'
        + '</div>'
        + '<div class=preset-btn-sub id=presetDsdSub>升到 ' + esc(dsdTarget) + '，EC-7 调制器</div>'
        + '</button>'
        + '</div>'
        + '</div>';
  return h;
}

function renderPresetCaps(j) {
  if (!j || !j.ok) return;
  var dev = j.device || {};
  var name = dev.name || '默认设备';
  var maxRate = dev.maxRate ? (dev.maxRate + ' Hz') : '未知';
  var dsdStr = dev.dsdNative ? ('是（最高 ' + (dev.dsdNativeMax || 'dsd256') + '）') : '否';

  if (D('presetCapsInfo')) {
    D('presetCapsInfo').textContent = '当前设备：' + name + ' · 最高 ' + maxRate + ' · 原生 DSD ' + dsdStr;
  }
  var pcmTargetRate = dev.maxRate ? dev.maxRate : '384000';
  if (D('presetPcmSub')) {
    D('presetPcmSub').textContent = '升到设备最高档（当前 ' + pcmTargetRate + '），tight 滤波 + 每相位 512 抽头';
  }
  var dsdTarget = (dev.dsdNative && dev.dsdNativeMax) ? dev.dsdNativeMax : (dev.dopMax || dev.dsdNativeMax || 'dsd256');
  if (D('presetDsdSub')) {
    D('presetDsdSub').textContent = '升到 ' + dsdTarget + '，EC-7 调制器';
  }
  if (D('presetDsdBadge')) {
    if (dev.dsdNative === false) {
      D('presetDsdBadge').textContent = dev.dop ? '★ 降级为 DoP' : '★ 硬件不支持 DSD';
      D('presetDsdBadge').style.display = '';
    } else {
      D('presetDsdBadge').style.display = 'none';
    }
  }
  if (D('presetPcmBadge')) {
    if (dev.maxRate && dev.maxRate <= 192000) {
      D('presetPcmBadge').textContent = '★ 仅到 ' + (dev.maxRate >= 1000 ? (dev.maxRate / 1000 + 'k') : dev.maxRate);
      D('presetPcmBadge').style.display = '';
    } else {
      D('presetPcmBadge').style.display = 'none';
    }
  }
  if (D('presetBpBadge')) {
    if (dev.exclusive === false) {
      D('presetBpBadge').textContent = '★ 仅共享模式';
      D('presetBpBadge').style.display = '';
    } else {
      D('presetBpBadge').style.display = 'none';
    }
  }
}

function loadPresetCaps(force) {
  var now = Date.now();
  if (!force && _capsCache && (now - _capsTime < 10000)) {
    renderPresetCaps(_capsCache);
    return Promise.resolve(_capsCache);
  }
  return fetch('/api/preset/caps').then(function (r) {
    return r.json();
  }).then(function (j) {
    if (j && j.ok) {
      _capsCache = j;
      _capsTime = Date.now();
      renderPresetCaps(j);
    }
    return j;
  }).catch(function (err) {
    console.error('loadPresetCaps error:', err);
  });
}

function applyPresetControls(applied, profile) {
  if (!applied || typeof applied !== 'object') return;

  var r = applied.rate !== undefined ? applied.rate : applied.sRate;
  if (r === undefined && profile === 'bitperfect') r = 'follow';
  if (r !== undefined) {
    setSelectValue(D('sRate'), r);
    setSelectValue(D('sp_rate'), r);
  }

  var f = applied.filter !== undefined ? applied.filter : applied.sFilt;
  if (f !== undefined) {
    setSelectValue(D('sFilt'), f);
    setSelectValue(D('sp_filt'), f);
  }

  var fnx = applied.filterNx !== undefined ? applied.filterNx : applied.sFiltNx;
  if (fnx !== undefined) {
    setSelectValue(D('sFiltNx'), fnx);
    setSelectValue(D('sp_filtNx'), fnx);
  }

  var sh = applied.shaper !== undefined ? applied.shaper : applied.sShap;
  if (sh === undefined && profile === 'bitperfect') sh = 'none';
  if (sh !== undefined) {
    setSelectValue(D('sShap'), sh);
    setSelectValue(D('sp_shaper'), sh);
  }

  var dsd = applied.dsd !== undefined ? applied.dsd : applied.sDSD;
  if (dsd === undefined && (profile === 'bitperfect' || profile === 'pcm')) dsd = '0';
  if (dsd !== undefined) {
    setSelectValue(D('sDSD'), dsd);
    if (D('sp_outmode')) D('sp_outmode').value = outModeOf();
    if (D('sp_dsdrate')) D('sp_dsdrate').value = dsdRateOf();
    if (D('sp_dsdpk')) D('sp_dsdpk').value = dsdPackOf();
    applyDsdState();
  }

  var buf = applied.buffer !== undefined ? applied.buffer : applied.sBuf;
  if (buf !== undefined) {
    if (D('sBuf')) D('sBuf').value = String(buf);
    if (D('sp_buffer')) D('sp_buffer').value = String(buf);
  }

  var vol = applied.volume !== undefined ? applied.volume : applied.vol;
  if (vol === undefined && profile === 'bitperfect') vol = 0;
  if (vol !== undefined) {
    var vNum = Number(vol) || 0;
    S.vol = vNum;
    S.volDb = vNum;
    if (D('vol')) D('vol').value = String(vNum);
    if (D('volDb')) D('volDb').textContent = vNum.toFixed(1) + ' dB';
    api('/api/volume', 'vol=' + vNum).catch(function () {});
  }

  var isp = applied.isp !== undefined ? applied.isp : applied.sIsp;
  if (isp === undefined && profile === 'bitperfect') isp = false;
  if (isp !== undefined) {
    var ispBool = (isp === true || isp === 1 || isp === '1');
    if (D('sIsp')) D('sIsp').checked = ispBool;
    if (D('sp_isp')) D('sp_isp').checked = ispBool;
    ADV.isp = ispBool;
  }

  var s2s = applied.twostage !== undefined ? applied.twostage : applied.s2s;
  if (s2s !== undefined) {
    var s2sBool = (s2s === true || s2s === 1 || s2s === '1');
    if (D('s2s')) D('s2s').checked = s2sBool;
    if (D('sp_2s')) D('sp_2s').checked = s2sBool;
  }

  if (applied.backend !== undefined) {
    if (D('sp_backend')) D('sp_backend').value = applied.backend;
    ADV.backend = applied.backend;
    applyBackendState();
  }

  for (var k in applied) {
    var el = D(k);
    if (el) {
      if (el.tagName === 'SELECT') setSelectValue(el, applied[k]);
      else if (el.type === 'checkbox') el.checked = !!applied[k];
      else el.value = applied[k];
    }
  }

  saveSettings(true);
  tick();
  if (typeof syncLiveSignal === 'function') syncLiveSignal();
  if (typeof updateSignalChain === 'function') updateSignalChain();
  if (D('modal') && D('modal').classList.contains('on')) showChain();
}

function applyPreset(profile) {
  var bBp = D('presetBitPerfect'), bPcm = D('presetPcm'), bDsd = D('presetDsd');
  if (bBp) bBp.disabled = true;
  if (bPcm) bPcm.disabled = true;
  if (bDsd) bDsd.disabled = true;

  return api('/api/preset/apply?profile=' + encodeURIComponent(profile), '').then(function (j) {
    if (bBp) bBp.disabled = false;
    if (bPcm) bPcm.disabled = false;
    if (bDsd) bDsd.disabled = false;

    if (!j || !j.ok) {
      toast((j && (j.error || j.message)) || '应用预设失败');
      return j;
    }

    if (j.applied) {
      applyPresetControls(j.applied, profile);
    }

    if (j.notes && Array.isArray(j.notes) && j.notes.length) {
      j.notes.forEach(function (note, idx) {
        setTimeout(function () { toast(note); }, idx * 1200);
      });
    } else {
      toast('已应用 ' + (profile === 'bitperfect' ? '原音直通' : (profile === 'pcm' ? 'PCM 升频' : 'DSD 升频')) + ' 预设');
    }
    return j;
  }).catch(function (err) {
    if (bBp) bBp.disabled = false;
    if (bPcm) bPcm.disabled = false;
    if (bDsd) bDsd.disabled = false;

    var msg = (err && (err.message || err.error)) || String(err) || '应用预设失败';
    toast(msg);
  });
}

function bindPresetCard() {
  if (D('presetCapsRefresh')) {
    D('presetCapsRefresh').onclick = function () {
      var btn = this;
      btn.disabled = true;
      loadPresetCaps(true).then(function () {
        btn.disabled = false;
        toast('硬件能力已刷新');
      }).catch(function () {
        btn.disabled = false;
      });
    };
  }
  if (D('presetBitPerfect')) {
    D('presetBitPerfect').onclick = function () { applyPreset('bitperfect'); };
  }
  if (D('presetPcm')) {
    D('presetPcm').onclick = function () { applyPreset('pcm'); };
  }
  if (D('presetDsd')) {
    D('presetDsd').onclick = function () { applyPreset('dsd'); };
  if (D('presetPcm2Dsd')) D('presetPcm2Dsd').onclick = function () { applyPreset('pcm2dsd'); };
  }
}

function renderSettingsPage() {
  var filts = D('sFilt') ? D('sFilt').innerHTML : '';
  // Nx 下拉必须带上"跟随 1x"（空值），否则重建后浏览器退回第一项，一保存就把 Nx 改掉
  var filtsNx = '<option value="">跟随 1x（不单独指定）</option>' + filts;
  var shaps = D('sShap') ? D('sShap').innerHTML : '';
  var BK = ADV.backend || 'wasapi';
  var CH = String(ADV.channels || 0);
  var h = '';

  // 2026-10-26（261026-01 #2）：官方推荐预设卡片**移进「输出」面板**（用户：不要做在所有东西外面）。
  // ---------- 页签（对齐 HQPlayer：Inputs / Outputs / PCM / SDM / Advanced） ----------
  h += '<div class=stabs>'
     + '<button class="stab on" data-g=in>输入</button>'
     + '<button class=stab data-g=out>输出</button>'
     + '<button class=stab data-g=pcm>PCM</button>'
     + '<button class=stab data-g=sdm>SDM</button>'
     + '<button class=stab data-g=adv>高级</button>'
     // #6（2026-10-27 用户 261026-09）：EQ 页内容臃肿 ⟹ 拆成 EQ / 声音校准 两个页签。
     + '<button class=stab data-g=eq>EQ</button>'
     + '<button class=stab data-g=cal>声音校准</button>'
     + '<button class=stab data-g=meta>元数据</button>'
     // 2026-10-24（用户 261024-01 #5）：「流媒体」从「高级」拆出来的独立页签。
     + '<button class=stab data-g=stream>流媒体</button>'
     // 「诊断」独立成页：用户 2026-10-22 功能3 明确要求不要塞在元数据里。
     // 开发期它是必需品（报错时要拿现场），所以与输入/输出/PCM/SDM/高级 平级。
     + '<button class=stab data-g=diag>诊断</button>'
     + '</div>';

  // ---------- 输出 ----------
  // 2026-10-26（261026-01 #2）：官方推荐预设卡片放在这里（用户要求「做到输出里面」）
  h += '<div class="spane" data-g=out>'
     // 2026-10-26（261026-01 #2）：官方推荐预设放在「输出」面板最上方（用户要求「做到输出里面」）。
     + presetCardHtml()
     + sgroup('输出后端', '独占 = bit-perfect；共享 = 兼容性最好（虚拟机 / 远程桌面）',
         // 261024-04 #2：用户要求「放在后端那个选项的右边」—— bit-perfect 直通按钮从独立一行挪进
         // 「后端」下拉的同一行（flex 行，底对齐；下拉可缩、按钮 nowrap，窄窗也不换行）。
         // 原来铺在页面上的一长串判据收进按钮的 tooltip，行内只留一个按钮。
         '<div class="sfld wide" style="display:flex;align-items:flex-end;gap:10px;flex-wrap:nowrap">'
       + '<div style="flex:1 1 auto;min-width:180px">'
       + '<label>后端' + tip('选到当前不可用的后端时，播放会直接返回原因，不会假装出声') + '</label>'
       + '<select id=sp_backend>'
       +   opt('wasapi', 'WASAPI（独占 · 事件驱动）', BK)
       +   opt('wasapi-shared', 'WASAPI 共享（兼容虚拟机 / 远程桌面）', BK)
       +   opt('asio', 'ASIO', BK)
       // 261024-05：NAA 后端已删除（用户「NAA 放弃，彻底删除这一功能」）。
       // 数播的 NAA 只认 HQPlayer 私有握手，实测 8 种握手变体全部无回应，且 EULA 禁止逆向。
       +   opt('upnp', 'UPnP / DLNA 投放（局域网渲染器）', BK)
       +   opt('null', 'Null output（只跑 DSP，不出声）', BK)
       + '</select></div>'
       + '<button class="btn pri" id=sp_bpfix style="flex:none;white-space:nowrap" ' +
         'title="判据：输出采样率 = 源 / 抖动 = none / 音量 0 dB / 无 EQ·IR / 独占 WASAPI">切到 bit-perfect 直通</button>'
       + '</div>'
      // 261024-05：NAA 端点选择/发现区块已删除（整个后端一起去掉）。
      // 261024-01 #9：后端下拉新增「UPnP / DLNA 投放」。选中后这里直接选目标渲染器，
      // 与下方「投放」区块共用同一份发现结果、同一个选择、同一条会话（不出现两套打架的控制）。
      + '<div class="sfld wide" id=sp_upnpwrap style="display:none">'
      + '<label>目标渲染器（UPnP / DLNA）</label>'
      + '<div style="display:flex;gap:8px">'
      + '<select id=sp_upnp_dev style="flex:1"><option value="">（正在发现…）</option></select>'
      + '<button class=btn id=sp_upnp_scan style="white-space:nowrap">重新发现</button>'
      + '</div>'
      + '<div class=hint id=sp_upnp_msg>播放 / 切歌自动投放到该渲染器（与下方「投放」共用会话）</div>'
      + '</div>'
       // 2026-10-24（261024-02 #8）：一键直通预设（源采样率 + 无抖动 + 0 dB + 无 EQ/IR + 独占）。
       // 261024-04 #2：按钮已挪到「后端」下拉右侧的同一行（见上），这里不再占一行。
       + sel('sp_device', '输出设备', '<option value="">默认设备</option>', '独占模式会接管这个设备；共享模式经系统混音，不接管')
       + sel('sp_outmode', '输出模式',
             opt('pcm', 'PCM', outModeOf()) + opt('dsd', 'DSD（SDM）', outModeOf()),
             'DSD 的比特率与封装（DoP / Native）在 SDM 页设置')
       + sel('sp_ch', '输出声道数',
             opt('0', '跟随源声道数', CH) + opt('2', '2 声道（立体声）', CH) +
             opt('6', '5.1（6 声道）', CH) + opt('8', '7.1（8 声道）', CH),
             '设备必须支持该声道数；多声道用 WAVEFORMATEXTENSIBLE 的声道掩码声明布局')
       + num({ id: 'sp_buffer', label: '缓冲（毫秒）' }, D('sBuf').value, 10, 500, 10, '越小延迟越低，但更容易欠载')
       + num({ id: 'sp_ramp', label: '音量渐变（毫秒）' }, ADV.ramp, 0, 2000, 10, '0 = 立即生效（可能爆音）')
       + '<div class="sfld wide"><div id=sp_backendmsg class=hint></div></div>')
     // #4（2026-10-27 用户 261026-09）：「输出设备选 VBMatrix In 2，为什么下面声道音量微调还是 2.0？」
     // ⟹ 行数由 effectiveChannels() 决定（共享 = 设备混音声道数；独占 = 显式设置或设备混音声道数），
     //    依据写在 id=sp_trimchmsg 那一行，用户一眼能对上 Windows 的声音设置。
     + '<div id=sp_trimwrap>' + sgroup('声道音量微调', '按设备实际声道（Windows 混音格式）生成，0 dB = 不动',
         '<div class="sfld wide"><div class=trimgrid>'+ trimFields(effectiveChannels()) + '</div>' +
         '<div class=hint id=sp_trimchmsg>按 ' + effectiveChannels() + ' 声道 显示</div>' +
         '<div class=hint>2 = 左/右；5.1 = 左/右/中置/低音/左环绕/右环绕；7.1 再加左后/右后。' +
          '行数来自所选设备的混音声道数（/api/devices 的 GetMixFormat 只读探测，不开流不出声）</div></div>') + '</div>'
     // #5（261026-09）：等响度补偿从「高级-播放」挪到这里 —— 它是唯一可见入口，不能随那栏一起删。
     // 放「输出」页的理由：它跟音量/输出电平是一件事，和「声道微调」正好挨着。
     + sgroup('音量与等响度', '等响度补偿 = 音量越低，低频按等响曲线补得越多（loudness.h）',
         chk('sp_loud', '等响度补偿', D('sLoud').checked, '音量越低低频提升越多；写入 conf 键 loudness，下次播放生效'))
      // 2026-10-27（261027-01 #2）：ReplayGain / Album Gain。三模式 + 防削波。
      // 语义（唯一真相源 = src/dsp/replaygain.h 文件头）：
      //   off   ⟹ 增益恒 0 ⟹ 管线段整段跳过 ⟹ 逐位直通（默认，必须保持）；
      //   track ⟹ 逐曲标签/逐曲响度换算；album ⟹ 专辑标签/专辑响度（缺了才退回逐曲）。
      //   防削波 = applied = min(gain, −peak_dBFS)，峰值取【源峰值】（所以增益乘在升频之前）。
      + sgroup('ReplayGain', '统一不同母带的响度：按标签把每首歌拉到同一听感电平',
         sel('sp_rgmode', '模式',
             opt('off', '关闭（bit-perfect 直通）', ADV.rgmode || 'off')
           + opt('track', 'ReplayGain（逐曲）', ADV.rgmode || 'off')
           + opt('album', 'Album Gain（整专辑）', ADV.rgmode || 'off'),
             '逐曲 = 每首歌各自归一；整专辑 = 一张专辑内保持相对响度（古典/流行专辑推荐）')
       + chk('sp_rgclip', '防削波', ADV.rgclip != 0,
             '把增益限到 −peak_dBFS（源峰值），保证不会因为我们加的增益而削波；源本身就过载时会反而压低', true)
       // ⚠️ 文案常量取自 dsp/replaygain.h:146 的 kBitPerfectWarning（** 是 markdown 标记，界面不显示）
       + '<div class="sfld wide"><div class=hint id=sp_rgwarn style="display:none;color:var(--err)">'
       + 'ReplayGain/Album Gain 会改变样本值：开启后不是 bit-perfect（默认关闭时逐位直通）。</div></div>')
      + '</div>';
     // 261024-02 #5：用户明确要求「输出栏位下的 UPnP 投放功能多余」⟹ 整块删除
     // （原「投放（UPnP）」快捷区 + 「投放实时状态」网格）。投放的入口统一挪到
     // 单曲页「推送到设备」按钮与设置→流媒体页的「推送到设备」区块，三处共用 /api/cast/state。

  // ---------- PCM ----------
  h += '<div class=spane data-g=pcm>'
     // 2026-10-25（261025-02 #4）：用户「输出采样率/位深应该放到 PCM 里才对」——
     // 它们是 PCM 链路的属性（DSD 恒 1 bit、速率在 SDM 页），放在「输出」页里会让人以为
     // 输出页没有 SDM 的内容。id 不变（sRate/sBits），只挪 DOM 位置，保存/读取逻辑零改动。
     + sgroup('输出格式（PCM）', 'DSD 输出用 SDM 页的档位，这里只影响 PCM 链路',
         sel('sp_rate', '输出采样率', D('sRate').innerHTML, '自动 = 所选设备的最高采样率')
       + sel('sp_bits', '输出位深（PCM）',
             opt(0, '24 bit', ADV.bits) + opt(16, '16 bit', ADV.bits) + opt(32, '32 bit', ADV.bits),
             'DSD 输出恒为 1 bit，此项只在 PCM 模式生效'))
     + sgroup('升频与滤波', 'HQPlayer 的 poly-sinc 家族',
         sel('sp_filt', '1x 滤波器（44.1 / 48 kHz 音源）', filts, '播放 44.1 / 48 kHz 文件时使用')
       + sel('sp_filtNx', 'Nx 滤波器（88.2 kHz 及以上音源）', filtsNx,
             '播放 88.2 / 96 / 176.4 kHz 等文件时使用；留空 = 与 1x 相同')
       + sel('sp_phase', '相位（覆盖滤波器预设的相位）',
             opt('', '跟随预设（用所选滤波器自带的相位）', ADV.phase) +
             opt('linear', '强制线性相位', ADV.phase) +
             opt('minimum', '强制最小相位', ADV.phase) +
             opt('intermediate', '强制混合相位（线性/最小 各 50%）', ADV.phase),
             '「跟随预设」= 不覆盖滤波器自带相位。强制项会覆盖预设：' +
             '线性 = 无相位失真、有前振铃；最小 = 无预振铃、群延迟低；混合 = 两者按 50:50 叠加（幅频不变）')
       + '<div class="sfld wide"><div class=hint id=sp_phasemsg></div></div>'
       + num({ id: 'sp_taps', label: '原型抽头数（0 = 按预设自动）' }, ADV.taps, 0, 2097152, 1024,
             '多相分解后每一相的抽头数 ≈ 抽头数 ÷ 升频比；0 = 用所选滤波器预设自带长度')
       + num({ id: 'sp_atten', label: '阻带衰减（dB，0 = 用预设）' }, ADV.atten, 0, 300, 10,
             '目标阻带深度；实际能不能达到还要看抽头数')
       + num({ id: 'sp_transition', label: '过渡带占比（0 = 用预设）' }, ADV.transition, 0, 0.45, 0.005,
             '截止点相对理想奈奎斯特回退的比例，例如 0.04 = 回退 4%')
       + chk('sp_2s', '两段式升频', D('s2s').checked, '大比率时 CPU 运算量约 1/4'))
     + sgroup('抖动与保护', '按采样率自动重标定',
         sel('sp_shaper', '抖动 / 噪声整形', shaps, 'auto = 按输出位深自动选型')
       + chk('sp_isp', '过采样峰值保护（ISP）', D('sIsp').checked, '检测到插值峰值超满幅时预衰减'))
     + '</div>';
  // ---------- SDM ----------
  h += '<div class=spane data-g=sdm>'
     + sgroup('SDM 输出', '只在输出 DSD 时生效',
         sel('sp_sdmo1', 'Oversampling 1x（44.1 / 48 kHz 音源）',
             '<option value="">跟随 PCM 页的 1x</option>' + filts,
             '与 PCM 页独立：输出 DSD 时用这一组滤波器。留空则沿用 PCM 页的 1x')
       + sel('sp_sdmoNx', 'Oversampling Nx（88.2 kHz 及以上音源）', '<option value="">跟随上面的 1x</option>' + filts,
             '输出 DSD 且源文件 >= 88.2 kHz 时使用')
       + sel('sp_dsdrate', '比特率',
             opt('64', 'DSD64（2.8224 MHz）', dsdRateOf()) + opt('128', 'DSD128（5.6448 MHz）', dsdRateOf()) +
             opt('256', 'DSD256（11.2896 MHz）', dsdRateOf()) + opt('512', 'DSD512（22.5792 MHz）', dsdRateOf()) +
             opt('1024', 'DSD1024（45.1584 MHz）', dsdRateOf()),
             'DSD512 / 1024 的 DoP 载波是 1411.2 / 2822.4 kHz，绝大多数设备不支持')
       + sel('sp_dsdpk', '封装',
             opt('dop', 'DoP（WASAPI / ASIO 均可）', dsdPackOf()) +
             opt('nat', 'Native（仅 ASIO）', dsdPackOf()),
             '两者数值等价，差别只在传输方式；Native 走 ASIO 原生 DSD（1-bit 直送驱动）。' +
             '前提：后端 ASIO、源为 DSD、音量 0 dB、无 EQ / 卷积 / 升频，否则会明确报错；' +
             '驱动拒绝原生 DSD 时自动回落 DoP，并在信号链里标出')
       // 2026-10-24（用户 261024-01 #12）：这两个控件不再灰掉（见 applyDsdState）。
       // 当前输出是 PCM 时，改这里会自动把输出模式联动到 DSD —— 一句常驻说明交代清楚。
       + '<div class="sfld wide" id=sp_dsdmodenote style="display:none"><div class=hint>' +
         '当前输出 PCM：改动比特率 / 封装会自动把输出切到 DSD（SDM）' + '</div></div>'
       + sel('sp_modfamily', '调制器族',
              opt('dsd', 'DSD 标准（Butterworth NTF，1-bit）', ADV.modfamily || 'dsd') +
              opt('chebyshev', 'Chebyshev（可配带外增益与截止，1-bit 保护运行于 5 阶）', ADV.modfamily || 'dsd') +
              opt('ec', 'EC 误差反馈（频带零点扩展消除）', ADV.modfamily || 'dsd') +
              opt('ahm', 'AHM 多电平（6~8 bit 风格）', ADV.modfamily || 'dsd'),
              '多种 NTF 滤波与量化架构')
        + sel('sp_modorder', '调制器阶数',
             opt(5, '5 阶（DSD64 起）', ADV.modorder || 5) + opt(7, '7 阶（需 DSD128 以上）', ADV.modorder || 5),
             '7 阶在低采样率或 Chebyshev 1-bit 下不稳定，程序会自动降为 5 阶')
       + num({ id: 'sp_modcut', label: '噪声整形拐点（kHz，0 = 100）' }, ADV.modcut, 0, 200, 5)
       + num({ id: 'sp_moddith', label: '调制器抖动（LSB，-1 = 默认）' }, ADV.moddith, -1, 1, 0.001, '抑制空闲音'))
     + sgroup('SDM 源（DSD 文件）', '原生读取 + DirectSDM',
         chk('sp_directsdm', 'DirectSDM：DSD 源同速率直通（逐位一致）', ADV.directsdm == 1,
             '源 DSD 速率 == 目标速率、音量 0 dB、无 EQ / 卷积 / 声道改动时，1-bit 流直接装进 DoP；' +
             '条件不满足自动回退普通路径，并在信号链里写明原因。')
       + sel('sp_dsdkind', 'DSD 源抽取器',
             opt('fir', 'FIR（Kaiser 120 dB，默认）', ADV.dsdKind || 'fir') +
             opt('iir', 'IIR（Butterworth 10 阶，相位非线性）', ADV.dsdKind || 'fir') +
             opt('cic', 'CIC（6 级无乘法 + 补偿）', ADV.dsdKind || 'fir'),
             '只在「DSD 源 → PCM」路径生效；DirectSDM 直通时整条 PCM 链都被跳过')
       + sel('sp_dsdconv', 'DSD 源 Conversion',
             opt('narrow', 'narrow（通带 20 kHz，默认）', ADV.dsdConv || 'narrow') +
             opt('wide', 'wide（通带 30 kHz）', ADV.dsdConv || 'narrow') +
             opt('xfi', 'XFi（通带 40 kHz）', ADV.dsdConv || 'narrow'),
             '通带越宽，阻带起点越靠近、过渡带越窄、抽头越多')
       + '<div class="sfld wide"><div class=hint id=sp_dsdmsg></div></div>')
     + '</div>';
  // ---------- 输入 ----------
  h += '<div class="spane on" data-g=in>'
     + sgroup('输入设备与采集', 'WASAPI 共享模式采集 / 录制与直通监听',
         sel('sp_inbackend', '输入后端', opt('none', '[none]', ADV.inBackend || 'none') + opt('wasapi', 'WASAPI', ADV.inBackend || 'none'),
             '选 WASAPI 后可以采集线路输入 / 麦克风 / 系统回环')
       + sel('sp_indev', '输入设备', '<option value="">（未选择 / 默认设备）</option>', '包含物理采集设备与 [Loopback] 系统混音回环端点')
       + sel('sp_inch', '声道偏移', opt('0', '0', ADV.inOffset || 0) + opt('1', '1', ADV.inOffset || 0) +
             opt('2', '2', ADV.inOffset || 0) + opt('4', '4', ADV.inOffset || 0),
             '多声道接口只取其中几路时用')
       + num({ id: 'sp_inbuf', label: '输入缓冲（毫秒）' }, ADV.inBuf || 100, 10, 500, 10, '环形缓冲时间深度')
       + chk('sp_inmon', '直通监听（非 bit-perfect）', ADV.inMonitor == 1,
             '勾选后把采集直接送到系统默认共享端点监听；不勾选则录制保存为 WAV')
       + num({ id: 'sp_inseconds', label: '录制时长（秒，0 为不限）' }, ADV.inSeconds || 0, 0, 86400, 1,
             '达到指定时长自动停止；0 = 持续采集直到手动停止')
       + '<div class="sfld wide"><label>输出文件路径（WAV）' +
           tip('仅录制模式生效；留空自动保存在程序目录 recordings 下') + '</label><input type=text id=sp_inout value="' +
             esc(ADV.inOut || '') + '" placeholder="留空自动存至 recordings' + SEP + 'cap-时间戳.wav"></div>'
       + '<div class="sfld wide" style="display:flex;align-items:center;gap:10px;margin-top:2px">'
       + '<button class="btn pri" id=sp_instart>开始采集</button>'
       + '<button class=btn id=sp_instop disabled>停止采集</button>'
       + '<span id=sp_inmsg class=hint style="margin:0"></span>'
       + '</div>')
     + sgroup('采集实时状态', '1 秒自动轮询',
         '<div class="sfld wide in-stat-grid">'
       + '<div class=in-stat-item><span class=lbl>运行状态</span><span class=val id=sp_stat_running>空闲</span></div>'
       + '<div class=in-stat-item><span class=lbl>采集帧数 / 时长</span><span class=val id=sp_stat_frames>0 帧 (0.0s)</span></div>'
       + '<div class=in-stat-item><span class=lbl>采样规格</span><span class=val id=sp_stat_spec>-</span></div>'
       + '<div class=in-stat-item><span class=lbl>缓冲水位</span><span class=val id=sp_stat_water>0.0%</span>'
       + '<div class=meter-bar><div id=sp_meter_water class=meter-val style="width:0%"></div></div></div>'
       + '<div class=in-stat-item><span class=lbl>溢出 / 欠载</span><span class=val id=sp_stat_xrun>溢出 0 / 欠载 0</span></div>'
       + '<div class=in-stat-item><span class=lbl>削波计数 (正 / 负)</span><span class=val id=sp_stat_clips>+0 / -0</span></div>'
       + '<div class=in-stat-item><span class=lbl>RMS 电平</span><span class=val id=sp_stat_rms>-∞ dBFS</span>'
       + '<div class=meter-bar><div id=sp_meter_rms class=meter-val style="width:0%"></div></div></div>'
       + '<div class=in-stat-item><span class=lbl>峰值电平</span><span class=val id=sp_stat_peak>-∞ dBFS</span>'
       + '<div class=meter-bar><div id=sp_meter_peak class=meter-val style="width:0%"></div></div></div>'
       + '<div class=in-stat-item style="grid-column:1/-1"><span class=lbl>输出文件</span><span class="val sm" id=sp_stat_out>-</span></div>'
       + '<div class=in-stat-item id=sp_stat_err_box style="grid-column:1/-1;display:none;color:var(--err)">'
       + '<span class=lbl style="color:var(--err)">错误信息</span><span class=val id=sp_stat_err></span></div>'
       + '</div>')
     + '</div>';


  // ---------- 高级 ----------
  h += '<div class=spane data-g=adv>'
     + sgroup('外观',
         '<div class=sfld><label>主题' + tip('顶栏右侧的小圆点也可以快速切换主题') + '</label><select id=sp_theme>'
       + opt('dark', '深色（默认）', S.theme) + opt('light', '白色', S.theme) + opt('blue', '浅蓝色', S.theme)
       + '</select></div>')
     + sgroup('GPU',
         sel('sp_gpu', 'GPU 后端',
             opt('auto', '自动', ADV.gpuvendor || 'auto') + opt('amd', 'AMD（HIP）', ADV.gpuvendor || 'auto') +
             opt('nvidia', 'NVIDIA（CUDA）', ADV.gpuvendor || 'auto') +
             opt('off', '关闭（纯 CPU）', ADV.gpuvendor || 'auto'),
             '自动 = 依次尝试 AMD HIP → DX12 → NVIDIA CUDA，选第一个真正能跑起来的后端；' +
             '后端缺失或 kernel 不可执行时自动退回 CPU 分块 FFT')
       + '<div class="sfld wide"><div id=sp_gpumsg class=hint></div></div>'
       + '<div class="sfld wide"><div id=sp_asiomsg class=hint></div></div>')
     // 2026-10-24（用户 261024-01 #5/#6）：
     //   ① 「流媒体服务器」整组搬去新增的「流媒体」页签（这里不再保留副本，避免重复 id）；
     //   ② 「音乐库与网络」改名「本地音乐库」—— 只放本地库根目录 / 扫描 / 线程；
     //   ③ 原组里的「发现 DLNA 设备」与结果区搬去「流媒体」页（DLNA 归流媒体）。
     + sgroup('本地音乐库', '本地目录扫描入索引；DLNA / UPnP 在「流媒体」页',
         '<div class="sfld wide libstats" id=sp_libstats></div>'
       + txt('sp_root', '添加音乐目录', '', '填好后点「添加目录」', 'D:\\Music')
       + '<div class="sfld wide" style="display:flex;gap:8px;flex-wrap:wrap">'
       + '<button class="btn" id=sp_addroot>添加目录</button> <button class="btn" id=sp_rescan>重新扫描全部</button></div>'
       + num({id:'sp_scanthreads', label:'扫描线程数'}, ADV.scanThreads || 0, 0, 32, 1, '0 = 自动（默认 min(CPU, 8)）')
       + '<div class="sfld wide"><div id=sp_roots class=rootlist></div></div>')
      // #5（2026-10-27 用户 261026-09）：「设置-高级-播放，这一栏全部删除，所有功能已经在外面有了」。
      // 逐项核对结论（**不是全删**，删之前每项都查了外部入口）：
      //   ① sp_rep「播放模式」：外部有 —— 底栏 #bRep（四态按钮）+ 单曲页 #npRep，都走同一个 cycleRep()。
      //      删掉这里只是去掉一份镜像，功能零损失。
      //   ② sp_shuf「随机播放」隐藏镜像：本来就由播放模式派生、无人可见 ⟹ 一起删。
      //      （旧注释说 tools/uicheck.js 的 needBind 里有 sp_shuf —— 核对后**没有**：
      //        清单里是隐藏源控件 #sShuf，删 sp_shuf 不影响自检。）
      //   ③ sp_loud「等响度补偿」：**外部没有任何入口**（grep 全仓库只此一处可见控件；
      //      #sLoud 是 display:none 的参数源，信号链页只是只读展示）⟹ 按「不许把功能删没了」
      //      的约束**保留**，并挪到「输出」页的「音量与等响度」组（原来挂在「高级-播放」里）。
      + '</div>';
  // EQ       = 20 段 PEQ + 耳机 EQ（OPRA）      —— 手调，改的是"音色"
  // 声音校准 = 房间校正向导 + 校正文件（EQ/IR） —— 测量与外部文件，改的是"系统误差"
  h += '<div class=spane data-g=eq>' + eqSettingsHtml() + '</div>';
  h += '<div class=spane data-g=cal>' + calSettingsHtml() + '</div>';

  // ---------- 流媒体（用户 261024-01 #5/#6：从「高级」拆成独立页签 + 多服务器 + DLNA 归位）----------
  h += '<div class=spane data-g=stream>'
     + sgroup('流媒体服务器', '可保存多台，同一时刻启用一台；只取原始文件，不转码',
         '<div class="sfld wide" style="display:flex;align-items:center;gap:10px;flex-wrap:wrap">'
       + '<button class="btn pri" id=sp_src_add>添加服务器</button></div>'
       + '<div class="sfld wide"><div id=sp_src_list class=srvlist></div></div>'
       + '<div class="sfld wide" id=sp_src_form style="display:none">'
       + '<div class=srvform>'
       + '<div class=mh id=sp_src_ftitle>添加服务器</div>'
       + '<div class=sfld><label>名称</label><input type=text id=sp_src_name placeholder="例如 客厅 NAS"></div>'
       + '<div class=sfld><label>类型</label><select id=sp_src_kind>'
       + '<option value=subsonic>Navidrome / Subsonic</option><option value=emby>Emby</option></select></div>'
       + '<div class="sfld wide"><label>服务器地址' + tip('带 http:// 或 https://，例如 http://192.168.1.10:4533') + '</label>'
       + '<input type=text id=sp_src_base placeholder="http://192.168.1.10:4533"></div>'
       + '<div class=sfld><label>用户名</label><input type=text id=sp_src_user autocomplete=off spellcheck=false></div>'
       + '<div class=sfld><label>密码' + tip('编辑已有服务器时留空 = 保持原密码不变；密码只存本机 rlplayer-sources.conf') + '</label>'
       + '<input type=password id=sp_src_pass autocomplete=new-password placeholder="留空 = 不修改"></div>'
       + '<div class="sfld wide" style="display:flex;align-items:center;gap:8px;flex-wrap:wrap">'
       + '<button class="btn pri" id=sp_src_save>保存</button>'
       + '<button class=btn id=sp_src_testform>测试连接</button>'
       + '<button class=btn id=sp_src_cancel>取消</button>'
       + '<span class=hint id=sp_src_fmsg style="margin:0"></span></div>'
       + '</div></div>')
     // 261024-02 #5：用户原话「你现在只有一个发现DLNA设备按钮，但是没有推送上去的按钮，导致无法测试」
     // ⟹ 这里从"只发现"升级为完整推送区：设备下拉 + 推送当前曲 / 停止投放 + 实时状态。
     // 状态与单曲页按钮、底栏徽标同源（/api/cast/state，1 秒轮询）。
     + sgroup('推送到设备（UPnP / DLNA）', '一键把当前曲目推送到局域网渲染器；之后的切歌 / 暂停 / 进度都作用在它上面',
         '<div class="sfld wide" style="display:flex;gap:8px;align-items:flex-end;flex-wrap:wrap">'
       + '<select id=sp_cast_dev style="flex:1;min-width:220px"><option value="">（未发现 UPnP 渲染器）</option></select>'
       + '<button class=btn id=sp_cast_scan style="white-space:nowrap">重新发现</button></div>'
       + '<div class="sfld wide"><div class=hint id=sp_cast_devmsg>点「重新发现」扫描局域网里的 MediaRenderer</div></div>'
       + '<div class="sfld wide" style="display:flex;align-items:center;gap:10px;flex-wrap:wrap;margin-top:2px">'
       + '<button class="btn pri" id=sp_cast_push>推送当前曲</button>'
       + '<button class=btn id=sp_cast_stop disabled>停止投放</button>'
       + '<span id=sp_cast_msg class=hint style="margin:0"></span></div>'
       + '<div class="sfld wide in-stat-grid" style="margin-top:6px">'
       + '<div class=in-stat-item><span class=lbl>运行阶段</span><span class=val id=sp_cast_stat_phase>已停止</span></div>'
       + '<div class=in-stat-item><span class=lbl>目标渲染器</span><span class="val sm" id=sp_cast_stat_dev>-</span></div>'
       + '<div class=in-stat-item><span class=lbl>播放进度</span><span class=val id=sp_cast_stat_pos>0:00</span></div>'
       + '<div class=in-stat-item><span class=lbl>传输状态</span><span class=val id=sp_cast_stat_transport>STOPPED</span></div>'
       + '<div class=in-stat-item><span class=lbl>供源传输字节</span><span class=val id=sp_cast_stat_bytes>0 B</span></div>'
       + '<div class=in-stat-item><span class=lbl>供源请求数</span><span class=val id=sp_cast_stat_reqs>0 次</span></div>'
       + '<div class=in-stat-item><span class=lbl>供源端口</span><span class=val id=sp_cast_stat_port>8912</span></div>'
       + '<div class=in-stat-item><span class=lbl>采样率取证</span><span class=val id=sp_cast_stat_ratematch>未取证</span></div>'
       + '<div class=in-stat-item id=sp_cast_stat_warn_box style="grid-column:1/-1;display:none;color:var(--warn, #eab308)">'
       + '<span class=lbl style="color:var(--warn, #eab308)">采样率告警</span><span class=val id=sp_cast_stat_warn></span></div>'
       + '<div class=in-stat-item id=sp_cast_stat_err_box style="grid-column:1/-1;display:none;color:var(--err)">'
       + '<span class=lbl style="color:var(--err)">错误信息</span><span class=val id=sp_cast_stat_err></span></div>'
       + '</div>')
     + '</div>';

  // ---------- 诊断（用户 2026-10-22 功能3：独立页签）----------
  // 用户原话：「导出诊断信息其实就是一个 log，最好是直接导出一个压缩包或一个整体的
  // 文件，不要用户单独找 rlplayer-diag.json / rlplayer-playback.log / launcher.log
  // 三份文件；另外这个选项放在元数据这里明显不合适，单独出来做选项卡吧，
  // 我们还在开发，报错是必不可少的。」
  // 2026-10-24（用户 261024-01 #7）：包内清单从页面正文挪进 (?) 提示，页面只留一句话 + 两个按钮。
  h += '<div class=spane data-g=diag>'
     + sgroup('导出诊断包', '一个文件，不用再到处找日志',
         '<div class="sfld wide" style="display:flex;align-items:center;gap:10px;flex-wrap:wrap">'
       + '<button class="btn pri" id=sp_diagzip>导出诊断包（.zip）</button>'
       + '<button class=btn id=sp_diagjson>只要 JSON</button>'
       + tip('包内：rlplayer-diag.json（版本 / 系统 / CPU / GPU / 端口）、rlplayer-playback.log（含 .1/.2 轮转）、launcher.log、dumps-manifest.txt、settings-summary.txt；' +
             'api_key / token / password 一律脱敏为 ***，rlplayer-llm.conf 本体不会被打进包。')
       + '<span style="font-size:11.5px;color:var(--mut2);margin-left:2px" id=sp_diagmsg></span></div>'
       + '<div class=hint>一个 zip 打包全部日志 / 系统信息 / 设置摘要（已脱敏），不用再到处找日志文件</div>')
     // 2026-10-27（261027-01 #1）：诊断页从「导出诊断包」升级成「一键反馈」。
     // 用户原话：报错要能直接发给我们 —— 不用再让他自己找日志、贴日志。
     // 表单只收「人才能提供」的四项（类型 / 标题 / 正文 / 复现步骤），其余（版本 / 系统 / CPU /
     // GPU / 设备 / 设置 / 日志尾 / 最近错误）由后端 feedback.h 自动采集并脱敏。
     // ★ 提交结果必须原样列出 uploaded / status / path / sha / local / error：
     //   无凭据时 uploaded=false 而 local 是本地文件的绝对路径 —— 不显示出来用户会以为反馈丢了。
     + sgroup('一键反馈', '填几句话直接发给我们；同时附上版本 / 系统 / 设备 / 设置 / 日志尾（已脱敏）',
         '<div class=sfld><label>类型</label><select id=sp_fbtype>'
       + opt('bug', 'BUG 报错', 'bug') + opt('idea', '功能建议', 'bug') + opt('other', '其他', 'bug')
       + '</select></div>'
       + txt('sp_fbtitle', '标题', '', '一句话说清现象或想要什么', '例如：96k 独占播放时有电流声')
       + '<div class="sfld wide"><label>详细描述' + tip('现象 / 什么时候开始 / 是否必现 / 期望是什么。会自动附上版本、系统、输出设备、当前设置与日志尾。') + '</label>'
       + '<textarea id=sp_fbbody rows=4 placeholder="现象、影响、期望" style="width:100%;padding:8px 10px;border-radius:8px;border:1px solid var(--line);background:var(--field);color:var(--txt);font-size:12px;font-family:inherit;line-height:1.5;resize:vertical"></textarea></div>'
       + '<div class="sfld wide"><label>复现步骤（可选）</label>'
       + '<textarea id=sp_fbsteps rows=3 placeholder="1. … 2. … 3. …" style="width:100%;padding:8px 10px;border-radius:8px;border:1px solid var(--line);background:var(--field);color:var(--txt);font-size:12px;font-family:inherit;line-height:1.5;resize:vertical"></textarea></div>'
       + '<div class="sfld wide" style="display:flex;align-items:center;gap:10px;flex-wrap:wrap">'
       + '<button class="btn pri" id=sp_fbsubmit>提交反馈</button>'
       + '<span style="font-size:11.5px;color:var(--mut2)" id=sp_fbmsg></span></div>'
       + '<div class=hint id=sp_fbresult style="white-space:pre-wrap;word-break:break-all"></div>')
     + '</div>';

  // ---------- 元数据补全（排在「高级」之后）----------
  // P1.5：源开关不再硬编码 —— #sp_metasrc 占位容器由 fillMetaProviders 按
  // /api/providers 动态填充（注册表驱动，后端新增数据源前端零改动）。
  // 2026-10-23 用户 261023-04 #3：三个开关 → 两个。
  //   ①「自动补全（不用 AI）」= metaon（只走公开数据源，不产生费用）；
  //   ②「AI 增强（DeepSeek）」= 原「DeepSeek 增强（无标签文件）」+「在线整理音乐资料简介」合并，
  //      保存时同时写 metallm 与 metasummary（开 = 1,1；关 = 0,0）。
  // 2026-10-24（用户 261024-01 #7/#8）：
  //   * 长说明全部收进 (?)；页面只留两个开关 + 一个「高级选项」入口；
  //   * 标签写回不再有「可选」字样/开关 —— 能力默认开启（后端本来就没有开关，永远可用）。
  var aiOn = (META.llm == 1 && META.summary == 1);
  h += '<div class=spane data-g=meta>'
     + sgroup('元数据补全', '播放时后台补全标签 / 封面 / 歌词，结果落在本地库',
         chk('sp_metaon', '自动补全（不用 AI）', META.on == 1,
             '只用公开数据源（QQ音乐 / 网易云 / iTunes / dizzylab / lrclib / Discogs）补全标签、封面、歌词；全程不调用 AI，不产生费用', true)
       + chk('sp_metallm', 'AI 增强（DeepSeek）', aiOn,
             '联网调用 DeepSeek（按次计费）：解析无标签文件、并在艺术家 / 专辑 / 单曲页整理简介并缓存（7 天有效）。' +
             '关闭后两项都停，本地与公开数据源照常工作', true)
       + '<div class="sfld wide" style="margin-top:2px">'
       + '<button class=btn id=sp_metamore style="font-size:12px" aria-expanded="false">高级选项 ▾</button>'
       + '</div>'
       // #sp_metaadv 必须跨满 .sgbody 的整行（否则会被压进单个 260px 网格单元，两列各只剩 140px）
       + '<div class="sfld wide" id=sp_metaadv style="display:none;margin-top:14px">'
       + '<div class=meta-adv-grid>'
       // 左列：数据源（逐源开关，注册表驱动渲染进 #sp_metasrc）
       + '<div class=meta-card>'
       + '<div class=mh>数据源' + tip('越靠上优先级越高；关掉的源完全不参与补全') + '</div>'
       + '<div id=sp_metasrc class=meta-src-grid>' + metaSrcHtml() + '</div>'
       + '</div>'
       // 右列：DeepSeek 接入 + 标签写回
       + '<div class=meta-card>'
       + '<div class=mh>DeepSeek 接入' + tip('只用于上面的「AI 增强」；不填也能用全部本地与公开数据源') + '</div>'
       + '<label style="display:block;font-size:11px;color:var(--mut);margin-bottom:5px">DeepSeek API Key' +
           tip('只存本机 build' + SEP + 'rlplayer-llm.conf；不进安装包，界面永不回显明文') + '</label>'
       + '<input type=password id=sp_llmkey autocomplete=off spellcheck=false placeholder="sk-…（粘贴后点保存，只显示掩码）" style="width:100%">'
       + '<div class=hint id=sp_llmmsg style="margin-top:6px">读取配置中…</div>'
       + '<div style="display:flex;gap:8px;margin-top:8px;flex-wrap:wrap">'
       + '<button class="btn pri" id=sp_llmsave>保存 Key</button>'
       + '<button class=btn id=sp_llmtest>测试连接</button>'
       + '<button class=btn id=sp_llmclear>清除 Key</button></div>'
       // 用户 261024-01 #8：默认开启，不提供开关 —— 只说明能力，两个动作按钮照旧。
       + '<div class="mh sub">标签写回<span class=hint style="font-weight:400">默认开启</span></div>'
       + '<div class=hint id=sp_wbdesc style="margin:2px 0 9px">补全到的标签 / 封面写回音频文件本身；写前校验音频数据逐位不变，原文件备份到 rlplayer-meta' + SEP + 'backups</div>'
       + '<div style="display:flex;gap:8px;margin-top:8px;flex-wrap:wrap;align-items:center">'
       + '<button class=btn id=sp_wbdry>预览写回</button>'
       + '<button class=btn id=sp_wbapply>执行写回</button>'
       + '<span id=sp_wbmsg style="font-size:11.5px;color:var(--mut2)"></span></div>'
       + '</div>'
       + '</div></div>')
     + '</div>';

  // 底部按钮（不随页签切换）
  h += '<div class=setfoot><button class="btn pri" id=sp_save>保存设置</button>'
     + '<button class=btn id=sp_reset>恢复默认</button><span class=sp></span>'
     + '<span style="font-size:11.5px;color:var(--mut2)">改动在下一次播放时生效</span></div>';

  D('setpage').innerHTML = h;

  bindPresetCard();
  loadPresetCaps(false);

  // P1.5：源开关的绑定不在上面的静态清单里 —— 重建 HTML 后逐源重新绑定
  //（/api/providers 已到则用缓存渲染+绑定；没到则等 fillMetaProviders 到达时补填）。
  renderMetaSrcRows();

  // 设置页里的下拉是复制的快捷面板 HTML，重建后浏览器会退回第一项。
  // 不同步回来，打开设置页再点保存就会把用户的选择悄悄改成第一项。
  // #5（261026-09）：sp_rep / sp_shuf 已删（见 sgroup('播放') 处的逐项核对），
  // 这里只剩还活着的镜像对。
  [['sp_filt', 'sFilt'], ['sp_filtNx', 'sFiltNx'], ['sp_shaper', 'sShap'],
   ['sp_rate', 'sRate'], ['sp_buffer', 'sBuf'],
   ['sp_loud', 'sLoud'], ['sp_isp', 'sIsp'], ['sp_2s', 's2s']].forEach(function (pair) {
    var a = D(pair[0]), b = D(pair[1]);
    if (!a || !b) return;
    if (a.type === 'checkbox') { a.checked = b.checked; return; }
    if (a.tagName === 'SELECT') {
      for (var i = 0; i < a.options.length; i++) {
        if (a.options[i].value === b.value) { a.value = b.value; return; }
      }
    } else { a.value = b.value; }
  });
  // #5（261026-09）：sp_shuf 兼容壳已删除（没有任何外部入口，也没有自检依赖它）。
  // 播放模式的唯一真相源仍是隐藏源控件 #sRep + 底栏 #bRep / 单曲页 #npRep。

  // 先把 DSD 三个控件的当前值从隐藏状态同步过来
  if (D('sp_outmode')) D('sp_outmode').value = outModeOf();
  if (D('sp_dsdrate')) D('sp_dsdrate').value = dsdRateOf();
  if (D('sp_dsdpk')) D('sp_dsdpk').value = dsdPackOf();
  if (D('sp_sdmo1')) D('sp_sdmo1').value = ADV.sdmFilt || '';
  if (D('sp_sdmoNx')) D('sp_sdmoNx').value = ADV.sdmFiltNx || '';
  applyDsdState();
  applyBackendState();   // 261024-01 #9/#11：NAA / UPnP 选中时只显示各自的目标选择器

  // 页签切换（输入 / 输出 / PCM / SDM / 高级）
  Array.prototype.forEach.call(document.querySelectorAll('#setpage .stab'), function (b) {
    b.onclick = function () {
      Array.prototype.forEach.call(document.querySelectorAll('#setpage .stab'), function (x) {
        x.classList.toggle('on', x === b);
      });
      Array.prototype.forEach.call(document.querySelectorAll('#setpage .spane'), function (p) {
        p.classList.toggle('on', p.dataset.g === b.dataset.g);
      });
      // 评审 #12：诊断页是纯操作页（导出/写回），底部「保存设置/恢复默认」在这里没有意义 → 隐藏。
      // 2026-10-24 #5：流媒体页的增删改/启用都是即时落盘，同样不需要底部保存。
      var foot = document.querySelector('#setpage .setfoot');
      if (foot) foot.style.display = (b.dataset.g === 'diag' || b.dataset.g === 'stream') ? 'none' : '';
      if (b.dataset.g === 'in') {
        loadInputDevices();
        pollInputState();
      } else if (b.dataset.g === 'out') {
        applyBackendState();
        // 只有"输出后端 = UPnP"时才需要渲染器列表（目标设备下拉在输出页里）
        if (((D('sp_backend') && D('sp_backend').value) || 'wasapi') === 'upnp') scanCastDevices(false);
      } else if (b.dataset.g === 'stream') {
        loadSrcConfig();
        scanCastDevices(false);   // 推送区块的设备列表
        pollCastState();          // 推送区块的实时状态（与单曲页按钮同源）
      }
    };
  });

  // 输出后端 / 声道 / 微调
  D('sp_backend').onchange = function () {
    ADV.backend = this.value;
    saveSettings();
    backendHint();
    applyBackendState();
    loadDeviceList();        // ASIO 列驱动，WASAPI 列端点，NAA/Null 没有本地设备
    // 261024-01 #9：切到 UPnP 时自动发现目标渲染器
    if (this.value === 'upnp') scanCastDevices(true);
  };
  // 261024-05：NAA 端点下拉绑定已删除（控件不再存在）
  // 261024-01 #9：目标渲染器与「投放」区块同一个选择（不出现两套互相打架的控制）
  if (D('sp_upnp_scan')) D('sp_upnp_scan').onclick = function () { scanCastDevices(true); };
  if (D('sp_upnp_dev')) D('sp_upnp_dev').onchange = function () { syncUpnpDev(this.value); backendHint(); };
  // 2026-10-24（261024-02 #8）：一键切到 bit-perfect 直通。
  // 用户只看到"非 bit-perfect"，不知道是哪一环：这里把判定口径对应的控件一次改到位。
  if (D('sp_bpfix')) D('sp_bpfix').onclick = function () {
    if (D('sp_backend')) { D('sp_backend').value = 'wasapi'; ADV.backend = 'wasapi'; }
    if (D('sp_outmode')) D('sp_outmode').value = 'pcm';
    if (D('sp_rate')) D('sp_rate').value = 'src';
    if (D('sRate')) D('sRate').value = 'src';
    if (D('sp_shaper')) D('sp_shaper').value = 'none';
    if (D('sShap')) D('sShap').value = 'none';
    if (D('sp_bits')) D('sp_bits').value = '0';
    // ISP 过采样峰值保护会在峰值超满幅时做预衰减（非逐位一致）→ 直通模式必须关
    if (D('sp_isp')) D('sp_isp').checked = false;
    if (D('sIsp')) D('sIsp').checked = false;
    ADV.isp = false;
    // 关掉 EQ / IR 卷积（它们都会让输出不再是逐位一致）
    if (D('sp_eq')) { D('sp_eq').value = ''; ADV.eq = ''; }
    if (D('sp_ir')) { D('sp_ir').value = ''; ADV.ir = ''; }
    // 音量：bit-perfect 必须 0 dB（不衰减），但 0 dB = 满幅输出，不能悄悄替用户拉满。
    // 必须显式确认（铁律 1：向听音设备输出可闻电平要先告知）。
    var volNow = (typeof S.vol === 'number') ? S.vol : 0;
    var volSet = false;
    if (Math.abs(volNow) > 1e-9) {
      if (window.confirm('bit-perfect 要求音量 0 dB（不衰减、输出满幅）。\n当前音量 ' + volNow.toFixed(1) +
                         ' dB。\n\n是否同时把音量设到 0 dB？\n（选"取消"则只改采样率/抖动/EQ，仍非 bit-perfect）')) {
        S.vol = 0;
        if (D('vol')) D('vol').value = '0';
        if (D('volDb')) D('volDb').textContent = '0.0 dB';
        volSet = true;
      }
    } else {
      volSet = true;
    }
    saveSettings();
    backendHint();
    if (D('sp_backendmsg')) D('sp_backendmsg').textContent = volSet
      ? '已切到 bit-perfect 直通：源采样率 + 无抖动 + 0 dB + 无 EQ/IR + 独占。下次播放生效。'
      : '已改采样率/抖动/EQ，但音量仍是 ' + volNow.toFixed(1) + ' dB（非 0）→ 仍非 bit-perfect。';
    toast(volSet ? '已切到 bit-perfect 直通（下次播放生效）' : '已改采样率/抖动/EQ；音量未动，仍非 bit-perfect');
  };
  D('sp_ch').onchange = function () {
    ADV.channels = parseInt(this.value, 10) || 0;
    saveSettings();
    // #4（261026-09）：显式选声道数后同样走 repaintTrims —— 它按 effectiveChannels() 决定行数
    //（独占 = 本设置；共享 = 设备混音声道数，设置项不生效）。
    repaintTrims();
  };
  bindTrims();
  // 2026-10-24（用户 261024-01 #12）：比特率 / 封装永远可选。
  // 在输出 PCM 时改这两个控件 = 用户明确想要 DSD ⟹ 输出模式自动联动到 DSD（并 toast 说明），
  // 不再出现"灰掉点不了"的控件。
  ['sp_outmode', 'sp_dsdrate', 'sp_dsdpk'].forEach(function (id) {
    var e = D(id);
    if (!e) return;
    e.onchange = function () {
      if (id !== 'sp_outmode' && D('sp_outmode') && D('sp_outmode').value !== 'dsd') {
        D('sp_outmode').value = 'dsd';
        toast('输出模式已切到 DSD（SDM）');
      }
      applyDsd(); saveSettings(); applyDsdState();
    };
  });
  ['sp_sdmo1', 'sp_sdmoNx'].forEach(function (id) {
    var e = D(id);
    if (e) e.onchange = function () { ADV[id === 'sp_sdmo1' ? 'sdmFilt' : 'sdmFiltNx'] = this.value; saveSettings(); };
  });
  ['sp_dsdkind', 'sp_dsdconv'].forEach(function (id) {
    var e = D(id); if (!e) return;
    e.onchange = function () {
      if (id === 'sp_dsdkind') ADV.dsdKind = this.value; else ADV.dsdConv = this.value;
      saveSettings();
    };
  });
  // 元数据补全开关（用户 261023-04 #3）：只剩两个。
  //   ① 自动补全 = metaon（公开数据源，不调 AI）；
  //   ② AI 增强 = metallm + metasummary 合并 —— 一开同开、一关同关（保存时写 1,1 / 0,0）。
  // 源开关仍在 renderMetaSrcRows 里按注册表逐源绑定（metasrc_<id>，改完即落盘）。
  var eMetaOn = D('sp_metaon');
  if (eMetaOn) eMetaOn.onchange = function () { META.on = this.checked ? 1 : 0; saveSettings(); };
  var eMetaAi = D('sp_metallm');
  if (eMetaAi) eMetaAi.onchange = function () {
    META.llm = META.summary = this.checked ? 1 : 0;
    saveSettings();
    if (S.lastCur) loadMeta(S.lastCur);   // 关掉后简介板块要立刻消失，不用刷新
  };
  // 功能4：DeepSeek API Key（保存 / 测试连接 / 清除）。输入框重建后必须为空 ——
  // 这是「永不回显明文」的最后一道：哪怕浏览器缓存了 value，重建也会清掉。
  if (D('sp_llmsave')) D('sp_llmsave').onclick = llmSave;
  if (D('sp_llmtest')) D('sp_llmtest').onclick = llmTest;
  if (D('sp_llmclear')) D('sp_llmclear').onclick = llmClear;
  if (D('sp_llmkey')) {
    D('sp_llmkey').value = '';
    D('sp_llmkey').addEventListener('keydown', function (ev) { if (ev.key === 'Enter') llmSave(); });
  }
  llmRefresh();
  // 诊断页签的两个按钮（2026-10-22 功能3：从元数据迁出 + 改为一个 ZIP）
  if (D('sp_diagzip')) D('sp_diagzip').onclick = exportDiagZip;
  if (D('sp_diagjson')) D('sp_diagjson').onclick = exportDiag;
  // 一键反馈（261027-01 #1）：提交按钮 + 回车即交（在标题框里按 Enter 不提交空表单）
  if (D('sp_fbsubmit')) D('sp_fbsubmit').onclick = fbSubmit;
  if (D('sp_fbtitle')) D('sp_fbtitle').addEventListener('keydown', function (ev) {
    if (ev.key === 'Enter') { ev.preventDefault(); fbSubmit(); }
  });
  // ReplayGain（261027-01 #2）：模式一变立刻重画"非 bit-perfect"警告，不留到保存后才提示
  if (D('sp_rgmode')) D('sp_rgmode').onchange = function () { ADV.rgmode = this.value || 'off'; rgWarn(); saveSettings(); };
  if (D('sp_rgclip')) D('sp_rgclip').onchange = function () { ADV.rgclip = this.checked ? 1 : 0; saveSettings(); };
  rgWarn();
  // 功能2：相位说明跟随 1x 滤波器与相位覆盖的选择联动
  ['sp_filt', 'sp_phase', 'sp_filtNx'].forEach(function (id) {
    var e = D(id);
    if (e) e.addEventListener('change', bindPhaseHint);
  });
  bindPhaseHint();
  // P2 标签写回：预览 / 执行按钮绑定，并同步一次在跑状态（中途刷新页面也能恢复禁用）
  if (D('sp_wbdry')) D('sp_wbdry').onclick = wbPreview;
  if (D('sp_wbapply')) D('sp_wbapply').onclick = wbApply;
  // 元数据「高级选项」折叠（2026-10-23 用户反馈「元数据栏繁杂」）：默认收起，点开才显示逐源开关/Key/写回
  if (D('sp_metamore')) D('sp_metamore').onclick = function () {
    var box = D('sp_metaadv'); if (!box) return;
    var open = (box.style.display === 'none');
    box.style.display = open ? '' : 'none';
    this.setAttribute('aria-expanded', open ? 'true' : 'false');
    this.textContent = open ? '高级选项 ▴' : '高级选项 ▾';
  };
  wbSyncBusy();
  if (D('sp_directsdm')) D('sp_directsdm').onchange = function () {
    ADV.directsdm = this.checked ? 1 : 0;
    if (this.checked && S.vol !== 0) {   // 直通要求 0 dB，否则逐位一致无从谈起
      S.vol = 0;
      if (D('vol')) D('vol').value = 0;
      api('/api/volume?db=0', '');
      toast('DirectSDM 需要 0 dB 音量，已把音量设为 0 dB');
    }
    saveSettings();
  };
  applyDsdState();
  if (D('sp_inbackend')) D('sp_inbackend').onchange = function () {
    ADV.inBackend = this.value; saveSettings();
    if (this.value === 'wasapi') loadInputDevices(); else if (D('sp_inmsg')) D('sp_inmsg').textContent = '';
  };
  if (D('sp_indev')) D('sp_indev').onchange = function () { ADV.inDev = this.value; saveSettings(); };
  if (D('sp_inch')) D('sp_inch').onchange = function () { ADV.inOffset = parseInt(this.value, 10) || 0; saveSettings(); };
  if (D('sp_inbuf')) D('sp_inbuf').onchange = function () { ADV.inBuf = parseInt(this.value, 10) || 100; saveSettings(); };
  if (D('sp_inmon')) D('sp_inmon').onchange = function () { ADV.inMonitor = this.checked ? 1 : 0; saveSettings(); };
  if (D('sp_inseconds')) D('sp_inseconds').onchange = function () { ADV.inSeconds = parseFloat(this.value) || 0; saveSettings(); };
  if (D('sp_inout')) D('sp_inout').onchange = function () { ADV.inOut = (this.value || '').trim(); saveSettings(); };
  if (D('sp_instart')) D('sp_instart').onclick = startInputCapture;
  if (D('sp_instop')) D('sp_instop').onclick = stopInputCapture;
  loadInputDevices();
  pollInputState();
  backendHint();

  // 事件
  loadDeviceList();
  // GPU 后端：把 AMD 标成本机真实状态（哪块卡、后端有没有编出来）
  api('/api/gpu').then(function (j) {
    var sg = D('sp_gpu');
    if (sg) {
      Array.prototype.forEach.call(sg.options, function (o) {
        if (o.value === 'amd') o.text = j.amd ? 'AMD（HIP）· 后端已就位' : 'AMD（HIP）· 本目录无 rtagpu.dll';
        // 2026-10-23：CUDA DLL 自 1.3.10 起随包内置 → 还分「有文件」和「本机真的能用」两种状态，
        // 否则 AMD 机器会看到"NVIDIA 后端已就位"的假象（cudaUsable=false）。
        if (o.value === 'nvidia') {
          o.text = !j.cuda ? 'NVIDIA（CUDA）· 本目录无 rtagpu_cuda.dll'
                 : (j.cudaUsable === false ? 'NVIDIA（CUDA）· 已内置，本机未检测到可用 NVIDIA 显卡（会自动回退）'
                                           : 'NVIDIA（CUDA）· 后端已就位');
        }
      });
    }
    var m = D('sp_gpumsg');
    if (m) {
      // 2026-10-24 #7：只在页面上留一句结论，DLL 明细收进 (?) 提示（动态内容写 title）。
      m.textContent = '当前状态：' + (j.available ? 'GPU 可用' : '走 CPU（未找到可执行的 GPU 后端）');
      m.title = 'rtagpu.dll=' + (j.amd ? '有' : '无') +
                '，rtagpu_cuda.dll=' + (j.cuda ? (j.cudaUsable === false ? '有（本机不可用）' : '有（可用）') : '无') +
                '，AMD HIP 运行库=' + (j.hip ? '有' : '无') +
                '；厂商偏好=' + (j.vendor || 'auto');
    }
  }).catch(function () { });
  // ASIO 驱动列表（Native DSD 需要 ASIO 后端）
  api('/api/asio').then(function (j) {
    var d = j.drivers || [];
    ASIO_DRIVERS = d;
    backendHint();
    var m = D('sp_asiomsg'); if (!m) return;
    // 2026-10-24 #7：驱动清单收进 title，页面只留结论。
    // 2026-10-24（原生 DSD 接入）：Native DSD 已接通 ASIO 原生通道，这里只交代前提。
    // 判据与后端 native_dsd_precheck() 一致（后端 ASIO / 源 DSD / 音量 0 dB / 无 PCM 处理 / 不升频）。
    m.textContent = d.length
      ? ('本机 ASIO 驱动 ' + d.length + ' 个 · Native DSD 可用（需后端 ASIO、源为 DSD、音量 0 dB、无 EQ / 卷积、采样率跟随源；驱动拒绝时回落 DoP）')
      : '本机未检测到 ASIO 驱动 · Native DSD 不可用（DoP 仍可用）';
    m.title = d.length ? d.join('、') : '';
  }).catch(function () { });
  if (ADV.inBackend === 'wasapi') loadInputDevices();
  // 流媒体页（用户 261024-01 #5）：列表 / 添加 / 编辑 / 删除 / 启用 / 测试 —— 全部即时落盘
  bindSrcPage();
  loadSrcConfig();
  D('sp_save').onclick = applySettings;
  D('sp_reset').onclick = function () {
    if (!confirm('把所有参数恢复成默认值？')) return;
    ADV = { device: '', bits: 0, taps: 0, atten: 0, transition: 0, phase: '', modcut: 0, moddith: -1, ir: '', irgpu: 0, ramp: 0, theme: S.theme, modorder: 5, modfamily: 'dsd', directsdm: 0, dsdKind: 'fir', dsdConv: 'narrow',
            backend: 'wasapi', channels: 0, naaHost: '127.0.0.1:43210',
            inBackend: 'none', inDev: '', inOffset: 0, inBuf: 100, inMonitor: 0, inSeconds: 0, inOut: '',
            rgmode: 'off', rgclip: 1 };
    // 元数据两键的默认：自动补全开；AI 增强关（联网计费项默认关，避免「恢复默认」反而产生费用）。
    // 源开关随注册表一并全开。
    META = { on: 1, llm: 0, summary: 0 };
    (META_PROVIDERS || []).forEach(function (p) { META_SRC[p.id] = 1; });
    D('sRate').value = 'max'; D('sFiltNx').value = ''; D('sShap').value = 'auto';
    D('sBuf').value = 100; D('sDSD').value = 0; D('sIsp').checked = true; D('s2s').checked = true;
    saveSettings(); renderSettingsPage(); toast('已恢复默认');
  };
  D('sp_theme').onchange = function () { setTheme(this.value); };
  D('sp_addroot').onclick = function () {
    var d = (D('sp_root').value || '').trim();
    if (!d) { toast('请填目录'); return; }
    D('sp_root').value = '';
    api('/api/lib/roots', 'add=' + encodeURIComponent(d)).then(function (j) {
      if (!j.ok) { toast(j.error || '添加失败'); return; }
      if (!S.roots) S.roots = [];
      if (S.roots.indexOf(d) < 0) S.roots.push(d);
      drawRoots();
      toast('已添加，正在扫描'); loadLibStats(); api('/api/lib/scan', '');
    });
  };
  D('sp_rescan').onclick = function () { api('/api/lib/scan', 'full=1').then(function () { toast('开始重新扫描'); loadLibStats(); }); };
  if (D('sp_scanthreads')) {
    D('sp_scanthreads').onchange = function () {
      ADV.scanThreads = parseInt(this.value, 10) || 0;
      saveSettings();
    };
  }
  // DLNA 发现按钮已移到「流媒体」页（用户 261024-01 #6），绑定在 bindSrcPage() 里做。
  drawRoots();
  // EQ 页签的内容在 renderSettingsPage 里一并渲染；这里统一做一次事件绑定 + 初始 OPRA 状态。
  initEqViewLogic();
  initCastControls();
  scanCastDevices(false);
  pollCastState();
}
/* ---------------- EQ 与房间校正（261023-03 第6/7条） ---------------- */
var OPRA_INDEX = null;
var OPRA_PRESETS = [];
var _opraLoading = false;
var OPRA_CUR_PRODUCT = '';   // 当前请求预设的型号（连点竞态守卫）

// 2026-10-23 DEBUG：旧实现用 setInterval(100ms) 轮询等 OPRA_INDEX，请求失败时 OPRA_INDEX 永远为 null
// → 定时器永不清理（每敲一个字派生一个，后台 10Hz 空转）。改成 promise 缓存 + 失败后可重试。
var _opraPromise = null;
function loadOpraIndex(cb) {
  if (OPRA_INDEX) { if (cb) cb(); return; }
  if (!_opraPromise) {
    _opraPromise = api('/api/opra/index').then(function (j) {
      if (j && j.ok && Array.isArray(j.items)) OPRA_INDEX = j.items;
    }).catch(function () {
      // 失败不缓存 promise（下次进页面可重试）；本次直接让调用方走“未加载”分支
    }).then(function () {
      _opraPromise = null;
      if (cb) cb();
    });
    return;
  }
  _opraPromise.then(function () { if (cb) cb(); });
}

/* ---------------- 调音目标曲线示意（261023-03 第7条） ----------------
   内联 SVG 手绘近似：横轴 20Hz→20kHz（对数近似），纵轴相对 dB。
   只用作「这条目标曲线大概长什么样」的示意，不是测量数据；不引外部图片。
   颜色只用 CSS 变量（--line / --acc / --mut2），跟随深色/浅色主题。 */
var TARGET_CURVES = {
  harman_oe: { label: 'Harman 头戴目标：低频温和提升、中高频平顺',
    pts: [[0,6],[0.08,6],[0.15,5],[0.22,2.6],[0.30,0.8],[0.38,0],[0.47,0.5],[0.54,2.0],[0.60,2.6],[0.66,1.5],[0.72,0.3],[0.80,-0.4],[0.90,-0.2],[1.0,0.9]] },
  harman_iem: { label: 'Harman 入耳目标：低频更厚、3kHz 耳道增益',
    pts: [[0,9],[0.07,9],[0.13,7.2],[0.20,3.4],[0.27,0.8],[0.34,0],[0.44,0.4],[0.50,1.8],[0.55,6.0],[0.59,7.2],[0.64,2.6],[0.70,0.4],[0.80,-1.0],[0.90,-0.6],[1.0,1.2]] },
  autoeq: { label: 'AutoEQ 校准：按实测目标平直化，小幅低频架',
    pts: [[0,3.8],[0.10,3.8],[0.18,2.8],[0.26,1.0],[0.34,0],[0.46,0.5],[0.53,1.8],[0.60,0.6],[0.70,-0.3],[0.82,0],[1.0,0.6]] },
  studio: { label: '录音室中性：全频段尽量平直',
    pts: [[0,0],[0.12,-0.3],[0.24,-0.6],[0.36,-0.2],[0.50,0.2],[0.62,0.5],[0.74,0],[0.86,-0.3],[1.0,0]] },
  consumer: { label: '消费级曲线：低频与极高频微抬',
    pts: [[0,4.5],[0.12,4.5],[0.20,3.0],[0.30,0.8],[0.40,0],[0.52,0.8],[0.58,2.2],[0.65,0.8],[0.75,0.2],[0.85,0.8],[1.0,2.6]] },
  diffuse: { label: '扩散场目标：声场开阔、高频细节多',
    pts: [[0,0],[0.10,-0.3],[0.20,-0.6],[0.30,-0.6],[0.40,-0.3],[0.50,0.2],[0.60,1.0],[0.70,2.0],[0.80,2.8],[0.90,3.3],[1.0,3.6]] },
  freefield: { label: '自由场目标：接近消音室直达声',
    pts: [[0,0],[0.15,-0.2],[0.30,-0.5],[0.45,-0.4],[0.55,-0.1],[0.65,0.3],[0.75,0.9],[0.85,1.6],[1.0,2.4]] },
  senselab: { label: 'SenseLab 目标：高频峰谷精细补偿',
    pts: [[0,0.5],[0.15,0.2],[0.28,-0.2],[0.40,-0.3],[0.52,0.2],[0.60,1.2],[0.68,0.4],[0.80,-0.2],[1.0,0.4]] },
  rtings: { label: 'Rtings 参考目标：平直均衡',
    pts: [[0,2.5],[0.12,2.5],[0.22,1.4],[0.32,0.2],[0.45,0],[0.55,0.6],[0.62,1.2],[0.72,0],[0.85,-0.4],[1.0,0.3]] },
  optimum: { label: 'Optimum HiFi：低频克制、中频通透',
    pts: [[0,2.0],[0.14,2.0],[0.24,1.0],[0.34,0],[0.48,0.3],[0.56,1.0],[0.66,0.2],[0.80,-0.4],[1.0,0]] },
  neutral: { label: 'IEF 中性：平直无染',
    pts: [[0,0],[0.12,0],[0.24,-0.2],[0.36,-0.2],[0.50,0],[0.58,0.8],[0.66,0.2],[0.80,-0.4],[1.0,0]] }
};

// 判据与 getTargetCurveDescInfo 各分支保持一致（只用于挑 SVG 示意）
function curveKindOfDetails(details) {
  var d = (details || '').toLowerCase();
  if (d.indexOf('harman') >= 0) return (d.indexOf('in-ear') >= 0 || d.indexOf('iem') >= 0) ? 'harman_iem' : 'harman_oe';
  if (d.indexOf('soundguys_studio') >= 0) return 'studio';
  if (d.indexOf('soundguys_consumer') >= 0) return 'consumer';
  if (d.indexOf('diffuse') >= 0 || d.indexOf('hrtf') >= 0) return 'diffuse';
  if (d.indexOf('free_field') >= 0) return 'freefield';
  if (d.indexOf('senselab') >= 0) return 'senselab';
  if (d.indexOf('rtings') >= 0) return 'rtings';
  if (d.indexOf('optimum') >= 0 || d.indexOf('hifi') >= 0) return 'optimum';
  if (d.indexOf('neutral') >= 0 || d.indexOf('ief') >= 0) return 'neutral';
  return 'autoeq';
}

function targetCurveSvg(kind, H) {
  var c = TARGET_CURVES[kind] || TARGET_CURVES.autoeq;
  var W = 260, HH = H || 58, PAD = 6, MAXDB = 9, MINDB = -2;
  function px(x) { return PAD + x * (W - 2 * PAD); }
  function py(db) { return PAD + (MAXDB - db) / (MAXDB - MINDB) * (HH - 2 * PAD); }
  var pts = c.pts.map(function (p) { return px(p[0]).toFixed(1) + ',' + py(p[1]).toFixed(1); }).join(' ');
  var s = '<svg viewBox="0 0 ' + W + ' ' + HH + '" style="display:block;width:100%;height:auto" role="img" aria-label="' + esc(c.label) + '">';
  // 0dB 基准线 + 1kHz / 10kHz 竖向参考线
  s += '<line x1="' + PAD + '" y1="' + py(0).toFixed(1) + '" x2="' + (W - PAD) + '" y2="' + py(0).toFixed(1) + '" stroke="var(--line)" stroke-width="1" stroke-dasharray="3 3"/>';
  s += '<line x1="' + px(0.52).toFixed(1) + '" y1="' + PAD + '" x2="' + px(0.52).toFixed(1) + '" y2="' + (HH - PAD) + '" stroke="var(--line)" stroke-width="1" stroke-dasharray="2 4" opacity=".7"/>';
  s += '<line x1="' + px(0.80).toFixed(1) + '" y1="' + PAD + '" x2="' + px(0.80).toFixed(1) + '" y2="' + (HH - PAD) + '" stroke="var(--line)" stroke-width="1" stroke-dasharray="2 4" opacity=".7"/>';
  s += '<polyline points="' + pts + '" fill="none" stroke="var(--acc)" stroke-width="1.8" stroke-linejoin="round" stroke-linecap="round"/>';
  s += '<circle cx="' + px(c.pts[0][0]).toFixed(1) + '" cy="' + py(c.pts[0][1]).toFixed(1) + '" r="2" fill="var(--acc)"/>';
  s += '<circle cx="' + px(c.pts[c.pts.length - 1][0]).toFixed(1) + '" cy="' + py(c.pts[c.pts.length - 1][1]).toFixed(1) + '" r="2" fill="var(--acc)"/>';
  s += '<text x="' + PAD + '" y="' + (HH - 1) + '" font-size="7" fill="var(--mut2)">20Hz</text>';
  s += '<text x="' + (px(0.52) - 8).toFixed(1) + '" y="' + (HH - 1) + '" font-size="7" fill="var(--mut2)">1k</text>';
  s += '<text x="' + (W - PAD - 16) + '" y="' + (HH - 1) + '" font-size="7" fill="var(--mut2)">20k</text>';
  s += '</svg>';
  return s;
}

// 对外接口：在原有「中文解释」上补一个 kind（用于选 SVG 示意），不改变原判据
function getTargetCurveDesc(details, author) {
  var c = getTargetCurveDescInfo(details, author);
  c.kind = curveKindOfDetails(details);
  return c;
}

function getTargetCurveDescInfo(details, author) {
  var d = (details || '').toLowerCase();
  var a = (author || '').toLowerCase();
  if (d.indexOf('harman') >= 0 && (d.indexOf('in-ear') >= 0 || d.indexOf('iem') >= 0)) {
    return {
      name: '哈曼入耳目标曲线 (Harman In-Ear)',
      scene: '针对入耳式耳道封闭增强低频动态与人声清晰度，适合现代流行、人声与节奏感强的音乐。',
      tech: 'Harman In-Ear Target 补偿了耳道封闭效应，适度提升 100Hz 以下低频与 3kHz 附近耳甲腔增益，提供接近高品质音箱的听感。'
    };
  }
  if (d.indexOf('harman') >= 0) {
    return {
      name: '哈曼头戴目标曲线 (Harman Over-Ear)',
      scene: '符合大众听音盲测偏好，低频温暖厚实、中高频平衡耐听，杂食百搭，适合绝大多数现代流行乐与影视原声。',
      tech: 'Harman Target 基于 Harman 实验室长期双盲听音实验，低频有约 4~6dB 适度平滑提升，高频平顺无刺激刺耳感。'
    };
  }
  if (d.indexOf('soundguys_studio') >= 0) {
    return {
      name: 'SoundGuys 录音室中性曲线',
      scene: '严格平直的工作室参考曲线，适合录音棚监听、音频剪辑与追求原汁原味还原的听众。',
      tech: 'SoundGuys Studio 目标注重全频段线性度，消除耳机本身声学谐振，为混音制作提供真实基准。'
    };
  }
  if (d.indexOf('soundguys_consumer') >= 0) {
    return {
      name: 'SoundGuys 大众消费曲线',
      scene: '兼顾清晰度与氛围感，微调低频弹性与高频空气感，适合日常通勤随身聆听与流媒体影视。',
      tech: '面向普通消费者的均衡曲线，在平直基础上微抬极低频与极高频，听感更具感染力。'
    };
  }
  if (d.indexOf('diffuse') >= 0 || d.indexOf('hrtf_5128') >= 0) {
    return {
      name: '扩散场目标曲线 (Diffuse Field / HRTF)',
      scene: '模拟无指向多反射声学空间，空间定位感与高频解析力极强，特别适合大编制古典交响乐与现场录音。',
      tech: 'Diffuse Field 曲线基于人工头在混响室中测量的自由声场平均响应，声场开阔通透，高频细节丰富。'
    };
  }
  if (d.indexOf('free_field') >= 0) {
    return {
      name: '自由场目标曲线 (Free Field)',
      scene: '模拟消音室正前方向直达声，干脆纯净，适合需要清晰方向判断的专业监听。',
      tech: 'Free Field 曲线反映消音室内扬声器正前方的直达声听觉响应。'
    };
  }
  if (d.indexOf('senselab') >= 0) {
    return {
      name: 'SenseLab 声学实验室目标',
      scene: '注重高保真声学平直度与相位一致性，适合乐器真实音色鉴赏与原声发烧录音。',
      tech: 'SenseLab 方案基于精细声学补偿，重点修正耳机单元在高频段的群延迟与频响峰谷。'
    };
  }
  if (d.indexOf('rtings') >= 0) {
    return {
      name: 'Rtings 参考目标曲线',
      scene: 'Rtings 实验室测量的平直均衡曲线，适合影视与游戏声效还原。',
      tech: 'Rtings 综合目标结合声学生理模型，旨在使耳机频响趋近于平直扬声器室内频响。'
    };
  }
  if (d.indexOf('optimum') >= 0 || d.indexOf('hifi') >= 0) {
    return {
      name: '最佳高保真目标曲线 (Optimum HiFi)',
      scene: '低频平顺不轰鸣、中频通透无染色，适合古典乐、爵士乐与纯乐器发烧录音。',
      tech: 'Optimum HiFi 强调极低失真与线性动态，不对低频做过多主观提升，保持纯正声底。'
    };
  }
  if (d.indexOf('neutral') >= 0 || d.indexOf('ief') >= 0) {
    return {
      name: 'IEF 中性监听目标 (Neutral)',
      scene: '监听级纯中性调音，无主观音染，适合专业音频工作者与追求纯粹原声的用户。',
      tech: 'IEF Neutral 以平直无染为核心，低频不外加低音架，高频顺滑延伸。'
    };
  }
  var authorName = author || 'AutoEQ';
  return {
    name: details || (authorName + ' 专业校准预设'),
    scene: '由 ' + authorName + ' 依据测量数据生成的校准滤波，平直化固有频响缺陷。',
    tech: '基于 AutoEq 优化算法与专业人工耳测量数据计算所得的双二阶 IIR 滤波器组。'
  };
}

// 某个设置页签当前是否可见（设置页开着 + 选中该页签）。用于丢弃切走后的异步回调。
// #6 拆分后有两个相关页签：eq（PEQ + 耳机 EQ）、cal（声音校准）。
function settingTabActive(g) {
  var p = document.querySelector('#setpage .spane[data-g=' + g + ']');
  return !!(p && p.classList.contains('on'));
}
function eqTabActive() { return settingTabActive('eq'); }
/* EQ 与房间校正（用户 261023-04 #2：从左侧主导航移进设置页的「EQ」页签）
   #6（2026-10-27 用户 261026-09）拆分后，这个函数只留「① 20 段 PEQ ② 耳机 EQ（OPRA）」；
「③ 房间校正 ④ 校正文件（参数 EQ / IR）」搬到 calSettingsHtml()（声音校准页签）。
   改用设置页统一的分组卡片（sgroup），不再用原来的独立大卡片，保证页签内视觉与其它页一致。 */
function eqSettingsHtml() {
  var h = '';
  // 0. 20 段参数均衡 PEQ（261024-02 #6）—— 新增，放在 EQ 页最上面
  h += sgroup('参数均衡 PEQ（20 段 · HIFI）',
      'RBJ 双二阶 · 20 段 ISO 1/3 倍频程中心频率（20Hz~20kHz）· 采样率相关重建 · 参数热更新交叉淡入',
      // 261024-04 #3：用户问「旁路是什么意思？当前是不是默认就启动 PEQ？只有勾选旁路才不生效？」——
      // 组标题下常驻一行状态（生效 / 已旁路），复选框文案直接写明勾选的含义，
      // 并说清「默认生效，但 20 段全 0 dB = 不改变声音」（后端 src/dsp/peq.h: bypass 默认 false，20 段默认 0 dB）。
      '<div class="sfld wide"><div class="peqstat" id=peqStat><span class=tag>状态</span>' +
        '<span id=peqStatTxt>PEQ 当前：读取中…</span></div>' +
        '<div class=hint id=peqByPassHint>旁路 = 绕过 PEQ（20 段全部不参与计算，信号直通）。默认<strong>生效</strong>；' +
        '出厂 20 段全为 0 dB、前级 0 dB，所以即使生效也不改变声音（等同直通）。</div></div>'
    + '<div class="sfld wide" style="display:flex;align-items:center;gap:14px;flex-wrap:wrap">'
    + '<label class=chk style="margin:0" title="勾选 = PEQ 不生效（直通）；不勾选 = PEQ 生效"><input type=checkbox id=peqBypass> 旁路（勾选 = PEQ 不生效，直通）</label>'
    + '<label style="font-size:12px;color:var(--mut2)">前级增益 <input type=number id=peqPreamp step=0.1 min=-24 max=24 value=0 style="width:74px;height:26px"> dB</label>'
    // 261024-03 #5：按曲线峰值一键设前级，防削波（只在前端算，DSP 不动）
    + '<button class="btn" id=peqAutoPre type=button title="把前级设为 -ceil(当前曲线峰值增益)，给 EQ 提升留出余量，避免数字削波">自动前级（防削波）</button>'
    + '<label style="font-size:12px;color:var(--mut2)">预设 <select id=peqPreset style="min-width:160px;height:26px"></select></label>'
    + '<button class="btn" id=peqLoad type=button>加载</button>'
    + '<button class="btn" id=peqSave type=button>保存为预设…</button>'
    + '<button class="btn" id=peqDelete type=button>删除预设</button>'
    + '<button class="btn" id=peqReset type=button>重置 Flat</button>'
    + '<span id=peqMsg style="font-size:11.5px;color:var(--mut2)"></span>'
    + '</div>'
    + '<div class="sfld wide"><div class="peqcv" id=peqCurve></div></div>'
    + '<div class="sfld wide"><div class=peqwrap>'
    + '<div id=peqRows class=peqrows></div>'
    + '</div>'
    + '<div style="font-size:11px;color:var(--mut2);margin-top:5px">增益 ±12 dB（步进 0.1）· Q 0.1~20 · 频率 10 Hz~20 kHz。'
    + '曲线上 <b>拖拽节点</b>：水平=频率（对数）、垂直=增益；<b>Shift+拖 / 滚轮</b>=改 Q；<b>双击节点</b>=该段复位（0 dB / Q 1.41）。'
    + '选中行后 <b>↑↓</b>=增益 0.1 dB、<b>←→</b>=频率 ±1%（Shift 加速）。修改即时生效（热更新，交叉淡入防爆音），并自动保存到 rlplayer-peq.txt。</div></div>');

  // 1. 耳机 EQ（OPRA 校准预设）
  h += opraSectionHtml();

  return h;
}
/* ---------------- EQ 页签里的耳机 EQ（OPRA）----------------
   单独成函数的原因：30 行的搜索框 + 型号卡片 + 预设卡片，混在 eqSettingsHtml 里会把
   「PEQ / 耳机 EQ」和「房子校准」两种东西搅在一起 —— 那正是 #6 要拆开的。 */
function opraSectionHtml() {
  var h = '';
  h += sgroup('耳机 EQ（OPRA 校准预设）', '收录 6,230 款耳机专业校准滤波 · CC BY-SA 4.0',
      '<div class="sfld wide" style="display:flex;align-items:center;gap:10px">'
    + '<input id=sp_opra_search type=text placeholder="搜索耳机品牌或型号（如 Sennheiser / HD 600 / AirPods / Sony / XM5…）" autocomplete=off style="flex:1;min-width:0;width:auto;height:34px;padding:0 12px;background:var(--field);border:1px solid var(--line);border-radius:8px;font-size:12.5px;color:var(--txt);box-sizing:border-box">'
    + '<span class="chip" id=sp_opra_copychip title="复制 OPRA 项目官网链接" style="cursor:pointer;display:inline-flex;align-items:center;font-size:11.5px;flex:none">'
    + '<svg width=12 height=12 viewBox="0 0 24 24" fill=none stroke=currentColor stroke-width=2 style="margin-right:4px"><rect x="9" y="9" width="13" height="13" rx="2" ry="2"/><path d="M5 15H4a2 2 0 0 1-2-2V4a2 2 0 0 1 2-2h9a2 2 0 0 1 2 2v1"/></svg>https://opra.roon.app/</span>'
    + '</div>'
    + '<div class="sfld wide" id=eqSearchGuide style="padding:2px 0 0">'
    + '<div style="display:flex;flex-wrap:wrap;gap:8px" id=eqBrandChips>'
    + '<button class="btn brand-chip" data-brand="Sennheiser">森海塞尔 Sennheiser</button>'
    + '<button class="btn brand-chip" data-brand="Sony">索尼 Sony</button>'
    + '<button class="btn brand-chip" data-brand="Apple">苹果 Apple</button>'
    + '<button class="btn brand-chip" data-brand="Beyerdynamic">拜雅 Beyerdynamic</button>'
    + '<button class="btn brand-chip" data-brand="Audio-Technica">铁三角 Audio-Technica</button>'
    + '<button class="btn brand-chip" data-brand="AKG">爱科技 AKG</button>'
    + '<button class="btn brand-chip" data-brand="Hifiman">海菲曼 Hifiman</button>'
    + '<button class="btn brand-chip" data-brand="Focal">劲浪 Focal</button>'
    + '</div></div>'
    + '<div class="sfld wide" id=sp_opra_searchhint style="font-size:12px;color:var(--mut2);display:none"></div>'
    + '<div class="sfld wide"><div id=eqModelCards style="display:grid;grid-template-columns:repeat(auto-fill,minmax(220px,1fr));gap:10px"></div></div>'
    + '<div class="sfld wide" id=eqPresetsArea style="display:none;margin-top:16px;padding-top:14px;border-top:1px dashed var(--line)">'
    + '<div style="display:flex;align-items:center;justify-content:space-between;margin-bottom:12px;gap:10px;flex-wrap:wrap">'
    + '<h3 id=eqSelectedModelTitle style="font-size:14px;margin:0;font-weight:600">选择调音目标预设</h3>'
    + '<span id=sp_opra_msg style="font-size:12px;color:var(--mut2)"></span>'
    + '</div>'
    + '<div id=eqPresetCards style="display:grid;grid-template-columns:repeat(auto-fill,minmax(280px,1fr));gap:12px"></div>'
    + '<div id=eqTargetCurveInfo style="margin-top:16px;padding:14px 16px;background:var(--field);border:1px solid var(--line);border-radius:8px;font-size:12.5px;color:var(--txt2);line-height:1.6;display:none"></div>'
    + '</div>');
  return h;
}
/* ---------------- 声音校准（#6：2026-10-27 用户 261026-09 从 EQ 页拆出的新页签） ----------------
   用户原话：「设置-EQ 里面的内容比较臃肿了，请拆分成 EQ 和声音校准两个不同的设置栏。
   EQ 栏位保留 PEQ 和耳机 EQ，声音校准则放置房间校正、REW 导入、IR 导入等」。
   ⟹ EQ 页签 = 参数均衡 PEQ（20 段）+ 耳机 EQ（OPRA）：都是"我把耳机调成什么样"。
     声音校准页签 = 房间校正（测量向导）+ 校正文件（REW/AutoEq 参数 EQ 文本 + IR 脉冲响应 WAV）：
        都是"用测量数据去校准这套回放系统"。
   命名理由（也是核对结论）：EQ 页的 PEQ/OPRA 是**手调**，校准页的全是**测量/外部文件导入**；
   原来那两个文件分组核实后**确实都是校正文件**（REW 一种软件能同时导出这两种，见下面的组标题）。 */
function calSettingsHtml() {
  var h = '';

  // 1.5 房间校正（测量向导）—— 2026-10-26（261026-01 #2 用户：「房间 EQ 功能请上架 EQ 那一栏的 UI」）
  // 位置：**紧跟 PEQ 之后**（第 2 张卡）——用户是在 EQ 页找这个功能，放在页尾等于没上架。
  // 2026-10-26 晚（261026-06）：后端 /api/roomcorr/{layouts,status,measure,cancel,download} 已接上，
  //   测量走真实 WASAPI（共享播放 + 采集，src/dsp/room_transport_wasapi.h），
  //   所以「预览」角标与「不会真的出声」的免责声明全部移除 —— 留着就是在骗用户。
  //   铁律 1 不变：向导最后一步必须弹授权横幅（设备名 / 时长 / 电平 / 会盖过系统声音）并要用户点确认，
  //   默认电平 −20 dBFS（MeasurementSession 缺省），不做任何放大。
  h += '<div id=roomCorrCard>' + sgroup('房间校正（测量向导）', 'Farina 对数扫频 → 反卷积 → 心理声学平滑 → 目标曲线 → 最小相位校正 FIR · 多声道 2.0~7.1.4',
      '<div class="sfld wide"><div class="in-stat-grid" style="grid-template-columns:repeat(auto-fit,minmax(250px,1fr))">'
    + '<div class=in-stat-item><span class=lbl>算法版本</span><span class="val sm">C1 算法 + C2 测量编排</span>' +
      '<div class=rcsub>room_correction.h · room_measurement.h · room_transport_wasapi.h</div></div>'
    + '<div class=in-stat-item><span class=lbl>支持声道布局</span><span class="val sm">2.0 ~ 7.1.4（6 种）</span>' +
      '<div class=rcsub>2.0 / 2.1 / 5.1 / 7.1 / 5.1.2 / 7.1.4 · 带 .1 的 LFE 分频 80 Hz</div></div>'
    + '<div class=in-stat-item><span class=lbl>当前状态</span><span class="val sm" id=rcStatus>未测量</span>' +
      '<div class=rcsub id=rcStatusSub>点「开始向导」跑一次测量</div></div>'
    + '</div></div>'
    + '<div class="sfld wide" style="display:flex;align-items:center;gap:12px;flex-wrap:wrap">'
    + '<button class="btn pri" id=bRoomWiz type=button>开始向导</button>'
    + '<button class="btn" id=bRoomDl type=button style="display:none">下载校正集</button>'
    + '<span class=rcnote id=rcHint>分 7 步走完一次测量（选布局 → 摆麦 → 测各声道 → 质检 → 生成校正集）。</span>'
    + '</div>'
    + '<div class="sfld wide"><div class=rcnote>测量结果（每声道校正 FIR，32-bit float）保存到 <code>data\\roomcorr\\</code>；'
    + '应用后由播放链路的 FIR 卷积执行（与下面「校正文件 → 脉冲响应 IR」共用同一个卷积器）。<br>'
    + '测量会<strong>真的出声</strong>：每个声道一段对数扫频，默认电平 −20 dBFS，开始前会让你确认设备与时长。</div></div>'
    ) + '</div>';
  // 2. 校正文件（参数 EQ 文本 + 脉冲响应 IR）—— #6 核对结论：
  //    用户问「最下面的两个是不是都是 REW 的校正文件？」——**是同一类东西，但不只有 REW**：
  //    ① 参数 EQ 文本（Filter 1: ON PK Fc … Gain … Q …）REW / AutoEq / 任何导出这种格式的软件都能给，
  //       后端按 Filter 行生成双二阶滤波器组（逐段参量 EQ）；
  //    ② 脉冲响应 IR（WAV）同样常由 REW 导出（File → Export → Export impulse response as WAV），
  //       也可以是任何测量/仿真工具给的房间或耳机 IR，后端做 FIR 卷积。
  //    两者都是"外部测量 → 校准文件 → 叠进播放链路"，所以合并成**一个组、两个子块**，
  //    不再像以前那样让用户以为它们是两件互不相干的设置。
  h += sgroup('校正文件（参数 EQ 文本 · 脉冲响应 IR）',
      '都是外部测量软件（REW / AutoEq / 任何导出这几种格式的工具）产出的校准文件；留空 = 不做对应校正',
      '<div class="sfld wide"><label>参数 EQ 文件（REW / AutoEq 文本）' +
        tip('每行形如 Filter 1: ON PK Fc 1000 Hz Gain -3.0 dB Q 1.41；按 Filter 行生成双二阶滤波器组') + '</label>'
    + '<input type=text id=sp_eq value="' + esc(ADV.eq || '') + '" placeholder="D:\\Music\\headphone-eq.txt"></div>'
    + '<div class="sfld wide" style="margin-top:4px"><label>脉冲响应 IR 文件（WAV）' +
        tip('留空 = 不做卷积；支持立体声 / 多声道 FIR 房间校正') + '</label>'
    + '<input type=text id=sp_ir value="' + esc(ADV.ir || '') + '" placeholder="D:\\Music\\room.wav"></div>'
    + chk('sp_irgpu', 'IR 卷积走 GPU 硬件加速（需要 rtagpu.dll 就位）', ADV.irgpu == 1,
          '房间 / 耳机 FIR 卷积交给 GPU；没有可用后端时自动退回 CPU')
    // 评审 #11：「保存 EQ 与房间校正设置」按钮与底部全局「保存设置」重复 → 不再提供第二个主操作
    //（sp_eq / sp_ir / sp_irgpu 随 saveSettings 一起提交）。
    );

  return h;
}
/* ---------------- 房间校正向导（预览）2026-10-26 261026-01 #2 ----------------
   为什么是「预览」：算法与测量编排已在 src/dsp 落地并通过单测，但**没有 HTTP 端点**
   （/api/roomcorr/* 不存在）⟹ 前端无法真的开始测量。按铁律 4「如实记录未解决问题」，
   这张向导只做流程演示：第 1 步第一句就写明「需要后端接线，当前版本为预览」，
   按钮带「预览」角标，点「完成」不改任何播放状态、不发任何请求。 */
// seconds/levelDb 必须有初值：面板直接用 RCW.seconds / RCW.levelDb 渲染，
// 漏了就会显示 undefined（实机截图抓到的 bug）。
var RCW = { step: 1, total: 7, layout: '2.0', seconds: 10, levelDb: -20, target: 'flat', done: false };
// 与 room_correction.h::room_layouts() 一一对应（2.0/2.1/5.1/7.1/5.1.2/7.1.4）
var RC_LAYOUTS = ['2.0', '2.1', '5.1', '7.1', '5.1.2', '7.1.4'];
var RC_STEPS = [
  { t: '开始之前：会出声，先看清楚',
    b: '这个向导会把你的房间<b>真的测一遍</b>：每个声道播放一段对数扫频（20 Hz ~ 22 kHz），同时录音，' +
       '反卷积分出每个声道的房间脉冲响应，再据此生成校正 FIR。<br>' +
       '<span class=warnline>扫频是要听得见的声音（不出声就测不到房间）。默认电平 −20 dBFS；' +
       '最后一步会先把「播放设备 / 采集设备 / 时长 / 电平」摊开让你确认，确认后才开始。' +
       '测量期间请关掉其它会出声的程序，否则会混进录音。</span>' },
  { t: '第 1 步：选声道布局',
    b: '先在下面选一套布局。带 <b>.1</b> 的布局会做 80 Hz 超低分频，并在 40~120 Hz 做超低与主声道的时延对齐；' +
       '带 <b>.2/.4</b> 的天空声道会单独测。选定的布局决定接下来要测几个声道（2.0 两个，7.1.4 十二个）。' },
  { t: '第 2 步：摆好测量麦克风',
    b: '把麦克风放在<b>听音位</b>（你平时头的位置），高度与耳朵齐平，指向正前方。' +
       '测试音会逐个声道播放，所以你不需要移动麦克风 —— 这是「一麦多位」的做法：每个声道单独测一次，最后拼成整套房间响应。' +
       '房间里尽量安静、关掉风扇与空调；麦克风离反射面太近会让低频测量偏高。' },
  { t: '第 3 步：播放扫频（会出声）',
    b: '每个声道会播放一次对数扫频（20 Hz ~ 22 kHz），电平 <b>−20 dBFS</b>（可在最后一步改成 −30 dBFS）。' +
       '<span class=warnline>这是要听得见的测试音。共享模式播放不会独占设备，但系统里其它声音会混进录音，' +
       '所以测量期间请静音音乐/视频/通知音。</span>' },
  { t: '第 4 步：采集与反卷积',
    b: '扫频播放的同时录制，并且<b>多录 1 秒</b>：用来装下房间混响尾和扫频泄漏尾，' +
       '否则圆周反卷积会把尾巴绕回脉冲响应的头部，低频就会测歪。录到的信号与参考扫频做频域正则化反卷积，得到每个声道的房间脉冲响应。' },
  { t: '第 5 步：质检（不合格就重测）',
    b: '每个声道都会算信噪比、直达声到达时间，并检查超低有没有反接（40~120 Hz 带限互相关的极性）。' +
       '信噪比太低通常是环境太吵或电平太小；超低反接只要把音箱线正负换过来再测。质检不合格的声道会单独提示重测，不影响已测好的。' },
  { t: '第 6 步：生成校正集并保存',
    b: '把每个声道的响应做<b>变分数倍频程平滑</b>（低频 1/6 oct → 高频 1/3 oct），减去目标曲线，' +
       '限制最大提升量，转成最小相位，再截断加窗成 8192 taps 的校正 FIR。' +
       '校正集保存到 <code>data\\roomcorr\\correction-<布局>.json</code>，可以直接下载。' +
       '<span class=warnline>生成后还需要在下方「校正文件 → 脉冲响应 IR」里把它挂进播放链路才会生效 —— 这一步本轮仍是手动的。</span>' }
];
function rcWizEnsure() {
  var box = D('roomWiz');
  if (box) return box;
  box = document.createElement('div');
  box.id = 'roomWiz';
  box.innerHTML =
    '<div class="rw-card" role=dialog aria-modal=true aria-labelledby=rwTitle>' +
      '<div class=rw-head><span>房间校正向导</span><span class=sp></span>' +
        '<button class=rw-x id=rcClose type=button title="关闭">&#10005;</button></div>' +
      '<div class=rw-dots id=rcDots></div>' +
      '<div class=rw-step id=rcStep></div>' +
      '<div class=rw-title id=rwTitle></div>' +
      '<div class=rw-body id=rcBody></div>' +
      '<div class=rw-foot><button class="btn" id=rcPrev type=button>上一步</button>' +
        '<button class="btn pri" id=rcNext type=button>下一步</button><span class=sp></span>' +
        '<span class=rcnote id=rcFootNote>测量会出声（默认 −20 dBFS）</span></div>' +
    '</div>';
  document.body.appendChild(box);
  D('rcClose').onclick = rcWizClose;
  D('rcPrev').onclick = function () { if (RCW.step > 1) { RCW.step--; rcWizRender(); } };
  D('rcNext').onclick = function () {
    if (RCW.step < RCW.total) { RCW.step++; rcWizRender(); } else rcWizClose();
  };
  box.onclick = function (ev) { if (ev.target === box) rcWizClose(); };
  // ESC 关向导；捕获阶段拦住，避免它同时被全局 ESC（返回上一级视图）吃掉
  document.addEventListener('keydown', function (ev) {
    if (!D('roomWiz') || !D('roomWiz').classList.contains('on')) return;
    if (ev.key === 'Escape') { ev.preventDefault(); ev.stopPropagation(); rcWizClose(); }
  }, true);
  return box;
}
function rcWizRender() {
  var box = rcWizEnsure();
  if (!box) return;
  var s = RC_STEPS[RCW.step - 1] || { t: '', b: '' };
  var dots = '';
  for (var i = 1; i <= RCW.total; i++) dots += '<i class="rw-dot' + (i <= RCW.step ? ' on' : '') + '"></i>';
  D('rcDots').innerHTML = dots;
  D('rcStep').textContent = '第 ' + RCW.step + ' / ' + RCW.total + ' 步';
  D('rwTitle').textContent = s.t;
  var body = s.b;
  if (RCW.step === 2) {
    body += '<div style="margin-top:12px;display:flex;align-items:center;gap:10px;flex-wrap:wrap">' +
            '<label style="font-size:12px;color:var(--mut2)">声道布局' +
            '<select id=rcLayout style="margin-left:8px">' +
            RC_LAYOUTS.map(function (o) {
              return '<option' + (o === RCW.layout ? ' selected' : '') + '>' + o + '</option>';
            }).join('') + '</select></label>' +
            '<span class=rcnote>当前选择：<b id=rcLayoutNow>' + RCW.layout + '</b></span></div>';
  }
  D('rcBody').innerHTML = body;
  if (D('rcLayout')) {
    D('rcLayout').onchange = function () {
      RCW.layout = this.value;
      if (D('rcLayoutNow')) D('rcLayoutNow').textContent = RCW.layout;
    };
  }
  // 最后一步：真实测量面板（铁律 1 的四项摊开 + confirm）
  if (RCW.step >= RCW.total) {
    body += '<div style="margin-top:14px;padding:12px 14px;border:1px solid var(--line);border-radius:8px;background:var(--field)">' +
      '<div style="font-size:13px;font-weight:600;margin-bottom:8px">确认测量参数</div>' +
      '<div class=rcsub style="line-height:1.9">' +
        '播放设备：<select id=rcPlaySel style="max-width:340px;vertical-align:middle"></select>（共享模式，不独占）<br>' +
        '采集设备：<select id=rcCapSel style="max-width:340px;vertical-align:middle"></select><br>' +
        '声道布局：<b>' + RCW.layout + '</b> · 时长：<b><span id=rcSecsVal>' + RCW.seconds + '</span> 秒/声道</b>' +
        ' · 电平：<b><span id=rcLevelVal>' + RCW.levelDb + '</span> dBFS</b><br>' +
        '<span class=warnline>扫频会盖过正在播放的音乐；测量期间请静音其它程序。</span>' +
      '</div>' +
      '<div style="margin-top:10px;display:flex;align-items:center;gap:14px;flex-wrap:wrap">' +
        '<label style="font-size:12px">时长 <input id=rcSecs type=range min=0.5 max=30 step=0.5 value=' + RCW.seconds + ' style="width:120px;vertical-align:middle"></label>' +
        '<label style="font-size:12px">电平 <input id=rcLevel type=range min=-60 max=-20 step=1 value=' + RCW.levelDb + ' style="width:120px;vertical-align:middle"></label>' +
        '<label style="font-size:12px;display:flex;align-items:center;gap:6px">' +
          '<input type=checkbox id=rcAck> 我知道会出声，开始测量</label>' +
        '<button class="btn pri" id=rcGo type=button disabled>开始测量</button>' +
        '<button class="btn" id=rcStop type=button style="display:none">中止</button>' +
      '</div>' +
      '<div id=rcProg style="margin-top:10px;font-size:12px;color:var(--mut2)"></div>' +
    '</div>';
  }
  D('rcBody').innerHTML = body;
  if (D('rcLayout')) { D('rcLayout').onchange = function () { RCW.layout = this.value; if (D('rcLayoutNow')) D('rcLayoutNow').textContent = RCW.layout; }; }
  if (RCW.step >= RCW.total) rcBindMeasurePanel();
  D('rcPrev').disabled = RCW.step <= 1;
  D('rcNext').textContent = (RCW.step >= RCW.total) ? '关闭' : '下一步';
}

/* ---- 房间测量面板：设备探测 / 启动 / 进度轮询 / 中止（261026-06）---- */
var RC_POLL = 0;
function rcBindMeasurePanel() {
  var secs = D('rcSecs'), level = D('rcLevel'), ack = D('rcAck'), go = D('rcGo');
  if (secs) secs.oninput = function () { RCW.seconds = parseFloat(this.value); if (D('rcSecsVal')) D('rcSecsVal').textContent = RCW.seconds; };
  if (level) level.oninput = function () { RCW.levelDb = parseFloat(this.value); if (D('rcLevelVal')) D('rcLevelVal').textContent = RCW.levelDb; };
  if (ack) ack.onchange = function () { if (go) go.disabled = !this.checked; };
  if (go) go.onclick = rcStartMeasure;
  if (D('rcStop')) D('rcStop').onclick = function () {
    api('/api/roomcorr/cancel').then(function () { rcProg('已请求中止…'); });
  };
  // 设备下拉（261026-06）：默认端点被独占时（DEVICE_IN_USE）用户能在这里换一个，
  // 否则只会看到「打开播放端点失败」而无处可改。
  api('/api/devices').then(function (j) {
    var sel = D('rcPlaySel'); if (!sel) return;
    var arr = (j && j.devices) || [];
    var opt = '<option value="">系统默认输出</option>';
    for (var i = 0; i < arr.length; i++) {
      var d = arr[i];
      opt += '<option value="' + esc(d.id || '') + '"' + (d['default'] ? ' selected' : '') + '>' +
             esc((d.name || d.id) + (d['default'] ? '（默认）' : '')) + '</option>';
    }
    sel.innerHTML = opt;
    RCW.playDev = sel.value;
    sel.onchange = function () { RCW.playDev = this.value; };
  }).catch(function () {});
  api('/api/input/devices').then(function (j) {
    var sel = D('rcCapSel'); if (!sel) return;
    var arr = (j && j.devices) || [];
    var opt = '<option value="">系统默认采集</option>';
    for (var i = 0; i < arr.length; i++) {
      var d = arr[i];
      opt += '<option value="' + esc(d.id || '') + '"' + (d.isDefault ? ' selected' : '') + '>' +
             esc((d.name || d.id) + (d.isDefault ? '（默认）' : '')) + '</option>';
    }
    sel.innerHTML = opt;
    RCW.capDev = sel.value;
    sel.onchange = function () { RCW.capDev = this.value; };
  }).catch(function () {});
}
function rcProg(s) { if (D('rcProg')) D('rcProg').innerHTML = s; }
function rcStartMeasure() {
  if (!D('rcAck') || !D('rcAck').checked) return;
  var q = 'layout=' + encodeURIComponent(RCW.layout) +
          '&seconds=' + encodeURIComponent(RCW.seconds) +
          '&levelDb=' + encodeURIComponent(RCW.levelDb) +
          '&target=' + encodeURIComponent(RCW.target || 'flat') +
          '&confirm=1' +
          (RCW.playDev ? '&playDev=' + encodeURIComponent(RCW.playDev) : '') +
          (RCW.capDev ? '&capDev=' + encodeURIComponent(RCW.capDev) : '');
  rcProg('正在启动…');
  api('/api/roomcorr/measure?' + q).then(function (j) {
    if (!j || !j.ok) { rcProg('<span style=color:#e66>' + ((j && j.error) || '启动失败') + '</span>'); return; }
    if (D('rcGo')) D('rcGo').disabled = true;
    if (D('rcStop')) D('rcStop').style.display = '';
    rcPollStart();
  }).catch(function (e) { rcProg('<span style=color:#e66>请求失败：' + e + '</span>'); });
}
function rcPollStart() {
  rcPollStop();
  RC_POLL = setInterval(function () {
    api('/api/roomcorr/status').then(function (j) {
      if (!j) return;
      if (j.running) {
        rcProg('第 ' + j.step + ' / ' + j.totalSteps + ' 步' + (j.stepName ? '：' + j.stepName : '') +
               (j.channel ? ' · 声道 ' + j.channel : '') + ' …');
      } else {
        rcPollStop();
        if (D('rcStop')) D('rcStop').style.display = 'none';
        if (D('rcGo')) D('rcGo').disabled = false;
        if (j.hasCorrectionSet) {
          rcProg('✅ 测量完成：' + (j.layout || RCW.layout) + ' · ' + j.irTaps + ' taps @ ' + j.irFs + ' Hz' +
                 (j.warnings && j.warnings.length ? '<br>⚠️ ' + j.warnings.join('；') : ''));
          var dl = D('bRoomDl'); if (dl) dl.style.display = '';
          RCW.done = true;
          rcStatusRefresh();
        } else {
          rcProg('<span style=color:#e66>' + (j.lastError || '测量未生成校正集') + '</span>');
        }
      }
    }).catch(function () {});
  }, 1200);
}
function rcPollStop() { if (RC_POLL) { clearInterval(RC_POLL); RC_POLL = 0; } }
function rcStatusRefresh() {
  api('/api/roomcorr/status').then(function (j) {
    if (!j) return;
    var el = D('rcStatus'), sub = D('rcStatusSub');
    if (el) el.textContent = j.hasCorrectionSet ? ('已测量 · ' + (j.layout || '')) : '未测量';
    if (sub) sub.textContent = j.hasCorrectionSet
      ? (j.irTaps + ' taps @ ' + j.irFs + ' Hz' + (j.file ? ' · ' + j.file.split('\\').pop() : ''))
      : '点「开始向导」跑一次测量';
    if (D('bRoomDl')) D('bRoomDl').style.display = j.hasCorrectionSet ? '' : 'none';
  }).catch(function () {});
}
function rcWizOpen() {
  var box = rcWizEnsure();
  if (!box) return;
  RCW.step = 1;
  box.classList.add('on');
  rcWizRender();
}
function rcWizClose() {
  var box = D('roomWiz');
  if (box) box.classList.remove('on');
}
function initRoomCorr() {
  if (D('bRoomWiz')) D('bRoomWiz').onclick = rcWizOpen;
  // 下载校正集（261026-06）：直接给下载链接，浏览器原生下载，不走 fetch
  if (D('bRoomDl')) D('bRoomDl').onclick = function () {
    var lg = (RCW.layout || '2.0');
    window.location.href = '/api/roomcorr/download?layout=' + encodeURIComponent(lg);
  };
  // 进页面先刷一次真实状态（有校正集就显示，没有就提示未测量）
  rcStatusRefresh();
}

/* ---------------- 20 段参数均衡 PEQ（261024-02 #6） ---------------- */
var PEQ_STATE = null;
var PEQ_TIMER = 0;

function peqRowsEl() { return document.querySelectorAll('#peqRows .peqr'); }
// 频率输入框接受友好写法：'20 Hz' / '1.02 kHz' / '1020' / '1.02k'
function peqParseFreq(s) {
  var t = String(s == null ? '' : s).toLowerCase().replace(/\s+/g, '');
  var m = /^([0-9]*\.?[0-9]+)(k|khz|hz)?$/.exec(t);
  if (!m) return NaN;
  var f = parseFloat(m[1]);
  if (m[2] === 'k' || m[2] === 'khz') f *= 1000;
  return f;
}
// 友好显示：<1 kHz 用 Hz（20 Hz），>=1 kHz 用 kHz（1.02 kHz / 10.0 kHz）
function peqFreqText(f) {
  f = +f || 0;
  if (f >= 10000) return (f / 1000).toFixed(1) + ' kHz';
  if (f >= 1000) return (f / 1000).toFixed(2) + ' kHz';
  return Math.round(f) + ' Hz';
}
function peqClampFreq(f) { return Math.max(10, Math.min(20000, +f || 1000)); }
function peqRowBand(tr) {
  var f = tr.querySelector('.peqF'), g = tr.querySelector('.peqG'), q = tr.querySelector('.peqQ');
  var t = tr.querySelector('.peqT'), on = tr.querySelector('.peqOn');
  var fv = peqParseFreq(f.value);
  if (!isFinite(fv) || fv <= 0) fv = 1000;
  return {
    i: +tr.dataset.i, type: t.value, freq: peqClampFreq(fv),
    gain: Math.max(-12, Math.min(12, parseFloat(g.value) || 0)),
    q: Math.max(0.1, Math.min(20, parseFloat(q.value) || 1.41)),
    on: on.checked
  };
}
function peqCollectBands() {
  var out = [];
  Array.prototype.forEach.call(peqRowsEl(), function (tr) { out.push(peqRowBand(tr)); });
  return out;
}
function peqBandsParam() {
  return peqCollectBands().map(function (b) {
    return b.i + ',' + b.type + ',' + b.freq + ',' + b.gain + ',' + b.q + (b.on ? '' : ',off');
  }).join(';');
}
// 频响曲线：后端 /api/peq 直接给 [频率, dB] 点（与 C++ 设计公式同源，前端不抄第二份）
// 261024-03 #5 HIFI 化：曲线上叠 20 个频段节点（可拖/滚轮/双击），并做增量更新
//（增量很关键：拖拽时 peqApply 每次回来都重建 SVG 会丢掉 pointer capture，拖到一半就断）
var PEQ_VIEW = null;      // 曲线坐标映射（拖拽换算要用）
var PEQ_DRAG = null;      // {i, mode:'g'|'q', y0, q0}
var PEQ_LASTCLICK = null; // {i, t}：自己判双击（不依赖宿主是否派发 dblclick）
function peqFreqX(f) {
  var v = PEQ_VIEW; if (!v) return 0;
  return v.PADL + (Math.log(Math.max(20, Math.min(20000, f)) / 20) / Math.log(1000)) * (v.W - v.PADL - v.PADR);
}
function peqGainY(db) {
  var v = PEQ_VIEW; if (!v) return 0;
  return v.PADT + (v.maxDb - db) / (v.maxDb - v.minDb) * (v.H - v.PADT - v.PADB);
}
function peqCurveFreqAt(x) {
  var v = PEQ_VIEW; if (!v) return 1000;
  var t = (x - v.PADL) / (v.W - v.PADL - v.PADR);
  return peqClampFreq(20 * Math.pow(1000, Math.max(0, Math.min(1, t))));
}
function peqCurveGainAt(y) {
  var v = PEQ_VIEW; if (!v) return 0;
  var t = (y - v.PADT) / (v.H - v.PADT - v.PADB);
  return Math.max(-12, Math.min(12, v.maxDb - Math.max(0, Math.min(1, t)) * (v.maxDb - v.minDb)));
}
/* #3（2026-10-27 用户 261026-09）：「PEQ 里面的波形和下面的表格，需要根据软件宽度自动匹配宽度」。
   以前这里 W 写死 640、CSS 又给 .peqcv 加 max-width:720px ⟹ 1600px 的窗口右边一整片空白。
   现在 W 由容器的真实客户区宽度算出（不是靠 CSS 把 640 拉伸 —— 那样横轴刻度会跟着变形），
   并在宽度变化时用 ResizeObserver 重算（见下文 peqObserveWidth）。 */
var PEQ_PTS = null;    // 最近一次曲线点（宽度变了要按新宽度重画）
var PEQ_W = 0;         // 当前逻辑宽度（= 容器内容宽度），用于判断"值得重画"
var PEQ_RO = null;     // ResizeObserver（只观察 #peqCurve 一个节点）
function peqContentWidth(box) {
  // clientWidth 含 padding；.peqcv 的左右 padding 各 10px ⟹ content = clientWidth - 20
  var w = (box && box.clientWidth) ? (box.clientWidth - 20) : 0;
  return Math.max(320, Math.floor(w || 0) || 640);
}
var PEQ_RO_BOX = null;
function peqObserveWidth() {
  var box = D('peqCurve'); if (!box) return;
  // 设置页每次重渲染都会重建 #peqCurve 节点：观察者要跟着换目标，否则观察的是一个已脱离文档的旧节点
  if (PEQ_RO && PEQ_RO_BOX === box) return;
  if (PEQ_RO) { try { PEQ_RO.disconnect(); } catch (e) {} PEQ_RO = null; }
  PEQ_RO_BOX = box;
  if (!window.ResizeObserver) return;               // 老引擎：靠下面的 window resize 兜底
  PEQ_RO = new ResizeObserver(function () {
    var w = peqContentWidth(box);
    if (Math.abs(w - PEQ_W) < 4) return;            // 高度由 viewBox 比例决定 ⟹ 不会自激循环
    if (PEQ_PTS) peqDrawCurve(PEQ_PTS);
  });
  PEQ_RO.observe(box);
}
// 兜底：宿主的窗口缩放（WebView2 里 ResizeObserver 已经能覆盖，双保险不冲突）
window.addEventListener('resize', function () { if (PEQ_PTS) peqOnResize(); });
function peqOnResize() {
  var box = D('peqCurve'); if (!box) return;
  if (Math.abs(peqContentWidth(box) - PEQ_W) >= 4 && PEQ_PTS) peqDrawCurve(PEQ_PTS);
}
function peqDrawCurve(points) {
  var box = D('peqCurve'); if (!box) return;
  if (!points || !points.length) {
    box.innerHTML = '<div style="font-size:11.5px;color:var(--mut2);padding:12px 0">频响曲线（等待数据…）</div>';
    PEQ_VIEW = null; PEQ_PTS = null;
    return;
  }
  PEQ_PTS = points;
  var W = peqContentWidth(box);
  // 高度跟着宽度走：宽窗不至于把曲线压成一条细带（1600px 时约 210px 高）
  var H = Math.round(Math.min(220, Math.max(120, W * 0.19)));
  var PADL = 36, PADR = 10, PADT = 10, PADB = 18;
  PEQ_W = W;
  var maxDb = 6, minDb = -6;
  points.forEach(function (p) { maxDb = Math.max(maxDb, p[1]); minDb = Math.min(minDb, p[1]); });
  maxDb = Math.ceil(maxDb / 3) * 3; minDb = Math.floor(minDb / 3) * 3;
  PEQ_VIEW = { W: W, H: H, PADL: PADL, PADR: PADR, PADT: PADT, PADB: PADB, maxDb: maxDb, minDb: minDb };

  var g = '';
  for (var d = Math.ceil(minDb / 6) * 6; d <= maxDb; d += 6) {
    g += '<line x1="' + PADL + '" y1="' + peqGainY(d).toFixed(1) + '" x2="' + (W - PADR) + '" y2="' + peqGainY(d).toFixed(1) +
         '" stroke="var(--line)" stroke-width="1" stroke-dasharray="' + (d === 0 ? '4 3' : '2 4') + '" opacity="' + (d === 0 ? '0.9' : '0.5') + '"/>';
    g += '<text x="2" y="' + (peqGainY(d) + 3).toFixed(1) + '" font-size="9" fill="var(--mut2)">' + d + 'dB</text>';
  }
  [[20, '20'], [100, '100'], [1000, '1k'], [10000, '10k'], [20000, '20k']].forEach(function (fk) {
    var x = peqFreqX(fk[0]);
    g += '<line x1="' + x.toFixed(1) + '" y1="' + PADT + '" x2="' + x.toFixed(1) + '" y2="' + (H - PADB) + '" stroke="var(--line)" stroke-width="1" stroke-dasharray="2 4" opacity=".45"/>';
    g += '<text x="' + (x - 8).toFixed(1) + '" y="' + (H - 4) + '" font-size="9" fill="var(--mut2)">' + fk[1] + '</text>';
  });
  var pts = points.map(function (p) { return peqFreqX(p[0]).toFixed(1) + ',' + peqGainY(p[1]).toFixed(1); }).join(' ');

  var svg = box.querySelector('svg.peqsvg');
  if (!svg) {
    box.innerHTML =
      '<svg class=peqsvg viewBox="0 0 ' + W + ' ' + H + '" preserveAspectRatio="none" data-w="' + W + '">' +
        '<g class=peqgrid></g>' +
        '<polyline class=peqline fill=none stroke="var(--acc)" stroke-width=1.8 stroke-linejoin=round stroke-linecap=round points=""/>' +
        '<g class=peqnodes></g>' +
      '</svg>' +
      '<div class=peqlegend><span><b>拖拽节点</b> 水平=频率 · 垂直=增益</span><span><b>Shift+拖 / 滚轮</b> 改 Q</span>' +
      '<span><b>双击</b> 该段复位</span><span><b>↑↓←→</b> 微调（Shift 加速）</span></div>' +
      '<div class=peqtip id=peqTip></div>';
    svg = box.querySelector('svg.peqsvg');
    peqBindCurve();
    peqObserveWidth();
  }
  // 宽度变了要同步 viewBox（否则 grid 坐标与比例对不上）
  svg.setAttribute('viewBox', '0 0 ' + W + ' ' + H);
  svg.setAttribute('data-w', String(W));
  svg.querySelector('.peqgrid').innerHTML = g;
  svg.querySelector('.peqline').setAttribute('points', pts);
  peqSyncNodes();
}
/* ---------------- PEQ：行渲染 / 选中 / 曲线节点交互（261024-03 #5） ---------------- */
function peqSelectRow(tr) {
  Array.prototype.forEach.call(peqRowsEl(), function (x) { x.classList.toggle('sel', x === tr); });
}
function peqRowTipText(b) {
  return '<b>' + peqFreqText(b.freq) + '</b> · ' + (b.gain >= 0 ? '+' : '') + b.gain.toFixed(1) +
         ' dB · Q ' + b.q.toFixed(2) + ' · ' + b.type + (b.on ? '' : '（已关）');
}
// tooltip 定位：有事件用鼠标位置；没有（滚轮/键盘）就用节点自身位置
function peqTipAt(html, ev, node) {
  var tip = D('peqTip'), box = D('peqCurve');
  if (!tip || !box) return;
  var br = box.getBoundingClientRect();
  var cx, cy;
  if (ev && typeof ev.clientX === 'number') { cx = ev.clientX; cy = ev.clientY; }
  else if (node) { var nr = node.getBoundingClientRect(); cx = nr.left + nr.width / 2; cy = nr.top + nr.height / 2; }
  else { cx = br.left + br.width / 2; cy = br.top + 20; }
  tip.innerHTML = html;
  tip.classList.add('on');
  var x = Math.max(4, Math.min(cx - br.left + 14, br.width - (tip.offsetWidth || 90) - 4));
  var y = Math.max(4, Math.min(cy - br.top + 8, br.height - (tip.offsetHeight || 24) - 4));
  tip.style.left = x + 'px'; tip.style.top = y + 'px';
}
function peqTipHide() { var t = D('peqTip'); if (t) t.classList.remove('on'); }
function peqResetBand(i) {
  var tr = document.querySelector('#peqRows .peqr[data-i="' + i + '"]'); if (!tr) return;
  tr.querySelector('.peqG').value = '0.0';
  tr.querySelector('.peqQ').value = '1.41';
  peqSyncNodes();
  peqScheduleApply(100);
  peqMsg('第 ' + i + ' 段已复位（0.0 dB / Q 1.41）');
}
function peqSvgPoint(ev) {
  var box = D('peqCurve');
  var svg = box && box.querySelector('svg.peqsvg');
  if (!svg || !PEQ_VIEW) return null;
  var r = svg.getBoundingClientRect();
  if (!r.width || !r.height) return null;
  return { x: (ev.clientX - r.left) / r.width * PEQ_VIEW.W,
           y: (ev.clientY - r.top) / r.height * PEQ_VIEW.H };
}
function peqNodeRow(i) { return document.querySelector('#peqRows .peqr[data-i="' + i + '"]'); }
function peqBindCurve() {
  var box = D('peqCurve'); if (!box) return;
  var svg = box.querySelector('svg.peqsvg'); if (!svg) return;
  var pick = function (ev) {
    var t = ev.target;
    while (t && t !== svg) { if (t.dataset && t.dataset.i) return t; t = t.parentNode; }
    return null;
  };
  svg.addEventListener('pointermove', function (ev) {
    if (PEQ_DRAG) return;
    var node = pick(ev);
    if (node) {
      var tr = peqNodeRow(node.dataset.i);
      if (tr) peqTipAt(peqRowTipText(peqRowBand(tr)), ev);
      return;
    }
    var p = peqSvgPoint(ev); if (!p) return;
    var f = peqCurveFreqAt(p.x), pts = (PEQ_STATE && PEQ_STATE.points) || [], db = null;
    for (var i = 0; i < pts.length; i++) { if (pts[i][0] >= f) { db = pts[i][1]; break; } }
    if (db === null && pts.length) db = pts[pts.length - 1][1];
    peqTipAt('<b>' + peqFreqText(f) + '</b>' + (db === null ? '' : ' · 曲线 ' + (db >= 0 ? '+' : '') + db.toFixed(1) + ' dB'), ev);
  });
  svg.addEventListener('pointerleave', function () { if (!PEQ_DRAG) peqTipHide(); });
  svg.addEventListener('dblclick', function (ev) {
    var node = pick(ev); if (!node) return;
    ev.preventDefault();
    peqResetBand(+node.dataset.i);
  });
  svg.addEventListener('wheel', function (ev) {
    var node = pick(ev); if (!node) return;
    ev.preventDefault();
    var tr = peqNodeRow(node.dataset.i); if (!tr) return;
    var qEl = tr.querySelector('.peqQ'), v = parseFloat(qEl.value) || 1.41;
    v = Math.max(0.1, Math.min(20, v * Math.pow(1.15, ev.deltaY > 0 ? 1 : -1)));
    qEl.value = v.toFixed(2);
    peqSyncNodes();
    peqTipAt(peqRowTipText(peqRowBand(tr)), null, node);
    peqScheduleApply(100);
  }, { passive: false });
  svg.addEventListener('pointerdown', function (ev) {
    var node = pick(ev); if (!node) return;
    ev.preventDefault();
    var i = +node.dataset.i, tr = peqNodeRow(i); if (!tr) return;
    peqSelectRow(tr);
    // 双击 = 该段复位。自己在 pointerdown 里判（350ms 内同一节点），
    // 因为 pointerdown 里 preventDefault + setPointerCapture 会让宿主的 dblclick 时序不可靠。
    var now = Date.now();
    if (PEQ_LASTCLICK && PEQ_LASTCLICK.i === i && now - PEQ_LASTCLICK.t < 350) {
      PEQ_LASTCLICK = null;
      peqResetBand(i);
      PEQ_DRAG = null;
      return;
    }
    PEQ_LASTCLICK = { i: i, t: now };
    PEQ_DRAG = { i: i, mode: ev.shiftKey ? 'q' : 'g', y0: ev.clientY,
                 q0: parseFloat(tr.querySelector('.peqQ').value) || 1.41 };
    try { svg.setPointerCapture(ev.pointerId); } catch (e) { }
    var c = svg.querySelector('.peqnode[data-i="' + i + '"]'); if (c) c.classList.add('on');
  });
  svg.addEventListener('pointermove', function (ev) {
    if (!PEQ_DRAG) return;
    var tr = peqNodeRow(PEQ_DRAG.i); if (!tr) return;
    if (PEQ_DRAG.mode === 'q') {
      var qv = Math.max(0.1, Math.min(20, PEQ_DRAG.q0 * Math.pow(2, (PEQ_DRAG.y0 - ev.clientY) / 60)));
      tr.querySelector('.peqQ').value = qv.toFixed(2);
    } else {
      var p = peqSvgPoint(ev); if (!p) return;
      tr.querySelector('.peqF').value = peqFreqText(peqCurveFreqAt(p.x));
      tr.querySelector('.peqG').value = peqCurveGainAt(p.y).toFixed(1);
    }
    peqSyncNodes();
    var node = svg.querySelector('.peqnode[data-i="' + PEQ_DRAG.i + '"]');
    peqTipAt(peqRowTipText(peqRowBand(tr)), ev, node);
    peqScheduleApply(100);           // 拖拽 100ms 节流
  });
  var endDrag = function () {
    if (!PEQ_DRAG) return;
    var i = PEQ_DRAG.i;
    PEQ_DRAG = null;
    var c = svg.querySelector('.peqnode[data-i="' + i + '"]'); if (c) c.classList.remove('on');
    peqTipHide();
    clearTimeout(PEQ_TIMER); peqApply();   // 松手立刻落一次，不等节流窗口
  };
  svg.addEventListener('pointerup', endDrag);
  svg.addEventListener('pointercancel', endDrag);
}
// 把 20 个节点同步到当前行值（拖拽/键盘/滚轮后立即反映，不等后端）
function peqSyncNodes() {
  var box = D('peqCurve');
  var svg = box && box.querySelector('svg.peqsvg');
  if (!svg || !PEQ_VIEW) return;
  var host = svg.querySelector('.peqnodes'); if (!host) return;
  var bands = peqCollectBands();
  if (!bands.length && PEQ_STATE && PEQ_STATE.bands) bands = PEQ_STATE.bands;
  if (host.querySelectorAll('circle.peqhit').length !== bands.length) {
    var h = '';
    bands.forEach(function (b, i) {
      h += '<circle class=peqhit data-i=' + (i + 1) + ' r=13></circle>'
         + '<circle class="peqnode' + (b.on === false ? ' off' : '') + '" data-i=' + (i + 1) + ' r=4.5></circle>';
    });
    host.innerHTML = h;
  }
  Array.prototype.forEach.call(host.querySelectorAll('circle.peqnode'), function (c) {
    var i = +c.dataset.i - 1, b = bands[i];
    if (!b) return;
    var cx = peqFreqX(b.freq).toFixed(1), cy = peqGainY(b.gain).toFixed(1);
    c.setAttribute('cx', cx); c.setAttribute('cy', cy);
    c.classList.toggle('off', b.on === false);
    var hit = host.querySelector('circle.peqhit[data-i="' + (i + 1) + '"]');
    if (hit) { hit.setAttribute('cx', cx); hit.setAttribute('cy', cy); }
  });
}
function peqRenderRows(j) {
  var box = D('peqRows'); if (!box) return;
  // 表头与数据行同处 #peqRows：同一个内容宽度 ⟹ 同一套列轨道（这是"对齐"的根）
  var h = '<div class=peqhd><span>段</span><span class=num>频率</span><span class=num>增益 dB</span>'
        + '<span class=num>Q</span><span>类型</span><span class=c>启用</span></div>';
  (j.bands || []).forEach(function (b, i) {
    var sel = '<select class=peqT>'
      + '<option value=PK' + (b.type === 'PK' ? ' selected' : '') + '>峰值 PK</option>'
      + '<option value=LS' + (b.type === 'LS' ? ' selected' : '') + '>低架 LS</option>'
      + '<option value=HS' + (b.type === 'HS' ? ' selected' : '') + '>高架 HS</option></select>';
    h += '<div class=peqr data-i=' + (i + 1) + ' tabindex=0>'
       + '<span class=peqn>' + (i + 1 < 10 ? '0' : '') + (i + 1) + '</span>'
       + '<span class=peqc><input class=peqF type=text spellcheck=false value="' + esc(peqFreqText(b.freq)) + '"></span>'
       + '<span class="peqc num"><input class=peqG type=number step=0.1 min=-12 max=12 value="' + (+b.gain).toFixed(1) + '"></span>'
       + '<span class="peqc num"><input class=peqQ type=number step=0.01 min=0.1 max=20 value="' + (+b.q).toFixed(2) + '"></span>'
       + '<span class=peqc>' + sel + '</span>'
       + '<span class="peqc c"><input class=peqOn type=checkbox' + (b.on ? ' checked' : '') + '></span>'
       + '</div>';
  });
  box.innerHTML = h;
  Array.prototype.forEach.call(box.querySelectorAll('.peqr'), function (tr) {
    var inputs = tr.querySelectorAll('input,select');
    Array.prototype.forEach.call(inputs, function (el) {
      el.addEventListener('change', function () { peqScheduleApply(); });
      el.addEventListener('input', function () { peqScheduleApply(); });
      el.addEventListener('focus', function () { peqSelectRow(tr); });
    });
    tr.addEventListener('click', function () { peqSelectRow(tr); });
    tr.addEventListener('focus', function () { peqSelectRow(tr); });
    // 键盘：↑↓ 增益 0.1 dB（Shift 1 dB）/ ←→ 频率 ±1%（Shift ±5%）
    tr.addEventListener('keydown', function (ev) {
      var k = ev.key, g = tr.querySelector('.peqG'), f = tr.querySelector('.peqF');
      var fast = ev.shiftKey;
      if (k === 'ArrowUp' || k === 'ArrowDown') {
        ev.preventDefault();
        var d = (k === 'ArrowUp' ? 1 : -1) * (fast ? 1 : 0.1);
        g.value = (Math.max(-12, Math.min(12, (parseFloat(g.value) || 0) + d))).toFixed(1);
      } else if (k === 'ArrowLeft' || k === 'ArrowRight') {
        if (ev.target !== tr && ev.target.tagName === 'INPUT' && ev.target.type === 'text') return; // 文本输入框里让光标移动
        ev.preventDefault();
        var mul = (k === 'ArrowRight' ? 1 : -1) * (fast ? 0.05 : 0.01);
        var fv = peqParseFreq(f.value) || 1000;
        f.value = peqFreqText(peqClampFreq(fv * (1 + mul)));
      } else return;
      peqSyncNodes();
      peqScheduleApply(100);
    });
  });
  peqSyncNodes();
}
// 自动前级（防削波）：把前级设为 -ceil(曲线峰值)。曲线峰值优先取后端点集（架式/叠加后的真实峰值），
// 没有点集时退回"各段增益最大值"。
function peqAutoPreamp() {
  var maxGain = 0;
  var pts = (PEQ_STATE && PEQ_STATE.points) || [];
  if (pts.length) {
    pts.forEach(function (p) { if (p[1] > maxGain) maxGain = p[1]; });
  } else {
    peqCollectBands().forEach(function (b) { if (b.on && b.gain > maxGain) maxGain = b.gain; });
  }
  var pre = -Math.max(0, Math.ceil(maxGain));
  var inp = D('peqPreamp');
  if (inp) inp.value = pre;
  peqApply();
  peqMsg('自动前级：曲线峰值 +' + maxGain.toFixed(1) + ' dB → 前级 ' + pre + ' dB（留出 ' + Math.abs(pre).toFixed(0) + ' dB 余量）');
  toast('自动前级 ' + pre + ' dB（峰值 +' + maxGain.toFixed(1) + ' dB）');
}
function peqRender(j) {
  PEQ_STATE = j;
  if (D('peqBypass')) D('peqBypass').checked = !!j.bypass;
  if (D('peqPreamp')) D('peqPreamp').value = j.preamp;
  peqStatusRender(j);   // 261024-04 #3：状态行跟着后端的真值走
  var sel = D('peqPreset');
  if (sel) {
    var cur = j.name || 'Flat';
    var h = '';
    (j.presets || []).forEach(function (n) {
      h += '<option value="' + esc(n) + '"' + (n === cur ? ' selected' : '') + '>' + esc(n) + '</option>';
    });
    sel.innerHTML = h;
  }
  peqRenderRows(j);
  peqDrawCurve(j.points);
}
function peqMsg(t) { var e = D('peqMsg'); if (e) e.textContent = t || ''; }
// 261024-04 #3：把「旁路 / 生效」的**语义**写成一行常驻状态，而不是只留一个 checkbox 让用户猜。
// 判据：勾选/取消勾选后这一行文字必须立刻跟着变（不依赖后端往返）。
// 文案分三态：
//   ① 已旁路         → PEQ 完全不算，信号直通；
//   ② 生效但全 0 dB  → 严格说"生效"，但 20 段全 0 dB + 前级 0 dB ⟹ 不改变声音（下载默认就是这一档）；
//   ③ 生效且有非 0 dB 段 → 输出确实被 EQ 改变。
function peqStatusRender(j) {
  var el = D('peqStat'), tx = D('peqStatTxt');
  if (!el || !tx) return;
  j = j || PEQ_STATE || {};
  var bands = j.bands || [];
  var active = 0, total = 0;
  for (var i = 0; i < bands.length; ++i) {
    total++;
    if (bands[i].on && Math.abs(+bands[i].gain || 0) > 0.001) active++;
  }
  var pre = (typeof j.preamp === 'number') ? j.preamp : 0;
  var bypass = !!j.bypass;
  el.className = 'peqstat' + (bypass ? ' bypass' : '');
  var txt;
  if (bypass) {
    txt = 'PEQ 当前：已旁路（PEQ 不生效，信号直通）';
  } else if (active === 0 && Math.abs(pre) < 0.001) {
    txt = 'PEQ 当前：生效 · ' + (total || 20) + ' 段全为 0 dB、前级 0 dB（不改变声音）';
  } else {
    txt = 'PEQ 当前：生效 · ' + active + ' / ' + (total || 20) + ' 段非 0 dB' +
          (Math.abs(pre) >= 0.001 ? ('、前级 ' + pre.toFixed(1) + ' dB') : '') + '（输出已被 EQ 改变）';
  }
  tx.textContent = txt;
  el.title = txt;
}
// 节流：文字输入 260ms；拖拽/滚轮/键盘 100ms（拖拽时不许每次 pointermove 都打后端）
function peqScheduleApply(ms) {
  clearTimeout(PEQ_TIMER);
  PEQ_TIMER = setTimeout(peqApply, ms === undefined ? 260 : ms);
}
function peqApply() {
  var body = 'bypass=' + ((D('peqBypass') && D('peqBypass').checked) ? 1 : 0)
    + '&preamp=' + encodeURIComponent((D('peqPreamp') ? D('peqPreamp').value : 0) || 0)
    + '&bands=' + encodeURIComponent(peqBandsParam());
  peqMsg('应用中…');
  api('/api/peq', body).then(function (j) {
    if (!j || !j.ok) { peqMsg('失败：' + ((j && j.error) || '未知错误')); return; }
    PEQ_STATE = j;
    peqDrawCurve(j.points);   // 只刷新曲线，不重建输入框（避免打断正在输入的光标）
    peqStatusRender(j);       // 261024-04 #3：状态行刷新（以后端返回值为准）
    peqMsg('已应用（' + (j.bypass ? '旁路' : '生效') + ' · 采样率 ' + Math.round(j.fs) + ' Hz）');
  }).catch(function (e) { peqMsg('失败：' + e); });
}
function peqLoadState() {
  return api('/api/peq').then(function (j) {
    if (j && j.ok) peqRender(j);
  }).catch(function () { });
}
function peqPresetAction(action, name) {
  api('/api/peq/preset', 'action=' + encodeURIComponent(action) + '&name=' + encodeURIComponent(name || '')).then(function (j) {
    if (!j || !j.ok) { toast('预设操作失败：' + ((j && j.error) || '未知错误')); return; }
    peqRender(j);
    if (action === 'load') toast('已加载预设：' + name);
    else if (action === 'save') toast('已保存预设：' + name);
    else if (action === 'reset') toast('已重置为 Flat');
    else toast('已删除预设');
  }).catch(function (e) { toast('预设操作失败：' + e); });
}
function initPeqControls() {
  if (!D('peqBypass')) return;   // 当前页面没有 PEQ 卡（例如还没渲染设置页）
  // 261024-04 #3：勾选/取消后状态行**立刻**变（先按本地状态刷，不等后端往返）；再节流下发。
  D('peqBypass').onchange = function () {
    var st = {};
    for (var kk in (PEQ_STATE || {})) st[kk] = PEQ_STATE[kk];
    st.bypass = !!this.checked;
    peqStatusRender(st);
    peqScheduleApply();
  };
  if (D('peqPreamp')) D('peqPreamp').oninput = function () { peqScheduleApply(); };
  if (D('peqAutoPre')) D('peqAutoPre').onclick = peqAutoPreamp;
  if (D('peqLoad')) D('peqLoad').onclick = function () {
    var sel = D('peqPreset'); if (sel) peqPresetAction('load', sel.value);
  };
  if (D('peqSave')) D('peqSave').onclick = function () {
    var n = prompt('预设名（保存到 rlplayer-peq-presets.txt）：', (PEQ_STATE && PEQ_STATE.name) || '我的 PEQ');
    if (n && n.trim()) peqPresetAction('save', n.trim());
  };
  if (D('peqDelete')) D('peqDelete').onclick = function () {
    var sel = D('peqPreset'); if (!sel) return;
    if (confirm('删除用户预设「' + sel.value + '」？（内置预设不能删除）')) peqPresetAction('delete', sel.value);
  };
  if (D('peqReset')) D('peqReset').onclick = function () {
    if (confirm('重置为 Flat（20 段全部 0dB）？')) peqPresetAction('reset', 'Flat');
  };
  peqLoadState();
}

function initEqViewLogic() {
  initPeqControls();
  initRoomCorr();      // 2026-10-26（261026-01 #2）：「房间校正」卡片的「开始向导」按钮（预览）
  var sInput = D('sp_opra_search');
  var guide = D('eqSearchGuide');
  var hint = D('sp_opra_searchhint');
  var cardsBox = D('eqModelCards');
  var presetsArea = D('eqPresetsArea');
  var presetsBox = D('eqPresetCards');
  var targetInfo = D('eqTargetCurveInfo');
  var copyChip = D('sp_opra_copychip');
  var selectedTitle = D('eqSelectedModelTitle');
  var msg = D('sp_opra_msg');

  if (copyChip) {
    copyChip.onclick = function () {
      var text = 'https://opra.roon.app/';
      if (navigator.clipboard && navigator.clipboard.writeText) {
        navigator.clipboard.writeText(text).then(function () { toast('已复制来源链接: ' + text); });
      } else {
        toast('来源链接: ' + text);
      }
    };
  }

  function doSearch(q) {
    q = (q || '').trim();
    if (!q) {
      // 空输入：型号卡片数 == 0，展示引导与热门芯片（261023-03 第6条）
      cardsBox.innerHTML = '';
      if (guide) guide.style.display = 'block';
      if (hint) hint.style.display = 'none';
      if (presetsArea) presetsArea.style.display = 'none';
      return;
    }

    if (guide) guide.style.display = 'none';

    if (!OPRA_INDEX) {
      if (hint) { hint.textContent = '正在加载耳机数据库…'; hint.style.display = 'block'; }
      loadOpraIndex(function () { if (eqTabActive()) doSearch(q); });   // 切走后丢弃过期回调
      return;
    }

    var tokens = q.toLowerCase().split(/\s+/).filter(Boolean);
    var allHits = [];
    for (var i = 0; i < OPRA_INDEX.length; ++i) {
      var item = OPRA_INDEX[i];
      var str = (item[1] + ' ' + item[2] + ' ' + item[0]).toLowerCase();
      var match = true;
      for (var t = 0; t < tokens.length; ++t) {
        if (str.indexOf(tokens[t]) < 0) { match = false; break; }
      }
      if (match) allHits.push(item);
    }

    // 最多 30 条，带继续输入提示（261023-03 第6条）
    var hits = allHits.slice(0, 30);
    if (hint) {
      if (!allHits.length) {
        hint.textContent = '未找到匹配的耳机型号，请尝试不同关键词';
      } else if (allHits.length > 30) {
        hint.textContent = '共匹配到 ' + allHits.length + ' 款耳机，已显示前 30 条。继续输入更具体关键词以精确筛选…';
      } else {
        hint.textContent = '匹配到 ' + hits.length + ' 款耳机型号';
      }
      hint.style.display = 'block';
    }

    // 渲染型号卡片（品牌 · 型号 · 预设）
    var ch = '';
    hits.forEach(function (it) {
      var id = it[0], vendor = it[1], name = it[2];
      var isCur = (id === ADV.opraproduct);
      // 每项统一显示「品牌 · 型号」（261023-03 第6条：品牌/型号不能是两个互相重叠的搜索框）
      ch += '<div class="model-card card' + (isCur ? ' active' : '') + '" data-id="' + esc(id) + '" data-vendor="' + esc(vendor) + '" data-name="' + esc(name) + '">' +
            '<div style="font-size:13px;font-weight:600;line-height:1.35;color:var(--txt)">' + esc(vendor) + ' · ' + esc(name) + '</div>' +
            '<div style="font-size:11px;color:var(--mut2);margin-top:5px;display:flex;align-items:center;justify-content:space-between">' +
            '<span>OPRA 校准预设</span><span class="badge" style="font-size:10px">查看调音预设</span>' +
            '</div></div>';
    });
    cardsBox.innerHTML = ch;

    // 绑定点击型号卡片
    Array.prototype.forEach.call(cardsBox.querySelectorAll('.model-card'), function (card) {
      card.onclick = function () {
        Array.prototype.forEach.call(cardsBox.querySelectorAll('.model-card'), function (c) { c.classList.remove('active'); });
        card.classList.add('active');
        loadProductPresets(card.dataset.id, card.dataset.vendor, card.dataset.name);
      };
    });
  }

  function loadProductPresets(productId, vendor, name) {
    if (!presetsArea) return;
    presetsArea.style.display = 'block';
    if (selectedTitle) selectedTitle.textContent = vendor + ' ' + name + ' · 调音预设';
    if (msg) msg.textContent = '正在获取预设…';
    if (presetsBox) presetsBox.innerHTML = '<div class=empty style="padding:20px 0">正在加载调音目标曲线…</div>';
    if (targetInfo) targetInfo.style.display = 'none';

    OPRA_CUR_PRODUCT = productId;   // 连点不同型号时，只认最后一次选择（防止慢请求把旧预设盖到新型号上）
    api('/api/opra/presets?productId=' + encodeURIComponent(productId)).then(function (j) {
      if (!eqTabActive() || OPRA_CUR_PRODUCT !== productId) return;   // 切走 / 已换型号 → 丢弃
      if (!j.ok || !j.items || !j.items.length) {
        if (msg) msg.textContent = '暂无可用预设';
        if (presetsBox) presetsBox.innerHTML = '<div class=empty style="padding:20px 0">该型号暂无可用调音预设</div>';
        return;
      }
      var presets = j.items;
      OPRA_PRESETS = presets;
      if (msg) msg.textContent = (j.fromCache ? '已加载本地缓存（' : '共 ') + presets.length + ' 种调音目标）';

      // 渲染卡片式调音预设（至少 3 张卡，261023-03 第7条）
      var ph = '';
      presets.forEach(function (p) {
        var curve = getTargetCurveDesc(p.details, p.author);
        var isActive = (ADV.opraproduct === productId && ADV.oprapreset === p.id);
        ph += '<div class="preset-card card' + (isActive ? ' active' : '') + '" data-id="' + esc(p.id) + '">' +
              '<div>' +
              '<div style="display:flex;align-items:flex-start;justify-content:space-between;gap:8px;margin-bottom:4px">' +
              '<b style="font-size:13.5px;color:var(--txt);line-height:1.35">' + esc((p.author || 'OPRA') + (p.details ? (' · ' + p.details) : '')) + '</b>' +
              '<span class="badge" style="background:var(--hov);white-space:nowrap">' + p.bandsCount + ' 段</span>' +
              '</div>' +
              '<div style="font-size:11px;color:var(--mut2)">目标曲线：' + esc(curve.name) + '</div>' +
              '<div class="tc-curve" title="目标曲线示意（手绘近似，非测量数据）">' + targetCurveSvg(curve.kind) + '</div>' +
              '<div style="font-size:12px;color:var(--txt2);line-height:1.5;margin:8px 0 0;background:var(--card);padding:8px 10px;border-radius:6px;border:1px solid var(--line)">' +
              '说明：' + esc(curve.scene) + '</div>' +
              '<div style="font-size:11.5px;color:var(--mut2);margin-top:8px">前级增益：<b>' + p.gainDb + ' dB</b> · 频段数：<b>' + p.bandsCount + ' 段</b></div>' +
              '</div>' +
              '<div style="margin-top:12px;display:flex;align-items:center;justify-content:space-between;gap:8px">' +
              '<span style="font-size:11px;color:var(--mut2)">点击卡片选中</span>' +
              '<button class="btn pri opra-apply-btn" data-pid="' + esc(productId) + '" data-eid="' + esc(p.id) + '" style="font-size:12px;padding:5px 14px">' +
              (isActive ? '已应用当前预设' : '应用到 EQ') + '</button>' +
              '</div></div>';
      });
      presetsBox.innerHTML = ph;

      // 选中当前或第一个预设展示曲线说明
      var curP = presets.find(function (x) { return x.id === ADV.oprapreset; }) || presets[0];
      if (curP) {
        showTargetCurveInfo(curP);
      }

      // 绑定应用按钮与卡片点击
      Array.prototype.forEach.call(presetsBox.querySelectorAll('.preset-card'), function (pcard) {
        pcard.onclick = function () {
          // 选中高亮（261023-03 第7条）：点卡片只选中/预览；真正写入 EQ 要再点「应用到 EQ」
          Array.prototype.forEach.call(presetsBox.querySelectorAll('.preset-card'), function (c) { c.classList.remove('active'); });
          pcard.classList.add('active');
          var eid = pcard.dataset.id;
          var p = presets.find(function (x) { return x.id === eid; });
          if (p) showTargetCurveInfo(p);
        };
      });

      Array.prototype.forEach.call(presetsBox.querySelectorAll('.opra-apply-btn'), function (btn) {
        btn.onclick = function (ev) {
          ev.stopPropagation();
          var pid = btn.dataset.pid;
          var eid = btn.dataset.eid;
          applyOpraPreset(pid, eid, presets);
        };
      });
    }).catch(function (err) {
      if (msg) msg.textContent = '获取失败: ' + err;
      if (presetsBox) presetsBox.innerHTML = '<div class=empty style="padding:20px 0">网络错误或预设获取失败</div>';
    });
  }

  function showTargetCurveInfo(p) {
    if (!targetInfo) return;
    var curve = getTargetCurveDesc(p.details, p.author);
    targetInfo.innerHTML = '<div style="display:flex;gap:18px;flex-wrap:wrap;align-items:flex-start">' +
      '<div style="flex:1;min-width:260px">' +
      '<div style="font-weight:700;font-size:13px;margin-bottom:6px;color:var(--txt)">' + esc(curve.name) + ' · 目标曲线说明</div>' +
      '<div style="margin-bottom:5px"><b style="color:var(--txt)">是什么：</b>' + esc(curve.tech) + '</div>' +
      '<div><b style="color:var(--txt)">适合谁：</b>' + esc(curve.scene) + '</div>' +
      '</div>' +
      '<div style="flex:0 0 260px;min-width:220px" class="tc-curve">' +
      '<div style="font-size:10.5px;color:var(--mut2);margin-bottom:2px">目标曲线示意（手绘近似）</div>' +
      targetCurveSvg(curve.kind, 78) +
      '</div>' +
      '</div>' +
      '<div style="margin-top:10px;font-size:11.5px;color:var(--mut2);border-top:1px dashed var(--line);padding-top:8px">' +
      '前级增益：' + p.gainDb + ' dB（独立衰减防削波） · 滤波段数：' + p.bandsCount + ' 段 · 预设：' + esc(p.details || p.id) + '</div>';
    targetInfo.style.display = 'block';
  }

  function applyOpraPreset(pid, eid, presets) {
    api('/api/opra/apply', 'productId=' + encodeURIComponent(pid) + '&presetId=' + encodeURIComponent(eid)).then(function (j) {
      if (!j.ok) { toast(j.error || '应用失败'); return; }
      ADV.opraproduct = pid;
      ADV.oprapreset = eid;
      // 后端 /api/opra/apply 返回字段是 path（见 rtaweb.cpp）；eqPath 仅作兼容。
      // 这里以前只认 eqPath，导致「应用成功」但 EQ 文件路径根本没写进去（261023-03 第7条实测抓到）。
      var appliedPath = j.eqPath || j.path;
      if (appliedPath) ADV.eq = appliedPath;
      if (D('sp_eq')) D('sp_eq').value = ADV.eq;
      saveSettings(true);
      toast('已应用调音预设到 EQ');
      Array.prototype.forEach.call(presetsBox.querySelectorAll('.preset-card'), function (c) {
        var isThis = (c.dataset.id === eid);
        c.classList.toggle('active', isThis);
        var b = c.querySelector('.opra-apply-btn');
        if (b) b.textContent = isThis ? '已应用当前预设' : '应用到 EQ';
      });
      var p = presets.find(function (x) { return x.id === eid; });
      if (p) showTargetCurveInfo(p);
    });
  }

  // 搜索框事件
  if (sInput) {
    sInput.oninput = function () { doSearch(this.value); };
    sInput.onkeydown = function (ev) { if (ev.key === 'Enter') doSearch(this.value); };
  }

  // 热门品牌芯片点击
  if (guide) {
    Array.prototype.forEach.call(guide.querySelectorAll('.brand-chip'), function (btn) {
      btn.onclick = function () {
        var b = btn.dataset.brand;
        if (sInput) {
          sInput.value = b;
          doSearch(b);
        }
      };
    });
  }

  // 房间校正保存按钮
  if (D('bSaveRoomEq')) {
    D('bSaveRoomEq').onclick = function () {
      if (D('sp_eq')) ADV.eq = (D('sp_eq').value || '').trim();
      if (D('sp_ir')) ADV.ir = (D('sp_ir').value || '').trim();
      if (D('sp_irgpu')) ADV.irgpu = D('sp_irgpu').checked ? 1 : 0;
      saveSettings(true);
      toast('已保存房间校正设置');
    };
  }

  // 初始：有已选型号 → 回填「品牌 型号」并加载预设；否则保持空输入（0 张型号卡片）。
  // 空输入时这里**不发起任何 /api/opra/index 请求**（261023-03 第6条）。
  if (ADV.opraproduct) {
    loadOpraIndex(function () {
      if (!eqTabActive()) return;
      var it = null;
      for (var i = 0; i < (OPRA_INDEX || []).length; ++i) {
        if (OPRA_INDEX[i][0] === ADV.opraproduct) { it = OPRA_INDEX[i]; break; }
      }
      if (it) {
        sInput.value = it[1] + ' ' + it[2];
        doSearch(sInput.value);
        loadProductPresets(it[0], it[1], it[2]);
      }
    });
  } else {
    doSearch('');
  }
}



// ============================================================================
// 音质分析 / 版本对比（B2 quality.h + B3 version_compare.h；2026-10-27 接线）
// ----------------------------------------------------------------------------
// 数据源：
//   POST /api/health?file=…  → track_health_json（老字段 + quality 19 项）
//   POST /api/compare        → {files:[…]} 逐项对比表 + 排序 + 解释
// 全部走自绘 #modal（CDP 自检下 window.prompt/confirm 会把页面挂住）。
// ============================================================================
function qFmt(v, d) {
  if (v === undefined || v === null || (typeof v === 'number' && !isFinite(v))) return '-';
  if (typeof v !== 'number') return String(v);
  if (v <= -299 && v > -1e6) return '不适用';   // −300 是"测不出"的哨兵值，不能显示成 −300 dB
  return v.toFixed(d === undefined ? 2 : d);
}
function openQualityModal(path) {
  if (!path) { toast('没有可分析的曲目'); return; }
  openModal('分析音质', '<div class=empty>正在分析…（流式分块解码，曲越长越久）</div>');
  apiRaw('/api/health?file=' + encodeURIComponent(path)).then(function (j) {
    var h = (j && j.health) || null;
    if (!j || !j.ok || !h) {
      D('mbody').innerHTML = '<div class=empty>' + esc((j && j.error) || (h && h.error) || '分析失败') + '</div>';
      return;
    }
    var q = h.quality || {};
    function row(label, val, hint) {
      return '<tr><td style="color:var(--mut);padding:3px 10px 3px 0;white-space:nowrap">' + esc(label) +
             (hint ? tip(hint) : '') + '</td><td style="padding:3px 0;font-variant-numeric:tabular-nums">' + esc(String(val)) + '</td></tr>';
    }
    var html = '<div class=hint style="margin-bottom:8px">' + esc(path) + '</div>';
    html += '<div style="display:flex;gap:18px;flex-wrap:wrap">';
    // 失真
    html += '<table style="font-size:12px"><tbody>'
         +  row('THD+N（单基频）', q.thdn_available ? (qFmt(q.thdn_db) + ' dB') : ('不适用（' + (q.thdn_method || '') + '）'))
         +  row('非谐波残留', qFmt(q.non_tonal_db) + ' dB', '16 个最强谱峰之外的能量占比 = 噪声/失真/有损残留的上界')
         +  row('互调失真 IMD', q.imd_available ? (qFmt(q.imd_db) + ' dB') : (q.imd_note || '不适用'))
         +  '</tbody></table>';
    // 噪声 + 频响
    html += '<table style="font-size:12px"><tbody>'
         +  row('本底噪声', qFmt(q.noise_floor_dbfs) + ' dBFS', '最低 10% 的 100 ms 块电平；密集音乐是本底的上界')
         +  row('谱最小本底', qFmt(q.spectral_min_dbfs) + ' dBFS')
         +  row('信噪比', qFmt(q.snr_db) + ' dB')
         +  row('谱重心', qFmt(q.centroid_hz, 0) + ' Hz')
         +  row('谱倾斜', qFmt(q.tilt_db_per_oct) + ' dB/oct')
         +  row('95% 滚降点', qFmt(q.rolloff95_hz, 0) + ' Hz')
         +  row('>10 kHz 占比', qFmt(q.hf_ratio_10k_db) + ' dB')
         +  '</tbody></table>';
    // 削波 + 动态 + 立体声
    html += '<table style="font-size:12px"><tbody>'
         +  row('峰值 / RMS', qFmt(q.peak_dbfs_q) + ' / ' + qFmt(q.rms_dbfs) + ' dBFS')
         +  row('削波样本', qFmt(q.clipped_samples, 0) + '（' + qFmt(q.clipped_pct, 4) + '%）')
         +  row('削波段数', qFmt(q.clipped_runs, 0) + ' 段，最长 ' + qFmt(q.longest_clip_run, 0) + ' 样本')
         +  row('峰值因子', qFmt(q.crest_db) + ' dB')
         +  row('动态 P95−P10', qFmt(q.dyn_p95_p10_db) + ' dB')
         +  row('动态重心', qFmt(q.dyn_centroid_dbfs) + ' dBFS')
         +  row('静态段占比', qFmt(q.silence_pct) + ' %')
         +  row('声道相关 / 单声道损失', q.stereo ? (qFmt(q.channel_corr, 3) + ' / ' + qFmt(q.mono_loss_db) + ' dB') : '非立体声')
         +  '</tbody></table>';
    html += '</div>';
    html += '<div class=hint style="margin-top:10px">体检老字段：DR ' + qFmt(h.dr_db) + ' dB ｜ LUFS ' + qFmt(h.lufs) +
            ' ｜ LRA ' + qFmt(h.lra) + ' ｜ 假无损 ' + (h.lossless_suspected ? '疑似' : '否') +
            ' ｜ 截止 ' + qFmt(h.cutoff_hz, 0) + ' Hz' + (q.note ? ('<br>' + esc(q.note)) : '') + '</div>';
    D('mbody').innerHTML = html;
  }).catch(function (e) {
    D('mbody').innerHTML = '<div class=empty>分析失败：' + esc(e && e.message ? e.message : e) + '</div>';
  });
}

// ---- 版本对比：多选同目录下的音频文件 → 对比页 ----
function openCompareModal(path) {
  var dir = String(path || '').replace(/[\\\/][^\\\/]*$/, '');
  if (!dir) { toast('没有可对比的曲目'); return; }
  openModal('对比版本', '<div class=empty>正在列出可选文件…</div>');
  apiRaw('/api/files?dir=' + encodeURIComponent(dir)).then(function (j) {
    var fs2 = (j && j.files) || [];
    if (!fs2.length) { D('mbody').innerHTML = '<div class=empty>该目录下没有可对比的音频文件</div>'; return; }
    var base = String(path).replace(/^.*[\\\/]/, '');
    var h = '<div class=hint style="margin-bottom:8px">' + esc(dir) + '：勾选 2~8 个版本（同一首曲子的不同转制/母带）</div>';
    h += '<div style="max-height:46vh;overflow:auto;display:flex;flex-direction:column;gap:2px">';
    fs2.forEach(function (n, i) {
      h += '<label class=chk style="font-size:12px;display:flex;gap:8px;align-items:center">' +
           '<input type=checkbox class=cmpck data-n="' + esc(n) + '"' + (n === base ? ' checked' : '') + '>' + esc(n) + '</label>';
    });
    h += '</div><div style="display:flex;gap:10px;align-items:center;margin-top:10px">' +
         '<button class="btn pri" id=cmpGo>开始对比</button><span class=hint id=cmpMsg></span></div>';
    h += '<div id=cmpOut></div>';
    D('mbody').innerHTML = h;
    D('cmpGo').onclick = function () {
      var sel = [];
      Array.prototype.forEach.call(document.querySelectorAll('#mbody .cmpck'), function (c) {
        if (c.checked) sel.push(dir + '\\' + c.dataset.n);
      });
      if (sel.length < 2) { D('cmpMsg').textContent = '至少勾选 2 个版本'; return; }
      D('cmpMsg').textContent = '正在对齐并分析 ' + sel.length + ' 个版本…';
      D('cmpGo').disabled = true;
      apiRaw('/api/compare', JSON.stringify({ files: sel })).then(function (r) {
        D('cmpGo').disabled = false;
        D('cmpMsg').textContent = '';
        D('cmpOut').innerHTML = renderCompare(r);
      }).catch(function (e) {
        D('cmpGo').disabled = false;
        D('cmpMsg').textContent = '对比失败：' + (e && e.message ? e.message : e);
      });
    };
  }).catch(function (e) {
    D('mbody').innerHTML = '<div class=empty>列目录失败：' + esc(e && e.message ? e.message : e) + '</div>';
  });
}
function renderCompare(r) {
  if (!r || !r.ok) return '<div class=empty>' + esc((r && r.error) || '对比失败') + '</div>';
  var vs = r.versions || [], rows = r.rows || [];
  var h = '<div style="margin-top:12px;font-size:12px">';
  if (!r.sameTrack) h += '<div class=hint style="color:var(--err)">⚠ 包络相关太低，这些文件可能不是同一首曲子：结论仅供参考</div>';
  h += '<div style="font-weight:650;margin-bottom:4px">排序（好 → 差）</div><ol style="margin:0 0 8px 18px;padding:0">';
  // 版本号（V1/V2…）是后端给的序号，单看它认不出是哪个文件 ⟹ 一律带上文件名。
  function vName(v, i) {
    var base = String((v && (v.path || v.label)) || ('#' + i)).replace(/^.*[\\\/]/, '');
    return base;
  }
  (r.ranking || []).forEach(function (i) {
    var v = vs[i] || {};
    h += '<li>' + esc(v.label || ('#' + i)) + ' <span style="color:var(--mut)">' + esc(vName(v, i)) + '</span>' +
         ' — <b>' + qFmt(v.score, 1) + '</b> 分' +
         (v.error ? (' <span style="color:var(--err)">' + esc(v.error) + '</span>') : '') + '</li>';
  });
  h += '</ol><div style="max-height:40vh;overflow:auto"><table style="font-size:11.5px;border-collapse:collapse;width:100%">';
  h += '<thead><tr><th style="text-align:left;padding:3px 8px 3px 0">指标</th>';
  vs.forEach(function (v, i) {
    h += '<th style="text-align:right;padding:3px 0 3px 10px;font-weight:600">' + esc(v.label || '') +
         '<div style="font-weight:400;color:var(--mut2);font-size:10.5px;max-width:180px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap">' +
         esc(vName(v, i)) + '</div></th>';
  });
  h += '</tr></thead><tbody>';
  rows.forEach(function (row) {
    h += '<tr' + (row.comparable ? '' : ' style="opacity:.55"') + '><td style="padding:2px 8px 2px 0;white-space:nowrap">' +
         esc(row.label) + (row.unit ? (' <span style="color:var(--mut2)">' + esc(row.unit) + '</span>') : '') +
         (row.comparable ? '' : tip(row.why)) + '</td>';
    (row.values || []).forEach(function (v, i) {
      var best = (i === row.bestIndex && row.comparable && !row.informational);
      h += '<td style="text-align:right;padding:2px 0 2px 10px;font-variant-numeric:tabular-nums' +
           (best ? ';font-weight:700;color:var(--acc)' : '') + '">' + qFmt(v) + '</td>';
    });
    h += '</tr>';
  });
  h += '</tbody></table></div>';
  h += '<div class=hint style="margin-top:8px;white-space:pre-wrap">' + esc(r.explanation || '') + '</div>';
  h += '</div>';
  return h;
}

// ============================================================================
// 一键反馈（261027-01 #1；后端 = src/net/feedback.h，端点 = POST /api/feedback）
// ----------------------------------------------------------------------------
// 为什么不用 api()/apiRaw()：那两个帮手把超时写死 8 s，而后端的反馈总预算是 14 s
// （取 token ≤2.5 s + HTTP ≤11 s）。用 8 s 会在"其实正在上传"时误报失败，用户会重复提交。
// 为什么不用 window.prompt/confirm：CDP 自检下原生对话框会把页面挂住（本项目踩过），
// 统一走自绘 #modal 与页内状态行。
// 返回的七个字段全部原样列出：uploaded / status / path / sha / local / error / message。
// 无凭据时 uploaded=false 且 local=<本地绝对路径> —— 界面必须显示它，否则用户以为反馈丢了。
// ============================================================================
function fbSubmit() {
  var msg = D('sp_fbmsg'), out = D('sp_fbresult'), btn = D('sp_fbsubmit');
  var payload = {
    type:  (D('sp_fbtype')  || {}).value || 'bug',
    title: ((D('sp_fbtitle') || {}).value || '').trim(),
    body:  ((D('sp_fbbody')  || {}).value || '').trim(),
    steps: ((D('sp_fbsteps') || {}).value || '').trim()
  };
  if (!payload.title && !payload.body) { toast('标题和详细描述至少要填一个'); return; }
  if (out) out.textContent = '';
  if (msg) msg.textContent = '正在打包并上传…（含日志尾，最长约 14 秒）';
  if (btn) btn.disabled = true;
  var ctrl = (typeof AbortController !== 'undefined') ? new AbortController() : null;
  var timer = setTimeout(function () { if (ctrl) ctrl.abort(); }, 30000);
  fetch('/api/feedback', {
    method: 'POST', headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(payload), signal: ctrl ? ctrl.signal : undefined
  }).then(function (r) { return r.json(); }).then(function (j) {
    clearTimeout(timer);
    if (btn) btn.disabled = false;
    var lines = [
      'uploaded: ' + (j.uploaded ? 'true' : 'false'),
      'status:   ' + (j.status === undefined ? '-' : j.status),
      'path:     ' + (j.path || '-'),
      'sha:      ' + (j.sha || '-'),
      'local:    ' + (j.local || '-'),
      'error:    ' + (j.error || '-')
    ];
    if (out) out.textContent = lines.join('\n') + (j.message ? ('\n\n' + j.message) : '');
    if (msg) msg.textContent = j.uploaded ? '已上传，感谢反馈！' : (j.local ? '已存到本地（无上传凭据）' : '提交失败');
    toast(j.message || (j.uploaded ? '反馈已上传' : '反馈已保存到本地'));
  }).catch(function (e) {
    clearTimeout(timer);
    if (btn) btn.disabled = false;
    var why = (e && e.name === 'AbortError') ? '请求超时（30 s）' : (e && e.message ? e.message : String(e));
    if (out) out.textContent = 'error: ' + why;
    if (msg) msg.textContent = '提交失败';
    toast('反馈提交失败：' + why);
  });
}

// ReplayGain 的非 bit-perfect 警告显隐（文案唯一来源：dsp/replaygain.h 的 kBitPerfectWarning）
function rgWarn() {
  var w = D('sp_rgwarn'), s = D('sp_rgmode');
  if (!w || !s) return;
  var on = (s.value === 'track' || s.value === 'album');
  w.style.display = on ? '' : 'none';
}

// 2026-10-23 用户 261023-04 #2：EQ 已并入设置页，不再需要「前往 EQ 页面」跳转按钮的绑定。
// ============================================================================
// 诊断导出（用户 2026-10-22 功能3）
// ----------------------------------------------------------------------------
// 用户原话：「'导出诊断信息'其实就是一个 log，最好是直接导出一个压缩包或一个整体的
// 文件，不要用户单独找 rlplayer-diag.json / rlplayer-playback.log、launcher.log
// 三份文件；另外这个选项放在元数据这里明显不合适，单独出来做选项卡吧，
// 我们还在开发，报错是必不可少的。」
//
// 落点：
//   * 独立「诊断」页签（不塞在元数据组里）—— 见 renderSettingsPage 的 data-g=diag；
//   * 主按钮「导出诊断包（.zip）」→ GET /api/diagzip，服务端打成【一个 ZIP】；
//   * 次按钮「只要 JSON」→ 保留 /api/diag，方便有人只想贴一段文本。
//
// 打包为什么在服务端做（选型理由，别改回去）：
//   ✗ 不调 PowerShell Compress-Archive —— 这是分发给用户的程序，用户机器上不保证有
//     pwsh/powershell（精简系统 / Server Core / 组策略禁用都可能没有）。把功能建在
//     "用户机器恰好有个 shell" 上，等于埋一个只在别人机器上炸的雷。
//   ✗ 不引 JSZip 之类第三方库 —— 项目至今零压缩依赖，还要过分发兼容门禁。
//   ✓ 服务端自己写 STORE（不压缩）ZIP：几十行，见 src/net/zipstore.h。
//     诊断包本来就是几 KB~几 MB 的文本，不压缩的代价可以忽略。
// ============================================================================
function downloadBlob(blob, filename) {
  var a = document.createElement('a');
  a.href = URL.createObjectURL(blob);
  a.download = filename;
  document.body.appendChild(a);
  a.click();
  a.remove();
  setTimeout(function () { URL.revokeObjectURL(a.href); }, 5000);
}
function diagStamp() {
  var d = new Date();
  function p2(n) { return (n < 10 ? '0' : '') + n; }
  return d.getFullYear() + p2(d.getMonth() + 1) + p2(d.getDate()) + '-' +
         p2(d.getHours()) + p2(d.getMinutes()) + p2(d.getSeconds());
}
function diagMsg(m) { var e = D('sp_diagmsg'); if (e) e.textContent = m || ''; }

function exportDiagZip() {
  var btn = D('sp_diagzip');
  if (btn) btn.disabled = true;
  diagMsg('正在打包…');
  fetch('/api/diagzip').then(function (r) {
    if (!r.ok) throw new Error('HTTP ' + r.status);
    return r.blob();
  }).then(function (blob) {
    // 必须校验是 ZIP（PK 03 04）：服务端没有这个端点时会回 JSON 兜底报文，
    // 那样下到的是坏包 —— 宁可不给，也不假装成功（铁律 4）。
    if (blob.size < 22) throw new Error('包太小');
    return blob.slice(0, 4).arrayBuffer().then(function (b) {
      var v = new Uint8Array(b);
      if (!(v[0] === 0x50 && v[1] === 0x4B && v[2] === 0x03 && v[3] === 0x04)) throw new Error('不是 ZIP');
      var name = 'rlplayer-diag-' + diagStamp() + '.zip';
      downloadBlob(blob, name);
      diagMsg('已导出 ' + name + '（' + Math.round(blob.size / 1024) + ' KB）');
      toast('诊断包已导出：' + name);
    });
  }).catch(function (e) {
    diagMsg('');
    toast('诊断包导出失败：' + (e && e.message ? e.message : e));
  }).then(function () { if (btn) btn.disabled = false; });
}

// 只要 JSON：有人只想贴一段文本给开发者时更方便，所以保留这条路径。
// 「端点不存在」有两种形态都要兜住：真 404，以及本后端对未知 API 的
// 「HTTP 200 + {"ok":false,"error":"not found"}」（rtaweb 的统一兜底）。
function exportDiag() {
  fetch('/api/diag').then(function (r) {
    if (!r.ok) throw new Error('HTTP ' + r.status);
    return r.text();
  }).then(function (t) {
    var noDiag = false;
    try { if (JSON.parse(t).ok === false) noDiag = true; } catch (e) { }
    if (noDiag) throw new Error('diag not available');
    var name = 'rlplayer-diag-' + diagStamp() + '.json';
    downloadBlob(new Blob([t], { type: 'application/json' }), name);
    diagMsg('已导出 ' + name);
    toast('诊断 JSON 已导出');
  }).catch(function () {
    diagMsg('');
    toast('诊断端点不可用（/api/diag 未响应）');
  });
}

function drawRoots() {
  var box = D('sp_roots'); if (!box) return;
  var h = '';
  (S.roots || []).forEach(function (r) {
    h += '<div class=rootline><span title="' + esc(r) + '">' + esc(r) + '</span><button data-r="' + esc(r) + '">\u00d7</button></div>';
  });
  box.innerHTML = h || '<div style="font-size:11.5px;color:var(--mut2)">还没有添加目录</div>';
  Array.prototype.forEach.call(box.querySelectorAll('button'), function (b) {
    b.onclick = function () {
      var r = b.dataset.r;
      S.roots = (S.roots || []).filter(function (x) { return x !== r; });
      drawRoots();
      api('/api/lib/roots', 'remove=' + encodeURIComponent(r)).then(function () {
        loadLibStats();
        toast('已移除');
      });
    };
  });
}

// 把设置页/快捷面板的值统一写回并保存
function collectSettings() {
  ADV.bits = parseInt(D('sp_bits').value, 10) || 0;
  ADV.taps = parseInt(D('sp_taps').value, 10) || 0;
  ADV.atten = parseFloat(D('sp_atten').value) || 0;
  ADV.transition = parseFloat(D('sp_transition').value) || 0;
  ADV.phase = D('sp_phase').value || '';
  ADV.modorder = parseInt(D('sp_modorder').value, 10) || 5;
  ADV.modcut = parseFloat(D('sp_modcut').value) || 0;
  ADV.moddith = (D('sp_moddith').value === '') ? -1 : parseFloat(D('sp_moddith').value);
  if (D('sp_eq')) ADV.eq = (D('sp_eq').value || '').trim();
  if (D('sp_ir')) ADV.ir = (D('sp_ir').value || '').trim();
  if (D('sp_irgpu')) ADV.irgpu = D('sp_irgpu').checked ? 1 : 0;
  ADV.ramp = parseFloat(D('sp_ramp').value) || 0;
  ADV.gpuvendor = D('sp_gpu').value || 'auto';
  if (D('sGpu')) D('sGpu').value = ADV.gpuvendor;
  // 输出后端 / 声道 / 每声道微调
  if (D('sp_backend')) ADV.backend = D('sp_backend').value || 'wasapi';
  // 261024-01 #11：NAA 端点优先取自动发现的下拉；手动框（折叠兜底）有值则以它为准
  // 261024-01 #9：UPnP 目标渲染器（与「投放」区块同一下拉选择）
  if (D('sp_upnp_dev') && D('sp_upnp_dev').value) ADV.upnpDev = D('sp_upnp_dev').value;
  if (D('sp_ch')) ADV.channels = parseInt(D('sp_ch').value, 10) || 0;
  [0, 1, 2, 3, 4, 5, 6, 7].forEach(function (i) {
    var e = D('sp_trim' + i);
    if (e) ADV['trim' + i] = parseFloat(e.value) || 0;
  });
  if (D('sp_sdmo1')) ADV.sdmFilt = D('sp_sdmo1').value || '';
  if (D('sp_sdmoNx')) ADV.sdmFiltNx = D('sp_sdmoNx').value || '';
  if (D('sp_directsdm')) ADV.directsdm = D('sp_directsdm').checked ? 1 : 0;
  if (D('sp_rgmode')) ADV.rgmode = D('sp_rgmode').value || 'off';
  if (D('sp_rgclip')) ADV.rgclip = D('sp_rgclip').checked ? 1 : 0;
  if (D('sp_dsdkind')) ADV.dsdKind = D('sp_dsdkind').value || 'fir';
  if (D('sp_dsdconv')) ADV.dsdConv = D('sp_dsdconv').value || 'narrow';
  applyDsd();
  if (D('sp_inbackend')) ADV.inBackend = D('sp_inbackend').value || 'none';
  if (D('sp_indev')) ADV.inDev = D('sp_indev').value || '';
  if (D('sp_inch')) ADV.inOffset = parseInt(D('sp_inch').value, 10) || 0;
  if (D('sp_inbuf')) ADV.inBuf = parseInt(D('sp_inbuf').value, 10) || 100;
  if (D('sp_inmon')) ADV.inMonitor = D('sp_inmon').checked ? 1 : 0;
  if (D('sp_inseconds')) ADV.inSeconds = parseFloat(D('sp_inseconds').value) || 0;
  if (D('sp_inout')) ADV.inOut = (D('sp_inout').value || '').trim();
  // 同步回快捷面板

  D('sRate').value = D('sp_rate').value;
  D('sFilt').value = D('sp_filt').value;
  D('sFiltNx').value = D('sp_filtNx').value;
  D('sShap').value = D('sp_shaper').value;
  D('sBuf').value = D('sp_buffer').value;
  D('sIsp').checked = D('sp_isp').checked;
  D('s2s').checked = D('sp_2s').checked;
  // #5：设置页「播放模式」下拉已删 ⟹ 不再从它回写；#sRep 自己就是真相源（底栏按钮改的就是它）。
  D('sLoud').checked = D('sp_loud').checked;
  S.loud = D('sLoud').checked;
  // #2：sShuf 是派生量（模式==3 就是随机），不再从第二个控件读、也不再单独发 /api/shuffle。
  // 旧实现这里分两条请求（repeat + shuffle），中间态会让"图标/行为"短暂不一致。
  D('sShuf').checked = (parseInt(D('sRep').value, 10) === 3);
  // 行为类参数立即下发（唯一入口）
  api('/api/playmode?mode=' + D('sRep').value);
  applyRep(D('sRep').value);      // 图标/角标跟着设置页的选择走（同一真相源）
}

function applySettings() {
  collectSettings();
  saveSettings(true);
  // 顺带把音乐库目录那一栏的输入也处理掉
  var extra = (D('sp_root') && D('sp_root').value.trim()) ? ('&rootpending=1') : '';
  toast('设置已保存' + extra);
}

/* ---------------- 键盘快捷键 ---------------- */
document.addEventListener('keydown', function (e) {
  if ((e.ctrlKey || e.metaKey) && (e.key === 'f' || e.key === 'F')) {
    e.preventDefault();
    var q = D('q');
    if (q) { q.focus(); q.select(); }
    return;
  }
  var t = e.target || {};
  if (t.tagName === 'INPUT' || t.tagName === 'SELECT' || t.tagName === 'TEXTAREA') {
    if (e.key === 'Escape' && t.id === 'q') {
      t.blur();
    }
    return;
  }
  if (e.key === ' ') { e.preventDefault(); D('bPause').click(); }
  else if (e.key === 'ArrowRight') {
    e.preventDefault();
    if (e.shiftKey) seekRel(5);
    else D('bNext').click();
  }
  else if (e.key === 'ArrowLeft') {
    e.preventDefault();
    if (e.shiftKey) seekRel(-5);
    else D('bPrev').click();
  }
  else if (e.key === 'ArrowUp') { e.preventDefault(); setVol(Math.min(0, S.vol + 1)); }
  else if (e.key === 'ArrowDown') { e.preventDefault(); setVol(Math.max(-60, S.vol - 1)); }
  else if (e.key === 'n') D('bNext').click();
  else if (e.key === 'p') D('bPrev').click();
  else if (e.key === 's') D('bStop').click();
  else if (e.key === 'l') D('bLyric').click();
  // Esc：层级返回（抽屉 -> 单曲页 -> 弹窗 -> 设置页 -> 专辑/艺术家详情页）与返回按钮行为一致
  else if (e.key === 'Escape') {
    if (D('npPage') && D('npPage').classList.contains('on')) {
      if (typeof npDrawerIsOpen === 'function' && npDrawerIsOpen()) {
        npDrawerClose(); npDataRenderBody();
      } else {
        closeNowPlaying();
      }
    } else if (D('modal') && D('modal').classList.contains('on')) {
      D('modal').classList.remove('on');
    } else if (D('setpage') && D('setpage').classList.contains('on')) {
      setView('albums');
    } else if (S.view === 'album') {
      loadAlbums();
    } else if (S.view === 'artist') {
      loadArtists();
    } else if (S.view === 'works') {
      if (S.worksFrom === 'np' || (D('wBackNp') && S.lastCur)) openNowPlaying();
      else loadAlbums();
    }
  }
  else if (e.key === 'i') showChain();
});

/* ---------------- 播放队列（主视图） ---------------- */
function renderQueuePage() {
  S.view = 'queue';
  // 2026-10-23（自检暴露）：标题原来只在 /api/queue 回来后才写 → 本地接口虽快，但慢的时候
  // 会出现"已经在队列页、标题还是上一页"的空窗。先同步写标题，再等数据补数量/按钮。
  head('播放队列', '', '');
  api('/api/queue').then(function (j) {
    if (S.view !== 'queue') return;   // 已切走 → 丢弃，别覆写当前页面
    S.q = { items: j.items || [], index: j.index || 0 };
    var n = S.q.items.length;
    // #2（261026-09）：用户「从播放队列那里直接保存为歌单即可」⟹ 队列页顶部提供
    // 「保存为歌单」（写入 rlplayer-savedqueues.conf，含顺序与 CUE 区间，重启后仍在）。
    head('播放队列', n ? (n + ' 首') : '',
      (n ? '<button class="btn pri" id=qSaveSq title="把当前队列原样存成一个歌单（含顺序与 CUE 区间，重启后仍在）">保存为歌单</button>' : '') +
      (n ? '<button class=btn id=qClrAll>清空队列</button>' : ''));
    if (!n) {
      D('list').innerHTML = '<div class=empty>队列是空的<br><span style="font-size:11.5px">把文件拖进窗口，或右键专辑/曲目「加入队列」</span></div>';
      return;
    }
    var h = '<div class=qlist2>';
    S.q.items.forEach(function (f, i) {
      h += '<div class="qrow' + (i === S.q.index ? ' on' : '') + '" data-i=' + i + ' draggable=true>' +
           '<span class=qn>' + (i + 1) + '</span>' +
           '<span class=qt title="' + esc(f) + '">' + esc(nam(f)) + '</span>' +
           // 评审 #8：以前这里打完整物理路径，又长又不可读；只显示所在文件夹名，全路径放 tooltip
           '<span class=qp title="' + esc(f) + '">' + esc(parentFolder(f)) + '</span>' +
           '<button class=qdel data-i=' + i + ' title="从队列移除">\u00d7</button></div>';
    });
    D('list').innerHTML = h + '</div>';
    Array.prototype.forEach.call(D('list').querySelectorAll('.qrow'), function (e) {
      var i = +e.dataset.i;
      e.onclick = function (ev) { if (ev.target.classList.contains('qdel')) return; playPath(S.q.items[i]); };
      e.ondragstart = function (ev) { ev.dataTransfer.setData('text/plain', String(i)); ev.dataTransfer.effectAllowed = 'move'; };
      e.ondragover = function (ev) { ev.preventDefault(); ev.dataTransfer.dropEffect = 'move'; };
      e.ondrop = function (ev) {
        ev.preventDefault(); ev.stopPropagation();
        var from = parseInt(ev.dataTransfer.getData('text/plain'), 10);
        if (isNaN(from) || from === i) return;
        api('/api/qmove?from=' + from + '&to=' + i, '').then(function () { renderQueuePage(); loadQ(); });
      };
      var d = e.querySelector('.qdel');
      if (d) d.onclick = function (ev) {
        ev.stopPropagation();
        api('/api/qdel?i=' + i, '').then(function () { toast('已移除'); renderQueuePage(); loadQ(); });
      };
    });
    if (D('qSaveSq')) D('qSaveSq').onclick = function () { saveQueueDialog(); };
    if (D('qClrAll')) D('qClrAll').onclick = function () {
      api('/api/clear', '').then(function () { toast('队列已清空'); renderQueuePage(); loadQ(); });
    };
  }).catch(function (e) { renderPanelError('queue', renderQueuePage, e); });
}

/* ---------------- 正在播放整页 ---------------- */
function openNowPlaying() {
  var pg = D('npPage'); if (!pg) return;
  pg.classList.add('on');
  // 每次都刷新：上一首的 LY 可能还在；而且内嵌歌词是异步加载的，
  // 只更新 #lyricbox 不会刷新整页的 #npLyric（用户反馈"整页没有歌词"）。
  if (S.lastCur) {
    loadLyrics(S.lastCur);
    if (!S.curMeta) loadMeta(S.lastCur);
  }
  renderNp();
  // 261024-04 #7：进入单曲页立刻自动加载这首歌的 AI 整理（不必等 400ms 的 renderNp 轮询）
  npAutoSummary();
  // 整页控件是底栏/设置页的镜像：进来时刷一次，绝不显示与底栏不同的值
  applyRep(D('sRep') ? D('sRep').value : 0);
  syncVolUi();
  pollCastState(true);   // 261024-02 #5：进来就取一次投放状态（按钮文案/高亮由 applyCastUi 统一刷）
}
function closeNowPlaying() { var pg = D('npPage'); if (pg) pg.classList.remove('on'); }

/* ---------------- 单曲页「资料」面板（用户 261023-04 #7/#9） ----------------
   数据：GET /api/entity/summary?kind=track|release|person&name=..&artist=..&lang=zh
     - 未命中时后端返回 {pending:true}，用 fetchSummary 的 2s 轮询等结果；
     - 命中：{ok:true, sections, citations, summaryKind, summaryCached, unverified?}
   行为（费用纪律）：
     - 打开单曲页自动加载「专辑 + 艺术家」（有缓存则秒回，不闪加载行）；
     - 「歌曲」只在首次切到该 tab 时请求（按曲目缓存，不随每首歌自动烧钱）；
     - 「作品」只在判定为古典时出现，走 /api/entity/versions（Discogs 版本推荐），
       并提供进入「古典作品」整页的入口（左导航已按 #9 移除）。
   状态机：idle → loading（骨架）→ ok / error；切歌/关页丢弃过期回调。 */
var NP_DATA = { key: '', active: '', tabs: [], cache: {}, inflight: {}, req: 0, bodyKey: '', tabSig: '', live: { v: 1 } };

function npFileName(p) {
  var s = String(p || '').replace(/\\/g, '/');
  var i = s.lastIndexOf('/');
  return i >= 0 ? s.slice(i + 1) : s;
}
function npPlayingKindText(m) {
  var t = '正在播放';
  if (!m) return t;
  var b = [];
  if (m.codec) b.push(String(m.codec).toUpperCase());
  if (m.sampleRate) b.push((m.sampleRate / 1000).toFixed(m.sampleRate % 1000 ? 1 : 0) + ' kHz');
  if (m.bits) b.push(m.bits + ' bit');
  return t + (b.length ? '  ·  ' + b.join(' / ') : '');
}
// 古典判定：后端若回 composer 字段则直接算；否则用 genre/专辑名/曲名特征兜底。
// 只有特征命中才出现「作品」tab，避免给流行专辑无端加一个会发 Discogs 请求的入口。
function npIsClassical(m) {
  if (!m) return false;
  if (m.composer && String(m.composer).trim()) return true;
  var g = String(m.genre || ''), a = String(m.album || ''), t = String(m.title || '');
  if (/classical|古典|classique|baroque|巴洛克|opera|オペラ/i.test(g)) return true;
  if (/古典/.test(a)) return true;
  var s = a + ' ' + t;
  return /(symphon|concerto|sonata|caprice|capriccio|nocturne|prelude|overture|string quartet|requiem|mass in|opus\s*\d|op\.\s*\d+|kv\.?\s*\d+|bwv\s*\d+|symphonie|konzert|奏鸣曲|交响|协奏|练习曲|夜曲|前奏曲|序曲|组曲|安魂曲|作品\s*\d+)/i.test(s);
}
// 作曲家猜测（后端暂未回 composer 时的兜底）：曲名末尾括号（(Tchaikovsky)）、
// 专辑名冒号前缀（Paganini: ...）。猜不到就留空（/api/entity/versions 的 composer 是可选的）。
function npGuessComposer(m) {
  if (!m) return '';
  if (m.composer) return String(m.composer);
  var a = String(m.album || ''), t = String(m.title || '');
  var pm = t.match(/\(([A-Z][A-Za-z\u00c0-\u024f.' -]{2,40})\)\s*$/);
  if (pm) return pm[1].trim();
  var i = a.indexOf(':');
  if (i > 1) {
    var pre = a.slice(0, i).trim();
    if (pre && pre.split(/\s+/).length <= 3 && /^[A-Za-z\u00c0-\u024f.' -]+$/.test(pre)) return pre;
  }
  return '';
}
function npWorkTarget(m) {
  if (!m) return { title: '', composer: '' };
  var t = String(m.title || '');
  // 在线补全有时把曲名改写成「原文 (中文译名)」。Discogs 检索要用**原文**：
  // 去掉末尾带 CJK 的括号译名（否则 composer 猜对也搜不到版本，实测 1024 那次就 0 条）。
  var mm = t.match(/^(.*?)\s*[（(]([^()（）]*)[）)]\s*$/);
  if (mm && mm[1] && /[\u4e00-\u9fff]/.test(mm[2])) t = mm[1].trim();
  return { title: t, composer: npGuessComposer(m) };
}
// 当前 EQ / IR 芯片（用户 261023-04 #4）：读 /api/settings 的 eq/ir（loadSettings 填进 ADV），
// 有值才出现；点击跳「EQ 与房间校正」页；tooltip 显示当前文件。
function npEqChipsHtml() {
  var h = '';
  var eq = ADV.eq || '', ir = ADV.ir || '';
  if (eq) {
    h += '<button class=chip id=npEqChip title="' + esc('当前 EQ：' + npFileName(eq) + '（点击打开 EQ 设置）') + '">' +
         '<svg width=11 height=11 viewBox="0 0 24 24" fill=none stroke=currentColor stroke-width=2 style="vertical-align:-1px"><path d="M4 21v-7M4 10V3M12 21v-9M12 8V3M20 21v-5M20 12V3M1 14h6M9 8h6M17 16h6"/></svg>EQ</button>';
  }
  if (ir) {
    h += '<button class=chip id=npIrChip title="' + esc('当前 IR：' + npFileName(ir) + '（点击打开 声音校准）') + '">' +
         '<svg width=11 height=11 viewBox="0 0 24 24" fill=none stroke=currentColor stroke-width=2 style="vertical-align:-1px"><path d="M3 12h3l2-6 3 12 2-8 2 4h6"/></svg>IR</button>';
  }
  return h;
}
function npBindEqChips() {
  var eq = D('npEqChip'), ir = D('npIrChip');
  if (eq) eq.onclick = function () { closeNowPlaying(); openSettingsTab('eq'); };
  // #6：IR 属于「声音校准」页签，EQ 芯片进「EQ」页签（各进各的地方，不再都跳 EQ）
  if (ir) ir.onclick = function () { closeNowPlaying(); openSettingsTab('cal'); };
}
function npDataReset() {
  // 261024-05 #T2：切歌 → 取消上一首所有在飞/轮询中的资料请求（旧实体的轮询不该继续打后端）
  if (NP_DATA.live) NP_DATA.live.v = 0;
  NP_DATA = { key: '', active: '', tabs: [], cache: {}, inflight: {}, req: 0, bodyKey: '', tabSig: '', live: { v: 1 } };
  var b = D('npDataBody'); if (b) b.innerHTML = '';
  var t = D('npTabs'); if (t) t.innerHTML = '';
  var s = D('npDataStat'); if (s) s.textContent = '';
  npPrefReset();      // 切歌：取消上一首的预取（含仍在 2s 轮询的旧专辑），下一首重新预取
  npDrawerClose();
}
// 抽屉开关（#3b ③）：资料默认不占版面，点标签才从右侧滑出
// 抽屉底边动态让出控制区高度（Lead 复核要求：1024x640 下抽屉不许盖住播放控制条）
function npDrawerFit() {
  var dr = D('npDrawer');
  if (!dr) return;
  var ctl = document.querySelector('#npPage .npctl2');
  var vh = window.innerHeight || 0;
  var bottom = 14;
  if (ctl) {
    var b = ctl.getBoundingClientRect();
    if (b.top > 0 && b.top < vh) bottom = Math.max(12, Math.round(vh - b.top + 8));
  }
  dr.style.bottom = bottom + 'px';
}
function npDrawerOpen() {
  var dr = D('npDrawer'), bg = D('npDrawerBg');
  npDrawerFit();
  if (dr) dr.classList.add('on');
  if (bg) bg.classList.add('on');
}
function npDrawerClose() {
  var dr = D('npDrawer'), bg = D('npDrawerBg');
  if (dr) dr.classList.remove('on');
  if (bg) bg.classList.remove('on');
  NP_DATA.active = '';
  npDataRenderTabs();
}
function npDrawerIsOpen() {
  var dr = D('npDrawer');
  return !!(dr && dr.classList.contains('on'));
}
function npDataTargets(m) {
  var out = [];
  var title = String((m && m.title) || '') || ((D('npt') && D('npt').textContent) || '');
  var album = String((m && m.album) || '');
  var albumArtist = String((m && m.albumArtist) || '');
  var artist = String((m && m.artist) || '');
  if (title) out.push({ k: 'track', label: '歌曲', name: title, artist: artist });
  if (album && album !== title) {
    var relArtist = (m && m.summaryArtist) || albumArtist || artist;
    out.push({ k: 'release', label: '专辑', name: album, artist: relArtist });
  }
  var personName = albumArtist || artist;
  if (personName) out.push({ k: 'person', label: '艺术家', name: personName, artist: '' });
  if (npIsClassical(m)) {
    var w = npWorkTarget(m);
    if (w.title) out.push({ k: 'work', label: '作品', name: w.title, artist: '', composer: w.composer });
  }
  return out;
}
function npDataCan(m) {
  return !!(META.summary && m && (m.summaryAvailable || m.summaryCached || m.summaryCacheOk || META.hasLlmKey === true));
}
function npDataTab(kind) {
  var tabs = NP_DATA.tabs || [];
  for (var i = 0; i < tabs.length; ++i) if (tabs[i].k === kind) return tabs[i];
  return null;
}
// 右侧标签栏（#3b ③）：默认只露「歌曲 / 专辑 / 艺术家 / 作品」四类药丸；
// 每个药丸带一个状态点（灰=没取过、脉冲=整理中、绿=已有内容、黄=取不到），点开才请求、才出抽屉。
function npDataTabsSig() {
  return (NP_DATA.tabs || []).map(function (t) {
    var st = NP_DATA.cache[t.k] || { state: 'idle' };
    return t.k + ':' + t.label + ':' + st.state + ':' + (NP_DATA.active === t.k ? '1' : '0');
  }).join('|');
}
function npDataRenderTabs() {
  var el = D('npTabs'); if (!el) return;
  var sig = npDataTabsSig();
  if (sig === NP_DATA.tabSig) return;     // renderNp 每 400ms 调一次：没变化绝不重绘（否则悬停会闪）
  NP_DATA.tabSig = sig;
  var h = '';
  (NP_DATA.tabs || []).forEach(function (t) {
    var st = NP_DATA.cache[t.k] || { state: 'idle' };
    var cls = st.state === 'ok' ? ' ok' : (st.state === 'loading' ? ' busy' : (st.state === 'error' ? ' err' : ''));
    h += '<button class="npchip' + cls + (NP_DATA.active === t.k ? ' on' : '') + '" data-k="' + esc(t.k) + '"' +
         ' title="' + esc(t.label + '：' + (t.name || '') + '（' +
           (t.k === 'release' ? (npAiReady() ? '本地曲目 + AI 整理' : '本地曲目') : 'AI 整理') + '）') + '">' +
         '<i class=dot></i>' + esc(t.label) + '</button>';
  });
  el.innerHTML = h;
  Array.prototype.forEach.call(el.querySelectorAll('.npchip'), function (b) {
    b.onclick = function () { npDataToggle(b.dataset.k); };
  });
}
// 再点同一个标签 = 收起抽屉（浮层"看一眼就走"，不逼用户找关闭按钮）
function npDataToggle(kind) {
  if (NP_DATA.active === kind && npDrawerIsOpen()) { npDrawerClose(); npDataRenderBody(); return; }
  npDataActivate(kind);
}
function npDataStatHtml(kind, st) {
  // 261024-05 A12：AI 关闭时「专辑」标签只有本地曲目 —— 不能显示永远等不到的"整理中…"
  if (kind === 'release' && !npAiReady()) {
    var ln = ((st && st.local && st.local.tracks) || []).length;
    return ln ? ('本地曲目 · ' + ln + ' 首') : '本地曲库';
  }
  if (!st || st.state === 'idle' || st.state === 'loading') {
    if (kind === 'release' && st && st.local) return '本地曲目已就绪 · AI 整理中…';
    return '整理中…';
  }
  if (st.state === 'error') return '暂无资料';
  if (st.state === 'local') {
    var n0 = ((st.local && st.local.tracks) || []).length;
    return n0 ? ('本地曲目 · ' + n0 + ' 首') : '本地曲库';
  }
  var d = st.data || {};
  if (kind === 'work') {
    var n = (d.performances || []).length;
    return (d.from_cache ? '缓存' : '实时') + (n ? ' · ' + n + ' 个版本' : '');
  }
  // 261024-01 #2：只标注来源状态（已缓存 / 刚刚整理），不再叠「未核对」这类长尾文案。
  return d.summaryCached ? '已缓存' : '刚刚整理';
}
function sanitizeStreamText(raw) {
  if (raw === null || raw === undefined) return '';
  var s = String(raw);
  return s.replace(/[\uFFFD\uD800-\uDBFF]+$/, '');
}

function npDataRenderProgress(st, kind) {
  var body = D('npDataBody');
  if (!body) return;
  var prog = (st && st.progress) || {};
  var rawText = (st && typeof st.streamText === 'string') ? st.streamText : sanitizeStreamText(prog.text);
  var cleanText = sanitizeStreamText(rawText);
  var isJson = /^\s*\{/.test(cleanText);
  var hasText = !!cleanText && !isJson;

  var chars = (typeof prog.chars === 'number') ? prog.chars : (cleanText ? cleanText.length : 0);
  var sec = (typeof prog.elapsedMs === 'number' && prog.elapsedMs >= 0)
    ? (prog.elapsedMs / 1000).toFixed(1) + 's' : '';
  var statTxt = (chars > 0 || sec)
    ? ('已生成 ' + chars + ' 字' + (sec ? (' · ' + sec) : ''))
    : '';
  var phaseTxt = prog.phase || '正在整理资料…';

  var curAttempt = (typeof prog.attempt === 'number') ? prog.attempt : 1;
  var attemptChanged = (st && st.lastAttempt !== undefined && st.lastAttempt !== curAttempt);
  if (st) st.lastAttempt = curAttempt;
  if (attemptChanged) {
    cleanText = '';
    hasText = false;
    if (st) st.streamText = '';
  }

  var wrap = D('npdStreamWrap');
  if (!wrap || wrap.dataset.kind !== kind) {
    body.innerHTML =
      '<div id=npdStreamWrap data-kind="' + esc(kind) + '" class="npdstream-wrap">' +
      '<div id=npdStreamPhase class=npdhint style="margin-bottom:8px"></div>' +
      '<div id=npdStreamStat class="npdhint npdstream-stat"></div>' +
      '<div id=npdStreamText class=npdstream-text></div>' +
      '</div>';
    NP_DATA.bodyKey = kind + '|streaming';
    wrap = D('npdStreamWrap');
  }

  var phaseEl = D('npdStreamPhase');
  var statEl = D('npdStreamStat');
  var textEl = D('npdStreamText');

  if (hasText) {
    if (phaseEl) phaseEl.style.display = 'none';
    if (statEl) {
      statEl.style.display = '';
      statEl.textContent = statTxt || ('已生成 ' + chars + ' 字');
    }
    if (textEl) {
      textEl.style.display = '';
      textEl.textContent = cleanText;
      textEl.scrollTop = textEl.scrollHeight;
    }
    body.scrollTop = body.scrollHeight;
  } else if (isJson) {
    if (phaseEl) {
      phaseEl.style.display = '';
      phaseEl.textContent = phaseTxt;
    }
    if (statEl) {
      statEl.style.display = '';
      statEl.textContent = statTxt || ('已生成 ' + chars + ' 字');
    }
    if (textEl) {
      textEl.style.display = 'none';
      textEl.textContent = '';
    }
  } else {
    if (phaseEl) {
      phaseEl.style.display = '';
      phaseEl.textContent = phaseTxt + (sec ? (' · ' + sec) : '');
    }
    if (statEl) {
      if (chars > 0 || sec) {
        statEl.style.display = '';
        statEl.textContent = statTxt;
      } else {
        statEl.style.display = 'none';
        statEl.textContent = '';
      }
    }
    if (textEl) {
      textEl.style.display = 'none';
      textEl.textContent = '';
    }
  }
}

function npDataOnProgress(kind, prog) {
  if (!prog) return;
  var st = NP_DATA.cache[kind];
  if (!st) st = NP_DATA.cache[kind] = { state: 'loading' };
  if (st.state !== 'loading' && st.state !== 'idle' && st.state !== 'local') return;

  var curAttempt = (typeof prog.attempt === 'number') ? prog.attempt : 1;
  var prevAttempt = st.lastAttempt;
  st.lastAttempt = curAttempt;
  st.progress = prog;

  var clean = sanitizeStreamText(prog.text);
  if (prevAttempt !== undefined && curAttempt !== prevAttempt) {
    st.streamText = '';
  } else {
    st.streamText = clean;
  }

  if (NP_DATA.active === kind) {
    npDataRenderProgress(st, kind);
  }
}

function npDataSkeleton() {
  return '<div class=npdskel><i style="width:92%"></i><i style="width:76%"></i><i style="width:84%"></i></div>' +
         '<div class=npdhint>正在整理资料…</div>';
}
function npDataSummaryBody(d, kind) {
  // 261024-01 #4：结构化 filters/fields → 标签值网格（renderSummaryContent 内部优先 fields）；
  // 261024-01 #2：脚注只留「AI 整理」。
  // 261024-02 #3：本地曲库数据优先（歌曲=文件真实时长；专辑=Σ本地曲目 + 逐行曲目列表）。
  kind = kind || NP_DATA.active;
  var opts = {};
  if (d && d.__local) opts.local = d.__local;
  if (kind === 'release' && d && d.__album) {
    var trs = d.__album.tracks || [], tot = 0;
    trs.forEach(function (t) { tot += (t.dur || 0); });
    if (!tot) tot = d.__album.dur || 0;
    opts.local = { '时长': fmtDurLong(tot) };
    opts.tracklist = trs;
    opts.drop = { '曲目列表': 1 };   // 用下面的对齐表格替代 AI 的一行文本
  }
  var h = renderSummaryContent(d || {}, '', kind, opts);
  if (!h) h = '<div class=npdhint>该实体暂无可用的 AI 整理资料。</div>';
  return h;
}
function npDataWorkBody(d) {
  var perfs = (d && d.performances) || [];
  var w = npDataTab('work') || { name: '', composer: '' };
  if (!d || !d.ok || !perfs.length) {
    return '<div class=npdhint>未在 Discogs 检索到该作品的演绎版本（可能网络不可达或作品名不匹配）。</div>' +
           '<div class=npdhint style="margin-top:6px">作品：' + esc(w.name) + (w.composer ? (' · ' + esc(w.composer)) : '') + '</div>' +
           '<div style="margin-top:8px"><button class=btn id=npWorkRetry>重试查询</button></div>';
  }
  var sub = (d.from_cache ? ('缓存 · ' + (d.generated_at ? fmtDate(d.generated_at) : '')) : 'Discogs 实时') +
            ' · 不同演绎 ' + (typeof d.distinctCount === 'number' ? d.distinctCount : perfs.length) +
            ' / 发行版本 ' + (typeof d.releaseCount === 'number' ? d.releaseCount : perfs.length);
  var h = '<div class=npdhint style="margin-bottom:6px">' + esc(sub) + '</div>';
  perfs.forEach(function (p) {
    var ens = p.ensemble || p.orchestra || '';
    var meta = [p.conductor, ens, p.label, p.format].filter(function (x) { return !!x; }).join(' · ');
    h += '<div class=npdwork>' +
         '<span class=wy>' + (p.year > 0 ? p.year : '—') + '</span>' +
         '<span class=wt title="' + esc(p.title) + '">' + esc(p.title || '—') + '</span>' +
         '<span class=wp title="' + esc(meta) + '">' + esc(meta || '—') + '</span>' +
         '<button class="btn chip-copy" data-url="' + esc(p.url) + '" title="复制版本链接" style="font-size:10px;padding:1px 6px;flex:none">复制</button>' +
         '</div>';
  });
  h += '<div style="margin-top:10px"><button class="btn pri" id=npWorkOpen>查看 Discogs 版本推荐 →</button></div>';
  return h;
}
// 抽屉头部：类型标签 + 实体名 + 状态（#3b ③：默认看不到这些，点标签才滑出）
function npDataHeader(kind, st) {
  var t = kind ? (npDataTab(kind) || { label: '', name: '' }) : { label: '', name: '' };
  var k = D('npDrawerKind'), n = D('npDrawerName'), s = D('npDataStat');
  if (k) k.textContent = t.label || '资料';
  if (n) n.textContent = t.name || '';
  if (s) s.textContent = kind ? npDataStatHtml(kind, st) : '';
}
function npDataRenderBody(force) {
  var body = D('npDataBody');
  if (!body) return;
  var kind = NP_DATA.active;
  if (!kind) {
    if (NP_DATA.bodyKey) { body.innerHTML = ''; NP_DATA.bodyKey = ''; }
    npDataHeader('', null);
    return;
  }
  var st = NP_DATA.cache[kind] || { state: 'idle' };
  npDataHeader(kind, st);
  // renderNp 400ms 一次：内容没变就不重绘（否则抽屉里的滚动位置会被反复重置）
  // 261024-05 A12：本地专辑到达 / 曲目数变化也必须触发重绘，否则"先本地后 AI"只生效一半。
  var lsig = st.local ? ('L' + ((st.local.tracks || []).length)) : (st.localState || '-');
  var psig = st.progress ? ('|P' + (st.progress.attempt || 1) + '|' + (st.progress.chars || 0) + '|' + (st.streamText || '').length) : '';
  var sig = kind + '|' + st.state + '|' + lsig + psig + '|' +
            (st.data ? (st.data.generated_at || (st.data.summaryCached ? 'c' : 'f')) : (st.error || ''));
  var need = (sig !== NP_DATA.bodyKey);
  if (!need && !force) return;
  NP_DATA.bodyKey = sig;
  var keepTop = body.scrollTop || 0;
  // 261024-05 A12：专辑标签 = **先本地曲目、后 AI 文本**。加载中只给一行轻量占位（不整块骨架），
  // AI 回来只补文字字段；列表永远来自本地曲库，绝不被 AI 的「曲目列表」覆盖。
  if (kind === 'release') {
    body.innerHTML = npReleaseBody(st);
    bindCopyChips(body);
    if (keepTop) body.scrollTop = keepTop;
    var rfb = D('npDataRefresh');
    if (rfb) rfb.onclick = function () { npDataEnsure('release', true); };
    return;
  }
  if (st.state === 'idle' || st.state === 'loading') {
    if (st.progress) {
      npDataRenderProgress(st, kind);
    } else {
      body.innerHTML = npDataSkeleton();
    }
    return;
  }
  if (st.state === 'error') {
    body.innerHTML = '<div class=npdhint>暂无该实体的资料。' + (st.error ? ('（' + esc(st.error) + '）') : '') + '</div>' +
      '<div style="margin-top:10px"><button class=btn id=npDataRetry>重试</button></div>';
    var rb = D('npDataRetry');
    if (rb) rb.onclick = function () { npDataEnsure(kind, true); };
    return;
  }
  if (kind === 'work') {
    body.innerHTML = npDataWorkBody(st.data) +
      '<div style="margin-top:14px"><button class="btn" id=npDataRefresh style="font-size:11px;padding:3px 10px">重新查询</button></div>';
    bindCopyChips(body);
    var wo = D('npWorkOpen'); if (wo) wo.onclick = npOpenWorks;
    var wr = D('npWorkRetry'); if (wr) wr.onclick = function () { npDataEnsure('work', true); };
    var wf = D('npDataRefresh'); if (wf) wf.onclick = function () { npDataEnsure('work', true); };
    return;
  }
  body.innerHTML = npDataSummaryBody(st.data, kind) +
    '<div style="margin-top:16px"><button class="btn" id=npDataRefresh style="font-size:11px;padding:3px 10px" title="重新请求 AI 整理（会消耗一次模型调用）">重新整理</button></div>';
  bindCopyChips(body);
  var rf = D('npDataRefresh');
  if (rf) rf.onclick = function () { npDataEnsure(kind, true); };
}
// 261024-02 #3：抽屉里的「专辑」标签也要用本地曲库真实曲目/总时长（AI 那份只补简介等文字字段）。
var NPLIB = { key: '', album: null };
function npLocalAlbum(name, artist, cb) {
  var key = String(name || '') + '|' + String(artist || '');
  if (NPLIB.key === key) { cb(NPLIB.album); return; }
  api('/api/lib/albums?q=' + encodeURIComponent(name || '')).then(function (j) {
    var list = (j && j.albums) || [], hit = null, i;
    for (i = 0; i < list.length; i++) {
      if (list[i].name === name && (!artist || !list[i].artist || list[i].artist === artist)) { hit = list[i]; break; }
    }
    if (!hit) for (i = 0; i < list.length; i++) { if (list[i].name === name) { hit = list[i]; break; } }
    if (!hit) { NPLIB.key = key; NPLIB.album = null; cb(null); return; }
    api('/api/lib/album?k=' + encodeURIComponent(hit.k)).then(function (a) {
      NPLIB.key = key; NPLIB.album = (a && a.ok) ? a : null; cb(NPLIB.album);
    }).catch(function () { cb(null); });
  }).catch(function () { cb(null); });
}
// 261024-05 A12：AI 整理是否真的可用（设置开关 + 后端权威 key/开关），决定要不要发 summary 请求。
// 注意与 npDataCan 的区别：那个还看当前曲目的 summaryAvailable 缓存标记，这里只看"能力"。
function npAiReady() {
  return META.summary == 1 && (META.hasLlmKey === true || META.summaryEnabled === true);
}
function npLocalKeyOf(t) { return String((t && t.name) || '') + '|' + String((t && t.artist) || ''); }
function npReleaseOpts(st) {
  var alb = st.local || (st.data && st.data.__album) || null;
  var opts = { drop: { '曲目列表': 1 } };   // 用下面的对齐表格替代 AI 的一行文本
  if (alb) {
    var trs = alb.tracks || [], tot = 0;
    trs.forEach(function (t) { tot += (t.dur || 0); });
    if (!tot) tot = alb.dur || 0;
    opts.local = {};
    if (tot > 0) opts.local['时长'] = fmtDurLong(tot);
    if (alb.artist) opts.local['艺术家'] = alb.artist;
  }
  return opts;
}
// 抽屉「专辑」正文：本地资料字段 + 本地曲目表（列表的唯一真相源），AI 只补文字字段。
// 状态无关：idle/loading/local/ok/error 都走这里 —— 本地一到就显示，AI 到/失败只是换文字。
function npReleaseBody(st) {
  st = st || {};
  var alb = st.local || (st.data && st.data.__album) || null;
  var opts = npReleaseOpts(st);
  var ai = (st.state === 'ok') ? st.data : null;
  var h = '';
  if (ai) h = renderSummaryContent(ai, '', 'release', opts);
  if (!h && st.progress) {
    var prog = st.progress;
    var rawText = (typeof st.streamText === 'string') ? st.streamText : sanitizeStreamText(prog.text);
    var cleanText = sanitizeStreamText(rawText);
    var isJson = /^\s*\{/.test(cleanText);
    var chars = (typeof prog.chars === 'number') ? prog.chars : (cleanText ? cleanText.length : 0);
    var sec = (typeof prog.elapsedMs === 'number' && prog.elapsedMs >= 0)
      ? (prog.elapsedMs / 1000).toFixed(1) + 's' : '';
    var statTxt = (chars > 0 || sec) ? ('已生成 ' + chars + ' 字' + (sec ? (' · ' + sec) : '')) : '';
    h += '<div id=npdStreamWrap data-kind="release" style="margin-bottom:12px;padding:10px 12px;border-radius:8px;background:var(--hov);border:1px solid var(--line)">';
    if (!cleanText && !isJson) {
      h += '<div class=npdhint id=npdStreamPhase>' + esc(prog.phase || '正在整理…') + (sec ? (' · ' + sec) : '') + '</div>';
    } else {
      h += '<div class="npdhint npdstream-stat" id=npdStreamStat style="color:var(--txt2);font-weight:600;margin-bottom:6px">' + esc(statTxt) + '</div>';
      if (cleanText && !isJson) {
        h += '<div class=npdstream-text id=npdStreamText style="max-height:160px;overflow:auto;white-space:pre-wrap;font-size:12px;color:var(--txt2)">' + esc(cleanText) + '</div>';
      }
    }
    h += '</div>';
  } else if (!h) {
    h = renderFieldGrid({}, 'release', opts);      // AI 不在/失败：静默只显示本地资料字段
  }
  if (alb) h += efTracklistHtml(alb.tracks || []);
  if (!alb) {
    if (st.state === 'loading' || st.state === 'idle' || st.localState === 'loading') {
      h += '<div class=npdhint>正在读取本地曲目…</div>';
    } else {
      h += '<div class=npdhint>本地曲库中没有这张专辑的曲目。</div>';
    }
  }
  if (ai) {
    h += '<div style="margin-top:16px"><button class="btn" id=npDataRefresh ' +
         'style="font-size:11px;padding:3px 10px" title="重新请求 AI 整理（会消耗一次模型调用）">重新整理</button></div>';
  }
  return h || '<div class=npdhint>暂无该专辑的资料。</div>';
}
// 261024-05 A12：切到「专辑」标签**先**用本地曲库拿这张专辑并渲染曲目列表（不依赖 AI、不等摘要）。
// 失败静默（localState=none），AI 文本后到时由 npDataEnsure/npPrefetchRelease 补进缓存再重绘。
function npLocalAlbumLoad(kind) {
  var t = npDataTab(kind); if (!t) return;
  var st = NP_DATA.cache[kind] || (NP_DATA.cache[kind] = {});
  if (!st.local && st.data && st.data.__album) st.local = st.data.__album;
  if (st.local) return;                                  // 预取时已经拿到本地专辑
  var key = npLocalKeyOf(t);
  if (st.localKey === key && st.localState === 'loading') return;
  st.localKey = key;
  st.localState = 'loading';
  if (NP_DATA.active === kind) npDataRenderBody(true);
  npLocalAlbum(t.name, t.artist || '', function (alb) {
    var cur = NP_DATA.cache[kind];
    if (!cur || cur.localKey !== key) return;            // 切歌/换实体 → 丢弃
    if (cur.data) cur.data.__album = alb || null;
    cur.local = alb || null;
    cur.localState = alb ? 'ok' : 'none';
    if (NP_DATA.active === kind) npDataRenderBody(true);
  });
}
// 261024-02 追加（Lead 指定，属于 #1 的体感）：打开单曲页 / 切歌时**后台预取当前专辑的 release 资料**。
// 目的：用户点「专辑」标签时直接命中缓存/已完成的生成任务，不用再等 10~30s 的首次生成。
// 纪律：
//   * 只在"用户已开启在线资料整理"时预取（META.summary==1 且 hasLlmKey/summaryEnabled 为真）；
//   * 只预取 release 这一个实体，一次（同一首歌只发一次）；不打开抽屉、不改变任何 UI 状态；
//   * 失败静默（不 toast、不留错误态）；切歌后旧结果直接丢弃（key 不匹配）。
// 261024-05 #T2：切歌时不仅丢结果，还要**停掉旧专辑的轮询**（旧实现 pending 会每 2s 再打 60 次）。
var NPPREF = { key: '', live: { v: 0 } };
function npPrefReset() {
  NPPREF.live.v = 0;
  NPPREF.key = '';
  NPPREF.live = { v: 0 };
}
// 单曲页各类资料的 URL 只在这里拼（抽屉 / 预取 / 自动加载共用，才能命中同一份 summaryOnce 缓存）
function npSummUrl(kind, t, force) {
  return '/api/entity/summary?kind=' + kind + '&name=' + encodeURIComponent((t && t.name) || '') +
         ((t && t.artist) ? ('&artist=' + encodeURIComponent(t.artist)) : '') + '&lang=zh' +
         (force ? '&refresh=1' : '');
}
function npPrefetchRelease() {
  if (!npAiReady()) return;
  var tabs = NP_DATA.tabs || [], rel = null;
  for (var i = 0; i < tabs.length; ++i) if (tabs[i].k === 'release') rel = tabs[i];
  if (!rel) return;
  var myTrack = S.lastCur || '';
  var key = myTrack + '|' + rel.name + '|' + (rel.artist || '');
  if (NPPREF.key === key) return;
  var live = { v: 1 };
  NPPREF = { key: key, live: live };
  var u = npSummUrl('release', rel);
  var req = NP_DATA.req;
  summaryOnce(u, function (s) {
    if (!live.v || NPPREF.live !== live) return;                     // 已切歌/已取消 → 丢弃
    if ((S.lastCur || '') !== myTrack) return;
    if (!summHas(s)) return;                                          // 失败静默
    var finish = function () {
      if (!live.v || NPPREF.live !== live) return;                   // 本地专辑回来前已切歌
      var cur = NP_DATA.cache['release'] || {};
      if (cur.state === 'ok') return;                                // 用户已自己取到
      var alb = s.__album || cur.local || null;
      NP_DATA.cache['release'] = { state: 'ok', data: s, local: alb,
                                  localState: alb ? 'ok' : (cur.localState || ''), localKey: cur.localKey };
      npDataRenderTabs();
      if (NP_DATA.active === 'release') npDataRenderBody();
    };
    npLocalAlbum(rel.name, rel.artist || '', function (alb) { if (alb) s.__album = alb; finish(); });
  }, false, function () { return live.v === 1 && NPPREF.live === live; }, function (prog) {
    if (NP_DATA.req !== req) return;
    npDataOnProgress('release', prog);
  });
}
/* 261024-04 #7：打开 AI 整理后，**进入单曲页就自动加载**这首歌的 summary（用户原话：不再需要点击）。
   纪律：
     * 只在"开了 AI 整理 + 有 key"时自动发起（否则一次请求都不发，行为与从前一致）；
     * 已经取到 / 正在取 / 上次失败（error 不自动重试，等用户点重试）⟹ 直接跳过；
     * 抽屉**不自动打开**（261024-01 #3b ③ 的「资料默认隐藏」是既有验收项），内容就绪后标签点一下即看；
     * 失败不弹错：右侧标签栏下面留一行可点击的「重试」。 */
function npAutoSummary() {
  if (META.summary != 1) return;
  if (META.hasLlmKey !== true && META.summaryEnabled !== true) return;
  if (!npDataTab('track')) return;
  var st = NP_DATA.cache['track'];
  if (st && (st.state === 'ok' || st.state === 'loading' || st.state === 'error')) { npRailHint(); return; }
  npDataEnsure('track', false);
  npRailHint();
}
// 右侧标签栏下面那一行状态：后台整理中 / 失败可重试（不打扰：只有这两种情况才出现）
function npRailHint() {
  var el = D('npSummHint');
  if (!el) return;
  var st = NP_DATA.cache['track'];
  if (st && st.state === 'error') {
    el.style.display = '';
    el.innerHTML = '<span class=nprailerr id=npSummRetry role=button tabindex=0 title="重新请求这首歌的 AI 整理">' +
                   '歌曲资料未能取到 · 点击重试</span>';
    var b = D('npSummRetry');
    if (b) b.onclick = function () { npDataEnsure('track', true); };
  } else if (st && st.state === 'loading' && !npDrawerIsOpen()) {
    el.style.display = '';
    el.innerHTML = '<span class=nprailbusy>正在后台整理歌曲资料…</span>';
  } else {
    el.style.display = 'none';
    el.innerHTML = '';
  }
}
function npDataEnsure(kind, force) {
  var t = npDataTab(kind); if (!t) return;
  // 261024-05 A12：AI 不可用时一次 summary 请求都不发；专辑标签照样有本地曲目/资料字段
  if (!npDataCan(S.curMeta)) {
    if (kind === 'release') {
      var stl = NP_DATA.cache[kind] || (NP_DATA.cache[kind] = {});
      if (stl.state !== 'ok') stl.state = 'local';
      if (NP_DATA.active === kind) npDataRenderBody();
    }
    return;
  }
  var st = NP_DATA.cache[kind];
  // release 的 local 态：只有**AI 确实试过且失败**才自动停手（AI 刚开启时要允许补请求）
  if (!force && st && (st.state === 'loading' || st.state === 'ok' ||
      (kind === 'release' && st.state === 'local' && st.aiFailed))) return;
  var fp = kind + '|' + t.name + '|' + (t.artist || '') + '|' + (t.composer || '');
  if (!force && NP_DATA.inflight[fp]) return;
  NP_DATA.inflight[fp] = 1;
  var req = NP_DATA.req;
  var prev = NP_DATA.cache[kind] || {};   // 换态时保留本地专辑（先本地、后 AI）
  NP_DATA.cache[kind] = { state: 'loading', local: prev.local || null,
                          localState: prev.localState || '', localKey: prev.localKey || '',
                          aiFailed: prev.aiFailed || 0,
                          progress: force ? null : (prev.progress || null),
                          streamText: force ? '' : (prev.streamText || ''),
                          lastAttempt: force ? undefined : prev.lastAttempt };
  if (NP_DATA.active === kind) npDataRenderBody();

  function done(data, err) {
    delete NP_DATA.inflight[fp];
    if (NP_DATA.req !== req) return;               // 已切歌 → 丢弃过期结果
    if (!D('npPage') || !D('npPage').classList.contains('on')) return;
    var cur = NP_DATA.cache[kind] || {};
    var alb = cur.local || (data && data.__album) || null;
    if (data) NP_DATA.cache[kind] = { state: 'ok', data: data, local: alb,
                                      localState: alb ? 'ok' : cur.localState,
                                      localKey: cur.localKey || '' };
    else NP_DATA.cache[kind] = { state: (kind === 'release' && alb) ? 'local' : 'error',
                                 error: err || '', local: alb,
                                 localState: alb ? 'ok' : cur.localState,
                                 localKey: cur.localKey || '', aiFailed: 1 };
    if (NP_DATA.active === kind) npDataRenderBody();
  }
  if (kind === 'work') {
    api('/api/entity/versions?kind=work&name=' + encodeURIComponent(t.name) +
        (t.composer ? ('&composer=' + encodeURIComponent(t.composer)) : '') + (force ? '&refresh=1' : ''))
      .then(function (j) { done((j && j.ok && (j.performances || []).length) ? j : null, j && j.error); })
      .catch(function (e) { done(null, String(e)); });
    return;
  }
  // 261024-04 #7：走 summaryOnce —— 同一实体只请求一次（专辑页/单曲页/预取共用同一份缓存）
  // 261024-05 #T2：切歌后（npDataReset 换掉 live）该实体的轮询立刻停，不再发旧 key 的请求
  var live = NP_DATA.live || { v: 1 };
  var u = npSummUrl(kind, t, force);
  summaryOnce(u, function (s) {
    // 261024-01 #4：后端新增结构化 fields（可能没有 sections）⟹ 有 fields 也算有资料
    if (!summHas(s)) { done(null, (s && s.error) || ''); return; }
    // 261024-02 #3：把本地曲库的真实时长/曲目挂到结果上（AI 的时长/曲目列表一律不采信）
    if (kind === 'track') {
      if (S.dur > 0) s.__local = { '时长': fmtDurLong(S.dur) };   // 本地文件真实时长（AI 猜的一律不采信）
      done(s, '');
    } else if (kind === 'release') {
      npLocalAlbum(t.name, t.artist, function (alb) { if (alb) s.__album = alb; done(s, ''); });
    } else {
      done(s, '');
    }
  }, false, function () { return live.v === 1 && NP_DATA.live === live; }, function (prog) {
    if (NP_DATA.req !== req) return;
    npDataOnProgress(kind, prog);
  });
}
function npDataActivate(kind) {
  NP_DATA.active = kind;
  var st = NP_DATA.cache[kind];
  if (!st) st = NP_DATA.cache[kind] = { state: 'idle' };
  // 261024-05 A12：AI 关闭时专辑标签直接是"本地曲库"态 —— 点开就有曲目，不发任何 AI 请求
  if (kind === 'release' && !npDataCan(S.curMeta) && st.state !== 'ok') st.state = 'local';
  npDataRenderTabs();
  npDataRenderBody(true);  // 强制刷一次：占位/本地列表立刻出现（点一下就有反馈）
  npDrawerOpen();
  if (kind === 'release') npLocalAlbumLoad(kind);   // 先本地曲目，AI 文本后到再补
  npDataEnsure(kind, false);
}
// 打开「古典作品 · 演绎版本」整页（左导航已移除，这是唯一入口；返回按钮见 loadWorksView）
function npOpenWorks() {
  var w = npWorkTarget(S.curMeta);
  closeNowPlaying();
  loadWorksView(w.title || '', w.composer || '', true);
}
function updateNpData(m) {
  var rail = D('npRail'), grid = D('npGrid');
  if (!rail) return;
  var key = S.lastCur || '';
  if (NP_DATA.key !== key) { npDataReset(); NP_DATA.key = key; NP_DATA.req++; }
  var can = npDataCan(m);
  var tabs = npDataTargets(m);
  // 261024-05 A12：AI 不可用时「专辑」标签仍然保留（本地曲目就能填满抽屉）；其余标签是 AI 专属
  if (!can) tabs = tabs.filter(function (t) { return t.k === 'release'; });
  if (D('npRailCap')) D('npRailCap').textContent = can ? 'AI 整理' : '专辑资料';
  if (!tabs.length) {
    // 没有任何可用资料：整个标签栏收起（不占版面、不留空白块），抽屉也收起
    rail.classList.remove('on');
    if (grid) grid.classList.remove('withdata');
    if (NP_DATA.active) npDrawerClose();
    return;
  }
  rail.classList.add('on');
  NP_DATA.tabs = tabs;
  // 切歌/元数据分两拍回来时，标签集合会变：抽屉开着的实体若已不存在 → 收起抽屉
  var activeStill = false;
  for (var i = 0; i < tabs.length; ++i) if (tabs[i].k === NP_DATA.active) activeStill = true;
  if (NP_DATA.active && !activeStill) npDrawerClose();
  npDataRenderTabs();
  // 261024-01 #3b ③「默认隐藏」：抽屉不打开、不占版面；
  // 261024-02 追加：但**专辑资料允许后台预取一次**（Lead 指定，改善"点开要等 10~30s"的体感）。
  npPrefetchRelease();
  // 261024-04 #7：进入单曲页自动加载**这首歌**的 summary（不需要点标签）
  npAutoSummary();
  if (NP_DATA.active) npDataEnsure(NP_DATA.active, false);
  npDataRenderBody();
}

// 单曲整页无内嵌封面时的占位图（内联 SVG，暗色黑胶；不引外部资源）
var NP_COVER_PLACEHOLDER = 'data:image/svg+xml;charset=utf-8,' + encodeURIComponent(
  '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 300 300">' +
  '<rect width="300" height="300" fill="#15171c"/>' +
  '<circle cx="150" cy="150" r="104" fill="none" stroke="#32353d" stroke-width="2"/>' +
  '<circle cx="150" cy="150" r="74" fill="none" stroke="#282b32" stroke-width="1.5"/>' +
  '<circle cx="150" cy="150" r="44" fill="none" stroke="#282b32" stroke-width="1.5"/>' +
  '<circle cx="150" cy="150" r="16" fill="#3d414b"/>' +
  '<circle cx="150" cy="150" r="4" fill="#15171c"/></svg>');

function renderNp() {
  var pg = D('npPage'); if (!pg || !pg.classList.contains('on')) return;
  D('npTitle').textContent = (D('npt') && D('npt').textContent) || '未播放';
  var sub = (D('npart') && D('npart').textContent) || '';
  var st = (D('nps') && D('nps').textContent) || '';
  D('npSub').textContent = sub + (st ? '  ·  ' + st : '');
  if (D('npKind')) D('npKind').textContent = npPlayingKindText(S.curMeta);
  // 元数据补全芯片 + 当前 EQ/IR 芯片（loadMeta / loadSettings 异步回来时也会走到这里刷一次）
  if (D('npMeta')) {
    // 261024-02 #5：单曲页「推送到设备」按钮（状态来自 /api/cast/state，与流媒体页/底栏同源）
    D('npMeta').innerHTML = (S.npMetaHtml || '') + npEqChipsHtml() + npCastBtnHtml();
    npBindEqChips();
    if (D('npCastBtn')) D('npCastBtn').onclick = castToggle;
    applyCastUi();
  }
  updateFavUi();
  updateNextTrackInfo();
  updateNpData(S.curMeta);
  // 视口/行高变化后抽屉底边要重新让出控制区（否则矮窗下会压住播放控制条）
  if (npDrawerIsOpen()) npDrawerFit();
  var bg = (D('cv') && D('cv').style.backgroundImage) || '';
  if (bg && bg.indexOf('url(') === 0) {
    D('npbg').style.backgroundImage = bg;
    D('npCover').src = bg.replace(/^url\("?/, '').replace(/"?\)$/, '');
    D('npCover').style.display = '';
  } else {
    D('npbg').style.backgroundImage = '';
    // 无封面时不再留一块黑斑（2026-10-23 DEBUG 视觉问题）：给一张内联 SVG 黑胶占位图，
    // 不依赖任何外部/包内图片资源。
    D('npCover').src = NP_COVER_PLACEHOLDER;
    D('npCover').style.display = '';
  }
  renderNpLyric();
}
// 整页歌词（261024-02 #4 重做）
//   * 当前行 .on 由**时间戳**决定（二分不需要，行数最多几百，线性即可）；
//   * 滚动 = 让 active 行在容器里垂直居中（lyricScrollTo），带平滑过渡，不是按百分比滚；
//   * 纯文本歌词（无时间戳）⟹ 不高亮、不自动滚（LY.timed=false）；
//   * 用户手动滚动后 3 秒内不抢滚动；seek / 切歌立刻重定位（lyricReposition 清保护）。
function renderNpLyric() {
  var box = D('npLyric'); if (!box) return;
  if (box.classList) box.classList.toggle('empty', !LY.lines.length);
  var grid = D('npGrid');
  if (grid) grid.classList.toggle('nolyric', !LY.lines.length);
  if (!LY.lines.length) {
    box.innerHTML = '<div class=npline>没有歌词<br><span style="font-size:12px;color:var(--mut2)">同名 .lrc 文件、内嵌歌词，或播放时自动在线补全（可在设置-元数据补全关闭）</span></div>';
    box.dataset.n = ''; box.dataset.at = ''; LY.at = -1;
    return;
  }
  var timed = lyricTimed();
  // 行集合或"带轴/纯文本"状态变化才重建 DOM（否则每 400ms 轮询都重建会把用户正在读的行冲掉）
  var key = LY.lines.length + ':' + (timed ? 1 : 0);
  if (box.dataset.n !== key) {
    box.dataset.n = key;
    var h = timed ? ''
      : '<div class=ly-hint title="这份歌词没有 [mm:ss] 时间轴，无法按播放位置高亮/跟随">纯文本歌词（无时间轴）· 不会自动跟随滚动</div>';
    LY.lines.forEach(function (l, i) { h += '<div class=npline data-i=' + i + '>' + esc(l.s) + '</div>'; });
    box.innerHTML = h;
    box.dataset.at = '';
    // 纯文本：留空让下面的 !timed 分支跑一次（复位 scrollTop + 清高亮），
    // 否则从"有轴歌词"切过来会停在上一首的滚动位置（实测 st=55 卡住）
    box.dataset.plain = timed ? '0' : '';
    LY.at = -1;
  }
  if (!timed) {
    // 无时间戳：一行都不高亮、也不自动滚（用户可能正在读，别动它）
    if (box.dataset.plain !== '1') {
      box.dataset.plain = '1';
      var rows0 = lyricRows(box);
      for (var z = 0; z < rows0.length; z++) rows0[z].classList.remove('on');
      box.scrollTop = 0;
    }
    return;
  }
  var pos = S.pos || 0;
  // 进度跳变（seek / 上一首 / 拖动进度条）→ 立刻重定位，不受"手动滚动保护"影响
  // ⚠ 必须同时清 dataset.at：否则"跳到同一行"时 dataset.at 相等，会吞掉这次重定位
  if (LY.lastPos >= 0 && Math.abs(pos - LY.lastPos) > 2.5) { LYMAN.until = 0; box.dataset.at = ''; }
  LY.lastPos = pos;
  var idx = -1;
  for (var i = 0; i < LY.lines.length; ++i) { if (LY.lines[i].t <= pos + 0.05) idx = i; else break; }
  if (idx < 0) idx = 0;   // 还没到第一句（前奏）：钉在第一行，避免整屏无高亮
  var rows = lyricRows(box);
  if (LY.at !== idx) {
    LY.at = idx;
    for (var r = 0; r < rows.length; r++) rows[r].classList.toggle('on', +rows[r].dataset.i === idx);
  }
  if (box.dataset.at !== String(idx)) {
    box.dataset.at = String(idx);
    if (Date.now() >= LYMAN.until) lyricScrollTo(box, rows[idx]);
  }
}
// 手动滚动保护（滚轮 / 触摸拖动）：3 秒内自动滚动不跟用户抢
(function bindLyricManual() {
  ['npLyric', 'lyricbox'].forEach(function (id) {
    var el = D(id);
    if (!el) return;
    el.addEventListener('wheel', lyricManual, { passive: true });
    el.addEventListener('touchstart', lyricManual, { passive: true });
  });
  // 视口尺寸变了（真机窗口 DPI/尺寸 race、用户拉窗）：留白与目标位置都要重算，
  // 否则"行高变了但 scrollTop 还是旧值"，看起来就是歌词停在半路。
  window.addEventListener('resize', function () {
    LYMAN.until = 0;
    var b = D('npLyric');
    if (b) b.dataset.at = '';
    if (D('npPage') && D('npPage').classList.contains('on')) renderNpLyric();
    if (RTAB === 'l') { LY.at = -1; syncLyrics(S.pos || 0, true); }
  });
})();

/* ---------------- 拖拽入队 ---------------- */
function pathsFromDrop(e) {
  var out = [], uris = '';
  try { uris = e.dataTransfer.getData('text/uri-list') || ''; } catch (x) { }
  if (!uris) { try { uris = e.dataTransfer.getData('text/plain') || ''; } catch (x) { } }
  String(uris).split(/\r?\n/).forEach(function (u) {
    u = u.trim();
    if (!u || u.charAt(0) === '#') return;
    if (u.indexOf('file:///') === 0) out.push(decodeURIComponent(u.slice(8)).replace(/\//g, SEP));
    else if (/^[A-Za-z]:[\\\/]/.test(u)) out.push(u);
  });
  return out;
}
function addMany(paths) {
  if (!paths.length) { toast('没有可加入的文件'); return; }
  var i = 0, n = 0;
  (function step() {
    if (i >= paths.length) {
      toast('已加入 ' + n + ' 项');
      loadQ();
      if (S.view === 'queue') renderQueuePage();
      return;
    }
    api('/api/add', 'file=' + encodeURIComponent(paths[i++])).then(function (j) {
      if (j && j.ok) n += (j.count || 1);
      step();
    }).catch(step);
  })();
}
function dropFiles(e) {
  var paths = pathsFromDrop(e);
  if (paths.length) { addMany(paths); return; }
  var files = e.dataTransfer.files ? Array.prototype.slice.call(e.dataTransfer.files) : [];
  if (!files.length) return;
  // 某些情况下浏览器不给真实路径：把内容上传到本机服务，再由服务端入队
  var done = 0, added = [];
  files.forEach(function (f) {
    var x = new XMLHttpRequest();
    x.open('POST', '/api/upload?name=' + encodeURIComponent(f.name));
    x.onload = function () {
      try { var r = JSON.parse(x.responseText); if (r && r.path) added.push(r.path); } catch (err) { }
      if (++done === files.length) addMany(added);
    };
    x.onerror = function () { if (++done === files.length) addMany(added); };
    x.send(f);
  });
}
(function initDrop() {
  var mask = document.createElement('div');
  mask.id = 'dropmask';
  mask.innerHTML = '<div>松开，加入播放队列</div>';
  document.body.appendChild(mask);
  var depth = 0;
  function hasFiles(e) {
    var t = e.dataTransfer && e.dataTransfer.types;
    if (!t) return false;
    for (var i = 0; i < t.length; ++i) if (t[i] === 'Files') return true;
    return false;
  }
  window.addEventListener('dragenter', function (e) {
    if (!hasFiles(e)) return;
    e.preventDefault(); ++depth; mask.classList.add('on');
  });
  window.addEventListener('dragover', function (e) {
    if (!hasFiles(e)) return;
    e.preventDefault(); e.dataTransfer.dropEffect = 'copy';
  });
  window.addEventListener('dragleave', function (e) {
    if (!hasFiles(e)) return;
    if (--depth <= 0) { depth = 0; mask.classList.remove('on'); }
  });
  window.addEventListener('drop', function (e) {
    if (!hasFiles(e)) return;
    e.preventDefault(); depth = 0; mask.classList.remove('on');
    dropFiles(e);
  });
})();

/* ---------------- 循环方式 ---------------- */
// applyRep / cycleRep 已在上面定义（底栏 bRep 与整页 npRep 共用同一份实现，
// 不各写一遍 —— 那正是历史上两处不一致的来源）。这里只做绑定。
D('bRep').onclick = cycleRep;
// 快捷面板 #sRep 被别处改动（如设置页保存）时，图标状态也要跟上
if (D('sRep')) D('sRep').addEventListener('change', function () { applyRep(this.value); });

/* ---------------- 启动 ---------------- */
initFavs();
// #6（261026-08）：左栏「保存的队列」列表（我的最爱下面）启动就渲染，重启后能立刻看到
renderSavedQueues();
if (D('npFavBtn')) D('npFavBtn').onclick = function (ev) { if (S.lastCur) toggleFav(S.lastCur, ev); };
loadLibStats();
loadSettings();
loadFilterPhases();   // 功能2：相位预设真相源（/api/filters）
fillMetaProviders();   // P1.5：数据源注册表（/api/providers）驱动元数据分组源开关
loadRates();
loadAlbums('');
loadQ();
loadSrcConfig();
tick();
setInterval(tick, 400);
setInterval(loadQ, 3000);
setInterval(loadLibStats, 2000);
)JSEOF";
}

} // namespace rta::net
