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
  eq: '', modorder: 5, modfamily: 'dsd', gpuvendor: 'auto', directsdm: 0, dsdKind: 'fir', dsdConv: 'narrow',
  // 输出后端与多声道
  backend: 'wasapi', channels: 0, naaHost: '127.0.0.1:43210',
  trim0: 0, trim1: 0, trim2: 0, trim3: 0, trim4: 0, trim5: 0, trim6: 0, trim7: 0,
  // 输入
  inBackend: 'none', inDev: '', inOffset: 0, inBuf: 100, inMonitor: 0, inSeconds: 0, inOut: '',
  // SDM（DSD 输出）专用滤波器：空 = 沿用 PCM 页那一组
  sdmFilt: '', sdmFiltNx: ''
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
          '&modfamily=' + encodeURIComponent(ADV.modfamily || 'dsd') +
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
function renderFieldGrid(fields, kind, opts) {
  if (!fields || typeof fields !== 'object') return '';
  var skip = (opts && opts.skip) || {};
  var labels = [], seen = {};
  var order = EF_ORDER[kind] || [];
  order.forEach(function (lb) {
    if (fields[lb] !== undefined && fields[lb] !== null && !skip[lb]) { labels.push(lb); seen[lb] = 1; }
  });
  Object.keys(fields).forEach(function (lb) {   // 契约外的标签也照渲染（前端不许丢后端给的内容）
    if (!seen[lb] && !skip[lb] && labels.indexOf(lb) < 0) labels.push(lb);
  });
  var rows = '';
  labels.forEach(function (lb) {
    var v = efValueText(fields[lb]);
    if (!v) return;                              // 空值 = 后端不知道 ⟹ 不占位
    var wide = !!EF_WIDE[lb] || v.length > 46 || v.indexOf('\n') >= 0;
    rows += '<div class="efrow' + (wide ? ' wide' : '') + '">' +
            '<div class=lb>' + esc(lb) + '</div><div class=vl>' + esc(v) + '</div></div>';
  });
  return rows ? ('<div class=efields>' + rows + '</div>') : '';
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
      h += '<button class="btn chip-copy" data-url="' + esc(c.url) + '" title="复制来源链接" style="font-size:11px;padding:2px 8px">' +
           '<svg width=11 height=11 viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" style="vertical-align:-1px;margin-right:3px"><rect x="9" y="9" width="13" height="13" rx="2" ry="2"/><path d="M5 15H4a2 2 0 0 1-2-2V4a2 2 0 0 1 2-2h9a2 2 0 0 1 2 2v1"/></svg>' +
           esc(label) +
           '</button>';
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
function fetchSummary(url, onDone, tries) {
  tries = tries || 0;
  api(url).then(function (j) {
    if (j && j.pending) {
      if (tries >= 60) { onDone({ ok: false, error: '资料整理超时' }); return; }
      setTimeout(function () { fetchSummary(url, onDone, tries + 1); }, 2000);
      return;
    }
    onDone(j);
  }).catch(function (e) { onDone({ ok: false, error: String(e) }); });
}

function bindCopyChips(container) {
  if (!container) return;
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

function openAlbum(k) {
  S.view = 'album';
  api('/api/lib/album?k=' + encodeURIComponent(k)).then(function (j) {
    if (S.view !== 'album') return;   // 已切走 → 丢弃（否则会把别页内容覆写成专辑页）
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

    // 2026-10-23（UI 评审 #2）：曲目列表必须在首屏可见 —— 以前简介（有时是很长的维基消歧义文）
    // 排在曲目之前，1024 下用户一首歌都看不到。现在先曲目、后简介。
    h += '<div style="padding:4px 0 24px">';
    j.tracks.forEach(function (t, i) {
      var isFav = FAV_SET.has(t.path);
      h += '<div class="trow trow-track" data-i=' + i + ' style="grid-template-columns:28px 24px minmax(120px,1fr) minmax(0,1fr) 58px;gap:8px">' +
           '<div class=n>' + (t.n || (i + 1)) + '</div>' +
           favIconHtml(t.path, isFav) +
           '<div class=tt title="' + esc(t.title) + '">' + esc(t.title) + '</div>' +
           '<div class=ar>' + esc(t.artist || j.artist) + '</div>' +
           '<div class=du>' + fmt(t.dur) + '</div>' +
           '</div>';
    });
    h += '</div>';
    // 简介放在曲目之后（评审 #2）；加载态只一行灰字，无 key 又无缓存时先隐藏
    if (META.summary == 1) {
      var albumSummLoading = (META.hasLlmKey === true);
      h += '<div class="sec-card summary-sec" id="albumSummary" data-k="' + esc(j.k) + '"' +
           ' style="margin:8px 0 24px' + (albumSummLoading ? '' : ';display:none') + '">' +
           (albumSummLoading ? '<div style="font-size:12px;color:var(--mut2)">正在整理资料…</div>' : '') +
           '</div>';
    }
    D('list').innerHTML = h;
    head(j.name, j.tracks.length + ' 首', '');

    if (META.summary == 1) {
      var albK = String(j.k);
      fetchSummary('/api/entity/summary?kind=release&name=' + encodeURIComponent(j.name) +
          '&artist=' + encodeURIComponent(j.artist || '') + '&lang=zh', function (s) {
          var el = D('albumSummary');
          if (!el || el.dataset.k !== albK) return;   // 异步回来时已切到别的专辑 → 丢弃
          // 261024-01 #4：后端现在回结构化 fields（可能没有 sections）⟹ 判据用"能渲染出东西"
          var albHtml = s ? renderSummaryContent(s, 'AI 整理', 'release') : '';
          if (albHtml) {
            el.style.display = '';
            el.innerHTML = albHtml;
            bindCopyChips(el);
          } else {
            el.remove();   // 无资料/无 key 又无缓存 → 干脆不显示
          }
        });
    }

    D('aPlay').onclick = function () { playAlbum(j.k, 0); };
    D('aAdd').onclick = function () { enqueueAlbum(j.k); };
    D('aShuf').onclick = function () { playAlbum(j.k, Math.floor(Math.random() * j.tracks.length)); };
    Array.prototype.forEach.call(D('list').querySelectorAll('.fav-btn'), function (btn) {
      btn.onclick = function (ev) {
        ev.stopPropagation();
        toggleFav(btn.dataset.file, ev);
      };
    });
    Array.prototype.forEach.call(D('list').querySelectorAll('.trow'), function (e) {
      e.onclick = function (ev) {
        if (ev.target.closest && ev.target.closest('.fav-btn')) return;
        playAlbum(j.k, +e.dataset.i);
      };
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
    var h = '<div class=grid style="grid-template-columns:repeat(auto-fill,minmax(150px,1fr))">';
    order.forEach(function (k) {
      var v = m[k];
      // 头像统一圆形（评审 #4）：占位字母**始终**渲染在底层，封面用 <img> 覆盖并 onerror 自隐。
      // 以前用 background-image：封面文件缺失时整块纯黑、连首字母都没有（Irotas/RØSE 就是这样）。
      var phChar = (String(k).trim().slice(0, 1) || '?').toUpperCase();
      h += '<div class=card data-a="' + esc(k) + '"><div class=cv style="border-radius:50%;overflow:hidden">' +
           '<div class=ph style="border-radius:50%">' + esc(phChar) + '</div>' +
           (v.cv ? '<img src="/api/lib/cover?k=' + v.key + '" loading=lazy alt="" style="position:absolute;inset:0;width:100%;height:100%;object-fit:cover;border-radius:50%" onerror="this.style.display=\'none\'">' : '') + '</div>' +
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

/* ---------------- 艺术家资料页（P3④a/④b） ---------------- */
function openArtistProfile(who, refresh) {
  S.view = 'artist';
  setView('artists');
  S.view = 'artist';
  S.artistWho = who;
  head(who, '正在获取权威资料…', '<button class=btn id=bBackArtistsTop>返回艺术家列表</button>');
  if (D('bBackArtistsTop')) D('bBackArtistsTop').onclick = function () { loadArtists(); };
  D('list').innerHTML = '<div class=empty style="padding:60px 0">正在获取权威资料…</div>';

  var uKnow = '/api/entity/knowledge?kind=person&name=' + encodeURIComponent(who) + (refresh ? '&refresh=1' : '');
  var uSumm = '/api/entity/summary?kind=person&name=' + encodeURIComponent(who) + '&lang=zh' + (refresh ? '&refresh=1' : '');

  // 2026-10-23（用户 261023-03 #2/#3）：权威资料与简介并行请求，谁先到都不丢；
  // 简介板块在 renderArtistProfile 里先渲染「正在整理资料…」（仅配了 key 时），回来再填。
  var wantSumm = (META.summary == 1);
  var summState = { done: false, res: null, enabled: wantSumm };
  function applyArtistSummary() {
    if (!wantSumm) return;
    if (S.view !== 'artist' || S.artistWho !== who) return;    // 已切走 → 丢弃
    var el = D('artistSummary');
    if (!el) return;
    if (!summState.done) return;
    var ss = summState.res;
    // 261024-01 #4：AI 资料优先按后端结构化 fields 渲染成标签值网格；旧缓存只有 sections 也能显示。
    var hasBody = ss && ((ss.fields && Object.keys(ss.fields).length > 0) ||
                         (ss.ok && ss.sections && ss.sections.length > 0));
    if (hasBody) {
      el.style.display = '';
      el.innerHTML = renderSummaryContent(ss, 'AI 整理', 'person', { skip: (summState.skipBio ? { '简介': 1 } : {}) });
      bindCopyChips(el);
    } else {
      el.remove();   // 无资料/失败/无 key 又无缓存 → 不留空白板块
    }
  }

  api(uKnow).catch(function (err) { return { ok: false, error: String(err), materials: [] }; }).then(function (res) {
    renderArtistProfile(who, res, summState);
    applyArtistSummary();
  });
  if (wantSumm) {
    fetchSummary(uSumm, function (s) {
      summState.done = true; summState.res = s; applyArtistSummary();
    });
  }
}

function renderArtistProfile(who, j, summary) {
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
  // 权威材料解析提前到这里：AI 资料网格要知道"简介"是否已由权威词条给出（避免同一页两段简介）。
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
      subText = '权威资料' + (firstDate ? (' · ' + firstDate + ' 取回') : '');
    }
  }
  // 261024-01 #3a：无权威资料时**不显示**「权威资料暂缺」这类状态小字 —— 顶部只留名字，
  // 下方安静地给 AI 整理资料；没有资料就什么都不显示（用户原话：那句说明"纯粹多余"）。

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

  // AI 整理资料（261024-01 #3a/#4）：这是本页**主内容**，紧跟 hero 之后；
  //   无权威词条时不再有任何"权威词条暂缺 / 维基百科没有匹配条目…"的解释卡（用户：那句纯粹多余）。
  //   结构化 fields → 标签值网格；旧缓存只有 sections → 段落兜底；两者都没有就整卡不渲染。
  var summHtml = '';
  if (META.summary == 1) {
    var sumObj = (summary && summary.enabled && summary.done) ? summary.res : null;
    var hasSummary = sumObj && ((sumObj.fields && Object.keys(sumObj.fields).length > 0) ||
                                (sumObj.ok && sumObj.sections && sumObj.sections.length > 0));
    var sumLoading = (META.hasLlmKey === true && !hasSummary);
    if (summary) summary.skipBio = !!bioText;   // 权威简介已在下面展示 → AI 网格里不重复「简介」
    summHtml = '<div class="sec-card summary-sec" id="artistSummary" style="margin-bottom:20px' + (sumLoading ? '' : ';display:none') + '">';
    if (hasSummary) {
      summHtml += renderSummaryContent(sumObj, 'AI 整理', 'person', { skip: (bioText ? { '简介': 1 } : {}) });
    } else if (sumLoading) {
      summHtml += '<div style="font-size:12px;color:var(--mut2)">正在整理资料…</div>';
    }
    summHtml += '</div>';
  }
  h += summHtml;

  if (!isOk) {
    // 261024-01 #3a：**删除整个「权威词条暂缺」说明块**。没有权威资料 → 上面 AI 资料安静显示，这里什么都不放。
    // 只有"在线整理也没开"这种确实无内容可显示的情况，才给一行极简空态 + 重试（绝不写一大段解释）。
    if (META.summary != 1) {
      h += '<div class="card empty-card" style="padding:22px 20px;text-align:center;background:var(--phbg);border:1px solid var(--line);border-radius:12px;margin-top:14px">' +
           '<div style="color:var(--mut);font-size:12.5px">暂无资料</div>' +
           '<button class="btn" id=bRetryKnowledge style="margin-top:12px">重试获取</button>' +
           '</div>';
    }
  } else {
    // 1) 权威简介（L2 原文，长文折叠「展开」）—— 排在 AI 资料之后，是"佐证"而不是主内容
    var isLong = bioText.length > 280 || (bioText.split('\n').length > 5);

    h += '<div class="sec-card bio-sec" style="margin-bottom:24px">' +
         '<div style="font-size:12px;font-weight:600;letter-spacing:1px;color:var(--mut2);margin-bottom:10px">权威简介</div>' +
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
  });
}

function renderTracksView() {
  if (S.view !== 'tracks') return;
  var t = S.currentTracks || [];
  var q = S.tracksQuery || '';
  head(q ? ('搜索：' + q) : '曲目', t.length + ' 首', '');
  if (!t.length) { D('list').innerHTML = '<div class=empty>没有匹配的曲目</div>'; return; }

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

  h += '<div style="padding:4px 0 24px">';
  sorted.forEach(function (x, i) {
    var isFav = FAV_SET.has(x.path);
    h += '<div class="trow trow-track" data-i=' + i + ' style="grid-template-columns:28px 24px minmax(120px,1.6fr) minmax(0,1fr) 92px 58px;gap:8px">' +
         '<div class=n>' + (i + 1) + '</div>' +
         favIconHtml(x.path, isFav) +
         '<div class=tt title="' + esc(x.title) + '">' + esc(x.title) + '</div>' +
         '<div class=ar title="' + esc(x.artist + ' \u2014 ' + x.album) + '">' + esc(x.artist) + ' \u2014 ' + esc(x.album) + '</div>' +
         '<div class=bd>' + fmtTag(x.sr, x.bits) + '</div>' +
         '<div class=du>' + fmt(x.dur) + '</div></div>';
  });
  h += '</div>';
  D('list').innerHTML = h;

  if (D('trackSort')) {
    D('trackSort').onchange = function () {
      S.trackSort = this.value;
      renderTracksView();
    };
  }

  Array.prototype.forEach.call(D('list').querySelectorAll('.fav-btn'), function (btn) {
    btn.onclick = function (ev) {
      ev.stopPropagation();
      toggleFav(btn.dataset.file, ev);
    };
  });

  Array.prototype.forEach.call(D('list').querySelectorAll('.trow'), function (e) {
    e.onclick = function (ev) {
      if (ev.target.closest && ev.target.closest('.fav-btn')) return;
      playPath(sorted[+e.dataset.i].path);
    };
    e.oncontextmenu = function (ev) {
      ev.preventDefault();
      addPath(sorted[+e.dataset.i].path);
    };
  });
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
    S.curMeta = m;
    if (m.title) D('npt').textContent = m.title;
    else D('npt').textContent = nam(f);
    var sub = [];
    if (m.artist) sub.push(m.artist);
    if (m.album) sub.push(m.album);
    D('npart').textContent = sub.join(' \u00b7 ');
    D('cv').style.backgroundImage = m.hasCover ? "url('/api/cover?file=" + encodeURIComponent(f) + "')" : '';
    // 补全芯片：存全局，renderNp 统一填进 #npMeta；整页开着就立刻刷一次（meta 是异步回来的）
    S.npMetaHtml = buildNpMetaHtml(m);
    if (D('npPage') && D('npPage').classList.contains('on')) {
      renderNp();   // renderNp 内部已调 updateNpSummary(S.curMeta)，不再重复请求
    }
  }).catch(function () { D('npt').textContent = nam(f); });
}

/* ---------------- 状态 ---------------- */
function tick() {
  api('/api/status').then(function (j) {
    S.playing = !!j.playing; S.paused = !!j.paused; S.dur = j.dur; S.pos = j.pos;
    // 2026-10-23 #1：底栏 bit-perfect 徽标的数据源（后端权威判定，见 /api/status 的 bitPerfect 字段）
    S.bpState = j.bitPerfect || ""; S.bpWhy = j.bitPerfectWhy || "";
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
      D('gpub').style.display = j.gpu ? '' : 'none';
    } else {
      D('fmt').textContent = '--';
      D('gpub').style.display = 'none';
      if (S.lastCur) { S.lastCur = ''; D('npt').textContent = '未播放'; D('npart').textContent = ''; D('nps').textContent = '选择一个专辑开始'; D('cv').style.backgroundImage = ''; S.npMetaHtml = ''; if (D('npMeta')) D('npMeta').innerHTML = ''; }
    }
    // 2026-10-23 #1：底栏常驻一个「b-p」徽标，不打开信号链也能知道当前是否 bit-perfect。
    // 2026-10-23 用户 261023-04 #1：改成紧凑 chip（class=bp-yes/bp-no/bp-direct），
    // 配色与显隐交给 CSS（三套主题各有一套颜色），这里不写死颜色、也不占中间列。
    if (D('bpBadge')) {
      var bpv = S.bpState || '';
      D('bpBadge').textContent = bpv === 'direct' ? 'BIT-PERFECT·1bit' : (bpv === 'yes' ? 'BIT-PERFECT' : (bpv === 'no' ? '非 bit-perfect' : ''));
      D('bpBadge').className = bpv ? ('bp-' + bpv) : '';
      D('bpBadge').title = S.bpWhy || '';
    }
    if (j.error) D('nps').textContent = j.error;
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
    if (j.err && !(j.items || []).length) {
      // 评审 #7：以前错误态整屏只留一行裸文本，像"页面坏了"。改成带说明 + 去配置入口的空态卡。
      head('流媒体', '', '');
      D('list').innerHTML = '<div class="card" style="max-width:560px;width:100%;margin:clamp(24px,10vh,90px) auto 0;padding:24px 26px;background:var(--phbg);border:1px solid var(--line);border-radius:12px">' +
        '<div style="font-size:14px;font-weight:650;margin-bottom:6px">还没有可浏览的流媒体服务器</div>' +
        '<div style="color:var(--mut);font-size:12.5px;line-height:1.7;margin-bottom:14px">' +
        esc(j.err) + '<br>支持 Navidrome / Subsonic / Emby（只取原始文件，不转码）。先在「设置 → 高级 → 流媒体服务器」填地址与账号，保存后回到这里即可浏览。</div>' +
        '<button class="btn pri" id=remoteGoSet>去设置里配置</button>' +
        '<button class="btn" id=remoteRetry style="margin-left:8px">重试</button></div>';
      if (D('remoteGoSet')) D('remoteGoSet').onclick = function () { openSettingsTab('adv'); };
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
  S.view = v;
  // 2026-10-23（uicheck 复现的真实 bug）：全局搜索框有 260ms 防抖，输入后立刻点左侧别的导航，
  // 定时器仍会触发 setView('tracks')+loadTracks，把用户刚切的页面顶掉（队列页就是这么被换掉的）。
  // 规则：切换视图即取消未触发的搜索，"最后一次用户操作"胜出。
  if (typeof searchTimer !== 'undefined' && searchTimer) { clearTimeout(searchTimer); searchTimer = 0; }
  // 作品页入口收进单曲页后，仍在内部使用 works 视图；离开 works 时清掉"从正在播放返回"的标记
  if (v !== 'works') S.worksFrom = '';
  Array.prototype.forEach.call(document.querySelectorAll('.nav'), function (e) {
    e.classList.toggle('on', e.dataset.v === v || (v === 'artist' && e.dataset.v === 'artists') || (v === 'work' && e.dataset.v === 'works'));
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
  };
});

/* ---------------- 搜索（全局） ---------------- */
var searchTimer = 0;
D('q').oninput = function () {
  var v = this.value.trim();
  clearTimeout(searchTimer);
  searchTimer = setTimeout(function () {
    if (v) { setView('tracks'); loadTracks(v); }
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
// 资料抽屉（261024-01 #3b ③）：关闭按钮 / 点遮罩 / 再点同一标签 都能收起
if (D('npDataClose')) D('npDataClose').onclick = function () { npDrawerClose(); npDataRenderBody(); };
if (D('npDrawerBg')) D('npDrawerBg').onclick = function () { npDrawerClose(); npDataRenderBody(); };
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
    LY.at = -1;
    D('lyricbox').innerHTML = LY.lines.length
      ? LY.lines.map(function (l, i) {
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
// wide=true 时整行显示（用于描述文字较长的开关，避免被 .sgbody 的多列网格压成窄条）
function chk(id, label, checked, hint, wide) {
  return '<div class="sfld' + (wide ? ' wide' : '') + '"><label class=chk><input type=checkbox id=' + id + (checked ? ' checked' : '') + '>' +
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

function pollCastState() {
  clearTimeout(_castPollTimer);
  var outPane = document.querySelector('#setpage .spane[data-g=out]');
  var isTabOn = outPane && outPane.classList.contains('on');
  var isSetOpen = D('setpage') && D('setpage').classList.contains('on');

  api('/api/cast/state').then(function (st) {
    updateCastStateUi(st);
    if ((isSetOpen && isTabOn) || (st && st.running)) {
      _castPollTimer = setTimeout(pollCastState, 1000);
    }
  }).catch(function () {
    if (isSetOpen && isTabOn) {
      _castPollTimer = setTimeout(pollCastState, 1500);
    }
  });
}

function scanCastDevices(force) {
  var s = D('sp_cast_dev');
  var msg = D('sp_cast_devmsg');
  var btn = D('sp_cast_scan');
  if (btn) btn.disabled = true;
  if (msg) msg.textContent = '正在发现渲染器…';

  var url = '/api/cast/devices' + (force ? '?scan=1' : '');
  api(url).then(function (j) {
    if (btn) btn.disabled = false;
    _castDevices = (j && j.devices) || [];
    if (s) {
      if (_castDevices.length === 0) {
        s.innerHTML = '<option value="">（未发现 UPnP 渲染器）</option>';
        if (msg) msg.textContent = '未发现 UPnP/DLNA 渲染器（可能未开机或不在同一子网）';
      } else {
        var h = '';
        _castDevices.forEach(function (d) {
          var label = (d.friendlyName || d.name || d.host) + (d.model ? (' · ' + d.model) : '');
          h += '<option value="' + esc(d.friendlyName || d.name || d.host) + '">' + esc(label) + '</option>';
        });
        s.innerHTML = h;
        if (msg) msg.textContent = '共发现 ' + _castDevices.length + ' 个渲染器';
      }
    }
  }).catch(function () {
    if (btn) btn.disabled = false;
    if (msg) msg.textContent = '扫描失败';
  });
}

function initCastControls() {
  var devSel = D('sp_cast_dev');
  var dopChk = D('sp_cast_dop');
  var btnScan = D('sp_cast_scan');
  var btnUseCur = D('sp_cast_usecur');
  var btnStart = D('sp_cast_start');
  var btnStop = D('sp_cast_stop');

  if (devSel) {
    devSel.onchange = function () {
      var d = _castDevices.find(function(x) { return (x.friendlyName || x.name || x.host) === devSel.value; });
      if (d) {
        var msg = D('sp_cast_devmsg');
        if (msg) msg.textContent = (d.model ? ('型号: ' + d.model + ' · ') : '') + (d.hasAVTransport ? '支持 AVTransport' : '未知传输服务');
      }
    };
  }
  if (dopChk) {
    dopChk.onchange = function () {
      // DoP 开关状态切换
    };
  }
  if (D('sp_cast_file')) {
    D('sp_cast_file').oninput = function () {
      // 用户输入音频路径
    };
  }
  if (btnScan) {
    btnScan.onclick = function () {
      scanCastDevices(true);
    };
  }
  if (btnUseCur) {
    btnUseCur.onclick = function () {
      api('/api/status').then(function (j) {
        if (j && j.cur) {
          var f = D('sp_cast_file');
          if (f) f.value = j.cur;
          toast('已填入当前播放文件');
        } else {
          toast('当前没有正在播放的文件');
        }
      }).catch(function () {
        toast('获取当前曲目失败');
      });
    };
  }
  if (btnStart) {
    btnStart.onclick = function () {
      var file = D('sp_cast_file') ? D('sp_cast_file').value.trim() : '';
      if (!file) {
        toast('请先填入源音频文件路径');
        return;
      }
      var dev = devSel ? devSel.value : '';
      var dop = (dopChk && dopChk.checked) ? 1 : 0;
      btnStart.disabled = true;
      if (D('sp_cast_msg')) D('sp_cast_msg').textContent = '正在启动投放…';

      var body = 'dev=' + encodeURIComponent(dev) +
                 '&file=' + encodeURIComponent(file) +
                 '&port=8912&dop=' + dop;
      api('/api/cast/start', body).then(function (j) {
        if (!j.ok) {
          toast(j.error || '投放启动失败');
          if (D('sp_cast_msg')) D('sp_cast_msg').textContent = j.error || '投放启动失败';
          btnStart.disabled = false;
        } else {
          toast('投放已启动');
          if (D('sp_cast_msg')) D('sp_cast_msg').textContent = '';
          pollCastState();
        }
      }).catch(function (e) {
        toast('请求失败');
        btnStart.disabled = false;
      });
    };
  }
  if (btnStop) {
    btnStop.onclick = function () {
      btnStop.disabled = true;
      api('/api/cast/stop', '').then(function (j) {
        toast('投放已停止');
        pollCastState();
      }).catch(function () {
        btnStop.disabled = false;
      });
    };
  }
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
  if (body === undefined) { body = hint; hint = ''; }
  return '<div class=sgroup><div class=sghead>' + title +
         (hint ? '<span class=hint>' + hint + '</span>' : '') +
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
     // 2026-10-23 用户 261023-04 #2：EQ 与房间校正从左侧主导航移到这里，单开一个页签。
     + '<button class=stab data-g=eq>EQ</button>'
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
     + sgroup('投放（UPnP）', '将本地音频流式推送到网络播放器/音箱（DLNA MediaRenderer）',
         // 2026-10-23 用户 261023-04 #12：渲染器名字常常很长（friendlyName + 型号），
         // 放在 260px 的小格里会被截断得只剩几个字；改成整行、下拉铺满宽度。
         '<div class="sfld wide"><label>目标渲染器</label>'
       + '<select id=sp_cast_dev style="width:100%"><option value="">（未发现 UPnP 渲染器）</option></select>'
       + '<div class=hint>选择局域网内的 DLNA/UPnP MediaRenderer 设备</div></div>'
       + '<div class="sfld" style="display:flex;align-items:flex-end">'
       + '<button class=btn id=sp_cast_scan>重新扫描</button>'
       + '<span id=sp_cast_devmsg class=hint style="margin-left:10px;line-height:30px"></span>'
       + '</div>'
       + '<div class="sfld wide"><label>源音频文件</label>'
       + '<div style="display:flex;gap:8px">'
       + '<input type=text id=sp_cast_file placeholder="输入音频文件绝对路径（如 D:\\Music\\test.wav）" style="flex:1">'
       + '<button class=btn id=sp_cast_usecur style="white-space:nowrap">用当前播放文件</button>'
       + '</div>'
       + '<div class=hint id=sp_cast_filehint>支持 WAV、FLAC、DSF/DFF 等本地音频格式</div>'
       + '</div>'
       + chk('sp_cast_dop', 'DoP 打包（DSD 源必须勾选）', false,
             'DSD 素材（.dsf / .dff）打包为 DoP v1.1 24-bit WAV 供源；若未勾选遇 DSD 源，后端为保证音质将明确拒绝（铁律 3）')
       + '<div class="sfld wide" style="display:flex;align-items:center;gap:10px;margin-top:6px">'
       + '<button class="btn pri" id=sp_cast_start>开始投放</button>'
       + '<button class=btn id=sp_cast_stop disabled>停止投放</button>'
       + '<span id=sp_cast_msg class=hint style="margin:0"></span>'
       + '</div>')
     // 2026-10-23 用户 261023-04 #12：状态网格必须跨满整张卡片（.sfld.wide），
     // 否则外层 .sgbody 的 auto-fill 网格把它压进单个 260px 单元 → 内层只剩 1 列，指标竖排成长条。
     + sgroup('投放实时状态', '1 秒自动轮询',
         '<div class="sfld wide in-stat-grid">'
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
       + sel('sp_modfamily', '调制器族',
              opt('dsd', 'DSD 标准（Butterworth NTF，1-bit）', ADV.modfamily || 'dsd') +
              opt('chebyshev', 'Chebyshev（可配带外增益与截止，1-bit 保护运行于 5 阶）', ADV.modfamily || 'dsd') +
              opt('ec', 'EC 误差反馈（频带零点扩展消除）', ADV.modfamily || 'dsd') +
              opt('ahm', 'AHM 多电平（6~8 bit 风格）', ADV.modfamily || 'dsd'),
              '提供多种 NTF 滤波与量化架构；非专有黑盒曲线')
        + sel('sp_modorder', '调制器阶数',
             opt(5, '5 阶（DSD64 起）', ADV.modorder || 5) + opt(7, '7 阶（需 DSD128 以上）', ADV.modorder || 5),
             '实测 7 阶在低采样率或 Chebyshev 1-bit 下会饱和失稳，程序会自动降为 5 阶')
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
     + sgroup('输入设备与采集', 'WASAPI 共享模式采集 / 录制与直通监听',
         sel('sp_inbackend', '输入后端', opt('none', '[none]', ADV.inBackend || 'none') + opt('wasapi', 'WASAPI', ADV.inBackend || 'none'),
             '选 WASAPI 后可以采集线路输入 / 麦克风 / 系统回环')
       + sel('sp_indev', '输入设备', '<option value="">（未选择 / 默认设备）</option>', '包含物理采集设备与 [Loopback] 系统混音回环端点')
       + sel('sp_inch', '声道偏移', opt('0', '0', ADV.inOffset || 0) + opt('1', '1', ADV.inOffset || 0) +
             opt('2', '2', ADV.inOffset || 0) + opt('4', '4', ADV.inOffset || 0),
             '多声道接口只取其中几路时用')
       + num({ id: 'sp_inbuf', label: '输入缓冲（毫秒）' }, ADV.inBuf || 100, 10, 500, 10, '环形缓冲时间深度')
       + chk('sp_inmon', '直通监听（非 bit-perfect）', ADV.inMonitor == 1,
             '勾选后启动采集将直接经系统默认共享端点监听输出；不勾选为录制保存至 WAV 文件')
       + num({ id: 'sp_inseconds', label: '录制时长（秒，0 为不限）' }, ADV.inSeconds || 0, 0, 86400, 1,
             '达到指定时长自动停止；0 表示持续采集直到手动停止')
       + '<div class="sfld wide"><label>输出文件路径（WAV）</label><input type=text id=sp_inout value="' +
             esc(ADV.inOut || '') + '" placeholder="留空由系统自动存至 recordings' + SEP + 'cap-时间戳.wav">' +
             '<div class=hint>仅录制模式生效；留空时自动保存在程序目录 recordings 文件夹下</div></div>'
       + '<div class="sfld wide" style="display:flex;align-items:center;gap:10px;margin-top:6px">'
       + '<button class="btn pri" id=sp_instart>开始采集</button>'
       + '<button class=btn id=sp_instop disabled>停止采集</button>'
       + '<span id=sp_inmsg class=hint style="margin:0"></span>'
       + '</div>')
     // 2026-10-23 用户 261023-04 #10：采集状态同样改成跨满整行的横向卡片网格。
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
     + sgroup('外观', '主题会记住',
         '<div class=sfld><label>主题</label><select id=sp_theme>'
       + opt('dark', '深色（默认）', S.theme) + opt('light', '白色', S.theme) + opt('blue', '浅蓝色', S.theme)
       + '</select><div class=hint>顶栏右侧的小圆点也可以快速切换</div></div>')
     + sgroup('GPU',
         sel('sp_gpu', 'GPU 后端',
             opt('auto', '自动', ADV.gpuvendor || 'auto') + opt('amd', 'AMD（HIP）', ADV.gpuvendor || 'auto') +
             opt('nvidia', 'NVIDIA（CUDA）', ADV.gpuvendor || 'auto') +
             opt('off', '关闭（纯 CPU）', ADV.gpuvendor || 'auto'),
             '自动＝按 rtagpu.dll（AMD/HIP）→ rtagpu_dx12.dll → rtagpu_cuda.dll（NVIDIA/CUDA）的顺序挑第一个能真正跑起来的后端；' +
             '选了具体厂商就只试该厂商的 DLL。后端缺失或 kernel 不可执行时自动退回 CPU 分块 FFT，并在信号链里写明实际用的哪个 DLL。')
       + '<div class="sfld wide"><div id=sp_gpumsg class=hint></div></div>'
       + '<div class="sfld wide"><div id=sp_asiomsg class=hint></div></div>')
     // 2026-10-23 用户 261023-04 #2：EQ 与房间校正已移进设置页的「EQ」页签，
     // 原来这里的「前往…页面」跳转按钮没有意义了，整组删掉（不再有重复 id / 失效入口）。
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
       + num({id:'sp_scanthreads', label:'扫描线程数'}, ADV.scanThreads || 0, 0, 32, 1, '库扫描并行线程数，0 为自动（默认 min(CPU, 8)）')
       + '<div class="sfld wide"><div id=sp_roots class=rootlist></div></div>'
       + '<div class="sfld wide"><div id=sp_upnpmsg style="font-size:11px;color:var(--mut2)"></div></div>')
     + sgroup('播放', '',
         sel('sp_rep', '循环模式', D('sRep').innerHTML, '')
       + chk('sp_shuf', '随机播放', D('sShuf').checked, '关掉会恢复原顺序')
       + chk('sp_loud', '等响度补偿', D('sLoud').checked, '音量越低低频提升越多'))
     + '</div>';

  // ---------- EQ 与房间校正（用户 261023-04 #2：从主导航移进设置页的独立页签）----------
  h += '<div class=spane data-g=eq>' + eqSettingsHtml() + '</div>';

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
  // 2026-10-23 用户 261023-04 #3：三个开关 → 两个。
  //   ①「自动补全（不用 AI）」= metaon（只走公开数据源，不产生费用）；
  //   ②「AI 增强（DeepSeek）」= 原「DeepSeek 增强（无标签文件）」+「在线整理音乐资料简介」合并，
  //      保存时同时写 metallm 与 metasummary（开 = 1,1；关 = 0,0）。
  // 高级选项：左右两列卡片（左 = 数据源，右 = DeepSeek 接入 + 标签写回），铺满宽度、留足间距。
  var aiOn = (META.llm == 1 && META.summary == 1);
  h += '<div class=spane data-g=meta>'
     + sgroup('元数据补全', '播放时后台补全标签 / 封面 / 歌词，结果落在本地库',
         chk('sp_metaon', '自动补全（不用 AI）', META.on == 1,
             '只用公开数据源（QQ音乐 / 网易云 / iTunes / dizzylab / lrclib）补全标签、封面、歌词；全程不调用 AI，不产生费用', true)
       + chk('sp_metallm', 'AI 增强（DeepSeek）', aiOn,
             '合并原「DeepSeek 解析无标签文件」与「在线整理音乐资料简介」：开启后会联网调用 DeepSeek（按次计费）；' +
             '关闭则两项都停（不解析无标签文件、也不整理简介），本地与公开数据源照常工作', true)
       + '<div class="sfld wide"><div class=hint style="color:var(--mut2)">'
       + 'AI 增强用于两件事：① 解析没有标签的文件，并与数据源结果互相校验；② 进入艺术家 / 专辑 / 单曲播放页时自动整理简介并缓存（同一条目只整理一次，缓存 7 天有效）。'
       + '不需要 AI 时可只开「自动补全（不用 AI）」。</div></div>'
       + '<div class="sfld wide" style="margin-top:6px">'
       + '<button class=btn id=sp_metamore style="font-size:12px" aria-expanded="false">高级选项 ▾</button>'
       + '<span style="font-size:11.5px;color:var(--mut2);margin-left:10px">数据源 · DeepSeek Key · 标签写回</span>'
       + '</div>'
       // #sp_metaadv 必须跨满 .sgbody 的整行（否则会被压进单个 260px 网格单元，两列各只剩 140px）
       + '<div class="sfld wide" id=sp_metaadv style="display:none;margin-top:14px">'
       + '<div class=meta-adv-grid>'
       // 左列：数据源（逐源开关，注册表驱动渲染进 #sp_metasrc）
       + '<div class=meta-card>'
       + '<div class=mh>数据源<span class=hint style="font-weight:400">越靠上优先级越高；关掉的源完全不参与补全</span></div>'
       + '<div id=sp_metasrc class=meta-src-grid>' + metaSrcHtml() + '</div>'
       + '</div>'
       // 右列：DeepSeek 接入 + 标签写回
       + '<div class=meta-card>'
       + '<div class=mh>DeepSeek 接入</div>'
       + '<div class=hint style="margin-bottom:8px">只用于上面的「AI 增强」；不填也能用全部本地与公开数据源</div>'
       + '<label style="display:block;font-size:11px;color:var(--mut);margin-bottom:5px">DeepSeek API Key</label>'
       + '<input type=password id=sp_llmkey autocomplete=off spellcheck=false placeholder="sk-…（粘贴后点保存，只显示掩码）" style="width:100%">'
       + '<div class=hint style="margin-top:4px">只存本机 build' + SEP + 'rlplayer-llm.conf；不进安装包，界面永不回显明文</div>'
       + '<div class=hint id=sp_llmmsg style="margin-top:6px">读取配置中…</div>'
       + '<div style="display:flex;gap:8px;margin-top:8px;flex-wrap:wrap">'
       + '<button class="btn pri" id=sp_llmsave>保存 Key</button>'
       + '<button class=btn id=sp_llmtest>测试连接</button>'
       + '<button class=btn id=sp_llmclear>清除 Key</button></div>'
       + '<div class="mh sub">标签写回<span class=hint style="font-weight:400">可选</span></div>'
       + '<div style="font-size:12px;color:var(--mut);line-height:1.75">'
       + '把在线补到的标签/封面<b>写进音频文件本身</b>，换播放器也能看到。<b>预览写回</b>只扫描并列出「哪些文件会被改成什么」，<b>不改任何文件</b>；'
       + '<b>执行写回</b>真正写入，写前逐位校验音频数据，任何不一致就放弃该文件，原文件备份在 rlplayer-meta' + SEP + 'backups。</div>'
       + '<div class=hint id=sp_wbdesc style="margin-top:6px;line-height:1.7">写回前自动校验音频数据逐位不变，原文件备份到 rlplayer-meta' + SEP + 'backups</div>'
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
      // 评审 #12：诊断页是纯操作页（导出/写回），底部「保存设置/恢复默认」在这里没有意义 → 隐藏
      var foot = document.querySelector('#setpage .setfoot');
      if (foot) foot.style.display = (b.dataset.g === 'diag') ? 'none' : '';
      if (b.dataset.g === 'in') {
        loadInputDevices();
        pollInputState();
      } else if (b.dataset.g === 'out') {
        scanCastDevices(false);
        pollCastState();
      }
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
    if (m) m.textContent = (j.available ? '当前 GPU 可用：' : '当前走 CPU：') + (j.status || '') +
      '；本目录 rtagpu.dll=' + (j.amd ? '有' : '无') +
      '，rtagpu_cuda.dll=' + (j.cuda ? (j.cudaUsable === false ? '有（本机不可用）' : '有（可用）') : '无') +
      '，AMD HIP 运行库=' + (j.hip ? '有' : '无') +
      '；厂商偏好=' + (j.vendor || 'auto');
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
    ADV = { device: '', bits: 0, taps: 0, atten: 0, transition: 0, phase: '', modcut: 0, moddith: -1, ir: '', irgpu: 0, ramp: 0, theme: S.theme, modorder: 5, modfamily: 'dsd', directsdm: 0, dsdKind: 'fir', dsdConv: 'narrow',
            backend: 'wasapi', channels: 0, naaHost: '127.0.0.1:43210',
            inBackend: 'none', inDev: '', inOffset: 0, inBuf: 100, inMonitor: 0, inSeconds: 0, inOut: '' };
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
    api('/api/lib/roots', 'add=' + encodeURIComponent(d)).then(function (j) {
      if (!j.ok) { toast(j.error || '添加失败'); return; }
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
  D('sp_upnp').onclick = function () {
    D('sp_upnpmsg').textContent = '正在搜索…';
    api('/api/upnp').then(function (j) {
      var hh = '';
      (j.devices || []).forEach(function (d) { hh += '<div>' + esc(d.name) + ' — ' + esc(d.host) + ':' + esc(d.port) + '</div>'; });
      D('sp_upnpmsg').innerHTML = hh || '没发现 DLNA 渲染器';
    });
  };
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

// EQ 页签当前是否可见（设置页开着 + 选中 EQ 页签）。用于丢弃切走后的异步回调。
function eqTabActive() {
  var p = document.querySelector('#setpage .spane[data-g=eq]');
  return !!(p && p.classList.contains('on'));
}
/* EQ 与房间校正（用户 261023-04 #2：从左侧主导航移进设置页的「EQ」页签）
   内容沿用原来的三块：① 耳机 EQ（OPRA 校准预设）② EQ 文件（REW / AutoEq）③ 房间校正 IR（WAV + GPU 卷积）。
   改用设置页统一的分组卡片（sgroup），不再用原来的独立大卡片，保证页签内视觉与其它页一致。 */
function eqSettingsHtml() {
  var h = '';
  // 1. 耳机 EQ（OPRA）
  h += sgroup('耳机 EQ（OPRA 校准预设）', '收录 6,230 款耳机专业校准滤波 · CC BY-SA 4.0',
      '<div class="sfld wide" style="display:flex;align-items:center;gap:10px">'
    + '<input id=sp_opra_search type=text placeholder="搜索耳机品牌或型号（如 Sennheiser / HD 600 / AirPods / Sony / XM5…）" autocomplete=off style="flex:1;min-width:0;width:auto;height:34px;padding:0 12px;background:var(--field);border:1px solid var(--line);border-radius:8px;font-size:12.5px;color:var(--txt);box-sizing:border-box">'
    + '<span class="chip" id=sp_opra_copychip title="复制 OPRA 项目官网链接" style="cursor:pointer;display:inline-flex;align-items:center;font-size:11.5px;flex:none">'
    + '<svg width=12 height=12 viewBox="0 0 24 24" fill=none stroke=currentColor stroke-width=2 style="margin-right:4px"><rect x="9" y="9" width="13" height="13" rx="2" ry="2"/><path d="M5 15H4a2 2 0 0 1-2-2V4a2 2 0 0 1 2-2h9a2 2 0 0 1 2 2v1"/></svg>https://opra.roon.app/</span>'
    + '</div>'
    + '<div class="sfld wide" id=eqSearchGuide style="padding:2px 0 0">'
    + '<div style="font-size:12px;color:var(--mut2);margin-bottom:8px">输入品牌或型号检索耳机，或直接点击下方热门品牌快速筛选：</div>'
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
  // 2. EQ 文件（REW / AutoEq 文本）
  h += sgroup('EQ 文件（REW / AutoEq 文本）', '参量 EQ 支持 REW / AutoEq 标准文本，按文件里的 Filter 行生成双二阶滤波器组',
      '<div class="sfld wide"><label>EQ 文件路径</label>'
    + '<input type=text id=sp_eq value="' + esc(ADV.eq || '') + '" placeholder="D:\\Music\\headphone-eq.txt">'
    + '<div class=hint>每行如 Filter 1: ON PK Fc 1000 Hz Gain -3.0 dB Q 1.41</div></div>');
  // 3. 房间校正 IR（WAV 路径 + GPU 卷积开关）
  h += sgroup('房间校正 IR（WAV 脉冲响应）', '把测量到的房间 / 耳机脉冲响应用 FIR 卷积叠进播放链路，做声学校正',
      '<div class="sfld wide"><label>IR 脉冲响应卷积文件（WAV）</label>'
    + '<input type=text id=sp_ir value="' + esc(ADV.ir || '') + '" placeholder="D:\\Music\\room.wav">'
    + '<div class=hint>留空 = 不做卷积；支持立体声 / 多声道 FIR 房间校正</div></div>'
    + chk('sp_irgpu', '卷积走 GPU 硬件加速（需要 rtagpu.dll 就位）', ADV.irgpu == 1,
          '房间 / 耳机 FIR 卷积交给 GPU；没有可用后端时自动退回 CPU')
    // 评审 #11：这里原来还有一个「保存 EQ 与房间校正设置」按钮，与底部全局「保存设置」重复 → 移除；
    // sp_eq / sp_ir / sp_irgpu 的值本来就会随 saveSettings 一起提交，不需要第二个主操作。
    );
  return h;
}
function initEqViewLogic() {
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
  if (D('sp_eq')) ADV.eq = (D('sp_eq').value || '').trim();
  if (D('sp_ir')) ADV.ir = (D('sp_ir').value || '').trim();
  if (D('sp_irgpu')) ADV.irgpu = D('sp_irgpu').checked ? 1 : 0;
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
  saveSettings(true);
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
  // Esc：资料抽屉开着先收抽屉（不把整页一起关掉），再按一次才退出单曲页
  else if (e.key === 'Escape' && D('npPage') && D('npPage').classList.contains('on')) {
    if (npDrawerIsOpen()) { npDrawerClose(); npDataRenderBody(); }
    else closeNowPlaying();
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
  if (S.lastCur) {
    loadLyrics(S.lastCur);
    if (!S.curMeta) loadMeta(S.lastCur);
  }
  renderNp();
  // 整页控件是底栏/设置页的镜像：进来时刷一次，绝不显示与底栏不同的值
  applyRep(D('sRep') ? D('sRep').value : 0);
  syncVolUi();
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
var NP_DATA = { key: '', active: '', tabs: [], cache: {}, inflight: {}, req: 0, bodyKey: '', tabSig: '' };

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
    h += '<button class=chip id=npEqChip title="' + esc('当前 EQ：' + npFileName(eq) + '（点击打开 EQ 与房间校正）') + '">' +
         '<svg width=11 height=11 viewBox="0 0 24 24" fill=none stroke=currentColor stroke-width=2 style="vertical-align:-1px"><path d="M4 21v-7M4 10V3M12 21v-9M12 8V3M20 21v-5M20 12V3M1 14h6M9 8h6M17 16h6"/></svg>EQ</button>';
  }
  if (ir) {
    h += '<button class=chip id=npIrChip title="' + esc('当前 IR：' + npFileName(ir) + '（点击打开 EQ 与房间校正）') + '">' +
         '<svg width=11 height=11 viewBox="0 0 24 24" fill=none stroke=currentColor stroke-width=2 style="vertical-align:-1px"><path d="M3 12h3l2-6 3 12 2-8 2 4h6"/></svg>IR</button>';
  }
  return h;
}
function npBindEqChips() {
  var eq = D('npEqChip'), ir = D('npIrChip');
  if (eq) eq.onclick = function () { closeNowPlaying(); openSettingsTab('eq'); };
  if (ir) ir.onclick = function () { closeNowPlaying(); openSettingsTab('eq'); };
}
function npDataReset() {
  NP_DATA = { key: '', active: '', tabs: [], cache: {}, inflight: {}, req: 0, bodyKey: '', tabSig: '' };
  var b = D('npDataBody'); if (b) b.innerHTML = '';
  var t = D('npTabs'); if (t) t.innerHTML = '';
  var s = D('npDataStat'); if (s) s.textContent = '';
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
         ' title="' + esc(t.label + '：' + (t.name || '') + '（点击查看 AI 整理的资料）') + '">' +
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
  if (!st || st.state === 'idle' || st.state === 'loading') return '整理中…';
  if (st.state === 'error') return '暂无资料';
  var d = st.data || {};
  if (kind === 'work') {
    var n = (d.performances || []).length;
    return (d.from_cache ? '缓存' : '实时') + (n ? ' · ' + n + ' 个版本' : '');
  }
  // 261024-01 #2：只标注来源状态（已缓存 / 刚刚整理），不再叠「未核对」这类长尾文案。
  return d.summaryCached ? '已缓存' : '刚刚整理';
}
function npDataSkeleton() {
  return '<div class=npdskel><i style="width:92%"></i><i style="width:76%"></i><i style="width:84%"></i></div>' +
         '<div class=npdhint>正在整理资料…</div>';
}
function npDataSummaryBody(d, kind) {
  // 261024-01 #4：结构化 filters/fields → 标签值网格（renderSummaryContent 内部优先 fields）；
  // 261024-01 #2：脚注只留「AI 整理」，删掉「未找到权威来源，仅供参考」「未核对 · 依据来源整理…」。
  var h = renderSummaryContent(d || {}, '', kind || NP_DATA.active);
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
  var sig = kind + '|' + st.state + '|' + (st.data ? (st.data.generated_at || (st.data.summaryCached ? 'c' : 'f')) : (st.error || ''));
  var need = (sig !== NP_DATA.bodyKey);
  if (!need && !force) return;
  NP_DATA.bodyKey = sig;
  if (st.state === 'idle' || st.state === 'loading') { body.innerHTML = npDataSkeleton(); return; }
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
function npDataEnsure(kind, force) {
  var t = npDataTab(kind); if (!t) return;
  var st = NP_DATA.cache[kind];
  if (!force && st && (st.state === 'loading' || st.state === 'ok')) return;
  var fp = kind + '|' + t.name + '|' + (t.artist || '') + '|' + (t.composer || '');
  if (!force && NP_DATA.inflight[fp]) return;
  NP_DATA.inflight[fp] = 1;
  var req = NP_DATA.req;
  NP_DATA.cache[kind] = { state: 'loading' };
  if (NP_DATA.active === kind) npDataRenderBody();

  function done(data, err) {
    delete NP_DATA.inflight[fp];
    if (NP_DATA.req !== req) return;               // 已切歌 → 丢弃过期结果
    if (!D('npPage') || !D('npPage').classList.contains('on')) return;
    if (data) NP_DATA.cache[kind] = { state: 'ok', data: data };
    else NP_DATA.cache[kind] = { state: 'error', error: err || '' };
    if (NP_DATA.active === kind) npDataRenderBody();
  }
  if (kind === 'work') {
    api('/api/entity/versions?kind=work&name=' + encodeURIComponent(t.name) +
        (t.composer ? ('&composer=' + encodeURIComponent(t.composer)) : '') + (force ? '&refresh=1' : ''))
      .then(function (j) { done((j && j.ok && (j.performances || []).length) ? j : null, j && j.error); })
      .catch(function (e) { done(null, String(e)); });
    return;
  }
  var u = '/api/entity/summary?kind=' + kind + '&name=' + encodeURIComponent(t.name) +
          (t.artist ? ('&artist=' + encodeURIComponent(t.artist)) : '') + '&lang=zh' + (force ? '&refresh=1' : '');
  fetchSummary(u, function (s) {
    // 261024-01 #4：后端新增结构化 fields（可能没有 sections）⟹ 有 fields 也算有资料
    var hasFields = s && s.fields && Object.keys(s.fields).length > 0;
    var hasSecs = s && s.ok && s.sections && s.sections.length > 0;
    if (hasFields || hasSecs) done(s, '');
    else done(null, (s && s.error) || '');
  });
}
function npDataActivate(kind) {
  NP_DATA.active = kind;
  npDataRenderTabs();
  npDataRenderBody();
  npDrawerOpen();          // 先滑出抽屉（骨架态），再请求 —— 点一下就有反馈，不做"静默等 10 秒"
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
  var tabs = can ? npDataTargets(m) : [];
  if (!can || !tabs.length) {
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
  // 261024-01 #3b ③「默认隐藏」⟹ 默认**不预取**任何实体：不点标签 = 不产生任何 AI 调用。
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
    D('npMeta').innerHTML = (S.npMetaHtml || '') + npEqChipsHtml();
    npBindEqChips();
  }
  updateFavUi();
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
function renderNpLyric() {
  var box = D('npLyric'); if (!box) return;
  if (box.classList) box.classList.toggle('empty', !LY.lines.length);
  // 评审 #19：纯音乐没有歌词时，歌词行不该继续占 1fr 高度（中间会留一大片黑）；
  // 让 .npgrid 收起歌词行，把空间让给资料面板。
  var grid = D('npGrid');
  if (grid) grid.classList.toggle('nolyric', !LY.lines.length);
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
initFavs();
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
