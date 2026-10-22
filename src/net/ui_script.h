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
var S = {
  view: 'albums', albums: [], album: null, files: [], remote: [],
  q: { items: [], index: 0 }, cur: '', playing: false, paused: false,
  vol: 0, loud: false, lastCur: '', roots: [], scanning: false, dur: 0, pos: 0,
  volDb: 0, startDb: 0, theme: 'dark', queueHidden: false
};
// 高级参数（设置页里改，params() 统一带上）。默认值与后端一致。
var ADV = {
  device: '', bits: 0, taps: 0, atten: 0, transition: 0, phase: '',
  modcut: 0, moddith: -1, ir: '', irgpu: 0, ramp: 0, theme: 'dark',
  eq: '', modorder: 5, gpuvendor: 'auto', directsdm: 0, dsdKind: 'fir', dsdConv: 'narrow',
  // 输出后端与多声道
  backend: 'wasapi', channels: 0, naaHost: '127.0.0.1:43210',
  trim0: 0, trim1: 0, trim2: 0, trim3: 0, trim4: 0, trim5: 0, trim6: 0, trim7: 0,
  // 输入
  inBackend: 'none', inDev: '', inOffset: 0, inBuf: 100, inMonitor: 0,
  // SDM（DSD 输出）专用滤波器：空 = 沿用 PCM 页那一组
  sdmFilt: '', sdmFiltNx: ''
};
// 元数据补全开关（P1.5 注册表化）：总开关 metaon 与 DeepSeek metallm 固定两键；
// 数据源开关按 /api/providers（后端注册表）动态渲染，写 metasrc_<id> ——
// 后端新增数据源 = 注册一个描述符，前端零改动。缺省全开，与后端缺省一致。
var META = { on: 1, llm: 1 };
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
function nam(p) {
  var s = String(p || ''), i = Math.max(s.lastIndexOf('/'), s.lastIndexOf(SEP));
  return i >= 0 ? s.slice(i + 1) : s;
}
function base(p) { return String(p || '').replace(/[/\\]+$/, ''); }
function join(a, b) { return base(a) + SEP + b; }
function api(u, body) {
  var opt = body === undefined ? {} : { method: 'POST', body: body };
  return fetch(u, opt).then(function (r) { return r.json(); });
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
         '&isp=' + (D('sIsp').checked ? 1 : 0) +
         '&twostage=' + (D('s2s').checked ? 1 : 0) +
         '&buffer=' + D('sBuf').value +
         '&modorder=' + (ADV.modorder || 5) +
         '&device=' + encodeURIComponent(ADV.device || '') +
         '&bits=' + (ADV.bits || 0) +
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
  if (D('wMin')) D('wMin').onclick = function () { wcmd('min'); };
  if (D('wMax')) D('wMax').onclick = function () { wcmd('max'); };
  if (D('wClose')) D('wClose').onclick = function () { wcmd('close'); };
})();

function fmtTag(sr, bits) {
  if (!sr) return '';
  return (sr % 1000 === 0 ? (sr / 1000) + 'k' : (Math.round(sr / 100) / 10) + 'k') + '/' + bits;
}

/* ---------------- 顶栏 / 工具区 ---------------- */
function head(title, subtitle, toolsHtml) {
  D('pth').textContent = title;
  D('cnt').textContent = subtitle || '';
  D('tools').innerHTML = toolsHtml || '';
}

/* ---------------- 专辑视图 ---------------- */
// 封面一律尝试加载：拿不到就让 img 自己消失，后面的首字母占位露出来。
// （早期用 cv 标记决定是否请求，结果「扫描时没识别出封面」就永远不显示了）
function albumCardHtml(a, i) {
  return '<div class=card data-i=' + i + '>' +
    '<div class=cv>' +
      '<div class=ph>' + esc((a.name || '?').slice(0, 1).toUpperCase()) + '</div>' +
      '<img src="/api/lib/cover?k=' + a.k + '" loading=lazy alt="" onerror="this.style.display=\'none\'">' +
      '<div class=pl><svg width=13 height=13 viewBox="0 0 24 24" fill=#fff><path d="M8 5v14l11-7z"/></svg></div>' +
    '</div>' +
    '<div class=nm title="' + esc(a.name) + '">' + esc(a.name) + '</div>' +
    '<div class=ar title="' + esc(a.artist) + '">' + esc(a.artist || '未知艺术家') +
      (a.year ? '  \u00b7  ' + a.year : '') + (a.sr ? '  \u00b7  ' + fmtTag(a.sr, a.bits) : '') + '</div>' +
    '</div>';
}
function loadAlbums(q, title) {
  S.view = 'albums';
  head(title || '专辑', '加载中…', '');
  api('/api/lib/albums?q=' + encodeURIComponent(q || '')).then(function (j) {
    S.albums = j.albums || [];
    if (!S.albums.length) {
      head(title || '专辑', '0 张', '');
      D('list').innerHTML = '<div class=empty>' +
        (S.roots.length ? '库里还没有音乐<br><span style="font-size:11.5px">在右侧「设置」里添加音乐目录，然后点重新扫描</span>'
                        : '还没有设置音乐目录<br><span style="font-size:11.5px">打开右侧「设置」→ 添加目录 → 重新扫描</span>') +
        '</div>';
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
  }).catch(function (e) { D('list').innerHTML = '<div class=empty>' + esc(e) + '</div>'; });
}

function openAlbum(k) {
  S.view = 'album';
  api('/api/lib/album?k=' + encodeURIComponent(k)).then(function (j) {
    if (!j.ok) { toast(j.error || '打不开专辑'); return; }
    S.album = j;
    var cv = "background-image:url('/api/lib/cover?k=" + j.k + "')";
    var sub = [];
    if (j.artist) sub.push(esc(j.artist));
    if (j.year) sub.push(j.year);
    sub.push(j.tracks.length + ' 首');
    sub.push(fmt(j.dur));
    var h = '<div class=ahead><div class=cv style="' + cv + '"></div><div class=info>' +
            '<div class=kind>ALBUM</div><h1>' + esc(j.name) + '</h1>' +
            '<div class=sub>' + sub.join(' \u00b7 ') + '</div>' +
            '<button class="btn pri" id=aPlay>播放</button> ' +
            '<button class=btn id=aAdd>加入队列</button> ' +
            '<button class=btn id=aShuf>随机播放</button>' +
            '</div></div>';
    h += '<div style="padding:4px 0 24px">';
    j.tracks.forEach(function (t, i) {
      h += '<div class=trow data-i=' + i + '>' +
           '<div class=n>' + (t.n || (i + 1)) + '</div>' +
           '<div class=tt title="' + esc(t.title) + '">' + esc(t.title) + '</div>' +
           '<div class=ar>' + esc(t.artist || j.artist) + '</div>' +
           '<div class=du>' + fmt(t.dur) + '</div>' +
           '</div>';
    });
    h += '</div>';
    D('list').innerHTML = h;
    head(j.name, j.tracks.length + ' 首', '');
    D('aPlay').onclick = function () { playAlbum(j.k, 0); };
    D('aAdd').onclick = function () { enqueueAlbum(j.k); };
    D('aShuf').onclick = function () { playAlbum(j.k, Math.floor(Math.random() * j.tracks.length)); };
    Array.prototype.forEach.call(D('list').querySelectorAll('.trow'), function (e) {
      e.onclick = function () { playAlbum(j.k, +e.dataset.i); };
    });
  });
}

function playAlbum(k, i) {
  api('/api/lib/play', 'k=' + encodeURIComponent(k) + '&i=' + (i || 0) + '&' + params()).then(function (j) {
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

/* ---------------- 艺术家 ---------------- */
function loadArtists() {
  S.view = 'artists';
  head('艺术家', '加载中…', '');
  api('/api/lib/albums').then(function (j) {
    var m = {}, order = [];
    (j.albums || []).forEach(function (a) {
      var k = a.artist || '未知艺术家';
      if (!m[k]) { m[k] = { n: 0, d: 0, cv: false, key: a.k }; order.push(k); }
      m[k].n++; m[k].d += a.dur;
      if (a.cv) m[k].cv = true, m[k].key = a.k;
    });
    order.sort(function (x, y) { return x.localeCompare(y, 'zh'); });
    head('艺术家', order.length + ' 位', '');
    if (!order.length) { D('list').innerHTML = '<div class=empty>库里还没有音乐</div>'; return; }
    var h = '<div class=grid style="grid-template-columns:repeat(auto-fill,minmax(180px,1fr))">';
    order.forEach(function (k) {
      var v = m[k];
      var cv = v.cv ? "background-image:url('/api/lib/cover?k=" + v.key + "');border-radius:50%" : '';
      h += '<div class=card data-a="' + esc(k) + '"><div class=cv style="' + cv + '">' +
           (v.cv ? '' : '<div class=ph>' + esc(k.slice(0, 1).toUpperCase()) + '</div>') + '</div>' +
           '<div class=nm>' + esc(k) + '</div><div class=ar>' + v.n + ' 张专辑</div></div>';
    });
    h += '</div>';
    D('list').innerHTML = h;
    Array.prototype.forEach.call(D('list').querySelectorAll('.card'), function (e) {
      e.onclick = function () {
        var who = e.dataset.a;
        openArtistProfile(who);
      };
    });
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

/* ---------------- 艺术家资料页（P3④a） ---------------- */
function openArtistProfile(who, refresh) {
  S.view = 'artist';
  setView('artists');
  S.view = 'artist';
  head(who, '正在获取权威资料…', '<button class=btn id=bBackArtistsTop>返回艺术家列表</button>');
  if (D('bBackArtistsTop')) D('bBackArtistsTop').onclick = function () { loadArtists(); };
  D('list').innerHTML = '<div class=empty style="padding:60px 0">正在获取权威资料…</div>';

  var u = '/api/entity/knowledge?kind=person&name=' + encodeURIComponent(who) + (refresh ? '&refresh=1' : '');
  api(u).then(function (j) {
    renderArtistProfile(who, j);
  }).catch(function (err) {
    renderArtistProfile(who, { ok: false, error: String(err), materials: [] });
  });
}

function renderArtistProfile(who, j) {
  S.view = 'artist';
  setView('artists');
  S.view = 'artist';

  var cvKey = '';
  if (S.albums && S.albums.length) {
    for (var i = 0; i < S.albums.length; i++) {
      if ((S.albums[i].artist || '') === who && S.albums[i].cv) { cvKey = S.albums[i].k; break; }
    }
  }
  var cvStyle = cvKey ? ("background-image:url('/api/lib/cover?k=" + cvKey + "')") : '';

  var mats = (j && j.materials) || [];
  var isOk = j && j.ok && mats.length > 0;

  var subText = '';
  if (isOk) {
    var firstDate = mats[0].fetched_at ? fmtDate(mats[0].fetched_at) : '';
    if (j.from_cache) {
      subText = '缓存 · ' + (firstDate || '近期') + ' 取回' + (j.offline ? ' · 离线' : '');
    } else {
      subText = '权威资料' + (firstDate ? (' · ' + firstDate + ' 取回') : '');
    }
  } else {
    subText = '权威资料暂缺';
  }

  head(who, subText, '<button class=btn id=bTopBack>返回艺术家列表</button>');
  if (D('bTopBack')) D('bTopBack').onclick = function () { loadArtists(); };

  var h = '';
  // 头像/名称
  h += '<div class="ahead artist-head">' +
       '<div class=cv style="border-radius:50%;' + cvStyle + '">' +
       (cvKey ? '' : '<div class=ph style="border-radius:50%">' + esc(who.slice(0, 1).toUpperCase()) + '</div>') +
       '</div>' +
       '<div class=info>' +
       '<div class=kind>ARTIST · 艺术家资料</div>' +
       '<h1>' + esc(who) + '</h1>' +
       '<div class=sub>' + esc(subText) + '</div>' +
       '<div style="display:flex;gap:8px;margin-top:10px;flex-wrap:wrap">' +
       '<button class=btn id=bArtistAlbums>查看专辑</button> ' +
       '<button class=btn id=bArtistRefresh>刷新资料</button> ' +
       '<button class=btn id=bBackArtists>返回列表</button>' +
       '</div></div></div>';

  h += '<div class=artist-body style="padding:0 22px 30px">';

  if (!isOk) {
    // 空态卡：没资料/网络失败 → 内置空态（.card + 说明 + 「重试」按钮，--phbg 风格）
    var errMsg = (j && j.errors && j.errors[0] && j.errors[0].error) || (j && j.error) || '未在维基百科或萌娘百科匹配到权威词条，或当前网络不可达。';
    h += '<div class="card empty-card" style="padding:36px 20px;text-align:center;background:var(--phbg);border:1px solid var(--line);border-radius:12px;margin-top:14px">' +
         '<div style="font-size:15px;font-weight:600;margin-bottom:8px;color:var(--txt)">暂无权威资料或网络连接失败</div>' +
         '<div style="color:var(--mut);font-size:12.5px;margin-bottom:18px;max-width:520px;margin-left:auto;margin-right:auto;line-height:1.6">' +
         esc(errMsg) + '</div>' +
         '<button class="btn pri" id=bRetryKnowledge>重试获取</button>' +
         '</div>';
  } else {
    // 1) 简介（L2 原文，长文折叠「展开」）
    var bioMat = null;
    var stylesMats = [];
    mats.forEach(function (m) {
      if (!bioMat && (m.kind === 'biography' || m.kind === 'disambiguation' || m.text)) {
        bioMat = m;
      }
      if (m.kind === 'genre' || m.kind === 'similar') {
        stylesMats.push(m);
      }
    });

    var bioText = bioMat ? bioMat.text : '';
    var isLong = bioText.length > 280 || (bioText.split('\n').length > 5);

    h += '<div class="sec-card bio-sec" style="margin-bottom:24px">' +
         '<div style="font-size:12px;font-weight:600;letter-spacing:1px;color:var(--mut2);margin-bottom:10px">简介</div>' +
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
      var srcName = '权威来源';
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
           '<button class="btn chip-copy" data-url="' + esc(m.url) + '" title="复制来源链接">' +
             '<svg width=12 height=12 viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" style="vertical-align:-1px;margin-right:4px"><rect x="9" y="9" width="13" height="13" rx="2" ry="2"/><path d="M5 15H4a2 2 0 0 1-2-2V4a2 2 0 0 1 2-2h9a2 2 0 0 1 2 2v1"/></svg>' +
             '复制链接' +
           '</button>' +
           '</div>';
    });
    h += '</div></div>';
  }

  h += '</div>';
  D('list').innerHTML = h;

  // 绑定事件
  if (D('bArtistAlbums')) {
    D('bArtistAlbums').onclick = function () {
      api('/api/lib/albums').then(function (jj) {
        S.albums = (jj.albums || []).filter(function (a) { return (a.artist || '未知艺术家') === who; });
        renderAlbumSubset(who);
      });
    };
  }
  if (D('bArtistRefresh')) {
    D('bArtistRefresh').onclick = function () { openArtistProfile(who, true); };
  }
  if (D('bBackArtists')) {
    D('bBackArtists').onclick = function () { loadArtists(); };
  }
  if (D('bRetryKnowledge')) {
    D('bRetryKnowledge').onclick = function () { openArtistProfile(who, true); };
  }
  if (D('bBioToggle')) {
    D('bBioToggle').onclick = function () {
      var box = D('bioBox');
      if (!box) return;
      var isCol = box.classList.toggle('collapsed');
      this.textContent = isCol ? '展开全部' : '收起简介';
    };
  }

  // 复制链接芯片：navigator.clipboard.writeText(url) + toast「已复制来源链接」
  Array.prototype.forEach.call(D('list').querySelectorAll('.chip-copy'), function (btn) {
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

/* ---------------- 曲目 / 搜索 ---------------- */
function loadTracks(q) {
  S.view = 'tracks';
  head(q ? ('搜索：' + q) : '曲目', '搜索中…', '');
  api('/api/lib/search?q=' + encodeURIComponent(q || '')).then(function (j) {
    var t = j.tracks || [];
    head(q ? ('搜索：' + q) : '曲目', t.length + ' 首', '');
    if (!t.length) { D('list').innerHTML = '<div class=empty>没有匹配的曲目</div>'; return; }
    var h = '<div style="padding:6px 0 24px">';
    t.forEach(function (x, i) {
      h += '<div class=trow data-i=' + i + ' style="grid-template-columns:34px 1fr 150px 92px 58px">' +
           '<div class=n>' + (i + 1) + '</div>' +
           '<div class=tt>' + esc(x.title) + '</div>' +
           '<div class=ar>' + esc(x.artist) + ' \u2014 ' + esc(x.album) + '</div>' +
           '<div class=bd>' + fmtTag(x.sr, x.bits) + '</div>' +
           '<div class=du>' + fmt(x.dur) + '</div></div>';
    });
    D('list').innerHTML = h + '</div>';
    Array.prototype.forEach.call(D('list').querySelectorAll('.trow'), function (e) {
      e.onclick = function () { playPath(t[+e.dataset.i].path); };
      e.oncontextmenu = function (ev) { ev.preventDefault(); addPath(t[+e.dataset.i].path); };
    });
  });
}

/* ---------------- 文件夹 ---------------- */
function loadFiles(d) {
  S.view = 'files';
  api('/api/files?dir=' + encodeURIComponent(d || '')).then(function (j) {
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
  }).catch(function (e) { D('list').innerHTML = '<div class=empty>' + esc(e) + '</div>'; });
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
  }).catch(function () { D('list').innerHTML = '<div class=empty>读取失败</div>'; });
}

/* ---------------- 播放控制 ---------------- */
function playPath(f) {
  api('/api/play', 'file=' + encodeURIComponent(f) + '&' + params()).then(function (j) {
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

/* ---------------- 队列 ---------------- */
function loadQ() {
  api('/api/queue').then(function (j) {
    S.q = { items: j.items || [], index: j.index || 0 };
    if (D('sShuf')) D('sShuf').checked = !!j.shuffle;
    if (D('sRep') && j.repeat !== undefined) { D('sRep').value = String(j.repeat); applyRep(j.repeat); }
    D('rcnt').textContent = '共 ' + S.q.items.length + ' 首';
    if (!S.q.items.length) {
      D('qlist').innerHTML = '<div class=empty style="padding:26px 14px">队列为空<br><span style="font-size:11px">右键专辑或曲目可加入</span></div>';
      return;
    }
    var h = '';
    S.q.items.forEach(function (f, i) {
      h += '<div class="qi' + (i === S.q.index ? ' on' : '') + '" data-i=' + i + ' draggable=true>' +
           '<span class=n>' + (i + 1) + '</span><span class=t title="' + esc(f) + '">' + esc(nam(f)) + '</span></div>';
    });
    D('qlist').innerHTML = h;
    Array.prototype.forEach.call(D('qlist').children, function (e) {
      if (e.dataset.i === undefined) return;
      e.onclick = function () { playPath(S.q.items[+e.dataset.i]); };
      e.oncontextmenu = function (ev) {          // 右键从队列移除
        ev.preventDefault();
        api('/api/qdel?i=' + e.dataset.i, '').then(function () { toast('已移除'); loadQ(); });
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
    if (m.title) D('npt').textContent = m.title;
    else D('npt').textContent = nam(f);
    var sub = [];
    if (m.artist) sub.push(m.artist);
    if (m.album) sub.push(m.album);
    D('npart').textContent = sub.join(' \u00b7 ');
    D('cv').style.backgroundImage = m.hasCover ? "url('/api/cover?file=" + encodeURIComponent(f) + "')" : '';
    // 补全芯片：存全局，renderNp 统一填进 #npMeta；整页开着就立刻刷一次（meta 是异步回来的）
    S.npMetaHtml = buildNpMetaHtml(m);
    if (D('npPage') && D('npPage').classList.contains('on')) renderNp();
  }).catch(function () { D('npt').textContent = nam(f); });
}

/* ---------------- 状态 ---------------- */
function tick() {
  api('/api/status').then(function (j) {
    S.playing = !!j.playing; S.paused = !!j.paused; S.dur = j.dur; S.pos = j.pos;
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
      D('gpub').style.display = j.gpu ? '' : 'none';
    } else {
      D('fmt').textContent = '--';
      D('gpub').style.display = 'none';
      if (S.lastCur) { S.lastCur = ''; D('npt').textContent = '未播放'; D('npart').textContent = ''; D('nps').textContent = '选择一个专辑开始'; D('cv').style.backgroundImage = ''; S.npMetaHtml = ''; if (D('npMeta')) D('npMeta').innerHTML = ''; }
    }
    if (j.error) D('nps').textContent = j.error;
  }).catch(function () { });
}

/* ---------------- 播放设置持久化 ---------------- */
var SET_KEYS = [['sRate','rate'], ['sFilt','filter'], ['sFiltNx','filterNx'], ['sShap','shaper'],
                ['sDSD','dsd'], ['sBuf','buffer'], ['sIsp','isp'], ['s2s','twostage'], ['sGpu','gpuvendor']];
function saveSettings() {
  var body = '';
  SET_KEYS.forEach(function (p) {
    var e = D(p[0]); if (!e) return;
    var v = (e.type === 'checkbox') ? (e.checked ? '1' : '0') : e.value;
    if (body) body += '&';
    body += p[1] + '=' + encodeURIComponent(v);
  });
  body += '&volume=' + S.vol + '&loudness=' + (D('sLoud').checked ? '1' : '0');
  body += '&theme=' + encodeURIComponent(S.theme || 'dark');
  body += '&device=' + encodeURIComponent(ADV.device || '') + '&bits=' + (ADV.bits || 0)
        + '&taps=' + (ADV.taps || 0) + '&atten=' + (ADV.atten || 0)
        + '&transition=' + (ADV.transition || 0) + '&phase=' + encodeURIComponent(ADV.phase || '')
        + '&modorder=' + (ADV.modorder || 5) + '&modcut=' + (ADV.modcut || 0)
        + '&moddith=' + (ADV.moddith === undefined ? -1 : ADV.moddith)
        + '&eq=' + encodeURIComponent(ADV.eq || '')
        + '&ir=' + encodeURIComponent(ADV.ir || '') + '&irgpu=' + (ADV.irgpu || 0)
        + '&ramp=' + (ADV.ramp || 0)
        + '&backend=' + encodeURIComponent(ADV.backend || 'wasapi')
        + '&channels=' + (ADV.channels || 0)
        + '&naahost=' + encodeURIComponent(ADV.naaHost || '')
        + '&trim=' + [0, 1, 2, 3, 4, 5, 6, 7].map(function (i) { return ADV['trim' + i] || 0; }).join(',')
        + '&inbackend=' + encodeURIComponent(ADV.inBackend || 'none')
        + '&indev=' + encodeURIComponent(ADV.inDev || '')
        + '&inoffset=' + (ADV.inOffset || 0)
        + '&inbuf=' + (ADV.inBuf || 100)
        + '&inmon=' + (ADV.inMonitor || 0)
        + '&sdmfilt=' + encodeURIComponent(ADV.sdmFilt || '')
        + '&sdmfiltnx=' + encodeURIComponent(ADV.sdmFiltNx || '')
        + '&directsdm=' + (ADV.directsdm ? 1 : 0)
        + '&dsdkind=' + (ADV.dsdKind || 'fir') + '&dsdconv=' + (ADV.dsdConv || 'narrow')
        // 元数据补全（P1.5）：metaon/metallm 固定两键；源开关按注册表逐源写 metasrc_<id>
        + '&metaon=' + (META.on ? 1 : 0)
        + '&metallm=' + (META.llm ? 1 : 0)
        + (META_PROVIDERS || []).map(function (p) {
            return '&metasrc_' + p.id + '=' + (META_SRC[p.id] === 0 ? 0 : 1);
          }).join('');
  clearTimeout(window._setT);
  window._setT = setTimeout(function () { api('/api/settings', body); }, 400);
}
function loadSettings() {
  api('/api/settings').then(function (j) {
    var kv = j.kv || {};
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
    if (kv.trim !== undefined) {
      var tv = String(kv.trim).split(',');
      for (var ti = 0; ti < 8; ti++) ADV['trim' + ti] = parseFloat(tv[ti]) || 0;
    }
    if (kv.inbackend !== undefined) ADV.inBackend = kv.inbackend || 'none';
    if (kv.indev !== undefined) ADV.inDev = kv.indev || '';
    if (kv.inoffset !== undefined) ADV.inOffset = parseInt(kv.inoffset, 10) || 0;
    if (kv.inbuf !== undefined) ADV.inBuf = parseInt(kv.inbuf, 10) || 100;
    if (kv.inmon !== undefined) ADV.inMonitor = (kv.inmon === '1') ? 1 : 0;
    if (kv.sdmfilt !== undefined) ADV.sdmFilt = kv.sdmfilt || '';
    if (kv.sdmfiltnx !== undefined) ADV.sdmFiltNx = kv.sdmfiltnx || '';
    if (kv.directsdm !== undefined) ADV.directsdm = (kv.directsdm === '1') ? 1 : 0;
    if (kv.dsdkind !== undefined) ADV.dsdKind = kv.dsdkind || 'fir';
    if (kv.dsdconv !== undefined) ADV.dsdConv = kv.dsdconv || 'narrow';
    // 元数据补全（P1.5）：metaon/metallm 从设置键读；源开关不在这里读 ——
    // 由 fillMetaProviders 走 /api/providers（同一 conf 的注册表视图，含旧键迁移）
    if (kv.metaon !== undefined) META.on = (kv.metaon === '1') ? 1 : 0;
    if (kv.metallm !== undefined) META.llm = (kv.metallm === '1') ? 1 : 0;
    // 主题：本地优先（避免闪一下），否则用服务端保存的
    var t = null;
    try { t = localStorage.getItem('rta-theme'); } catch (e) { }
    setTheme(t || kv.theme || 'dark', false);
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

function loadDeviceList() {
  var bk = ADV.backend || 'wasapi';
  var url = (bk === 'asio') ? '/api/asiodevices' : '/api/devices';
  api(url).then(function (j) {
    var sel2 = D('sp_device'); if (!sel2) return;
    var h = '<option value="">' +
      (bk === 'asio' ? '（第一个可用驱动）'
                     : (bk === 'wasapi' || bk === 'wasapi-shared' ? '默认设备' : '（此后端没有本地设备）')) +
      '</option>';
    var list = j.devices || j.drivers || [];
    list.forEach(function (d) {
      h += '<option value="' + esc(d.id) + '">' + esc(d.name) + (d.default ? '（默认）' : '') + '</option>';
    });
    sel2.innerHTML = h;
    sel2.value = ADV.device || '';
    sel2.onchange = function () {
      ADV.device = this.value;
      if (bk === 'wasapi') loadRates();   // 只有 WASAPI 能查设备支持的采样率
      saveSettings();
      toast('输出设备已选择，下次播放生效');
    };
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
function loadLibStats() {
  api('/api/lib/stats').then(function (j) {
    S.scanning = !!j.scanning;
    S.roots = j.roots || [];
    if (S.scanning) {
      D('scanbar').style.display = '';
      var pct = j.total > 0 ? Math.min(100, j.scanned / j.total * 100) : 0;
      D('scanfill').style.width = pct + '%';
      D('cnt').textContent = '扫描中 ' + j.scanned + '/' + j.total;
    } else {
      D('scanbar').style.display = 'none';
      if (j.tracks === 0 && S.view === 'albums' && !D('list').children.length) loadAlbums('');
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
    if (j.err && !(j.items || []).length) { D('list').innerHTML = '<div class=empty>' + esc(j.err) + '</div>'; head('流媒体', '', ''); return; }
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

function loadSrcConfig() {
  if (!D('sKind')) return;    // 这些控件现在渲染在「高级」页里，没渲染就不取
  api('/api/src/config').then(function (j) {
    if (j.kind) D('sKind').value = j.kind;
    if (j.base) D('sBase').value = j.base;
    if (j.user) D('sUser').value = j.user;
  }).catch(function () { });
}
function saveSrc() {
  var body = 'kind=' + encodeURIComponent(D('sKind').value) + '&base=' + encodeURIComponent(D('sBase').value.trim()) +
             '&user=' + encodeURIComponent(D('sUser').value.trim()) + '&pass=' + encodeURIComponent(D('sPass').value);
  api('/api/src/config', body).then(function () { return api('/api/src/ping'); }).then(function (j) {
    D('srcMsg').textContent = j.ok ? '\u2713 已连接' : ('\u2717 ' + (j.error || '失败'));
    D('srcMsg').style.color = j.ok ? '#34d399' : '#e8654b';
    if (j.ok) toast('流媒体服务器已连接');
  }).catch(function (e) { D('srcMsg').textContent = '\u2717 ' + e; });
}

/* ---------------- 视图切换 ---------------- */
function setView(v) {
  S.view = v;
  Array.prototype.forEach.call(document.querySelectorAll('.nav'), function (e) {
    e.classList.toggle('on', e.dataset.v === v || (v === 'artist' && e.dataset.v === 'artists'));
  });
  var isSet = (v === 'settings');
  // 设置页占满整屏：右栏（队列/歌词）在这里没有意义，直接藏掉。
  // 走 #app.noq 这套已有的网格规则，否则网格会留一个空列。
  D('app').classList.toggle('noq', isSet || S.queueHidden);
  D('app').classList.toggle('setmode', isSet);   // 设置页隐藏 crumb，页签自己吸顶
  applyRTab(isSet);
  if (D('setpage')) D('setpage').classList.toggle('on', isSet);
  if (D('list')) D('list').style.display = isSet ? 'none' : '';
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
  };
});

/* ---------------- 搜索（全局） ---------------- */
var searchTimer = 0;
D('q').oninput = function () {
  var v = this.value.trim();
  clearTimeout(searchTimer);
  searchTimer = setTimeout(function () {
    if (v) loadTracks(v);
    else if (S.view === 'tracks') loadAlbums('');
  }, 260);
};
D('q').onkeydown = function (e) {
  if (e.key === 'Escape') { this.value = ''; loadAlbums(''); }
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
// 整页里的播放控件：直接复用底栏那套动作，避免两处逻辑漂移
D('npPrev').onclick = function () { api('/api/skip?dir=-1', '').then(loadQ); };
D('npNext').onclick = function () { api('/api/skip?dir=1', '').then(loadQ); };
D('npPlay').onclick = playOrPause;
D('npStop').onclick = function () { api('/api/stop', '').then(loadQ); };
// ---- 循环方式：整页按钮与底栏 #bRep、设置页 #sp_rep 共用 #sRep 的值（唯一真相源）----
// 为什么不自己存一份：历史上"两处各有一份"必然不一致（HANDOVER §4.5 的规矩）。
function applyRep(v) {
  v = parseInt(v, 10) || 0;
  ['repOne', 'npRepOne'].forEach(function (id) {
    var e = D(id); if (e) e.style.display = (v === 2) ? '' : 'none';
  });
  ['bRep', 'npRep'].forEach(function (id) {
    var e = D(id); if (e) e.style.color = v ? 'var(--acc-text)' : '';
  });
}
function cycleRep() {
  var cur = parseInt(D('sRep').value, 10) || 0;
  var next = (cur + 1) % 3;
  D('sRep').value = String(next);
  api('/api/repeat', 'rate=' + next);
  applyRep(next);
  var sp = D('sp_rep');            // 设置页渲染中时一起同步（它只是同一值的镜像）
  if (sp) sp.value = String(next);
  toast(['循环：关', '循环：列表', '循环：单曲'][next]);
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
  var f = (ev.clientX - r.left) / Math.max(1, r.width);
  f = Math.max(0, Math.min(0.9999, f));
  api('/api/seek?pos=' + f.toFixed(4), '');
};
D('tq').onclick = function () {
  S.queueHidden = !S.queueHidden;
  D('app').classList.toggle('noq', S.queueHidden || S.view === 'settings');
};
D('sRep').onchange = function () { api('/api/repeat', 'rate=' + this.value); };
D('sShuf').onchange = function () { api('/api/shuffle?on=' + (this.checked ? 1 : 0), '').then(loadQ); };
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
if (D('srcSave')) D('srcSave').onclick = saveSrc;   // 只在设置页里存在
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
  var f = (e.clientX - r.left) / r.width;
  if (f < 0) f = 0; if (f > 1) f = 1;
  api('/api/seek?pos=' + f.toFixed(4), '');
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

function seekRel(d) {
  if (!S.dur || S.dur <= 0) return;
  var f = (S.pos + d) / S.dur;
  if (f < 0) f = 0; if (f > 0.9999) f = 0.9999;
  api('/api/seek?pos=' + f.toFixed(4), '');
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
  if (t === 'l') { loadLyrics(S.lastCur); }
}
Array.prototype.forEach.call(document.querySelectorAll('.rtab'), function (e) {
  e.onclick = function () { setRTab(e.dataset.t); };
});

/* ---------------- 歌词（LRC）---------------- */
var LY = { lines: [], at: -1, raw: '' };
function parseLrc(s) {
  var out = [];
  if (!s) return out;
  var rows = String(s).split(/\r?\n/);
  for (var i = 0; i < rows.length; i++) {
    var m = rows[i].match(/\[(\d{1,3}):(\d{1,2})(?:[.:](\d{1,3}))?\]/g);
    if (!m) continue;
    var text = rows[i].replace(/\[[^\]]*\]/g, '').trim();
    if (!text) continue;
    for (var k = 0; k < m.length; k++) {
      var t = m[k].match(/\[(\d{1,3}):(\d{1,2})(?:[.:](\d{1,3}))?\]/);
      var mm = parseInt(t[1], 10), ss = parseInt(t[2], 10), xx = t[3] ? parseInt(t[3], 10) : 0;
      if (t[3] && t[3].length === 2) xx *= 10;
      if (t[3] && t[3].length === 1) xx *= 100;
      out.push({ t: mm * 60 + ss + xx / 1000, s: text });
    }
  }
  out.sort(function (a, b) { return a.t - b.t; });
  // 纯文本歌词（没有时间轴）也接受
  if (!out.length) {
    rows.forEach(function (r) {
      r = r.trim();
      if (r && r.charAt(0) !== '[') out.push({ t: -1, s: r });
    });
  }
  return out;
}
function loadLyrics(file) {
  if (!file) { D('lyricbox').innerHTML = '<div class="ly none">还没有播放曲目</div>'; return; }
  if (/^https?:/.test(file)) { D('lyricbox').innerHTML = '<div class="ly none">网络曲目暂无歌词</div>'; return; }
  api('/api/lyrics?file=' + encodeURIComponent(file)).then(function (j) {
    LY.lines = j.ok ? parseLrc(j.lrc) : [];
    LY.at = -1;
    D('lyricbox').innerHTML = LY.lines.length
      ? LY.lines.map(function (l, i) {
          return '<div class="ly" data-i="' + i + '">' + esc(l.s) + '</div>';
        }).join('')
      : '<div class="ly none">没有歌词<br><span style="font-size:11px">同名 .lrc 文件、内嵌歌词，或播放时自动在线补全（可在设置-元数据补全关闭）</span></div>';
    Array.prototype.forEach.call(D('lyricbox').children, function (e) {
      if (e.dataset.i === undefined) return;
      e.onclick = function () {
        var l = LY.lines[+e.dataset.i];
        if (l.t >= 0 && S.dur > 0) api('/api/seek?pos=' + (l.t / S.dur).toFixed(4), '');
      };
    });
    syncLyrics(S.pos);
    if (D('npPage') && D('npPage').classList.contains('on')) renderNpLyric();
  }).catch(function () { D('lyricbox').innerHTML = '<div class="ly none">歌词加载失败</div>'; });
}
function syncLyrics(pos) {
  if (!LY.lines.length || LY.lines[0].t < 0) return;
  var idx = -1;
  for (var i = 0; i < LY.lines.length; i++) { if (LY.lines[i].t <= pos + 0.05) idx = i; else break; }
  if (idx === LY.at) return;
  LY.at = idx;
  Array.prototype.forEach.call(D('lyricbox').children, function (e, i) {
    if (e.dataset.i === undefined) return;
    e.classList.toggle('on', i === idx);
  });
  var el = D('lyricbox').children[idx];
  if (el && RTAB === 'l') {
    var box = D('lyricbox');
    box.scrollTop = el.offsetTop - box.clientHeight / 2 + el.clientHeight / 2;
  }
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
    nodes.push({ k: '源文件', v: nam(p.src) + (p.codec ? '  \u00b7  ' + p.codec.toUpperCase() : '') });
    nodes.push({ k: '解码', v: (p.note || '原生无损解码') + '  \u00b7  ' + p.inRate + ' Hz / ' + p.inBits + ' bit / ' + p.inCh + ' ch' });
    if (p.resample) nodes.push({ k: '采样率转换', v: (p.filter || 'Kaiser sinc') + '  \u00b7  ' + p.phase + '  \u00b7  ' + p.taps + ' 抽头' + (p.twoStage ? '  \u00b7  两段式' : '') + '  \u2192  ' + p.outRate + ' Hz', gpu: true });
    else nodes.push({ k: '采样率转换', v: '旁路（源采样率与输出一致，逐位直通）' });
    if (p.dsd) {
      nodes.push({ k: 'DSD 调制', v: 'DSD' + p.dsd + '（' + (p.dsdRate / 1000000).toFixed(4) + ' MHz）\u00b7 1-bit \u00b7 ' + (p.modOrder || 5) + ' 阶 SDM' });
      nodes.push({ k: 'DoP 封装', v: '24-bit 载波 ' + p.outRate + ' Hz \u00b7 标记 0x05/0xFA 交替' });
    } else {
      nodes.push({ k: '抖动 / 噪声整形', v: (p.shaper && p.shaper !== 'none') ? p.shaper : '无（逐位）' });
    }
    if (p.preAtten && p.preAtten < 0) {
      nodes.push({ k: '峰值保护', v: '检测到过采样峰值 ' + (p.ispPeak * 100).toFixed(1) + '%（' + p.ispOver +
        ' 个点超出满幅）→ 预衰减 ' + p.preAtten.toFixed(1) + ' dB' });
    }
    if (p.loudness) nodes.push({ k: '等响度补偿', v: '开启' });
    nodes.push({ k: '音量', v: volText(p.volume), id: 'chainVol' });
    nodes.push({ k: '输出', v: p.device + '  \u00b7  ' + p.outRate + ' Hz / ' + p.outBits + ' bit / ' + p.outCh +
      ' ch  \u00b7  ' + ((ADV.backend === 'wasapi-shared') ? '共享模式（非 bit-perfect）' : '独占模式') +
      '  \u00b7  缓冲 ' + p.buffer + ' ms' });
    var h = '<div class=chain>';
    nodes.forEach(function (n, i) {
      h += '<div class="cnode' + (n.gpu ? ' gpu' : '') + '"><div class=dot></div><div><div class=k>' + esc(n.k) + '</div>' +
           '<div class=v' + (n.id ? ' id=' + n.id : '') + '>' + esc(n.v) + '</div></div></div>';
      if (i !== nodes.length - 1) h += '<div class=cline></div>';
    });
    h += '</div>';
    h += '<div style="margin-top:14px;font-size:11px;color:#5b6478;line-height:1.7">绿色节点表示由 GPU 参与计算。<br>只要每一环都不做多余处理，输出就是逐位（bit-perfect）的。</div>';
    D('mbody').innerHTML = h;
  }).catch(function () { D('mbody').innerHTML = '<div class=empty>读取失败</div>'; });
}


/* ---------------- 完整设置页 ---------------- */
var SET_GROUPS = null;
function opt(v, label, cur) {
  return '<option value="' + esc(v) + '"' + (String(cur) === String(v) ? ' selected' : '') + '>' + esc(label) + '</option>';
}
function num(id, val, min, max, step, hint) {
  return '<div class=sfld><label>' + id.label + '</label><input type=number id=' + id.id +
         ' value="' + val + '" min=' + min + ' max=' + max + ' step=' + step + '>' +
         (hint ? '<div class=hint>' + hint + '</div>' : '') + '</div>';
}
function sel(id, label, optsHtml, hint) {
  return '<div class=sfld><label>' + label + '</label><select id=' + id + '>' + optsHtml + '</select>' +
         (hint ? '<div class=hint>' + hint + '</div>' : '') + '</div>';
}
function chk(id, label, checked, hint) {
  return '<div class=sfld><label class=chk><input type=checkbox id=' + id + (checked ? ' checked' : '') + '>' +
         label + '</label>' + (hint ? '<div class=hint>' + hint + '</div>' : '') + '</div>';
}
function txt(id, label, val, hint, ph) {
  return '<div class="sfld wide"><label>' + label + '</label><input type=text id=' + id + ' value="' + esc(val || '') +
         '" placeholder="' + esc(ph || '') + '">' + (hint ? '<div class=hint>' + hint + '</div>' : '') + '</div>';
}

/* ---------------- 输出后端 / 输入设备 ---------------- */
var ASIO_DRIVERS = null;   // 由 /api/asio 填充
function backendHint() {
  var m = D('sp_backendmsg'); if (!m) return;
  var bk = (D('sp_backend') && D('sp_backend').value) || 'wasapi';
  if (bk === 'wasapi') {
    m.textContent = 'WASAPI 独占 · 事件驱动：本机可用。多声道走 WAVEFORMATEXTENSIBLE 的声道掩码。';
  } else if (bk === 'wasapi-shared') {
    m.textContent = 'WASAPI 共享：兼容性最好（虚拟机/远程桌面）；经系统混音，非 bit-perfect。' +
      '采样率/声道由系统混音格式决定；不支持 DoP/DSD 输出；独占流无响应时会自动回退到它。';
  } else if (bk === 'null') {
    m.textContent = 'Null output：照常跑完整 DSP，数据直接丢弃、不出声，用于离线验证与频谱分析。';
  } else if (bk === 'naa') {
    m.textContent = 'NAA：把 PCM 按 RTANet 协议推到端点 ' +
      (((D('sp_naahost') && D('sp_naahost').value) || '')) + '（端点机需运行 rtanet_rx）。';
  } else if (bk === 'asio') {
    if (ASIO_DRIVERS === null) m.textContent = 'ASIO：正在检测本机驱动…';
    else if (!ASIO_DRIVERS.length) m.textContent = 'ASIO：本机未检测到 ASIO 驱动。';
    else m.textContent = 'ASIO：本机驱动 ' + ASIO_DRIVERS.join('、') + '。';
  }
}

function loadInputDevices() {
  api('/api/indevices').then(function (j) {
    var s = D('sp_indev'); if (!s) return;
    var hh = '<option value="">（未选择）</option>';
    (j.devices || []).forEach(function (d) { hh += '<option value="' + esc(d.id) + '">' + esc(d.name) + '</option>'; });
    s.innerHTML = hh;
    s.value = ADV.inDev || '';
    s.onchange = function () { ADV.inDev = this.value; saveSettings(); };
    if (D('sp_inmsg')) {
      D('sp_inmsg').textContent = (j.devices || []).length ? '' : '本机没有可用的采集设备';
    }
  }).catch(function () {
    if (D('sp_inmsg')) D('sp_inmsg').textContent = '输入设备列表读取失败';
  });
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
function applyDsdState() {
  var isDsd = D('sp_outmode') && D('sp_outmode').value === 'dsd';
  ['sp_dsdrate', 'sp_dsdpk'].forEach(function (id) {
    var e = D(id);
    if (e) { e.disabled = !isDsd; e.style.opacity = isDsd ? '1' : '.5'; }
  });
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
  return '<div class=sgroup><div class=sghead>' + title +
         (hint ? '<span class=hint>' + hint + '</span>' : '') +
         '</div><div class=sgbody>' + body + '</div></div>';
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
  if (!fname) { msg.textContent = '先在上面选一个 1x 滤波器'; return; }
  var it = FILTER_PHASES ? FILTER_PHASES[fname] : null;
  if (!it) {
    // 相位表还没回来（或该滤波器不在表里）：只说清"预设"是什么，不编造具体相位
    msg.textContent = '「预设」=「' + fname + '」这个滤波器自带的相位（相位表读取中…）';
    return;
  }
  var actual = override
    ? ({ linear: '线性相位', minimum: '最小相位', intermediate: '混合相位' }[override] || override)
    : it.phaseName;
  msg.textContent = '当前 1x 滤波器「' + fname + '」的预设相位 = ' + it.phaseName +
    '（抽头 ' + it.taps + '）。' +
    (override ? ('你选了强制覆盖 ⟹ 实际按「' + actual + '」跑。')
              : ('选「跟随预设」⟹ 实际就按「' + actual + '」跑（不覆盖）。'));
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

function renderSettingsPage() {
  var filts = D('sFilt') ? D('sFilt').innerHTML : '';
  // Nx 下拉必须带上"跟随 1x"（空值），否则重建后浏览器退回第一项，一保存就把 Nx 改掉
  var filtsNx = '<option value="">跟随 1x（不单独指定）</option>' + filts;
  var shaps = D('sShap') ? D('sShap').innerHTML : '';
  var BK = ADV.backend || 'wasapi';
  var CH = String(ADV.channels || 0);
  var h = '';

  // ---------- 页签（对齐 HQPlayer：Inputs / Outputs / PCM / SDM / Advanced） ----------
  h += '<div class=stabs>'
     + '<button class="stab on" data-g=in>输入</button>'
     + '<button class=stab data-g=out>输出</button>'
     + '<button class=stab data-g=pcm>PCM</button>'
     + '<button class=stab data-g=sdm>SDM</button>'
     + '<button class=stab data-g=adv>高级</button>'
     + '<button class=stab data-g=meta>元数据</button>'
     // 「诊断」独立成页：用户 2026-10-22 功能3 明确要求不要塞在元数据里。
     // 开发期它是必需品（报错时要拿现场），所以与输入/输出/PCM/SDM/高级 平级。
     + '<button class=stab data-g=diag>诊断</button>'
     + '</div>';

  // ---------- 输出 ----------
  h += '<div class="spane" data-g=out>'
     + sgroup('输出后端', '独占 = bit-perfect；共享 = 兼容性最好（虚拟机 / 远程桌面）',
         sel('sp_backend', '后端',
             opt('wasapi', 'WASAPI（独占 · 事件驱动）', BK) +
             opt('wasapi-shared', 'WASAPI 共享（兼容虚拟机 / 远程桌面）', BK) +
             opt('asio', 'ASIO', BK) +
             opt('naa', 'NetworkAudioAdapter（NAA / RTANet）', BK) + opt('null', 'Null output（只跑 DSP，不出声）', BK),
             '选到当前不可用的后端时，播放会直接返回原因，不会假装出声')
       + '<div class=sfld><label>NAA 端点（host:port）</label><input type=text id=sp_naahost value="' +
           esc(ADV.naaHost || '127.0.0.1:43210') + '"><div class=hint>只在后端选 NAA 时使用</div></div>'
       + sel('sp_device', '输出设备', '<option value="">默认设备</option>', '独占模式会接管这个设备；共享模式经系统混音，不接管')
       + sel('sp_outmode', '输出模式',
             opt('pcm', 'PCM', outModeOf()) + opt('dsd', 'DSD（SDM）', outModeOf()),
             'DSD 的比特率与封装（DoP / Native）在 SDM 页设置')
       + sel('sp_rate', '输出采样率', D('sRate').innerHTML, '自动 = 所选设备的最高采样率')
       + sel('sp_bits', '输出位深（PCM）',
             opt(0, '24 bit', ADV.bits) + opt(16, '16 bit', ADV.bits) + opt(32, '32 bit', ADV.bits),
             'DSD 输出恒为 1 bit，此项只在 PCM 模式生效')
       + sel('sp_ch', '输出声道数',
             opt('0', '跟随源声道数', CH) + opt('2', '2 声道（立体声）', CH) +
             opt('6', '5.1（6 声道）', CH) + opt('8', '7.1（8 声道）', CH),
             '设备必须支持该声道数；多声道用 WAVEFORMATEXTENSIBLE 的声道掩码声明布局')
       + num({ id: 'sp_buffer', label: '缓冲（毫秒）' }, D('sBuf').value, 10, 500, 10, '越小延迟越低，但更容易欠载')
       + num({ id: 'sp_ramp', label: '音量渐变（毫秒）' }, ADV.ramp, 0, 2000, 10, '0 = 立即生效（可能爆音）')
       + '<div class="sfld wide"><div id=sp_backendmsg class=hint></div></div>')
     + sgroup('声道音量微调', '按声道名标注，0 dB = 不动',
         '<div class="sfld wide"><div class=trimgrid>'+ trimFields(CH) + '</div>' +
         '<div class=hint>显示的是「输出声道数」对应的实际声道。2 = 左/右；5.1 = 左/右/中置/低音/左环绕/右环绕；7.1 再加左后/右后</div></div>')
     + '</div>';

  // ---------- PCM ----------
  h += '<div class=spane data-g=pcm>'
     + sgroup('升频与滤波', 'HQPlayer 的 poly-sinc 家族',
         sel('sp_filt', '1x 滤波器（44.1 / 48 kHz 音源）', filts, '播放 44.1 / 48 kHz 文件时使用')
       + sel('sp_filtNx', 'Nx 滤波器（88.2 kHz 及以上音源）', filtsNx,
             '播放 88.2 / 96 / 176.4 kHz 等文件时使用；留空 = 与 1x 相同')
       + sel('sp_phase', '相位（覆盖滤波器预设的相位）',
             opt('', '跟随预设（用所选滤波器自带的相位）', ADV.phase) +
             opt('linear', '强制线性相位', ADV.phase) +
             opt('minimum', '强制最小相位', ADV.phase) +
             opt('intermediate', '强制混合相位（线性/最小 各 50%）', ADV.phase),
             '「预设」= 上面 1x / Nx 滤波器自带的相位（手册里 -lp 是线性、-mp 是最小）。' +
             '选「跟随预设」就不覆盖。强制项会覆盖预设：' +
             '线性 = 无相位失真、有前振铃；最小 = 无预振铃、群延迟低；' +
             '混合 = 两者冲激响应按 50:50 叠加（幅频不变）')
       + '<div class="sfld wide"><div class=hint id=sp_phasemsg></div></div>'
       + num({ id: 'sp_taps', label: '原型抽头数（0 = 按预设自动）' }, ADV.taps, 0, 2097152, 1024,
             '抽头数 = 原型低通滤波器的系数个数，也就是滤波器长度。多相分解后每一相的抽头数 ≈ 抽头数 ÷ 升频比，' +
             '每输出一个采样要做这么多乘加。0 表示不覆盖，直接用所选滤波器预设自带的长度')
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
       + sel('sp_dsdrate', '比特率（/Limit）',
             opt('64', 'DSD64（2.8224 MHz）', dsdRateOf()) + opt('128', 'DSD128（5.6448 MHz）', dsdRateOf()) +
             opt('256', 'DSD256（11.2896 MHz）', dsdRateOf()) + opt('512', 'DSD512（22.5792 MHz）', dsdRateOf()) +
             opt('1024', 'DSD1024（45.1584 MHz）', dsdRateOf()),
             'DSD512 / 1024 的 DoP 载波是 1411.2 / 2822.4 kHz，绝大多数设备不支持')
       + sel('sp_dsdpk', '封装',
             opt('dop', 'DoP（封在 24-bit PCM 容器里，WASAPI / ASIO 都行）', dsdPackOf()) +
             opt('nat', 'Native（直接用 DSD 采样率，只能走 ASIO）', dsdPackOf()),
             '两者数值等价，差别只在传输方式')
       + sel('sp_modorder', '调制器阶数',
             opt(5, '5 阶（DSD64 起）', ADV.modorder || 5) + opt(7, '7 阶（需 DSD128 以上）', ADV.modorder || 5),
             '实测 7 阶在 DSD64 会饱和，程序会自动降为 5 阶')
       + num({ id: 'sp_modcut', label: '噪声整形拐点（kHz，0 = 100）' }, ADV.modcut, 0, 200, 5)
       + num({ id: 'sp_moddith', label: '调制器抖动（LSB，-1 = 默认）' }, ADV.moddith, -1, 1, 0.001, '抑制空闲音'))
     + sgroup('SDM 源（DSD 文件）', '原生读取 + DirectSDM',
         chk('sp_directsdm', 'DirectSDM：DSD 源同速率直通（逐位一致）', ADV.directsdm == 1,
             '源 DSD 速率 == 目标速率、音量 0 dB、无 EQ / 卷积 / 声道改动时，1-bit 流直接装进 DoP，' +
             '不再「DSD 进 → PCM 中转 → 重新调制」。条件不满足会自动回退普通路径，并在信号链里写明原因。') +
         '<div class="sfld wide"><div class=hint>DSD 文件（.dsf / .dff）现在由播放器原生读取，' +
         '读取失败才回退 ffmpeg。速率不同（例如 DSD64 源 → DSD256 输出）时仍走「抽取成 PCM → 升频 → 重新调制」。</div></div>' +
         sel('sp_dsdkind', 'DSD 源抽取器',
             opt('fir', 'FIR（Kaiser 120 dB，默认）', ADV.dsdKind || 'fir') +
             opt('iir', 'IIR（Butterworth 10 阶，相位非线性）', ADV.dsdKind || 'fir') +
             opt('cic', 'CIC（6 级无乘法 + 补偿）', ADV.dsdKind || 'fir'),
             '只在「DSD 源 → PCM」路径生效；DirectSDM 直通时整条 PCM 链都被跳过。实测 THD+N 三者都约 99 dB') +
         sel('sp_dsdconv', 'DSD 源 Conversion',
             opt('narrow', 'narrow（通带 20 kHz，默认）', ADV.dsdConv || 'narrow') +
             opt('wide', 'wide（通带 30 kHz）', ADV.dsdConv || 'narrow') +
             opt('xfi', 'XFi（通带 40 kHz）', ADV.dsdConv || 'narrow'),
             '通带越宽，阻带起点（outRate − 通带）越靠近，过渡带越窄、抽头越多；三组都是真参数，测试里有数值表') +
         '<div class="sfld wide"><div class=hint id=sp_dsdmsg></div></div>')
     + '</div>';

  // ---------- 输入 ----------
  h += '<div class="spane on" data-g=in>'
     + sgroup('输入设备', 'WASAPI 采集',
         sel('sp_inbackend', '输入后端', opt('none', '[none]', ADV.inBackend || 'none') + opt('wasapi', 'WASAPI', ADV.inBackend || 'none'),
             '选 WASAPI 后可以采集线路输入 / 麦克风 / 系统回环')
       + sel('sp_indev', '输入设备', '<option value="">（未选择）</option>', '')
       + sel('sp_inch', '声道偏移', opt('0', '0', ADV.inOffset || 0) + opt('1', '1', ADV.inOffset || 0) +
             opt('2', '2', ADV.inOffset || 0) + opt('4', '4', ADV.inOffset || 0),
             '多声道接口只取其中几路时用')
       + num({ id: 'sp_inbuf', label: '输入缓冲（毫秒）' }, ADV.inBuf || 100, 10, 500, 10, '')
       + chk('sp_inmon', '采集后送 DSP 输出（监听）', ADV.inMonitor == 1, '')
       + '<div class="sfld wide"><div id=sp_inmsg class=hint></div></div>')
     + '</div>';

  // ---------- 高级 ----------
  h += '<div class=spane data-g=adv>'
     + sgroup('外观', '主题会记住',
         '<div class=sfld><label>主题</label><select id=sp_theme>'
       + opt('dark', '深色（默认）', S.theme) + opt('light', '白色', S.theme) + opt('blue', '浅蓝色', S.theme)
       + '</select><div class=hint>顶栏右侧的小圆点也可以快速切换</div></div>')
     + sgroup('GPU',
         sel('sp_gpu', 'GPU 后端',
             opt('auto', '自动', ADV.gpuvendor || 'auto') + opt('amd', 'AMD（HIP）', ADV.gpuvendor || 'auto') +
             opt('off', '关闭（纯 CPU）', ADV.gpuvendor || 'auto'),
             '本版本只提供 AMD（HIP）后端；没有可用 GPU/运行库时自动退回 CPU，并在信号链里写明。NVIDIA CUDA 后端尚未提供')
       + '<div class="sfld wide"><div id=sp_gpumsg class=hint></div></div>'
       + '<div class="sfld wide"><div id=sp_asiomsg class=hint></div></div>')
     + sgroup('EQ 与房间校正', 'FIR 用 WAV，EQ 用 REW / AutoEq 文本',
         txt('sp_eq', 'EQ 文件路径（REW / AutoEq 文本）', ADV.eq,
             '每行形如「Filter 1: ON PK Fc 1000 Hz Gain -3.0 dB Q 1.41」，或用 AutoEq 的 Preamp / Low Shelf / Peaking 段落',
             'D:\Music\headphone-eq.txt')
       + txt('sp_ir', 'IR 文件路径（WAV）', ADV.ir, '留空 = 不做卷积。路径形如 D:\Music\room.wav', 'D:\Music\room.wav')
       + chk('sp_irgpu', '卷积走 GPU', ADV.irgpu == 1, '需要 rtagpu.dll'))
     + sgroup('流媒体服务器', 'Navidrome / Subsonic / Emby（只取原始文件，不用转码流）',
         '<div class="sfld wide"><div class=hint>DLNA/UPnP 是"发现网络上的播放器"，与这里的"登录音乐服务器"是两件事。' +
         '要连 Emby / Navidrome，在这一栏填地址与账号。</div></div>'
       + '<div class=sfld><label>类型</label><select id=sKind><option value=subsonic>Navidrome / Subsonic</option><option value=emby>Emby</option></select></div>'
       + '<div class=sfld><label>服务器地址</label><input type=text id=sBase placeholder="http://192.168.1.10:4533"></div>'
       + '<div class=sfld><label>用户名</label><input type=text id=sUser placeholder="用户名"></div>'
       + '<div class=sfld><label>密码</label><input type=password id=sPass placeholder="密码"></div>'
       + '<div class="sfld wide"><div class=hint id=srcMsg></div></div>'
       + '<div class="sfld wide"><button class="btn pri" id=srcSave>保存并测试连接</button></div>')
     + sgroup('音乐库与网络', '',
         txt('sp_root', '添加音乐目录', '', '填好后点下面的按钮', 'D:\Music')
       + '<div class="sfld wide"><button class="btn" id=sp_addroot>添加目录</button> <button class="btn" id=sp_rescan>重新扫描全部</button> <button class="btn" id=sp_upnp>发现 DLNA 设备</button></div>'
       + '<div class="sfld wide"><div id=sp_roots class=rootlist></div></div>'
       + '<div class="sfld wide"><div id=sp_upnpmsg style="font-size:11px;color:var(--mut2)"></div></div>')
     + sgroup('播放', '',
         sel('sp_rep', '循环模式', D('sRep').innerHTML, '')
       + chk('sp_shuf', '随机播放', D('sShuf').checked, '关掉会恢复原顺序')
       + chk('sp_loud', '等响度补偿', D('sLoud').checked, '音量越低低频提升越多'))
     + '</div>';

  // ---------- 诊断（用户 2026-10-22 功能3：独立页签）----------
  // 用户原话：「导出诊断信息其实就是一个 log，最好是直接导出一个压缩包或一个整体的
  // 文件，不要用户单独找 rlplayer-diag.json / rlplayer-playback.log / launcher.log
  // 三份文件；另外这个选项放在元数据这里明显不合适，单独出来做选项卡吧，
  // 我们还在开发，报错是必不可少的。」
  // 所以：① 独立页签；② 一个 ZIP 打走全部；③ 先把包内容列清楚再给按钮。
  h += '<div class=spane data-g=diag>'
     + sgroup('导出诊断包', '一个文件，不用再到处找日志',
         '<div class="sfld wide"><div style="font-size:12px;color:var(--mut);line-height:1.8">'
       + '点下面的按钮，会生成一个 <b>rlplayer-diag-日期时间.zip</b>，里面已经装好：'
       + '<br>· rlplayer-diag.json —— 版本 / 系统 / CPU / GPU / 端口'
       + '<br>· rlplayer-playback.log（含 .1 / .2 轮转）—— 播放格式、峰值、欠载、失败原因'
       + '<br>· launcher.log —— 启动器日志（启动横幅、主题、子进程）'
       + '<br>· dumps-manifest.txt —— 崩溃转储清单（dump 本体不打包，体积太大）'
       + '<br>· settings-summary.txt —— 设置摘要（仅白名单键）'
       + '<br><span style="color:var(--mut2)">已脱敏：api_key / token / password 的值一律替换为 ***；'
       + '配置文件本体（rlplayer-llm.conf）不会被打进包。</span>'
       + '</div></div>'
       + '<div class="sfld wide"><button class="btn pri" id=sp_diagzip>导出诊断包（.zip）</button> '
       + '<button class=btn id=sp_diagjson>只要 JSON</button>'
       + '<span style="font-size:11.5px;color:var(--mut2);margin-left:10px" id=sp_diagmsg></span></div>')
     + '</div>';

  // ---------- 元数据补全（排在「高级」之后）----------
  // P1.5：源开关不再硬编码 —— #sp_metasrc 占位容器由 fillMetaProviders 按
  // /api/providers 动态填充（注册表驱动，后端新增数据源前端零改动）。
  h += '<div class=spane data-g=meta>'
     + sgroup('元数据补全', '播放时后台补全标签 / 封面 / 歌词，结果落在本地库',
         chk('sp_metaon', '自动补全（总开关）', META.on == 1,
             '关闭后播放不再触发在线补全；已补全的结果仍在本地库，随时可以再打开')
       + '<div id=sp_metasrc>' + metaSrcHtml() + '</div>'
       + chk('sp_metallm', 'DeepSeek 增强', META.llm == 1, '无标签文件用 LLM 解析与裁决；不影响已有标签的优先级')
       // ---- 功能4（用户 2026-10-22）：「元数据没有提供 ds 的 api 输入接口」----
       // 红线：界面【只显示掩码】，输入框永不回填，保存后立刻清空；
       // key 只写 exe 同目录 rlplayer-llm.conf（路径由后端决定，前端不碰文件）。
       + '<div class=sfld><label>DeepSeek API Key</label>'
       + '<input type=password id=sp_llmkey autocomplete=off spellcheck=false placeholder="sk-…（粘贴后点保存，只显示掩码）">'
       + '<div class=hint>只存本机 build' + SEP + 'rlplayer-llm.conf；不进安装包，界面永不回显明文</div></div>'
       + '<div class="sfld wide"><div class=hint id=sp_llmmsg>读取配置中…</div></div>'
       + '<div class="sfld wide"><button class="btn pri" id=sp_llmsave>保存 Key</button> '
       + '<button class=btn id=sp_llmtest>测试连接</button> '
       + '<button class=btn id=sp_llmclear>清除 Key</button></div>'
       + '<div class="sfld wide"><div style="font-size:12px;color:var(--mut);line-height:1.7">'
       + '播放时自动补全标签/封面/歌词；数据源清单来自 /api/providers；DeepSeek 用上面填的 key</div></div>'
       // P2 标签写回：预览（只读扫描）/ 执行（后台批 + 轮询进度）。写回前服务端做
       // PCM 逐位校验（不一致绝不动原文件），原文件备份到 rlplayer-meta\backups。
       + '<div class="sfld wide"><div id=sp_wbdesc style="font-size:12px;color:var(--mut);line-height:1.7">'
       + '把补全结果写回音频文件标签。写回前自动校验音频数据逐位不变，原文件备份到 rlplayer-meta' + SEP + 'backups</div></div>'
       + '<div class="sfld wide"><button class=btn id=sp_wbdry>预览写回</button> '
       + '<button class=btn id=sp_wbapply>执行写回</button>'
       + '<span id=sp_wbmsg style="font-size:11.5px;color:var(--mut2);margin-left:10px"></span></div>')
     + '</div>';

  // 底部按钮（不随页签切换）
  h += '<div class=setfoot><button class="btn pri" id=sp_save>保存设置</button>'
     + '<button class=btn id=sp_reset>恢复默认</button><span class=sp></span>'
     + '<span style="font-size:11.5px;color:var(--mut2)">改动在下一次播放时生效</span></div>';

  D('setpage').innerHTML = h;

  // P1.5：源开关的绑定不在上面的静态清单里 —— 重建 HTML 后逐源重新绑定
  //（/api/providers 已到则用缓存渲染+绑定；没到则等 fillMetaProviders 到达时补填）。
  renderMetaSrcRows();

  // 设置页里的下拉是复制的快捷面板 HTML，重建后浏览器会退回第一项。
  // 不同步回来，打开设置页再点保存就会把用户的选择悄悄改成第一项。
  [['sp_filt', 'sFilt'], ['sp_filtNx', 'sFiltNx'], ['sp_shaper', 'sShap'],
   ['sp_rate', 'sRate'], ['sp_rep', 'sRep'], ['sp_buffer', 'sBuf'],
   ['sp_shuf', 'sShuf'], ['sp_loud', 'sLoud'], ['sp_isp', 'sIsp'], ['sp_2s', 's2s']].forEach(function (pair) {
    var a = D(pair[0]), b = D(pair[1]);
    if (!a || !b) return;
    if (a.type === 'checkbox') { a.checked = b.checked; return; }
    if (a.tagName === 'SELECT') {
      for (var i = 0; i < a.options.length; i++) {
        if (a.options[i].value === b.value) { a.value = b.value; return; }
      }
    } else { a.value = b.value; }
  });

  // 先把 DSD 三个控件的当前值从隐藏状态同步过来
  if (D('sp_outmode')) D('sp_outmode').value = outModeOf();
  if (D('sp_dsdrate')) D('sp_dsdrate').value = dsdRateOf();
  if (D('sp_dsdpk')) D('sp_dsdpk').value = dsdPackOf();
  if (D('sp_sdmo1')) D('sp_sdmo1').value = ADV.sdmFilt || '';
  if (D('sp_sdmoNx')) D('sp_sdmoNx').value = ADV.sdmFiltNx || '';
  applyDsdState();

  // 页签切换（输入 / 输出 / PCM / SDM / 高级）
  Array.prototype.forEach.call(document.querySelectorAll('#setpage .stab'), function (b) {
    b.onclick = function () {
      Array.prototype.forEach.call(document.querySelectorAll('#setpage .stab'), function (x) {
        x.classList.toggle('on', x === b);
      });
      Array.prototype.forEach.call(document.querySelectorAll('#setpage .spane'), function (p) {
        p.classList.toggle('on', p.dataset.g === b.dataset.g);
      });
    };
  });

  // 输出后端 / 声道 / 微调
  D('sp_backend').onchange = function () {
    ADV.backend = this.value;
    saveSettings();
    backendHint();
    loadDeviceList();        // ASIO 列驱动，WASAPI 列端点，NAA/Null 没有本地设备
  };
  if (D('sp_naahost')) D('sp_naahost').oninput = function () { ADV.naaHost = this.value; saveSettings(); };
  D('sp_ch').onchange = function () {
    ADV.channels = parseInt(this.value, 10) || 0;
    saveSettings();
    var g = document.querySelector('#setpage .trimgrid');
    if (g) { g.innerHTML = trimFields(this.value); bindTrims(); }   // 标签跟着声道布局换
  };
  bindTrims();
  ['sp_outmode', 'sp_dsdrate', 'sp_dsdpk'].forEach(function (id) {
    var e = D(id);
    if (e) e.onchange = function () { applyDsd(); saveSettings(); applyDsdState(); };
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
  // 元数据补全开关（P1.5）：总开关与 DeepSeek 固定绑定；源开关在 renderMetaSrcRows
  // 里按注册表逐源绑定（metasrc_<id>，改完即落盘）
  [['sp_metaon', 'on'], ['sp_metallm', 'llm']].forEach(function (p) {
    var e = D(p[0]); if (!e) return;
    e.onchange = function () { META[p[1]] = this.checked ? 1 : 0; saveSettings(); };
  });
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
  // 功能2：相位说明跟随 1x 滤波器与相位覆盖的选择联动
  ['sp_filt', 'sp_phase', 'sp_filtNx'].forEach(function (id) {
    var e = D(id);
    if (e) e.addEventListener('change', bindPhaseHint);
  });
  bindPhaseHint();
  // P2 标签写回：预览 / 执行按钮绑定，并同步一次在跑状态（中途刷新页面也能恢复禁用）
  if (D('sp_wbdry')) D('sp_wbdry').onclick = wbPreview;
  if (D('sp_wbapply')) D('sp_wbapply').onclick = wbApply;
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
  backendHint();

  // 事件
  loadDeviceList();
  // GPU 后端：把 AMD 标成本机真实状态（哪块卡、后端有没有编出来）
  api('/api/gpu').then(function (j) {
    var sg = D('sp_gpu');
    if (sg) {
      Array.prototype.forEach.call(sg.options, function (o) {
        if (o.value === 'amd') o.text = j.amd ? 'AMD（HIP）· 后端已就位' : 'AMD（HIP）· 本目录无 rtagpu.dll';
      });
    }
    var m = D('sp_gpumsg');
    if (m) m.textContent = (j.available ? '当前 GPU 可用：' : '当前走 CPU：') + (j.status || '') +
      '；本目录 rtagpu.dll=' + (j.amd ? '有' : '无') + '，AMD HIP 运行库=' + (j.hip ? '有' : '无');
  }).catch(function () { });
  // ASIO 驱动列表（Native DSD 需要 ASIO 后端）
  api('/api/asio').then(function (j) {
    var d = j.drivers || [];
    ASIO_DRIVERS = d;
    backendHint();
    var m = D('sp_asiomsg'); if (!m) return;
    m.textContent = d.length
      ? ('本机 ASIO 驱动：' + d.join('、') + '。Native DSD 后端尚未实现：选 Native 时不会出声，只返回未实现的原因')
      : '本机未检测到 ASIO 驱动。Native DSD 只能经 ASIO 输出，当前无可用后端';
  }).catch(function () { });
  if (ADV.inBackend === 'wasapi') loadInputDevices();
  if (D('srcSave')) { D('srcSave').onclick = saveSrc; loadSrcConfig(); }
  D('sp_save').onclick = applySettings;
  D('sp_reset').onclick = function () {
    if (!confirm('把所有参数恢复成默认值？')) return;
    ADV = { device: '', bits: 0, taps: 0, atten: 0, transition: 0, phase: '', modcut: 0, moddith: -1, ir: '', irgpu: 0, ramp: 0, theme: S.theme, modorder: 5, directsdm: 0, dsdKind: 'fir', dsdConv: 'narrow' };
    META = { on: 1, llm: 1 };   // 元数据固定两键的默认；源开关随注册表一并全开
    (META_PROVIDERS || []).forEach(function (p) { META_SRC[p.id] = 1; });
    D('sRate').value = 'max'; D('sFiltNx').value = ''; D('sShap').value = 'auto';
    D('sBuf').value = 100; D('sDSD').value = 0; D('sIsp').checked = true; D('s2s').checked = true;
    saveSettings(); renderSettingsPage(); toast('已恢复默认');
  };
  D('sp_theme').onchange = function () { setTheme(this.value); };
  D('sp_addroot').onclick = function () {
    var d = (D('sp_root').value || '').trim();
    if (!d) { toast('请填目录'); return; }
    api('/api/lib/roots', 'add=' + encodeURIComponent(d)).then(function (j) {
      if (!j.ok) { toast(j.error || '添加失败'); return; }
      toast('已添加，正在扫描'); loadLibStats(); api('/api/lib/scan', '');
    });
  };
  D('sp_rescan').onclick = function () { api('/api/lib/scan', 'full=1').then(function () { toast('开始重新扫描'); loadLibStats(); }); };
  D('sp_upnp').onclick = function () {
    D('sp_upnpmsg').textContent = '正在搜索…';
    api('/api/upnp').then(function (j) {
      var hh = '';
      (j.devices || []).forEach(function (d) { hh += '<div>' + esc(d.name) + ' — ' + esc(d.host) + ':' + esc(d.port) + '</div>'; });
      D('sp_upnpmsg').innerHTML = hh || '没发现 DLNA 渲染器';
    });
  };
  drawRoots();
}
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
      api('/api/lib/roots', 'remove=' + encodeURIComponent(b.dataset.r)).then(function () { loadLibStats(); toast('已移除'); });
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
  ADV.eq = (D('sp_eq').value || '').trim();
  ADV.ir = (D('sp_ir').value || '').trim();
  ADV.irgpu = D('sp_irgpu').checked ? 1 : 0;
  ADV.ramp = parseFloat(D('sp_ramp').value) || 0;
  ADV.gpuvendor = D('sp_gpu').value || 'auto';
  if (D('sGpu')) D('sGpu').value = ADV.gpuvendor;
  // 输出后端 / 声道 / 每声道微调
  if (D('sp_backend')) ADV.backend = D('sp_backend').value || 'wasapi';
  if (D('sp_naahost')) ADV.naaHost = (D('sp_naahost').value || '').trim();
  if (D('sp_ch')) ADV.channels = parseInt(D('sp_ch').value, 10) || 0;
  [0, 1, 2, 3, 4, 5, 6, 7].forEach(function (i) {
    var e = D('sp_trim' + i);
    if (e) ADV['trim' + i] = parseFloat(e.value) || 0;
  });
  if (D('sp_sdmo1')) ADV.sdmFilt = D('sp_sdmo1').value || '';
  if (D('sp_sdmoNx')) ADV.sdmFiltNx = D('sp_sdmoNx').value || '';
  if (D('sp_directsdm')) ADV.directsdm = D('sp_directsdm').checked ? 1 : 0;
  if (D('sp_dsdkind')) ADV.dsdKind = D('sp_dsdkind').value || 'fir';
  if (D('sp_dsdconv')) ADV.dsdConv = D('sp_dsdconv').value || 'narrow';
  applyDsd();
  if (D('sp_inbackend')) ADV.inBackend = D('sp_inbackend').value || 'none';
  if (D('sp_indev')) ADV.inDev = D('sp_indev').value || '';
  if (D('sp_inch')) ADV.inOffset = parseInt(D('sp_inch').value, 10) || 0;
  if (D('sp_inbuf')) ADV.inBuf = parseInt(D('sp_inbuf').value, 10) || 100;
  if (D('sp_inmon')) ADV.inMonitor = D('sp_inmon').checked ? 1 : 0;
  // 同步回快捷面板

  D('sRate').value = D('sp_rate').value;
  D('sFilt').value = D('sp_filt').value;
  D('sFiltNx').value = D('sp_filtNx').value;
  D('sShap').value = D('sp_shaper').value;
  D('sBuf').value = D('sp_buffer').value;
  D('sIsp').checked = D('sp_isp').checked;
  D('s2s').checked = D('sp_2s').checked;
  D('sRep').value = D('sp_rep').value;
  D('sShuf').checked = D('sp_shuf').checked;
  D('sLoud').checked = D('sp_loud').checked;
  S.loud = D('sLoud').checked;
  // 行为类参数立即下发
  api('/api/repeat', 'rate=' + D('sRep').value);
  applyRep(D('sRep').value);      // 循环图标/数字跟着设置页的选择走（同一真相源）
  api('/api/shuffle?on=' + (D('sShuf').checked ? 1 : 0), '');
}

function applySettings() {
  collectSettings();
  saveSettings();
  // 顺带把音乐库目录那一栏的输入也处理掉
  var extra = (D('sp_root') && D('sp_root').value.trim()) ? ('&rootpending=1') : '';
  toast('设置已保存' + extra);
}

/* ---------------- 键盘快捷键 ---------------- */
document.addEventListener('keydown', function (e) {
  var t = e.target || {};
  if (t.tagName === 'INPUT' || t.tagName === 'SELECT' || t.tagName === 'TEXTAREA') return;
  if (e.key === ' ') { e.preventDefault(); D('bPause').click(); }
  else if (e.key === 'ArrowRight') { e.preventDefault(); seekRel(5); }
  else if (e.key === 'ArrowLeft') { e.preventDefault(); seekRel(-5); }
  else if (e.key === 'ArrowUp') { e.preventDefault(); setVol(Math.min(0, S.vol + 1)); }
  else if (e.key === 'ArrowDown') { e.preventDefault(); setVol(Math.max(-60, S.vol - 1)); }
  else if (e.key === 'n') D('bNext').click();
  else if (e.key === 'p') D('bPrev').click();
  else if (e.key === 's') D('bStop').click();
  else if (e.key === 'l') D('bLyric').click();
  else if (e.key === 'Escape' && D('npPage') && D('npPage').classList.contains('on')) closeNowPlaying();
  else if (e.key === 'i') showChain();
});

/* ---------------- 播放队列（主视图） ---------------- */
function renderQueuePage() {
  S.view = 'queue';
  api('/api/queue').then(function (j) {
    S.q = { items: j.items || [], index: j.index || 0 };
    var n = S.q.items.length;
    head('播放队列', n ? (n + ' 首') : '', n ? '<button class=btn id=qClrAll>清空队列</button>' : '');
    if (!n) {
      D('list').innerHTML = '<div class=empty>队列是空的<br><span style="font-size:11.5px">把文件拖进窗口，或右键专辑/曲目「加入队列」</span></div>';
      return;
    }
    var h = '<div class=qlist2>';
    S.q.items.forEach(function (f, i) {
      h += '<div class="qrow' + (i === S.q.index ? ' on' : '') + '" data-i=' + i + ' draggable=true>' +
           '<span class=qn>' + (i + 1) + '</span>' +
           '<span class=qt title="' + esc(f) + '">' + esc(nam(f)) + '</span>' +
           '<span class=qp>' + esc(base(f)) + '</span>' +
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
    if (D('qClrAll')) D('qClrAll').onclick = function () {
      api('/api/clear', '').then(function () { toast('队列已清空'); renderQueuePage(); loadQ(); });
    };
  }).catch(function () { });
}

/* ---------------- 正在播放整页 ---------------- */
function openNowPlaying() {
  var pg = D('npPage'); if (!pg) return;
  pg.classList.add('on');
  // 每次都刷新：上一首的 LY 可能还在；而且内嵌歌词是异步加载的，
  // 只更新 #lyricbox 不会刷新整页的 #npLyric（用户反馈"整页没有歌词"）。
  if (S.lastCur) loadLyrics(S.lastCur);
  renderNp();
  // 整页控件是底栏/设置页的镜像：进来时刷一次，绝不显示与底栏不同的值
  applyRep(D('sRep') ? D('sRep').value : 0);
  syncVolUi();
}
function closeNowPlaying() { var pg = D('npPage'); if (pg) pg.classList.remove('on'); }
function renderNp() {
  var pg = D('npPage'); if (!pg || !pg.classList.contains('on')) return;
  D('npTitle').textContent = (D('npt') && D('npt').textContent) || '未播放';
  var sub = (D('npart') && D('npart').textContent) || '';
  var st = (D('nps') && D('nps').textContent) || '';
  D('npSub').textContent = sub + (st ? '  ·  ' + st : '');
  // 元数据补全芯片（loadMeta 异步回来时也会走到这里刷一次）
  if (D('npMeta')) D('npMeta').innerHTML = S.npMetaHtml || '';
  var bg = (D('cv') && D('cv').style.backgroundImage) || '';
  if (bg && bg.indexOf('url(') === 0) {
    D('npbg').style.backgroundImage = bg;
    D('npCover').src = bg.replace(/^url\("?/, '').replace(/"?\)$/, '');
    D('npCover').style.display = '';
  } else {
    D('npbg').style.backgroundImage = '';
    D('npCover').removeAttribute('src');
    D('npCover').style.display = 'none';
  }
  renderNpLyric();
}
function renderNpLyric() {
  var box = D('npLyric'); if (!box) return;
  if (!LY.lines.length) {
    box.innerHTML = '<div class=npline>没有歌词<br><span style="font-size:12px;color:var(--mut2)">同名 .lrc 文件、内嵌歌词，或播放时自动在线补全（可在设置-元数据补全关闭）</span></div>';
    return;
  }
  if (box.dataset.n !== String(LY.lines.length)) {
    box.dataset.n = String(LY.lines.length);
    var h = '';
    LY.lines.forEach(function (l, i) { h += '<div class=npline data-i=' + i + '>' + esc(l.s) + '</div>'; });
    box.innerHTML = h;
    box.dataset.at = '';
  }
  var idx = -1;
  for (var i = 0; i < LY.lines.length; ++i) if (LY.lines[i].t <= S.pos) idx = i;
  Array.prototype.forEach.call(box.children, function (e, i) { e.classList.toggle('on', i === idx); });
  if (idx >= 0 && box.dataset.at !== String(idx)) {
    box.dataset.at = String(idx);
    var el = box.children[idx];
    if (el) box.scrollTop = el.offsetTop - box.clientHeight / 2 + el.clientHeight / 2;
  }
}

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
