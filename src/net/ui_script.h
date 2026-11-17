// ui_script.h — 界面脚本（以 C++ 原始字符串承载，零转义）
//
// 视图：专辑 / 艺术家 / 曲目 / 文件夹 / 流媒体 / 队列 / 设置
// 原则：以专辑为中心；搜索是全局的；任何一次点击都不应该让界面失去上下文。
#pragma once
#include <string>
#include "ui_studio.h"
#include "ui_waveform.h"
#include "ui_eq_console.h"
#include "ui_listening_home.h"
#include "ui_listening_library.h"
#include "ui_workbench_state.h"

namespace rta::net {

inline const char* rta_ui_core_script() {
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
    catch (e) { toast(tf('打开失败：{0}', [u])); }
  }
  return true;
}
var S = {
  view: 'albums', albums: [], album: null, files: [], remote: [],
  q: { items: [], index: 0 }, cur: '', playing: false, paused: false,
  // 261026-08 #5/#6/#3：我的最爱 / 保存的队列 / 播放列表 的视图状态
  favs: [], sqs: [], sqName: '', pls: [], plOpen: '', plItems: {},
  vol: 0, loud: false, lastCur: '', roots: [], scanning: false, dur: 0, pos: 0,
  volDb: 0, startDb: 0, rawVolumeLocked:false, theme: 'dark', queueHidden: false,
  // 261024-04 #8：GUP 相关的两个运行时状态（gpuOn = 有 GPU；gpuUsed = 本次播放真的用了 GPU）
  gpuOn: false, gpuUsed: false
};
var VOLUME_WRITE={sequence:0,current:null,confirmed:null,statusBarrier:0};
var VOLUME_ACTION_QUEUE=Promise.resolve();
// 高级参数（设置页里改，params() 统一带上）。默认值与后端一致。
var ADV = {
  quickStart: 1,
  device: '', bits: 0, taps: 0, atten: 0, transition: 0, phase: '',
  modcut: 0, moddith: -1, ir: '', irgpu: 0, ramp: 0, theme: 'dark',
  // 房间校正自动应用（B3，2026-10-27）：1 = 播放时按当前布局自动挂 data\roomcorr\correction-<布局>.json。
  // 默认 1（用户原话「接上就是了」）；0 = 退回"只认手动 IR"的老行为。
  roomcorrauto: 1,
  eq: '', modorder: 5, modfamily: 'dsd', gpuvendor: 'auto', directsdm: 1, dsdKind: 'fir', dsdConv: 'narrow',
  // 输出后端与多声道
  // 261024-01 #9/#11：upnpDev = UPnP/DLNA 渲染器（设备描述 URL）；naaHost 自动发现填，
  // 手动框只是折叠兜底（用户原话：NAA 端点这个选项不应该出现，应该自动获取）。
  backend: 'wasapi', channels: 0, upnpDev: '',
  upnpUpsample: 0, upnpForceRate: 0, upnpRateCeiling: 192000,
  cleanup: 0, cleanupAuto: 1, cleanupStrength: .5, cleanupNoiseDb: -101.1,
  crossfeed: 0, crossfeedFreq: 700, crossfeedLevel: 12, dacbits: 0,
  trim0: 0, trim1: 0, trim2: 0, trim3: 0, trim4: 0, trim5: 0, trim6: 0, trim7: 0,
  // 输入
  inBackend: 'none', inDev: '', inOffset: 0, inBuf: 100, inMonitor: 0, inSeconds: 0, inOut: '',
  // SDM（DSD 输出）专用滤波器：空 = 沿用 PCM 页那一组
  sdmFilt: '', sdmFiltNx: '',
  // ReplayGain（261027-01 #2）：默认 off = 逐位直通；rgclip 默认开（防削波）
  rgmode: 'off', rgclip: 1,
  // 261027-04 #4：播放时自动分析（单曲页那三块）。默认 **1 = 开**（用户要求「播放就自动运行一次」），
  // 关掉后只有手动点「重新分析」才跑。conf 键 = autoanl（/api/settings 白名单里）。
  autoAnl: 1,
  // 2026-10-28：音频分析**模块总开关**（conf 键 anlmaster，默认 1）。
  // 与 autoAnl 的区别：autoAnl=0 只是不自动跑（卡还在，可手动跑）；
  // anlMaster=0 是整块关掉（监控面板不渲染、不发请求、右键无入口）。
  anlMaster: 1
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

/* ---------------- 界面多语言（i18n，2026-10-30 用户 261029-03） ----------------
   为什么用「中文原文当键」而不是给每个字面量改成 t("key")：
     本仓 UI 有 1 万多个中文字面量（ui_page.h + ui_script.h），逐处替换等于重写界面，
     而且任何一处漏改都会在切换语言时露出中文。改成「渲染后替换文本节点」：
       · 语言包 = { 中文原文: 译文 }，来自 /api/i18n（后端读 data\i18n\<code>.json）；
       · applyI18n(root) 遍历文本节点 + title/placeholder/aria-label/alt，命中就换掉；
       · 原文记在 WeakMap 里，切语言时先按原文重算（否则第二次翻译会拿英文当中文查）；
       · MutationObserver 监听 DOM 变化 —— 这个界面几乎全是 innerHTML 重建的，
         不监听的话切页/换歌后新内容又会变回中文。
   没命中的字符串保持中文（首批未覆盖的词条），这是有意的降级，不是 bug。 */
var I18N = { lang: "zh", map: {}, orig: new WeakMap(), attrOrig: new WeakMap(), observer: null, timer: 0 };
function i18nNorm(s) { return String(s == null ? "" : s).replace(/\s+/g, " ").trim(); }
// 2026-11-02（多语言审计抓到的真 bug）：i18nNorm 会 trim + 折叠空格，而语言包里大量「片段键」
//   本身带首尾空格或双空格（如「（相对峰值 −60 dB；参考 」结尾就有一个空格）⟹ 规范化后永远查不到，
//   t()/tf() 只能回落中文。实测设置页「Pre-ringing envelope 0.10 ms（相对峰值 −60 dB…）」恒为中文。
//   修法：查表时**两种形态都试**——先规范化（兼容用户手改的词条），再原始（兼容片段键）。
function i18nGet(map, raw) {
  if (!map) return undefined;
  var k = i18nNorm(raw);
  if (Object.prototype.hasOwnProperty.call(map, k)) return map[k];
  if (Object.prototype.hasOwnProperty.call(map, raw)) return map[raw];
  return undefined;
}
function t(s) { var v = i18nGet(I18N.map, String(s == null ? "" : s)); return (v === undefined ? s : v); }
// 2026-11-15：整串查表 + 正则规则 + 分段（·/|/、）三件一起做的版本，给**程序反复重设**的
//   属性用（bit-perfect 徽标 title 每秒重写一次，靠 1.5 s 兜底扫描会闪一下中文）。
function tr(s) { var v = i18nSeg(String(s == null ? "" : s)); return (v === null ? String(s == null ? "" : s) : v); }
// 2026-11-02（用户三次报同一类问题：外语界面里仍夹中文）：
//   「运行时拼出来的句子」整串查不到表 —— 数字一变就是新串，永远命中不了。
//   以前靠"给每种数字长度写一条正则规则"兜，规则越写越多、还总有漏（本次截图里三处全漏）。
//   现在改用**带占位符的模板当键**：模板本身（含 {0} {1}）进语言包，译文可以自由调语序/量词，
//   运行时只做占位符替换。为什么不用"片段拼接"：各语言语序不同，片段法必然拼出病句。
//   未翻译时原样返回模板并替换，等于中文原样 —— 与旧的 fallback 行为一致。
// 用法：tf('共 {0} 首', [n]) / tf('未发现转码痕迹 · 检出 {cut} kHz、下降 {edge} dB', {cut: c, edge: e})
function tf(tpl, params) {
  var m = I18N.map, s0 = i18nGet(m, tpl), s = (s0 === undefined ? tpl : s0);
  if (!params) return s;
  if (Object.prototype.toString.call(params) === '[object Array]') {
    for (var i = 0; i < params.length; i++) s = s.split('{' + i + '}').join(String(params[i]));
    return s;
  }
  for (var k in params) if (Object.prototype.hasOwnProperty.call(params, k))
    s = s.split('{' + k + '}').join(String(params[k]));
  return s;
}
// 计数/拼装串的规则表（键以 ~ 开头的是正则，值是替换串，$1 取捕获组）。
// 为什么需要它：界面上大量文本是 "共 6 首" "14 张专辑" 这种运行时拼的，
// 整串当键永远查不到；把数字当通配符才能翻。规则同样放在语言包 JSON 里（数据驱动，
// 加一条规则不用改 JS）。
function i18nPatterns() {
  if (I18N.pats) return I18N.pats;
  I18N.pats = [];
  var m = I18N.map || {};
  for (var k in m) {
    if (k.charAt(0) !== "~") continue;
    try { I18N.pats.push([new RegExp(k.slice(1)), m[k]]); } catch (e) { }
  }
  return I18N.pats;
}
function i18nByPattern(s) {
  var ps = i18nPatterns();
  for (var i = 0; i < ps.length; i++) {
    if (ps[i][0].test(s)) return s.replace(ps[i][0], ps[i][1]);
  }
  return null;
}
// ★ 2026-10-31（DEBUG-UI 报告 §三-1）：原列表漏了 `data-tip` ⟹ 设置页 43 处帮助浮层
//   在外语界面下原样显示中文（属性不在扫描列表里，DOM 文本节点替换根本碰不到）。
//   `data-tip` 必须一起进列表；同时 tipPlace() 里也要过 t()（见该函数）。
function i18nAttrs() { return ["title", "placeholder", "aria-label", "alt", "data-tip"]; }
// 2026-11-15（用户报「外语界面里还有中文」）：把「整串查表 → 正则规则 → 分段」抽成一个函数，
//   **文本节点与属性共用**。旧实现里属性（title/aria-label/data-tip）只做整串精确查表 ⟹
//   「音量 -60 dB、抖动/整形」这种后端拼出来的提示、「新建歌单」「音量旋钮」这类 aria-label
//   在外语界面里永远是中文。分段里多认一个顿号「、」：后端 bit-perfect 原因是一串用「、」
//   拼起来的短语（重采样 44100→48000 Hz、音量 -3 dB、抖动/整形），整串永远命中不了。
//   非中文/日文界面把顿号换成 ", "（顿号不是拉丁标点）。
function i18nJoinSep(sep) {
  if (sep === '\u3001') return (I18N.lang === 'zh' || I18N.lang === 'ja') ? sep : ', ';
  return sep;
}
function i18nSeg(raw) {
  if (raw === null || raw === undefined) return null;
  var s = String(raw); if (!s) return null;
  var tr = i18nGet(I18N.map, s);
  if (tr !== undefined && tr !== s) return tr;
  var key = i18nNorm(s);
  if (Object.prototype.hasOwnProperty.call(I18N.map, key)) {
    // 2026-11-02：允许"翻成空串"（值 === ""），例如分析表里的裸量词单元格「个」。
    var kv = I18N.map[key];
    if (kv !== key && kv !== s) return kv;
  }
  // 规则要拿**归一化后**的串去试：DOM 里 "年份 2011" 中间可能是不换行空格或换行，
  // 直接对原始串跑正则永远不匹配（实测英文界面里这一条一直是中文）。
  // 空串也是**有效译文**（例如「【置信度擦线…】」整段要删掉）⟹ 用 null 表示"没命中"。
  var byPat = i18nByPattern(key);
  if (byPat === null || byPat === key) {
    var byPat2 = i18nByPattern(s);
    if (byPat2 !== null) byPat = byPat2;
  }
  if (byPat !== null && byPat !== s) return byPat;
  // 2026-10-30：整串没命中时，按 " · " / " | " / "、" 拆开逐段译（实测界面上大量文本是
  //   「未知艺术家 · 44.1k/16」「音量 -60 dB、抖动/整形」这种拼装串，整串永远查不到）。
  var parts = s.split(/( \u00b7 | \| |\u3001)/);
  if (parts.length > 1) {
    var changed = false, buf = "";
    for (var pi = 0; pi < parts.length; pi++) {
      var p = parts[pi];
      if (pi % 2 === 1) { buf += i18nJoinSep(p); continue; }
      var pv = i18nSeg(p);
      if (pv !== null && pv !== p) { buf += pv; changed = true; } else buf += p;
    }
    if (changed) return buf;
  }
  return null;
}
function i18nNode(n) {
  var o = n.nodeValue; if (!o) return;
  var rec = I18N.orig.get(n);
  // 值被程序改过（时钟/进度）⟹ 旧原文作废，重新记一份
  if (!rec || rec.trans !== o) { rec = { orig: o, trans: null }; I18N.orig.set(n, rec); }
  var key = i18nNorm(rec.orig); if (!key) return;
  var tr = i18nSeg(rec.orig);
  if (tr === null) return;
  var lead = rec.orig.match(/^\s*/)[0], tail = rec.orig.match(/\s*$/)[0];
  var nv = lead + tr + tail;
  rec.trans = nv;
  if (n.nodeValue !== nv) n.nodeValue = nv;
}
function i18nEl(e) {
  if (!e || e.nodeType !== 1) return;
  var at = i18nAttrs();
  var store = I18N.attrOrig.get(e); if (!store) { store = {}; I18N.attrOrig.set(e, store); }
  for (var i = 0; i < at.length; i++) {
    var a = at[i], v = e.getAttribute(a); if (!v) continue;
    // 2026-11-15：属性值被程序改过（换歌/换设备/徽标刷新）时，旧原文作废。
    //   旧实现只在第一次记原文，之后一直拿**第一份中文**去查表：新值要么翻不出来，
    //   要么被上一份的译文覆盖（实测 bit-perfect 徽标 title 永远停在中文）。
    if (store[a] === undefined) store[a] = v;
    else if (v !== store[a]) {
      var prev = i18nSeg(store[a]);
      if (prev === null || v !== prev) store[a] = v;
    }
    var tv = i18nSeg(store[a]);
    if (tv !== null && tv !== '' && tv !== store[a] && v !== tv) e.setAttribute(a, tv);
  }
}
function applyI18n(root) {
  root = root || document.body; if (!root) return;
  if (I18N.lang === "zh" && !Object.keys(I18N.map).length) return;
  if (root.nodeType === 3) { i18nNode(root); return; }
  i18nEl(root);
  var w = document.createTreeWalker(root, NodeFilter.SHOW_TEXT | NodeFilter.SHOW_ELEMENT, null);
  var n = w.currentNode;
  while (n) { if (n.nodeType === 3) i18nNode(n); else i18nEl(n); n = w.nextNode(); }
}
function i18nSchedule() {
  if (!I18N.observer) return;
  clearTimeout(I18N.timer);
  I18N.timer = setTimeout(function () { applyI18n(document.body); }, 60);
}
function i18nWatch() {
  if (I18N.observer || !window.MutationObserver) return;
  I18N.observer = new MutationObserver(function (muts) {
    for (var i = 0; i < muts.length; i++) {
      var m = muts[i];
      if (m.type === "childList" && m.addedNodes && m.addedNodes.length) {
        for (var k = 0; k < m.addedNodes.length; k++) applyI18n(m.addedNodes[k]);
      } else if (m.type === "characterData") { applyI18n(m.target); }
    }
    i18nSchedule();
  });
  I18N.observer.observe(document.body, { childList: true, subtree: true, characterData: true });
  // 兜底扫描：MutationObserver 只报"新增/改动"，但本界面有大量**预先渲染好、之后只切
  // display** 的区块（单曲页的芯片就是），那些节点从来没有 DOM 变更事件 ⟹ 永远译不到
  // （实测英文界面里「年份 2011」「专辑艺术家」一直是中文）。
  // 每 1.5 s 全量扫一遍；单次遍历几千个节点约 1 ms，比漏译划算。
  setInterval(function () {
    if (I18N.lang && I18N.lang !== "zh") applyI18n(document.body);
  }, 1500);
}

// ---------------- 多语言验收驱动（仅 RLPLAYER_QA=1 时由 tick 启动） ----------------
// 页面自己动：展开音频分析的所有折叠块、打开 AI 抽屉、按步滚动，然后把两个面板的
// innerText 回报给后端（写 qa_text.log）。外部脚本按 step 推进并逐屏截图。
var QA = { on: false, last: -1, timer: 0, wantTransition: false, wantEqDialog: false };
function qaDismissTip() {
  // 截图前把浮动提示清掉（有的提示挂在顶层，截图里会压在按钮上，看起来像"叠字"）
  var t = document.querySelectorAll('.tiptext,.tipbox,.tipr,.tipcard');
  for (var i = 0; i < t.length; i++) t[i].remove();
  var a = document.activeElement;
  if (a && a !== document.body) { try { a.blur(); } catch (e) {} }
}
function qaPanelsText() {
  if (QA.wantWorkbench) {
    QA.wantWorkbench = 0;
    var ids=['[data-studio-advanced=pcm]','[data-studio-advanced=output]','#npAnlDynSummary','#npAnlQualSummary','#npAnlFakeSummary','#npAnlMore'];
    return '[WORKBENCH] '+JSON.stringify({view:S.view,mode:rlMode,viewport:[innerWidth,innerHeight],
      state:rlConsole.state,comparison:{active:rlCompare.activeId,slots:rlCompare.slots},
      paused:S.paused,metaFile:S.metaFile,lastCur:S.lastCur,queue:S.q,metadata:S.curMeta,
      cover:D('cv').style.backgroundImage,homeCover:D('rlListenCover')&&D('rlListenCover').style.backgroundImage,
      homeHasArt:!!(D('rlListenCover')&&D('rlListenCover').classList.contains('has-art')),
      settingsHidden:document.querySelector('.side .nav[data-v=settings]').hidden,
      dialogOpen:!!(D('rlListeningDialog')&&D('rlListeningDialog').open),
      analysisReady:!!(ANL.cur&&ANL.cache[ANL.cur]),analysisExpanded:!!(D('npAnlGrid')&&!D('npAnlGrid').hidden),
      legacyAnalysisControls:!!document.querySelector('#npPage #npAnlAuto,#npPage #npAnlNow'),monitorInStudio:!!document.querySelector('#rlStudio #npAnal'),
      geometry:ids.map(function(id){var e=document.querySelector(id);if(!e)return null;var r=e.getBoundingClientRect();return{id:id,x:r.x,y:r.y,w:r.width,h:r.height};})});
  }
  // 2026-11-02（多语言全面审计）：dumpbody 模式下抓**整页可见文本**（按行去重），
  //   用来逐页查残留中文；面板模式的判据不变，避免历史基线（7149 字符 / 132 行）失效。
  if (QA.wantEqDialog) {
    QA.wantEqDialog = 0;
    var eqd=D('rlEqSaveDialog');
    var rect=eqd?eqd.getBoundingClientRect():null;
    return '[EQDIALOG] ' + JSON.stringify({open:!!(eqd&&eqd.open),rect:rect?{left:rect.left,top:rect.top,w:rect.width,h:rect.height}:null,viewport:[innerWidth,innerHeight]});
  }
  if (QA.wantTransition) {
    QA.wantTransition = 0;
    return '[TRANSITION] ' + JSON.stringify(window.__rlTransitionTrace || []);
  }
  if (QA.wantAttr) { QA.wantAttr = 0; return QA.attrDump || '[ATTR] 0'; }
  if (QA.wantFull) {
    QA.wantFull = 0;
    var root = document.querySelector('#view') || document.body;
    var txt = (root.innerText || document.body.innerText || '');
    var seen = {}, out = [];
    txt.split('\n').forEach(function (l) {
      var s = l.replace(/\s+$/, '');
      if (!s.trim() || seen[s]) return;
      seen[s] = 1; out.push(s);
    });
    return '[BODY] ' + out.length + ' lines\n' + out.join('\n');
  }
  var a = document.querySelector('#npAnal');
  var d = document.querySelector('#npDataBody');
  var t1 = a ? (a.innerText || '') : '';
  var t2 = d ? (d.innerText || '') : '';
  // 把抽屉/分析条的屏幕矩形也报回来：截图之外再给一份"它确实在视口里"的证据
  var dr = document.querySelector('#npDrawer');
  var geo = '';
  if (dr) {
    var r = dr.getBoundingClientRect();
    geo = '[drawer on=' + dr.classList.contains('on') + ' x=' + Math.round(r.left) + ' y=' + Math.round(r.top) +
          ' w=' + Math.round(r.width) + ' h=' + Math.round(r.height) + ']\n';
  }
  if (a) { var ra = a.getBoundingClientRect(); geo += '[analysis y=' + Math.round(ra.top) + ' h=' + Math.round(ra.height) + ']\n'; }
  if (dr) {
    var cs = getComputedStyle(dr);
    geo += '[style vis=' + cs.visibility + ' op=' + cs.opacity + ' z=' + cs.zIndex + ' tr=' + cs.transform +
           ' bg=' + cs.backgroundColor + ' pos=' + cs.position + ']\n';
    var host = dr.parentElement;
    if (host) { var hs = getComputedStyle(host); geo += '[host id=' + host.id + ' pos=' + hs.position + ' ov=' + hs.overflow + ']\n'; }
  }
  t2 = geo + t2;
  return '### ANALYSIS ###\n' + t1 + '\n### AI ###\n' + t2;
}
function qaExec(cmd) {
  cmd = cmd || '';
  // The existing opt-in QA bridge can drive only these real workspace controls.
  // No arbitrary script or parameter is accepted through the HTTP QA channel.
  if (cmd === 'nav:albums') { if (typeof rlNav === 'function') rlNav('albums'); return; }
  if (cmd === 'nav:listening') { if (typeof rlNav === 'function') rlNav('listening'); return; }
  if (cmd.indexOf('workbench:') === 0) {
    var action=cmd.slice(10), selector='';
    if(action==='state'){QA.wantWorkbench=1;return;}
    if(action==='transition:trace'){QA.wantTransition=1;return;}
    // 3.3.9：验收用 —— 打开 EQ 保存二级窗口并回报它的居中位置，便于真窗口截图核对。
    if(action==='eqsave'){
      if(typeof rlEqOpenDialog==='function'){rlEqOpenDialog('custom','');QA.wantEqDialog=1;}
      return;
    }
    if(action==='eqsave:close'){var eqd=D('rlEqSaveDialog');if(eqd&&eqd.open)eqd.close();return;}
    if(action==='dither-help')selector='[data-shaper-help]';
    else if(action==='dither-close')selector='#mclose';
    else if(/^advanced:(pcm|sdm|output)$/.test(action))selector='[data-studio-advanced='+action.slice(9)+']';
    else if(action==='crossfeed-toggle')selector='#rlStudioAdvancedDialog [data-studio-key=crossfeed]';
    else if(/^crossfeed:(off|light|standard)$/.test(action))selector='#rlStudioAdvancedDialog [data-crossfeed-preset='+action.slice(10)+']';
    else if(action==='advanced-close')selector='#rlStudioAdvancedDialog header button';
    if(selector){var qaControl=document.querySelector(selector);if(qaControl&&!qaControl.disabled&&qaControl.getBoundingClientRect().width)qaControl.click();return;}
    if(action==='mode:listen'||action==='mode:pro'){rlSetMode(action.slice(5),false);return;}
    if(action==='save'){if(typeof rlStudioFlush==='function')rlStudioFlush().catch(function(){});return;}
    if(action==='pin'){if(typeof rlStudioSetBoundary==='function')rlStudioSetBoundary('start');return;}
    // 2026-11-12：四组左右箭头已按用户要求移除；QA 桥改为直接调用同一套切换函数，
    // 不再依赖已不存在的 #rlStudioComparePrev/#rlStudioCompareNext。
    if(action==='prev'||action==='next'){if(typeof rlStudioSelectDelta==='function')rlStudioSelectDelta(action==='prev'?-1:1);return;}
    if(/^tab:(ab|eq|monitor)$/.test(action)){if(typeof rlStudioTab==='function')rlStudioTab(action.slice(4));return;}
    if(action==='rack'||action==='top'||action==='segment'){
      if((action==='rack'||action==='segment')&&typeof rlStudioTab==='function')rlStudioTab('ab');
      var section=action==='rack'?D('rlStudioModFamily'):action==='segment'?D('rlStudioSegment'):D('rlStudio');
      if(section)section.scrollIntoView({block:action==='segment'?'center':'start'});return;
    }
    if(/^card:(output|profile|style)$/.test(action)){rlOpenListeningCard(action.slice(5));return;}
    if(/^style:(warm|balanced|sharp)$/.test(action))selector='[data-rl-listening-style='+action.slice(6)+']';
    else if(/^slot:[ABCD]$/.test(action))selector='[data-studio-slot='+action.slice(5)+']';
    else selector={apply:'#rlListeningDialogApply',cancel:'#rlListeningDialogCancel',save:'#rlStudioSlotSave',prev:'#rlStudioComparePrev',next:'#rlStudioCompareNext',pin:'#rlStudioPinCurrent'}[action]||'';
    var control=selector&&document.querySelector(selector);
    if(control&&!control.disabled&&control.getBoundingClientRect().width)control.click();
    return;
  }
  if (cmd === 'np') { if (typeof openNowPlaying === 'function') openNowPlaying(); return; }
  // 2026-11-02（多语言全面审计）：切到任意主视图，用来逐页抓文本查残留中文。
  if (cmd.indexOf('view:') === 0) {
    var v = cmd.slice(5);
    try {
      if (typeof setView === 'function') { setView(v); return; }
      if (typeof openSettingsTab === 'function' && v === 'settings') { openSettingsTab('out'); return; }
      location.hash = '#/' + v;
    } catch (e) {}
    return;
  }
  // 把整页可见文本回报（比只抓面板更能发现"列表头/toast/设置项"里的残留）
  if (cmd === 'dumpbody') { QA.wantFull = 1; return; }
  // QA-only native-host capture of the production feedback result renderer.
  // Does not call submit, collect diagnostics, look up credentials, or send HTTP.
  if (cmd === 'feedback-local') {
    openSettingsTab('diag');
    setTimeout(function () { fbShowResult({ uploaded: false, status: 503,
      local: 'QA synthetic local fixture', error: 'QA synthetic transport failure; no request sent' }); }, 300);
    QA.wantFull = 1;
    return;
  }
  if (cmd === 'feedback-success') {
    openSettingsTab('diag');
    setTimeout(function () { fbShowResult({ uploaded: true, status: 201, id: 'FB-QA-LOCAL',
      path: 'FB-QA-LOCAL', diag_stored: true });
      var result = D('sp_fbresult'); if (result && result.scrollIntoView) result.scrollIntoView({ block: 'center' }); }, 300);
    QA.wantFull = 1;
    return;
  }
  if (cmd === 'update-manual' || cmd === 'update-quota') {
    openSettingsTab('upd');
    setTimeout(function () {
      if (cmd === 'update-manual') updShowCheck({ ok: true, hasUpdate: true, latest: '4.0.0',
        installMode: 'manual', reason: 'major-upgrade', cn: 'https://pan.baidu.com/s/QA-LOCAL',
        global: 'https://github.com/okhuoyanyan/RLPlayer2/releases', extractionCode: 'QA-LOCAL', fullSize: 409276280 });
      else updShowCheck({ ok: false, error: 'quota_exceeded：QA synthetic quota failure; no request sent',
        latest: '3.2.3', cn: 'https://pan.baidu.com/s/QA-LOCAL',
        global: 'https://github.com/okhuoyanyan/RLPlayer2/releases', extractionCode: 'QA-LOCAL' });
      var card = D('sp_updcard'); if (card && card.scrollIntoView) card.scrollIntoView({ block: 'center' });
    }, 300);
    QA.wantFull = 1;
    return;
  }
  // 2026-11-02：右键菜单 / 对话框审计入口（这些浮层不在页面文本里，dumpbody 抓不到，必须先打开）
  if (cmd.indexOf('menu:') === 0) {
    var what = cmd.slice(5);
    try {
      if (what === 'track') { showTrackMenu(300, 300, { path: (S.curPath || S.curFile || ''), cs: 0, ce: 0 }); }
      else if (what === 'queue') { showQueueContextMenu(200, 200, 0); }
      else if (what === 'lyric') { showLyricMenu(200, 200); }
      else if (what === 'row') { showQueueRowMenu(200, 200, 0); }
    } catch (e) {}
    QA.wantFull = 1;
    return;
  }
  // 更新对话框（设置-高级里的「检查更新」）
  if (cmd === 'dlg:update') {
    try { var b = document.querySelector('#sp_updcheck'); if (b) b.click(); } catch (e) {}
    QA.wantFull = 1;
    return;
  }
  // 2026-11-02：切设置页签（in/out/pcm/sdm/adv/eq/cal/meta），配合 dumpbody 逐页签审计
  if (cmd.indexOf('stab:') === 0) {
    try { openSettingsTab(cmd.slice(5)); } catch (e) { try { setView('settings'); } catch (e2) {} }
    return;
  }
  // 2026-11-02：弹一条真实 toast 并立刻抓整页文本 —— 验证 toast（短命文本节点）也走 i18n
  if (cmd.indexOf('toast:') === 0) {
    var msg = cmd.slice(6);
    try { toast(msg); } catch (e) {}
    QA.wantFull = 1;
    return;
  }
  // 2026-11-02：属性文本审计（title/placeholder/aria-label/alt/data-tip）。innerText 抓不到它们，
  //   而这几个属性同样会露中文；命中后回报 "ATTR 标签|属性|值"。
  if (cmd === 'dumpattr') {
    var at = ['title', 'placeholder', 'aria-label', 'alt', 'data-tip'];
    var out = [], seen = {};
    var els = document.querySelectorAll('*');
    for (var i = 0; i < els.length; i++) {
      for (var k = 0; k < at.length; k++) {
        var v = els[i].getAttribute && els[i].getAttribute(at[k]);
        if (!v || !/[\u4e00-\u9fff]/.test(v)) continue;
        var key = at[k] + '|' + v;
        if (seen[key]) continue;
        seen[key] = 1;
        out.push('ATTR ' + els[i].tagName + '|' + at[k] + '|' + v);
      }
    }
    QA.attrDump = '[ATTR] ' + out.length + ' 条\n' + out.join('\n');
    QA.wantAttr = 1;
    return;
  }
  // 2026-11-02：打开第 N 张专辑 / 第 N 位艺术家的详情页（多语言审计要覆盖详情页）
  if (cmd.indexOf('album:') === 0) {
    var ai = parseInt(cmd.slice(6), 10) || 0;
    var cards = document.querySelectorAll('#gridAlb .alb, #albums .alb, .alb');
    if (cards && cards[ai]) { cards[ai].click(); return; }
    if (typeof openAlbum === 'function' && S.albums && S.albums[ai]) { openAlbum(S.albums[ai]); return; }
    return;
  }
  if (cmd.indexOf('artist:') === 0) {
    var ri = parseInt(cmd.slice(7), 10) || 0;
    var ac = document.querySelectorAll('#gridArt .art, #artists .art, .art');
    if (ac && ac[ri]) { ac[ri].click(); return; }
    return;
  }
  // AI 抽屉：走 npDataActivateAll()（四类实体并行发），与点「AI 资料 ›」完全同一条路径
  if (cmd === 'ai') { if (typeof npDataActivateAll === 'function') npDataActivateAll(); return; }
  if (cmd === 'closeai') { if (typeof npDrawerClose === 'function') npDrawerClose(); return; }
  if (cmd === 'expand') {
    // 直接置到 L2（= 全部原始指标），不靠按钮文字匹配 —— i18n 之后按钮文案会变英文
    if (typeof anlSetLvl === 'function') { anlSetLvl(2); return; }
    var btns = document.querySelectorAll('#npAnal button, #npAnal .btn');
    for (var i = 0; i < btns.length; i++) btns[i].click();
    return;
  }
  if (cmd.indexOf('top:') === 0) {
    var y0 = parseInt(cmd.slice(4), 10) || 0;
    // 单曲页的滚动容器是 #npBody（不是 window）
    var nb = document.querySelector('#npBody');
    if (nb) nb.scrollTop = y0;
    window.scrollTo(0, 0);
    return;
  }
  if (cmd.indexOf('scroll:') === 0) {
    var y = parseInt(cmd.slice(7), 10) || 0;
    var el = document.querySelector('#npDataBody');
    if (el) el.scrollTop = y;
    return;
  }
}
function qaTick() {
  if (!QA.on) return;
  fetch('/api/qa/next').then(function (r) { return r.json(); }).then(function (j) {
    if (!j || j.step === QA.last || j.step < 0) return;
    QA.last = j.step;
    try { qaExec(j.cmd); } catch (e) {}
    try { qaDismissTip(); } catch (e) {}
    setTimeout(function () {
      try { fetch('/api/qa/text?step=' + j.step, { method: 'POST', body: qaPanelsText() }).catch(function () {}); } catch (e) {}
      fetch('/api/qa/done?step=' + j.step, { method: 'POST' }).catch(function () {});
    }, 400);
  }).catch(function () {});
}
function i18nApplyRtl() {
  var rtl = (I18N.lang === "ar");
  document.documentElement.dir = rtl ? "rtl" : "ltr";
  document.documentElement.lang = I18N.lang;
  // 2026-11-02：语言标记挂到 body，供 .zh-only（如 QQ 群）按语言显示/隐藏
  document.body.setAttribute('data-i18n-lang', I18N.lang || 'zh');
}
// 切语言前必须**先把译文还原成中文**，否则新语言包会把旧译文当成原文去查（永远查不到）。
// 这是「用渲染结果当键」这套方案的唯一陷阱，必须显式处理。
function i18nRestore(root) {
  root = root || document.body; if (!root) return;
  var w = document.createTreeWalker(root, NodeFilter.SHOW_TEXT | NodeFilter.SHOW_ELEMENT, null);
  var n = w.currentNode;
  while (n) {
    if (n.nodeType === 3) {
      var rec = I18N.orig.get(n);
      if (rec && rec.trans !== null && n.nodeValue === rec.trans) n.nodeValue = rec.orig;
    } else {
      var store = I18N.attrOrig.get(n);
      if (store) {
        var at = i18nAttrs();
        for (var i = 0; i < at.length; i++) {
          var a = at[i];
          if (store[a] !== undefined) n.setAttribute(a, store[a]);
        }
      }
    }
    n = w.nextNode();
  }
}
// 切语言：先按原文还原（把上次的译文换回中文），再套新包，最后整页重译。
function i18nSetLang(code, persist) {
  return apiGetRaw("/api/i18n?lang=" + encodeURIComponent(code || "auto")).then(function (j) {
    i18nRestore(document.body);
    // A parked settings form keeps its inputs but is outside the body walker.
    // Restore its source strings while the previous language maps still exist.
    if (typeof rlSettingsState !== 'undefined' && rlSettingsState) {
      Object.keys(rlSettingsState.parked).forEach(function(mode) {
        i18nRestore(rlSettingsState.parked[mode]);
      });
    }
    I18N.lang = j.lang || "zh";
    I18N.map = j.map || {};
    I18N.orig = new WeakMap(); I18N.attrOrig = new WeakMap(); I18N.pats = null;
    i18nApplyRtl();
    applyI18n(document.body);
    if (typeof rlSyncHomeNavigation === 'function') rlSyncHomeNavigation();
    if (typeof rlSettingsRoute==='function' && S.view==='settings')rlSettingsRoute(false);
    if (typeof rlHomeActive === 'function' && rlHomeActive()) {
      rlPaintHome(); rlPaintQueue();
    }
    if (D("sp_uilang")) D("sp_uilang").value = code || "auto";
    if (persist !== false) { ADV.uiLang = code || "auto"; saveSettings(true); }
    return j;
  });
}
function i18nBoot() {
  return apiGetRaw("/api/i18n").then(function (j) {
    I18N.lang = j.lang || "zh";
    I18N.map = j.map || {};
    I18N.pats = null;
    i18nApplyRtl();
    applyI18n(document.body);
    i18nWatch();
    if (D("sp_uilang") && j.setting) D("sp_uilang").value = j.setting;
    // 2026-11-02（多语言审计抓到的真 bug）：首屏可能在语言包到达**之前**就渲染完，
    // 那时 t()/tf() 只能返回中文原文；而拼进数字/设备名之后的串再也无法被文本节点替换命中
    // （实测设置页「Pre-ringing envelope 0.10 ms（相对峰值 −60 dB…）」恒为中文）。
    // 包一到就重渲染当前视图，让所有 t()/tf() 走一遍带词典的路径。
    try { if (typeof setView === 'function' && S && S.view) setView(S.view, true); } catch (e) {}
    return j;
  }).catch(function () { /* 语言包拿不到就保持中文，不影响其它功能 */ });
}

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

function playbackErrorText(message) {
  var raw = String(message == null ? '' : message);
  // Only fixed application-owned prefixes and exact known DSP messages are
  // interpreted. Device names, filenames and other diagnostics stay verbatim.
  var prefix = /^(?:渲染失败|Render failed|异常)\s*[:：]\s*/.exec(raw);
  var body = prefix ? raw.slice(prefix[0].length) : raw;
  var budget = /^Requested FIR exceeds the coefficient budget; quality was not reduced\. inRate=(\d+) outRate=(\d+) L=(\d+) M=(\d+) T=(\d+) taps=(\d+) budget=(\d+)\. Use a cascade or explicitly change the filter configuration\.$/.exec(body);
  var localized = body;
  if (budget) {
    localized = tf('FIR 设计超出系数预算，已拒绝且未降低音质：{0} → {1} Hz，请求 {2} 个系数，预算 {3} 个。请启用两段式升频，或调整滤波器/输出率。', [budget[1], budget[2], budget[6], budget[7]]);
  } else if ([
    'Resampler sample rates must be positive', 'Resampler supports 1 to 64 channels',
    'Resampler tap counts and budgets are invalid', 'Resampler transition must be finite and in [0,1)',
    'Resampler attenuation must be finite and in [0,1000] dB', 'Unknown resampler window or phase',
    'Invalid resampler phase mix', 'Invalid resampler apodization edges',
    'Automatic tap design requires a positive transition'
  ].indexOf(body) >= 0) {
    localized = t('滤波器参数无效，已拒绝处理。请检查采样率、声道数、相位、衰减、过渡带及抽头设置。');
  } else if ([
    'Reduced resampler ratio exceeds the supported integer range', 'Resampler coefficient count overflow',
    'Automatic source support exceeds the tap integer range', 'Transition requires an unrepresentable tap count',
    'Cascade intermediate sample rate overflow', 'Cascade transition requires too many taps', 'FFT size overflow'
  ].indexOf(body) >= 0) {
    localized = t('滤波器配置超出支持范围，已拒绝处理且未降低音质。请使用两段式升频或调整滤波器/输出率。');
  } else if (body === 'Filter preparation cancelled') {
    localized = t('滤波器准备已取消，未开始输出。');
  } else if (body === 'Manual FIR is invalid; playback was not started.') {
    localized = t('手动 IR 无效，已拒绝播放。请检查文件、采样率、声道数和 FIR 数据。');
  } else if (body === 'SDM_CORNER_QUALITY: One-bit noise-shaping corners below 100 kHz can distort the audio band; use 0/default 100 kHz or the EC family.') {
    localized = t('低于 100 kHz 的 1-bit 噪声整形拐点可能造成音频带失真，已拒绝输出。请选择 0／默认 100 kHz，或使用 EC。');
  } else if (body === 'SDM corner must be finite and positive') {
    localized = t('SDM 拐点必须为 0 或 100～200 kHz；EC 族不使用此值，但仍须是 0～200 的有限数字。输入已保留，请修正后再保存。');
  }
  return prefix ? tf('渲染失败：{0}', [localized]) : localized;
}
var PLAYBACK_SETTINGS = { ready: false, loading: false, failed: false, invalidVolume: false, generation: 0 };
function playbackSettingsMessage() {
  if (PLAYBACK_SETTINGS.invalidVolume) return t('保存的音量无效，请修正为 -120～+20 dB 后重新读取播放设置。');
  return t(PLAYBACK_SETTINGS.failed ? '播放设置读取失败，请重试后再手动播放。' : '正在读取播放设置，完成后请再次点击播放。');
}
function playbackSettingsBlocked() {
  var message = playbackSettingsMessage();
  toast(message);
  return { ok: false, errorCode: 'PLAYBACK_SETTINGS_NOT_READY', error: message };
}
function ensurePlaybackSettings() {
  if (PLAYBACK_SETTINGS.ready) return true;
  playbackSettingsBlocked(); return false;
}
function updatePlaybackSettingsGate() {
  ['vol', 'npVol', 'barVolInput', 'npVolInput', 'volFloor', 'npVolFloor'].forEach(function (id) {
    var e = D(id); if (!e) return;
    e.disabled = !PLAYBACK_SETTINGS.ready || isRawVolumeLocked();
    e.style.visibility = PLAYBACK_SETTINGS.ready ? '' : 'hidden';
    e.setAttribute('aria-busy', PLAYBACK_SETTINGS.ready ? 'false' : 'true');
  });
  if (D('volumePending')) {
    D('volumePending').hidden=PLAYBACK_SETTINGS.ready&&!isRawVolumeLocked();
    D('volumePending').textContent=t(PLAYBACK_SETTINGS.ready&&isRawVolumeLocked()?'设备调音':'音量尚未读取');
    D('volumePending').title=isRawVolumeLocked()?rawVolumeMessage():'';
  }
  if (D('barVolDb')) D('barVolDb').hidden = !PLAYBACK_SETTINGS.ready;
  ['volKnob','npVolKnob'].forEach(function(id){var e=D(id);if(!e)return;var locked=!PLAYBACK_SETTINGS.ready||isRawVolumeLocked();e.dataset.locked=String(locked);e.tabIndex=locked?-1:0;e.setAttribute('aria-disabled',String(locked));});
  if (D('volFloor') && (!PLAYBACK_SETTINGS.ready || isRawVolumeLocked())) D('volFloor').disabled = true;
  if (D('npVolLab') && !PLAYBACK_SETTINGS.ready) D('npVolLab').textContent = t('音量尚未读取');
  if (D('playbackSettingsNotice')) D('playbackSettingsNotice').hidden = PLAYBACK_SETTINGS.ready;
  if (D('playbackSettingsMsg')) D('playbackSettingsMsg').textContent = playbackSettingsMessage();
  if (D('playbackSettingsRetry')) {
    D('playbackSettingsRetry').hidden = !PLAYBACK_SETTINGS.failed;
    D('playbackSettingsRetry').disabled = PLAYBACK_SETTINGS.loading;
    D('playbackSettingsRetry').textContent = t('重新读取播放设置');
    D('playbackSettingsRetry').onclick = function () { loadSettings(); };
  }
}
function apiRaw(u, body) {
  // A blocked action is resolved as a refusal, never retried or queued for later.
  if (/^\/api\/(?:play|lib\/play|sq\/play|skip|pause|seek|cast\/start|input\/start|roomcorr\/measure|volume|settings|preset\/apply|workbench\/(?:apply|profile|eqpresets))(?:\?|$)/.test(u) && !PLAYBACK_SETTINGS.ready)
    return Promise.resolve(playbackSettingsBlocked());
  // 2026-10-25（安全加固后）：/api/ 下的一律用 POST —— 后端现在把副作用端点限制为 POST
  // （GET 一律 405，防 <img src="/api/quit"> 这类 CSRF）。读端点 POST 也照常工作（handler 不看 method），
  // 所以这里统一 POST 既修好「无 body 的 api('/api/fav?...') 变成 GET 被拒」，也不用逐个调用点改。
  var isApi = u.indexOf('/api/') === 0;
  var opt = (body === undefined && !isApi) ? {} : { method: 'POST' };
  if (body !== undefined) opt.body = body;
  var ctrl = (typeof AbortController !== 'undefined') ? new AbortController() : null;
  if (ctrl) opt.signal = ctrl.signal;
  // Enrichment owns a 20s server budget; let its explicit result reach the UI.
  var requestTimeout = /^\/api\/workbench\/(?:apply|profile|eqpresets)(?:\?|$)/.test(u) ? 35000 : (/^\/api\/enrich(?:\?|$)/.test(u) ? 22000 : 8000);
  var timer = setTimeout(function () { if (ctrl) ctrl.abort(); }, requestTimeout);
  return fetch(u, opt).then(function (r) {
    if (!r.ok) {
      return r.text().then(function (txt) {
        var msg = '';
        try {
          var j = JSON.parse(txt);
          if (j && j.error) msg = playbackErrorText(j.error);
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
    return r.json().then(function (j) {
      if (j && j.errorCode === 'ENRICH_BUDGET_EXCEEDED')
        j.message = t('元数据网络整理已达到时间预算，可重试；已有结果不会丢失');
      return j;
    });
  }).then(function (j) {
    // Headers alone do not finish a request: a stalled body must still abort,
    // otherwise every status caller could remain attached to one pending poll.
    clearTimeout(timer);
    return j;
  }, function (e) {
    clearTimeout(timer);
    throw e;
  });
}

/* 2026-10-27（P0 真实事故，261027-06 验收时抓到）：
   上面的 apiRaw 对所有 /api/* 一律 POST（2026-10-25 CSRF 加固：副作用端点限 POST，GET 一律 405）。
   但有一个读端点会因此读不到东西 —— /api/settings 的 GET 返回完整 kv，POST 只回 {merged:true}。
   loadSettings() 用 api(/api/settings) 读，拿到的是 {ok,merged} ⟹ kv 恒为 {} ⟹
   每个设置控件都停在 HTML 默认值。
 
   为什么这是 P0 而不是显示不对：
     ① 用户看到的设置是默认值，以为没保存；
     ② 更糟的是 saveSettings() 会把控件当前值整体 POST 回去 ——
        控件是默认值 ⟹ 一次无关的改动就把 volume/filter/shaper/device 全部洗回默认；
     ③ 2026-10-27 实测：settings.conf 里的 metasummary=1 被 saveSettings 写成 0 ⟹
        用户每保存一次设置，在线整理就被关掉一次（AI 入口随之整块消失）。
   所以这里需要一个真正发 GET 的最小封装，专门给读端点用。
   只给 /api/settings 用（其它读端点 POST 正常，handler 不看 method）。 */
function apiGetRaw(u) {
  var ctrl = (typeof AbortController != 'undefined') ? new AbortController() : null;
  var opt = {};
  if (ctrl) opt.signal = ctrl.signal;
  var timer = setTimeout(function () { if (ctrl) ctrl.abort(); }, 8000);
  return fetch(u, opt).then(function (r) {
    if (!r.ok) {
      var err = new Error('HTTP ' + r.status);
      err.status = r.status;
      throw err;
    }
    return r.json();
  }).then(function (j) {
    clearTimeout(timer);
    return j;
  }, function (e) {
    clearTimeout(timer);
    throw e;
  });
}
 
// ★ 2026-10-29（两轮审查 F27）：非幂等动作**不许**自动重试。
//
// 缺陷：api() 把任何"非 4xx"的失败都当成"服务端肯定没执行"并原样重发一次。
//   而 /api/skip、/api/qdel、/api/play、/api/sq/delete 这类端点一旦执行过就改变状态：
//   请求送达、响应丢了（超时/连接被重置）时重发 ⟹ 切两首、删两条。实测（评审用原始
//   api 函数 + 替身调度）：calls=2、index 0→2。
//
// 契约：
//   · 只读请求（GET 语义）照旧重试 —— 重发不会改变任何状态；
//   · 非幂等动作默认**不重试**，并把"结果未知"如实反馈给调用方（err.uncertain=true），
//     由 UI 提示用户而不是替用户猜；
//   · 需要重试时调用方显式传 {retry:true}（例如服务端已支持幂等键的场景）。
function api(u, body, opts) {
  var o = opts || {};
  if (/^\/api\/(?:workbench\/(?:apply|profile|eqpresets)|preset\/apply|peq(?:\/preset)?|opra\/apply)(?:\?|$)/.test(u) && !o.parameterTransport) {
    var kind=rlSettingsFence.kind(u,body);
    if(kind)return rlSettingsFence.request(function(){return api(u,body,Object.assign({},o,{parameterTransport:true}));},kind,o.parameterLease,body);
  }
  // URL parameters are also mutations: an absent body does not make sq/add,
  // lib/enqueue or roomcorr/measure safe to repeat after a lost response.
  // Unknown endpoints fail closed; only reviewed reads may retry by default.
  var readPath = String(u).split(/[?#]/)[0];
  var readOnly = (body === undefined || body === null) &&
    /^\/api\/(?:status|queue|signal|meta|lyrics|recent|files|net|providers|filters|filter\/detail|shapers|shaper-info|gpu|asio|devices|fav\/list|sq\/(?:list|info)|lib\/(?:albums|album|search|stats|cover\/progress)|src\/(?:browse|config|ping)|input\/(?:devices|state)|cast\/(?:state|devices)|writeback\/status|peq|opra\/(?:index|presets)|preset\/caps|roomcorr\/(?:status|layouts)|update\/(?:check|progress|state))$/.test(readPath);
  var idem = (o.idempotent === true) || readOnly;
  return apiRaw(u, body).catch(function (err) {
    if (err && err.status && err.status >= 400 && err.status < 500) {
      throw err;
    }
    if (o.retry === false || (!idem && o.retry !== true)) {
      // 结果未知：明确告诉调用方"可能已执行、可能没有"，绝不自动重放
      var e2 = new Error((err && err.message ? err.message : "网络异常") + "（结果未知，请刷新确认）");
      e2.uncertain = true;
      throw e2;
    }
    return new Promise(function (res) { setTimeout(res, 200); }).then(function () {
      return apiRaw(u, body);
    });
  });
}
// 非幂等动作的显式入口：语义与 api(u, body) 相同，但"永不自动重试"是硬约定。
function apiAct(u, body, lease) { return api(u, body, { retry: false, parameterLease:lease }); }

// PCM word-length controls share server metadata with the actual quantizer.
// A stopped player has no measured output rate: do not invent an auto result.
var RL_SHAPER_INFO = { cache: {}, pending: {} };
function rlShaperConditions() {
  var j = typeof rlLastStatus !== 'undefined' ? rlLastStatus : null;
  var active = !!(j && j.playing && Number(j.outRate) > 0 && Number(j.outBits) > 0);
  var selected = D('sShap') ? D('sShap').value : 'auto';
  var studio = D('rlStudioShaper');
  if (studio && typeof rlMode !== 'undefined' && rlMode === 'pro' && S.view === 'listening') selected = studio.value;
  var dsd = active ? (Number(j.outBits) === 1 || /^(?:dsd|dop|native)$/.test(j.outMode || ''))
                   : !!(D('sDSD') && D('sDSD').value !== '0');
  return {rate:active && !dsd ? Number(j.outRate) : 0,bits:active && !dsd ? Number(j.quantizationBits || 0) : 0,dsd:dsd,selected:selected === 'Shaped' ? 'shaped' : selected};
}
function rlLoadShaperInfo(rate, bits) {
  var key = rate + '/' + bits;
  if (RL_SHAPER_INFO.cache[key]) return Promise.resolve(RL_SHAPER_INFO.cache[key]);
  if (RL_SHAPER_INFO.pending[key]) return RL_SHAPER_INFO.pending[key];
  var request = apiGetRaw('/api/shaper-info?rate=' + rate + '&bits=' + bits).then(function (j) {
    if (!j || !j.ok || !Array.isArray(j.options)) throw Error('Invalid shaper metadata');
    RL_SHAPER_INFO.cache[key] = j; return j;
  });
  RL_SHAPER_INFO.pending[key] = request.then(function(j){delete RL_SHAPER_INFO.pending[key];return j;},function(e){delete RL_SHAPER_INFO.pending[key];throw e;});
  return RL_SHAPER_INFO.pending[key];
}
function rlShaperSummary(j, c) {
  if (c.dsd) return t('当前选择 DSD 输出；PCM 抖动不参与 1-bit 调制。');
  var choice = j && j.options.find(function(o){return o.id === c.selected;});
  var text = choice ? t(choice.description) : t('抖动用于减少降低位深时的量化失真；噪声整形把部分噪声移向高频。');
  if (c.selected === 'auto') text = c.rate > 0 && c.bits > 0 && j.autoResolved
    ? tf('按格式建议：{0} · {1} Hz / {2} bit；实际应用见信号链。', [j.autoResolved,c.rate,c.bits])
    : t('播放后按实际输出采样率与位深选择。');
  if (choice && c.rate > 0 && c.bits > 0 && choice.recommended === false)
    text += ' ' + t('当前输出条件不在建议范围内；可选择 auto / TPDF。');
  return text;
}
function rlRefreshShaperGuidance() {
  var targets = ['rlStudioShaperHint','sp_shaper_hint'].map(D).filter(Boolean);
  if (!targets.length) return;
  var c = rlShaperConditions(), key = JSON.stringify(c) + '/' + (typeof I18N !== 'undefined' ? I18N.lang : 'zh');
  targets = targets.filter(function(el){return el.dataset.shaperKey !== key;});
  if (!targets.length) return;
  targets.forEach(function(el){el.dataset.shaperKey=key;el.textContent=t(c.dsd?'当前选择 DSD 输出；PCM 抖动不参与 1-bit 调制。':'正在读取抖动说明…');});
  rlLoadShaperInfo(c.rate,c.bits).then(function(j){targets.forEach(function(el){if(el.isConnected&&el.dataset.shaperKey===key)el.textContent=rlShaperSummary(j,c);});})
    .catch(function(){targets.forEach(function(el){if(el.isConnected&&el.dataset.shaperKey===key)el.textContent=t('抖动说明暂时不可用，可点「抖动说明」重试。');});});
}
function rlOpenShaperInfo() {
  var c = rlShaperConditions();
  openModal(t('PCM 抖动与噪声整形'),'<div id=rlShaperInfoContent role=status>'+esc(t('正在读取抖动说明…'))+'</div>');
  var host = D('rlShaperInfoContent');
  rlLoadShaperInfo(c.rate,c.bits).then(function(j){
    if(D('rlShaperInfoContent')!==host)return;
    var h='<p>'+esc(t('抖动用于减少降低位深时的量化失真；噪声整形把部分噪声移向高频。'))+'</p><p class=hint>'+esc(rlShaperSummary(j,c))+'</p>';
    h+='<div style="overflow-x:auto"><table style="width:100%;border-collapse:collapse"><thead><tr><th style="padding:8px;text-align:left">'+esc(t('选项'))+'</th><th style="padding:8px;text-align:left">'+esc(t('用途与建议'))+'</th></tr></thead><tbody>';
    j.options.forEach(function(o){h+='<tr'+(o.id===c.selected?' aria-current=true':'')+'><td style="padding:9px 8px;border-top:1px solid var(--line);white-space:nowrap">'+esc(t(o.label||o.id))+'</td><td style="padding:9px 8px;border-top:1px solid var(--line)">'+esc(t(o.description))+'</td></tr>';});
    host.innerHTML=h+'</tbody></table></div><p class=hint>'+esc(t('高阶整形并不适合所有采样率；请按实际输出条件选择。'))+'</p><p class=hint>'+esc(t('选项名称用于描述本项目实现，不表示与 HQPlayer 的专有算法等同。'))+'</p>';
  }).catch(function(){if(D('rlShaperInfoContent')!==host)return;host.textContent=t('抖动说明加载失败，请重试。');var b=document.createElement('button');b.className='btn';b.textContent=t('重新加载');b.onclick=rlOpenShaperInfo;host.appendChild(b);});
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
// 隐藏源控件是唯一保存值；设置页重建、切歌都复用它，避免策略只停留在界面上。
function normalizeRateFamilyPolicy(value) {
  return value === 'strict' || value === 'compatible' ? value : 'prefer';
}
function rateFamilyPolicyValue() {
  return normalizeRateFamilyPolicy(D('sRateFamilyPolicy') && D('sRateFamilyPolicy').value);
}
function syncRateFamilyPolicy() {
  var policy = rateFamilyPolicyValue();
  if (D('sRateFamilyPolicy')) D('sRateFamilyPolicy').value = policy;
  if (D('sp_rateFamilyPolicy')) D('sp_rateFamilyPolicy').value = policy;
  var hint = D('sp_rateFamilyHint');
  if (hint) hint.textContent = t(policy === 'strict'
    ? '只允许源采样率同族输出，手动跨族目标也会拒绝。同族升降频仍是重采样，非逐位直通。'
    : policy === 'compatible'
    ? '自动档优先同族；设备没有可用同族档位时允许跨族回退。发生转换时显示重采样，非逐位直通。'
    : '自动档只选择源采样率同族；手动指定跨族是你的主动选择，会保留目标并显示真实转换：重采样，非逐位直通。');
}
/* PROCESSING_SETTINGS_BEGIN */
function processingSettingsValid(strength, noise) {
  return String(strength).trim() !== '' && String(noise).trim() !== '' &&
    Number.isFinite(Number(strength)) && Number(strength) >= 0 && Number(strength) <= 1 &&
    Number.isFinite(Number(noise)) && Number(noise) >= -120 && Number(noise) <= -60;
}
function validateProcessingSettings() {
  var strength=D('sp_cleanup_strength'), noise=D('sp_cleanup_noise');
  var valid=processingSettingsValid(strength?strength.value:ADV.cleanupStrength,noise?noise.value:ADV.cleanupNoiseDb);
  if(!valid)toast(t('清理强度须为 0～1，噪声底须为 -120～-60 dBFS，且必须是有限数字。请修正后再保存。'));
  if(!valid)return false;
  var freq=D('sp_crossfeed_freq'),level=D('sp_crossfeed_level');
  var f=freq?freq.value:(ADV.crossfeedFreq===undefined?700:ADV.crossfeedFreq),l=level?level.value:(ADV.crossfeedLevel===undefined?12:ADV.crossfeedLevel);
  if(String(f).trim()===''||String(l).trim()===''||!Number.isFinite(Number(f))||!Number.isFinite(Number(l))||Number(f)<300||Number(f)>1200||Number(l)<3||Number(l)>15){toast(t('交叉馈送频率须为 300～1200 Hz，交叉衰减须为 3～15 dB。'));return false;}
  return valid;
}
function processingSettingsParam() {
  return '&upnpUpsample='+(ADV.upnpUpsample?1:0)+'&upnpForceRate='+(ADV.upnpForceRate?1:0)
    +'&upnpRateCeiling='+ADV.upnpRateCeiling+'&cleanup='+(ADV.cleanup?1:0)
    +'&cleanup_auto='+(ADV.cleanupAuto===0?0:1)
    +'&cleanup_strength='+ADV.cleanupStrength+'&cleanup_noise_db='+ADV.cleanupNoiseDb
    +'&crossfeed='+(ADV.crossfeed?1:0)+'&crossfeed_freq='+(ADV.crossfeedFreq===undefined?700:ADV.crossfeedFreq)
    +'&crossfeed_level='+(ADV.crossfeedLevel===undefined?12:ADV.crossfeedLevel)+'&dacbits='+(ADV.dacbits||0);
}
function syncCleanupNoiseMode() {
  if(D('sp_cleanup_noise_mode'))D('sp_cleanup_noise_mode').value=ADV.cleanupAuto===0?'manual':'auto';
  if(D('sp_cleanup_noise'))D('sp_cleanup_noise').disabled=ADV.cleanupAuto!==0;
}
function syncProcessingControls() {
  [['sp_upnp_upsample',ADV.upnpUpsample],['sp_upnp_force',ADV.upnpForceRate],['sp_cleanup',ADV.cleanup],['sp_crossfeed',ADV.crossfeed]].forEach(function(x){if(D(x[0]))D(x[0]).checked=!!x[1];});
  [['sp_upnp_ceiling',ADV.upnpRateCeiling],['sp_cleanup_strength',ADV.cleanupStrength],['sp_cleanup_noise',ADV.cleanupNoiseDb],['sp_crossfeed_freq',ADV.crossfeedFreq],['sp_crossfeed_level',ADV.crossfeedLevel],['sp_dacbits',ADV.dacbits||0]].forEach(function(x){if(D(x[0])){if(D(x[0]).tagName==='SELECT')setSelectValue(D(x[0]),String(x[1]));else D(x[0]).value=String(x[1]);}});
  syncCleanupNoiseMode();
}
/* PROCESSING_SETTINGS_END */
/* CLEANUP_STATUS_BEGIN */
function cleanupStatusText(p, context) {
  var c = context || {}, finite = function (v) { return typeof v === 'number' && Number.isFinite(v) && v <= 0; };
  if (!c.enabled) return t('录音清理已关闭。');
  if (!p || typeof p.cleanupAuto !== 'boolean' || !p.cleanupAutoState || p.cleanupObserved === false)
    return t('尚未测量；下次播放时生效。');
  if (p.cleanupRequested === false || p.cleanupAuto !== !!c.auto ||
      (!c.auto && typeof p.cleanupNoiseDb === 'number' && p.cleanupNoiseDb !== c.manualNoiseDb))
    return t('设置已更改，下次播放时生效。');
  var text;
  if (c.auto) {
    if (p.cleanupAutoState === 'estimated' && p.cleanupAutoEstimateValid === true && finite(p.cleanupEstimatedNoiseFloorDbfs)) {
      text = tf('自动估计噪声底：{0} dBFS', [p.cleanupEstimatedNoiseFloorDbfs.toFixed(1)]);
      text += ' · ' + (finite(p.cleanupAppliedNoiseFloorDbfs)
        ? tf('本次采用：{0} dBFS', [p.cleanupAppliedNoiseFloorDbfs.toFixed(1)])
        : t('尚未采用估计，暂不降噪。'));
    } else text = t(p.cleanupAutoState === 'insufficient'
      ? '证据不足，暂不降噪。' : '等待可信安静片段；证据不足，暂不降噪。');
  } else text = finite(p.cleanupAppliedNoiseFloorDbfs) && p.cleanupAppliedNoiseFloorDbfs >= -120 && p.cleanupAppliedNoiseFloorDbfs <= -60
    ? tf('手动噪声底：{0} dBFS', [p.cleanupAppliedNoiseFloorDbfs.toFixed(1)])
    : t('尚未测量；下次播放时生效。');
  if (!(c.playing || c.paused) || ((p.src || p.file) && (p.src || p.file) !== c.current))
    text = tf('上次处理：{0}', [text]);
  return text;
}
/* CLEANUP_STATUS_END */
var CLEANUP_UI = {facts:null, source:'', currentFile:'', playbackKey:'', generation:0};
function paintCleanupStatus() {
  var node = D('sp_cleanup_status'); if (!node) return;
  var cast = ADV.backend === 'upnp', facts = CLEANUP_UI.source === (cast ? 'cast' : 'local') ? CLEANUP_UI.facts : null;
  node.textContent = cleanupStatusText(facts, {enabled:!!ADV.cleanup, auto:ADV.cleanupAuto!==0,
    manualNoiseDb:ADV.cleanupNoiseDb, playing:cast ? !!(facts && facts.running) : S.playing,
    paused:cast ? false : S.paused, current:cast ? ((facts && (facts.src || facts.file)) || '') : CLEANUP_UI.currentFile});
}
function observeCleanupReport(facts, source) {
  if (source !== (ADV.backend === 'upnp' ? 'cast' : 'local')) return;
  CLEANUP_UI.facts = facts || null; CLEANUP_UI.source = source;
  paintCleanupStatus();
}
function refreshCleanupStatus() {
  paintCleanupStatus();
  if (!ADV.cleanup || S.view !== 'settings') return Promise.resolve();
  var cast = ADV.backend === 'upnp', source = cast ? 'cast' : 'local';
  var generation = ++CLEANUP_UI.generation, file = CLEANUP_UI.currentFile;
  return api(cast ? '/api/cast/state' : '/api/signal').then(function (j) {
    if (generation !== CLEANUP_UI.generation || S.view !== 'settings' ||
        source !== (ADV.backend === 'upnp' ? 'cast' : 'local') || (!cast && file !== CLEANUP_UI.currentFile)) return;
    observeCleanupReport(j && j.ok ? (cast ? j : j.path) : null, source);
  }).catch(function () {
    if (generation === CLEANUP_UI.generation) observeCleanupReport(null, source);
  });
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
         '&rateFamilyPolicy=' + rateFamilyPolicyValue() +
         '&filter=' + encodeURIComponent(D('sFilt').value) +
         '&filterNx=' + encodeURIComponent(D('sFiltNx').value) +
         '&shaper=' + encodeURIComponent(D('sShap').value) +
         '&volume=' + S.vol +
         '&loudness=' + (S.loud ? 1 : 0) + processingSettingsParam() +
         // ReplayGain：off / track / album + 防削波（后端 decide() → cfg.replayGainDb）
         '&rgmode=' + (ADV.rgmode || 'off') + '&rgclip=' + (ADV.rgclip ? 1 : 0) +
         '&isp=' + (D('sIsp').checked ? 1 : 0) +
         '&twostage=' + D('s2s').value +
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
         '&roomcorrauto=' + (ADV.roomcorrauto === 0 ? 0 : 1) +
         '&ramp=' + (ADV.ramp || 0) +
         '&backend=' + encodeURIComponent(ADV.backend || 'wasapi') +
         '&channels=' + (ADV.channels || 0) +
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

// ============================================================================
// P2（2026-10-27 用户 261027-05 A4：「多声道就都标记出来就好了」）
// ----------------------------------------------------------------------------
// 全界面**只有这两个函数**决定"声道怎么显示"，不许各处自己拼字符串：
//   chLayout(ch) = 布局名（1=单声道 / 2=立体声 / 6=5.1 / 8=7.1 / 其他=Nch 或 N.M）
//   chMark(ch)   = 只给 >2 声道返回标记（0/1/2 返回空串 —— 2.0 是默认，标出来只增加噪声）
//   chText(ch)   = 带声道数的完整写法（"6 ch（5.1）"），给分析卡/信号链这种要精确的地方用
// 逐处落点：曲目列表、专辑页曲目行、单曲页副标题、底部播放条、信号链（解码/输出两环）、分析卡。
// ============================================================================
function chLayout(ch) {
  ch = parseInt(ch, 10) || 0;
  if (ch <= 0) return '';
  if (ch === 1) return t('单声道');
  if (ch === 2) return t('立体声');
  if (ch === 3) return '2.1';
  if (ch === 4) return '4.0';
  if (ch === 6) return '5.1';
  if (ch === 8) return '7.1';
  if (ch === 12) return '7.1.4';
  return ch + 'ch';
}
function chMark(ch) {
  ch = parseInt(ch, 10) || 0;
  if (ch <= 2) return '';
  return chLayout(ch);
}
function chText(ch) {
  ch = parseInt(ch, 10) || 0;
  if (ch <= 0) return '—';
  var l = chLayout(ch);
  return (l && l !== (ch + 'ch')) ? tf('{0} ch（{1}）', [ch, t(l)]) : (ch + ' ch');
}
// 行内后缀：多声道才追加（' · 5.1'），2.0 素材上界面与以前逐字相同
function chSuffix(ch) {
  var m = chMark(ch);
  return m ? (' · ' + m) : '';
}

// 格式徽标：采样率/位深（+ 多声道布局）。ch 可缺省 —— 老调用点零改动。
function fmtTag(sr, bits, ch) {
  if (!sr) return '';
  return (sr % 1000 === 0 ? (sr / 1000) + 'k' : (Math.round(sr / 100) / 10) + 'k') + '/' + bits + chSuffix(ch);
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
// 封面加载失败后的有限重试（见 albumCardHtml 里的注释）
function covRetry(im) {
  var n = parseInt(im.getAttribute('data-rt') || '0', 10);
  if (n >= 3) { im.style.display = 'none'; return; }
  im.setAttribute('data-rt', String(n + 1));
  setTimeout(function () {
    var u;
    try { u = new URL(im.src, location.href); } catch (e) { im.style.display = 'none'; return; }
    u.searchParams.set('_r', String(Date.now()));
    im.style.display = '';
    im.src = u.href;
  }, 700 * (n + 1));
}
function albumIdentityLabel(a) {
  var source = a && (a.albumIdentitySource || a.identitySource);
  if (source === 'directory_inferred') return t('文件夹分组');
  if (source === 'legacy_unknown') return t('来源待核实');
  return '';
}
function albumCardHtml(a, i) {
  return '<div class=card data-i=' + i + '>' +
    // 2026-10-26（261026-02 #3）：封面端点现在**找不到就 404**（不再回 1x1 占位图），
    // 所以 onerror 必须是"隐藏 <img>、露出底下的首字母"，而不是让格子空着。
    '<div class=cv>' +
      '<div class=ph>' + esc((a.name || '?').slice(0, 1).toUpperCase()) + '</div>' +
      // 2026-11-02（用户反馈「封面有时刷得出来有时候刷不出来，很随机」）：
      //   服务端对并发抽图做了限流，抢不到槽位的请求会立刻 404（保护页面不被拖死）。
      //   旧实现 onerror 直接隐藏，这一格就再也不重试了 ⟹ 看起来"随机缺图"。
      //   现在：失败后按 0.7s / 1.4s / 2.1s 退避重试 3 次（带 cache-busting），仍失败才露首字母。
      '<img data-album-key="' + esc(a.k) + '"' + (a.cv ? ' src="/api/lib/cover?k=' + encodeURIComponent(a.k) + '"' : '') +
      ' loading=lazy alt="" onerror="covRetry(this)">' +
      '<div class=pl><svg width=13 height=13 viewBox="0 0 24 24" fill=#fff><path d="M8 5v14l11-7z"/></svg></div>' +
    '</div>' +
    '<div class=nm title="' + esc(a.name) + '">' + esc(a.name) + '</div>' +
    '<div class=ar title="' + esc(a.artist) + '">' + (albumIdentityLabel(a) ? esc(albumIdentityLabel(a)) + ' · ' : '') + esc(a.artist || '未知艺术家') +
      (a.year ? '  \u00b7  ' + a.year : '') + (a.sr ? '  \u00b7  ' + fmtTag(a.sr, a.bits) : '') + '</div>' +
    '</div>';
}
// 2026-10-26（261026-02 #3）：封面抓取要有**看得见的进度**，并且进库页自动开跑。
// 后端：POST /api/lib/cover/prefetch（副作用）· GET /api/lib/cover/progress → {running,done,total,hit,miss,fail}
var COVP = { timer: 0, cacheTimer: 0, cacheDeadline: 0 };
function startCoverPrefetch() {
  api('/api/lib/cover/prefetch', '').then(function (j) {
    if (j && j.ok && j.queued) toast(tf('正在抓取封面：共 {0} 张缺图', [j.queued]));
    // 2026-10-26 (261026-05): always poll while the backend is running.
    // Previously polling only started when THIS call queued something, so a job started
    // elsewhere (prefetch?all=1, or a still-running batch) finished without repainting the grid,
    // leaving covers blank even though the server had them.
    if (j && j.ok && j.running) pollCoverProgress();
  }).catch(function () { });
}
// 刷新图片时保留当前视图和搜索；当前歌曲也可能刚获得封面。
// Cache completion is independent of online-prefetch progress (e.g. local tag
// extraction). Observe missing artwork briefly without restarting any provider.
function stopCachedCoverImages() {
  if (COVP.cacheTimer) clearInterval(COVP.cacheTimer);
  COVP.cacheTimer = 0;
}
window.addEventListener('pagehide', stopCachedCoverImages);
function watchCachedCoverImages() {
  stopCachedCoverImages();
  COVP.cacheDeadline = Date.now() + 60000;
  COVP.cacheTimer = setInterval(function () {
    if ((S.view !== 'albums' && S.view !== 'album') || Date.now() >= COVP.cacheDeadline) {
      stopCachedCoverImages(); return;
    }
    refreshCoverImages(true);
  }, 2000);
}
function refreshCoverImages(cacheOnly) {
  if (!cacheOnly) refreshCurrentCover();
  if (S.view !== 'albums' && S.view !== 'album') return;
  var view = S.view, album = S.album;
  // Retry only the track's own cache URL. A negative lookup must not prevent a
  // later local/cache result, nor cause another online lookup on each retry.
  Array.prototype.forEach.call(document.querySelectorAll('#list img[data-song-index]'), function (im) {
    var item = im.songArtwork;
    if (!item || !item.current() || !item.visible || !item.missing || item.cacheLoading) return;
    item.cacheLoading = true;
    im.src = item.url + '&_cov=' + Date.now();
  });
  var images = Array.prototype.filter.call(document.querySelectorAll('#list img[data-album-key]'), function (im) {
    return !im.getAttribute('src') || im.style.display === 'none';
  });
  if (!images.length) return;
  api('/api/lib/albums').then(function (j) {
    if (!j || !j.ok || S.view !== view || (view === 'album' && S.album !== album)) return;
    var available = new Set((j.albums || []).filter(function (a) { return a.cv; }).map(function (a) { return a.k; }));
    Array.prototype.forEach.call(images, function (im) {
      if (!im.isConnected || !available.has(im.dataset.albumKey)) return;
      if (im.getAttribute('src') && im.style.display !== 'none') return;
      im.style.display = '';
      im.setAttribute('data-rt', '0');
      im.src = '/api/lib/cover?k=' + encodeURIComponent(im.dataset.albumKey) + '&_cov=' + Date.now();
    });
  }).catch(function () {});
}
function pollCoverProgress() {
  if (COVP.timer) return;
  COVP.lastHit = -1;
  COVP.timer = setInterval(function () {
    api('/api/lib/cover/progress').then(function (g) {
      if (!g || !g.ok) return;
      var el = D('covProg');
      if (el) { el.style.display = 'flex'; el.textContent = tf('封面 {0}/{1}（命中 {2}）', [(g.done || 0), (g.total || 0), (g.hit || 0)]); }
      // 2026-11-01（紧急）：后台每补到新图就**立刻**把网格里缺图的格子重指一次，
      // 不等整批跑完。否则 178 张要跑几分钟，用户在这几分钟里只看到首字母。
      if (g.running && (g.hit || 0) > (COVP.lastHit || 0)) {
        COVP.lastHit = g.hit || 0;
        refreshCoverImages();
      }
      if (!g.running) {
        clearInterval(COVP.timer); COVP.timer = 0;
        if (el && g.done > 0) setTimeout(function () { el.style.display = 'none'; }, 2500);
        if (g.hit > 0) {
          toast(tf('封面已补齐 {0} 张', [g.hit]));
          refreshCoverImages();
        }
      }
    }).catch(function () { });
  }, 700);
}

var albumListRequest = 0;
function loadAlbums(q, title) {
  if(typeof rlPageTransitionBegin==='function')rlPageTransitionBegin('albums');
  stopCachedCoverImages();
  var request = ++albumListRequest;
  S.view = 'albums';
  startCoverPrefetch();     // 进库页就开跑（没有缺图时后端立即返回 queued=0）
  head(title || '专辑', '加载中…', '');
  api('/api/lib/albums?q=' + encodeURIComponent(q || '')).then(function (j) {
    if (!q && request === albumListRequest && j && Array.isArray(j.albums) && typeof rlPaintShelf === 'function') {
      rlHomeAlbums = j.albums.slice(0,3);
      rlPaintShelf();
    }
    if (S.view !== 'albums' || request !== albumListRequest) return;
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
      if (D('bEmptyAddRoot')) D('bEmptyAddRoot').onclick = function () { rlOpenDailyTask('library'); };
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
    watchCachedCoverImages();
    Array.prototype.forEach.call(D('list').querySelectorAll('.card'), function (e) {
      var a = S.albums[+e.dataset.i];
      e.onclick = function (ev) {
        if (ev.target.closest && ev.target.closest('.pl')) { playAlbum(a.k, 0); ev.stopPropagation(); return; }
        openAlbum(a.k);
      };
      e.oncontextmenu = function (ev) { ev.preventDefault(); enqueueAlbum(a.k); };
    });
    // 2026-10-29（#2）：曲库渲染完就开始后台预热艺术家头像。
    // 不放在 loadArtists 里，是因为用户可能一直在专辑页——那样头像永远不会被抓。
    artaWarmSoon();
  }).catch(function (e) {
    if (S.view === 'albums' && request === albumListRequest)
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
    toast(tf('收藏操作失败: {0}', [e]));
  });
}

function updateFavUi() {
  rlPaintListeningFavorite();
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
// 2026-10-29（管理员 261029-01 #4）：「曲目信息 AI 整理里有大量（AI 知识）标志，还加在正文里，请全部删除」。
//   AI 可不可信应该由界面顶部那个 AI 标记统一表达，正文里再挂一遍纯属噪声。
//   这里做**渲染期清洗**：即便后端缓存里还是旧提示词生成的脏内容，显示出来也干干净净。
var AI_MARK_RE = /[（(]\s*(?:AI\s*知识|AI\s*推测|AI\s*推断|AI\s*整理|AI\s*补全|据推测|推测|存疑|待核实|未核实)\s*[)）]/gi;
function stripAiMarks(s) {
  s = String(s == null ? '' : s);
  if (!s) return '';
  var o = s.replace(AI_MARK_RE, '');
  // 去掉标记后常留下「… 」或「…，」这类残渣，顺手收干净
  o = o.replace(/[ \t]{2,}/g, ' ').replace(/[ \t]+([，。；、,;])/g, '$1');
  return o.replace(/^[ \t]+/, '').replace(/[ \t]+$/, '');
}
// 2026-10-29（#5）：「不同的名盘分行显示，不要只用『；』隔开」。
//   列举型字段（≥3 段被分号隔开）拆成多行，每行一条；普通句子不动，避免把散文拆烂。
function splitListValue(v) {
  v = String(v == null ? '' : v);
  if (!v) return [v];
  if (v.indexOf('\n') >= 0) return v.split(/\n+/);
  var parts = v.split(/[；;]/), out = [];
  for (var i = 0; i < parts.length; ++i) {
    var p = parts[i].replace(/^\s+|\s+$/g, '');
    if (p) out.push(p);
  }
  return out.length >= 3 ? out : [v];
}
function efValueText(v) {
  if (v === null || v === undefined) return '';
  if (typeof v === 'string') return stripAiMarks(v.replace(/\s+$/, '').replace(/^\s+/, ''));
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
// 2.0 UI-3：字段值里的 URL 自动转成超链接（丢弃括号内裸 URL，可读文本优先）
var DOMAIN_NAMES = {
  'discogs.com': 'Discogs',
  'www.discogs.com': 'Discogs',
  'musicbrainz.org': 'MusicBrainz',
  'moegirl.org.cn': '萌娘百科',
  'zh.moegirl.org.cn': '萌娘百科',
  'wikipedia.org': '维基百科',
  'zh.wikipedia.org': '维基百科',
  'ja.wikipedia.org': '维基百科 (日文)',
  'en.wikipedia.org': 'Wikipedia',
  'vgmdb.net': 'VGMdb',
  'baike.baidu.com': '百度百科',
  'bilibili.com': '哔哩哔哩',
  'www.bilibili.com': '哔哩哔哩',
  'youtube.com': 'YouTube',
  'www.youtube.com': 'YouTube',
  'spotify.com': 'Spotify',
  'open.spotify.com': 'Spotify',
  'apple.com': 'Apple Music',
  'music.apple.com': 'Apple Music'
};
function friendlyUrlTitle(u, links) {
  u = String(u || '').trim();
  if (Array.isArray(links)) {
    var found = links.find(function (l) { return l && l.url === u; });
    if (found && found.name) return found.name;
    var uNoSlash = u.replace(/\/+$/, '');
    var found2 = links.find(function (l) { return l && l.url && l.url.replace(/\/+$/, '') === uNoSlash; });
    if (found2 && found2.name) return found2.name;
  }
  try {
    var parsed = new URL(u);
    var host = parsed.hostname.toLowerCase();
    var siteName = DOMAIN_NAMES[host] || DOMAIN_NAMES[host.replace(/^www\./, '')] || host;
    var pathname = parsed.pathname;
    if (pathname && pathname !== '/') {
      var parts = pathname.split('/').filter(Boolean);
      if (parts.length > 0) {
        var lastPart = parts[parts.length - 1];
        try {
          var decoded = decodeURIComponent(lastPart).replace(/_/g, ' ');
          if (decoded && decoded !== lastPart && !/^[\d\-_]+$/.test(decoded)) {
            return (siteName ? (siteName + ' · ') : '') + decoded;
          }
          if (decoded && !/^[\d\-_]+$/.test(decoded) && decoded.length < 30) {
            return (siteName ? (siteName + ' · ') : '') + decoded;
          }
        } catch (e) {}
      }
    }
    return siteName;
  } catch (e) {
    return u;
  }
}
function makeLinkTag(href, text) {
  if (!/^https?:\/\//i.test(href)) return esc(text || href);
  return '<a href="' + esc(href) + '" target="_blank" rel="noopener noreferrer" class="eflink" data-url="' + esc(href) + '">' +
         esc(text) + '</a>';
}
function linkifyEscaped(v, links) {
  v = String(v == null ? '' : v);
  if (!v) return '';
  var masterRegex = /([《【][^》】\n\r<>]+[》】]|[^\s,，、；;（(\n\r<>]+)\s*[（(]\s*(https?:\/\/[^\s<>"'）)]+)\s*[）)]|([^\s,，、；;\n\r<>]+[（(][^）)\n\r<>]+[）)])\s*(https?:\/\/[^\s<>"'）)\]】]+)|(https?:\/\/[^\s<>"'）)\]】]+)/gi;
  var out = '', lastIndex = 0, match;
  while ((match = masterRegex.exec(v)) !== null) {
    out += esc(v.slice(lastIndex, match.index));
    lastIndex = masterRegex.lastIndex;
    if (match[1] && match[2]) {
      var textA = match[1];
      var bm = /^(.*?)([《【][^》】]+[》】])$/.exec(textA);
      if (bm) {
        out += esc(bm[1]) + makeLinkTag(match[2].replace(/[.,;。，；]+$/, ''), bm[2]);
      } else {
        out += makeLinkTag(match[2].replace(/[.,;。，；]+$/, ''), textA);
      }
    } else if (match[3] && match[4]) {
      out += makeLinkTag(match[4].replace(/[.,;。，；]+$/, ''), match[3]);
    } else if (match[5]) {
      var clean = match[5].replace(/[.,;。，；]+$/, '');
      var trailing = match[5].slice(clean.length);
      out += makeLinkTag(clean, friendlyUrlTitle(clean, links)) + esc(trailing);
    }
  }
  out += esc(v.slice(lastIndex));
  return out;
}
// 外链点击（事件委托，重绘后不需要重新绑定）
document.addEventListener('click', function (ev) {
  var t = ev.target;
  while (t && t !== document) {
    if (t.tagName === 'A' && t.classList && t.classList.contains('eflink')) {
      ev.preventDefault();
      openExternal(t.getAttribute('data-url') || t.getAttribute('href'));
      return;
    }
    if (t.classList && t.classList.contains('eflink')) { ev.preventDefault(); openExternal(t.dataset.url || t.getAttribute('href')); return; }
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
    // 2026-10-29（#5）：列举型字段按分号拆行 → 每条一行，URL 逐行挂链接（linkify 必须在拆行后逐个做，
    // 否则整段先转义会把 <br> 一起转义掉）。
    var lines = splitListValue(v);
    var vlHtml = '';
    for (var li = 0; li < lines.length; ++li) {
      if (li) vlHtml += '<br>';
      vlHtml += linkifyEscaped(lines[li], opts && opts.links);
    }
    var wide = !!EF_WIDE[lb] || v.length > 46 || lines.length > 1;
    rows += '<div class="efrow' + (wide ? ' wide' : '') + '">' +
            '<div class=lb>' + esc(lb) + '</div><div class=vl>' + vlHtml + '</div></div>';
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
  opts = opts || {};
  if (!opts.links && summary && summary.links) opts.links = summary.links;
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
        b0 += '<div style="font-size:12.5px;line-height:1.7;color:var(--txt);margin-bottom:6px">' + linkifyEscaped(p.text, opts && opts.links) + sups + '</div>';
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
/* 2026-10-27（261027-06 P3）：AI 整理"是否开机"的单一判据。
   两个来源任一为真即可：
     META.summary        —— 设置键 metasummary（前端从 /api/settings 读到的）
     META.summaryEnabled —— 后端 /api/status 回报的**权威**开关（真正决定发不发请求）
   为什么要「或」：本仓历史遗留 —— loadSettings() 用 api('/api/settings') 读，而 api() 对
   /api/* 一律走 POST（2026-10-25 CSRF 加固），POST 只回 {merged:true}、不回 kv ⟹ META.summary
   长期停在默认 0。只认它会让整块 AI 功能在界面上人间蒸发（本轮 ST3 实测就是这么发现的）。
   summaryEnabled 直接来自后端 PlaySettings，不依赖那条读设置的路径。 */
function aiSummaryOn() {
  return META.summary === 1 || META.summaryEnabled === true;
}
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

var albumArtwork = { generation: 0, observer: null, queue: [], active: 0 };
function stopAlbumArtwork() {
  stopCachedCoverImages();
  albumArtwork.generation++;
  albumArtwork.queue = [];
  if (albumArtwork.observer) albumArtwork.observer.disconnect();
  albumArtwork.observer = null;
}
function pumpAlbumArtwork() {
  while (albumArtwork.active < 2 && albumArtwork.queue.length) {
    var item = albumArtwork.queue.shift();
    if (item.visible && item.current() && META.on) item.lookup();
    else item.queued = false;
  }
}
function bindAlbumArtwork(album) {
  var generation = albumArtwork.generation;
  var observer = new IntersectionObserver(function (entries) {
    entries.forEach(function (entry) {
      var item = entry.target.songArtwork;
      item.visible = entry.isIntersecting;
      if (!item.visible || !item.current()) return;
      if (!item.started) {
        item.started = true;
        entry.target.src = item.url;
      } else item.schedule();
    });
  });
  albumArtwork.observer = observer;
  Array.prototype.forEach.call(D('list').querySelectorAll('img[data-song-index]'), function (im) {
    var track = album.tracks[Number(im.dataset.songIndex)];
    if (!track || !track.path) return;
    var file = encodeURIComponent(track.path);
    var item = { url: '/api/cover?file=' + file, visible: false, missing: false, queued: false };
    im.songArtwork = item;
    item.current = function () {
      return im.isConnected && generation === albumArtwork.generation && S.view === 'album' && S.album === album;
    };
    item.schedule = function () {
      if (!item.missing || item.queued || !item.visible || !item.current() || !META.on) return;
      item.queued = true;
      albumArtwork.queue.push(item);
      pumpAlbumArtwork();
    };
    item.lookup = function () {
      albumArtwork.active++;
      var deadline = Date.now() + 60000;
      function done() { albumArtwork.active--; pumpAlbumArtwork(); }
      function poll() {
        if (!item.current() || !item.visible || !META.on) { item.queued = false; done(); return; }
        api('/api/cover/lookup?file=' + file, '').then(function (j) {
          if (item.current() && j && j.ok) {
            if (j.hasCover && typeof j.coverUrl === 'string' && j.coverUrl.indexOf('/api/cover?') === 0 &&
                new URL(j.coverUrl, location.href).searchParams.get('file') === track.path) {
              im.src = j.coverUrl;
            } else if (j.pending && item.visible && META.on && Date.now() < deadline) {
              setTimeout(poll, Math.max(500, Math.min(2500, j.retryAfterMs || 1000)));
              return;
            }
          }
          done();
        }, done);
      }
      poll();
    };
    im.onload = function () { item.cacheLoading = false; item.missing = false; im.style.visibility = 'visible'; };
    im.onerror = function () { item.cacheLoading = false; im.style.visibility = 'hidden'; item.missing = true; item.schedule(); };
    observer.observe(im);
  });
  watchCachedCoverImages();
}
function openAlbum(k) {
  if(typeof rlPageTransitionBegin==='function')rlPageTransitionBegin('album');
  // 261024-05 #T2：打开另一张专辑 = 离开上一张 → 先停掉它的曲目预取队列（含在飞轮询）
  albPrefStop();
  var artworkGeneration = albumArtwork.generation;
  S.view = 'album';
  // 从聆听室点专辑卡时也要把右栏/导航切回浏览态；否则 noq 会把队列永久藏住。
  D('app').classList.remove('noq');applyRTab(false);
  api('/api/lib/album?k=' + encodeURIComponent(k)).then(function (j) {
    if (S.view !== 'album' || artworkGeneration !== albumArtwork.generation) return;
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
            '<img data-album-key="' + esc(j.k) + '"' + (j.cv ? ' src="/api/lib/cover?k=' + encodeURIComponent(j.k) + '"' : '') +
            ' alt="" onerror="this.style.display=\'none\'" style="position:absolute;inset:0;width:100%;height:100%;object-fit:cover;border-radius:inherit">' +
            '</div><div class=info>' +
            '<div class=kind>' + esc(albumIdentityLabel(j) || 'ALBUM') + '</div><h1 title="' + esc(j.name) + '">' + esc(j.name) + '</h1>' +
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
           '<div class="tt album-song-title" title="' + esc(t.title) + '"><span class=album-song-art>' +
           '<img alt="" data-song-index="' + i + '"></span><span class=album-song-label>' + esc(t.title) + '</span></div>' +
           '<div class=ar title="' + esc(t.artist || j.artist) + '">' + esc(t.artist || j.artist) + '</div>' +
           // P2（261027-05 A4）：专辑页曲目行也标多声道 —— /api/lib/album 的 tracks 带 ch 字段
           '<div class=bd>' + fmtTag(t.sr, t.bits, t.ch) + '</div>' +
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
    if (aiSummaryOn()) {
      h += '<div class="sec-card summary-sec" id="albumSummary" data-k="' + esc(j.k) + '"' +
           ' style="margin:8px 22px 24px">' +
           '<div style="font-size:12px;color:var(--mut2)">正在整理资料…</div>' +
           '</div>';
    }
    D('list').innerHTML = h;
    bindAlbumArtwork(j);
    head(j.name, j.tracks.length + ' 首', '<button class=btn id=bBackAlbums title="返回专辑列表" aria-label="返回专辑列表">← 返回专辑</button>');
    if (D('bBackAlbums')) D('bBackAlbums').onclick = function () { loadAlbums(); };

    if (aiSummaryOn()) {
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
                         tf('资料整理失败（{0}）· 点击重试', [esc(s.error || '超时')]) + '</div>';
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
          if (r && r.ok) { toast('封面已设置'); refreshCurrentCover(); openAlbum(j.k); }
          else { toast(tf('设置失败：{0}', [((r && r.error) || '未知原因')])); }
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
  // 2026-10-28（用户真机：AI 把《19:30》挂到不相干的《火花》上）：
  //   **必须带上所属专辑名**。这张碟的 WAV 没有任何标签，旧 URL 只有曲名 ⟹ 后端拿
  //   "19:30" 去搜，全网同名无数，命中了什么全看运气。
  //   专辑名从专辑对象来（专辑页已把它确定下来了），**不用文件夹名去猜**。
  var albName = (alb && alb.name) || (t && t.album) || '';
  return '/api/entity/summary?kind=track&name=' + encodeURIComponent((t && t.title) || '') +
         (ar ? ('&artist=' + encodeURIComponent(ar)) : '') +
         (albName ? ('&album=' + encodeURIComponent(albName)) : '') + '&lang=zh';
}
// 261024-05 #T2：离开专辑页 / 打开另一张专辑 → 立刻停掉剩余队列（含还在 2s 轮询的旧 key）。
// 用户原话：「只在单曲播放页面才预取本曲所属的专辑，不是全部专辑」——专辑页的"整张"只能指
// **当前打开的这张**，绝不能把上一张的待办带进下一张。
function albPrefStop() {
  stopAlbumArtwork();
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
    ? (isOver ? (tf('已整理 {0}/{1}（其余可按需点开）', [ALBPREF.done, ALBPREF.allTotal]))
              : (tf('曲目资料已后台整理 {0} 首', [ALBPREF.total])))
    : (tf('正在后台整理曲目资料 {0} / {1}', [ALBPREF.done, ALBPREF.total]));
  box.innerHTML = '<div class="albprog"><span class=t>' + esc(txt) + '</span>' +
                  '<span class=bar><i style="width:' + pct + '%"></i></span></div>';
}
/* 2026-10-27（用户 261027-06 P2）：「整理资料」（专辑本身）与「整理曲目资料」（逐首）
   原来是**共用 2 个槽位**的 —— 专辑那条一占，曲目侧就只剩 1 条在跑，用户看到的就是
   「一个是正在整理资料、一个是正在后台整理曲目资料，好像是顺序执行」。
   现在两件事各有独立预算：
     * 曲目批任务：最多 ALB_PREF_MAX(=3) 条同时飞，不再看 SUMC.inflight 的总数；
     * 专辑/艺术家/单曲那几条：本来就是每个实体 1 条（summaryOnce 单飞），不受批任务影响。
   峰值 4 条在飞，对 DeepSeek 的 5 QPS（retrieval 侧还有 220ms 限频器）完全在额度内，
   而总耗时从「专辑 + N×单条」变成 ≈ max(专辑, 曲目批次)。 */
var ALB_PREF_MAX = 3;
function albPrefPump() {
  var gen = ALBPREF.gen;
  while (ALBPREF.active < ALB_PREF_MAX && ALBPREF.queue.length) {
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
  if (!aiSummaryOn()) return;
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
  if (!ensurePlaybackSettings()) return;
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
// 低饱和圆形占位里显示什么（2026-10-27 261027-03 #7，用户复测后的修正）：
// 旧版这里是一个 opacity .35 的音符图标 —— 用户实测「整页看着就是空白的」。
// 本轮用户明确要求「每张头像位置**立刻**渲染首字母占位（带艺人名首字）」，
// 而 2026-10-26 的另一条要求是「不要大灰块 / 不许用专辑封面顶替」
// ⟹ 取交集：**淡描边圆形 + 首字**，不做灰底块、不借任何封面。
function artistInitialHtml(name, size) {
  var s = String(name || '').trim();
  var ch = '';
  try { ch = Array.from(s)[0] || ''; } catch (e) { ch = s.charAt(0); }
  if (!ch) ch = '音';
  return '<span class="aini" style="font-size:' + (size || 22) + 'px">' + esc(ch) + '</span>';
}
// 兼容旧调用（详情页曾在别处引用过音符图标）
function artistNoteSvg(size) { return artistInitialHtml('', size); }
var ARTAV = { total: 0, ok: 0, miss: 0, timer: 0, round: 0, gen: 0, queue: [], deadline: 0, stopped: false, lastOkAt: 0, hideGen: -1 };
/* ---- 头像加载泵（2026-10-27 261027-03 #7）------------------------------------------
   用户本轮原话：「头像刚刚一直没出现…已经出现了。所以问题是加载时间太长，以及加载没有任何
   提示，也没有逐个出现，导致我判断出了问题」。旧实现（上面这段历史里那句「每轮重放 ≤48 张」）
   实测有三个问题：
     ① **一次性 128 个 <img> 同时塞 src**：后端 WebUi 只有 6 个 worker、listen backlog 只有 8，
        冷缓存时并发连接把 accept 队列顶满 —— 实测 127 个并发 GET 会直接 connect refused，
        浏览器端则表现为「一直转、迟迟不出图」；
     ② **占位几乎看不见**（opacity .35 的音符图标）⟹ 图没到时整页就是空白；
     ③ **进度算不出终态**：X 只数「已成功」，缺图的永远补不上 ⟹ 实测卡在
        「头像补齐中 113 / 128 位」25 s 不下来；而且每 1.8 s 把 48 张**在途**的 src 重设一遍
        （等于把正在下载的请求全部掐掉重来），慢网下永远收敛不了。
   新实现：
     · 占位（首字 + 淡描边圆）**立刻**出现，不依赖任何网络；
     · 图片由泵按 6 并发逐个喂（与后端 worker 数对齐），每张一落地就自己替换占位
       （CSS opacity 过渡 ⟹ 看得出是「一张一张出现」）；
     · 失败按 0.9 s→8 s 退避重试，超过次数或总时限（150 s）才定案成「暂无图源」；
     · 进度 = 已定案（成功 + 确定没有）/ 总数 ⟹ 数字一直在动，且有明确终态。 */
var ARTA_CONC = 6;                 // 同时最多 6 张（后端 WebUi 是 6 个 worker，再多只是排队）
var ARTA_RETRIES = 4;
// 2026-11-14（3.3.11）：快速重试用尽后的"慢速尾巴"（见 artaSettle 里的注释）
var ARTA_SLOW_RETRIES = 24;   // 最多再慢试 24 次
var ARTA_SLOW_MS = 15000;     // 每次间隔 15 s ⟹ 覆盖约 6 分钟的后台抓图窗口
// 终态收敛阈值：旧值直接复用 ARTA_IDLE_MS(8 s)，会把慢速尾巴刚排上的重试立刻判死。
// 现在只有「连续 30 s 一张新图都没进来」才收尾 —— 后台真停了就 30 s 内给完成态，
// 后台还在供货就继续补图（进度条一直在动，不会出现"卡住"的观感）。
var ARTA_FINAL_IDLE_MS = 30000;      // 2026-10-29：后端有负缓存，缺图会稳定 404，重试 40 次纯属拖延             // 单张最多重试次数
var ARTA_MAXWAIT_MS = 150000;      // 单页总时限：到点就如实收尾，不再无限转
// 「多久没有新图落地就收尾」：冷缓存时预热还在供货（一张 1~3 s），不能过早判死；
// 供货停了 25 s 就定案成「暂无图源」，进度条进入完成态（用户要求：失败也要有终态）。
var ARTA_IDLE_MS = 8000;     // 2026-10-29：25 s 太久，用户看到的就是「卡在 114」
// 2026-10-29（管理员反馈 261029-01 #1 #2）：头像进度条每次切页都重置、且卡在 112/128。
//   根因：每进一次艺术家页 artaStart() 都把计数清零重跑，而「确实没有图源」的那十几位
//   要走完 25 s 空闲判定才定案 ⟹ 观感就是「进度条归零 → 又卡在 112」。
//   修法：把已定案的「无图源」记进内存负缓存 ARTA_MISS，重进页面直接出完成态，不再重跑。
//   只有用户主动点「重试」才清空它，真去重新抓一遍。
// 2026-10-29（管理员 261029-02 #1/#2 复测：仍然卡 114/128、切页仍重置）：
//   上一版只加了「已定案无图源」的负缓存，但真因在别处：切页后 DOM 消失 ⟹ artaTick 里
//   `if (!D('artProgWrap')) { artaStop(); return; }` 把泵停掉 ⟹ 那 14 张 pending **永远走不到定案**
//   ⟹ 负缓存从来没被写入过 ⟹ 每次进来都从 0 重算、又卡在 114。
//   本轮三处一起改：① 泵不再因切页而停；② 正/负缓存都持久化；③ 定案时间从 25 s 收到 8 s。
var ARTA_MISS = {};   // name -> 1：已定案「没有图源」
var ARTA_OK = {};     // name -> 1：已成功过（重进页面直接计 ok，不再从 0 重算）
function artaStop() {
  if (ARTAV.timer) { clearInterval(ARTAV.timer); ARTAV.timer = 0; }
  ARTAV.gen++;
  ARTAV.queue = [];
}

// 2026-10-29（用户反馈 FB-20261028-CMGW7 #2）：
//   「加载文件夹后，头像不会自动加载，非要我点击艺术家页面后才会启动加载。而且切换页面后，还会打断。」
//   旧行为有两处问题：
//     ① 预热只在 loadArtists() 里发 ⟹ 别的页面永远不会去抓；
//     ② loadArtists() 开头就 artaStop() ⟹ 每次切页都把正在跑的泵掐掉，退回重来。
//   修法：
//     · 曲库就绪后就**后台预热一次**（与当前在哪个页面无关）；
//     · 泵本身不再随页面切换而死 —— 页面离开只是没有 DOM 可画，泵继续把图喂进浏览器缓存，
//       回来时直接命中，肉眼就是「一进艺术家页就有图」。
var ARTA_BG = { started: 0, timer: 0 };

// 后台预热：只发一次。失败不重试（后端自己有并发上限，重复发只是排队）。
function artaWarmBackground() {
  if (ARTA_BG.started) return;
  ARTA_BG.started = 1;
  api('/api/artist/prefetch').catch(function () { ARTA_BG.started = 0; });
}

// 曲库刚扫完/刚加载时调用：给它一点时间，别和首屏抢带宽。
function artaWarmSoon() {
  if (ARTA_BG.timer) return;
  ARTA_BG.timer = setTimeout(function () {
    ARTA_BG.timer = 0;
    artaWarmBackground();
  }, 3000);
}
function artaRender() {
  var box = D('artProgWrap');
  if (!box) return;
  if (!ARTAV.total) { box.innerHTML = ''; return; }
  var settled = ARTAV.ok + ARTAV.miss;
  var done = settled >= ARTAV.total;
  // 2026-11-01（用户 261101 #1 追加）：全部命中缓存/负缓存时**根本不要出现这条** ——
  //   旧实现会先渲染一次完成态再等 2.5 s 收起，用户看到的就是"每次刷页面闪一下"。
  if (done && !ARTAV.fetched) { box.innerHTML = ''; return; }
  var pct = Math.round(settled / ARTAV.total * 100);
  var txt;
  if (done) {
    // 有缺图就如实写「暂无图源」，绝不写成「已补齐」（铁律 4）
    txt = ARTAV.miss
      ? (tf('头像已加载 {0} / {1} 位（{2} 位暂无图源，保留首字占位）', [ARTAV.ok, ARTAV.total, ARTAV.miss]))
      : (tf('头像已加载 {0} / {1} 位', [ARTAV.ok, ARTAV.total]));
  } else {
    txt = tf('头像加载中 {0} / {1} 位（逐张出现，缺图会自动重试）', [ARTAV.ok, ARTAV.total]);
  }
  box.innerHTML = '<div class="albprog artprog' + (done ? ' done' : '') + '">' +
                  '<span class=t>' + esc(txt) + '</span>' +
                  '<span class=bar><i style="width:' + pct + '%"></i></span>' +
                  (done && ARTAV.miss ? '<button class="btn artagain" id=artAgain>重试</button>' : '') +
                  '</div>';
  if (D('artAgain') && !D('artAgain').onclick) D('artAgain').onclick = function () {
    // 用户主动重试：清掉负缓存，真去重新抓一遍（否则点了也还是那 112 位）
    ARTA_MISS = {};
    artaStart();
  };
  // 2026-11-01（用户 261101 #2）：跑完就**收起这一条**，不要永远挂在那里。
  //   用户原话：「如果已经加载完成，那为什么不隐藏这一栏？专辑页面就没有类似的东西。」
  //   专辑页的封面进度就是 2.5 s 后自己消失的（covProg），这里对齐同一行为。
  //   缺图信息仍然如实存在（标题栏的「128 位」计数 + 首字母占位），不靠这条横幅才成立。
  if (done && ARTAV.hideGen !== ARTAV.gen) {
    ARTAV.hideGen = ARTAV.gen;
    var g0 = ARTAV.gen;
    setTimeout(function () {
      if (g0 !== ARTAV.gen) return;          // 期间又开了新一轮：不要误清
      var b = D('artProgWrap');
      if (b) b.innerHTML = '';
    }, 2500);
  }
}
function artaSettle(st, ok) {
  if (st.state === 'ok' || st.state === 'miss') return;
  if (ok) {
    st.state = 'ok';
    st.im.classList.add('ok');
    st.im.classList.remove('err');
    if (st.n) ARTA_OK[st.n] = 1;   // 记住成功过：重进页面直接计 ok，不再从 0 重算
    ARTAV.ok++;
    ARTAV.lastOkAt = Date.now();   // 有新图落地 ⟹ 供货还没停，别急着收尾
    artaRender();
    return;
  }
  st.tries++;
  if (st.tries <= ARTA_RETRIES && Date.now() < ARTAV.deadline) {
    st.state = 'pending';
    st.due = Date.now() + Math.min(8000, 900 * st.tries);
    return;
  }
  // 2026-11-14（3.3.11，用户反馈「艺术家头像全部不见了」）：
  // 快速重试 4 次（≈9 s）用完就定案成"无图源"并写死负缓存 —— 但**全新曲库首次进页**时
  // 后台抓头像要跑好几分钟（实测 09:32 开始、09:37:07 才 done），9 s 根本等不到。
  // 结果：128 位全部显示首字占位，而且负缓存写死，只有手动刷新页面才会恢复。
  // 修法：快速重试用尽后进"慢速尾巴" —— 只要后台还在陆续出图（lastOkAt 在 ARTA_IDLE_MS 内），
  // 就每 ARTA_SLOW_MS 再试一次，最多 ARTA_SLOW_RETRIES 次（≈6 分钟），不再提前写死负缓存。
  st.slow = st.slow || 0;
  if (st.slow < ARTA_SLOW_RETRIES && Date.now() - ARTAV.lastOkAt <= ARTA_IDLE_MS) {
    st.slow++;
    st.state = 'pending';
    st.due = Date.now() + ARTA_SLOW_MS;
    return;
  }
  st.state = 'miss';
  st.im.classList.add('err');
  if (st.n) ARTA_MISS[st.n] = 1;    // 记入负缓存：下次进页面直接定案，见 #1/#2 注释
  ARTAV.miss++;
  artaRender();
}
function artaTick(gen) {
  if (gen !== ARTAV.gen) return;
  // 2026-10-29（261029-02 #1/#2）：切页后 artProgWrap 不存在 —— 旧代码在这里 artaStop() 直接掐泵，
  //   于是那十几张 pending 永远等不到「定案」，负缓存永远不写入，下次进来又从头跑一遍。
  //   现在：没有 DOM 就只跳过渲染（artaRender 内部自带空判），**泵继续跑完定案**。
  var now = Date.now(), busy = 0, allDone = true;
  for (var i = 0; i < ARTAV.queue.length; i++) {
    var st = ARTAV.queue[i];
    if (st.state === 'ok' || st.state === 'miss') continue;
    allDone = false;
    if (st.state === 'inflight') busy++;
  }
  // 终态收敛（用户要求「失败也要有终态，不要一直转圈」）：只要还有成功进来，就说明预热
  // 还在供货，继续等；一旦连续 ARTA_IDLE_MS 没有新图落地，就把还在重试的按「暂无图源」
  // 定案，进度条进入「已加载 x / y」完成态（并给一个重试入口）。
  if (!allDone && now - ARTAV.lastOkAt > ARTA_FINAL_IDLE_MS) {
    for (var j = 0; j < ARTAV.queue.length; j++) {
      var s3 = ARTAV.queue[j];
      if (s3.state === 'ok' || s3.state === 'miss') continue;
      s3.state = 'miss';
      s3.im.classList.add('err');
      // 2026-10-29（261029-02 #2 复测：切页后又回到「加载中 114/128」）：
      //   定案分支**必须也写负缓存**，否则切页后 ARTAV 清零、负缓存还是空的，
      //   于是又从头跑一遍再卡在同一个数字。上一版只写在 artaSettle 里，漏了这里。
      if (s3.n) ARTA_MISS[s3.n] = 1;
      ARTAV.miss++;
    }
    if (ARTAV.timer) { clearInterval(ARTAV.timer); ARTAV.timer = 0; }
    ARTAV.stopped = true;
    artaRender();
    return;
  }
  if (allDone) {
    if (ARTAV.timer) { clearInterval(ARTAV.timer); ARTAV.timer = 0; }
    ARTAV.stopped = true;
    artaRender();
    return;
  }
  for (var k = 0; k < ARTAV.queue.length && busy < ARTA_CONC; k++) {
    var s2 = ARTAV.queue[k];
    if (s2.state !== 'pending' || s2.due > now) continue;
    s2.state = 'inflight';
    busy++;
    ARTAV.round++;
    ARTAV.fetched = 1;                 // 本轮真的发过硬请求（决定横幅要不要出现，见 artaRender）
    s2.im.src = artistImgUrl(s2.n, ARTAV.round);
  }
}
// 渲染完立刻调：占位已就位，这里只负责「把图一张一张喂进来」
function artaStart() {
  artaStop();
  var imgs = document.querySelectorAll('#list .card .acv img');
  ARTAV.total = imgs.length; ARTAV.ok = 0; ARTAV.miss = 0;
  ARTAV.queue = []; ARTAV.round = 0; ARTAV.stopped = false;
  ARTAV.deadline = Date.now() + ARTA_MAXWAIT_MS;
  ARTAV.lastOkAt = Date.now();
  ARTAV.fetched = 0;
  var gen = ARTAV.gen;
  Array.prototype.forEach.call(imgs, function (im) {
    var nm = im.getAttribute('data-n') || '';
    var st = { im: im, n: nm, state: 'pending', tries: 0, due: 0 };
    ARTAV.queue.push(st);
    // #1/#2：上次已经定案「没有图源」的，直接判 miss —— 不再重走一遍退避等待，
    // 这样重进页面时进度条立刻就是完成态，不会出现「从 0 重算再卡在 114」。
    if (nm && ARTA_OK[nm]) {
      st.state = 'ok';
      im.classList.add('ok');
      ARTAV.ok++;
      // 图从浏览器缓存取；万一缓存被清/后端删了缓存，退回正常重试（不能让计数说谎）
      im.onerror = function () {
        if (gen !== ARTAV.gen) return;
        delete ARTA_OK[nm];
        setTimeout(function () { artaStart(); }, 0);
      };
      im.src = artistImgUrl(nm, 0);
      return;
    }
    if (nm && ARTA_MISS[nm]) {
      st.state = 'miss';
      im.classList.add('err');
      ARTAV.miss++;
      return;
    }
    im.onload = function () {
      if (gen !== ARTAV.gen) return;
      artaSettle(st, im.naturalWidth > 0);
    };
    im.onerror = function () { if (gen === ARTAV.gen) artaSettle(st, false); };
  });
  artaRender();
  if (!ARTAV.total) return;
  // 全都已定案（全部命中负缓存或本身没有 img）⟹ 不起泵，进度条直接停在完成态
  if (ARTAV.ok + ARTAV.miss >= ARTAV.total) return;
  ARTAV.timer = setInterval(function () { artaTick(gen); }, 120);
  artaTick(gen);
}
// 详情页 hero（单张图）：占位在底下，图到了就淡入；失败留首字
function artaHeroBind() {
  Array.prototype.forEach.call(document.querySelectorAll('.ahead .acv img'), function (im) {
    if (!im.getAttribute('src')) im.src = artistImgUrl(im.getAttribute('data-n'), 0);
    im.onload = function () { if (im.naturalWidth > 0) { im.classList.add('ok'); im.classList.remove('err'); } };
    im.onerror = function () { im.classList.add('err'); };
    if (im.complete && im.naturalWidth > 0) im.classList.add('ok');
  });
}
// 兼容旧调用名（别再新增调用点）：等价于「重开一轮泵」
function artaBind() { artaStart(); }
function artaPoll() { /* 轮询已被泵取代（见 artaStart）：保留空函数只为兼容旧调用点 */ }

/* ---------------- 艺术家 ---------------- */
// 2026-10-26（261026-05 #1）：支持按关键词过滤（搜索栏在艺术家页搜索时用）。
// q 为空 = 全部；非空 = 只保留名字包含关键词的艺术家（不区分大小写）。
var artistListRequest = 0;
function loadArtists(q) {
  var request = ++artistListRequest;
  if(typeof rlPageTransitionBegin==='function')rlPageTransitionBegin('artists');
  S.view = 'artists';
  var kw = String(q || '').trim().toLowerCase();
  // 2026-10-29：#2 不再在这里 artaStop() —— 切页就把泵掐掉是「切换页面还会打断」的根因。
  // 预热也改成复用后台那一次（artaWarmBackground 自带去重）。
  artaWarmBackground();
  head('艺术家', '加载中…', '');
  // 2026-10-26（261026-01 #1）：艺术家头像走外部源，但**抓取必须后台做** ——
  // WebUi 是单线程 accept，如果在图片请求里同步抓，246 个头像会把整站串行阻塞掉。
  // 这里只做一件事：告诉后端"开始预热这些名字"，图片端点本身只读缓存（未命中立刻 404）。
  // 2026-10-27（261027-03 #7）：预热照旧开跑，但**页面不再等它** —— 占位立刻渲染，
  // 图片由 artaStart() 的泵逐个喂（后端缓存命中的图几百毫秒内就到，缺图的按退避重试）。
  artaWarmBackground();   // 去重版（首次进艺术家页时后台可能已经在跑）
  api('/api/lib/albums').then(function (j) {
    if (S.view !== 'artists' || request !== artistListRequest) return;
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
      head('艺术家', tf('{0} 位（搜索：{1}）', [order.length, q]), '');
    } else {
      head('艺术家', order.length + ' 位', '');
    }
    if (!order.length) { D('list').innerHTML = '<div class=empty>库里还没有音乐</div>'; return; }
    var h = '<div id=artProgWrap></div>';
    h += '<div class=grid style="grid-template-columns:repeat(auto-fill,minmax(150px,1fr))">';
    order.forEach(function (k) {
      var v = m[k];
      // 2026-10-26（261026-01 #1）+ 2026-10-27（261027-03 #7）：
      // 用户两条要求叠在一起 —— ①「取不到就留空，不许用专辑封面顶替」②「每张头像位置**立刻**
      // 渲染首字母占位（带艺人名首字），不要留空白」⟹ 占位 = 淡描边圆 + 首字（不是旧版的灰块，
      // 也不是上一版那个几乎看不见的音符图标）。
      // <img> 永远留在 DOM 里（失败只是 opacity:0，不删不摘）⟹「img 数 === 艺术家数」可被断言。
      // ★ **不在 HTML 里写 src**：由 artaStart() 按 6 并发逐张喂（见上面的加载泵注释），
      //   这样图是一张一张出现的，也不会一次把 128 个连接怼到后端。
      h += '<div class=card data-a="' + esc(k) + '">' +
           '<div class="cv acv">' +
             '<div class=aph>' + artistInitialHtml(k, 22) + '</div>' +
             '<img data-n="' + esc(k) + '" class=aimg alt="">' +
           '</div>' +
           '<div class=nm title="' + esc(k) + '">' + esc(k) + '</div>' +
           '<div class=ar>' + v.n + ' 张专辑</div></div>';
    });
    h += '</div>';
    D('list').innerHTML = h;
    // 渲染完立刻开泵：占位已在 DOM 里，图一张一张补进来，进度条同步走
    artaStart();
    Array.prototype.forEach.call(D('list').querySelectorAll('.card'), function (e) {
      e.onclick = function () {
        var who = e.dataset.a;
        openArtistProfile(who);
      };
    });
  }).catch(function (e) {
    if (S.view !== 'artists' || request !== artistListRequest) return;
    renderPanelError('artists', function () { loadArtists(); }, e);
  });
}
function renderAlbumSubset(who) {
  S.view = 'albums';
  head(who, S.albums.length + ' 张专辑', '<button class=btn id=bArtistBio>艺术家资料</button> <button class=btn id=bAll>返回全部专辑</button>');
  var h = '<div class=grid>';
  S.albums.forEach(function (a, i) { h += albumCardHtml(a, i); });
  D('list').innerHTML = h + '</div>';
  watchCachedCoverImages();
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
         '<div class=aph>' + artistInitialHtml(who, 40) + '</div>' +
         '<img data-n="' + esc(who) + '" class=aimg alt="">' +
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
  if (aiSummaryOn()) {
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
  // 261027-03 #7：hero 头像也走「占位先出、图到了再淡入」（img 的 src 由这里补上）
  artaHeroBind();

  var wantSumm = aiSummaryOn();
  var summState = { done: false, res: null, enabled: wantSumm, skipBio: false };
  var uSumm = '/api/entity/summary?kind=person&name=' + encodeURIComponent(who) +
              '&lang=' + encodeURIComponent(I18N.lang || 'zh') +
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
                     tf('资料整理失败（{0}）· 点击重试', [esc(ss.error || '超时')]) + '</div>';
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
      subText = tf('缓存 · {0} 取回{1}', [(firstDate || '近期'), (j.offline ? ' · 离线' : '')]);
    } else {
      // 261024-04 #5：去掉「权威」二字（用户要求全文案不再出现「权威资料 / 权威词条」）
      subText = tf('资料{0}', [(firstDate ? (' · ' + firstDate + ' 取回') : '')]);
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
  if (aiSummaryOn()) {
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
    if (!aiSummaryOn() || META.hasLlmKey !== true) {
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
           '<span style="font-size:11.5px;color:var(--mut2);margin-right:auto">' + (fDate ? (tf('取回于 {0}', [fDate])) : '') + '</span>' +
           citationChipHtml(m.url, '复制链接', false) +
           '</div>';
    });
    h += '</div></div>';
  }

  h += '</div>';
  D('list').innerHTML = h;

  // 绑定事件（261024-04 #5：与基础骨架共用 bindArtistButtons，两处不再各写一遍）
  bindArtistButtons(who);
  // ★ 2026-10-27（261027-03 #7 第二处真根因）：这个「资料回来后的整页重绘」也会重建 hero，
  //   但**没有重新给 <img> 挂 src** ⟹ 打开艺术家页约 1 s 后（knowledge 返回那一刻）头像就消失，
  //   只剩首字占位。实测：t=0.6 s 时 naturalWidth=300，t=2.5 s 变 0。必须在这里补一次绑定。
  artaHeroBind();

  // 复制链接芯片：navigator.clipboard.writeText(url) + toast「已复制来源链接」
  bindCopyChips(D('list'));
}

/* ---------------- 古典作品与版本推荐（P3④b Discogs L4 / 261023-03 第4条） ---------------- */
function loadWorksView(initialTitle, initialComposer, fromNp) {
  if(typeof rlPageTransitionBegin==='function')rlPageTransitionBegin('works');
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
  if(typeof rlPageTransitionBegin==='function')rlPageTransitionBegin('work');
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
  var subStr = (composer ? (tf('作曲家: {0} · ', [esc(composer)])) : '') +
               (j.from_cache ? (tf('缓存 · {0}{1}', [fmtDate(j.generated_at), (j.offline ? ' · 离线' : '')])) : 'Discogs 实时') +
               tf(' · 不同演绎 {0} / 发行版本 {1}', [distinctCount, releaseCount]);
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
var trackListRequest = 0;
function loadTracks(q) {
  var request = ++trackListRequest;
  if(typeof rlPageTransitionBegin==='function')rlPageTransitionBegin('tracks');
  S.view = 'tracks';
  S.tracksQuery = q || '';
  // 评审：搜索结果页顶栏搜索框不回显当前关键词 → 这里同步（用户正在输入时值本来就相同，不会打断输入）
  if (D('q') && String(D('q').value) !== String(q || '')) D('q').value = q || '';
  head(q ? (tf('搜索：{0}', [q])) : '曲目', '搜索中…', '');
  api('/api/lib/search?q=' + encodeURIComponent(q || '')).then(function (j) {
    if (S.view !== 'tracks' || request !== trackListRequest) return;
    var t = j.tracks || [];
    S.currentTracks = t;
    renderTracksView();
  }).catch(function (e) {
    if (S.view !== 'tracks' || request !== trackListRequest) return;
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
  // ★ 261027-03 #3（用户：「我根本没有添加进入队列的方法」）：
  //   曲目页/搜索页原来**没有任何可见的入队控件**（实测 300 行 0 个），只能靠猜右键，
  //   而右键又是不弹菜单、直接加入 —— 用户既找不到入口、也不知道发生了什么。
  //   现在每行行末有一个常显「＋」（.add-btn，与专辑页同一套），右键改成正经菜单。
  var addBtn = e.querySelector('.add-btn');
  if (addBtn) {
    addBtn.onclick = function (ev) {
      ev.stopPropagation();
      var x = sorted[+e.dataset.i] || {};
      addPathCue(x.path, x.cs, x.ce);
    };
  }
  e.onclick = function (ev) {
    if (ev.target.closest && ev.target.closest('.fav-btn')) return;
    if (ev.target.closest && ev.target.closest('.add-btn')) return;
    var x = sorted[+e.dataset.i] || {};
    playPath(x.path, x.cs, x.ce);
  };
  e.oncontextmenu = function (ev) {
    ev.preventDefault();
    // ★ 必须 stopPropagation：文件末尾那个 document 级 contextmenu 监听器（"点别处关菜单"）
    //   只认 .qi，会把刚弹出的菜单立刻 hideContextMenu() 掉 —— 实测右键曲目行"菜单一闪就没有"。
    ev.stopPropagation();
    var x = sorted[+e.dataset.i] || {};
    // 菜单（立即播放 / 加入播放队列 / 分析音质…），不再"右键即加"
    showTrackMenu(ev.clientX, ev.clientY, x);
  };
}

function renderTracksView() {
  if (S.view !== 'tracks') return;
  var t0 = performance.now();
  var t = S.currentTracks || [];
  var q = S.tracksQuery || '';
  var totalCount = t.length;
  head(q ? (tf('搜索：{0}', [q])) : '曲目', totalCount + ' 首', '');
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
      // 261027-03 #3：多一列 26px 放常显「＋」（列宽与专辑页曲目行一致，窄屏媒体查询已同步）
      chunkHtml += '<div class="trow trow-track" data-i=' + i + ' style="grid-template-columns:28px 24px minmax(120px,1.6fr) minmax(0,1fr) 92px 58px 26px;gap:8px">' +
                   '<div class=n>' + (i + 1) + '</div>' +
                   favIconHtml(x.path, isFav) +
                   '<div class=tt title="' + esc(x.title) + '">' + esc(x.title) + '</div>' +
                   '<div class=ar title="' + esc(x.artist + ' \u2014 ' + x.album) + '">' + esc(x.artist) + ' \u2014 ' + esc(x.album) + '</div>' +
                   // P2（261027-05 A4）：曲目页同样标（后端 /api/lib/search 目前不带 ch，
                   // 所以这里只对带 ch 的响应生效；专辑页那条是确定能标出来的路）
                   '<div class=bd>' + fmtTag(x.sr, x.bits, x.ch) + '</div>' +
                   '<div class=du>' + fmt(x.dur) + '</div>' +
                   '<button class="add-btn" title="加入播放队列" aria-label="加入播放队列">+</button></div>';
    }
    var temp = document.createElement('div');
    temp.innerHTML = chunkHtml;
    while (temp.firstChild) {
      var node = temp.firstChild;
      bindTrackRow(node, sorted);
      rowsContainer.appendChild(node);
    }
    rendered = next;
    var sub = sorted.length > BATCH ? (sorted.length + tf(' 首 · 已显示 {0} 首', [rendered])) : (sorted.length + ' 首');
    head(q ? (tf('搜索：{0}', [q])) : '曲目', sub, '');
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
  if(typeof rlPageTransitionBegin==='function')rlPageTransitionBegin('files');
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
  if(typeof rlPageTransitionBegin==='function')rlPageTransitionBegin('recent');
  S.view = 'recent';
  head('最近播放', '加载中…', '');
  api('/api/recent').then(function (j) {
    var it = j.items || [];
    head('最近播放', it.length + ' 首', '');
    if (!it.length) { D('list').innerHTML = '<div class=empty>还没有播放记录</div>'; return; }
    var h = '<div style="padding:6px 0 24px">';
    it.forEach(function (x, i) {
      // 261027-04 #2：用户「每一行的行距不一样，主要是专辑名称那些占用的行数不一样」。
      // 旧版三宗罪：① 四列用 inline grid-template-columns 写死 34px 1fr 160px 92px（没有 minmax(0,..)，
      //   长内容可以把列撑开）；② .du（专辑）**只有 text-align:right**，没有 nowrap/ellipsis ⟹
      //   长专辑名在 92px 里折 2~3 行，行高 32/50/68px 各不相同；③ 行高不固定，随内容浮动。
      // 现在：class=rrow（CSS 里写死 height:44px + 四列都 minmax(0,..)+nowrap+ellipsis），
      //   曲名/艺术家/专辑各自单行省略，全名走 title（行 div 的 title 仍给完整路径）。
      h += '<div class="trow rrow" data-i=' + i + ' title="' + esc(x.path) + '">' +
           '<div class=n>' + (i + 1) + '</div>' +
           '<div class=tt title="' + esc(x.title || nam(x.path)) + '">' + esc(x.title || nam(x.path)) + '</div>' +
           '<div class=ar title="' + esc(x.artist) + '">' + esc(x.artist) + '</div>' +
           '<div class=du title="' + esc(x.album) + '">' + esc(x.album) + '</div></div>';
    });
    D('list').innerHTML = h + '</div>';
    Array.prototype.forEach.call(D('list').querySelectorAll('.trow'), function (e) {
      e.onclick = function () { playPath(it[+e.dataset.i].path); };
      e.oncontextmenu = function (ev) { ev.preventDefault(); showTrackMenu(ev.clientX, ev.clientY, { path: it[+e.dataset.i].path }); };
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
  if (!ensurePlaybackSettings()) return;
  files = (files || []).filter(function (f) { return !!f; });
  if (!files.length) { toast('这个列表是空的'); return; }
  var i = parseInt(start, 10) || 0;
  if (i < 0) i = 0;
  if (i > files.length - 1) i = files.length - 1;
  ensureSequentialMode(function () {
    api('/api/queue/set', 'files=' + encodeURIComponent(files.join(String.fromCharCode(10)))).then(function (j) {
      if (!j || j.ok === false) { toast((j && j.error) || '建立队列失败'); return; }
      playPath(files[i]);
      toast(tf('从第 {0} 首开始顺序播放{1}（共 {2} 首）', [(i + 1), (label ? ('：' + label) : ''), files.length]));
      if (S.view === 'queue') renderQueuePage();
    }).catch(function (e) { toast(tf('播放失败: {0}', [e])); });
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
  if(typeof rlPageTransitionBegin==='function')rlPageTransitionBegin('favs');
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
  }).catch(function (e) { toast(tf('取消收藏失败: {0}', [e])); });
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
  var box = D('sqList'); if (!box) return;
  // 2026-11-12：复刻 QQ 音乐 —— 标题右侧「+」新建，歌单行双击改名，单击进详情。
  var h = '<div class=sqhd><span>我的歌单</span><button type=button class=sqadd id=sqNewBtn title="新建歌单" aria-label="新建歌单">+</button></div>';
  if (!S.sqs.length) h += '<div class=sqempty>暂无歌单，点 + 新建</div>';
  S.sqs.forEach(function (q, i) {
    h += '<div class="nav sqrow" data-v=sq data-sq="' + esc(q.name) + '" data-i=' + i + ' title="双击改名">' +
         '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" aria-hidden="true">' +
           '<path d="M4 7h9M4 12h9M4 17h6"/>' +
           '<path d="M18.6 16.6V7.3l3.2-.9"/>' +
           '<circle cx="17" cy="16.8" r="1.8"/>' +
         '</svg>' +
         '<span class=sqn title="' + esc(q.name) + '">' + esc(q.name) + '</span>' +
         '<span class="sqb ren" title="重命名">✎</span>' +
         '<span class="sqb del" title="删除">×</span></div>';
  });
  box.innerHTML = h;
  if (D('sqNewBtn')) D('sqNewBtn').onclick = function (ev) { ev.stopPropagation(); createPlaylistDialog(); };
  Array.prototype.forEach.call(box.querySelectorAll('.sqrow'), function (e) {
    var q = S.sqs[+e.dataset.i]; if (!q) return;
    var clickTimer = 0;
    e.onclick = function (ev) {
      if (ev.target.closest && ev.target.closest('.sqb')) return;
      clearTimeout(clickTimer);
      clickTimer = setTimeout(function () { renderSavedQueuePage(q.name); }, 220);
    };
    e.ondblclick = function (ev) {
      if (ev.target.closest && ev.target.closest('.sqb')) return;
      clearTimeout(clickTimer); renameQueueDialog(q.name);
    };
    var r1 = e.querySelector('.ren'); if (r1) r1.onclick = function (ev) { ev.stopPropagation(); renameQueueDialog(q.name); };
    var r2 = e.querySelector('.del'); if (r2) r2.onclick = function (ev) { ev.stopPropagation(); deleteSavedQueue(q.name); };
  });
}
function renderSavedQueuePage(name) {
  if (!name) return;
  if(typeof rlPageTransitionBegin==='function')rlPageTransitionBegin('sq');
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
  if (!ensurePlaybackSettings()) return;
  i = i || 0;
  ensureSequentialMode(function () {
    api('/api/sq/play?name=' + encodeURIComponent(name) + '&i=' + i).then(function (j) {
      if (!j || !j.ok) { toast((j && j.error) || '载入失败'); return; }
      api('/api/sq/info?name=' + encodeURIComponent(name)).then(function (info) {
        var it = (info && info.items) || [];
        var k = Math.min(Math.max(0, i), Math.max(0, it.length - 1));
        if (it[k]) playPath(it[k].file, it[k].cs, it[k].ce);
        toast(tf('播放队列：{0}（{1} 首）', [name, it.length]));
        loadQ();
      });
    }).catch(function (e) { toast(tf('播放失败: {0}', [e])); });
  });
}
function createPlaylistDialog(onCreated) {
  askText({ title: '新建歌单', ok: '新建', placeholder: '例如 夜深人静', hint: '只新建一个空歌单，不会改动当前播放队列。', onOk: function (name) {
    api('/api/sq/create?name=' + encodeURIComponent(name)).then(function (j) {
      if (!j || !j.ok) throw Error((j && j.error) || '新建失败');
      toast(tf('已新建歌单：{0}', [name]));
      renderSavedQueues();
      if (typeof onCreated === 'function') onCreated(name);
    }).catch(function (e) { toast(e.message || '新建失败'); });
  } });
}
function addTrackToPlaylist(name, tk) {
  if (!name || !tk || !tk.path) return Promise.resolve();
  return api('/api/sq/add?name=' + encodeURIComponent(name) + '&file=' + encodeURIComponent(tk.path) + cueQuery(tk.cs, tk.ce)).then(function (j) {
    if (!j || !j.ok) throw Error((j && j.error) || '添加失败');
    toast(tf('已添加到歌单：{0}（{1} 首）', [name, j.count]));
    renderSavedQueues();
    if (S.view === 'sq' && S.sqName === name) renderSavedQueuePage(name);
    return j;
  }).catch(function (e) { toast(e.message || '添加失败'); });
}
function saveQueueDialog() {
  var n = (S.q && S.q.items) ? S.q.items.length : 0;
  if (!n) { toast('队列是空的，先加几首歌再保存'); return; }
  askText({ title: '保存为歌单', ok: '保存', value: S.sqName || '',
    placeholder: '例如 夜深人静',
    hint: tf('把现在这 {0} 首（含顺序与 CUE 区间）存成一个歌单；写入 rlplayer-savedqueues.conf，重启后仍在', [n]),
    onOk: function (name) {
      api('/api/sq/save?name=' + encodeURIComponent(name)).then(function (j) {
        if (!j || !j.ok) { toast((j && j.error) || '保存失败'); return; }
        toast(tf('已存为歌单：{0}（{1} 首）', [j.name, j.count]));
        renderSavedQueues();
        if (S.view === 'favs') loadFavs();
        else if (S.view === 'sq') renderSavedQueuePage(j.name);
      }).catch(function (e) { toast(tf('保存失败: {0}', [e])); });
    } });
}
function renameQueueDialog(name) {
  askText({ title: '重命名歌单', ok: '改名', value: name,
    hint: '改名后立刻写入 rlplayer-savedqueues.conf（重启后是新名字）',
    onOk: function (to) {
      api('/api/sq/rename?from=' + encodeURIComponent(name) + '&to=' + encodeURIComponent(to)).then(function (j) {
        if (!j || !j.ok) { toast((j && j.error) || '改名失败'); return; }
        toast(tf('已改名为：{0}', [j.to]));
        if (S.sqName === name) S.sqName = j.to;
        renderSavedQueues();
        if (S.view === 'sq') renderSavedQueuePage(j.to);
        else if (S.view === 'favs') loadFavs();
      }).catch(function (e) { toast(tf('改名失败: {0}', [e])); });
    } });
}
function deleteSavedQueue(name) {
  askConfirm({ title: '删除歌单', ok: '删除',
    text: tf('删除歌单「{0}」？只删这份歌单，不动当前播放队列。', [name]),
    onOk: function () {
      api('/api/sq/delete?name=' + encodeURIComponent(name)).then(function (j) {
        if (!j || !j.ok) { toast((j && j.error) || '删除失败'); return; }
        toast(tf('已删除：{0}', [name]));
        if (S.sqName === name) S.sqName = '';
        renderSavedQueues();
        if (S.view === 'sq') { setView('favs'); loadFavs(); }
        else if (S.view === 'favs') loadFavs();
      }).catch(function (e) { toast(tf('删除失败: {0}', [e])); });
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
function playPath(f, cs, ce, queueIndex) {
  if (!ensurePlaybackSettings()) return;
  var selected = Number.isSafeInteger(queueIndex) && queueIndex >= 0 ? '&queueIndex=' + queueIndex : '';
  api('/api/play', 'file=' + encodeURIComponent(f) + cueQuery(cs, ce) + selected + '&' + params()).then(function (j) {
    if (j && j.ok === false) { toast(j.error || '播放失败'); return; }
    toast('开始播放'); loadQ();
  });
}
function addPath(f) {
  api('/api/add', 'file=' + encodeURIComponent(f)).then(function () { toast('已加入队列'); loadQ(); });
}
function playRemote(it) {
  if (!ensurePlaybackSettings()) return;
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
  // 2026-11-12：QQ 音乐式「添加到 → 歌单」子菜单（我的歌单 = /api/sq 具名队列）。
  (function(){
    var addWrap=document.createElement('div');addWrap.className='ctxitem has-sub';addWrap.innerHTML='<span>添加到</span><span class=ctxarrow>›</span>';
    var sub=document.createElement('div');sub.className='ctxsub';
    function subItem(label,fn,muted){var d=document.createElement('div');d.className='ctxitem'+(muted?' ctxmuted':'');d.textContent=label;if(fn)d.onclick=function(ev){ev.stopPropagation();hideContextMenu();fn();};sub.appendChild(d);}
    var names=(S.sqs||[]).map(function(q){return q.name;});
    if(!names.length)subItem('暂无歌单',null,true);
    names.forEach(function(name){subItem(name,function(){addTrackToPlaylist(name,tk);});});
    subItem('＋ 新建歌单…',function(){createPlaylistDialog(function(name){addTrackToPlaylist(name,tk);});});
    addWrap.appendChild(sub);
    addWrap.onclick=function(ev){ev.stopPropagation();addWrap.classList.toggle('open');};
    if(typeof x==='number'&&x>innerWidth-240)addWrap.classList.add('flip');
    m.appendChild(addWrap);
  })();
  // 分析 + 多版本对比。两项都只读文件（/api/health 流式解码、/api/compare 对齐后分析），不开音频设备。
  // 2026-10-27（261027-04 #4）：设置页「分析」页签已删除（用户要求搬到单曲页）。
  // P3（2026-10-27 261027-05 C4，用户：「有损检测就是假无损检测，归类到一起就行」）：
  //   这里原来是**两个**分析入口 ——「查看分析（动态/音质/假无损）」+「分析音质（19 项）」，
  //   两处都对无损性下结论 ⟹ 合并为一条，弹窗按 L2 打开（三块卡 + 全部原始指标 + 判定依据）。
  //   「对比版本…」不动（用户明确要求这个功能不要删，入口也不变）。
  // 2026-10-28：模块总开关关掉 ⟹ 这个入口也不给（否则"模块已关"却还能点开分析，语义不成立）
  if (ANL.master) item('查看分析（动态/音质/无损性）', function () { openHealthModal(tk.path); });
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
      playPath(S.q.items[idx], c[0], c[1], idx);
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
  if (repMode === '2') {
    // 单曲循环：下一首就是当前曲目，不能再显示「队列末尾」自我否认循环。
    nextIdx = Math.max(0, curIdx);
  } else if (nextIdx >= qLen) {
    if (repMode === '1' || repMode === '3') {
      nextIdx = 0;
    } else {
      hasNext = false;
    }
  }
  var remain = Math.max(0, qLen - 1 - (curIdx >= 0 ? curIdx : 0));
  var text = '';
  var tip = '';
  if (repMode === '2' && qLen > 0) {
    var loopTitle = nam(S.q.items[Math.max(0, curIdx)] || '');
    text = '单曲循环: ' + loopTitle;
    tip = tf('单曲循环：{0}（播完从头重播）', [loopTitle]);
  } else if (hasNext && qLen > 0) {
    var nextPath = S.q.items[nextIdx] || '';
    var nextTitle = nam(nextPath);
    text = '下一首: ' + nextTitle + (remain > 0 ? (' · 剩 ' + remain + ' 首') : '');
    tip = tf('下一首：{0}（队列共 {1} 首，剩余 {2} 首）', [nextTitle, qLen, remain]);
  } else {
    text = '队列末尾' + (qLen > 0 ? (' · 共 ' + qLen + ' 首') : '');
    tip = tf('当前已是队列最后一首（共 {0} 首）', [qLen]);
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
    D('rcnt').textContent = tf('共 {0} 首', [S.q.items.length]);
    updateNextTrackInfo();
    if (typeof rlHomeActive === 'function' && rlHomeActive()) { rlPaintQueue(); rlPaintHome(); }
    if (!S.q.items.length) {
      D('qlist').innerHTML = '<div class=empty style="padding:26px 14px">' + t('队列为空') + '<br><span style="font-size:11px">' + t('右键专辑或曲目可加入') + '</span></div>';
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
        playPath(S.q.items[i], c[0], c[1], i);
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
    chips += '<span class=badge title="' + esc(tf('来源: {0}', [(src[p[0]] || '本地标签')])) + '">' +
             esc(t(p[1]) + '  ' + v) + '</span>';
  });
  chips += '<span class="badge g" title="标签/封面/歌词已由在线来源补全，可在设置-元数据补全关闭">已补全</span>';
  return chips;
}
function refreshCurrentCover() {
  if (!S.lastCur || S.coverRefreshTimer) return;
  S.coverRefreshTimer = setTimeout(function () {
    S.coverRefreshTimer = 0;
    if (S.lastCur) loadMeta(S.lastCur);
  }, 150);
}
function saveTrackCover(file, image) {
  if (!file || !image) return Promise.reject(new Error('Missing cover target'));
  return new Promise(function (resolve, reject) {
    var x = new XMLHttpRequest();
    x.open('POST', '/api/cover/set?file=' + encodeURIComponent(file));
    x.timeout = 20000;
    x.onload = function () {
      var result;
      try { result = JSON.parse(x.responseText); }
      catch (e) { reject(new Error(t('设置失败：响应无法解析'))); return; }
      if (x.status < 200 || x.status >= 300 || !result || result.ok !== true) {
        reject(new Error((result && result.error) || t('设置失败'))); return;
      }
      if (S.lastCur === file) loadMeta(file);
      resolve(result);
    };
    x.onerror = x.ontimeout = function () { reject(new Error(t('设置失败：网络错误'))); };
    x.send(image);
  });
}
function resetTrackCover(file) {
  if (!file) return Promise.resolve();
  return api('/api/cover/reset?file=' + encodeURIComponent(file), '').then(function (result) {
    if (!result || result.ok !== true) throw new Error((result && result.error) || t('设置失败'));
    if (S.lastCur === file) loadMeta(file);
    return result;
  });
}
function stopCurrentCachedCover() {
  var watch = S.coverCacheWatch;
  S.coverCacheWatch = null;
  if (!watch) return;
  clearInterval(watch.timer);
  if (watch.image) watch.image.onload = watch.image.onerror = null;
}
// Retry only the exact current file's local image endpoint. Reloading full
// metadata here could start unrelated online/LLM work merely to repaint art.
function watchCurrentCachedCover(file) {
  stopCurrentCachedCover();
  var watch = { file: file, request: S.metaRequest, deadline: Date.now() + 60000, image: null };
  S.coverCacheWatch = watch;
  function current() {
    return S.coverCacheWatch === watch && S.lastCur === file && S.metaRequest === watch.request;
  }
  watch.timer = setInterval(function () {
    if (!current() || Date.now() >= watch.deadline) { if (S.coverCacheWatch === watch) stopCurrentCachedCover(); return; }
    if (watch.image) return;
    var image = watch.image = new Image();
    var url = '/api/cover?file=' + encodeURIComponent(file) + '&_cov=' + Date.now();
    image.onload = function () {
      if (!current()) return;
      var bg = 'url(' + JSON.stringify(url) + ')';
      D('cv').style.backgroundImage = bg;
      if (S.curMeta) { S.curMeta.hasCover = true; S.curMeta.coverUrl = url; }
      if (D('npPage') && D('npPage').classList.contains('on')) {
        D('npbg').style.backgroundImage = bg;
        D('npCover').src = url;
        D('npCover').style.display = '';
      }
      stopCurrentCachedCover();
    };
    image.onerror = function () { if (current()) watch.image = null; };
    image.src = url;
  }, 2000);
}
window.addEventListener('pagehide', stopCurrentCachedCover);
function loadMeta(f) {
  if (!f) return;
  stopCurrentCachedCover();
  var request = S.metaRequest = (S.metaRequest || 0) + 1;
  if (S.metaFile !== f) {
    S.metaFile = f;
    S.curMeta = null;
    S.curCh = 0;
    S.npMetaHtml = '';
    D('cv').style.backgroundImage = '';
    D('npt').textContent = nam(f);
    D('npt').title = nam(f);
    D('npart').textContent = '';
    D('npart').title = '';
    if (D('npPage') && D('npPage').classList.contains('on')) renderNp();
  }
  function current() { return request === S.metaRequest && S.lastCur === f; }
  return api('/api/meta?file=' + encodeURIComponent(f)).then(function (m) {
    if (!current()) return;
    if (!m || m.ok === false) throw new Error('Metadata unavailable');
    S.curMeta = m;
    // P2（261027-05 A4）：当前曲目的**源**声道数。底栏 / 单曲页副标题要标多声道，
    // 而 /api/status 没有声道字段（不许改 rtaweb.cpp）⟹ 只能从这次 /api/meta 里取。
    // 跟着 meta 一起更新，就是为了"换歌时不会拿上一首的声道数冒充这一首"。
    S.curCh = Number(m.channels) || 0;
    var tName = m.title || nam(f);
    D('npt').textContent = tName;
    D('npt').title = tName;
    var sub = [];
    if (m.artist) sub.push(m.artist);
    if (m.album) sub.push(m.album);
    else if (m.folderName) sub.push(tf('文件夹：{0}', [m.folderName]));
    var subStr = sub.join(' \u00b7 ');
    D('npart').textContent = subStr;
    D('npart').title = subStr;
    var coverUrl = typeof m.coverUrl === 'string' && m.coverUrl.indexOf('/api/cover?') === 0
      ? m.coverUrl : '/api/cover?file=' + encodeURIComponent(f) + '&_cov=' + request;
    D('cv').style.backgroundImage = m.hasCover ? 'url(' + JSON.stringify(coverUrl) + ')' : '';
    if (!m.hasCover) watchCurrentCachedCover(f);
    // 补全芯片：存全局，renderNp 统一填进 #npMeta；整页开着就立刻刷一次（meta 是异步回来的）
    S.npMetaHtml = buildNpMetaHtml(m);
    if (D('npPage') && D('npPage').classList.contains('on')) {
      renderNp();   // renderNp 内部已调 updateNpSummary(S.curMeta)，不再重复请求
    }
  }).catch(function () {
    if (!current()) return;
    S.curCh = 0;                       // 取不到元数据 ⟹ 不标声道（宁可不标，也不标错的）
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
    // 2026-10-30：第四态。链路没改样本，但源本身有损（mp3/aac/ogg…）。
    // 旧实现把它塞进 'no' ⟹ 用户看到"非 bit-perfect"就去改设置，怎么改都不变
    // （实测：同一会话里 16bit WAV 报 yes、MP3 报 no，差别只在解码容器是 float32）。
    else if (bpv === 'lossy') { t = '链路逐位·源有损'; c = 'bp-lossy'; title = title || '链路不改解码结果；源本身有损，无原始 PCM 可比'; }
    else if (bpv === 'no') { t = '非 bit-perfect'; c = 'bp-no'; title = title || '没有升频，但链路里有改动（原因见信号链）'; }
    // 2026-10-29（261029-01 #11）：探测不到就**不显示**，绝不落成红色的「非 bit-perfect」。
  else if (bpv === 'unknown') { t = ''; c = 'bp-unknown'; title = title || '无法判定：渲染器未声明能力，本机不做猜测'; }
  else { t = 'bit'; c = 'bp-no'; title = title || '直通，但没有拿到后端的 bit-perfect 结论'; }
  } else if (dsdOut) {
    t = srcDsd ? 'DSD-DSD 升频' : 'PCM-DSD 升频';
    c = 'bp-up'; title = title || (srcDsd ? 'DSD 源 → 更高 DSD 档（经解码 + 重新调制）' : 'PCM 源 → DSD（SDM 调制）');
  } else {
    t = 'PCM-PCM 升频'; c = 'bp-up'; title = title || 'PCM 源 → 更高 PCM 采样率';
  }
  if (S.bpFallback) title = (title ? title + '；' : '') + tf('降级原因：{0}', [S.bpFallback]);
  return { text: t, cls: c, title: title };
}

function syncPlaybackCaption(j, srcCh) {
  var caption = j.error || ((j.playing ? (j.paused ? '已暂停' : '播放中') : '已停止') +
    (j.outRate ? ' \u00b7 ' + j.outRate + ' Hz / ' + j.outBits + ' bit' : '') + chSuffix(srcCh));
  if (D('nps')) D('nps').textContent = caption;
  // The open track page otherwise keeps the caption captured by renderNp()
  // before an asynchronous decoder/IR failure arrived.
  if (D('npSub') && D('npPage') && D('npPage').classList.contains('on')) {
    var artist = (D('npart') && D('npart').textContent) || '';
    D('npSub').textContent = artist + (caption ? '  ·  ' + caption : '');
  }
}
var statusRequest = 0, statusApplied = 0, statusInFlight = null;
function tick() {
  // Slow backends must never accumulate a new request every 400 ms. Sharing
  // the pending poll keeps playback controls free to reach the local server.
  if (statusInFlight) return statusInFlight;
  var request = ++statusRequest;
  statusInFlight = api('/api/status').then(function (j) {
    // A delayed poll must not restore the preceding track, artwork, or seek position.
    if (!j || j.ok === false || request <= statusApplied) return;
    statusApplied = request;
    if (j && j.error) j.error = playbackErrorText(j.error);
    // 2026-11-01：测试钩子（后端只在设了 RLPLAYER_START_VIEW 时才给这个字段）。
    // 合成输入进不到 WebView2 子窗口，验收截图必须能直达某个页面。
    // 2026-11-01：多语言覆盖率采集（只在后端 RLPLAYER_I18N_DEBUG=1 时才为 true）。
    //   把「渲染后仍是中文、语言包里也没有」的文本/属性上报后端日志，用来生成待译清单。
    if (j && j.i18nDebug && !S.i18nDbg) {
      S.i18nDbg = 1;
      setInterval(function () {
        if (!I18N.map || I18N.lang === 'zh') return;
        var miss = {}, out = [];
        var w = document.createTreeWalker(document.body, NodeFilter.SHOW_TEXT, null);
        var n = w.currentNode;
        while (n) {
          var s = i18nNorm(n.nodeValue);
          if (s && /[\u4e00-\u9fff]/.test(s) && !I18N.map[s] && !i18nByPattern(s) && !miss[s]) {
            miss[s] = 1; if (out.length < 400) out.push(s);
          }
          n = w.nextNode();
        }
        var at = i18nAttrs();
        Array.prototype.forEach.call(document.querySelectorAll('[title],[placeholder],[aria-label],[alt],[data-tip]'), function (e) {
          for (var i = 0; i < at.length; i++) {
            var v = i18nNorm(e.getAttribute(at[i]));
            if (v && /[\u4e00-\u9fff]/.test(v) && !I18N.map[v] && !i18nByPattern(v) && !miss[v]) { miss[v] = 1; if (out.length < 400) out.push(v); }
          }
        });
        if (out.length) fetch('/api/i18n/miss', { method: 'POST', body: JSON.stringify(out) }).catch(function () { });
      }, 2500);
    }
    if (!S.startApplied && j && j.startView) {
      S.startApplied = 1;
      var sv = String(j.startView).split(':');
      setTimeout(function () {
        if (sv[0] === 'settings') { openSettingsTab(sv[1] || 'input'); return; }
        if (sv[0] === 'np') { if (typeof openNowPlaying === 'function') openNowPlaying(); return; }
        // 走真实导航点击：setView() 只切类名，各视图的数据加载挂在导航按钮的 onclick 上
        var nav = document.querySelector('.nav[data-v="' + sv[0] + '"]');
        if (nav) nav.click(); else if (sv[0]) setView(sv[0]);
      }, 1200);
    }
    // 2026-11-02（用户反馈）：共享 WASAPI 下「永远不是 bit-perfect」但界面不提示。
    //   底栏芯片现在会是 bit-perfect=no 且 tooltip 写明原因；这里再给一次性 toast，
    //   否则用户只会觉得"怎么调都不是"。每进程只提示一次，不打扰。
    if (j && j.outShared && j.playing && !S.sharedHinted) {
      S.sharedHinted = 1;
      toast('共享输出：采样经 Windows 混音器，无法 bit-perfect。设置 → 输出 改为「WASAPI（独占 · 事件驱动）」');
    }
    if (j && j.qa && !QA.on) { QA.on = true; QA.timer = setInterval(qaTick, 500); }
    S.playing = !!j.playing; S.paused = !!j.paused; S.dur = j.dur; S.pos = j.pos;
    S.cd = !!j.cd; S.cdTitle = j.cdTitle || '';
    // CD 音轨没有本地文件元数据；标题用 /api/status 的 cdTitle（CD-Text 或 Track NN），
    // 不让底栏显示 Track01.cda 这种索引文件名。
    if (j.cd && D('npt')) {
      var cdName = j.cdTitle || ('Track ' + String(j.cdTrack || 0).padStart(2, '0'));
      if (D('npt').textContent !== cdName) { D('npt').textContent = cdName; D('npt').title = cdName; }
    }
    // Re-label a cached report only when the existing playback state changes.
    // No signal request, measurement, or audio scan is started by this observer.
    var cleanupPlaybackKey = [j.current || '', S.playing, S.paused].join('|');
    if (cleanupPlaybackKey !== CLEANUP_UI.playbackKey) {
      if (CLEANUP_UI.currentFile !== (j.current || '') && CLEANUP_UI.source === 'local') CLEANUP_UI.facts = null;
      CLEANUP_UI.currentFile = j.current || ''; CLEANUP_UI.playbackKey = cleanupPlaybackKey;
      paintCleanupStatus();
    }
    // 2026-10-27（261027-04 #4）：单曲页分析条的数据源 —— 用 /api/status 的 current 驱动
    // 「播放就自动分析一次」。选它而不是用 /api/play 的返回值，是因为 skip / 上一首 / 队列自动
    // 切歌 / UPnP 投放**都不经过** /api/play，只有 current 是所有这些路径的共同出口。
    // 去重全在 anlUpdateCur 内部（按路径），所以 400 ms 轮询不会重复触发。
    anlUpdateCur(j.current || '');
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
      if (D('npIcP')) D('npIcP').setAttribute('d', (S.playing && !S.paused)
        ? 'M7 5h4v14H7zM13 5h4v14h-4z' : 'M8 5l12 7-12 7z');
      renderNpLyric();
    }
    syncVolumeStatus(j,false,request);
    syncTransportSeek(j);
    S.startDb = (typeof j.startDb === 'number') ? j.startDb : S.vol;
    // 信号链弹层开着时，音量节点要跟着实时变（否则就是在显示过期数据）
    if (D('chainVol') && D('modal').classList.contains('on')) D('chainVol').textContent = volText(S.volDb);
    if (D('chainTiming') && D('modal').classList.contains('on')) {
      var dspB = (typeof j.dspBlockMs === 'number' && j.dspBlockMs > 0) ? j.dspBlockMs.toFixed(2) + ' ms/块' : '';
      var dspT = (typeof j.dspTotalMs === 'number' && j.dspTotalMs > 0) ? tf('（累计 {0} ms）', [j.dspTotalMs.toFixed(1)]) : '';
      var dspStr = (dspB || dspT) ? (dspB + dspT) : '–';
      var decStr = (typeof j.decodeMs === 'number' && j.decodeMs > 0) ? (j.decodeMs >= 1000 ? (j.decodeMs/1000).toFixed(2) + ' s' : j.decodeMs.toFixed(1) + ' ms') : '–';
      var wavStr = '–';
      if (typeof j.ffmpegMs === 'number' && j.ffmpegMs > 0) {
        wavStr = '转码写 ' + j.ffmpegMs.toFixed(1) + ' ms' + (typeof j.wavReadMs === 'number' && j.wavReadMs > 0 ? ' / 读 ' + j.wavReadMs.toFixed(1) + ' ms' : '');
      } else if (typeof j.wavReadMs === 'number' && j.wavReadMs > 0) {
        wavStr = tf('读取 {0} ms', [j.wavReadMs.toFixed(1)]);
      }
      var scanStr = (typeof j.scanMs === 'number' && j.scanMs > 0) ? (j.scanMs >= 1000 ? (j.scanMs/1000).toFixed(2) + ' s' : j.scanMs.toFixed(0) + ' ms') : '–';
      D('chainTiming').textContent = tf('解码 {0}  \u00b7  临时WAV {1}  \u00b7  DSP {2}  \u00b7  库扫描 {3}', [decStr, wavStr, dspStr, scanStr]);
    }
    D('icP').setAttribute('d', S.playing && !S.paused
      ? 'M7 5h4v14H7zM13 5h4v14h-4z'                     /* 暂停图标 */
      : 'M8 5v14l11-7z');                                 /* 播放图标 */
    if (j.cd) {
      // CD 没有本地元数据：不要拿上一首本地文件的 current 去 loadMeta，否则
      // 底栏会显示上一首的标题/封面。标题由上面的 cdTitle 负责，这里只补播放状态行。
      syncPlaybackCaption(j, 2);
      if (D('fmt')) { D('fmt').textContent = '44.1k/16'; D('fmt').title = t('CD 音频（44.1 kHz / 16 bit / 2ch）'); }
      updateNextTrackInfo();
    } else if (j.current) {
      if (S.lastCur !== j.current) { S.lastCur = j.current; loadMeta(j.current); loadLyrics(j.current); }
      syncLyrics(j.pos);
      // P2（2026-10-27 261027-05 A4「多声道就都标记出来就好了」）：底栏这一行原来是
      // 「播放中 · 96000 Hz / 24 bit」，6 声道文件与 2.0 完全看不出区别 ⟹ 多声道追加布局名（· 5.1）。
      // 声道数只能从 /api/meta（loadMeta 存的 S.curCh）取：/api/status 的响应里没有声道字段，
      // 而本轮不允许改 rtaweb.cpp（/api/meta 明确带 channels，见 rtaweb.cpp:11299）。
      var srcCh = (typeof S.curCh === 'number' && S.curCh > 0) ? S.curCh : 0;
      syncPlaybackCaption(j, srcCh);
      if (D('nps')) D('nps').title = srcCh > 0 ? (tf('源文件 {0}', [chText(srcCh)])) : '';
      D('fmt').textContent = j.outRate ? ((Math.round(j.outRate / 100) / 10) + 'k/' + j.outBits + chSuffix(srcCh)) : '--';
      if (D('fmt')) D('fmt').title = srcCh > 0 ? (tf('源文件 {0}', [chText(srcCh)])) : '输出格式';
      updateNextTrackInfo();
      // 261024-04 #8：底栏 GPU 徽标只表示「GPU **真的参与**了本次播放」。
      // 后端新字段 gpuUsed（本次播放 GPU 是否实际参与）；字段还没上线时回退旧的 gpu 字段。
      // 选择"不参与就不亮"而不是"显示灰色 GPU 可用·未参与"：底栏右侧本来就挤，
      // 常驻一个灰 chip 会把"绿点 = GPU 参与"这个唯一的强信号稀释掉；可用性在设置→高级与信号链里能查到。
      S.gpuUsed = (typeof j.gpuUsed === 'boolean') ? j.gpuUsed : !!j.gpu;
      S.gpuOn = !!j.gpu;
      D('gpub').style.display = S.gpuUsed ? '' : 'none';
      D('gpub').title = S.gpuUsed ? 'GPU 参与本次播放'
        : (S.gpuOn ? t('GPU 可用 · 本次未参与（信号链里绿色节点才会亮）') : t('GPU 未参与本次播放'));
    } else {
      S.gpuUsed = false; S.gpuOn = false;
      D('fmt').textContent = '--';
      D('gpub').style.display = 'none';
      if (S.lastCur) { stopCurrentCachedCover(); S.lastCur = ''; S.metaFile = ''; S.curMeta = null; S.metaRequest = (S.metaRequest || 0) + 1; D('npt').textContent = '未播放'; D('npart').textContent = ''; D('nps').textContent = '选择一个专辑开始'; D('cv').style.backgroundImage = ''; S.npMetaHtml = ''; if (D('npMeta')) D('npMeta').innerHTML = ''; }
      updateNextTrackInfo();
    }
    // 2026-10-23 #1：底栏常驻一个「b-p」徽标，不打开信号链也能知道当前是否 bit-perfect。
    // 2026-10-23 用户 261023-04 #1：改成紧凑 chip（class=bp-yes/bp-no/bp-direct），
    // 配色与显隐交给 CSS（三套主题各有一套颜色），这里不写死颜色、也不占中间列。
    if (D('bpBadge')) {
      var bi = bpBadgeInfo();
      // #11：unknown 时 text 为空 ⟹ 整个徽标收起（不给错误结论）
      D('bpBadge').textContent = bi.text;
      D('bpBadge').className = bi.cls;
      D('bpBadge').title = tr(bi.title);
      D('bpBadge').style.display = bi.text ? '' : 'none';
    }
    if (j.error && !j.current) syncPlaybackCaption(j, 0);
    rlLastStatus = j;
    rlRefreshShaperGuidance();
    if (S.view==='settings' && D('rlPresetApply')) rlPaintPreset();
    if (rlHomeActive()) rlPaintHome();
    syncAlbPlayAllBtn();
  }).catch(function () { }).then(function () { statusInFlight = null; });
  return statusInFlight;
}

/* ---------------- 播放设置持久化 ---------------- */
var SET_KEYS = [['sRate','rate'], ['sRateFamilyPolicy','rateFamilyPolicy'], ['sFilt','filter'], ['sFiltNx','filterNx'], ['sShap','shaper'],
                ['sDSD','dsd'], ['sBuf','buffer'], ['sIsp','isp'], ['s2s','twostage'], ['sGpu','gpuvendor']];
function analysisAutoStatus() {
  if (ANL.master === 0)
    return t('模块已关闭：单曲页不显示分析卡，也不会再请求分析数据');
  return tf('当前：{0}　·　已在单曲页展示，不需要额外操作', [t(ANL.auto ? '开' : '关')]);
}
var settingsSaveQueue=Promise.resolve();
function persistSettings(body,volumeWrite,stamp) {
  var skippedGroup=false,skippedFields=[];
  if(!stamp&&!volumeWrite)rlSettingsFence.mark(body);
  function send() {
   function dispatch(){
    if(volumeWrite&&(VOLUME_WRITE.current!==volumeWrite||volumeWrite.sequence!==VOLUME_WRITE.sequence||!volumeWrite.accepted||isRawVolumeLocked()))return {ok:false,stale:true};
    // Debounced callers may have built their body before a new gesture began.
    // Re-check at dispatch, and never turn an optimistic value into a preference.
    var data=new URLSearchParams(body),volume=confirmedVolumeForSave();
    var reconciliation=rlSettingsFence.reconcile(data,stamp);
    skippedGroup=reconciliation.group;skippedFields=reconciliation.removed;
    if(data.has('volume')){
      data.delete('volume');
      if(volume!==null&&(!volumeWrite||volume===volumeWrite.value))data.set('volume',String(volume));
    }
    if(!data.toString())return {ok:true,skipped:true};
    return api('/api/settings', data.toString());
   }
   return rlSettingsFence.hasParameters(body)?rlSettingsFence.legacy(dispatch,stamp):dispatch();
  }
  var next=settingsSaveQueue.then(send,send);
  settingsSaveQueue=next.then(function(){},function(){});
  return next.then(function(j){
    if(j&&j.stale)return j;
    if(!j||!j.ok){toast(tf('保存失败：{0}',[t((j&&j.error)||'保存失败')]));return j;}
    if(skippedGroup)return rlSettingsFence.readback().then(function(){return {ok:false,stale:true,errorCode:'STALE_GROUP_SETTINGS',preferencesSaved:!j.skipped,skippedFields:skippedFields};});
    if(skippedFields.length)return Object.assign({},j,{rebased:true,skippedFields:skippedFields});
    return j;
  }).catch(function(){toast(t('保存失败：服务没响应'));});
}
function sdmCornerSettingsValid(value, family) {
  var corner = (value === '' || value === undefined || value === null) ? 0 : Number(value);
  return Number.isFinite(corner) && corner >= 0 && corner <= 200 &&
    (family === 'ec' || corner === 0 || corner >= 100);
}
function validateSdmCornerSettings() {
  var field = D('sp_modcut'), familyField = D('sp_modfamily');
  var value = field && field.value !== undefined ? field.value : ADV.modcut;
  var family = (familyField && familyField.value) || ADV.modfamily || 'dsd';
  if (sdmCornerSettingsValid(value, family)) return true;
  toast(t('SDM 拐点必须为 0 或 100～200 kHz；EC 族不使用此值，但仍须是 0～200 的有限数字。输入已保留，请修正后再保存。'));
  return false;
}
function saveSettings(immediate) {
  if (!ensurePlaybackSettings()) return Promise.resolve({ok:false,errorCode:'PLAYBACK_SETTINGS_NOT_READY'});
  if (!validateSdmCornerSettings()) return Promise.resolve({ok:false,errorCode:'SDM_CORNER_QUALITY'});
  if (!validateProcessingSettings()) return Promise.resolve({ok:false,errorCode:'PROCESSING_INPUT'});
  var settingsStamp=rlSettingsFence.capture();
  var body = '';
  SET_KEYS.forEach(function (p) {
    var e = D(p[0]); if (!e) return;
    var v = (e.type === 'checkbox') ? (e.checked ? '1' : '0') : e.value;
    if (body) body += '&';
    body += p[1] + '=' + encodeURIComponent(v);
  });
  var confirmedVolume=confirmedVolumeForSave();
  if(confirmedVolume!==null)body+='&volume='+confirmedVolume;
  body += '&loudness=' + (D('sLoud').checked ? '1' : '0');
  body += processingSettingsParam();
  body += '&rgmode=' + (ADV.rgmode || 'off') + '&rgclip=' + (ADV.rgclip ? 1 : 0);
  // 261027-04 #4：播放时自动分析开关（conf 键 autoanl；默认 1）
  body += '&autoanl=' + (ANL.auto ? 1 : 0);
  // 2026-10-28：模块总开关也要落盘，否则重启后又冒出来。
  body += '&anlmaster=' + (ANL.master ? 1 : 0);
  body += '&theme=' + encodeURIComponent(S.theme || 'dark');
  // 2026-10-30（用户 261029-03 #1/#3）：界面语言（conf 键 uilang；"auto" = 跟随系统）
  body += '&uilang=' + encodeURIComponent(ADV.uiLang || 'auto');
  body += '&quickstart=' + (ADV.quickStart === 0 ? 0 : 1);
  body += '&device=' + encodeURIComponent(ADV.device || '') + '&bits=' + (ADV.bits || 0)
        + '&taps=' + (ADV.taps || 0) + '&atten=' + (ADV.atten || 0)
        + '&transition=' + (ADV.transition || 0) + '&phase=' + encodeURIComponent(ADV.phase || '')
        + '&modorder=' + (ADV.modorder || 5) + '&modfamily=' + encodeURIComponent(ADV.modfamily || 'dsd') + '&modcut=' + (ADV.modcut || 0)
        + '&moddith=' + (ADV.moddith === undefined ? -1 : ADV.moddith)
        + '&eq=' + encodeURIComponent(ADV.eq || '')
        + '&ir=' + encodeURIComponent(ADV.ir || '') + '&irgpu=' + (ADV.irgpu || 0)
        + '&roomcorrauto=' + (ADV.roomcorrauto === 0 ? 0 : 1)
        + '&ramp=' + (ADV.ramp || 0)
        + '&backend=' + encodeURIComponent(ADV.backend || 'wasapi')
        + '&channels=' + (ADV.channels || 0)
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
        // 2026-10-27（261027-06 P3）：AI 整理开关的**上报值**必须与界面真实状态一致。
        // 以前写的是 META.summary，而它因 loadSettings 读不到 kv 长期为 0 ⟹ 用户每保存一次设置
        // 就会把 metasummary 从 1 冲回 0（后端随即关掉在线整理；实测 build-ai2 实例复现过：
        // settings.conf 里 metasummary=1 → 任意一次保存后变 0）。改走 aiSummaryOn()。
        + '&metasummary=' + (aiSummaryOn() ? 1 : 0)
        + (META_PROVIDERS || []).map(function (p) {
            return '&metasrc_' + p.id + '=' + (META_SRC[p.id] === 0 ? 0 : 1);
          }).join('');
  clearTimeout(window._setT);
  if (immediate) {
    return persistSettings(body,undefined,settingsStamp);
  } else {
    window._setT = setTimeout(function () { persistSettings(body,undefined,settingsStamp); }, 300);
  }
}
function loadSettings() {
  var generation = ++PLAYBACK_SETTINGS.generation;
  PLAYBACK_SETTINGS.ready = false; PLAYBACK_SETTINGS.loading = true; PLAYBACK_SETTINGS.failed = false; PLAYBACK_SETTINGS.invalidVolume = false;
  updatePlaybackSettingsGate();
  // 2026-10-27 P0：必须用 apiGetRaw（真 GET）。api() 会走 POST，只回 {merged:true}、不回 kv，
  // 导致这里 kv 恒空 —— 设置控件全停在默认值，而 saveSettings() 又会把默认值写回 conf。
  return apiGetRaw('/api/settings').then(function (j) {
    if (generation !== PLAYBACK_SETTINGS.generation) return {ok:false,stale:true};
    if (!j || j.ok !== true || !j.kv || typeof j.kv !== 'object') throw new Error('Invalid playback settings response');
    var kv = j.kv || {};
    var restoredVolume = kv.volume === undefined ? -60 : Number(kv.volume);
    if ((kv.volume !== undefined && String(kv.volume).trim() === '') || !Number.isFinite(restoredVolume) || restoredVolume < -120 || restoredVolume > 20) {
      var volumeError = new Error('Invalid saved volume'); volumeError.invalidVolume = true; throw volumeError;
    }
    // 2026-10-30（用户 261029-03 #1/#3）：界面语言必须在**碰任何 DOM 之前**落定 ——
    //   它是"界面能不能看懂"的前提，而本函数后面有一批裸 D(...) 调用（元素不在就抛 TypeError，
    //   整个 .then 会从那一行中断，见下方 anlmaster 事故链）。放这里，任何后续错误都带不偏它。
    if (kv.uilang !== undefined) {
      ADV.uiLang = kv.uilang || 'auto';
      if (D('sp_uilang')) D('sp_uilang').value = ADV.uiLang;
    }
    ADV.quickStart = (kv.quickstart === '0' || kv.quickstart === 'false') ? 0 : 1;
    // 2026-10-28（P0，真机定位）：**先落这两个开关，再碰任何 DOM**。
    //   事故链：本函数中间有一批「裸 D(...)」调用，例如
    //     3298: if (kv.loudness !== undefined) { D('sLoud').checked = ... }
    //   而 D('sLoud') 只在「输出」页签渲染后才存在；用户停在专辑页时它是 null
    //   ⟹ 抛 TypeError ⟹ **整个 .then 从那一行中断** ⟹ 后面所有字段（包括 anlmaster）
    //   一律读不到，于是总开关永远停在 HTML 默认的「勾选」上。
    //   autoanl 之所以看起来"生效"，是因为它同时在 SET_KEYS 里，走了另一条路径。
    //   修法：把这两个与「功能是否可用」直接相关的开关**提到最前**，让它们不受后续任何
    //   行的影响；剩下的字段即使中途出错，也不会把模块开关带偏。
    if (kv.autoanl !== undefined) ADV.autoAnl = (kv.autoanl === '0' || kv.autoanl === 'false') ? 0 : 1;
    ANL.auto = ADV.autoAnl ? 1 : 0;
    if (kv.anlmaster !== undefined) ADV.anlMaster = (kv.anlmaster === '0' || kv.anlmaster === 'false') ? 0 : 1;
    ANL.master = ADV.anlMaster ? 1 : 0;
    if (D('sp_autoanl'))    D('sp_autoanl').checked = !!ANL.auto;
    if (D('sp_anlmaster'))  D('sp_anlmaster').checked = !!ANL.master;
    if (D('npAnal')) { if (ANL.master) anlPaint(); else anlHideCard(); }
    if (kv.scanThreads !== undefined || kv.scanthreads !== undefined) {
      ADV.scanThreads = parseInt(kv.scanThreads || kv.scanthreads, 10) || 0;
    }
    SET_KEYS.forEach(function (p) {
      var e = D(p[0]); if (!e || kv[p[1]] === undefined) return;
      if (e.type === 'checkbox') e.checked = (kv[p[1]] === '1');
      else e.value = kv[p[1]];
    });
    // 老配置缺字段或无效枚举均使用同族优先；加载不回写用户配置。
    if (D('sRateFamilyPolicy')) D('sRateFamilyPolicy').value = normalizeRateFamilyPolicy(kv.rateFamilyPolicy);
    syncRateFamilyPolicy();
    // ---- 老用户配置迁移（功能1 的红线：不许悄悄改老用户行为）----
    // 只有 conf 里【完全没有 rate 键】时才用新默认（**src 源采样率直通**）：
    //   老用户 conf 通常有 rate=max → kv.rate 有值 → 上面 SET_KEYS 已把它写回 <select>，
    //   所以 TA 的行为一点没变（继续按"设备最高"播，直到自己改）。
    //   全新安装没有 conf → kv.rate 缺失 → 用 src。
    //
    // ★ 2026-10-30（261030-01 D-17）：新默认由 follow 改为 src。
    //   D-17 原文：「软件默认 = 源采样率直通（不滤波）；升频默认 = poly-sinc-short-lp」。
    //   follow 在「源 44.1k + 系统混音 48k」时必然重采样 ⟹ 默认就破坏逐位直通，与 D-17 冲突。
    //   2026-10-22 那个坑是「默认 max 会挑 384k 导致独占不出声」，src 不会挑设备最高率，
    //   设备不支持源率时由 rate_pick.h 降级 —— 两个问题不是同一个。
    // 注意：这里【只改 DOM 初值，不立刻 saveSettings】，避免读到旧 conf 就回写。
    if (kv.rate === undefined) {
      if (D('sRate')) D('sRate').value = 'src';
    }
    // 2026-10-28：这两行原本是「裸 D(...)」，元素不存在时抛 TypeError 会把整个 .then 打断
    //   （上面注释里那条事故链就是它们触发的）。加判空，缺元素时只跳过 UI 同步，不影响状态。
    {
      var volumeWrite=VOLUME_WRITE.current;
      if(!volumeWrite||!volumeWrite.pending&&!volumeWrite.awaitChange&&(!volumeWrite.commitRequested||volumeWrite.persisted)){
        VOLUME_WRITE.confirmed=isRawVolumeLocked()?0:restoredVolume;
        S.vol=restoredVolume;
      }
      if (D('vol')) { D('vol').value = S.vol; syncVolUi(); }
    }
    if (kv.loudness !== undefined) {
      var sl = D('sLoud');
      if (sl) { sl.checked = (kv.loudness === '1'); S.loud = sl.checked; }
      else { S.loud = (kv.loudness === '1'); }
    }
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
    // B3：房间校正自动应用。缺键 = 老配置 ⟹ 默认「开」（后端 GET /api/settings 已 injectKv roomcorrauto=1）。
    if (kv.roomcorrauto !== undefined) ADV.roomcorrauto = (kv.roomcorrauto === '0' || kv.roomcorrauto === 'false') ? 0 : 1;
    if (D('sp_roomauto')) D('sp_roomauto').checked = (ADV.roomcorrauto !== 0);
    if (kv.ramp !== undefined) ADV.ramp = parseFloat(kv.ramp) || 0;
    if (kv.gpuvendor !== undefined) { ADV.gpuvendor = kv.gpuvendor || 'auto'; if (D('sGpu')) D('sGpu').value = ADV.gpuvendor; }
    if (kv.backend !== undefined) ADV.backend = kv.backend || 'wasapi';
    if (kv.channels !== undefined) ADV.channels = parseInt(kv.channels, 10) || 0;
    // naahost：NAA 已删除（261024-05）。老 conf 里有这行也直接忽略，不读进状态、不回写。
    if (kv.upnpdev !== undefined) ADV.upnpDev = kv.upnpdev || '';
    if(kv.upnpUpsample!==undefined)ADV.upnpUpsample=(kv.upnpUpsample==='1'||kv.upnpUpsample==='true')?1:0;
    if(kv.upnpForceRate!==undefined)ADV.upnpForceRate=(kv.upnpForceRate==='1'||kv.upnpForceRate==='true')?1:0;
    if(kv.upnpRateCeiling!==undefined)ADV.upnpRateCeiling=Number(kv.upnpRateCeiling)||192000;
    if(kv.cleanup!==undefined)ADV.cleanup=(kv.cleanup==='1'||kv.cleanup==='true')?1:0;
    ADV.cleanupAuto=(kv.cleanup_auto==='0'||kv.cleanup_auto==='false')?0:1;
    if(kv.cleanup_strength!==undefined)ADV.cleanupStrength=Number(kv.cleanup_strength);
    if(kv.cleanup_noise_db!==undefined)ADV.cleanupNoiseDb=Number(kv.cleanup_noise_db);
    ADV.crossfeed=(kv.crossfeed==='1'||kv.crossfeed==='true')?1:0;
    ADV.crossfeedFreq=kv.crossfeed_freq===undefined?700:Number(kv.crossfeed_freq);
    ADV.crossfeedLevel=kv.crossfeed_level===undefined?12:Number(kv.crossfeed_level);
    ADV.dacbits=kv.dacbits===undefined?0:Number(kv.dacbits);
    syncProcessingControls();
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
    // 261027-04 #4：播放时自动分析（单曲页那三块）。缺键 = 老配置 ⟹ 默认「开」。
    // 后端 GET /api/settings 已经 injectKv("autoanl","1")，所以这里通常拿得到值。
    if (kv.autoanl !== undefined) ADV.autoAnl = (kv.autoanl === '0' || kv.autoanl === 'false') ? 0 : 1;
    ANL.auto = ADV.autoAnl ? 1 : 0;
    if (D('sp_autoanl')) D('sp_autoanl').checked = !!ANL.auto;
    // 2026-10-28：**模块总开关**。缺键 = 老配置 ⟹ 默认「开」（升级用户不能突然丢功能）。
    if (kv.anlmaster !== undefined) ADV.anlMaster = (kv.anlmaster === '0' || kv.anlmaster === 'false') ? 0 : 1;
    ANL.master = ADV.anlMaster ? 1 : 0;
    if (D('sp_anlmaster')) D('sp_anlmaster').checked = !!ANL.master;
    if (D('npAnal')) { if (ANL.master) anlPaint(); else anlHideCard(); }
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
    // 2026-10-28（用户真机：「启用音频分析模块」取消勾选、保存、刷新后又勾上）：
    //   根因不在那个复选框，而在更上面一层 —— loadSettings() 定义了却从来没有调用点。
    //   ⟹ kv 恒为空 ⟹ 控件显示 HTML 默认值 ⟹ saveSettings() 把默认值写回 conf，
    //   用户的设置每保存一次就被覆盖一次。anlmaster 只是第一个被发现的受害者
    //   （autoanl 同样读不到，所以它也是勾着的）。
    //   修法两半：① 启动块补 loadSettings()；② 这里在加载完成时，若用户正停在
    //   设置页，就按刚读到的 kv 重画一次 —— 否则他会一直看着默认值。
    // 2026-10-28：只在用户**确实停在设置页**时重绘。
    //   早先我写成 sp.innerHTML.length > 200 —— 但 #setpage 是所有页面共用的容器，
    //   专辑页的内容同样超过 200 字符，于是启动时会把专辑页误画成设置页
    //   （真机表现：切到设置再切高级，页面整体跳到「加载失败」）。
    //   正确判据是 #setpage 上的 'on' 类 —— 路由函数 nav() 用它标记设置页可见。
    // 2026-10-28（真机第三稿，最终）：**不能重建整页**。
    //   试过 renderSettingsPage()：它会把 #setpage 整个 innerHTML 换掉 ⟹
    //   页签弹回第一个（输入）、滚动位置丢失、连当前视图都可能被路由判错（实测跳回专辑页）。
    //   正确做法是**就地刷新控件的值**：conf 到达时，页面结构早已就位，
    //   只需要把「用户看到的值」改成「conf 里的值」。不重建 ⟹ 零副作用。
    try {
      var sp0 = D('setpage');
      if (sp0 && sp0.classList.contains('on')) {
        SET_KEYS.forEach(function (p) {
          var e = D(p[0]); if (!e) return;
          var v = kv[p[1]];
          if (v === undefined) return;
          if (e.type === 'checkbox') e.checked = (v === '1');
          else e.value = v;
        });
        syncRateFamilyPolicy();
        // 这两个不在 SET_KEYS 里（它们由 ANL 状态机单独维护）
        var cm = D('sp_anlmaster'); if (cm) cm.checked = !!ANL.master;
        var ca = D('sp_autoanl');   if (ca) ca.checked = !!ANL.auto;
        var msg = D('sp_anlmsg');
        if (msg) msg.textContent = analysisAutoStatus();
        // 主题也要跟上（否则用户改了主题、重启后又看到旧色）
        try { setTheme(kv.theme || 'dark', false); } catch (e4) { }
      }
    } catch (e) { }
    PLAYBACK_SETTINGS.ready = true; PLAYBACK_SETTINGS.loading = false; PLAYBACK_SETTINGS.failed = false;
    RATE_UI.savedKey=(ADV.backend||'wasapi')+'|'+(ADV.device||'');
    loadRates();
    updatePlaybackSettingsGate(); syncVolUi();
    applyQuickStartPreference(ADV.quickStart);
    if(rlHomeActive())rlConsole.refresh().catch(function(){});
    if (S.view==='settings' && D('rlOutputKind') && rlSettingsState.task==='out')rlInitOutput();
    if (typeof rlSettingsState!=='undefined' && rlSettingsState.pendingPreset) {
      rlSettingsState.syncPreset=rlSettingsState.pendingPreset;rlSettingsState.pendingPreset=null;
      if(D('setpage') && D('setpage').dataset.rlSurface==='pro')rlSyncProfessionalForm();
    }
    return {ok:true};
  }).catch(function (error) {
    if (generation !== PLAYBACK_SETTINGS.generation) return {ok:false,stale:true};
    PLAYBACK_SETTINGS.ready = false; PLAYBACK_SETTINGS.loading = false; PLAYBACK_SETTINGS.failed = true;
    PLAYBACK_SETTINGS.invalidVolume = !!(error && error.invalidVolume);
    updatePlaybackSettingsGate();
    return {ok:false,errorCode:'PLAYBACK_SETTINGS_NOT_READY'};
  });
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
      wbMsg(tf('写回进行中 {0}/{1}…', [(j.done || 0), (j.total || 0)]));
      setTimeout(wbPoll, 800);
      return;
    }
    WB_BUSY = 0; wbButtons(0); wbMsg('');
    if (j.error) { toast(tf('写回未执行：{0}', [j.error])); return; }
    toast('写入 ' + (j.written || 0) + '，跳过 ' + (j.skipped || 0) + '，失败 ' + (j.failed || 0) +
          tf('；失败清单见 rlplayer-meta{0}writeback-failures.json', [SEP]));
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
    if (!j || !j.ok) { toast(tf('预览失败：{0}', [((j && j.error) || '未知错误')])); return; }
    toast((j.writable || 0) + tf(' 首可写，{0} 首跳过', [(j.skipped || 0)]));
  }).catch(function () { WB_BUSY = 0; wbButtons(0); wbMsg(''); toast('预览失败：网络错误'); });
}
function wbApply() {
  if (WB_BUSY) return;
  WB_BUSY = 1; wbButtons(1); wbMsg('写回批次已启动…');
  api('/api/writeback', '{"apply":true}').then(function (j) {
    if (!j || !j.ok || !j.started) {
      WB_BUSY = 0; wbButtons(0); wbMsg('');
      toast(tf('无法启动写回：{0}', [((j && j.error) || '未知错误')]));
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
  if (parseInt(ADV.channels, 10) > 0) tail = tf('；「输出声道数」= {0}，以此为准', [parseInt(ADV.channels, 10)]);
  else if (bk === 'wasapi-shared') tail = '；共享模式输出恒等于设备混音声道数';
  else if (bk === 'wasapi') tail = '；「输出声道数」= 跟随源，独占播放时实际声道随源文件变化';
  else tail = '；「输出声道数」= 跟随源';
  el.innerHTML = '按 <b>' + n + ' 声道</b> 显示' +
    (devCh ? (tf('（设备混音 {0} ch{1}）', [devCh, (t ? (' · ' + esc(t.replace(/^(\d+) 声道 · /, ''))) : '')])) : '') + tail;
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
           (d.default ? t('（默认）') : '') +
           (dm && dm.channels ? ('  ·  ' + dm.channels + 'ch' + (dm.mixRate ? ('/' + (dm.mixRate / 1000) + 'k') : '')) : '') +
           '</option>';
    });
    sel2.innerHTML = h;
    sel2.value = ADV.device || '';
    sel2.onchange = function () {
      ADV.device = this.value;
      loadRates();
      // #4 核心：换设备 ⟹ 按该设备的 channels 重建声道微调（2 → L/R；6 → L/R/C/LFE/Ls/Rs；8 → 7.1）
      repaintTrims();
      saveSettings();
      var dm = devMixOf(this.value);
      toast(dm && dm.channels ? (tf('输出设备已选择（{0} 声道），下次播放生效', [dm.channels]))
                             : '输出设备已选择，下次播放生效');
    };
    repaintTrims();      // 首屏：此时 DEVMIX 已有值，微调按设备声道数渲染
  }).catch(function () { });
}

var RATE_UI={generation:0,savedKey:'',model:null};
function paintRateOptions(model) {
  RATE_UI.model=model;
  ['sRate','sp_rate'].forEach(function(id) {
    var sel=D(id);if(!sel)return;
    var selected=sel.value,list=model.rates||[],known=model.status==='known';
    sel.dataset.rlRateState=model.status;
    Array.prototype.forEach.call(sel.options,function(o) {
      o.disabled=false;o.title='';
      if(o.value==='max') {
        o.textContent=t('自动（按族策略选最高）');
        o.title=known?t('设备最高支持')+' '+Math.max.apply(Math,list)+' Hz':t(model.status==='offline'?'离线处理，不打开音频设备。':'设备能力未确认');
        return;
      }
      if(o.value==='follow') {o.textContent=t('跟随系统')+(model.sysRate?'（'+model.sysRate+' Hz）':'');return;}
      if(o.value==='src')return;
      var rate=Number(o.value);if(!Number.isFinite(rate)||rate<=0)return;
      var unsupported=known&&list.indexOf(rate)<0;
      o.disabled=unsupported;
      o.textContent=String(rate)+(unsupported?'  '+t('（设备不支持）'):model.status==='offline'?'  '+t('（离线处理）'):!known?'  '+t('（能力未确认）'):'');
    });
    // Capability is descriptive: keep saved values and edits, even if unsupported.
    sel.value=selected;
  });
  if(model.device&&D('devName'))D('devName').textContent=t('输出设备：')+model.device;
  if(typeof rlStudioSync==='function'&&typeof rlMode!=='undefined'&&rlMode==='pro')rlStudioSync();
}
function loadRates() {
  var generation=++RATE_UI.generation,backend=ADV.backend||'wasapi',device=ADV.device||'';
  var key=backend+'|'+device,ready=PLAYBACK_SETTINGS.ready;
  var status=ready&&backend==='null'?'offline':'unknown';
  paintRateOptions({status:status,rates:[]});
  // /api/rates probes WASAPI. Its default DAC is not an ASIO or Null capability.
  if(!ready||backend==='null'||backend==='upnp')return Promise.resolve();
  var asio=backend==='asio';
  if(asio&&RATE_UI.savedKey!==key)return Promise.resolve();
  return api(asio?'/api/preset/caps':'/api/rates'+(device?'?device='+encodeURIComponent(device):'')).then(function(j) {
    if(generation!==RATE_UI.generation||key!==(ADV.backend||'wasapi')+'|'+(ADV.device||''))return;
    var result=asio&&j&&j.device||j||{},list=Array.isArray(result.rates)?result.rates.filter(function(r){return Number.isFinite(r)&&r>0;}):[];
    var known=!!(j&&j.ok&&list.length&&(asio?result.backend==='asio':backend==='wasapi'));
    paintRateOptions({status:known?'known':'unknown',rates:list,sysRate:!asio&&j&&j.sysRate||0,device:asio?result.name:j&&j.device});
  }).catch(function() {
    if(generation===RATE_UI.generation)paintRateOptions({status:'unknown',rates:[]});
  });
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
        bScanEl.title = tf('正在扫描音乐库 ({0}/{1})', [(j.scanned || 0), (j.total || 0)]);
      }
      D('scanbar').style.display = '';
      var pct = j.total > 0 ? Math.min(100, j.scanned / j.total * 100) : 0;
      D('scanfill').style.width = pct + '%';
      D('cnt').textContent = tf('扫描中 {0}/{1}', [j.scanned, j.total]);
    } else {
      if (bScanEl) {
        bScanEl.classList.remove('spinning');
        bScanEl.title = t('重新扫描音乐库');
      }
      D('scanbar').style.display = 'none';
      if (wasScanning) {
        // A v5 index migration can establish the current track's album/cover
        // identity while it remains selected. Re-resolve it when scanning ends.
        refreshCurrentCover();
        var msg = tf('扫描完成：共 {0} 首曲目，{1} 张专辑', [(j.tracks || 0), (j.albums || 0)]);
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
    if (typeof rlDailyNode==='function' && S.view==='settings' && D('rlFolderList')) rlDrawDailyRoots();
  }).catch(function () { });
}

/* ---------------- 流媒体 ---------------- */
function loadRemote(id) {
  if(typeof rlPageTransitionBegin==='function')rlPageTransitionBegin('remote');
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
      if (D('remoteGoSet')) D('remoteGoSet').onclick = function () { rlOfferProfessional('stream'); };
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
  if (ft) ft.textContent = s ? (tf('编辑服务器 · {0}', [(s.name || s.base || '')])) : '添加服务器';
  D('sp_src_name').value = s ? (s.name || '') : '';
  D('sp_src_kind').value = s ? (s.kind || 'subsonic') : 'subsonic';
  D('sp_src_base').value = s ? (s.base || '') : '';
  D('sp_src_user').value = s ? (s.user || '') : '';
  D('sp_src_pass').value = '';
  D('sp_src_pass').placeholder = s ? '留空 = 保持原密码' : '密码';
  srcFormMsg(s ? (tf('正在编辑「{0}」', [(s.name || s.base)])) : '');
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
          if (!j || !j.ok) { toast(tf('启用失败：{0}', [((j && j.error) || '未知错误')])); return; }
          toast(tf('已启用「{0}」', [(s ? (s.name || s.base) : id)]));
          loadSrcConfig();
        }).catch(function () { toast('启用失败：服务没响应'); });
        return;
      }
      if (a === 'del') {
        if (!confirm(tf('删除服务器「{0}」？只删配置，不动已下载缓存。', [(s ? (s.name || s.base) : id)]))) return;
        api('/api/src/config', 'action=remove&id=' + encodeURIComponent(id)).then(function (j) {
          if (!j || !j.ok) { toast(tf('删除失败：{0}', [((j && j.error) || '未知错误')])); return; }
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
    if (!j || !j.ok) { srcFormMsg(tf('保存失败：{0}', [((j && j.error) || '未知错误')]), false); return; }
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
// 2026-10-23（合并 r4-shell 的tf('EQ 移入设置页{0}', [r4-np]) 的"单曲页 EQ 芯片"）：
// 以前 EQ 是一级视图（setView('eq') + loadEqView()），现在它是设置页里的一个页签。
// 统一走这个入口，别再直接 setView('eq')（那个视图已经不存在）。
function openSettingsTab(g, remember) {
  if(['pcm','sdm','eq'].indexOf(g)>=0){
    if(rlMode!=='pro')rlSetMode('pro',!!remember);setView('listening');
    if(typeof rlOpenStudioModule==='function')rlOpenStudioModule(g);return;
  }
  if(rlMode!=='pro') rlSetMode('pro',!!remember);
  setView('settings',true);
  var b = document.querySelector('#setpage .stab[data-g=' + (g || 'eq') + ']');
  if (b) b.click();
  var sp = document.getElementById('setpage');
  if (sp && sp.scrollTo) sp.scrollTo(0, 0);
}
/* QUICK_START_LOGIC_BEGIN */
// Capability-first workstation flow inspired by RLPlayer-Mac (GPL-3.0-or-later).
function quickStartModel(caps, backend, runtime) {
  var d = (caps && caps.ok && caps.device) || {}, r = runtime || {};
  var remote = backend === 'upnp', shared = backend === 'wasapi-shared', offline=backend==='null';
  var known = !offline && !remote && !!(caps && caps.ok && d.name && Array.isArray(d.rates) && d.rates.length && d.maxRate > 0);
  var nativeDsd = backend === 'asio' && d.dsdNative === true;
  return {name: offline?t('离线处理（Null）'):(remote?t('请查看投放设备能力'):(d.name || t('未知'))), backend: backend || 'wasapi', known: known,
    remote: remote, shared: shared, offline:offline, maxRate: known ? d.maxRate : 0,
    pcmReady: known && !remote && !shared,
    dsdReady: known && !remote && !shared && (nativeDsd || d.dop === true),
    dsdRoute: nativeDsd ? 'Native DSD' : (d.dop ? 'DoP' : t('不支持')),
    dsdMax: nativeDsd ? d.dsdNativeMax : d.dopMax,
    gpuAvailable: !!r.gpuOn, gpuUsed: !!r.gpuUsed,
    recommended: known && !remote && !shared ? 'pcm' : '', notes: (caps && caps.notes) || []};
}
function quickStartHtml(m) {
  var chip = function (label, value) { return '<div class=qs-cap><span>' + esc(t(label)) + '</span><strong>' + esc(value) + '</strong></div>'; };
  var choices = [['bitperfect', '原音直通', '保持源采样率；关闭会改变样本的处理。', true],
    ['pcm', 'PCM 升频', '根据设备能力设置 PCM 升频。', m.pcmReady],
    ['dsd', 'DSD 升频', '通过 Native DSD 或 DoP 输出。', m.dsdReady]];
  var h = '<section class=quick-start aria-labelledby=qsTitle><div class=qs-intro><div><div class=qs-eyebrow>RLPLAYER / START</div>'
    + '<h1 id=qsTitle>' + esc(t('快速开始')) + '</h1><p>' + esc(t('先确认输出，再选择你的播放方式。')) + '</p></div>'
    + '<button class=btn id=qsEnterLibrary>' + esc(t('打开音乐库')) + ' →</button></div>'
    + '<div class=qs-device><div><span class=qs-label>' + esc(t('当前设备')) + '</span><strong>' + esc(m.name) + '</strong>'
    + '<span class=qs-backend>' + esc(m.backend) + '</span></div><button class=btn id=qsRefresh>' + esc(t('刷新硬件能力')) + '</button></div>'
    + '<div class=qs-caps>' + chip('PCM', m.remote ? t('请查看投放设备能力') : (m.maxRate ? (m.maxRate / 1000 + ' kHz') : t('未知')))
    + chip('DSD', m.dsdReady ? (m.dsdRoute + ' · ' + (m.dsdMax || t('未知'))) : (m.remote ? t('请查看投放设备能力') : t('不支持')))
    + chip('GPU', m.gpuUsed ? t('本次正在使用') : (m.gpuAvailable ? t('可用，本次未使用') : t('CPU 处理'))) + '</div>';
  if (m.remote || m.shared || !m.known) h += '<p class=qs-note role=status>' + esc(t(m.remote ? '投放格式以远端设备声明为准，本机声卡能力不适用。' : (m.shared ? '共享输出经过系统混音，不承诺逐位直通。' : (m.offline?'离线处理，不打开音频设备。':'能力尚未确认，请先选择输出设备。')))) + '</p>';
  h += '<h2>' + esc(t('选择播放方式')) + '</h2><div class=qs-profiles>';
  choices.forEach(function (c) {
    h += '<button class=qs-profile data-qs-profile="' + c[0] + '" aria-pressed=false' + (c[3] ? '' : ' disabled') + '><span class=qs-profile-top><strong>' + esc(t(c[1])) + '</strong>'
      + (m.recommended === c[0] ? '<span class=qs-recommend>' + esc(t('推荐')) + '</span>' : '') + '</span><span>' + esc(t(c[2])) + '</span><span class=qs-profile-action>' + esc(t(c[3] ? '查看并应用' : '当前能力未确认或不支持')) + ' →</span></button>';
  });
  h += '</div><div id=qsPreview class=qs-preview hidden aria-live=polite></div><h2>' + esc(t('下一步')) + '</h2><div class=qs-actions>';
  [['qsChooseMusic', '添加音乐目录'], ['qsChooseOutput', '选择输出'], ['qsInspectSignal', '查看信号链'], ['qsOpenEq', 'EQ / 声音校准']].forEach(function (a) {
    h += '<button class=qs-action id=' + a[0] + '>' + esc(t(a[1])) + '<span>↗</span></button>';
  });
  return h + '</div><p class=qs-foot>' + esc(t('这里只更改你确认的设置，不会自动播放。高级参数仍可在设置中调整。')) + '</p></section>';
}
/* QUICK_START_LOGIC_END */
var quickStartSelection = '', quickStartGeneration = 0, quickStartRuntime = '';
/* QUICK_START_PREFERENCE_BEGIN */
var QUICK_START_PREF = {loaded:false, initial:true, navigated:false, saving:false};
function quickStartEnabled() {
  return QUICK_START_PREF.loaded && ADV.quickStart !== 0;
}
function syncQuickStartVisibility() {
  var nav=D('navQuickStart');if(nav){nav.hidden=true;nav.style.display='none';}
  var settings=document.querySelector('.side .nav[data-v=settings]');
  if(settings)settings.hidden=rlMode!=='pro';
}
function applyQuickStartPreference(value) {
  ADV.quickStart=value===0?0:1;QUICK_START_PREF.loaded=true;syncQuickStartVisibility();
  if(QUICK_START_PREF.initial&&!QUICK_START_PREF.navigated)setView('listening');
  QUICK_START_PREF.initial=false;
}
function saveQuickStartPreference(value) {
  if (!ensurePlaybackSettings() || QUICK_START_PREF.saving) {
    syncQuickStartVisibility(); return Promise.resolve({ok:false});
  }
  var next = value ? 1 : 0;
  QUICK_START_PREF.saving = true;
  if (D('sp_quickstart')) D('sp_quickstart').disabled = true;
  // This appearance preference must not re-save volume or any audio parameter.
  return persistSettings('quickstart=' + next).then(function (j) {
    QUICK_START_PREF.saving = false;
    if (j && j.ok) applyQuickStartPreference(next);
    else syncQuickStartVisibility();
    return j || {ok:false};
  });
}
/* QUICK_START_PREFERENCE_END */
// A presentation preference never enters the audio configuration or preset planner.
var rlMode = 'listen', rlLastStatus = null, rlHomeAlbums = [];
function rlSetMode(mode, remember) {
  rlMode = mode === 'pro' ? 'pro' : 'listen';
  document.documentElement.dataset.rlMode = rlMode;
  if (remember) {
    try { localStorage.setItem('rlplayer-ui-mode', rlMode); } catch (_) {}
  }
  Array.prototype.forEach.call(document.querySelectorAll('#rlViewModes [data-rl-mode]'), function (button) {
    button.setAttribute('aria-pressed', String(button.dataset.rlMode === rlMode));
  });
  if (typeof rlSettingsState!=='undefined' && rlSettingsState) {
    if(mode==='listen' && S.view==='settings')setView('listening');
    else if(S.view==='settings')rlSettingsRoute(false);
  }
  if (D('rlListenHome')) rlPaintHome();
  if(typeof rlConsole!=='undefined' && PLAYBACK_SETTINGS.ready)rlConsole.refresh().catch(function(){});
  rlSyncHomeNavigation();
}
(function () {
  var saved;
  try { saved = localStorage.getItem('rlplayer-ui-mode'); } catch (_) {}
  rlSetMode(saved, false);
  D('rlViewModes').onclick = function (event) {
    var button = event.target.closest('button[data-rl-mode]');
    if (button && button.dataset.rlMode !== rlMode) rlSetMode(button.dataset.rlMode, true);
  };
})();
function rlSetText(id, value) {
  var el = D(id);
  if (el && el.textContent !== value) el.textContent = value;
}
function rlHomeActive() { return S.view === 'station' || S.view === 'listening'; }
function rlHomeLabel() { return t(rlMode === 'pro' ? '数字音乐台' : '聆听室'); }
function rlSetNavCurrent(button, current) {
  button.classList.toggle('on', current);
  if (current) button.setAttribute('aria-current', 'page');
  else button.removeAttribute('aria-current');
}
function rlSyncHomeNavigation() {
  var label=rlHomeLabel(),nav=D('rlHomeNavLabel');
  rlSetText('rlHomeNavLabel',label);syncQuickStartVisibility();
  if(!rlHomeActive())return;
  if(nav)rlSetNavCurrent(nav.closest('.nav'),true);
  head(label,'','');
}
function rlNav(view) {
  var link = document.querySelector('.side .nav[data-v="' + view + '"]');
  if (link) link.click();
}
function rlHomeButton(id, label, primary) {
  return '<button type=button class="btn' + (primary ? ' pri' : '') + '" id=' + id + '>' + esc(label) + '</button>';
}
function rlInstallHome() {
  if (D('rlListenHome')) return;
  var pathNode = function (title, id) {
    return '<div class=rlPathNode><small>' + esc(title) + '</small><strong id=' + id + '></strong></div>';
  };
  D('list').innerHTML = '<section id=rlListenHome aria-labelledby=rlHomeTitle><header class=rlListenHeading><h1 id=rlHomeTitle></h1>'
     + '</header><div class=rlListenGrid><section class=rlListenHero>'
    + '<div class=rlListenCover id=rlListenCover aria-hidden=true></div><div class=rlListenInfo>'
    + '<div class=rlListenStatus id=rlListenStatus></div><h2 class=rlListenTitle id=rlListenTitle></h2>'
    + '<p class=rlListenMeta id=rlListenMeta></p><div class=rlListenActions>'
    + '<button type=button class=btn id=rlListenFavorite aria-pressed=false disabled><svg width=17 height=17 viewBox="0 0 24 24" fill=none stroke=currentColor stroke-width=1.8 aria-hidden=true><path d="M20.84 4.61a5.5 5.5 0 00-7.78 0L12 5.67l-1.06-1.06a5.5 5.5 0 00-7.78 7.78l1.06 1.06L12 21.23l7.78-7.78 1.06-1.06a5.5 5.5 0 000-7.78z"/></svg><span></span></button>' + rlHomeButton('rlListenMore', '打开曲目详情')
    + '</div></div></section><section class=rlListenQueue><div class=rlQueueHeading><h2>' + esc('播放队列') + '</h2>'
    + rlHomeButton('rlQueueOpen', '查看全部') + '</div><div id=rlListenQueueList></div></section></div>'
    + '<section class=rlProPath id=rlProPath><div class=rlQueueHeading><h2>' + esc('实际信号路径') + '</h2>'
    + rlHomeButton('rlChainOpen', '查看信号链') + '</div><div class=rlPathNodes>'
    + pathNode('源文件','rlPathSource') + pathNode('声音处理','rlPathProcessing') + pathNode('设备输出','rlPathOutput') + '</div></section>'
    + '<section id=rlHomeShelf hidden><div class=rlQueueHeading><h2>' + esc('专辑') + '</h2>'
    + rlHomeButton('rlLibraryOpen', '打开音乐库') + '</div><div class=rlShelfItems id=rlShelfItems></div></section></section>';

  D('rlListenFavorite').onclick = function (event) {
    var file=this.dataset.file;if(file)toggleFav(file,event);
  };
  D('rlListenMore').onclick = function () {
    var queued = (S.q.items || [])[S.q.index || 0];
    if (S.lastCur && (S.playing || !queued || queued === S.lastCur)) openNowPlaying(); else if (queued) rlNav('albums');
    else { rlOpenDailyTask('library'); }
  };
  D('rlQueueOpen').onclick = function () { rlNav('queue'); };
  D('rlLibraryOpen').onclick = function () { rlNav('albums'); };
  D('rlChainOpen').onclick = showChain;
  D('rlListenQueueList').onclick = function (event) {
    var row = event.target.closest('[data-rl-index]');
    if (!row) return;
    var index = Number(row.dataset.rlIndex), q = S.q;
    if (!q || !q.items[index]) return;
    var cue = (q.cues || [])[index] || [-1,-1];
    playPath(q.items[index],cue[0],cue[1],index);
  };
  D('rlShelfItems').onclick = function (event) {
    var button = event.target.closest('[data-rl-album]');
    if (button) openAlbum(button.dataset.rlAlbum);
  };
  rlInstallStudio();
  rlInstallListeningControls();
  applyI18n(D('rlListenHome'));
  rlPaintShelf();
}
function rlPaintShelf() {
  var shelf = D('rlHomeShelf');
  if (!shelf || !rlHomeActive()) return;
  var list = D('rlShelfItems');
  var html = rlHomeAlbums.map(function (a) {
    return '<button type=button class=rlShelfItem data-rl-album="' + esc(a.k) + '"><span class=rlShelfCover aria-hidden=true>'
      + (a.cv ? '<img alt="" loading=lazy src="/api/lib/cover?k=' + encodeURIComponent(a.k) + '" onerror="this.hidden=true">' : '')
      + '</span><span><strong>' + esc(a.name) + '</strong><small>' + esc(a.artist || '') + '</small></span></button>';
  }).join('');
  if (list.rlHtml !== html) { list.rlHtml = html; list.innerHTML = html; }
  shelf.hidden = !rlHomeAlbums.length;
}
function rlPaintQueue() {
  var list = D('rlListenQueueList');
  if (!list || !rlHomeActive()) return;
  var q = S.q, items = q.items || [], index = Math.min(Math.max(q.index || 0, 0), items.length-1);
  var start = Math.max(0, Math.min(index, items.length - 4)), end = Math.min(items.length, start+4);
  var html = '';
  for (var j=start; j<end; j++) {
    var cue = (q.cues || [])[j] || [], title = nam(items[j]);
    if (cue[0] >= 0) title += ' · ' + fmt(cue[0]);
    html += '<button type=button class="rlListenQueueRow' + (j === index ? ' on' : '') + '" data-rl-index=' + j
      + (j === index ? ' aria-current=true' : '') + '><span class=rlQueueNumber>' + (j+1) + '</span><span class=rlQueueTitle>' + esc(title) + '</span></button>';
  }
  if (!html) html = '<p class=rlQueueEmpty>' + esc(t('队列为空')) + '</p>';
  if (list.rlHtml !== html) { list.rlHtml=html; list.innerHTML=html; }
  rlPaintStudioQueue();
}
function rlPaintListeningFavorite(file) {
  var heart=D('rlListenFavorite');if(!heart)return;
  file=file===undefined?heart.dataset.file:String(file||'');heart.dataset.file=file;
  var on=!!file&&FAV_SET.has(file),label=t(on?'取消收藏':'收藏');
  heart.disabled=!file;heart.classList.toggle('on',on);heart.setAttribute('aria-pressed',String(on));heart.setAttribute('aria-label',label);heart.title=label;heart.querySelector('span').textContent=label;
}
function rlPaintHome() {
  var home = D('rlListenHome');
  if (!home || !rlHomeActive()) return;
  var q = S.q || {}, index = q.index || 0, queued = (q.items || [])[index] || '';
  var file = S.playing ? S.lastCur : (queued || S.lastCur), cue = (q.cues || [])[index] || [];
  var current = !!file && file === S.lastCur, m = current && S.metaFile === file ? S.curMeta : null;
  var facts = rlLastStatus && rlLastStatus.current === S.lastCur && rlLastStatus.playing ? rlLastStatus : null;
  rlSetText('rlHomeTitle', rlHomeLabel());
  var status = S.playing ? (S.paused ? '已暂停' : '播放中') : (file ? '队列待播' : '未播放');
  rlSetText('rlListenStatus', t(status));
  var title = m && m.title || (file ? nam(file) : t('等你选一张唱片'));
  if (file && file === queued && cue[0] >= 0) title += ' · ' + fmt(cue[0]);
  rlSetText('rlListenTitle', title);
  rlSetText('rlListenMeta', m ? [m.artist,m.album || m.folderName].filter(Boolean).join(' · ') : (file ? parentFolder(file) : t('从音乐库选择专辑，或添加音乐目录。')));
  var art = D('rlListenCover'), bg = current && S.metaFile === file ? D('cv').style.backgroundImage : '';
  if (art.dataset.image !== bg) {
    art.dataset.image = bg; art.style.backgroundImage = bg; art.classList.toggle('has-art', !!bg);
  }
  rlPaintListeningFavorite(file);
  rlSetText('rlListenMore', t(current ? '打开曲目详情' : (queued ? '打开音乐库' : '添加音乐目录')));
  D('rlProPath').hidden = rlMode !== 'pro';
  var pending = t('等候播放'), sourceFmt = m && (m.sr || m.rate || m.sampleRate);
  rlSetText('rlPathSource', facts ? (sourceFmt ? (sourceFmt / 1000) + ' kHz' : nam(facts.current)) : pending);
  var badge = facts ? bpBadgeInfo() : null;
  rlSetText('rlPathProcessing', facts ? (badge.text || t('未知')) : pending);
  rlSetText('rlPathOutput', facts && facts.outRate > 0 ? (facts.outRate / 1000) + ' kHz' + (facts.outBits > 0 ? ' / ' + facts.outBits + ' bit' : '') : pending);
  rlPaintStudio({file:file,current:current,m:m,facts:facts,title:title,status:status,bg:bg});
  rlPaintListeningControls();
}
function rlFocus(id) {
  var field = D(id);
  if (!field) return;
  field.scrollIntoView({block:'center'});
  field.focus({preventScroll:true});
}
// The two forms keep their own DOM and unfinished entries. Only the active
// fragment is attached, so professional tabs never become daily task controls.
var rlSettingsState = {parked:{}, task:'home', deviceDirty:false, deviceBusy:false, deviceGeneration:0,
  profile:'', caps:null, applying:false, libraryBusy:false, updateBusy:false, update:null, staged:false};
var rlTasks = {
  out:['选择输出设备','设备和声道','out'], preset:['选择播放方式','原音直通或升频','out'],
  library:['选择音乐目录','添加或管理本地音乐','adv'], diag:['问题反馈','记录问题与诊断信息','diag'],
  upd:['关于与更新','版本与增量更新','upd'], pro:['打开专业参数','查看全部调整选项','out']
};
function rlDailyButton(id,label,primary) {
  return '<button type=button id="'+id+'" class="btn'+(primary?' pri':'')+'">'+esc(label)+'</button>';
}
function rlDailyField(id,label,control) {
  return '<div class=rlDailyField><label for="'+id+'">'+esc(label)+'</label>'+control+'</div>';
}
function rlDailySection(key,content) {
  return '<section class=rlDailyDetail data-rl-detail='+key+' hidden><h2 tabindex=-1>'+esc(rlTasks[key][0])+'</h2>'+content+'</section>';
}
function rlDailyStatus(id) { return '<p class=rlDailyStatus role=status aria-live=polite id='+id+'></p>'; }
function rlSettingsMenu() {
  var home='<section id=rlDailyMenu><h2 id=rlSettingsTitle tabindex=-1>日常设置</h2><div class=rlSettingTasks>';
  Object.keys(rlTasks).forEach(function(key) { var row=rlTasks[key];
    home+='<button type=button data-rl-task='+key+'><strong>'+esc(row[0])+'</strong><small>'+esc(row[1])+'</small></button>'; });
  home+='</div><button type=button class=btn data-rl-task=help>快捷键与技巧</button></section>';
  var out=rlDailyField('rlOutputKind','播放设备类型','<select id=rlOutputKind></select>')
    +rlDailyField('rlOutputDevice','输出设备','<select id=rlOutputDevice></select>')
    +'<div class=rlDailyActions>'+rlDailyButton('rlOutputSave','保存输出设备',true)+rlDailyButton('rlOutputRefresh','刷新设备')+'</div>'+rlDailyStatus('rlOutputMsg');
  var preset='<div class=rlDailyActions>'
    +[['bitperfect','原音直通'],['pcm','PCM 升频'],['dsd','DSD 升频']].map(function(p){return '<button type=button class=btn data-rl-profile='+p[0]+' aria-pressed=false>'+esc(p[1])+'</button>';}).join('')
    +'</div>'+rlDailyStatus('rlPresetHint')+'<div id=rlPresetPreview hidden><p id=rlPresetPlan></p>'+rlDailyButton('rlPresetApply','应用预设',true)+'</div>'+rlDailyStatus('rlPresetMsg');
  var library=rlDailyField('rlFolderPath','添加音乐目录','<input id=rlFolderPath type=text spellcheck=false dir=auto placeholder="D:&#92;Music">')
    +'<div class=rlDailyActions>'+rlDailyButton('rlFolderPick','选择文件夹')+rlDailyButton('rlFolderAdd','添加并扫描',true)+rlDailyButton('rlFolderScan','重新扫描音乐库')+'</div>'
    +'<h3>音乐目录</h3><ul id=rlFolderList></ul>'+rlDailyStatus('rlFolderMsg');
  var fb='<p>提交时会附上版本、设备、设置与脱敏日志，用于排查问题。</p>'
    +rlDailyField('rlFbTitle','标题','<input id=rlFbTitle type=text maxlength=140>')
    +rlDailyField('rlFbBody','详细描述','<textarea id=rlFbBody rows=4></textarea>')
    +'<div class=rlDailyActions>'+rlDailyButton('rlFbPreview','先看会发送什么')+rlDailyButton('rlFbSubmit','提交反馈',true)+'</div>'
    +rlDailyStatus('rlFbMsg')+'<div id=rlFbPreviewbox style="display:none" class=rlDailyResult></div><p id=rlFbResult class=rlDailyResult></p>';
  var update='<p><span>当前版本</span> <strong id=rlUpdateVersion>—</strong></p><div class=rlDailyActions>'
    +rlDailyButton('rlUpdateCheck','检查更新',true)+rlDailyButton('rlUpdateDownload','下载更新')+rlDailyButton('rlUpdateManual','下载完整版')
    +rlDailyButton('rlUpdateBaidu','百度网盘')+rlDailyButton('rlUpdateApply','立即更新并重启')+'</div>'+rlDailyStatus('rlUpdateMsg')+'<p id=rlUpdateNotes class=rlDailyResult></p>';
  return '<div id=rlListenSettings>'+rlDailyButton('rlSettingsBack','返回日常设置')+home+rlDailySection('out',out)+rlDailySection('preset',preset)
    +rlDailySection('library',library)+rlDailySection('diag',fb)+rlDailySection('upd',update)+'</div>';
}
function rlDailyNode(id) { return rlSettingsState.root && rlSettingsState.root.querySelector('#'+id); }
function rlDailySay(id,text) { var el=rlDailyNode(id); if(el) el.textContent=text||''; }
function rlSetSettingsSurface(mode) {
  var page=D('setpage'), state=rlSettingsState, before=page.dataset.rlSurface;
  if(before===mode) return;
  if(before) {
    var parked=document.createDocumentFragment();
    while(page.firstChild) parked.appendChild(page.firstChild);
    state.parked[before]=parked;
  }
  page.dataset.rlSurface=mode;
  page.dataset.rlSettings=mode==='listen'?'home':'detail';
  if(state.parked[mode]) {page.appendChild(state.parked[mode]); delete state.parked[mode];}
  else if(mode==='pro') { state.syncPreset=null;state.syncDevice=false;renderSettingsPage(); }
  else {page.innerHTML=rlSettingsMenu();state.root=D('rlListenSettings');rlBindDaily();}
  applyI18n(page);
  if(mode==='pro')rlSyncProfessionalForm();
}
function rlSyncProfessionalForm() {
  var state=rlSettingsState,page=D('setpage');
  if(state.syncDevice) {
    if(D('sp_backend')) D('sp_backend').value=ADV.backend;
    applyBackendState();loadDeviceList();state.syncDevice=false;
  }
  if(state.syncPreset) {
    var draft=settingsProfessionalDraft(), group=page.querySelector('.stab.on')?.dataset.g;
    renderSettingsPage();
    restoreProfessionalDraft(draft,group,state.syncPreset);state.syncPreset=null;
  }
}
function settingsProfessionalDraft() {
  var draft={};
  D('setpage').querySelectorAll('input[id],textarea[id],select[id]').forEach(function(el){
    if(el.dataset.rlEdited==='1' || ['sp_fbtitle','sp_fbbody','sp_fbsteps','sp_root'].indexOf(el.id)>=0)
      draft[el.id]=el.type==='checkbox'?el.checked:el.value;
  });
  return draft;
}
function restoreProfessionalDraft(draft, group, profile) {
  var replaced=['sp_rate','sp_filt','sp_filtNx','sp_shaper','sp_taps','sp_dsdrate','sp_outmode','sp_2s','sp_sdmo1','sp_sdmoNx','sp_directsdm'];
  if(profile==='bitperfect') replaced=replaced.concat(['sp_eq','sp_ir','sp_phase','sp_irgpu','sp_isp','sp_loud','sp_roomauto','sp_cleanup','sp_rgmode','sp_atten','sp_transition','peqBypass']);
  Object.keys(draft).forEach(function(id){var el=D(id);if(!el||replaced.indexOf(id)>=0)return;
    if(el.type==='checkbox')el.checked=draft[id];else el.value=draft[id];el.dataset.rlEdited='1';});
  var tab=group&&D('setpage').querySelector('.stab[data-g="'+group+'"]');if(tab)tab.click();
}
function rlSettingsRoute(reset) {
  if(reset)rlSettingsState.proGroup='';
  rlSetSettingsSurface('pro');
  head(t('专业模式'),'', '');
}
function rlOfferProfessional(group) {openSettingsTab(group);}
function rlOpenDailyTask(task,proGroup) {
  if(task==='library' && rlMode==='listen'){rlOpenInlineLibrary();return;}
  if(rlMode==='listen' && (task==='out'||task==='preset')){
    if(!rlHomeActive())setView('listening');
    if(typeof rlOpenListeningCard==='function')rlOpenListeningCard(task==='out'?'output':'profile');
    return;
  }
  openSettingsTab(proGroup||(rlTasks[task]||rlTasks.out)[2]);
}
function rlShowDaily(task,focus) {
  var root=rlSettingsState.root;if(!root||!root.isConnected)return;
  var previous=rlSettingsState.task;
  if(!rlTasks[task]||task==='pro')task='home';
  rlSettingsState.task=task;
  D('setpage').dataset.rlSettings=task==='home'?'home':'detail';
  D('rlDailyMenu').hidden=task!=='home';D('rlSettingsBack').hidden=task==='home';
  root.querySelectorAll('[data-rl-detail]').forEach(function(section){section.hidden=section.dataset.rlDetail!==task;});
  head(t(task==='home'?'日常设置':rlTasks[task][0]),'', '');
  if(task==='out')rlInitOutput();
  if(task==='preset')rlInitPreset();
  if(task==='library')rlDrawDailyRoots();
  if(task==='upd')rlReadUpdateState();
  if(focus){var target=task==='home'&&rlTasks[previous]?root.querySelector('[data-rl-task="'+previous+'"]'):
    task==='home'?D('rlSettingsTitle'):root.querySelector('[data-rl-detail="'+task+'"] h2');if(target)target.focus();}
}
function rlBindDaily() {
  var root=rlSettingsState.root;
  root.addEventListener('click',function(event){
    var b=event.target.closest('[data-rl-task]');if(!b)return;
    var task=b.dataset.rlTask;
    if(task==='help')D('bHelp').click();
    else if(task==='pro')openSettingsTab(rlSettingsState.proGroup||'out',true);
    else rlShowDaily(task,true);
  });
  D('rlSettingsBack').onclick=function(){rlShowDaily('home',true);};
  D('rlOutputKind').onchange=function(){rlSettingsState.deviceDirty=true;rlDevices('');};
  D('rlOutputDevice').onchange=function(){rlSettingsState.deviceDirty=true;};
  D('rlOutputSave').onclick=rlSaveOutput;
  D('rlOutputRefresh').onclick=function(){rlDevices(D('rlOutputDevice').value);};
  root.querySelectorAll('[data-rl-profile]').forEach(function(b){b.onclick=function(){rlSettingsState.profile=b.dataset.rlProfile;rlDailySay('rlPresetMsg','');rlPaintPreset();};});
  D('rlPresetApply').onclick=rlApplyDailyPreset;
  D('rlFolderPick').hidden=!WV;
  D('rlFolderPick').onclick=function(){if(WV)wcmd('pick-music-folder');};
  D('rlFolderAdd').onclick=rlAddFolder;D('rlFolderScan').onclick=rlScanFolder;
  function feedbackField(id){var key=id.slice(5);return rlDailyNode('rlFb'+key.charAt(0).toUpperCase()+key.slice(1));}
  D('rlFbPreview').onclick=function(){fbPreview(feedbackField);};
  D('rlFbSubmit').onclick=function(){fbSubmit(feedbackField);};
  D('rlUpdateCheck').onclick=rlCheckUpdate;D('rlUpdateDownload').onclick=rlDownloadUpdate;
  D('rlUpdateManual').onclick=function(){rlManualUpdate('github');};D('rlUpdateBaidu').onclick=function(){rlManualUpdate('baidu');};
  D('rlUpdateApply').onclick=rlInstallUpdate;
  ['rlUpdateDownload','rlUpdateManual','rlUpdateBaidu','rlUpdateApply'].forEach(function(id){D(id).hidden=true;});
}
function rlInitOutput() {
  var state=rlSettingsState, kind=D('rlOutputKind'), device=D('rlOutputDevice'), bk=ADV.backend||'wasapi';
  if(state.deviceDirty && kind.options.length)return;
  var kinds=[['wasapi-shared','Windows 共享输出'],['wasapi','Windows 独占输出'],['asio','ASIO']];
  if(!kinds.some(function(o){return o[0]===bk;}))kinds.push([bk,bk==='upnp'?'UPnP / DLNA':bk==='null'?'离线处理（Null）':bk]);
  kind.replaceChildren();kinds.forEach(function(o){kind.add(new Option(t(o[1]),o[0]));});kind.value=bk;
  rlDevices(ADV.device||'');
}
function rlDevices(chosen) {
  var state=rlSettingsState, gen=++state.deviceGeneration, kind=rlDailyNode('rlOutputKind').value, select=rlDailyNode('rlOutputDevice');
  var editable=['wasapi','wasapi-shared','asio'].indexOf(kind)>=0;
  select.disabled=true;rlDailyNode('rlOutputSave').disabled=true;
  if(!editable){select.replaceChildren(new Option(chosen||t('当前设备'),chosen));rlDailySay('rlOutputMsg',t('可选择 Windows 或 ASIO 本地输出。'));return;}
  apiGetRaw(kind==='asio'?'/api/asiodevices':'/api/devices').then(function(j){
    if(gen!==state.deviceGeneration)return;
    if(!j||!j.ok)throw Error((j&&j.error)||t('加载失败'));
    var list=(j.devices||j.drivers||[]).filter(function(d){return d&&d.id;});
    select.replaceChildren(new Option(t('默认设备'),''));
    list.forEach(function(d){select.add(new Option(d.name||d.id,d.id));});
    if(chosen&&!list.some(function(d){return d.id===chosen;}))select.add(new Option(chosen,chosen));
    select.value=chosen;select.disabled=false;rlDailyNode('rlOutputSave').disabled=state.deviceBusy;
    rlDailySay('rlOutputMsg',t('更改将在停止后重新播放时使用。'));
  }).catch(function(e){if(gen===state.deviceGeneration)rlDailySay('rlOutputMsg',e.message||t('加载失败'));});
}
function rlSaveOutput(){
  var state=rlSettingsState;if(!ensurePlaybackSettings()||state.deviceBusy)return;
  var kind=rlDailyNode('rlOutputKind'), device=rlDailyNode('rlOutputDevice'), button=rlDailyNode('rlOutputSave');
  if(device.disabled)return;
  var bk=kind.value,id=device.value;state.deviceBusy=true;button.disabled=kind.disabled=device.disabled=true;
  apiAct('/api/settings','backend='+encodeURIComponent(bk)+'&device='+encodeURIComponent(id)).then(function(j){
    if(!j||!j.ok)throw Error((j&&j.error)||t('保存失败'));
    return apiGetRaw('/api/settings');
  }).then(function(j){
    if(!j||!j.ok||!j.kv||j.kv.backend!==bk||(j.kv.device||'')!==id) throw Error(t('无法确认已保存，请重试。'));
    ADV.backend=bk;ADV.device=id;state.deviceDirty=false;state.syncDevice=true;_capsCache=null;
    rlDailySay('rlOutputMsg',t('输出设备已保存，停止后重新播放时使用。'));
    if(D('sp_backend')){D('sp_backend').value=bk;applyBackendState();loadDeviceList();state.syncDevice=false;}
  }).catch(function(e){rlDailySay('rlOutputMsg',e.message||t('保存失败'));}).finally(function(){state.deviceBusy=false;button.disabled=kind.disabled=device.disabled=false;});
}
function rlInitPreset() {
  var state=rlSettingsState;state.caps=null;rlPaintPreset();
  var bk=ADV.backend;
  loadPresetCaps(true).then(function(j){if(bk===ADV.backend){state.caps=j;rlPaintPreset();}});
}
function rlPaintPreset() {
  var state=rlSettingsState, root=state.root;if(!root||state.task!=='preset')return;
  var model=quickStartModel(state.caps,ADV.backend,S), profile=state.profile;
  if((profile==='pcm'&&!model.pcmReady)||(profile==='dsd'&&!model.dsdReady))profile='';
  root.querySelectorAll('[data-rl-profile]').forEach(function(b){
    var k=b.dataset.rlProfile;b.disabled=state.applying||(k==='pcm'&&!model.pcmReady)||(k==='dsd'&&!model.dsdReady);
    b.setAttribute('aria-pressed',String(k===profile));
  });
  rlDailySay('rlPresetHint',t(S.playing||S.paused?'请先停止播放，再应用预设。':!model.pcmReady?'能力尚未确认，请先选择输出设备。':'选择一种方式，先查看会更改的设置。'));
  rlDailyNode('rlPresetPreview').hidden=!profile;
  if(profile)rlDailySay('rlPresetPlan',t(profile==='bitperfect'?
    '保留当前音量，恢复源采样率，关闭 EQ、卷积、响度、房间校正、修复与预衰减，复位声道增益。音量衰减或共享输出时不能逐位直通。':
    '按设备能力替换升频、滤波与抖动设置，保留当前音量、EQ、卷积和校准。'));
  rlDailyNode('rlPresetApply').disabled=!profile||state.applying||S.playing||S.paused||!PLAYBACK_SETTINGS.ready;
}
function rlApplyDailyPreset(){
  var state=rlSettingsState, profile=state.profile;
  if(!ensurePlaybackSettings()||state.applying||rlDailyNode('rlPresetApply').disabled)return;
  state.applying=true;rlPaintPreset();
  apiGetRaw('/api/status').then(function(j){
    if(!j||!j.ok||j.playing||j.paused)throw Error(t('请先停止播放，再应用预设。'));
    return apiAct('/api/preset/apply?profile='+encodeURIComponent(profile)+'&preserve_processing=1&keep_volume=1&require_stopped=1','');
  }).then(function(j){
    if(!j||!j.ok)throw Error((j&&j.error)||t('应用预设失败'));
    state.pendingPreset=profile;state.profile='';
    return loadSettings();
  }).then(function(j){if(!j||!j.ok)throw Error(t('无法确认已保存，请重试。'));rlDailySay('rlPresetMsg',t('预设已应用。'));})
    .catch(function(e){rlDailySay('rlPresetMsg',e.message||t('应用预设失败'));})
    .finally(function(){state.applying=false;rlPaintPreset();});
}
function rlDrawDailyRoots(){
  var list=rlDailyNode('rlFolderList');if(!list)return;
  var roots=S.roots||[], key=JSON.stringify(roots)+'|'+I18N.lang;
  if(list.rlKey===key)return;list.rlKey=key;list.replaceChildren();
  (roots.length?roots:[t('还没有添加目录')]).forEach(function(path){var li=document.createElement('li');li.textContent=path;list.appendChild(li);});
}
function rlReceiveMusicFolder(path){
  if(typeof rlListeningFolderPicked==='function'&&rlListeningFolderPicked(path))return;
  var field=rlDailyNode('rlFolderPath');if(typeof path==='string'&&path&&field){field.value=path;if(field.isConnected)field.focus();}
}
function rlFolderBusy(busy){rlSettingsState.libraryBusy=busy;['rlFolderAdd','rlFolderScan','rlFolderPick','rlFolderPath'].forEach(function(id){rlDailyNode(id).disabled=busy;});}
function rlScanRequest(){return apiAct('/api/lib/scan','').then(function(j){if(!j||!j.ok)throw Error((j&&j.error)||t('扫描失败'));loadLibStats();});}
function rlScanFolder(){if(rlSettingsState.libraryBusy)return;rlFolderBusy(true);
  rlScanRequest().then(function(){rlDailySay('rlFolderMsg',t('开始扫描音乐库'));}).catch(function(e){rlDailySay('rlFolderMsg',e.message);}).finally(function(){rlFolderBusy(false);});}
function rlAddFolder(){
  var field=rlDailyNode('rlFolderPath'),path=field.value.trim();if(rlSettingsState.libraryBusy)return;
  if(!path){rlDailySay('rlFolderMsg',t('请填写目录'));field.focus();return;}
  rlFolderBusy(true);
  apiAct('/api/lib/roots','add='+encodeURIComponent(path)).then(function(j){
    if(!j||!j.ok)throw Error((j&&j.error)||t('添加失败'));
    field.value='';loadLibStats();
    return rlScanRequest().then(function(){rlDailySay('rlFolderMsg',t('已添加，正在扫描'));},
      function(e){rlDailySay('rlFolderMsg',t('目录已保存。')+' '+e.message);});
  }).catch(function(e){rlDailySay('rlFolderMsg',e.message);}).finally(function(){rlFolderBusy(false);});
}
function rlReadUpdateState(){
  var s=rlSettingsState;
  apiGetRaw('/api/update/state').then(function(j){
    if(!j||!j.ok)return;
    rlDailySay('rlUpdateVersion',j.current||'—');s.updateBlocked=j.pendingPhase==='rollback_failed';s.staged=!!j.staged;
    if(s.updateBlocked)rlDailySay('rlUpdateMsg',tf('上次回滚未完成，备份已保留：{0}',[j.pendingDetail||'']));
    else if(s.staged)rlDailySay('rlUpdateMsg',tf('已下好 v{0} 的更新包，点「立即更新并重启」生效',[j.stagedVersion||'']));
    rlUpdateButtons();
  }).catch(function(e){rlDailySay('rlUpdateMsg',e.message);});
}
function rlUpdateButtons(){
  var s=rlSettingsState,j=s.update||{}, manual=j.installMode==='manual'||j.install_mode==='manual',blocked=!!s.updateBlocked;
  rlDailyNode('rlUpdateCheck').disabled=s.updateBusy||blocked;
  var download=rlDailyNode('rlUpdateDownload');download.hidden=blocked||s.staged||!j.hasUpdate||manual;download.disabled=s.updateBusy||!j.filesToDownload;
  rlDailyNode('rlUpdateApply').hidden=blocked||!s.staged;rlDailyNode('rlUpdateApply').disabled=s.updateBusy;
  var fallback=manual||String(j.error||'').indexOf('quota_exceeded')>=0;
  rlDailyNode('rlUpdateManual').hidden=!fallback||!(j.global||j.globalUrl);rlDailyNode('rlUpdateBaidu').hidden=!fallback||!(j.cn||j.cnUrl);
}
// 2026-11-15（用户报「Check failed: 拒绝版本回滚」）：后端 check 失败原因原本是**中文整句**，
//   外语界面里 tf('检查失败：{0}') 只能译掉前半句，后半句原样是中文。
//   后端 /api/update/check 已经给出 errorCode，这里按 code 取本地化文案；
//   没有对应词条（或旧后端不给 code）时回落后端原文，不吞信息。
var UPD_ERR = {
  rollback: '拒绝版本回滚',
  network: '网络不可用',
  auth: '更新服务器鉴权失败',
  quota_exceeded: '服务器本月增量配额已用完，请到百度网盘或 GitHub 下载完整版，或下月重试。',
  expired: '更新清单已过期',
  'signature-error': '更新清单签名校验失败',
  'payload-size': '更新清单过大',
  'payload-json': '更新 payload 不是严格 JSON 对象',
  'payload-field': '更新清单字段非法',
  schema: '更新 schema 必须为 2',
  'metadata-version': '不支持的更新 metadata_version',
  platform: '更新平台不是 windows',
  channel: '更新 channel 不匹配',
  version: '本地或目标版本号非法',
  timestamp: 'published_at / expires_at 不是合法 UTC 时间',
  'install-mode': 'install_mode 非法',
  manual: '手动外链、提取码、摘要或大小非法',
  files: '更新清单的文件条目非法',
  delete: '更新清单的删除条目非法',
  'empty-plan': '增量清单没有文件或删除项',
  'bad-url': '更新地址不合法',
  'parse-error': '更新清单解析失败'
};
function updWhy(j) {
  var code = String((j && j.errorCode) || '');
  if (code === 'http') return tf('更新服务器返回 HTTP {0}', [String((j && j.httpStatus) || '?')]);
  // 单测的 vm 沙箱只塞被测函数（没有 UPD_ERR 这个 var）⟹ 必须容错，否则取不到原因就抛。
  var table = (typeof UPD_ERR !== 'undefined' && UPD_ERR) ? UPD_ERR : null;
  if (table && table[code]) return t(table[code]);
  return String((j && (j.error || j.checkError)) || t('未知原因'));
}
function rlUpdateError(e){rlDailySay('rlUpdateMsg',e&&e.message||t('网络错误'));rlSettingsState.updateBusy=false;rlUpdateButtons();}
function rlCheckUpdate(){
  var s=rlSettingsState;if(s.updateBusy||s.updateBlocked)return;
  s.updateBusy=true;s.update=null;rlDailySay('rlUpdateNotes','');rlUpdateButtons();rlDailySay('rlUpdateMsg',t('正在检查…（最长 8 秒）'));
  function check(force){apiGetRaw('/api/update/check'+(force?'?force=1':'')).then(function(j){
    if(j&&j.checking){setTimeout(function(){check(false);},700);return;}
    s.update=j||{};s.updateBusy=false;rlUpdateButtons();
    if(!j||!j.ok)throw Error(tf('检查失败：{0}',[updWhy(j)]));
    rlDailySay('rlUpdateMsg',j.hasUpdate?tf('发现新版本 v{0}',[j.latest]):tf('已是最新版本 v{0}',[j.current||j.latest||'—']));
    rlDailySay('rlUpdateNotes',j.notes||'');
  }).catch(rlUpdateError);}check(true);
}
function rlManualUpdate(target){var j=rlSettingsState.update||{},url=target==='baidu'?(j.cn||j.cnUrl):(j.global||j.globalUrl);
  if(!url||!extUrlAllowed(url))return;apiAct('/api/update/manual',JSON.stringify({target:target})).catch(function(){});openExternal(url);}
function rlDownloadUpdate(){
  var s=rlSettingsState,btn=rlDailyNode('rlUpdateDownload');if(s.updateBusy||s.updateBlocked||btn.hidden||btn.disabled)return;
  s.updateBusy=true;rlUpdateButtons();rlDailySay('rlUpdateMsg',t('正在下载…'));
  function poll(){apiGetRaw('/api/update/progress').then(function(p){
    if(!p||p.phase==='error'||p.error)throw Error((p&&p.error)||t('下载失败'));
    if(p.phase==='ready'||p.canApply){s.updateBusy=false;s.staged=true;rlUpdateButtons();rlDailySay('rlUpdateMsg',t('下载已完成。'));return;}
    rlDailySay('rlUpdateMsg',t('正在下载…')+' '+(p.pct||0)+'%');setTimeout(poll,700);
  }).catch(rlUpdateError);}
  apiAct('/api/update/download','').then(function(j){if(!j||!j.ok)throw Error((j&&j.error)||t('下载失败'));poll();}).catch(rlUpdateError);
}
function rlInstallUpdate(){
  var s=rlSettingsState;if(s.updateBusy||s.updateBlocked||!s.staged)return;
  if(!(s.updateArm>Date.now())){s.updateArm=Date.now()+6000;rlDailySay('rlUpdateMsg',t('再点一次：更新并退出'));return;}
  s.updateArm=0;s.updateBusy=true;rlUpdateButtons();
  apiAct('/api/update/apply','').then(function(j){if(!j||!j.ok)throw Error((j&&j.error)||t('下载失败'));
    rlDailySay('rlUpdateMsg',t('程序即将退出，重新打开即完成更新'));}).catch(rlUpdateError);
}


function paintQuickStart(caps) {
  rlInstallHome();rlSyncHomeNavigation();rlPaintHome();rlPaintQueue();rlPaintShelf();
}
function previewQuickStart(profile) {
  var btn = document.querySelector('[data-qs-profile="' + profile + '"]');
  if (!btn || btn.disabled || !D('qsPreview')) return;
  quickStartSelection = profile;
  Array.prototype.forEach.call(document.querySelectorAll('[data-qs-profile]'), function (b) {
    b.setAttribute('aria-pressed', b.dataset.qsProfile === profile ? 'true' : 'false');
  });
  var box = D('qsPreview'); box.hidden = false;
  var desc = profile === 'bitperfect' ? '将设置音量 0 dB、源采样率，并关闭 EQ、IR、等响度和其他处理。'
    : '将按设备能力配置滤波与输出，保留 EQ 和校准；预设参数会替换当前升频设置。';
  box.innerHTML = '<div><strong>' + esc(t('应用前确认')) + '</strong><p>' + esc(t(desc)) + '</p><small>' + esc(t(S.playing ? '请先停止播放，再应用预设。' : '不会启动播放，也不会改变音乐库或队列。'))
    + '</small></div><button class="btn pri" id=qsApply' + (S.playing ? ' disabled' : '') + '>' + esc(t('应用预设')) + '</button>';
  D('qsApply').onclick = function () {
    if (S.playing) return;
    this.disabled = true;
    applyPreset(profile,{preserveProcessing:true}).then(function (j) { if (j && j.ok) quickStartSelection = ''; if (S.view === 'station') paintQuickStart(_capsCache); });
  };
}
function renderQuickStart(force) {
  var generation = ++quickStartGeneration;
  paintQuickStart(_capsCache);
  loadPresetCaps(!!force).then(function (j) {
    if (S.view === 'station' && generation === quickStartGeneration) paintQuickStart(j || _capsCache);
  });
}
// 2026-11-12（3.3.8）：整页切换原子化。
// 根因不是右栏显示太早，而是左页的封面/列表还在异步到达：旧实现只藏右栏，右栏位置先露出
// 主题背景（浅色/蓝色主题下就是黑块），左页封面随后才逐张出现。现在切页时主区+右栏一起藏，
// 主题色遮罩盖住内容区；等左页 DOM 有内容、首屏图片解码完成（最多 320ms）后同一帧揭示两侧。
var RL_PAGE_TRANSITION={on:false,timer:0,slowTimer:0,frame:0,observer:null,started:0,target:'',endToken:0,oldText:'',contentChanged:false};
function rlPageTransitionNow(){return (typeof performance!=='undefined'&&performance.now)?performance.now():Date.now();}
function rlPageTransitionTrace(tag){
  try{
    var main=document.querySelector('.main'),right=D('right'),veil=D('pageVeil'),list=D('list');
    var trace=window.__rlTransitionTrace||(window.__rlTransitionTrace=[]);
    trace.push({t:Math.round(rlPageTransitionNow()),tag:tag,target:RL_PAGE_TRANSITION.target,
      main:main?getComputedStyle(main).visibility:'',right:right?getComputedStyle(right).visibility:'',
      veil:veil?getComputedStyle(veil).display:'',children:list?list.children.length:0,
      text:list?String(list.textContent||'').trim().length:0,
      imgs:list?list.querySelectorAll('img').length:0,
      pending:list?Array.prototype.filter.call(list.querySelectorAll('img'),function(i){return !i.complete;}).length:0,
      visiblePending:list?Array.prototype.filter.call(list.querySelectorAll('img'),function(i){if(i.complete)return false;var r=i.getBoundingClientRect();return r.width>0&&r.height>0&&r.bottom>0&&r.top<(window.innerHeight||800)&&r.right>0&&r.left<(window.innerWidth||1200);}).length:0});
    if(trace.length>120)trace.splice(0,trace.length-120);
  }catch(e){}
}
function rlPageTransitionBegin(target){
  var app=D('app');if(!app)return;
  target=target||S.view;
  if(target==='listening'||target==='station'||target==='settings'||target==='queue'){rlPageTransitionEnd(true);return;}
  RL_PAGE_TRANSITION.on=true;RL_PAGE_TRANSITION.target=target;
  RL_PAGE_TRANSITION.started=rlPageTransitionNow();
  var oldList=D('list');RL_PAGE_TRANSITION.oldText=oldList?String(oldList.textContent||'').trim():'';RL_PAGE_TRANSITION.contentChanged=false;
  app.classList.remove('rlSwitchSlow');
  // 只有在 90ms 内还没画完才铺遮罩：常见页（专辑缓存命中）根本不会闪，慢页才用同色遮罩过渡。
  clearTimeout(RL_PAGE_TRANSITION.slowTimer);
  RL_PAGE_TRANSITION.slowTimer=setTimeout(function(){if(RL_PAGE_TRANSITION.on)app.classList.add('rlSwitchSlow');},90);
  app.classList.add('rlPageSwitching');
  window.__rlTransitionTrace=[];
  rlPageTransitionTrace('begin');
  clearTimeout(RL_PAGE_TRANSITION.timer);
  RL_PAGE_TRANSITION.timer=setTimeout(function(){rlPageTransitionEnd(true);},6000);
  rlPageTransitionWatch();
}
function rlPageTransitionEnd(force){
  if(!RL_PAGE_TRANSITION.on)return;
  if(!force&&rlPageTransitionNow()-RL_PAGE_TRANSITION.started<70){
    clearTimeout(RL_PAGE_TRANSITION.timer);
    RL_PAGE_TRANSITION.timer=setTimeout(function(){rlPageTransitionTryEnd();},35);
    return;
  }
  RL_PAGE_TRANSITION.on=false;
  clearTimeout(RL_PAGE_TRANSITION.timer);RL_PAGE_TRANSITION.timer=0;
  if(RL_PAGE_TRANSITION.frame&&typeof cancelAnimationFrame==='function')cancelAnimationFrame(RL_PAGE_TRANSITION.frame);
  RL_PAGE_TRANSITION.frame=0;
  if(RL_PAGE_TRANSITION.observer){try{RL_PAGE_TRANSITION.observer.disconnect();}catch(e){}RL_PAGE_TRANSITION.observer=null;}
  clearTimeout(RL_PAGE_TRANSITION.slowTimer);RL_PAGE_TRANSITION.slowTimer=0;
  var app=D('app');if(app)app.classList.remove('rlPageSwitching','rlSwitchSlow');
  rlPageTransitionTrace('end');
}
function rlPageTransitionWatch(){
  var list=D('list');if(!list)return;
  if(RL_PAGE_TRANSITION.observer){try{RL_PAGE_TRANSITION.observer.disconnect();}catch(e){}RL_PAGE_TRANSITION.observer=null;}
  if(typeof MutationObserver!=='function'||typeof requestAnimationFrame!=='function')return;
  var observer=new MutationObserver(function(){rlPageTransitionTryEnd();});
  observer.observe(list,{childList:true,subtree:true,characterData:true});
  RL_PAGE_TRANSITION.observer=observer;
}
function rlPageTransitionTryEnd(){
  if(!RL_PAGE_TRANSITION.on)return;
  if(RL_PAGE_TRANSITION.frame&&typeof cancelAnimationFrame==='function')cancelAnimationFrame(RL_PAGE_TRANSITION.frame);
  var token=++RL_PAGE_TRANSITION.endToken;
  // 双 rAF：先让本次 innerHTML 完成布局，再检查内容/封面；两侧始终在同一帧揭示。
  RL_PAGE_TRANSITION.frame=requestAnimationFrame(function(){
    RL_PAGE_TRANSITION.frame=requestAnimationFrame(function(){
      if(!RL_PAGE_TRANSITION.on||token!==RL_PAGE_TRANSITION.endToken)return;
      rlPageTransitionTrace('probe');
      var list=D('list');if(!list)return rlPageTransitionEnd(true);
      if(!list.children.length||!String(list.textContent||'').trim())return; // 还在 loading 空态，等下一次 DOM 更新
      var nowText=String(list.textContent||'').trim();
      if(!RL_PAGE_TRANSITION.contentChanged&&nowText===RL_PAGE_TRANSITION.oldText&&rlPageTransitionNow()-RL_PAGE_TRANSITION.started<700)return; // 还是旧页，等新页替换
      RL_PAGE_TRANSITION.contentChanged=true;
      // 只等首屏可见封面：178 张全部 decode 会把切页拖到数秒；首屏就绪即可整页揭示，
      // 首屏以下继续由浏览器懒加载，不会出现「右边先出、左边封面最后才一张张冒出来」。
      var vh=window.innerHeight||800,vw=window.innerWidth||1200;
      var imgs=Array.prototype.slice.call(list.querySelectorAll('img')).filter(function(img){
        if(img.complete)return false;var r=img.getBoundingClientRect();
        return r.width>0&&r.height>0&&r.bottom>0&&r.top<vh&&r.right>0&&r.left<vw;
      }).slice(0,24);
      if(!imgs.length)return rlPageTransitionEnd();
      var done=false,finish=function(){if(done)return;done=true;rlPageTransitionEnd();};
      var timer=setTimeout(finish,500);
      Promise.all(imgs.map(function(img){
        if(img.decode)return img.decode().catch(function(){});
        return new Promise(function(resolve){
          if(img.complete)return resolve();
          img.addEventListener('load',resolve,{once:true});img.addEventListener('error',resolve,{once:true});
        });
      })).then(function(){clearTimeout(timer);finish();},function(){clearTimeout(timer);finish();});
    });
  });
}
function setView(v, sameViewRefresh) {
  if(v==='station')v='listening';
  if(typeof rlPageTransitionBegin==='function')rlPageTransitionBegin(v);
  if(v==='settings'&&rlMode!=='pro')rlSetMode('pro',true);
  if (v !== 'settings') ++CLEANUP_UI.generation;
  if (!sameViewRefresh || v !== S.view) QUICK_START_PREF.navigated = true;
  var blockedQuickStart = false;
  if (blockedQuickStart) v = 'albums';
  // 261024-05 #T2：离开专辑页 → 立即停掉整张专辑的曲目预取队列，切页面后不再发旧 key 的请求
  if (v !== 'album' && (v !== 'albums' || S.view !== 'albums')) albPrefStop();
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
    rlSetNavCurrent(e, on);
  });
  var isSet = (v === 'settings');
  // 设置页占满整屏：右栏（队列/歌词）在这里没有意义，直接藏掉。
  // 走 #app.noq 这套已有的网格规则，否则网格会留一个空列。
  // 2026-10-23（UI 评审 #8）：播放队列页自己就是队列，右侧抽屉再显示一份同样的队列是重复信息
  D('app').classList.toggle('noq', isSet || S.queueHidden || v === 'queue' || v === 'station' || v === 'listening');
  D('app').classList.toggle('setmode', isSet);   // 设置页隐藏 crumb，页签自己吸顶
  applyRTab(isSet);
  if (D('setpage')) D('setpage').classList.toggle('on', isSet);
  // 2026-10-23 用户反馈「切到作品栏后无法切回其它栏位」：干净环境下三种路径都复现不出来，
  // 但这里确实存在一类残留风险 —— 只要有任何路径把 #list 置为 display:none（设置页就会），
  // 离开设置页时若不显式恢复，视图就会「看起来卡住/空白」。此处按目标视图**强制**设定，
  // 不再依赖「上一个视图是什么」。
  if (D('list')) D('list').style.display = isSet ? 'none' : '';
  if (!isSet && D('scanbar')) D('scanbar').style.display = S.scanning ? '' : 'none';
  if (isSet) {
    rlSettingsRoute(!sameViewRefresh);
  }
  else if (v === 'station') { renderQuickStart(); if (D('scanbar')) D('scanbar').style.display = 'none'; }
  else if (v === 'listening') {
    rlInstallHome();
    if (D('rlQuickSetup')) D('rlQuickSetup').remove();
    rlSyncHomeNavigation();
    rlPaintHome(); rlPaintQueue(); rlPaintShelf();
    if(PLAYBACK_SETTINGS.ready)rlConsole.refresh().catch(function(){});
    if (D('scanbar')) D('scanbar').style.display = 'none';
  }
  else if (D('scanbar')) D('scanbar').style.display = S.scanning ? '' : 'none';
  if (blockedQuickStart) loadAlbums('');
}

Array.prototype.forEach.call(document.querySelectorAll('.nav'), function (e) {
  if (e.dataset.d) return;
  e.onclick = function () {
    var v = e.dataset.v;
    if (!v) return;
    setView(v);
    if (v === 'station' && D('rlQuickSetup')) {
      var setup = D('rlQuickSetup');
      setup.open = true;
      rlSyncHomeNavigation();
      setup.querySelector('summary').focus({preventScroll:true});
      setup.scrollIntoView({block:'start'});
    } else if (v === 'listening' && D('rlListenHome')) {
      D('rlListenHome').scrollIntoView({block:'start'});
    }
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
function skipTrack(dir) {
  if (!ensurePlaybackSettings()) return;
  // Before this session's first play, backend skip parameters may still be defaults.
  if (!S.playing) {
    var q = S.q || {}, items = q.items || [];
    if (!items.length) { toast('队列是空的，先拖一首歌进来'); return; }
    var i = (((q.index || 0) + dir) % items.length + items.length) % items.length;
    var cue = (q.cues || [])[i] || [-1, -1];
    playPath(items[i], cue[0], cue[1], i); return;
  }
  api('/api/skip?dir=' + dir, '').then(loadQ);
}
D('bNext').onclick = function () { skipTrack(1); };
D('bPrev').onclick = function () { skipTrack(-1); };
function playOrPause() {
  if (!ensurePlaybackSettings()) return;
  if (!S.playing) {
    // 停止之后点播放：必须重新起播并把最新参数（采样率/滤波器/位深…）一起下发，
    // 只发 /api/pause 是不会有任何反应的（本轮用户报的就是这个）。
    var q = S.q || {}, i = q.index || 0;
    if (q.items && q.items[i]) {
      var cue = (q.cues || [])[i] || [-1, -1];
      playPath(q.items[i], cue[0], cue[1], i); return;
    }
    if (S.lastCur) { playPath(S.lastCur); return; }
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
D('npPrev').onclick = function () { skipTrack(-1); };
D('npNext').onclick = function () { skipTrack(1); };
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
  var i1 = D('repIco'); if (i1) i1.innerHTML = repIconHtml(v, 17);
  var i2 = D('npIco');  if (i2) i2.innerHTML = repIconHtml(v, 18);
  ['bRep', 'npRep'].forEach(function (id) {
    var e = D(id); if (!e) return;
    e.style.color = (v === 0) ? '' : 'var(--acc-text)';
    e.dataset.mode = String(v);                       // 自检/脚本可读的机器可判据
    e.title = tf('播放模式：{0}（点击切换）', [t(PM_LABEL[v])]);
    e.setAttribute('aria-label', tf('播放模式：{0}', [t(PM_LABEL[v])]));
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
  toast(tf('播放模式：{0}', [t(PM_LABEL[next])]));
  return next;
}
// ---- 音量：整页滑块是 #vol 的镜像，两边都只改 S.vol + 下发 /api/volume ----
// setVol() 是唯一入口（键盘 ↑↓ 也用它）；syncVolUi() 把状态刷到所有镜像控件。
// ---- 2026-11-12：音量旋钮（0 dB 在正上方，负值向左、正值向右，对数映射让 0 dB 附近刻度更密）----
var VOL_MIN=-120,VOL_MAX=20,VOL_STEP=0.5;
function volClamp(db){db=Number(db);if(!Number.isFinite(db))return 0;return Math.round(Math.max(VOL_MIN,Math.min(VOL_MAX,db))*10)/10;}
// 以 0 dB 为圆心：0→0.5；负值用 log1p(-db/8)/log1p(120/8) 铺满左半弧；正值同理铺满右半弧。
function volDbToNorm(db){
  db=Math.max(VOL_MIN,Math.min(VOL_MAX,Number(db)||0));
  if(db>=0)return 0.5+0.5*Math.log1p(db/8)/Math.log1p(VOL_MAX/8);
  return 0.5-0.5*Math.log1p(-db/8)/Math.log1p(-VOL_MIN/8);
}
function volNormToDb(norm){
  norm=Math.max(0,Math.min(1,Number(norm)||0));
  if(norm>=0.5)return volClamp(8*(Math.exp(((norm-0.5)/0.5)*Math.log1p(VOL_MAX/8))-1));
  return volClamp(-8*(Math.exp(((0.5-norm)/0.5)*Math.log1p(-VOL_MIN/8))-1));
}
function rlVolPoint(norm,r){var a=(-135+270*Math.max(0,Math.min(1,norm))-90)*Math.PI/180;return [32+r*Math.cos(a),32+r*Math.sin(a)];}
function rlVolArcPath(norm){
  if(norm<=0.0001)return '';
  // 2026-11-14 真机（3.3.10 用户反馈「音量弧线仍然偏移」）：
  // 弧从 −225° 顺时针扫 270×norm 度，而 large-arc-flag 只在**扫角 > 180°** 时才为 1。
  // 旧代码写 norm>0.5（= 扫角 135°）就置 1 ⟹ 0 dB 附近（norm≈0.53）被画成另一条 216° 的大弧，
  // 绕到左下角，看起来就是「弧线跑到圈外、和刻度不同心」。实测包围盒：轨道 48×41 居中，
  // 弧 41×47.7 且中心左偏 37px。正确阈值是 180/270 = 2/3。
  var a=rlVolPoint(0,24),b=rlVolPoint(norm,24),large=(270*norm)>180?1:0;
  return 'M'+a[0].toFixed(2)+' '+a[1].toFixed(2)+' A24 24 0 '+large+' 1 '+b[0].toFixed(2)+' '+b[1].toFixed(2);
}
var RL_VOL_TICKS=[-120,-60,-30,-18,-12,-9,-6,-3,0,3,6,12,20];
function rlPaintVolumeKnob(prefix,db,locked){
  var knob=D(prefix+'Knob');if(!knob)return;
  db=volClamp(db);locked=!!locked;
  var norm=volDbToNorm(db),angle=(-135+270*norm-90)*Math.PI/180;
  var arc=D(prefix+'KnobArc');if(arc)arc.setAttribute('d',rlVolArcPath(norm));
  var needle=D(prefix+'KnobNeedle');
  if(needle){needle.setAttribute('x2',(32+19*Math.cos(angle)).toFixed(2));needle.setAttribute('y2',(32+19*Math.sin(angle)).toFixed(2));}
  var val=D(prefix+'KnobVal');if(val)val.textContent=db.toFixed(1);
  knob.setAttribute('aria-valuenow',db.toFixed(1));
  knob.setAttribute('aria-valuetext',db.toFixed(1)+' dB');
  knob.setAttribute('aria-disabled',String(locked));
  knob.dataset.locked=String(locked);
  knob.tabIndex=locked?-1:0;
  var ticks=D(prefix+'KnobTicks');
  if(ticks&&!ticks.rlBuilt){
    var h='';
    RL_VOL_TICKS.forEach(function(v){var n=volDbToNorm(v),major=(v===-120||v===-60||v===-12||v===0||v===6||v===12||v===20);var p1=rlVolPoint(n,major?29.2:28.4),p2=rlVolPoint(n,major?25.8:26.6);h+='<line class="rlVolTick'+(major?' major':'')+'" x1="'+p1[0].toFixed(2)+'" y1="'+p1[1].toFixed(2)+'" x2="'+p2[0].toFixed(2)+'" y2="'+p2[1].toFixed(2)+'"/>';});
    ticks.innerHTML=h;ticks.rlBuilt=true;
  }
}
function rlBindVolumeKnob(prefix){
  var knob=D(prefix+'Knob');if(!knob||knob.rlKnobBound)return;knob.rlKnobBound=true;
  var dragging=false,pendingEvent=null,rafId=0;
  function paint(ev){
    if(isRawVolumeLocked()||!PLAYBACK_SETTINGS.ready)return;
    var r=knob.getBoundingClientRect();if(!r.width)return;
    var deg=Math.atan2(ev.clientX-(r.left+r.width/2),(r.top+r.height/2)-ev.clientY)*180/Math.PI;
    var norm=(Math.max(-135,Math.min(135,deg))+135)/270;
    setVol(volNormToDb(norm),true);
  }
  // 3.3.9：拖动按帧合并，一帧最多画一次 —— 旧实现每个 pointermove 都重画 SVG，
  // 高频事件下弧线与数字会落在不同帧，看起来就是“错位”。
  function schedule(ev){
    pendingEvent=ev;
    if(rafId)return;
    rafId=requestAnimationFrame(function(){rafId=0;var e=pendingEvent;pendingEvent=null;if(dragging&&e)paint(e);});
  }
  function stopDrag(){
    if(!dragging)return;dragging=false;
    if(rafId&&typeof cancelAnimationFrame==='function')cancelAnimationFrame(rafId);
    rafId=0;pendingEvent=null;commitVolumeGesture();
  }
  // 只有真实按下左键才进入拖动；鼠标只是移过（未按下）绝不改音量。
  knob.addEventListener('pointerdown',function(ev){
    if(knob.dataset.locked==='true')return;
    if(ev.button!==0&&ev.pointerType==='mouse')return;
    ev.preventDefault();dragging=true;paint(ev);
    try{knob.setPointerCapture(ev.pointerId);}catch(e){}
  });
  knob.addEventListener('pointermove',function(ev){
    if(!dragging)return;
    if(ev.pointerType==='mouse'&&ev.buttons===0){stopDrag();return;}
    schedule(ev);
  });
  knob.addEventListener('pointerup',stopDrag);
  knob.addEventListener('pointercancel',stopDrag);
  knob.addEventListener('lostpointercapture',stopDrag);
  window.addEventListener('pointerup',stopDrag);
  window.addEventListener('blur',stopDrag);
  knob.addEventListener('wheel',function(ev){if(knob.dataset.locked==='true')return;ev.preventDefault();var step=ev.shiftKey?0.1:VOL_STEP;setVol(volClamp((Number(S.vol)||0)+(ev.deltaY>0?-step:step)),true);commitVolumeGesture();},{passive:false});
  knob.addEventListener('keydown',function(ev){
    if(knob.dataset.locked==='true')return;
    var step=ev.shiftKey?0.1:VOL_STEP,cur=Number(S.vol)||0,next=null;
    if(ev.key==='ArrowUp'||ev.key==='ArrowRight')next=cur+step;
    else if(ev.key==='ArrowDown'||ev.key==='ArrowLeft')next=cur-step;
    else if(ev.key==='PageUp')next=cur+6;
    else if(ev.key==='PageDown')next=cur-6;
    else if(ev.key==='Home')next=VOL_MIN;
    else if(ev.key==='End')next=VOL_MAX;
    if(next===null)return;ev.preventDefault();ev.stopPropagation();setVol(volClamp(next),true);commitVolumeGesture();
  });
  knob.addEventListener('dblclick',function(){if(knob.dataset.locked==='true')return;setVol(0,true);commitVolumeGesture();});
  var mute=D(prefix+'Floor');
  if(mute){['pointerdown','click','dblclick'].forEach(function(type){mute.addEventListener(type,function(ev){ev.stopPropagation();});});}
}
['vol','npVol'].forEach(rlBindVolumeKnob);
var volumeRestore = null, volumeGestureUntil = 0;
function confirmedVolumeForSave() {
  if(VOLUME_WRITE.current&&VOLUME_WRITE.current.pending)return null;
  var value=isRawVolumeLocked()?0:VOLUME_WRITE.confirmed;
  return typeof value==='number'&&Number.isFinite(value)&&value>=-120&&value<=20?value:null;
}
function persistVolumeGesture(write) {
  if(!write||VOLUME_WRITE.current!==write||write.sequence!==VOLUME_WRITE.sequence||!write.accepted||!write.commitRequested)return Promise.resolve({ok:false,stale:true});
  if(write.savePromise)return write.savePromise;
  write.savePromise=persistSettings('volume='+write.value,write).then(function(j){if(j&&j.ok&&!j.skipped)write.persisted=true;return j;});
  return write.savePromise;
}
function commitVolumeGesture() {
  var write=VOLUME_WRITE.current;
  if(!write||write.sequence!==VOLUME_WRITE.sequence||isRawVolumeLocked())return;
  write.awaitChange=false;write.commitRequested=true;
  if(write.accepted)return persistVolumeGesture(write);
  if(write.promise)return write.promise.then(function(){return persistVolumeGesture(write);});
}
function isRawVolumeLocked() {return S.rawVolumeLocked===true;}
function rawVolumeMessage() {return t('原始 DSD 音量由设备控制。请使用设备音量，或切回 PCM / 可处理的 DoP 路径。');}
function syncVolumeStatus(j,authoritative,request) {
  // Only backend session facts can lock volume. A Native/Direct preset alone
  // does not prove that raw transport is actually active.
  if(!authoritative&&typeof request==='number'&&request<=VOLUME_WRITE.statusBarrier)return;
  var wasLocked=isRawVolumeLocked();
  S.rawVolumeLocked=j.rawVolumeLocked===true||j.volumeWritable===false;
  if(typeof j.volDb==='number'&&Number.isFinite(j.volDb))S.volDb=j.volDb;
  if(isRawVolumeLocked()) {
    if(VOLUME_WRITE.current){++VOLUME_WRITE.sequence;VOLUME_WRITE.current=null;}
    VOLUME_WRITE.confirmed=0;S.vol=S.volDb=0;volumeRestore=null;volumeGestureUntil=0;
  }
  else if((authoritative||wasLocked)&&Number.isFinite(S.volDb)){S.vol=S.volDb;volumeGestureUntil=0;}
  if(!VOLUME_WRITE.current||!VOLUME_WRITE.current.pending)VOLUME_WRITE.confirmed=S.volDb;
  if(PLAYBACK_SETTINGS.ready)syncVolUi(S.playing||authoritative?S.volDb:S.vol);
}
function syncVolUi(live) {
  var ready=PLAYBACK_SETTINGS.ready,locked=isRawVolumeLocked();
  if(locked){S.vol=0;volumeRestore=null;volumeGestureUntil=0;}
  updatePlaybackSettingsGate();
  if(!ready){if(D('npVolLab'))D('npVolLab').textContent=t('音量尚未读取');return;}
  var pending=VOLUME_WRITE.current&&VOLUME_WRITE.current.pending;
  var db=locked?0:!pending&&typeof live==='number'&&Number.isFinite(live)&&Date.now()>=volumeGestureUntil?live:S.vol;
  if(!Number.isFinite(db))return;
  var label=db.toFixed(1)+' dB';
  ['vol','npVol'].forEach(function(id){var e=D(id);if(!e)return;if(Math.abs(Number(e.value)-db)>1e-9)e.value=db;e.setAttribute('aria-valuetext',label);e.title=locked?rawVolumeMessage():t('音量（dB）；正增益可能削波');});
  ['barVolInput','npVolInput'].forEach(function(id){var e=D(id);if(!e)return;if(locked||document.activeElement!==e){e.value=db.toFixed(1);e.removeAttribute('aria-invalid');}e.title=locked?rawVolumeMessage():t('输入 -120 至 +20 dB，Enter 确认，Esc 取消。');});
  rlPaintVolumeKnob('vol',db,locked);rlPaintVolumeKnob('npVol',db,locked);
  ['barVolDb','npVolLab'].forEach(function(id){var lab=D(id);if(lab){lab.hidden=false;lab.textContent=label;}});
  ['npRawVolumeNotice','rlStudioRawVolumeNotice'].forEach(function(id){var notice=D(id);if(!notice)return;notice.hidden=!locked;notice.textContent=id==='npRawVolumeNotice'?t('设备调音'):rawVolumeMessage();notice.title=rawVolumeMessage();});
  ['volFloor','npVolFloor'].forEach(function(id){var floor=D(id);if(!floor)return;var muted=db<=-120;floor.disabled=locked||!ready||muted&&volumeRestore===null;floor.setAttribute('aria-pressed',String(!locked&&muted));floor.title=locked?rawVolumeMessage():t(muted&&volumeRestore!==null?'恢复先前音量':'静音')+(muted&&volumeRestore!==null?' ('+volumeRestore.toFixed(1)+' dB)':' (−120 dB)');floor.setAttribute('aria-label',floor.title);});
  document.querySelectorAll('[data-volume-step]').forEach(function(button){button.disabled=!ready||locked||(Number(button.dataset.volumeStep)>0?db>=VOL_MAX:db<=VOL_MIN);button.title=locked?rawVolumeMessage():t('每次 0.5 dB；按住 Shift 每次 0.1 dB。');});
}
function toggleVolumeMute(){
  if(!ensurePlaybackSettings())return;
  if(isRawVolumeLocked()){syncVolUi();return;}
  var db=Number(D('vol').value);
  if(db<=-120){if(volumeRestore===null)return;var restored=volumeRestore;setVol(restored);}
  else{volumeRestore=db;setVol(-120);}
  commitVolumeGesture();
}
['volFloor','npVolFloor'].forEach(function(id){if(D(id))D(id).onclick=toggleVolumeMute;});
function commitPreciseVolume(field){
  if(!ensurePlaybackSettings()||isRawVolumeLocked()){syncVolUi();return;}
  var text=field.value.trim(),value=Number(text);
  if(!text||!Number.isFinite(value)||value<VOL_MIN||value>VOL_MAX){field.setAttribute('aria-invalid','true');toast(t('请输入 -120 至 +20 dB 的音量。'));return;}
  value=Math.round(value*10)/10;field.value=value.toFixed(1);field.removeAttribute('aria-invalid');
  if(value===Number(S.vol)&&(!VOLUME_WRITE.current||VOLUME_WRITE.current.value===value)){commitVolumeGesture();return;}
  setVol(value,true);commitVolumeGesture();
}
['barVolInput','npVolInput'].forEach(function(id){var field=D(id);if(!field)return;
  field.addEventListener('change',function(){commitPreciseVolume(field);});
  field.addEventListener('keydown',function(event){
    if(event.key==='Enter'){event.preventDefault();event.stopPropagation();commitPreciseVolume(field);field.blur();syncVolUi();}
    else if(event.key==='Escape'){event.preventDefault();event.stopPropagation();field.value=Number(S.vol).toFixed(1);field.removeAttribute('aria-invalid');field.blur();syncVolUi();}
  });
});
document.querySelectorAll('[data-volume-step]').forEach(function(button){button.onclick=function(event){if(!ensurePlaybackSettings()||isRawVolumeLocked())return;var delta=Number(button.dataset.volumeStep);if(event.shiftKey)delta=delta>0?.1:-.1;setVol(Math.round((Number(S.vol)+delta)*10)/10,true);commitVolumeGesture();};});
if (D('npRep')) D('npRep').onclick = cycleRep;
if (D('npVol')) {
  D('npVol').oninput = function () {
    setVol(Number(this.value),true);
  };
  D('npVol').addEventListener('change',commitVolumeGesture);
}
D('tq').onclick = function () {
  S.queueHidden = !S.queueHidden;
  applyRTab(S.view === 'settings');
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
  setVol(Number(this.value),true);
};
D('sLoud').onchange = function () { S.loud = this.checked; saveSettings(); };
// 参数类控件改动后自动落盘（下次启动还在）
['sRate','sRateFamilyPolicy','sFilt','sFiltNx','sShap','sDSD','sIsp','s2s','sBuf'].forEach(function (id) {
  var e = D(id); if (!e) return;
  e.addEventListener('change', saveSettings);
});
D('vol').addEventListener('change',commitVolumeGesture);
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
      (j.lan ? (tf('局域网：http://{0}:{1}/（已开启）', [esc(j.ip), j.port]))
             : ('局域网访问：未开启（用 <code>--lan</code> 启动即可以 http://' + esc(j.ip) + ':' + j.port + '/ 访问）'));
  });
};
D('bLyric').onclick = function () { setRTab(RTAB === 'l' ? 'q' : 'l'); };
D('bChain').onclick = showChain;
D('bHelp').onclick = function () {
  D('mtitle').textContent = '快捷键与提示';
  D('modal').classList.add('on');
  var rows = [
    ['空格', '暂停 / 继续'], ['← →', '后退 / 前进 5 秒'], ['↑ ↓', '音量增减 0.5 dB（Shift 0.1）'],
    ['n / p', '下一首 / 上一首'], ['s', '停止'], ['l', '歌词'], ['i', '信号链'],
    // 261027-03 #3：这四条原来和实现对不上（「右键队列条目」在队列页根本没实现，
    // 曲目页右键是"直接加入"不弹菜单）—— 现在两边都已对齐，且补上可见的「＋」入口。
    ['曲目/专辑行「＋」', '加入播放队列（常显，不用 hover）'],
    ['队列页「＋ 添加曲目」', '搜曲库加入队列'],
    ['单曲页「＋ 加入队列」', '把正在播的这首加入队列'],
    ['右键曲目 / 专辑', '菜单：立即播放 / 加入播放队列'],
    ['右键队列条目', '菜单：立即播放 / 从队列移除'],
    ['队列页「排序…」', '标题 / 艺术家 / 专辑 / 时长 / 年份 / 反转 / 随机打乱'],
    ['「↩ 撤销排序」', '回到排序前那一版顺序（有快照时才出现）'],
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
var seekControls = [
  { input:'transportSeek', bar:'bar', pos:'tpos', dur:'tdur' },
  { input:'npTransportSeek', bar:'npBar', pos:'npPos', dur:'npDur' }
];
function paintTransportTime(control, seconds, duration) {
  var node = D(control.input);
  var position = Math.max(0, Math.min(duration, Number(seconds) || 0));
  node.setAttribute('aria-valuetext', fmt(position) + ' / ' + fmt(duration));
  D(control.bar).style.width = (duration ? 100 * position / duration : 0) + '%';
  D(control.pos).textContent = fmt(position);
  D(control.dur).textContent = fmt(duration);
}
var npWaveform=null,npWaveformFile='';
function syncNpWaveform(status){
  var host=D('npWaveform');if(!host||typeof rlMountWaveform!=='function')return;
  if(!D('npPage')||!D('npPage').classList.contains('on')){if(npWaveform){npWaveform.destroy();npWaveform=null;}return;}
  var current=status||rlLastStatus||{},file=status?status.current||'':S.lastCur||'',duration=status?status.dur:S.dur,position=status?status.pos:S.pos;
  var cueStart=Number.isFinite(current.cueStart)&&current.cueStart>=0?current.cueStart:0,cueLength=Number.isFinite(current.cueEnd)&&current.cueEnd>cueStart?current.cueEnd-cueStart:0;
  var trackKey=JSON.stringify([file,current.queueIndex,cueStart,cueLength]);
  var props={file:file,duration:duration,position:position,trackKey:trackKey,clipStart:cueStart,clipLength:cueLength,disabled:!S.playing,onSeek:function(seconds){if(file===(S.lastCur||'')&&npWaveformFile===trackKey&&S.dur>0)doSeek(seconds/S.dur);}};
  if(!npWaveform||host.rlWaveform!==npWaveform)npWaveform=rlMountWaveform(host,props);else npWaveform.update(props);
  npWaveformFile=trackKey;
}
function syncTransportSeek(status) {
  syncNpWaveform(status);
  var duration = Number.isFinite(status.dur) && status.dur > 0 ? status.dur : 0;
  seekControls.forEach(function (control) {
    var node = D(control.input);
    if (!node) return;
    node.disabled = !status.playing || !duration;
    node.max = Math.max(duration, 0.001);
    if (control.gesture && control.file !== status.current) control.cancelled = true;
    control.file = status.current || '';
    if (!control.gesture || control.cancelled) node.value = Math.max(0, Number(status.pos) || 0);
    paintTransportTime(control, Number(node.value), duration);
  });
}
seekControls.forEach(function (control) {
  var node = D(control.input);
  if (!node) return;
  node.title = t('拖动或点击跳转。方向键移动 5 秒，Home / End 跳至两端。');
  node.addEventListener('pointerdown', function () { control.gesture = true; control.cancelled = false; });
  node.addEventListener('input', function () {
    if (!control.cancelled) paintTransportTime(control, Number(node.value), S.dur);
  });
  node.addEventListener('change', function () {
    if (!control.cancelled && !node.disabled && S.dur > 0) doSeek(Number(node.value) / S.dur);
    control.gesture = false; control.cancelled = false;
  });
  node.addEventListener('pointerup', function () { control.gesture = false; });
  node.addEventListener('pointercancel', function () { control.gesture = false; control.cancelled = true; });
  node.addEventListener('keydown', function (event) {
    var delta = {ArrowLeft:-5, ArrowDown:-5, ArrowRight:5, ArrowUp:5};
    if (!(event.key in delta) && event.key !== 'Home' && event.key !== 'End') return;
    event.preventDefault();
    if (node.disabled || !S.dur) return;
    var at = event.key === 'Home' ? 0 : event.key === 'End' ? S.dur : Number(node.value) + delta[event.key];
    node.value = Math.max(0, Math.min(S.dur, at));
    paintTransportTime(control, Number(node.value), S.dur);
    doSeek(Number(node.value) / S.dur);
  });
});
function doSeek(frac) {
  if (!S.playing || !(S.dur > 0) || !Number.isFinite(frac)) return;
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
function setVol(v,awaitChange) {
  if (!ensurePlaybackSettings()) return;
  if(isRawVolumeLocked()){syncVolUi();return Promise.resolve({ok:false,rawVolumeLocked:true});}
  if (!Number.isFinite(v)) return;
  v = Math.round(Math.max(VOL_MIN, Math.min(VOL_MAX, v))*10)/10;
  var write={sequence:++VOLUME_WRITE.sequence,value:v,pending:true,accepted:false,awaitChange:awaitChange===true,commitRequested:false,persisted:false};
  VOLUME_WRITE.current=write;
  if (v > VOL_MIN) volumeRestore = null;
  volumeGestureUntil = Date.now() + 900;
  S.vol = v;
  D('vol').value = v;
  if (typeof syncVolUi === 'function') syncVolUi();   // 整页滑块跟随（同一个 S.vol）
  var action=VOLUME_ACTION_QUEUE.then(function(){
    if(VOLUME_WRITE.current!==write||write.sequence!==VOLUME_WRITE.sequence||isRawVolumeLocked()){write.pending=false;return {ok:false,stale:true};}
    return api('/api/volume?db=' + v.toFixed(1), '');
  });
  VOLUME_ACTION_QUEUE=action.then(function(){},function(){});
  write.promise=action.then(function(j){
    if(j&&j.stale)return j;
    if(!j||j.ok===false)throw Error(j&&j.error||t('切换失败'));
    write.pending=false;
    if(VOLUME_WRITE.current!==write||write.sequence!==VOLUME_WRITE.sequence)return {ok:false,stale:true};
    write.accepted=true;
    if(typeof j.db==='number'&&Number.isFinite(j.db)&&j.db>=VOL_MIN&&j.db<=VOL_MAX)write.value=j.db;
    VOLUME_WRITE.confirmed=write.value;VOLUME_WRITE.statusBarrier=statusRequest;S.volDb=S.vol=write.value;
    syncVolUi();
    if(write.commitRequested)persistVolumeGesture(write);
    return j;
  }).catch(function(){
    write.pending=false;
    if(VOLUME_WRITE.current!==write||write.sequence!==VOLUME_WRITE.sequence)return {ok:false,stale:true};
    write.awaitChange=false;write.commitRequested=false;
    // A rejected gesture cannot leave its optimistic attenuation on screen.
    volumeGestureUntil=0;volumeRestore=null;
    if(Number.isFinite(S.volDb)){S.vol=S.volDb;syncVolUi(S.volDb);}
    var request=++statusRequest;
    return apiGetRaw('/api/status').then(function(j){
      if(!j||j.ok===false)throw Error();
      if(VOLUME_WRITE.current!==write||write.sequence!==VOLUME_WRITE.sequence)return {ok:false,stale:true};
      if(request>statusApplied){statusApplied=request;syncVolumeStatus(j,true);}
      toast(t('音量更改未生效，已重新读取实际音量。'));
      return {ok:false};
    }).catch(function(){tick();return {ok:false};});
  });
  return write.promise;
}


/* ---------------- 右栏标签页 ---------------- */
var RTAB = 'q';
function applyRTab(forceHide) {
  forceHide=forceHide||S.queueHidden||S.view==='settings'||S.view==='queue'||S.view==='station'||S.view==='listening';
  D('app').classList.toggle('noq',!!forceHide);
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
    s += tf('（起播 {0} dB，实时微调 {1} dB）', [S.startDb.toFixed(1), (v - S.startDb).toFixed(1)]);
  return s;
}
function sdmOrderText(p) {
  if (p.directSdm) return t('未调制（1-bit 直通）');
  var requested = Number(p.modOrderRequested) || 0, effective = Number(p.modOrder) || 0;
  if (effective > 0) return requested > 0
    ? tf('请求 {0} 阶 · 实际 {1} 阶 SDM', [requested, effective])
    : tf('实际 {0} 阶 SDM', [effective]);
  return requested > 0 ? tf('请求 {0} 阶 · 实际阶数待确认', [requested]) : t('实际阶数待确认');
}
function showChain() {
  D('mtitle').textContent = '信号链';
  D('modal').classList.add('on');
  var shortcuts = '<p class=transportKeys>' + esc(t('↑ / ↓ 调节音量，Shift + ← / → 跳转 5 秒。')) + '</p>';
  api('/api/signal').then(function (j) {
    if (!j.ok) { D('mbody').innerHTML = '<div class=empty>还没有开始播放</div>' + shortcuts; return; }
    var p = j.path;
    observeCleanupReport(p, 'local');
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
      // 2026-10-30：第四态（链路逐位但源有损）—— 与 'no' 分开，否则用户会一直去调设置。
      else if (bp === 'lossy') { label = '链路逐位 · 源本身有损'; color = '#38bdf8'; icon = '\u25cf'; }
      else if (bp === 'no') { label = '非 bit-perfect'; color = '#fbbf24'; icon = '!'; }
      else { label = 'BIT-PERFECT 状态未知'; color = '#94a3b8'; icon = '?'; }
      nodes.push({ k: icon + ' ' + t(label), v: why || t('（本次未记录）'), _bp: bp, _color: color });
    }());
    nodes.push({ k: '源文件', v: nam(p.src) + (p.codec ? '  \u00b7  ' + p.codec.toUpperCase() : '') });
    // P2（261027-05 A4）：解码环也要标多声道 —— 原来只有 "6 ch"，用户得自己知道 6 ch 就是 5.1。
    nodes.push({ k: '解码', v: (p.note || '原生无损解码') + '  \u00b7  ' + p.inRate + ' Hz / ' + p.inBits + ' bit / ' +
      p.inCh + ' ch' + (chMark(p.inCh) ? '（' + chLayout(p.inCh) + '）' : '') });
    // 261024-04 #8：绿色节点 = 本次**真的**由 GPU 参与计算的那一环。
    // 旧实现给「采样率转换」无条件加 gpu:true ⟹ 只要开了升频，信号链就永远有一个绿色节点，
    // 用户看到的"GPU"其实和本次播放是否用 GPU 无关。
    // 后端字段：/api/signal 的 p.gpuUsed（本次播放 GPU 是否参与）/ p.convGpu（卷积是否走 GPU）。
    // ⚠️ 未解决项：这两个字段后端还没加进 JSON（现在卷积开关叫 irGpu），所以先用
    //    p.convGpu ?? p.irGpu 兜底；后端加好后本兜底自动失效（新字段优先），到时候可以删掉兜底。
    var convGpu = (typeof p.convGpu === 'boolean') ? p.convGpu : (p.irGpu === true);
    var sigGpuUsed = (typeof p.gpuUsed === 'boolean') ? p.gpuUsed : !!S.gpuUsed;
    nodes.push({k:t('录音噪声清理'),v:cleanupStatusText(p,{enabled:!!ADV.cleanup,auto:ADV.cleanupAuto!==0,
      manualNoiseDb:ADV.cleanupNoiseDb,playing:S.playing,paused:S.paused,current:CLEANUP_UI.currentFile})});
    if (p.resample) nodes.push({ k: '采样率转换', v: (p.filter || 'Kaiser sinc') + '  \u00b7  ' + p.phase + '  \u00b7  ' + p.taps + t(' 抽头') + (p.twoStage ? t('  ·  两段式') : '') + '  \u2192  ' + p.outRate + ' Hz' + (sigGpuUsed ? '  \u00b7  GPU' : ''), gpu: !!sigGpuUsed });
    else nodes.push({ k: '采样率转换', v: '旁路（源采样率与输出一致，逐位直通）' });
    // 261024-04 #8：EQ / 卷积单独成一环，并且**只有卷积真的走 GPU 时**才标绿（后端 convGpu 字段）
    if (p.crossfeedRequested || p.crossfeed) nodes.push({k:t('耳机交叉馈送'),v:p.crossfeed?tf('已启用 · {0} Hz / 交叉衰减 {1} dB',[p.crossfeedFreq,p.crossfeedLevelDb]):t('已请求，当前链路未应用')});
    if (p.eq || p.conv) {
      var eqParts = [];
      if (p.eq) eqParts.push(tf('参数 EQ{0}', [(ADV.eq ? (' ' + nam(ADV.eq)) : '')]));
      if (p.conv) eqParts.push(tf('FIR 卷积{0}', [(ADV.ir ? (' ' + nam(ADV.ir)) : '')]));
      nodes.push({ k: 'EQ / 卷积', v: eqParts.join('  \u00b7  ') + '  \u00b7  ' + (convGpu ? 'GPU 计算' : 'CPU 计算'), gpu: convGpu });
    }
    // B3（2026-10-27）：房间校正自动应用 —— 单独一环。它和上面手填的 IR 是两件不同的事：
    // 一个是"测出来、自动挂上"，一个是"用户自己挑的文件"。三种状态都必须看得见：
    //   已应用（文件 X）/ 未应用（附 why）/ 已关闭（设置项为关）。
    // 后端字段：/api/signal 的 p.roomCorr = { enabled, applied, mismatch, manual, file, layout, want, taps, fs, ch, why }
    if (p.roomCorr && typeof p.roomCorr === 'object') {
      var rc = p.roomCorr;
      if (rc.applied) {
        nodes.push({ k: tf('房间校正 已应用（{0}）', [(rc.file || '校正集')]),
          v: (rc.layout || '?') + ' 布局  \u00b7  ' + (rc.ch || 0) + ' 声道  \u00b7  ' + (rc.taps || 0) +
             ' 抽头  \u00b7  ' + (rc.fs || 0) + ' Hz' +
             (rc.subDelaySamples || rc.subGainDb ? ('  \u00b7  超低对齐 ' + (+rc.subDelaySamples || 0).toFixed(1) +
               tf(' 样 / {0} dB（未接进播放链）', [(+rc.subGainDb || 0).toFixed(1)])) : '') +
             (rc.why ? ('  \u00b7  ' + rc.why) : '') });
      } else if (rc.enabled) {
        nodes.push({ k: rc.mismatch ? '房间校正 未应用（布局不匹配）' : '房间校正 未应用',
          v: rc.why || '（后端没有给出原因）' });
      } else {
        nodes.push({ k: '房间校正 已关闭', v: '设置「声音校准 → 自动应用校正」为关；只认手填的 IR' });
      }
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
      nodes.push({ k: p.directSdm ? t('DSD 直通') : t('DSD 调制'), v: 'DSD' + p.dsd + '（' + (p.dsdRate / 1000000).toFixed(4) + ' MHz）\u00b7 1-bit \u00b7 ' + sdmOrderText(p) });
      // 载波率必须用 p.carrier：DirectSDM / 回落的信号链里 outRate 记的是 DSD 位率（2822400），
      // DoP 真正开给设备的率是 carrier（DSD64 -> 176400）。旧写法会把 2822400 说成"24-bit 载波"。
      nodes.push({ k: 'DoP 封装', v: t('24-bit 载波 ') + (p.carrier || p.outRate) + t(' Hz · 标记 0x05/0xFA 交替') });
    } else {
      nodes.push({ k: '抖动 / 噪声整形', v: (p.shaper && p.shaper !== 'none') ? p.shaper : '无抖动（仅取整）' });
      if (p.quantizationBits > 0) nodes.push({ k: t('DAC 有效位深'), v: tf('实际量化 {0} bit · 驱动容器 {1} bit',[p.quantizationBits,p.outBits]) });
    }
    // 原生 DSD 失败回落 DoP：原因原样显示，绝不让用户以为这一曲是原生 DSD
    if (p.dsdFallback) nodes.push({ k: 'DoP 回落', v: p.dsdFallback });
    if (p.modFallback) nodes.push({ k: 'DSD 实际处理说明', v: p.modFallback });
    if (p.preAtten && p.preAtten < 0) {
      nodes.push({ k: '峰值保护', v: '检测到过采样峰值 ' + (p.ispPeak * 100).toFixed(1) + '%（' + p.ispOver +
        tf(' 个点超出满幅）→ 预衰减 {0} dB', [p.preAtten.toFixed(1)]) });
    }
    if (p.loudness) nodes.push({ k: '等响度补偿', v: '开启' });
    nodes.push({ k: '音量', v: volText(p.volume), id: 'chainVol' });
    var isNullOutput = p.device === 'Null 输出（不出声）';
    var endpointRate = p.dsd && !p.dsdNative ? (p.carrier || p.outRate) : p.outRate;
    nodes.push({ k: '输出', v: (isNullOutput ? t('Null 输出（不出声）') : p.device) + '  \u00b7  ' + endpointRate + ' Hz / ' + p.outBits + ' bit / ' + p.outCh +
      ' ch' + (chMark(p.outCh) ? '（' + chLayout(p.outCh) + '）' : '') +
      (isNullOutput ? '' : ('  \u00b7  ' + t((ADV.backend === 'wasapi-shared') ? '共享模式（非 bit-perfect）' : '独占模式'))) +
      tf('  \u00b7  缓冲 {0} ms', [p.buffer]) });

    var tDec = (typeof p.decodeMs === 'number' && p.decodeMs > 0)
      ? (p.decodeMs >= 1000 ? (p.decodeMs / 1000).toFixed(2) + ' s' : p.decodeMs.toFixed(1) + ' ms')
      : '–';
    var tWav = '–';
    if (typeof p.ffmpegMs === 'number' && p.ffmpegMs > 0) {
      tWav = '转码写 ' + p.ffmpegMs.toFixed(1) + ' ms' +
             (typeof p.wavReadMs === 'number' && p.wavReadMs > 0 ? tf(' / 读 {0} ms', [p.wavReadMs.toFixed(1)]) : '');
    } else if (typeof p.wavReadMs === 'number' && p.wavReadMs > 0) {
      tWav = tf('读取 {0} ms', [p.wavReadMs.toFixed(1)]);
    }
    var tDsp = '–';
    if (typeof p.dspBlockMs === 'number' && p.dspBlockMs > 0) {
      tDsp = p.dspBlockMs.toFixed(2) + ' ms/块' +
             (typeof p.dspTotalMs === 'number' && p.dspTotalMs > 0 ? tf('（累计 {0} ms）', [p.dspTotalMs.toFixed(1)]) : '');
    } else if (typeof p.dspTotalMs === 'number' && p.dspTotalMs > 0) {
      tDsp = tf('累计 {0} ms', [p.dspTotalMs.toFixed(1)]);
    }
    var tScan = (typeof p.scanMs === 'number' && p.scanMs > 0)
      ? (p.scanMs >= 1000 ? (p.scanMs / 1000).toFixed(2) + ' s' : p.scanMs.toFixed(0) + ' ms')
      : '–';

    nodes.push({
      k: '耗时统计',
      v: tf('解码 {0}  \u00b7  临时WAV {1}  \u00b7  DSP {2}  \u00b7  库扫描 {3}', [tDec, tWav, tDsp, tScan]),
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
    D('mbody').innerHTML = h + shortcuts;
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

/* (#6 / 261027-03) ? 的帮助浮层：body 下的固定定位单例 -------------------------------------
   旧做法是 CSS 伪元素 .tip::after（绝对定位在 .tip 里）。设置页整块在 .main{overflow:auto}
   里滚动，浮层一旦伸出主区左边界就被裁掉，而主区左边界 = 左栏右边界 ⟹ 用户看到的就是
   「帮助文字被左侧栏挡住」。实测（69 个 ? 逐个 hover）17 个浮层压在左栏上（30~2445.7px²），
   最狠的被吃掉 69.5px 宽），同一批 17 个被 .main 裁掉 1.71~69.5px。左栏没有层叠上下文，
   z-index:40 本来就压得住它 —— 输的是**裁剪**。
   ⟹ 改成固定定位的单例层，位置由 tipPlace() 算，并夹在「左栏右边界 → 视口右/下边界」之内。 */
var _tipLayer = null, _tipOwner = null;
function tipLayer() {
  if (_tipLayer && document.body.contains(_tipLayer)) return _tipLayer;
  _tipLayer = document.getElementById('tipLayer');
  if (!_tipLayer) {
    _tipLayer = document.createElement('div');
    _tipLayer.id = 'tipLayer';
    _tipLayer.setAttribute('role', 'tooltip');
    document.body.appendChild(_tipLayer);
  }
  return _tipLayer;
}
function tipPlace(el) {
  var L = tipLayer(); if (!L || !el) return;
  // 属性里的原文是中文，浮层用 textContent 直接注入 ⟹ 必须显式过一遍 t()（ui_script.h:98），
  // 否则 data-tip 就算进了 i18nAttrs 也只是把属性值换了、浮层还是旧值。
  var tipText = t(el.getAttribute('data-tip') || '');
  if (L.textContent !== tipText) L.textContent = tipText;
  L.classList.add('on');                       // 先上屏才能量到尺寸
  var side = document.querySelector('.side');
  var sr = side ? side.getBoundingClientRect() : null;
  var vw = window.innerWidth || document.documentElement.clientWidth || 0;
  var vh = window.innerHeight || document.documentElement.clientHeight || 0;
  // 左下限 = 左栏右边界 + 8：浮层绝不压到左栏（固定定位不参与 grid 布局，只能靠坐标避开）
  var minL = sr ? (Math.ceil(sr.right) + 8) : 8;
  if (minL > vw - 160) minL = 8;               // 极窄窗口：宁可靠左，也不能把浮层挤成 0 宽
  L.style.maxWidth = Math.max(140, Math.min(290, vw - minL - 8)) + 'px';
  var w = L.offsetWidth, h = L.offsetHeight;
  var r = el.getBoundingClientRect();
  var x = Math.round(r.left + r.width / 2 - w / 2);
  if (x < minL) x = minL;
  if (x + w > vw - 8) x = Math.max(minL, vw - 8 - w);
  // 分组标题的 (?) 一律向下展开：向上会盖住吸顶的页签条（旧 CSS 也是这样单独处理的）
  var below = !!(el.closest && el.closest('.sghead'));
  var y = below ? Math.round(r.bottom + 8) : Math.round(r.top - h - 8);
  if (y < 8) y = Math.round(r.bottom + 8);          // 上方放不下 → 翻到下方
  if (y + h > vh - 8) y = Math.max(8, vh - 8 - h);  // 下方也放不下 → 夹进视口
  L.style.left = x + 'px';
  L.style.top = y + 'px';
}
function tipShow(el) { if (!el) return; _tipOwner = el; tipPlace(el); }
function tipHide() { if (_tipLayer) _tipLayer.classList.remove('on'); _tipOwner = null; }
function bindTips() {
  if (window.__tipBound) return; window.__tipBound = 1;
  function ownerOf(e) {
    var t = e && e.target;
    return (t && t.closest) ? t.closest('.tip') : null;
  }
  document.addEventListener('mouseover', function (e) {
    var el = ownerOf(e);
    if (el) { if (el !== _tipOwner) tipShow(el); return; }
    if (_tipOwner) tipHide();
  }, true);
  document.addEventListener('mouseout', function (e) {
    var el = ownerOf(e);
    if (el && (el === _tipOwner)) tipHide();
  }, true);
  document.addEventListener('focusin', function (e) {
    var el = ownerOf(e);
    if (el) tipShow(el); else if (_tipOwner) tipHide();
  }, true);
  document.addEventListener('focusout', function () { if (_tipOwner) tipHide(); }, true);
  // 位置是 fixed ⟹ 一旦滚动/改尺寸就作废（否则浮层会脱离它的 ?）。
  // 设置页的滚动容器是 .main，scroll 不冒泡，必须用捕获阶段。
  window.addEventListener('scroll', function () { if (_tipOwner) tipHide(); }, true);
  window.addEventListener('resize', function () { if (_tipOwner) tipHide(); });
  document.addEventListener('click', function () { if (_tipOwner) tipHide(); }, true);
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
    m.textContent = tf('NAA：把 PCM 按 RTANet 协议推到 {0}（端点机需运行 rtanet_rx）。', [(ep || '端点（尚未发现）')]);
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
    else m.textContent = tf('ASIO：本机驱动 {0}。', [ASIO_DRIVERS.join('、')]);
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
        name = tf('[回环] {0}', [name]);
      }
      if (d.isDefault) {
        name += t('（默认）');
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
    fEl.textContent = (st.frames || 0) + tf(' 帧 ({0}s)', [sec]);
  }

  var sEl = D('sp_stat_spec');
  if (sEl) {
    if (st.rate && st.channels) {
      sEl.textContent = st.rate + ' Hz / ' + st.channels + tf(' 声道{0}', [(st.device ? (' · ' + st.device) : '')]);
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
    xEl.textContent = tf('溢出 {0} / 欠载 {1}', [(st.overflow || 0), (st.underflow || 0)]);
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
      toast(tf('启动失败: {0}', [err]));
      pollInputState();
    }
  }).catch(function (e) {
    var err = (e && e.message) || '网络或接口错误';
    if (D('sp_inmsg')) D('sp_inmsg').textContent = err;
    toast(tf('启动失败: {0}', [err]));
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
      toast(tf('停止失败: {0}', [err]));
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
  if (!ensurePlaybackSettings()) return Promise.resolve({ok:false,errorCode:'PLAYBACK_SETTINGS_NOT_READY'});
  // Device discovery owns this preference alone; a delayed DSP form snapshot
  // must never overwrite a newer workbench transaction.
  return persistSettings('upnpdev=' + encodeURIComponent(ADV.upnpDev));
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
        ? (tf('共发现 {0} 个渲染器', [_castDevices.length]))
        : '未发现 UPnP/DLNA 渲染器（可能未开机、不在同一子网，或 SSDP 组播被代理/防火墙拦截）';
    }
    if (msg2) {
      msg2.textContent = _castDevices.length
        ? (tf('共发现 {0} 个渲染器', [_castDevices.length]))
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
// 2026-10-29（管理员反馈 261029-01 #10：「设置-PCM中，输出格式的设置选项不见了！」）：
//   真因：backend=upnp 时这里连 sp_rate / sp_bits 一起隐藏，而「输出格式（PCM）」那一组只有这两个字段
//   ⟹ sgroup 标题还在、字段全没了，看起来就是一个空框，用户以为功能被删了。
//   采样率/位深是 **PCM 链路属性**（切回 WASAPI 立刻生效），跟当前后端无关，不该隐藏。
//   仍然隐藏的这几项确实只对本机输出有意义（选设备/声道/缓冲/渐变/输出模式）。
var _UPNP_IRRELEVANT = ['sp_device', 'sp_ch', 'sp_buffer', 'sp_ramp', 'sp_outmode'];
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
  _collapseEmptySgroups();
}

// 2026-10-29（#10 的兜底）：某个 .sgroup 里的 .sfld 如果全被隐藏，就把整组一起收掉。
// 为什么需要：sgroup 的标题是独立渲染的，字段被 display:none 后标题仍占位 ⟹ 出现"只有标题的空框"，
// 用户会以为功能消失（这正是 #10 的现象）。按组收敛，比逐个字段打补丁更不容易再犯。
function _collapseEmptySgroups() {
  var page = D('setpage');
  if (!page) return;
  Array.prototype.forEach.call(page.querySelectorAll('.sgroup'), function (g) {
    var body = g.querySelector('.sgbody');
    if (!body) return;
    var fields = body.querySelectorAll('.sfld');
    if (!fields.length) return;                  // 没有字段的组（纯说明）不动
    var anyVisible = false;
    for (var i = 0; i < fields.length; i++) {
      if (fields[i].style.display !== 'none') { anyVisible = true; break; }
    }
    g.style.display = anyVisible ? '' : 'none';
  });
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
        msg.textContent = d ? ((d.model ? (tf('型号: {0} · ', [d.model])) : '') + (d.hasAVTransport ? '支持 AVTransport' : '未知传输服务'))
                            : (v ? (tf('已选择：{0}', [v])) : '点「重新发现」扫描局域网里的 MediaRenderer');
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
  return '<svg width=16 height=16 viewBox="0 0 24 24" fill=none stroke=currentColor stroke-width=1.8 stroke-linecap=round stroke-linejoin=round>' +
         '<path d="M2 16.1A5 5 0 0 1 5.9 20"/><path d="M2 12.05A9 9 0 0 1 9.95 20"/>' +
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
    nb.innerHTML = castIconSvg();
    nb.classList.toggle('on', running);
    nb.title = running ? (tf('点击停止投放并恢复本机输出（已投放到 {0}{1}）', [nm, (st.phase ? tf(' · 阶段：{0}', [st.phase]) : '')]))
                       : '把当前曲目推送到 UPnP / DLNA 设备';
  }
  var badge = D('castBadge');
  if (badge) {
    badge.style.display = running ? 'inline-flex' : 'none';
    badge.title = running ? (tf('正在投放到 {0}', [nm])) : '';
  }
  var push = D('sp_cast_push'), stop = D('sp_cast_stop');
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
    observeCleanupReport(st && st.ok ? st : null, 'cast');
    // 2026-10-29（#9 的连带）：投放失败后把后端回退，否则 ADV.backend 一直停在 upnp，
    // 用户之后每次播放都会再走一次注定失败的投放（表现为「点了没反应 / 又自动暂停」）。
    if (st && st.running) CAST.wasRunning = true;
    else if (CAST.wasRunning && st && st.phase === 'error') {
      CAST.wasRunning = false;
      if (ADV.backend === 'upnp') {
        ADV.backend = CAST.prevBackend || 'wasapi';
        saveSettings();
        toast(tf('投放失败：{0}，已回到本机输出', [(st.error || '未知原因')]));
      }
    }
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
function castPush(file, devOverride) {
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
    toast(tf('已推送到 {0}', [castDevNameOf(dev)]));
    pollCastState(true);
  };
  var dev = (devOverride || ADV.upnpDev || '').trim();
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
    toast(tf('已停止投放，输出回到 {0}', [back]));
    if (S.lastCur) playPath(S.lastCur);
    pollCastState(true);
  }).catch(function () { toast('停止投放失败'); });
}
function castToggle() {
  if (castRunning()) { castStop(); return; }
  // 2026-10-29（管理员 261029-02 #3）：「点击推流按钮，仍然没有看到弹框选择推流对象。
  //   我确实在主页看到了一个推流对象选择，但是推流按钮是在单曲页面的，你在外面搞一个对象选择有什么用？」
  //   ⟹ 单曲页这个按钮**必须先让用户当场选设备**再推，不能沿用别处记忆的 upnpDev 直接推。
  //   （设置-流媒体页的推送区块有自己的下拉，那条路径仍走 castPush(file) 的旧逻辑。）
  npPickCastDevice(function (d) { if (d) castPush(S.lastCur, d); });
}
function npCastBtnHtml() {
  return '';
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
  for (var i = 0; i < n; ++i) out.push(tf('声道 {0}', [(i + 1)]));
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
  // 261030-01 规格 v2.1：带源参数取真相源（fs=源采样率，content=真实内容边缘）。
  // 1x 路径按 CD（44.1k / 内容 20 kHz）算；Nx 路径另有一份（见 loadFilterPhasesNx）。
  return api('/api/filters?fs=44100&content=20000').then(function (j) {
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
  // 261030-01 规格 v2.1（D-13 / D-16）：名字后面挂「抑制 @ 预振铃」两个数字。
  // 后端 preset_metrics() 是唯一真相源，前端不重算规则（否则必然漂移）。
  // 2026-11-01（多语言）：这一行是运行时拼装串，i18n 的整串匹配抓不到 ⟹ 每段都过 t()。
  var metrics = (typeof it.ceilingDb === 'number') ? (' · ' + t('目标抑制') + ' ' + it.ceilingDb + ' dB') : '';
  if (it.ringMeasured && typeof it.ringMs === 'number') {
    metrics += ' · ' + t('峰前包络') + ' ' + it.ringMs.toFixed(2) + ' ms' + t('（相对峰值 −60 dB；参考 ')
      + (it.ringReferenceInRate / 1000) + ' → ' + (it.ringReferenceOutRate / 1000)
      + ' kHz' + t('，PCM 预设；不是群延迟）');
  }
  var tapTxt = (typeof it.T === 'number' && it.T > 0)
    ? t('（T=§）').replace('§', it.T)
    : t('（抽头 §）').replace('§', it.taps);
  // 261030-01：这行是运行时拼装串，i18n 的「文本节点整串匹配」抓不到 ⟹ 直接走 t()。
  // 数字与单位（dB / ms / T=）语言无关，不进字典。
  // ★ 2026-10-31（§7.4 L1）：选了「自动」时，先说清它到底解析成了哪个预设 ——
  //   否则用户只看到「预设 线性相位」却不知道该预设叫什么。resolved 由后端给，前端不硬编码。
  var autoPrefix = it.resolved ? (t('自动') + ' → ' + it.resolved + ' · ') : '';
  msg.textContent = autoPrefix + t('预设') + ' ' + t(it.phaseName) + metrics + tapTxt
    + ' · ' + t('实际输出') + ' ' + t(actual) + (override ? '（' + t('强制覆盖') + '）' : '');
}

/* ---------------- 滤波器库 / 详情（2026-11-15） ----------------
   用户要求：鼠标移到任何一个滤波上，右侧显示它的设计详情；要做得比参考图更清楚。
   数据来源：/api/filters（设计参数与相位真相源）+ /api/filter/detail（构建期用生产系数
   测出的幅频/群延迟/冲激）。前端不重算规则，只负责画图和解释。 */
var RL_FB = { target: 'filter', items: [], chosen: '', preview: '', cache: {}, timer: 0, seq: 0, dialog: null };
function rlFbHz(v) {
  if (!isFinite(v)) return '—';
  return v >= 1000 ? ((v / 1000).toFixed(v >= 10000 ? 1 : 2) + ' kHz') : (Math.round(v) + ' Hz');
}
function rlFbDb(v) {
  if (!isFinite(v)) return '—';
  return (v <= -299.5 ? '≤ −300 dB' : (v.toFixed(v > -10 ? 3 : 1) + ' dB'));
}
function rlFbMs(v) {
  if (!isFinite(v)) return '—';
  return (v < 0.01 ? v.toFixed(3) : v.toFixed(2)) + ' ms';
}
function rlFbGroup(it) {
  if (it.name === 'auto') return '自动';
  if (it.apodizing) return '切趾';
  if (it.phase === 'minimum') return '最小相位';
  if (it.phase === 'intermediate') return '混合相位';
  if (it.forNx) return 'Nx 专用';
  if (it.twoStage) return '两段式';
  return '线性相位';
}
function rlFbChart(points, opt) {
  var w = opt.w || 360, h = opt.h || 118, pl = 38, pr = 10, pt = 8, pb = 24;
  var valid = (points || []).filter(function (p) { return p && isFinite(p[0]) && isFinite(p[1]); });
  if (!valid.length) return '<div class=rlFbEmpty>无数据</div>';
  var x0 = (opt.x0 !== undefined) ? opt.x0 : valid[0][0];
  var x1 = (opt.x1 !== undefined) ? opt.x1 : valid[valid.length - 1][0];
  var ys = valid.map(function (p) { return p[1]; });
  var y0 = (opt.y0 !== undefined) ? opt.y0 : Math.min.apply(Math, ys);
  var y1 = (opt.y1 !== undefined) ? opt.y1 : Math.max.apply(Math, ys);
  if (y1 <= y0) { y1 = y0 + 1; }
  function X(v) {
    var t = opt.logX ? (Math.log10(Math.max(v, 1e-9)) - Math.log10(Math.max(x0, 1e-9))) /
                       (Math.log10(Math.max(x1, 1e-9)) - Math.log10(Math.max(x0, 1e-9)))
                     : (v - x0) / (x1 - x0);
    return pl + Math.max(0, Math.min(1, t)) * (w - pl - pr);
  }
  function Y(v) {
    var t = (v - y0) / (y1 - y0);
    return pt + (1 - Math.max(0, Math.min(1, t))) * (h - pt - pb);
  }
  var path = '';
  for (var i = 0; i < valid.length; ++i) {
    path += (i ? 'L' : 'M') + X(valid[i][0]).toFixed(2) + ' ' + Y(valid[i][1]).toFixed(2);
  }
  var grid = '';
  var yTicks = opt.yTicks || 5;
  for (var g = 0; g <= yTicks; ++g) {
    var yv = y0 + (y1 - y0) * g / yTicks;
    grid += '<line x1=' + pl + ' y1=' + Y(yv).toFixed(1) + ' x2=' + (w - pr) + ' y2=' + Y(yv).toFixed(1) + ' class="rlFbGrid"/>';
    grid += '<text x=' + (pl - 4) + ' y=' + (Y(yv) + 3).toFixed(1) + ' class=rlFbTick text-anchor=end>' + esc(opt.yFmt ? opt.yFmt(yv) : String(Math.round(yv))) + '</text>';
  }
  (opt.xTicks || []).forEach(function (v) {
    if (!isFinite(v) || v < x0 || v > x1) return;
    var mx = X(v);
    grid += '<text x=' + mx.toFixed(1) + ' y=' + (h - 13) + ' class=rlFbTick text-anchor=middle>' +
            esc(opt.xFmt ? opt.xFmt(v) : String(Math.round(v))) + '</text>';
  });
  var marks = '';
  (opt.marks || []).forEach(function (m) {
    if (!isFinite(m.x) || m.x < x0 || m.x > x1) return;
    var mx = X(m.x);
    marks += '<line x1=' + mx.toFixed(1) + ' y1=' + pt + ' x2=' + mx.toFixed(1) + ' y2=' + (h - pb) + ' class="rlFbMark" style="stroke:' + (m.color || 'var(--acc)') + '"/>';
    marks += '<text x=' + (mx + 3).toFixed(1) + ' y=' + (pt + 9) + ' class=rlFbMarkText>' + esc(m.label || '') + '</text>';
  });
  return '<svg viewBox="0 0 ' + w + ' ' + h + '" class=rlFbChart role=img aria-label="' + esc(opt.label || '') + '">' +
    grid + marks + '<path d="' + path + '" class="rlFbLine"/></svg>';
}
function rlFbMetrics(it) {
  var trans = Math.max(0, it.stopStartHz - it.passHz);
  var rows = [
    ['通带边缘', rlFbHz(it.passHz)],
    ['阻带起点', rlFbHz(it.stopStartHz)],
    ['过渡带宽', rlFbHz(trans)],
    ['设计衰减', rlFbDb(-Math.abs(it.designDb))],
    ['实测阻带', rlFbDb(it.stopDb)],
    ['全频带阻带', rlFbDb(it.fullBandStopDb || it.stopDb)],
    ['通带最大偏差', rlFbDb(it.passDb)],
    ['每相抽头 T', String(it.T)],
    ['原型抽头', String(it.taps)],
    ['群延迟 @1 kHz', rlFbMs(it.gd1kMs)],
    ['峰前 / 峰后振铃', rlFbMs(it.preMs) + ' / ' + rlFbMs(it.postMs)],
    ['参考配置', (it.inRate / 1000) + ' → ' + (it.outRate / 1000) + ' kHz'],
    ['全频带证书', it.fullBandPoints ? (it.fullBandPoints.toLocaleString() + ' 点 FFT') : '—']
  ];
  return '<div class=rlFbGrid>' + rows.map(function (r) {
    return '<div class=rlFbCell><span>' + esc(t(r[0])) + '</span><b>' + esc(r[1]) + '</b></div>';
  }).join('') + '</div>';
}
function rlFbDetailHtml(it) {
  if (!it) return '<div class=rlFbEmpty>把鼠标移到左侧任意滤波器上，这里显示它的设计详情。</div>';
  var badges = [it.window, it.phase, it.family];
  if (it.resolved) badges.push('自动 → ' + it.resolved);
  if (it.apodizing) badges.push('切趾');
  if (it.twoStage) badges.push('两段式');
  if (it.forNx) badges.push('Nx 专用');
  var audioTop = it.inRate / 2;
  var resp = rlFbChart(it.response, {
    label: '幅频响应', x0: 10, x1: it.fs / 2, logX: true, y0: -320, y1: 10,
    xLabel: 'Hz（对数）', yFmt: function (v) { return v + ' dB'; },
    xTicks: [100, 1000, 10000, audioTop], xFmt: function (v) { return v >= 1000 ? (v / 1000).toFixed(v >= 10000 ? 1 : 0) + 'k' : String(Math.round(v)); },
    marks: [{ x: it.passHz, label: '通带', color: '#7fd1b9' }, { x: it.stopStartHz, label: '阻带', color: '#e6a23c' }, { x: audioTop, label: '音频上限', color: '#8ab4f8' }]
  });
  var pass = rlFbChart(it.passCurve, {
    label: '通带放大', x0: 0, x1: it.passHz, y0: Math.min(-0.05, it.passDb * 1.4), y1: Math.max(0.05, it.passDb * 1.4),
    xLabel: 'Hz（线性）', yFmt: function (v) { return v.toFixed(2); }, yTicks: 4,
    xTicks: [0, it.passHz / 2, it.passHz], xFmt: function (v) { return v >= 1000 ? (v / 1000).toFixed(1) + 'k' : String(Math.round(v)); }
  });
  var gdTop = Math.min(20000, it.passHz);
  var gd = rlFbChart(it.gdCurve, {
    label: '群延迟', x0: 100, x1: gdTop, logX: true, y0: 0, y1: Math.max(0.1, it.gd1kMs * 1.25),
    xLabel: 'Hz（对数）', yFmt: function (v) { return v.toFixed(1); }, yTicks: 4,
    xTicks: [100, 1000, 10000, gdTop], xFmt: function (v) { return v >= 1000 ? (v / 1000).toFixed(v >= 10000 ? 1 : 0) + 'k' : String(Math.round(v)); },
    marks: [{ x: 1000, label: '1 kHz', color: '#8ab4f8' }]
  });
  var imp = rlFbChart(it.impulse, {
    label: '冲激响应', x0: -(it.impulse.length / 2) / it.fs * 1000, x1: (it.impulse.length / 2) / it.fs * 1000,
    y0: -1.05, y1: 1.05, xLabel: 'ms（相对峰值）', yFmt: function (v) { return v.toFixed(2); }, yTicks: 4,
    xTicks: [-(it.impulse.length / 2) / it.fs * 1000, 0, (it.impulse.length / 2) / it.fs * 1000], xFmt: function (v) { return v.toFixed(2); },
    marks: [{ x: 0, label: '峰值', color: '#e6a23c' }]
  });
  return '<div class=rlFbDetailHead><h3>' + esc(it.name) + '</h3><div class=rlFbBadges>' +
    badges.map(function (b) { return '<span>' + esc(t(b)) + '</span>'; }).join('') + '</div></div>' +
    rlFbMetrics(it) +
    '<div class=rlFbCharts><figure><figcaption>' + esc(t('幅频响应 · 全频段（含镜像）')) + '</figcaption>' + resp + '</figure>' +
    '<figure><figcaption>' + esc(t('通带放大（偏差轴）')) + '</figcaption>' + pass + '</figure>' +
    '<figure><figcaption>' + esc(t('群延迟（实测）')) + '</figcaption>' + gd + '</figure>' +
    '<figure><figcaption>' + esc(t('冲激响应 · 峰值两侧')) + '</figcaption>' + imp + '</figure></div>' +
    '<p class=rlFbNote>' + esc(tf('读数来自当前生产系数的 {0} 点全频带 FFT（10 Hz–Nyquist），UI 曲线 {1} 点；实际播放会按设备采样率重新设计，数值会随输出率与抽头预算变化。', [it.fullBandPoints || 0, it.responseCount || 0])) + '</p>';
}
function rlFbRender() {
  var host = D('rlFbDetail');
  if (!host) return;
  var it = RL_FB.cache[RL_FB.preview];
  host.innerHTML = rlFbDetailHtml(it);
  var btn = D('rlFbUse');
  if (btn) { btn.disabled = !RL_FB.chosen; btn.textContent = RL_FB.chosen ? tf('选用「{0}」', [RL_FB.chosen]) : t('选用此滤波器'); }
  var st = D('rlFbState');
  if (st) st.textContent = RL_FB.chosen ? tf('已选：{0}', [RL_FB.chosen]) : t('悬停预览，点击选定');
}
function rlFbPreview(name) {
  if (!name || (name === RL_FB.preview && RL_FB.cache[name])) return;
  RL_FB.preview = name;
  var list = D('rlFbList');
  if (list) Array.prototype.forEach.call(list.querySelectorAll('[data-fb-name]'), function (b) {
    b.setAttribute('aria-current', String(b.dataset.fbName === name));
  });
  if (RL_FB.cache[name]) { rlFbRender(); return; }
  var host = D('rlFbDetail');
  if (host) host.innerHTML = '<div class=rlFbLoading>' + esc(t('正在读取设计数据…')) + '</div>';
  var seq = ++RL_FB.seq;
  api('/api/filter/detail?name=' + encodeURIComponent(name)).then(function (j) {
    if (seq !== RL_FB.seq || !j || !j.ok || !j.item) return;
    RL_FB.cache[name] = j.item;
    if (RL_FB.preview === name) rlFbRender();
  }).catch(function () {
    if (seq === RL_FB.seq && host) host.innerHTML = '<div class=rlFbEmpty>' + esc(t('读取失败：该滤波器没有详情数据')) + '</div>';
  });
}
function rlFbListHtml(items) {
  var q = (D('rlFbSearch') && D('rlFbSearch').value || '').trim().toLowerCase();
  var order = ['自动', '线性相位', '最小相位', '混合相位', '切趾', '两段式', 'Nx 专用'];
  var groups = {};
  items.forEach(function (it) {
    if (q && it.name.toLowerCase().indexOf(q) < 0) return;
    var g = rlFbGroup(it);
    (groups[g] = groups[g] || []).push(it);
  });
  var h = '';
  order.forEach(function (g) {
    if (!groups[g] || !groups[g].length) return;
    h += '<div class=rlFbGroup><div class=rlFbGroupTitle>' + esc(t(g)) + ' · ' + groups[g].length + '</div>';
    groups[g].forEach(function (it) {
      h += '<button type=button class=rlFbItem data-fb-name="' + esc(it.name) + '"' +
           (it.name === RL_FB.chosen ? ' aria-pressed=true' : '') + '>' +
           '<span>' + esc(it.name) + '</span><small>' + esc(rlFbHz(it.passHz)) + ' · ' + esc(rlFbDb(-Math.abs(it.ceilingDb))) + '</small></button>';
    });
    h += '</div>';
  });
  return h || '<div class=rlFbEmpty>' + esc(t('没有匹配的滤波器')) + '</div>';
}
function rlFbFill() {
  var list = D('rlFbList');
  if (!list) return;
  list.innerHTML = rlFbListHtml(RL_FB.items);
  if (RL_FB.preview) rlFbPreview(RL_FB.preview);
}
function rlFilterBrowserOpen(target) {
  target = target === 'filterNx' ? 'filterNx' : 'filter';
  RL_FB.target = target;
  var old = D('rlFilterBrowser');
  if (old) old.remove();
  var dialog = document.createElement('dialog');
  dialog.id = 'rlFilterBrowser';
  dialog.className = 'rlFbDialog';
  dialog.innerHTML = '<style>' +
    '.rlFbDialog{width:min(1120px,96vw);max-height:92vh;padding:0;border:1px solid var(--line,#3a3a3a);border-radius:12px;background:var(--bg,#1b1b1b);color:var(--fg,#eee);overflow:hidden}' +
    '.rlFbDialog::backdrop{background:#000a}' +
    '.rlFbHead{display:flex;align-items:center;justify-content:space-between;gap:12px;padding:14px 18px;border-bottom:1px solid var(--line,#3a3a3a)}' +
    '.rlFbHead h2{margin:0;font:600 18px/1.3 Georgia,serif}.rlFbHead p{margin:2px 0 0;font-size:11px;color:var(--muted,#999)}' +
    '.rlFbHead button{background:none;border:0;color:inherit;font-size:22px;cursor:pointer}' +
    '.rlFbBody{display:grid;grid-template-columns:minmax(220px,300px) minmax(0,1fr);height:calc(92vh - 132px);min-height:360px;overflow:hidden}' +
    '.rlFbLeft{border-right:1px solid var(--line,#3a3a3a);display:flex;flex-direction:column;min-height:0;height:100%}' +
    '.rlFbSearchWrap{padding:10px 12px;border-bottom:1px solid var(--line,#3a3a3a)}' +
    '.rlFbSearchWrap input{width:100%;padding:7px 9px;border-radius:6px;border:1px solid var(--line,#3a3a3a);background:var(--field,#111);color:inherit}' +
    '.rlFbList{overflow:auto;padding:8px 8px 14px;min-height:0}' +
    '.rlFbGroupTitle{font-size:10px;letter-spacing:.08em;color:var(--muted,#999);margin:10px 6px 5px;text-transform:uppercase}' +
    '.rlFbItem{display:flex;justify-content:space-between;gap:8px;width:100%;text-align:start;padding:7px 9px;border:0;border-radius:6px;background:none;color:inherit;cursor:pointer;font:12px/1.35 ui-monospace,Consolas,monospace}' +
    '.rlFbItem:hover,.rlFbItem:focus-visible,.rlFbItem[aria-current=true]{background:var(--tint,#2a2a2a);outline:none}' +
    '.rlFbItem[aria-current=true]{box-shadow:inset 3px 0 var(--acc,#d9b875)}.rlFbItem small{color:var(--muted,#999);white-space:nowrap}' +
    '.rlFbRight{overflow:auto;padding:14px 18px 18px;min-height:0;height:100%}' +
    '.rlFbDetailHead{display:flex;align-items:center;gap:10px;flex-wrap:wrap;margin-bottom:10px}.rlFbDetailHead h3{margin:0;font:600 20px/1.2 Georgia,serif}' +
    '.rlFbBadges{display:flex;gap:6px;flex-wrap:wrap}.rlFbBadges span{font-size:10px;padding:3px 7px;border:1px solid var(--line,#3a3a3a);border-radius:999px;color:var(--muted,#bbb)}' +
    '.rlFbGrid{display:grid;grid-template-columns:repeat(4,minmax(0,1fr));gap:7px;margin:8px 0 12px}' +
    '.rlFbCell{border:1px solid var(--line,#3a3a3a);border-radius:7px;padding:7px 8px;min-width:0}' +
    '.rlFbCell span{display:block;font-size:10px;color:var(--muted,#999);margin-bottom:3px}.rlFbCell b{font:600 13px/1.3 ui-monospace,Consolas,monospace;overflow-wrap:anywhere}' +
    '.rlFbCharts{display:grid;grid-template-columns:1fr 1fr;gap:10px}.rlFbCharts figure{margin:0;border:1px solid var(--line,#3a3a3a);border-radius:8px;padding:7px 8px 4px;min-width:0}' +
    '.rlFbCharts figcaption{font-size:10px;color:var(--muted,#999);margin-bottom:2px}.rlFbChart{display:block;width:100%;height:auto}' +
    'line.rlFbGrid{stroke:#5556;stroke-width:.6}.rlFbTick{font-size:8px;fill:#999}.rlFbMark{stroke-width:.8;stroke-dasharray:2 2;opacity:.8}.rlFbMarkText{font-size:7.5px;fill:#bbb}' +
    '.rlFbLine{fill:none;stroke:var(--acc,#d9b875);stroke-width:1.3;vector-effect:non-scaling-stroke}' +
    '.rlFbNote{font-size:10px;line-height:1.6;color:var(--muted,#999);margin:10px 0 0}' +
    '.rlFbLoading,.rlFbEmpty{padding:28px 10px;color:var(--muted,#999);font-size:12px;text-align:center}' +
    '.rlFbFoot{display:flex;align-items:center;justify-content:space-between;gap:12px;padding:10px 18px;border-top:1px solid var(--line,#3a3a3a)}' +
    '.rlFbFoot span{font-size:11px;color:var(--muted,#999)}.rlFbFoot .btn{min-height:32px;padding:6px 14px;border-radius:6px;border:1px solid var(--line,#3a3a3a);background:var(--field,#222);color:inherit;cursor:pointer}' +
    '.rlFbFoot .btn.pri{background:var(--acc,#d9b875);color:#20180b;border-color:var(--acc,#d9b875)}.rlFbFoot .btn:disabled{opacity:.45;cursor:default}' +
    '@media(max-width:760px){.rlFbBody{grid-template-columns:1fr}.rlFbLeft{max-height:220px;border-right:0;border-bottom:1px solid var(--line,#3a3a3a)}.rlFbGrid{grid-template-columns:1fr 1fr}.rlFbCharts{grid-template-columns:1fr}}' +
    '</style>' +
    '<header class=rlFbHead><div><h2>' + esc(t('滤波器库')) + '</h2><p>' + esc(t('悬停查看设计详情 · 点击选定 · 键盘 ↑↓ 浏览')) + '</p></div><button type=button aria-label="' + esc(t('关闭')) + '">×</button></header>' +
    '<div class=rlFbBody><div class=rlFbLeft><div class=rlFbSearchWrap><input id=rlFbSearch type=search placeholder="' + esc(t('搜索滤波器名…')) + '" spellcheck=false></div><div class=rlFbList id=rlFbList></div></div>' +
    '<div class=rlFbRight id=rlFbDetail></div></div>' +
    '<footer class=rlFbFoot><span id=rlFbState></span><div><button type=button class=btn id=rlFbCancel>' + esc(t('取消')) + '</button> <button type=button class="btn pri" id=rlFbUse disabled>' + esc(t('选用此滤波器')) + '</button></div></footer>';
  document.body.appendChild(dialog);
  RL_FB.dialog = dialog;
  dialog.querySelector('header button').onclick = function () { dialog.close(); };
  dialog.addEventListener('click', function (e) { if (e.target === dialog) dialog.close(); });
  dialog.addEventListener('close', function () { dialog.remove(); RL_FB.dialog = null; });
  D('rlFbCancel').onclick = function () { dialog.close(); };
  D('rlFbSearch').oninput = function () { rlFbFill(); };
  var list = D('rlFbList');
  list.addEventListener('mouseover', function (e) { var b = e.target.closest('[data-fb-name]'); if (b) rlFbPreview(b.dataset.fbName); });
  list.addEventListener('focusin', function (e) { var b = e.target.closest('[data-fb-name]'); if (b) rlFbPreview(b.dataset.fbName); });
  list.addEventListener('click', function (e) {
    var b = e.target.closest('[data-fb-name]');
    if (!b) return;
    RL_FB.chosen = b.dataset.fbName;
    Array.prototype.forEach.call(list.querySelectorAll('[data-fb-name]'), function (x) { x.setAttribute('aria-pressed', String(x === b)); });
    rlFbPreview(RL_FB.chosen);
    rlFbRender();
  });
  list.addEventListener('dblclick', function (e) { var b = e.target.closest('[data-fb-name]'); if (b) { RL_FB.chosen = b.dataset.fbName; rlFbApply(); } });
  list.addEventListener('keydown', function (e) {
    if (e.key !== 'ArrowDown' && e.key !== 'ArrowUp') return;
    e.preventDefault();
    var all = Array.prototype.slice.call(list.querySelectorAll('[data-fb-name]'));
    if (!all.length) return;
    var i = all.indexOf(document.activeElement);
    var next = all[Math.max(0, Math.min(all.length - 1, i + (e.key === 'ArrowDown' ? 1 : -1)))];
    if (next) { next.focus(); rlFbPreview(next.dataset.fbName); }
  });
  D('rlFbUse').onclick = rlFbApply;
  var fs = target === 'filterNx' ? 96000 : 44100;
  var content = target === 'filterNx' ? 40000 : 20000;
  var current = (D(target === 'filterNx' ? 'sp_filtNx' : 'sp_filt') || {}).value ||
                (D(target === 'filterNx' ? 'sFiltNx' : 'sFilt') || {}).value || '';
  api('/api/filters?fs=' + fs + '&content=' + content).then(function (j) {
    RL_FB.items = (j && j.ok && j.items) || [];
    RL_FB.chosen = current && RL_FB.items.some(function (x) { return x.name === current; }) ? current : '';
    RL_FB.preview = RL_FB.chosen || (RL_FB.items[0] && RL_FB.items[0].name) || '';
    rlFbFill();
    if (RL_FB.preview) rlFbPreview(RL_FB.preview);
    rlFbRender();
    var first = list.querySelector('[data-fb-name="' + (RL_FB.preview || '').replace(/"/g, '') + '"]');
    if (first) first.focus();
  }).catch(function () {
    D('rlFbList').innerHTML = '<div class=rlFbEmpty>' + esc(t('滤波器列表读取失败')) + '</div>';
  });
  dialog.showModal();
}
function rlFbApply() {
  var name = RL_FB.chosen;
  if (!name) return;
  var isNx = RL_FB.target === 'filterNx';
  var sp = D(isNx ? 'sp_filtNx' : 'sp_filt');
  var s = D(isNx ? 'sFiltNx' : 'sFilt');
  if (s) s.value = name;
  if (sp) {
    if (!Array.prototype.some.call(sp.options, function (o) { return o.value === name; })) sp.add(new Option(name, name));
    sp.value = name;
  }
  var studio = D(isNx ? 'rlStudioFilterNx' : 'rlStudioFilter1');
  if (studio) { RL_STUDIO.draft[isNx ? 'filterNx' : 'filter'] = name; rlStudioFlush().catch(function () {}); }
  else if (typeof saveSettings === 'function') saveSettings();
  if (D('rlFilterBrowser')) D('rlFilterBrowser').close();
  toast(tf('已选用滤波器：{0}', [name]));
}

// ---- 2026-11-15：滤波器下拉框的内联详情面板 ----
// 用户要求：点开下拉菜单时，在它右侧浮出与菜单同高的详情框，鼠标移到哪一项就显示哪一项。
// 原生 <select> 的弹出层由浏览器绘制，DOM 收不到 option 的 hover 事件，所以这里拦截
// mousedown（阻止原生弹层），用自绘列表 + 右侧详情替代；select 本身仍是表单值的唯一来源。
var RL_FP = { pop: null, sel: null, seq: 0, rows: [], idx: -1, outside: null, key: null };
function rlFpEnsureStyle() {
  if (D('rlFpStyle')) return;
  var st = document.createElement('style'); st.id = 'rlFpStyle';
  st.textContent =
    '.rlFpPop{position:fixed;z-index:99999;display:flex;gap:8px;align-items:flex-start}' +
    '.rlFpList{background:#141414;border:1px solid #3a3a3a;border-radius:8px;box-shadow:0 12px 40px #000a;padding:4px;overflow:auto;min-width:220px}' +
    '.rlFpGroup{font-size:10px;letter-spacing:.06em;color:#999;padding:8px 8px 4px;text-transform:uppercase}' +
    '.rlFpRow{padding:6px 8px;border-radius:6px;cursor:pointer;white-space:nowrap;overflow:hidden;text-overflow:ellipsis;font:12px/1.35 ui-monospace,Consolas,monospace;color:#ddd}' +
    '.rlFpRow:hover,.rlFpRow.on{background:#2a2a2a;color:#fff}' +
    '.rlFpDetail{background:#141414;border:1px solid #3a3a3a;border-radius:8px;box-shadow:0 12px 40px #000a;padding:10px 12px;overflow:auto}' +
    '.rlFpDetail .rlFbGrid{grid-template-columns:repeat(2,minmax(0,1fr))}' +
    '.rlFbDetailHead{display:flex;align-items:center;gap:10px;flex-wrap:wrap;margin-bottom:8px}.rlFbDetailHead h3{margin:0;font:600 18px/1.2 Georgia,serif}' +
    '.rlFbBadges{display:flex;gap:6px;flex-wrap:wrap}.rlFbBadges span{font-size:10px;padding:3px 7px;border:1px solid var(--line,#3a3a3a);border-radius:999px;color:var(--muted,#bbb)}' +
    '.rlFbGrid{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:6px;margin:6px 0 10px}' +
    '.rlFbCell{border:1px solid var(--line,#3a3a3a);border-radius:7px;padding:6px 7px;min-width:0}' +
    '.rlFbCell span{display:block;font-size:10px;color:var(--muted,#999);margin-bottom:2px}.rlFbCell b{font:600 12px/1.3 ui-monospace,Consolas,monospace;overflow-wrap:anywhere}' +
    '.rlFbCharts{display:grid;grid-template-columns:1fr 1fr;gap:8px}.rlFbCharts figure{margin:0;border:1px solid var(--line,#3a3a3a);border-radius:8px;padding:6px 7px 3px;min-width:0}' +
    '.rlFbCharts figcaption{font-size:10px;color:var(--muted,#999);margin-bottom:2px}.rlFbChart{display:block;width:100%;height:auto}' +
    'line.rlFbGrid{stroke:#5556;stroke-width:.6}.rlFbTick{font-size:8px;fill:#999}.rlFbMark{stroke-width:.8;stroke-dasharray:2 2;opacity:.8}.rlFbMarkText{font-size:7.5px;fill:#bbb}' +
    '.rlFbLine{fill:none;stroke:var(--acc,#d9b875);stroke-width:1.3;vector-effect:non-scaling-stroke}' +
    '.rlFbNote{font-size:10px;line-height:1.55;color:var(--muted,#999);margin:8px 0 0}' +
    '.rlFbLoading,.rlFbEmpty{padding:24px 8px;color:var(--muted,#999);font-size:12px;text-align:center}';
  document.head.appendChild(st);
}
function rlFpPreview(name) {
  var host = D('rlFpDetail');
  if (!host) return;
  // 空值 = “自动（推荐）”：/api/filter/detail 的 auto 条目会解析到真实预设。
  var key = name || 'auto';
  var it = RL_FB.cache[key];
  if (it) { host.innerHTML = rlFbDetailHtml(it); return; }
  host.innerHTML = '<div class=rlFbLoading>' + esc(t('读取中…')) + '</div>';
  var seq = ++RL_FP.seq;
  api('/api/filter/detail?name=' + encodeURIComponent(key)).then(function (j) {
    if (seq !== RL_FP.seq || !D('rlFpDetail')) return;
    if (!j || !j.ok || !j.item) { host.innerHTML = '<div class=rlFbEmpty>' + esc(t('读取失败：该滤波器没有详情数据')) + '</div>'; return; }
    RL_FB.cache[key] = j.item;
    host.innerHTML = rlFbDetailHtml(j.item);
  }).catch(function () {
    if (seq === RL_FP.seq && D('rlFpDetail')) host.innerHTML = '<div class=rlFbEmpty>' + esc(t('读取失败：网络或服务未响应')) + '</div>';
  });
}
function rlFpClose() {
  if (RL_FP.outside) { document.removeEventListener('mousedown', RL_FP.outside, true); RL_FP.outside = null; }
  if (RL_FP.key) { document.removeEventListener('keydown', RL_FP.key, true); RL_FP.key = null; }
  if (RL_FP.pop) { RL_FP.pop.remove(); RL_FP.pop = null; }
  RL_FP.sel = null; RL_FP.rows = []; RL_FP.idx = -1;
}
function rlFpPick(v) {
  var sel = RL_FP.sel;
  if (!sel) return;
  sel.value = v;
  sel.dispatchEvent(new Event('change', { bubbles: true }));
  rlFpClose();
}
function rlFpMove(delta) {
  if (!RL_FP.rows.length) return;
  var i = RL_FP.idx + delta;
  if (i < 0) i = RL_FP.rows.length - 1;
  if (i >= RL_FP.rows.length) i = 0;
  RL_FP.idx = i;
  for (var k = 0; k < RL_FP.rows.length; ++k) RL_FP.rows[k].classList.toggle('on', k === i);
  var row = RL_FP.rows[i], list = D('rlFpList');
  if (list) list.scrollTop = Math.max(0, row.offsetTop - list.clientHeight / 2);
  rlFpPreview(row.dataset.v);
}
function rlFpOpen(sel) {
  if (RL_FP.sel === sel) { rlFpClose(); return; }
  rlFpClose();
  rlFpEnsureStyle();
  var r = sel.getBoundingClientRect();
  var vw = window.innerWidth, vh = window.innerHeight;
  var w = Math.max(240, Math.round(r.width));
  var dw = Math.min(560, Math.max(340, vw - w - 48));
  var below = vh - r.bottom - 12, above = r.top - 12;
  var listH = Math.min(430, Math.max(220, Math.max(below, above)));
  var left = Math.round(r.left);
  if (left + w + 8 + dw > vw - 8) left = Math.max(8, vw - 8 - w - 8 - dw);
  var top = (below >= 300 || below >= above) ? Math.round(r.bottom + 4)
                                              : Math.max(8, Math.round(r.top - listH - 4));
  if (top + listH > vh - 8) listH = Math.max(160, vh - top - 8);
  var pop = document.createElement('div');
  pop.id = 'rlFpPop'; pop.className = 'rlFpPop';
  pop.style.left = left + 'px'; pop.style.top = top + 'px';
  pop.innerHTML = '<div class=rlFpList id=rlFpList></div><div class=rlFpDetail id=rlFpDetail></div>';
  document.body.appendChild(pop);
  RL_FP.pop = pop; RL_FP.sel = sel; RL_FP.rows = []; RL_FP.idx = -1;
  var list = D('rlFpList'), detail = D('rlFpDetail');
  list.style.width = w + 'px'; list.style.maxHeight = listH + 'px';
  detail.style.width = dw + 'px'; detail.style.maxHeight = listH + 'px';
  var seen = {}, seenLabel = {};
  function addRow(v, label) {
    // 设置页与音乐台可能各补一个空值“自动”，值或显示名重复都只保留第一次。
    if (seen[v] || seenLabel[label]) return;
    seen[v] = 1; seenLabel[label] = 1;
    var d = document.createElement('div');
    d.className = 'rlFpRow'; d.dataset.v = v; d.textContent = label; d.title = label;
    d.addEventListener('mouseenter', function () {
      for (var k = 0; k < RL_FP.rows.length; ++k) RL_FP.rows[k].classList.remove('on');
      d.classList.add('on'); RL_FP.idx = RL_FP.rows.indexOf(d);
      rlFpPreview(v);
    });
    d.addEventListener('click', function () { rlFpPick(v); });
    list.appendChild(d); RL_FP.rows.push(d);
    if (v === sel.value) RL_FP.idx = RL_FP.rows.length - 1;
  }
  for (var i = 0; i < sel.children.length; ++i) {
    var ch = sel.children[i];
    if (ch.tagName === 'OPTGROUP') {
      var g = document.createElement('div'); g.className = 'rlFpGroup'; g.textContent = ch.label; list.appendChild(g);
      for (var j = 0; j < ch.children.length; ++j) {
        var op = ch.children[j];
        if (op.tagName === 'OPTION') addRow(op.value, (op.textContent || '').trim());
      }
    } else if (ch.tagName === 'OPTION') {
      addRow(ch.value, (ch.textContent || '').trim());
    }
  }
  if (RL_FP.idx >= 0 && RL_FP.rows[RL_FP.idx]) {
    RL_FP.rows[RL_FP.idx].classList.add('on');
    list.scrollTop = Math.max(0, RL_FP.rows[RL_FP.idx].offsetTop - list.clientHeight / 2);
    rlFpPreview(RL_FP.rows[RL_FP.idx].dataset.v);
  } else if (RL_FP.rows[0]) {
    RL_FP.idx = 0; RL_FP.rows[0].classList.add('on'); rlFpPreview(RL_FP.rows[0].dataset.v);
  }
  RL_FP.outside = function (e) {
    if (RL_FP.pop && !RL_FP.pop.contains(e.target) && e.target !== sel) rlFpClose();
  };
  document.addEventListener('mousedown', RL_FP.outside, true);
  RL_FP.key = function (e) {
    if (!RL_FP.pop) return;
    if (e.key === 'Escape') { e.preventDefault(); e.stopPropagation(); rlFpClose(); }
    else if (e.key === 'ArrowDown') { e.preventDefault(); e.stopPropagation(); rlFpMove(1); }
    else if (e.key === 'ArrowUp') { e.preventDefault(); e.stopPropagation(); rlFpMove(-1); }
    else if (e.key === 'Enter') { e.preventDefault(); e.stopPropagation(); if (RL_FP.idx >= 0) rlFpPick(RL_FP.rows[RL_FP.idx].dataset.v); }
  };
  document.addEventListener('keydown', RL_FP.key, true);
}
function rlFpAttach(sel) {
  if (!sel || sel.dataset.rlFp === '1') return;
  sel.dataset.rlFp = '1';
  sel.style.cursor = 'pointer';
  sel.addEventListener('mousedown', function (e) { e.preventDefault(); rlFpOpen(sel); });
  sel.addEventListener('keydown', function (e) {
    if (e.key === 'ArrowDown' || e.key === 'Enter') { e.preventDefault(); rlFpOpen(sel); }
  });
}
function rlFpAttachAll() {
  ['rlStudioFilter1', 'rlStudioFilterNx', 'rlStudioSdmFilter1', 'rlStudioSdmFilterNx',
   'sp_filt', 'sp_filtNx', 'sp_sdmo1', 'sp_sdmoNx'].forEach(function (id) {
    var s = D(id); if (s) rlFpAttach(s);
  });
}
setInterval(function () { try { rlFpAttachAll(); } catch (e) {} }, 1200);

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
    ? (tf('已配置：{0}（模型 {1}）', [LLM.masked, LLM.model]))
    : '未配置 —— 填入 DeepSeek API Key 后点「保存 Key」';
  m.textContent = s + '；key 只存本机 rlplayer-llm.conf，不进安装包、界面从不回显明文' +
    (LLM.proxy ? '；配置里的 proxy= 行已保留' : '');
  llmModelRender();       // 模型那一行跟着配置刷新（2026-10-27 用户 261027-06 P1）
}
function llmRefresh() {
  return fetch('/api/llm').then(function (r) { return r.json(); }).then(llmRender)
    .catch(function () { var m = D('sp_llmmsg'); if (m) m.textContent = '读取配置失败（服务没响应）'; });
}
function llmPost(body, okMsg) {
  return fetch('/api/llm', { method: 'POST', body: body })
    .then(function (r) { return r.json(); })
    .then(function (j) {
      if (!j || !j.ok) { toast(tf('操作失败：{0}', [((j && j.error) || '未知错误')])); return j; }
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
    if (j && j.ok) m.textContent = tf('连接成功：模型 {0}，耗时 {1} ms（key 有效）', [j.model, j.latencyMs]);
    else m.textContent = tf('连接失败：{0}', [((j && j.error) || '未知错误')]);
  }).catch(function () {
    if (b) { b.disabled = false; b.textContent = '测试连接'; }
    if (m) m.textContent = '连接失败：服务没响应';
  });
}

/* 2026-10-27（用户 261027-06 P1）：模型列表 + 下拉选择。
   对应后端 GET /api/llm/models（永远 200：拉不到就 ok:false + error）。
   三条纪律：
     * 拉不到 ⟹ 自动切「手填输入框」并写出原因 —— 设置页绝不能因为网关没有 /models 就崩；
     * 列表里有当前 model ⟹ 直接选中它；没有 ⟹ 额外插一条「（当前值，不在列表中）」+ 黄字提示重选，
       但**不改**用户配置（不擅自把用户选的模型换掉）；
     * 选中即保存：POST /api/llm model=…（与保存 Key 同一条路径，写同一份 rlplayer-llm.conf）。 */
var LLM_MODELS = { tried: false, ok: false, list: [], current: '', inList: false, err: '', at: 0 };
function llmModelMsg(txt, warn) {
  var m = D('sp_llmmodmsg');
  if (!m) return;
  m.innerHTML = warn ? ('<span style="color:var(--warn,#e6a23c)">' + txt + '</span>') : txt;
}
function llmModelRender() {
  var sel = D('sp_llmmodel'), txt = D('sp_llmmodeltext');
  var cur = LLM.model || '';
  if (!sel || !txt) return;
  if (LLM_MODELS.ok && LLM_MODELS.list.length) {
    sel.style.display = '';
    txt.style.display = 'none';
    var h = '', has = false;
    for (var i = 0; i < LLM_MODELS.list.length; ++i) {
      var id = String(LLM_MODELS.list[i].id || '');
      if (!id) continue;
      if (id === cur) has = true;
      h += '<option value="' + esc(id) + '">' + esc(id) + (id === cur ? '　（当前）' : '') + '</option>';
    }
    if (!has && cur) h += '<option value="' + esc(cur) + '">' + esc(cur) + '　（当前值，不在列表中）</option>';
    sel.innerHTML = h;
    sel.value = cur;
    var n = LLM_MODELS.list.length;
    if (cur && !has) {
      llmModelMsg('已拉到 ' + n + ' 个模型，但当前用的 ' + esc(cur) +
                  ' 不在列表里 —— 建议在上面选一个（不改也能继续用）。', true);
    } else {
      llmModelMsg('已拉到 ' + n + ' 个模型' + (cur ? ('，当前 ' + esc(cur)) : '') +
                  ' · search / fetch 不受这里影响，始终用默认模型');
    }
    return;
  }
  // 降级：手填。值直接放在输入框里，用户可改可留空（留空 = 不动配置）
  sel.style.display = 'none';
  txt.style.display = '';
  if (!txt.value) txt.value = cur;
  if (LLM_MODELS.err) {
    llmModelMsg('拉取模型列表失败：' + esc(LLM_MODELS.err) +
                ' —— 可直接手填模型 id（回车生效），保持默认也可以。', true);
  } else if (LLM_MODELS.tried) {
    llmModelMsg('服务端没有返回任何模型 —— 可直接手填模型 id（回车生效）。', true);
  } else {
    llmModelMsg('模型列表读取中…（读不到也可以直接手填）');
  }
}
function llmModelsLoad(force) {
  var sel = D('sp_llmmodel');
  if (!sel) return Promise.resolve(null);
  if (!D('sp_llmmodeltext')) return Promise.resolve(null);
  if (force) llmModelMsg('正在重新拉取模型列表…');
  var url = '/api/llm/models' + (force ? '?refresh=1' : '');
  return fetch(url).then(function (r) { return r.json(); }).then(function (j) {
    LLM_MODELS.tried = true;
    LLM_MODELS.ok = !!(j && j.ok);
    LLM_MODELS.list = (j && j.models) || [];
    LLM_MODELS.inList = !!(j && j.currentInList);
    LLM_MODELS.err = (j && j.error) || '';
    LLM_MODELS.at = Date.now();
    if (j && j.current) { LLM.model = j.current; LLM_MODELS.current = j.current; }
    llmModelRender();
    return j;
  }).catch(function () {
    LLM_MODELS.tried = true;
    LLM_MODELS.ok = false;
    LLM_MODELS.list = [];
    LLM_MODELS.err = '服务没响应';
    llmModelRender();
    return null;
  });
}
function llmSaveModel(v) {
  v = String(v || '').replace(/^\s+|\s+$/g, '');
  if (!v) { toast('模型 id 不能为空'); llmModelRender(); return; }
  if (v === LLM.model) { toast(tf('当前已经在用 {0}', [v])); return; }
  llmPost('model=' + encodeURIComponent(v), tf('模型已切换为 {0}', [v])).then(function (j) {
    if (j && j.ok) { LLM.model = j.model || v; llmModelRender(); llmModelsLoad(false); }
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
  // Empty means no selected preset here; keep the persisted value unchanged.
  if (sel.id === 'sp_filt' && strVal === '') {
    for (var j = 0; j < sel.options.length; j++) {
      if (sel.options[j].value === '') sel.options[j].textContent = t('未选择滤波器');
    }
  }
  sel.value = strVal;
}

function presetCardHtml() {
  var dev = (_capsCache && _capsCache.device) || {};
  var name = dev.name || '检测中…';
  var maxRate = dev.maxRate ? (dev.maxRate + ' Hz') : '- Hz';
  var dsdStr = '-';
  if (dev.dsdNative !== undefined) {
    dsdStr = dev.dsdNative ? (tf('是（最高 {0}）', [(dev.dsdNativeMax || 'dsd256')])) : t('否');
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
        + tf('当前设备：{0} · 最高 {1} · 原生 DSD {2}', [esc(name), esc(maxRate), esc(dsdStr)])
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
  var dsdStr = dev.dsdNative ? (tf('是（最高 {0}）', [(dev.dsdNativeMax || 'dsd256')])) : t('否');

  if (D('presetCapsInfo')) {
    D('presetCapsInfo').textContent = tf('当前设备：{0} · 最高 {1} · 原生 DSD {2}', [name, maxRate, dsdStr]);
  }
  var pcmTargetRate = dev.maxRate ? dev.maxRate : '384000';
  if (D('presetPcmSub')) {
    D('presetPcmSub').textContent = tf('升到设备最高档（当前 {0}），tight 滤波 + 每相位 512 抽头', [pcmTargetRate]);
  }
  var dsdTarget = (dev.dsdNative && dev.dsdNativeMax) ? dev.dsdNativeMax : (dev.dopMax || dev.dsdNativeMax || 'dsd256');
  if (D('presetDsdSub')) {
    D('presetDsdSub').textContent = tf('升到 {0}，EC-7 调制器', [dsdTarget]);
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
  if(applied && (applied.clearProcessing===true || profile==='bitperfect')) {
    ADV.eq='';ADV.ir='';ADV.irgpu=0;ADV.atten=0;ADV.transition=0;ADV.phase='';S.loud=false;
    for(var ci=0;ci<8;++ci)ADV['trim'+ci]=0;
    ['sp_eq','sp_ir','sp_phase'].forEach(function(id){if(D(id))D(id).value='';});
    ['sLoud','sp_loud'].forEach(function(id){if(D(id))D(id).checked=false;});
    if(profile==='bitperfect') {
      ADV.roomcorrauto=0;ADV.cleanup=0;ADV.crossfeed=0;ADV.dacbits=0;ADV.upnpUpsample=0;ADV.upnpForceRate=0;ADV.rgmode='off';
      if(D('sp_roomauto'))D('sp_roomauto').checked=false;
      if(D('sp_rgmode'))D('sp_rgmode').value='off';
      clearTimeout(PEQ_TIMER);if(PEQ_STATE)PEQ_STATE.bypass=true;if(D('peqBypass'))D('peqBypass').checked=true;
      peqLoadState();syncProcessingControls();
    }
  }
  ADV.sdmFilt='';ADV.sdmFiltNx='';ADV.taps=0;if(D('sp_taps'))D('sp_taps').value='0';
  if(applied&&applied.modfamily)ADV.modfamily=applied.modfamily;
  if(applied&&applied.modorder)ADV.modorder=Number(applied.modorder);
  if(applied&&applied.directsdm!==undefined)ADV.directsdm=Number(applied.directsdm)?1:0;
  if (!applied || typeof applied !== 'object') return;

  var r = applied.rate !== undefined ? applied.rate : applied.sRate;
  // 2026-10-29（261029-02 新功能1）：原音直通必须「保持源采样率」才可能 bit-perfect。
  //   旧值 'follow' 的真实语义是「跟随系统混音率」（rate_pick.h:113），
  //   源 48k + 声卡默认 44.1k 时会重采样 ⟹ 永远不 bit-perfect。
  if (r === undefined && profile === 'bitperfect') r = 'src';
  if (r !== undefined) {
    setSelectValue(D('sRate'), r);
    setSelectValue(D('sp_rate'), r);
  }
  if (applied.rateFamilyPolicy !== undefined && D('sRateFamilyPolicy')) {
    D('sRateFamilyPolicy').value = normalizeRateFamilyPolicy(applied.rateFamilyPolicy);
  }
  syncRateFamilyPolicy();

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
    var vNum = volClamp(Number(vol) || 0);
    S.vol = vNum;
    S.volDb = vNum;
    if (D('vol')) D('vol').value = String(vNum);
    if (D('volDb')) D('volDb').textContent = vNum.toFixed(1) + ' dB';
    syncVolUi(vNum);
    api('/api/volume?db=' + encodeURIComponent(vNum), '').catch(function () {});
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
    var s2sMode = (s2s === true || s2s === 1 || s2s === '1') ? '1'
                : (s2s === false || s2s === 0 || s2s === '0') ? '0' : 'auto';
    if (D('s2s')) D('s2s').value = s2sMode;
    if (D('sp_2s')) D('sp_2s').value = s2sMode;
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

  var saved=saveSettings(true);
  tick();
  if (typeof syncLiveSignal === 'function') syncLiveSignal();
  if (typeof updateSignalChain === 'function') updateSignalChain();
  if (D('modal') && D('modal').classList.contains('on')) showChain();
  return saved;
}

function applyPreset(profile, options) {
  if (!ensurePlaybackSettings()) return Promise.resolve({ok:false,errorCode:'PLAYBACK_SETTINGS_NOT_READY'});
  var bBp = D('presetBitPerfect'), bPcm = D('presetPcm'), bDsd = D('presetDsd');
  if (bBp) bBp.disabled = true;
  if (bPcm) bPcm.disabled = true;
  if (bDsd) bDsd.disabled = true;

  clearTimeout(window._setT);clearTimeout(PEQ_TIMER);
  return api('/api/preset/apply?profile=' + encodeURIComponent(profile)+'&keep_volume=1'+(options&&options.preserveProcessing?'&preserve_processing=1':''), '').then(function (j) {
    if (bBp) bBp.disabled = false;
    if (bPcm) bPcm.disabled = false;
    if (bDsd) bDsd.disabled = false;

    if (!j || !j.ok) {
      toast((j && (j.error || j.message)) || '应用预设失败');
      return j;
    }

    return Promise.resolve(j.applied ? applyPresetControls(j.applied, profile) : null).then(function(saved){
    if(saved && !saved.ok){if(saved.stale)toast(t('当前组已切换，请重新操作'));return saved;}
    if (j.notes && Array.isArray(j.notes) && j.notes.length) {
      j.notes.forEach(function (note, idx) {
        setTimeout(function () { toast(note); }, idx * 1200);
      });
    } else {
      toast(tf('已应用 {0} 预设', [(profile === 'bitperfect' ? '原音直通' : (profile === 'pcm' ? 'PCM 升频' : 'DSD 升频'))]));
    }
    return j;
    });
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
  ++CLEANUP_UI.generation;
  var filts = D('sFilt') ? D('sFilt').innerHTML : '';
  // Nx 下拉必须带上"跟随 1x"（空值），否则重建后浏览器退回第一项，一保存就把 Nx 改掉
  var filtsNx = '<option value="">跟随 1x（不单独指定）</option>' + filts;
  var shaps = D('sShap') ? D('sShap').innerHTML : '';
  var BK = ADV.backend || 'wasapi';
  var CH = String(ADV.channels || 0);
  var h = '';
  // Static labels use their source text. The DOM translator records each source
  // so the same form can change language without replacing unfinished inputs.

  // 2026-10-26（261026-01 #2）：官方推荐预设卡片**移进「输出」面板**（用户：不要做在所有东西外面）。
  // ---------- 页签（对齐 HQPlayer：Inputs / Outputs / PCM / SDM / Advanced） ----------
  h += '<div class=stabs>'
     + '<button class="stab on" data-g=in>输入</button>'
     + '<button class=stab data-g=out>输出</button>'
     + '<button class=stab data-g=adv>高级</button>'
     // #6（2026-10-27 用户 261026-09）：EQ 页内容臃肿 ⟹ 拆成 EQ / 声音校准 两个页签。
     + '<button class=stab data-g=cal>声音校准</button>'
     + '<button class=stab data-g=meta>元数据</button>'
     // 2026-10-27（261027-04 #4）：**删除**「分析」页签 —— 用户原话「设置-分析，整个功能页不应该
     // 放到这里，直接将分析放到正在播放的那首歌音乐上」⟹ 三类结果（动态/音质/假无损）改挂在单曲页
     // 的 #npAnal 分析条上，播放即自动跑一次。版本对比仍是曲目右键的「对比版本…」（功能没删）。
     // 2026-10-24（用户 261024-01 #5）：「流媒体」从「高级」拆出来的独立页签。
     + '<button class=stab data-g=stream>流媒体</button>'
     // 关于与更新（261027-02 #5）：当前版本 / 检查 / 增量下载 / 重启完成。
     // 「诊断」独立成页：用户 2026-10-22 功能3 明确要求不要塞在元数据里。
     // 开发期它是必需品（报错时要拿现场），所以与输入/输出/PCM/SDM/高级 平级。
     + '<button class=stab data-g=diag>诊断</button>'
     + '<button class=stab data-g=upd>关于与更新</button>'
     + '</div>';

  // ---------- 输出 ----------
  // 2026-10-26（261026-01 #2）：官方推荐预设卡片放在这里（用户要求「做到输出里面」）
  h += '<div class="spane" data-g=out>'
     // 2026-10-26（261026-01 #2）：官方推荐预设放在「输出」面板最上方（用户要求「做到输出里面」）。
     + sgroup('输出后端', '独占 = bit-perfect；共享 = 兼容性最好（虚拟机 / 远程桌面）',
         // 2026-11-15：原「切到 bit-perfect 直通」按钮已删除。A–D 四组的后端/滤波/EQ/音量
         // 现在统一由数字音乐台管理，这里再放一套会直接改坏当前组（ASIO/DSD 组被改成 WASAPI PCM）。
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
      + chk('sp_upnp_upsample', '投放前升频', !!ADV.upnpUpsample, '目标采样率与滤波器使用 PCM 页设置；保持源采样率时不会升频。',true)
      + chk('sp_upnp_force','强制使用未声明的目标率',!!ADV.upnpForceRate,'仍遵守同族整数倍和上限；设备可能拒播。',true)
      + sel('sp_upnp_ceiling','投放采样率上限',[88200,96000,176400,192000,352800,384000,705600,768000].map(function(r){return opt(r,r/1000+' kHz',ADV.upnpRateCeiling);}).join(''),'只选择位深、声道与速率同时匹配的设备格式。')
      + '<div class=hint>'+esc('先在本机准备完整 WAV，再向设备供源；准备时可以停止取消。')+'</div>'
      + '<div class=hint id=sp_upnp_msg>播放 / 切歌自动投放到该渲染器（与下方「投放」共用会话）</div>'
      + '</div>'
       // 2026-11-15：一键直通按钮已删除，这里不再有第二套控制入口。
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
         sel('sp_rate', '输出采样率', D('sRate').innerHTML, '自动 = 按采样率族策略选择设备支持的最高档位；同族指 44.1 kHz 系列或 48 kHz 系列')
       + sel('sp_rateFamilyPolicy', '采样率族策略', D('sRateFamilyPolicy').innerHTML,
             '同族优先：自动档只选源族，手动目标保留明确选择；严格同族：手动跨族也拒绝；兼容重采样：允许自动跨族回退。')
       + '<div class="sfld wide"><div class=hint id=sp_rateFamilyHint></div></div>'
       + sel('sp_bits', '输出位深（PCM）',
             opt(0, '24 bit', ADV.bits) + opt(16, '16 bit', ADV.bits) + opt(32, '32 bit', ADV.bits),
             'DSD 输出恒为 1 bit，此项只在 PCM 模式生效'))
     + sgroup('升频与滤波', 'HQPlayer 的 poly-sinc 家族',
         sel('sp_filt', '1x 滤波器（44.1 / 48 kHz 音源）', filts, '播放 44.1 / 48 kHz 文件时使用')
       + sel('sp_filtNx', 'Nx 滤波器（88.2 kHz 及以上音源）', filtsNx,
             '播放 88.2 / 96 / 176.4 kHz 等文件时使用；留空 = 与 1x 相同')
       + '<div class="sfld wide" style="display:flex;gap:8px;flex-wrap:wrap">'
       + '<button type=button class=btn id=sp_filt_browse>' + esc('滤波器库 · 1x 详情') + '</button>'
       + '<button type=button class=btn id=sp_filtNx_browse>' + esc('滤波器库 · Nx 详情') + '</button>'
       + '</div>'
       + sel('sp_phase', '相位（覆盖滤波器预设的相位）',
             opt('', '跟随预设（用所选滤波器自带的相位）', ADV.phase) +
             opt('linear', '强制线性相位', ADV.phase) +
             opt('minimum', '强制最小相位', ADV.phase) +
             opt('intermediate', '强制混合相位', ADV.phase),
             '「跟随预设」= 不覆盖滤波器自带相位。强制项会覆盖预设：' +
             '线性：各频率延迟一致；最小：能量更靠前；混合：组合线性与最小相位响应。强制覆盖会改变滤波器，预设测量值仅供参考。')
       + '<div class="sfld wide"><div class=hint id=sp_phasemsg></div></div>'
       + num({ id: 'sp_taps', label: '原型抽头数（0 = 按预设自动）' }, ADV.taps, 0, 2097152, 1024,
             '多相分解后每一相的抽头数 ≈ 抽头数 ÷ 升频比；0 = 用所选滤波器预设自带长度')
       + num({ id: 'sp_atten', label: '阻带衰减（dB，0 = 用预设）' }, ADV.atten, 0, 300, 10,
             '目标阻带深度；实际能不能达到还要看抽头数')
       + num({ id: 'sp_transition', label: '过渡带占比（0 = 用预设）' }, ADV.transition, 0, 0.45, 0.005,
             '截止点相对理想奈奎斯特回退的比例，例如 0.04 = 回退 4%')
       + sel('sp_2s', '两段式升频',
             opt('auto', '自动（推荐）', D('s2s').value) + opt('1', '开启', D('s2s').value) + opt('0', '关闭', D('s2s').value),
             '自动：PCM 按滤波器预设，DSD 使用级联。开启或关闭会覆盖自动选择。'))
     + sgroup('录音噪声清理','在输入 PCM 域自适应降噪，可与 PCM 升频或 DSD 调制组合；逐位 DSD 不经过此处理。',
         chk('sp_cleanup','启用录音噪声清理',!!ADV.cleanup,'默认关闭。降低噪声会改变样本，不能恢复录音中已丢失的信息。',true)
        + sel('sp_cleanup_noise_mode','噪声底模式',opt('auto','自动估计（推荐）',ADV.cleanupAuto===0?'manual':'auto')+opt('manual','手动',ADV.cleanupAuto===0?'manual':'auto'),'自动模式仅从可信安静片段估计；证据不足时暂不降噪。手动值会保留。')
       + sel('sp_cleanup_strength','清理强度',opt(.25,'轻柔',ADV.cleanupStrength)+opt(.5,'标准',ADV.cleanupStrength)+opt(.75,'强力',ADV.cleanupStrength),'强度范围 0～1；更高强度可能影响微弱细节。')
        + num({id:'sp_cleanup_noise',label:'手动噪声底（dBFS）'},ADV.cleanupNoiseDb,-120,-60,.1,'仅手动模式使用；自动模式保留此值，不把它当作测量结果。')
        + '<div class="sfld wide"><label>' + esc('清理实况') + '</label><div class=hint id=sp_cleanup_status role=status></div><button type=button class=btn id=sp_cleanup_refresh>' + esc('刷新清理状态') + '</button></div>')
     + sgroup('耳机交叉馈送', '耳机交叉馈送会改变声像和频响；默认关闭。',
         chk('sp_crossfeed','启用耳机交叉馈送',!!ADV.crossfeed,'只用于两声道输入与两声道输出；不兼容逐位直通。',true)
       + num({id:'sp_crossfeed_freq',label:'交叉馈送截止频率（Hz）'},ADV.crossfeedFreq,300,1200,10,'低于此频段的信号会少量送入另一声道。')
       + num({id:'sp_crossfeed_level',label:'交叉声道衰减（dB）'},ADV.crossfeedLevel,3,15,.5,'值越大效果越轻；12 dB 轻度，6 dB 标准。'))
     + sgroup('抖动与保护', '按实际输出采样率与位深选择',
         sel('sp_shaper', 'PCM 抖动 / 噪声整形', shaps, 'auto = 按实际输出采样率与位深选择')
       + sel('sp_dacbits','DAC 有效位深',opt(0,'自动（跟随输出容器）',ADV.dacbits)+Array.from({length:25},function(_,i){return opt(i+8,(i+8)+' bit',ADV.dacbits);}).join(''),'仅在已知 DAC 精度时手动设置；18/20 bit 可装入 24/32 bit 容器，不改变驱动传输位深。')
       + '<div class="sfld wide"><div id=sp_shaper_hint class=hint role=status></div><button type=button class=btn id=sp_shaper_help>'+esc(t('抖动说明'))+'</button></div>'
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
             '前提：后端 ASIO。DSD 源走逐位直通（音量 0 dB、无 EQ / 卷积）；PCM 源会先经 SDM 调制再送 1-bit，音量 / EQ / 升频照常生效；' +
             'DSD 源支持同族整数倍率转换；驱动拒绝原生 DSD 时自动回落 DoP，并在信号链里标出')
       // 2026-10-24（用户 261024-01 #12）：这两个控件不再灰掉（见 applyDsdState）。
       // 当前输出是 PCM 时，改这里会自动把输出模式联动到 DSD —— 一句常驻说明交代清楚。
       + '<div class="sfld wide" id=sp_dsdmodenote style="display:none"><div class=hint>' +
         '当前输出 PCM：改动比特率 / 封装会自动把输出切到 DSD（SDM）' + '</div></div>'
       + sel('sp_modfamily', '调制器族',
              opt('dsd', 'DSD 标准（Butterworth NTF，1-bit）', ADV.modfamily || 'dsd') +
              opt('chebyshev', 'Chebyshev（可配带外增益与截止，1-bit 保护运行于 5 阶）', ADV.modfamily || 'dsd') +
              opt('ec', 'EC 误差反馈（频带零点扩展消除）', ADV.modfamily || 'dsd') +
              opt('ahm', 'AHM 算法（当前为 1-bit DSD 输出）', ADV.modfamily || 'dsd'),
              '多种 NTF 滤波与量化架构；AHM 当前为 1-bit DSD 输出。真正 6/8-bit 多电平可用随包离线导出工具 rlmultilevel；该文件不直接发送到 DoP / ASIO DAC')
        + sel('sp_modorder', '调制器阶数',
             opt(5, '5 阶（请求值）', ADV.modorder || 5) + opt(7, '7 阶（请求值）', ADV.modorder || 5),
             '实际阶数由调制器族、NTF 稳定性与 OSR 质量门决定；以信号链显示为准')
       + num({ id: 'sp_modcut', label: '噪声整形拐点（kHz，0 = 100）' }, ADV.modcut, 0, 200, 5,
             '0 使用已验证的默认 100 kHz；非 EC 的 1-bit 输出只允许 0 或 100～200 kHz，低值因已证音频带失真而拒绝，绝不静默改值；EC 按 Hinf 综合，不使用此值')
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
       + '</select></div>'
       // 2026-10-30（用户 261029-03 第 1/2/3 条）：界面语言。
       // 「跟随系统」是默认值：后端按 Windows 显示语言（GetUserDefaultUILanguage）解析，
       // 换一台机器不用改设置。首批 7 种；没译到的词条回落中文（见 i18n 注释）。
       + '<div class=sfld><label>界面语言' + tip('默认「跟随系统」：按 Windows 的显示语言自动选择。切换立即生效并写入设置。没翻译到的词条保持中文。') + '</label><select id=sp_uilang>'
       + opt('auto', '跟随系统（默认）', ADV.uiLang || 'auto')
       + opt('zh', '中文', ADV.uiLang || 'auto')
       + opt('en', 'English', ADV.uiLang || 'auto')
       + opt('ja', '日本語', ADV.uiLang || 'auto')
       + opt('fr', 'Français', ADV.uiLang || 'auto')
       + opt('ru', 'Русский', ADV.uiLang || 'auto')
       + opt('es', 'Español', ADV.uiLang || 'auto')
       + opt('ar', 'العربية', ADV.uiLang || 'auto')
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
      // 2026-10-27（261027-04 #4）：新增「音频分析」组 —— 用户要求「自动分析要能被关掉」。
      // 关闭自动分析后仍保留已经缓存的结果。
      + sgroup('音频分析', '正在播放那首歌的动态 / 音质 / 假无损，展示在单曲页；播放时自动分析一次',
          // 2026-10-28（用户要求）：**模块总开关**。
          // 关掉后：单曲页那块框整个消失（包括"自动分析已关闭"的提示框）、右键菜单没有分析入口、
          // 不再发任何 /api/health 请求。与下面「播放时自动分析」的区别是"要不要这个模块"vs"要不要自动跑"。
          chk('sp_anlmaster', '启用音频分析模块', ANL.master !== 0,
              '总开关。关掉后单曲页不再显示分析卡（包括那个"自动分析已关闭"的提示框）、' +
              '右键菜单不再有「查看分析」入口、也不会再向后台请求任何分析数据。' +
              '如果你只是不想每次播放都自动跑、但仍想手动看结果，用下面的「播放时自动分析」。') +
          chk('sp_autoanl', '播放时自动分析', ANL.auto !== 0,
              '开启后会自动分析当前曲目并缓存结果。关闭后仍可查看已有结果。')
        + '<div class="sfld wide"><div class=hint id=sp_anlmsg>' +
            analysisAutoStatus() + '</div></div>')
      + '</div>';
  // EQ       = 20 段 PEQ + 耳机 EQ（OPRA）      —— 手调，改的是"音色"
  // 声音校准 = 房间校正向导 + 校正文件（EQ/IR） —— 测量与外部文件，改的是"系统误差"
  h += '<div class=spane data-g=eq>' + eqSettingsHtml() + '</div>';
  h += '<div class=spane data-g=cal>' + calSettingsHtml() + '</div>';
  // ---------- 分析（2026-10-27 261027-04 #4）：**整块删除** ----------
  // 三类结果（动态 / 音质 / 假无损）已搬到单曲页 #npAnal（见 anlPaint）；
  // 「版本对比」保留原入口（曲目右键 →「对比版本…」→ openCompareModal）。

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
  // 2026-10-27（261027-02 #1）：用户要求**删除「导出诊断包」这一栏**，功能合并进下面的一键反馈。
  // 原话：「设置-诊断，导致诊断包，这一栏删除，功能合并到下面一键反馈，点击"提交反馈"自动发送诊断包到我的GITHUB上」
  // ⟹ 诊断包内容（版本/系统/CPU/GPU/设备/设置/日志尾）本来就由 feedback.h 自动采集并脱敏后随反馈一起上传，
  //   所以这里不再单独给「导出 zip / 只要 JSON」两个按钮；后端 /api/diagzip 与 /api/diag 端点保留（CLI 与排障仍可用）。
  h += '<div class=spane data-g=diag>'
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
       + '<button class="btn" id=sp_fbpreview>先看会发送什么</button>'
       + '<span style="font-size:11.5px;color:var(--mut2)" id=sp_fbmsg></span></div>'
       // 预览框与结果区放在 flex 行之外，各自整行，避免被挤成两列
       + '<div id=sp_fbpreviewbox style="display:none;width:100%;box-sizing:border-box;margin-top:12px;padding:14px 16px;border-radius:10px;'
       + 'background:rgba(107,138,253,.07);border:1px solid rgba(107,138,253,.22);font-size:12.5px;line-height:1.9;'
       + 'color:var(--mut);word-break:break-word"></div>'
       + '<div class=hint id=sp_fbresult style="white-space:pre-wrap;word-break:break-all"></div>')
     + '</div>';

  // ---------- 关于与更新（261027-02 #5）----------
  // 用户原话：「主要是有个在线更新功能，未来我们WIN版本更新，也需要这个」+「尽量做成增量更新」。
  // 本页只做四件事：说清当前版本 / 检查 / 增量下载 / 重启完成。
  // ★ 必须明确写「更新不会动你的数据」——这是用户最可能的顾虑，且清单与后端受保护清单同源。
  h += '<div class=spane data-g=upd>'
     + sgroup('版本', '增量更新：只下 sha256 与本机不一致的文件',
         '<div class="sfld wide" style="display:flex;align-items:center;gap:12px;flex-wrap:wrap">'
       + '<div style="font-size:19px;font-weight:600;letter-spacing:.4px">v<span id=sp_updcur>—</span></div>'
       + '<button class="btn pri" id=sp_updcheck>检查更新</button>'
       + '<span style="font-size:12px;color:var(--mut2)" id=sp_updstate>点「检查更新」看看有没有新版本</span>'
       + '</div>'
       + '<div class="sfld wide hint" style="margin-top:8px;line-height:1.6">'
       + '更新只替换<b>程序文件</b>（RLPlayer.exe / rtaweb.exe / rtagpu*.dll / tools / 其余代码资产），'
       + '每个文件都校验 sha256。下面这些<b>一个字节都不会改</b>：'
       + 'rlplayer-settings.conf · rlplayer-library.conf · rlplayer-llm.conf · rlplayer-feedback.conf · '
       + 'rlplayer-playlist.txt · rlplayer-savedqueues.conf · rlplayer-favs.json · data\\（库索引/封面缓存/房间校正集） · '
       + 'covercache\\ · WebView2Runtime\\ · playlists\\。'
       + '跨大版本或 WebView2 更新时，请按提示通过百度网盘（国内）或 GitHub（国外）手动安装完整版。'
       + '</div>'
       + '<div class="sfld wide" id=sp_updcard style="display:none;margin-top:14px">'
       + '<div class=meta-card>'
       + '<div class=mh>新版本 <b id=sp_updver>—</b><span style="font-weight:400;color:var(--mut2);font-size:11.5px" id=sp_updsize></span></div>'
       + '<div class=hint id=sp_updplan style="margin-top:4px"></div>'
       + '<div class=hint id=sp_updnotes style="white-space:pre-wrap;max-height:170px;overflow:auto;margin-top:6px"></div>'
       + '<div class=hint id=sp_updfiles style="margin-top:6px;font-family:ui-monospace,Consolas,monospace;font-size:11px;white-space:pre-wrap;max-height:120px;overflow:auto"></div>'
       + '<div style="margin-top:10px;height:6px;border-radius:4px;background:var(--line);overflow:hidden;display:none" id=sp_updbarwrap>'
       + '<div id=sp_updbar style="height:100%;width:0%;background:var(--acc);transition:width .25s"></div></div>'
       + '<div class=hint id=sp_updmsg style="margin-top:8px"></div>'
       + '<div style="display:flex;gap:8px;margin-top:10px;flex-wrap:wrap">'
       + '<button class="btn pri" id=sp_upddl>下载更新</button>'
       + '<button class="btn pri" id=sp_updcn style="display:none">国内下载：百度网盘</button>'
       + '<button class=btn id=sp_updglobal style="display:none">国外下载：GitHub</button>'
       + '<button class=btn id=sp_updapply style="display:none">立即更新并重启</button>'
       + '<button class=btn id=sp_updskip style="font-size:12px">跳过此版本</button>'
       + '</div></div></div>')
       // 2026-10-28（2.0 正式版）：关于本项目。用户要求「开发者信息一定要整合进去」，
       // 放这里而不是只在 README —— 用户遇到问题第一眼会来「关于」页找联系方式。
       + sgroup('关于本项目', 'RLPlayer 2.0 · GPL-3.0 开源',
           '<div class="sfld wide" style="line-height:1.75;font-size:12.5px">'
         + '<div><b>项目开发者</b>　RainLain、猫与小行星</div>'
         + '<div><b>协助开发者</b>　聆听风暴</div>'
         + '<div><b>媒体与美术</b>　Gosed</div>'
         + '<div><b>测试鸣谢</b>　Miyu、正在下载99%</div>'
         + '<div style="margin-top:8px"><b>官方主页</b>　'
         + '<a href="#" class="eflink" data-url="https://rlplayer.rainlain.com">rlplayer.rainlain.com</a>'
         + '</div>'
         // 2026-11-02（用户）：Q 群只在中文界面显示；其他语种整行隐藏（切语言即时生效）
         + '<div class="zh-only"><b>粉丝和反馈 Q 群</b>　'
         + '<a href="#" class="eflink" data-url="https://qm.qq.com/q/895816505">895816505</a></div>'
         + '<div style="margin-top:8px;color:var(--mut2);font-size:11.5px">'
         + '本程序以 GNU 通用公共许可证第 3 版（GPL-3.0-or-later）发布；'
         + '内置第三方组件（ffmpeg / AMD HIP / NVIDIA CUDA / WebView2 / OPRA）各自遵循原始许可，'
         + '详见程序目录下的 THIRD-PARTY-NOTICES.txt。</div>'
         + '</div>')
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
           tip(tf('只存本机 build{0}rlplayer-llm.conf；不进安装包，界面永不回显明文', [SEP])) + '</label>'
       + '<input type=password id=sp_llmkey autocomplete=off spellcheck=false placeholder="sk-…（粘贴后点保存，只显示掩码）" style="width:100%">'
       + '<div class=hint id=sp_llmmsg style="margin-top:6px">读取配置中…</div>'
       + '<div style="display:flex;gap:8px;margin-top:8px;flex-wrap:wrap">'
       + '<button class="btn pri" id=sp_llmsave>保存 Key</button>'
       + '<button class=btn id=sp_llmtest>测试连接</button>'
       + '<button class=btn id=sp_llmclear>清除 Key</button></div>'
       // 2026-10-27（用户 261027-06 P1）：**模型可选** —— 拉取当前列表 + 下拉选择 + 手填兜底。
       //   用户原话：「允许用户拉取当前模型列表，并选择使用哪个模型。不再强制指定 deepseek v4.1 flash。
       //             但是 fetch 和 search 则保持默认。」
       //   ① 有列表：<select> 列出服务端全部 id（当前值一定在里面并标注「当前」）；选中即保存；
       //   ② 拉不到（断网 / 401 / 网关没有 /models）：自动切成手填输入框并写明原因 —— 设置页绝不崩；
       //   ③ 列表拉到了但当前 model 不在里面：黄字提示「建议重选」，但不擅自改动用户配置。
       + '<div class="mh sub" style="margin-top:14px">模型' +
           '<span class=hint style="font-weight:400">search / fetch 始终用默认模型</span></div>'
       + '<div style="display:flex;gap:8px;margin-top:4px;align-items:center">'
       + '<select id=sp_llmmodel style="flex:1;min-width:0"></select>'
       + '<button class=btn id=sp_llmmodels title="重新拉取可用模型列表（GET {base}/models；拉不到也可以手填）">刷新列表</button>'
       + '</div>'
       + '<input id=sp_llmmodeltext type=text autocomplete=off spellcheck=false placeholder="模型 id（例如 deepseek-chat）" style="width:100%;margin-top:6px;display:none">'
       + '<div class=hint id=sp_llmmodmsg style="margin-top:6px">模型列表读取中…</div>'
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
     + '<span style="font-size:11.5px;color:var(--mut2)">播放参数在停止后重新播放时使用</span></div>';

  D('setpage').innerHTML = h;
  D('setpage').oninput = function(event) {
    if(event.target.id)event.target.dataset.rlEdited='1';
  };

  bindPresetCard();
  if (D('sp_filt_browse')) D('sp_filt_browse').onclick = function () { rlFilterBrowserOpen('filter'); };
  if (D('sp_filtNx_browse')) D('sp_filtNx_browse').onclick = function () { rlFilterBrowserOpen('filterNx'); };
  loadPresetCaps(false);

  // P1.5：源开关的绑定不在上面的静态清单里 —— 重建 HTML 后逐源重新绑定
  //（/api/providers 已到则用缓存渲染+绑定；没到则等 fillMetaProviders 到达时补填）。
  renderMetaSrcRows();

  // 设置页里的下拉是复制的快捷面板 HTML，重建后浏览器会退回第一项。
  // 不同步回来，打开设置页再点保存就会把用户的选择悄悄改成第一项。
  // #5（261026-09）：sp_rep / sp_shuf 已删（见 sgroup('播放') 处的逐项核对），
  // 这里只剩还活着的镜像对。
  [['sp_filt', 'sFilt'], ['sp_filtNx', 'sFiltNx'], ['sp_shaper', 'sShap'],
   ['sp_rate', 'sRate'], ['sp_rateFamilyPolicy', 'sRateFamilyPolicy'], ['sp_buffer', 'sBuf'],
   ['sp_loud', 'sLoud'], ['sp_isp', 'sIsp'], ['sp_2s', 's2s']].forEach(function (pair) {
    var a = D(pair[0]), b = D(pair[1]);
    if (!a || !b) return;
    if (pair[0] === 'sp_filt') { setSelectValue(a, b.value); return; }
    if (a.type === 'checkbox') { a.checked = b.checked; return; }
    if (a.tagName === 'SELECT') {
      for (var i = 0; i < a.options.length; i++) {
        if (a.options[i].value === b.value) { a.value = b.value; return; }
      }
    } else { a.value = b.value; }
  });
  syncRateFamilyPolicy();
  if (D('sp_shaper_help')) D('sp_shaper_help').onclick = rlOpenShaperInfo;
  if (D('sp_shaper')) D('sp_shaper').addEventListener('change',function(){if(D('sShap'))D('sShap').value=this.value;rlRefreshShaperGuidance();});
  rlRefreshShaperGuidance();
  if (D('sp_rateFamilyPolicy')) D('sp_rateFamilyPolicy').onchange = function () {
    D('sRateFamilyPolicy').value = normalizeRateFamilyPolicy(this.value);
    syncRateFamilyPolicy();
    saveSettings();
  };
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
      D('setpage').dataset.rlSettings='detail';
      Array.prototype.forEach.call(document.querySelectorAll('#setpage .stab'), function (x) {
        x.classList.toggle('on', x === b);
      });
      Array.prototype.forEach.call(document.querySelectorAll('#setpage .spane'), function (p) {
        p.classList.toggle('on', p.dataset.g === b.dataset.g);
      });
      if (b.dataset.g === 'pcm') refreshCleanupStatus();
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

  // 2026-10-30（用户 261029-03）：界面语言切换（立即生效 + 落盘）
  if (D('sp_uilang')) D('sp_uilang').onchange = function () {
    var v = this.value || 'auto';
    ADV.uiLang = v;
    i18nSetLang(v, true);
  };
  // 输出后端 / 声道 / 微调
  D('sp_backend').onchange = function () {
    ADV.backend = this.value;
    loadRates();
    saveSettings();
    backendHint();
    applyBackendState();
    loadDeviceList();        // ASIO 列驱动，WASAPI 列端点，NAA/Null 没有本地设备
    // 261024-01 #9：切到 UPnP 时自动发现目标渲染器
    if (this.value === 'upnp') scanCastDevices(true);
  };
  // 261024-05：NAA 端点下拉绑定已删除（控件不再存在）
  [['sp_upnp_upsample','upnpUpsample'],['sp_upnp_force','upnpForceRate'],['sp_cleanup','cleanup'],['sp_crossfeed','crossfeed']].forEach(function(x){if(D(x[0]))D(x[0]).onchange=function(){ADV[x[1]]=this.checked?1:0;saveSettings();if(x[1]==='cleanup')refreshCleanupStatus();};});
  [['sp_upnp_ceiling','upnpRateCeiling'],['sp_cleanup_strength','cleanupStrength'],['sp_cleanup_noise','cleanupNoiseDb'],['sp_crossfeed_freq','crossfeedFreq'],['sp_crossfeed_level','crossfeedLevel'],['sp_dacbits','dacbits']].forEach(function(x){if(D(x[0]))D(x[0]).onchange=function(){if(!validateProcessingSettings())return;ADV[x[1]]=Number(this.value);saveSettings();rlRefreshShaperGuidance();};});
  if(D('sp_cleanup_noise_mode'))D('sp_cleanup_noise_mode').onchange=function(){
    if(!validateProcessingSettings()){syncCleanupNoiseMode();return;}
    ADV.cleanupAuto=this.value==='manual'?0:1;syncCleanupNoiseMode();saveSettings();refreshCleanupStatus();
  };
  if(D('sp_cleanup_refresh'))D('sp_cleanup_refresh').onclick=refreshCleanupStatus;
  paintCleanupStatus();
  syncProcessingControls();
  // 261024-01 #9：目标渲染器与「投放」区块同一个选择（不出现两套互相打架的控制）
  if (D('sp_upnp_scan')) D('sp_upnp_scan').onclick = function () { scanCastDevices(true); };
  if (D('sp_upnp_dev')) D('sp_upnp_dev').onchange = function () { syncUpnpDev(this.value); backendHint(); };
  // 2026-11-15：一键「切到 bit-perfect 直通」按钮已移除。
  // 现在 A–D 四组的后端/滤波/EQ/音量全部由数字音乐台统一控制；这里再改一套
  // 会直接和当前配置组打架（点一下可能把 ASIO/DSD 组改成 WASAPI PCM）。
  // bit-perfect 的判定仍由底栏/信号链按实际播放参数给出，不再提供第二套入口。
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
  // 2026-10-27（用户 261027-06 P1）：模型下拉 —— 刷新按钮 / 选中即保存 / 手填回车生效。
  if (D('sp_llmmodels')) D('sp_llmmodels').onclick = function () { llmModelsLoad(true); };
  if (D('sp_llmmodel')) D('sp_llmmodel').onchange = function () { llmSaveModel(this.value); };
  if (D('sp_llmmodeltext')) D('sp_llmmodeltext').addEventListener('keydown', function (ev) {
    if (ev.key === 'Enter') llmSaveModel(this.value);
  });
  llmModelRender();          // 先用本地已知的 model 画一次，避免空白下拉
  llmModelsLoad(false);      // 再异步拉服务端列表（后端 60s 缓存，重复打开设置不会重复打网络）
  if (D('sp_llmkey')) {
    D('sp_llmkey').value = '';
    D('sp_llmkey').addEventListener('keydown', function (ev) { if (ev.key === 'Enter') llmSave(); });
  }
  llmRefresh();
  // 2026-10-27（261027-02 #1）：「导出诊断包」那一栏已按用户要求删除，按钮随之不存在。
  // exportDiagZip / exportDiag 两个函数仍保留（未来若要在别处挂入口可直接复用），这里不再绑定。
  // 一键反馈（261027-01 #1）：提交按钮 + 回车即交（在标题框里按 Enter 不提交空表单）
  if (D('sp_fbsubmit')) D('sp_fbsubmit').onclick = fbSubmit;
  // 2026-10-28：提交前预览。让用户先看清这一条会带出去哪些东西 ——
  // 这是本地软件敢让终端用户直接点提交的前提。
  if (D('sp_fbpreview')) D('sp_fbpreview').onclick = fbPreview;
  // 关于与更新（261027-02 #5）：按钮 + 初始状态（进入设置页就把当前版本刷出来）
  updBind();
  if (D('sp_fbtitle')) D('sp_fbtitle').addEventListener('keydown', function (ev) {
    if (ev.key === 'Enter') { ev.preventDefault(); fbSubmit(); }
  });
  // ReplayGain（261027-01 #2）：模式一变立刻重画"非 bit-perfect"警告，不留到保存后才提示
  if (D('sp_rgmode')) D('sp_rgmode').onchange = function () { ADV.rgmode = this.value || 'off'; rgWarn(); saveSettings(); };
  if (D('sp_rgclip')) D('sp_rgclip').onchange = function () { ADV.rgclip = this.checked ? 1 : 0; saveSettings(); };
  // B3（2026-10-27）：房间校正自动应用开关。改完立刻落盘（下一次播放起生效），
  // 并把当前磁盘上的校正集状态刷新到卡片上 —— 用户必须能看出"我说开/关，后端到底认哪个"。
  if (D('sp_roomauto')) D('sp_roomauto').onchange = function () {
    ADV.roomcorrauto = this.checked ? 1 : 0;
    saveSettings(true);
    rcAutoSync();
    toast(this.checked ? '已开启：播放时自动应用房间校正' : '已关闭：不再自动应用房间校正');
  };
  rcAutoSync();
  // 261027-04 #4：播放时自动分析开关 —— 与单曲页那个小按钮共用 anlSetAuto（同一个值、同一份落盘）
  if (D('sp_autoanl')) D('sp_autoanl').onchange = function () {
    anlSetAuto(this.checked);
    var m = D('sp_anlmsg');
    if (m) m.textContent = analysisAutoStatus();
  };
  // 2026-10-28（用户紧急反馈）：「设置-高级-音频分析 → 启用音频分析模块」取消勾选、保存、刷新后又自动勾上。
  // 真因：sp_anlmaster 这个复选框**从来没有绑定 onchange**（anlSetMaster 定义了却零调用点）⟹
  //   取消勾选只改了 DOM 的 checked，ANL.master 仍是 1；saveSettings() 于是照旧把 anlmaster=1 写回 conf，
  //   刷新读回 1、又勾上。用户因此根本无法验证这个开关有没有用。
  // 修法：与 sp_autoanl 完全同构 —— 变化即写内存状态并落盘（anlSetMaster 内部已经 saveSettings()）。
  if (D('sp_anlmaster')) D('sp_anlmaster').onchange = function () {
    anlSetMaster(this.checked);
  };
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
      m.textContent = tf('当前状态：{0}', [(j.available ? t('GPU 可用') : t('走 CPU（未找到可执行的 GPU 后端）'))]);
      m.title = tf('rtagpu.dll={0}，rtagpu_cuda.dll={1}，AMD HIP 运行库={2}',
                   [(j.amd ? t('有') : t('无')),
                    (j.cuda ? (j.cudaUsable === false ? t('有（本机不可用）') : t('有（可用）')) : t('无')),
                    (j.hip ? t('有') : t('无'))]) +
                tf('；厂商偏好={0}', [(j.vendor || 'auto')]);
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
    // 后端 ASIO / 源 DSD / 音量 0 dB / 无 PCM 处理；同族整数倍率转换还需设备接受目标位率。
    m.textContent = d.length
      ? (tf('本机 ASIO 驱动 {0} 个 · Native DSD 需后端 ASIO、源为 DSD、音量 0 dB、无 EQ / 卷积；支持设备可用的同族整数倍率转换，同位率未转换时可 bit-perfect，转换后非 bit-perfect；驱动拒绝时回落 DoP', [d.length]))
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
    ADV = { device: '', bits: 0, taps: 0, atten: 0, transition: 0, phase: '', modcut: 0, moddith: -1, ir: '', irgpu: 0, ramp: 0, theme: S.theme, modorder: 5, modfamily: 'dsd', directsdm: 1, dsdKind: 'fir', dsdConv: 'narrow',
            backend: 'wasapi', channels: 0, quickStart: 1, cleanupAuto: 1,
            inBackend: 'none', inDev: '', inOffset: 0, inBuf: 100, inMonitor: 0, inSeconds: 0, inOut: '',
            rgmode: 'off', rgclip: 1, autoAnl: 1 };
    // 元数据两键的默认：自动补全开；AI 增强关（联网计费项默认关，避免「恢复默认」反而产生费用）。
    // 源开关随注册表一并全开。
    META = { on: 1, llm: 0, summary: 0 };
    (META_PROVIDERS || []).forEach(function (p) { META_SRC[p.id] = 1; });
    // 261027-04 #4：「恢复默认」也要把自动分析恢复成「开」（ADV.autoAnl 与 ANL.auto 是同一个值的两份视图）
    ANL.auto = 1; ANL.autoDone = '';
    if (D('sp_autoanl')) D('sp_autoanl').checked = true;
    D('sRate').value = 'src'; D('sRateFamilyPolicy').value = 'prefer'; D('sFiltNx').value = ''; D('sShap').value = 'auto';
    D('sBuf').value = 100; D('sDSD').value = 0; D('sIsp').checked = true; D('s2s').value = 'auto';
    saveSettings(); renderSettingsPage(); toast('已恢复默认');
  };
  D('sp_theme').onchange = function () { setTheme(this.value); };
  if (D('sp_quickstart')) D('sp_quickstart').onchange = function () { saveQuickStartPreference(this.checked); };
  syncQuickStartVisibility();
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
    scene: tf('由 {0} 依据测量数据生成的校准滤波，平直化固有频响缺陷。', [authorName]),
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
/* ---------------- 测量麦克风校准（2026-10-29 261029-02 新功能2） ----------------
   数据源：GET /api/miccal · POST /api/miccal（path= 或 name=+content=）· POST /api/miccal/clear
   为什么走上传而不是让用户填路径：浏览器 <input type=file> 拿不到本地绝对路径，
   让用户手抄路径对普通用户等于不可用。 */
function micCalPaint(j) {
  var nm = D('micCalName'), rg = D('micCalRange'), cr = D('micCalCorr');
  var sub = D('micCalSub'), hint = D('micCalHint');
  if (!nm) return;
  if (!j || !j.ok || !j.present) {
    nm.textContent = '未导入';
    if (rg) rg.textContent = '-';
    if (cr) cr.textContent = '-';
    if (sub) sub.textContent = '没有校准文件时，测到的房间响应会带上麦克风自身的误差';
    if (hint) hint.textContent = (j && j.error)
      ? (tf('读取失败：{0}', [j.error]))
      : '未导入校准文件：测量仍可进行，但结果会包含麦克风自身误差。';
    return;
  }
  nm.textContent = j.name || '（已导入）';
  if (sub) sub.textContent = j.path || '';
  if (rg) rg.textContent = (j.minHz && j.maxHz)
    ? (Math.round(j.minHz) + ' – ' + Math.round(j.maxHz) + ' Hz') : '-';
  if (cr) {
    var a = (typeof j.minDb === 'number') ? j.minDb.toFixed(2) : '-';
    var b = (typeof j.maxDb === 'number') ? j.maxDb.toFixed(2) : '-';
    cr.textContent = (j.points || 0) + tf(' 个点 · {0} ~ {1} dB', [a, b]);
  }
  if (hint) hint.textContent = '已导入：房间测量会按这条曲线扣除麦克风频响（只补偿幅度，相位不变）。';
}
function micCalLoad() {
  if (!D('micCalName')) return;
  // 2026-10-29：必须用 GET。api() 默认是 POST，而 /api/miccal 的 POST 是「导入」，
  //   拿它读状态会被后端判成「缺少 path/content」⟹ 界面永远显示未导入 + 一行错误。
  fetch('/api/miccal').then(function (r) { return r.json(); }).then(micCalPaint).catch(function () {
    if (D('micCalHint')) D('micCalHint').textContent = '读取校准状态失败（/api/miccal 无响应）';
  });
}
function micCalImportText(name, text) {
  var msg = D('sp_miccal_msg');
  if (msg) msg.textContent = '正在导入…';
  fetch('/api/miccal', {
    method: 'POST',
    headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
    body: 'name=' + encodeURIComponent(name) + '&content=' + encodeURIComponent(text)
  }).then(function (r) { return r.json(); }).then(function (j) {
    if (!j || !j.ok) {
      if (msg) msg.textContent = tf('导入失败：{0}', [((j && j.error) || '未知原因')]);
      toast('校准文件导入失败');
      return;
    }
    if (msg) msg.textContent = '';
    toast(tf('已导入校准：{0}（{1} 点）', [(j.name || ''), (j.points || 0)]));
    micCalPaint(j);
  }).catch(function (e) {
    if (msg) msg.textContent = tf('导入失败：{0}', [(e && e.message ? e.message : '网络错误')]);
  });
}
function micCalBind() {
  var f = D('sp_miccal_file'), pick = D('sp_miccal_pick'), clr = D('sp_miccal_clear');
  if (pick && f) pick.onclick = function () { f.value = ''; f.click(); };
  if (f) f.onchange = function () {
    var file = f.files && f.files[0];
    if (!file) return;
    if (file.size > 4 * 1024 * 1024) { toast('校准文件超过 4 MB，拒绝导入'); return; }
    var fr = new FileReader();
    fr.onload = function () { micCalImportText(file.name, String(fr.result || '')); };
    fr.onerror = function () { toast('读取文件失败'); };
    fr.readAsText(file);
  };
  if (clr) clr.onclick = function () {
    var msg = D('sp_miccal_msg');
    if (msg) msg.textContent = '';
    api('/api/miccal/clear', '').then(function () {
      toast('已清除麦克风校准');
      micCalLoad();
    }).catch(function () { toast('清除失败'); });
  };
  micCalLoad();
}

function calSettingsHtml() {
  var h = '';

  // 1.5 房间校正（测量向导）—— 2026-10-26（261026-01 #2 用户：「房间 EQ 功能请上架 EQ 那一栏的 UI」）
  // 位置：**紧跟 PEQ 之后**（第 2 张卡）——用户是在 EQ 页找这个功能，放在页尾等于没上架。
  // 2026-10-26 晚（261026-06）：后端 /api/roomcorr/{layouts,status,measure,cancel,download} 已接上，
  //   测量走真实 WASAPI（共享播放 + 采集，src/dsp/room_transport_wasapi.h），
  //   所以「预览」角标与「不会真的出声」的免责声明全部移除 —— 留着就是在骗用户。
  //   铁律 1 不变：向导最后一步必须弹授权横幅（设备名 / 时长 / 电平 / 会盖过系统声音）并要用户点确认，
  //   默认电平 −20 dBFS（MeasurementSession 缺省），不做任何放大。
  // 2026-10-29（管理员 261029-02 新功能2）：「设置-声音校准，缺少导入校正麦克风曲线功能，
//   有一部分出厂校准的麦克风是自带校准文件的。」
//   UMIK-1 / UMIK-2 / EMM-6 这类测量麦出厂带一份频响校准文件（频率 Hz + dB，可带相位）。
//   房间测量测到的是「房间 + 麦克风」的合成响应，必须扣掉麦克风自身频响，否则校正结果带着麦克风的误差。
//   放在房间校正**上面**：它是测量的前提条件，先有校准麦才谈得上准。
h += sgroup('测量麦克风校准', '导入麦克风出厂校准文件（.txt / .csv），房间测量时自动扣除麦克风自身频响',
    '<div class="sfld wide"><div class=in-stat-grid style="grid-template-columns:repeat(auto-fit,minmax(250px,1fr))">'
  + '<div class=in-stat-item><span class=lbl>当前校准曲线</span><span class="val sm" id=micCalName>未导入</span>'
  + '<div class=rcsub id=micCalSub>没有校准文件时，测到的房间响应会带上麦克风自身的误差</div></div>'
  + '<div class=in-stat-item><span class=lbl>覆盖范围</span><span class="val sm" id=micCalRange>-</span>'
  + '<div class=rcsub id=micCalCorr>-</div></div>'
  + '</div></div>'
  + '<div class="sfld wide" style="display:flex;align-items:center;gap:10px;flex-wrap:wrap">'
  + '<input type=file id=sp_miccal_file accept=".txt,.csv,.cal,text/plain" style="display:none">'
  + '<button class="btn pri" id=sp_miccal_pick type=button>选择校准文件…</button>'
  + '<button class=btn id=sp_miccal_clear type=button>清除</button>'
  + '<span class=hint id=sp_miccal_msg style="margin:0"></span></div>'
  + '<div class="sfld wide"><div class=rcnote>支持两列（频率 Hz、dB）或三列（第三列相位，忽略）；'
  + '常见来源：miniDSP UMIK-1 / UMIK-2、Dayton EMM-6、REW 导出的校准文件。'
  + '校准只影响<strong>测量与校正生成</strong>，不改变播放链路。</div></div>')
  + '<div class="sfld wide"><div class=rcnote id=micCalHint>正在读取当前校准状态…</div></div>';
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
    // B3（2026-10-27）：自动应用开关。用户原话「既然已经基本搞定了，接上就是了」——
    // 生成完校正集就该直接用上，不再要求用户自己转成 WAV 去「校正文件」里挑。
    + chk('sp_roomauto', '自动应用校正（播放时按当前输出布局自动挂载校正集）', ADV.roomcorrauto !== 0,
          '缺省开。播放时若 data\\roomcorr\\ 下存在与当前输出声道数匹配的 correction-<布局>.json，' +
          '就把其中的每声道校正 FIR 挂进卷积链；布局不匹配（例如测的是 2.0、现在按 5.1 输出）时不应用，' +
          '并在信号链里写明原因。手动填了「脉冲响应 IR」时以手动为准，不会被自动覆盖。', true)
    + '<div class="sfld wide"><div class=rcnote id=rcAutoHint>自动应用：读取中…</div></div>'
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
       '<span class=warnline>生成后自动生效：播放时若「自动应用校正」开着（默认开），会按当前输出布局自动挂载；' +
       '布局不匹配（例如测的是 2.0、现在按 5.1 输出）则不应用，并在信号链里写明原因。</span>' }
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
  D('rcStep').textContent = tf('第 {0} / {1} 步', [RCW.step, RCW.total]);
  D('rwTitle').textContent = s.t;
  var body = s.b;
  if (RCW.step === 2) {
    body += '<div style="margin-top:12px;display:flex;align-items:center;gap:10px;flex-wrap:wrap">' +
            '<label style="font-size:12px;color:var(--mut2)">声道布局' +
            '<select id=rcLayout style="margin-left:8px">' +
            RC_LAYOUTS.map(function (o) {
              return '<option' + (o === RCW.layout ? ' selected' : '') + '>' + o + '</option>';
            }).join('') + '</select></label>' +
            '<span class=rcnote>' + t('当前选择：') + '<b id=rcLayoutNow>' + RCW.layout + '</b></span></div>';
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
        t('声道布局：') + '<b>' + RCW.layout + '</b>' + tf(' · 时长：<b><span id=rcSecsVal>{0}</span> 秒/声道</b>', [RCW.seconds]) +
        tf(' · 电平：<b><span id=rcLevelVal>{0}</span> dBFS</b><br>', [RCW.levelDb]) +
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
             esc((d.name || d.id) + (d['default'] ? t('（默认）') : '')) + '</option>';
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
             esc((d.name || d.id) + (d.isDefault ? t('（默认）') : '')) + '</option>';
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
        rcProg(tf('第 {0} / {1} 步{2}', [j.step, j.totalSteps, (j.stepName ? tf('：{0}', [j.stepName]) : '')]) +
               (j.channel ? tf(' · 声道 {0}', [j.channel]) : '') + ' …');
      } else {
        rcPollStop();
        if (D('rcStop')) D('rcStop').style.display = 'none';
        if (D('rcGo')) D('rcGo').disabled = false;
        if (j.hasCorrectionSet) {
          rcProg(tf('✅ 测量完成：{0} · {1} taps @ {2} Hz', [(j.layout || RCW.layout), j.irTaps, j.irFs]) +
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
    if (el) el.textContent = j.hasCorrectionSet ? (tf('已测量 · {0}', [(j.layout || '')])) : '未测量';
    if (sub) sub.textContent = j.hasCorrectionSet
      ? (j.irTaps + ' taps @ ' + j.irFs + ' Hz' + (j.file ? ' · ' + j.file.split('\\').pop() : ''))
      : '点「开始向导」跑一次测量';
    if (D('bRoomDl')) D('bRoomDl').style.display = j.hasCorrectionSet ? '' : 'none';
  }).catch(function () {});
}
// B3（2026-10-27）：自动应用状态一句话 —— 开了/关了、磁盘上是哪个布局、当前输出几声道。
// 这句存在的理由：布局不匹配是**必然会发生**的正常情况（向导里测了 2.0、之后把输出改成 5.1），
// 只靠"没生效"三个字用户无法判断该改哪边。
function rcAutoSync() {
  var el = D('rcAutoHint');
  if (!el) return;
  var on = (ADV.roomcorrauto !== 0);
  api('/api/roomcorr/status').then(function (j) {
    var disk = (j && j.hasCorrectionSet) ? (tf('磁盘上有校正集：{0} · {1} taps @ {2} Hz', [(j.layout || '?'), j.irTaps, j.irFs])) : '磁盘上还没有校正集（先跑一次向导）';
    var ch = (ADV.channels || 0);
    var cnt = ch > 0 ? (tf('当前输出 {0} 声道', [ch])) : '当前输出跟随源文件声道数';
    var localMatch = '';
    var LAY = { '2.0': 2, '2.1': 3, '5.1': 6, '7.1': 8, '5.1.2': 8, '7.1.4': 12 };
    if (j && j.hasCorrectionSet && ch > 0 && LAY[j.layout] && LAY[j.layout] !== ch)
      localMatch = '<br><span style=color:#fbbf24>⚠️ 布局不匹配：校正集是 ' + j.layout + '（' + LAY[j.layout] +
                   ' 声道），当前按 ' + ch + ' 声道输出 ⟹ 播放时不会应用。</span>';
    el.innerHTML = (on ? '自动应用：<strong>开</strong>' : '自动应用：<strong>关</strong>（只用下面手动的 IR）') +
      ' · ' + disk + ' · ' + cnt + localMatch;
  }).catch(function () { el.textContent = tf('自动应用：{0}', [(on ? '开' : '关')]); });
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
  // 2026-10-29（261029-02 新功能2）：测量麦克风校准卡的绑定与状态刷新
  micCalBind();
  // 下载校正集（261026-06）：直接给下载链接，浏览器原生下载，不走 fetch
  if (D('bRoomDl')) D('bRoomDl').onclick = function () {
    var lg = (RCW.layout || '2.0');
    window.location.href = '/api/roomcorr/download?layout=' + encodeURIComponent(lg);
  };
  // 进页面先刷一次真实状态（有校正集就显示，没有就提示未测量）
  rcStatusRefresh();
  rcAutoSync();          // B3：自动应用那一行也要看到磁盘现状与布局是否匹配
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
  peqMsg(tf('第 {0} 段已复位（0.0 dB / Q 1.41）', [i]));
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
  peqMsg(tf('自动前级：曲线峰值 +{0} dB → 前级 {1} dB（留出 {2} dB 余量）', [maxGain.toFixed(1), pre, Math.abs(pre).toFixed(0)]));
  toast(tf('自动前级 {0} dB（峰值 +{1} dB）', [pre, maxGain.toFixed(1)]));
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
    txt = tf('PEQ 当前：生效 · {0} 段全为 0 dB、前级 0 dB（不改变声音）', [(total || 20)]);
  } else {
    txt = 'PEQ 当前：生效 · ' + active + ' / ' + (total || 20) + ' 段非 0 dB' +
          (Math.abs(pre) >= 0.001 ? (tf('、前级 {0} dB', [pre.toFixed(1)])) : '') + '（输出已被 EQ 改变）';
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
    if (!j || !j.ok) { peqMsg(tf('失败：{0}', [((j && j.error) || '未知错误')])); return; }
    PEQ_STATE = j;
    peqDrawCurve(j.points);   // 只刷新曲线，不重建输入框（避免打断正在输入的光标）
    peqStatusRender(j);       // 261024-04 #3：状态行刷新（以后端返回值为准）
    peqMsg(tf('已应用（{0} · 采样率 {1} Hz）', [(j.bypass ? '旁路' : '生效'), Math.round(j.fs)]));
  }).catch(function (e) { peqMsg(tf('失败：{0}', [e])); });
}
function peqLoadState() {
  return api('/api/peq').then(function (j) {
    if (j && j.ok) peqRender(j);
  }).catch(function () { });
}
function peqPresetAction(action, name) {
  api('/api/peq/preset', 'action=' + encodeURIComponent(action) + '&name=' + encodeURIComponent(name || '')).then(function (j) {
    if (!j || !j.ok) { toast(tf('预设操作失败：{0}', [((j && j.error) || '未知错误')])); return; }
    peqRender(j);
    if (action === 'load') toast(tf('已加载预设：{0}', [name]));
    else if (action === 'save') toast(tf('已保存预设：{0}', [name]));
    else if (action === 'reset') toast('已重置为 Flat');
    else toast('已删除预设');
  }).catch(function (e) { toast(tf('预设操作失败：{0}', [e])); });
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
    if (confirm(tf('删除用户预设「{0}」？（内置预设不能删除）', [sel.value]))) peqPresetAction('delete', sel.value);
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
        navigator.clipboard.writeText(text).then(function () { toast(tf('已复制来源链接: {0}', [text])); });
      } else {
        toast(tf('来源链接: {0}', [text]));
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
        hint.textContent = tf('共匹配到 {0} 款耳机，已显示前 30 条。继续输入更具体关键词以精确筛选…', [allHits.length]);
      } else {
        hint.textContent = tf('匹配到 {0} 款耳机型号', [hits.length]);
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
              '<div style="font-size:11px;color:var(--mut2)">' + tf('目标曲线：{0}', [esc(curve.name)]) + '</div>' +
              '<div class="tc-curve" title="目标曲线示意（手绘近似，非测量数据）">' + targetCurveSvg(curve.kind) + '</div>' +
              '<div style="font-size:12px;color:var(--txt2);line-height:1.5;margin:8px 0 0;background:var(--card);padding:8px 10px;border-radius:6px;border:1px solid var(--line)">' +
              '<span>' + tf('说明：{0}', [esc(curve.scene)]) + '</span></div>' +
              tf('<div style="font-size:11.5px;color:var(--mut2);margin-top:8px">前级增益：<b>{0} dB</b> · 频段数：<b>{1} 段</b></div>', [p.gainDb, p.bandsCount]) +
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
      if (msg) msg.textContent = tf('获取失败: {0}', [err]);
      if (presetsBox) presetsBox.innerHTML = '<div class=empty style="padding:20px 0">网络错误或预设获取失败</div>';
    });
  }

  function showTargetCurveInfo(p) {
    if (!targetInfo) return;
    var curve = getTargetCurveDesc(p.details, p.author);
    targetInfo.innerHTML = '<div style="display:flex;gap:18px;flex-wrap:wrap;align-items:flex-start">' +
      '<div style="flex:1;min-width:260px">' +
      '<div style="font-weight:700;font-size:13px;margin-bottom:6px;color:var(--txt)">' + esc(curve.name) + ' · 目标曲线说明</div>' +
      '<div style="margin-bottom:5px">' + tf('是什么：{0}', ['<b style="color:var(--txt)"></b>' + esc(curve.tech)]) + '</div>' +
      '<div>' + tf('适合谁：{0}', [esc(curve.scene)]) + '</div>' +
      '</div>' +
      '<div style="flex:0 0 260px;min-width:220px" class="tc-curve">' +
      '<div style="font-size:10.5px;color:var(--mut2);margin-bottom:2px">目标曲线示意（手绘近似）</div>' +
      targetCurveSvg(curve.kind, 78) +
      '</div>' +
      '</div>' +
      '<div style="margin-top:10px;font-size:11.5px;color:var(--mut2);border-top:1px dashed var(--line);padding-top:8px">' +
      tf('前级增益：{0} dB（独立衰减防削波） · 滤波段数：{1} 段 · 预设：{2}', [p.gainDb, p.bandsCount, esc(p.details || p.id)]) + '</div>';
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
      return Promise.resolve(saveSettings(true)).then(function(saved){
      if(!saved||!saved.ok){if(saved&&saved.stale)toast(t('当前组已切换，请重新操作'));return saved;}
      toast('已应用调音预设到 EQ');
      Array.prototype.forEach.call(presetsBox.querySelectorAll('.preset-card'), function (c) {
        var isThis = (c.dataset.id === eid);
        c.classList.toggle('active', isThis);
        var b = c.querySelector('.opra-apply-btn');
        if (b) b.textContent = isThis ? '已应用当前预设' : '应用到 EQ';
      });
      var p = presets.find(function (x) { return x.id === eid; });
      if (p) showTargetCurveInfo(p);
      return saved;
      });
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
      return Promise.resolve(saveSettings(true)).then(function(saved){if(saved&&saved.ok)toast('已保存房间校正设置');else if(saved&&saved.stale)toast(t('当前组已切换，请重新操作'));return saved;});
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
// ============================================================================
// 音质 19 项：素材分类 + 显隐判据（2026-10-27 置信度审查，见 build-qc/quality_confidence_report.md §3/§5）
// ----------------------------------------------------------------------------
// 一条原则：**没有有效音频的素材不该给出一整套"看起来有效"的 0 值**；跨段落/跨采样率不可比的
// 指标不该裸着数值出现。这里只做"分类 + 显隐 + 限定词"，算法与字段全部来自后端（quality.h 的
// 19 项），前端一个数都不改，也不重算 —— 判据、口径、实测数据都在 quality.h 的文件头里。
// ============================================================================
function qualFacts(h, path) {
  var q = (h && h.quality) || {};
  var fs = Number(h && h.fs) || 0;
  var p = String(path || (typeof ANL !== 'undefined' && ANL.cur) || '');
  var available = !!q.available;
  var peak = Number(q.peak_dbfs_q), rms = Number(q.rms_dbfs), crest = Number(q.crest_db);
  var cen = Number(q.centroid_hz), roll = Number(q.rolloff95_hz), sil = Number(q.silence_pct);
  // DSD 源（.dsf/.dff）或 DSD 化 PCM（352.8k / 705.6k 倍率）：噪声整形把重心推到 50–100 kHz、
  // >10k 占比 ≈ 0 dB ⟹ 频响四项失去意义（实测 DSD 化素材 centroid 99.9 kHz、hf10k −0.02 dB）。
  var dsd = /\.(dsf|dff)$/i.test(p) || fs >= 300000;
  // ① 数字静音：峰值 ≤ −120 dBFS（−300 是"测不出"哨兵）；② 整轨都在 −60 dBFS 以下
  //    （silence_pct ≈ 100 且 RMS ≤ −70）：库里那首 5.25 s「100% 静音」音轨 peak 只有 −59.7 dBFS、
  //    RMS −91 dB —— 它是全库 >10k 占比最高的"文件"（−5.2 dB），正是这一条要挡的样本。
  var silent = available && ((isFinite(peak) && peak <= -120) ||
                             (sil >= 99.5 && isFinite(rms) && rms <= -70));
  // ③ 纯直流：峰值因子 ≈ 0（peak == rms）且重心 < 100 Hz（实测 0.5 直流：crest 0.00、centroid 2 Hz）
  var dc = available && !silent && isFinite(crest) && crest < 0.5 && isFinite(cen) && cen < 100 &&
           isFinite(rms) && rms > -60;
  // ④ 单点冲激：峰值因子 ≥ 40 dB（音乐到不了）且 95% 滚降 ≥ 0.4×采样率（能量摊到全带）
  var impulse = available && !silent && !dc && isFinite(crest) && crest >= 40 && fs > 0 &&
                isFinite(roll) && roll >= 0.4 * fs;
  var noAudio = silent || dc || impulse;
  var why = silent ? (isFinite(peak) && peak <= -120 ? '全静音（峰值 ≤ −120 dBFS）'
                                                    : tf('整轨都低于 −60 dBFS（silence {0}%）', [sil.toFixed(1)]))
          : dc ? '纯直流（峰值因子 ≈ 0、谱重心 < 100 Hz）'
          : impulse ? '单点冲激（峰值因子 ≥ 40 dB、95% 滚降 ≥ 0.4×采样率）' : '';
  return {
    q: q, fs: fs, available: available, dur: Number(h && h.duration_sec),
    dsd: dsd, noAudio: noAudio, noAudioWhy: why,
    spectralOk: available && !noAudio && fs >= 44100 && !dsd
  };
}

// 谱四项（重心/倾斜/95% 滚降/>10k 占比）不展示的原因；空串 = 可以展示。
function qualSpectralHide(F) {
  if (!F.available) return '样本不足，未出结论';
  if (F.noAudio) return tf('素材无有效音频：{0}', [F.noAudioWhy]);
  if (F.dsd) return 'DSD / DSD 化素材：噪声整形把能量推到 50–100 kHz，四项无意义';
  if (F.fs > 0 && F.fs < 44100) return '采样率 ' + (F.fs / 1000).toFixed(1) + ' kHz < 44.1 kHz：10 kHz 以上频段不存在';
  return '';
}

// 削波口径（报告 §5）：**headline 只认 clippedRuns / longestClipRun**；clippedPct 是上界。
// 「pct>0 但 runs=0」= 稀疏单点满幅（全库 110/655 首如此），必须说清不是连续削波。
function qualClipText(q) {
  var runs = Number(q.clipped_runs), pct = Number(q.clipped_pct), longest = Number(q.longest_clip_run);
  if (isFinite(runs) && runs > 0) {
    return { head: tf('有数字削波 {0} 段 · 最长 {1} 样本', [anlNum(runs, 0), anlNum(longest, 0)]),
             sub: anlNum(q.clipped_samples, 0) + tf(' 样本 / {0} %（上界，含单样本越限）', [anlNum(pct, 4)]),
             sparse: false };
  }
  if (isFinite(pct) && pct > 0) {
    return { head: '0 段 · 仅零星单样本越限，非连续削波',
             sub: anlNum(q.clipped_samples, 0) + tf(' 样本 / {0} %（上界，含单样本越限）', [anlNum(pct, 4)]),
             sparse: true };
  }
  return { head: '无削波', sub: '0 样本 / 0 %（上界，含单样本越限）', sparse: false };
}
// 立体声三项（报告 §3）：mono-in-stereo 与极性反相都必须有说明，不能只说"立体声正常"。
function qualStereoText(q) {
  if (!q || !q.stereo) return '非立体声（单声道或 >2 声道）';
  var corr = Number(q.channel_corr), loss = Number(q.mono_loss_db), sm = Number(q.side_mid_db);
  if (isFinite(corr) && corr > 0.99995 && isFinite(loss) && loss < 0.05) {
    return '双声道但左右完全相同（等同单声道）';
  }
  var s = tf('相关 {0} / 单声道损失 {1} dB', [anlNum(corr, 4), anlNum(loss, 2)]);
  if (isFinite(loss) && loss >= 60 && isFinite(corr) && corr < -0.9) {
    s += ' —— 可能是极性反相（老录音常见），不一定是错误';
  }
  if (isFinite(sm) && sm > -0.01 && sm < 0.01) s += ' —— side/mid = 0 dB：单边静音也会这样，不是"完全中置"';
  return s;
}

// P3（2026-10-27 用户 261027-05 C4）：旧「分析音质（19 项）」弹窗**已并入** openHealthModal。
// 原来这里是第二套渲染器（另一张表、另一句「假无损 疑似/否」、另一个口径），两个入口各说各话。
// 现在实现只有一份：openHealthModal 按 L2 打开（三块卡 + 全部原始指标 + 无损性判定依据）。
// 保留这个符号是为了不让任何遗留调用点报 ReferenceError（当前仓库内已无调用者）。
function openQualityModal(path) {
  openHealthModal(path);
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
      D('cmpMsg').textContent = tf('正在对齐并分析 {0} 个版本…', [sel.length]);
      D('cmpGo').disabled = true;
      apiRaw('/api/compare', JSON.stringify({ files: sel })).then(function (r) {
        D('cmpGo').disabled = false;
        D('cmpMsg').textContent = '';
        D('cmpOut').innerHTML = renderCompare(r);
      }).catch(function (e) {
        D('cmpGo').disabled = false;
        D('cmpMsg').textContent = tf('对比失败：{0}', [(e && e.message ? e.message : e)]);
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
  // 版本号（V1/V2…）是后端给的序号，单看它认不出是哪个文件 ⟹ 一律带上文件名。
  function vName(v, i) {
    var base = String((v && (v.path || v.label)) || ('#' + i)).replace(/^.*[\\\/]/, '');
    return base;
  }
  // 第 8 维（2026-10-27）：有效带宽 / 有损转码。数值全部来自后端「长期平均谱的陡沿截止」
  // （信号本身），不看扩展名/码率 —— 所以「改名成 .flac」「改码率标签」骗不过这一维。
  function bwTxt(v) {
    if (!v) return '';
    var hz = v.cutoffDetected ? Number(v.cutoffHz) : Number(v.bandwidthHz || 0);
    if (!isFinite(hz) || hz <= 0) return '—';
    return (hz / 1000).toFixed(1) + ' kHz';
  }
  // 2026-10-27：这一条只说明「经过编码器」，**不再是「扣 12 分」的同义词** ——
  // 后端的扣分拆成两笔（带宽损失按 Hz、编解码损伤按陡沿×置信度×截止离奈奎斯特的距离），
  // 320k（截止约 0.92×奈奎斯特）那档的编解码损伤会小到 2~3 分，128k 才是满额 12 分。
  function lossyTag(v) {
    if (!v || !v.lossySuspected) return '';
    return ' <span style="color:var(--err);font-weight:650">［有损转码（截止 ' + bwTxt(v) + '）］</span>';
  }
  var topIdx = (r.ranking && r.ranking.length) ? r.ranking[0] : 0;
  var topV = vs[topIdx] || {};
  var anyClean = false;
  vs.forEach(function (v) { if (v && !v.lossySuspected) anyClean = true; });
  // 第一名仍是「疑似有损」⟹ 红字警告（正常情况不该出现：带宽损失 + 编解码损伤应把它压下去）
  if (topV.lossySuspected && anyClean) {
    h += '<div style="color:var(--err);font-weight:650;margin:0 0 8px;line-height:1.5">' +
         '⚠ 排序第 1 的 ' + esc(topV.label || '') + ' 是疑似有损转码（截止 ' + esc(bwTxt(topV)) +
         '），却排在没有该标记的版本之前：评分权重可能失效，请勿据此选版本。</div>';
  }
  // 对齐可信度（2026-10-27 DEBUG 增补，只加限定词）：相关值必须露在名次旁边 ——
  // 0.45~0.70 只是「像同一首」（母带/剪辑不同也会落在这里），只有后端 explanation 里写一句
  // 用户基本看不到；而名次是拿来当结论的，可信度必须和名次同时出现。
  var corr = Number(r.sameTrackCorr);
  if (isFinite(corr)) {
    h += '<div class="hint" style="margin:0 0 6px">对齐相关 ' + esc(corr.toFixed(3)) +
         '（≥0.45 才判同曲）' + ((corr < 0.70) ? ' —— 偏低：母带/剪辑可能不同，结论仅供参考' : '') + '</div>';
  }
  h += '<div style="font-weight:650;margin-bottom:4px">排序（好 → 差）</div><ol style="margin:0 0 8px 18px;padding:0">';
  var prevScore = null;
  (r.ranking || []).forEach(function (i) {
    var v = vs[i] || {};
    // 分差 <1 分：这套指标的测量噪声就有这个量级（实测同一内容只差前导静音 0.20 分、
    // 48k 往返一次 0.67 分），所以名次在这里没有意义 —— 必须在界面上说出来，否则
    // 「V3 100.0 > V1 99.4」会被读成音质高低。
    var gapNote = '';
    if (prevScore !== null && isFinite(v.score)) {
      var g = prevScore - Number(v.score);
      if (g >= 0 && g < 1.0)
        gapNote = ' <span style="color:var(--warn,#e6a23c);font-size:11px">与上一名仅差 ' +
                  g.toFixed(2) + ' 分 · 差异可忽略</span>';
    }
    if (isFinite(v.score)) prevScore = Number(v.score);
    // 前导静音/整体偏移：对齐结论是"公共区段"的前提，露出来便于核对
    var lagNote = '';
    var lg = Number(v.lagSec);
    if (isFinite(lg) && Math.abs(lg) >= 0.005)
      lagNote = ' <span style="color:var(--mut2);font-size:11px">前导' + (lg > 0 ? '+' : '') +
                lg.toFixed(2) + ' s 已对齐</span>';
    h += '<li>' + esc(v.label || ('#' + i)) + ' <span style="color:var(--mut)">' + esc(vName(v, i)) + '</span>' +
         ' — <b>' + qFmt(v.score, 1) + '</b> 分' +
         ' <span style="color:var(--mut2);font-size:11px">带宽 ' + esc(bwTxt(v)) + '</span>' +
         lagNote + lossyTag(v) + gapNote +
         (v.error ? (' <span style="color:var(--err)">' + esc(v.error) + '</span>') : '') + '</li>';
  });
  h += '</ol>';
  // 不可比项：表格里已灰显 + 行首 tip，这里给一个总数，避免用户以为"所有指标都参与了打分"
  var nNotComp = 0;
  rows.forEach(function (row) { if (row && !row.comparable) ++nNotComp; });
  if (nNotComp > 0) {
    h += '<div class="hint" style="margin:0 0 6px">' + nNotComp +
         ' 项指标不可比（表内灰显，鼠标移到指标名上看原因），它们不参与评分</div>';
  }
  if (!anyClean && vs.length) {
    h += '<div class=hint style="color:var(--warn,#e6a23c)">本批次所有版本都带「有损」标记 ⟹ 排序只在有损版本之间有效（带宽维度全员同罚，不影响名次）。</div>';
  }
  // 扣分口径说明（2026-10-27 P1 修复）：用户最容易误读的就是「有损 ⟹ 一律 −12」。
  // 现在两笔罚的是不同性质的损失，且后一笔在截止接近奈奎斯特时自动变小 —— 必须在界面上讲清，
  // 否则「320k 只比无损低几分」会被读成"工具漏判了"。
  if (vs.some(function (v) { return v && v.cutoffDetected; })) {
    h += '<div class=hint style="margin:0 0 6px">有损版本的扣分拆两笔（不重复罚同一个截止值）：' +
         '<b>带宽损失</b> 按「缺了多少频谱」连续折算；<b>编解码损伤</b> 最多 12 分，' +
         '按编码器级陡沿 × 置信度 × 截止离奈奎斯特的距离平滑折算 —— ' +
         '128k（截止约 0.76×奈奎斯特）吃满额，320k（约 0.92×）只剩 2~3 分，' +
         '接近奈奎斯特时归零。' +
         '（这两笔都只看信号本身，不看扩展名/码率。轻度的谐波失真本页测不出，见已知缺陷说明。）</div>';
  }
  h += '<div style="max-height:40vh;overflow:auto"><table style="font-size:11.5px;border-collapse:collapse;width:100%">';
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
// 音频分析（2026-10-27 261027-04 #4）：从「主页 + 设置」搬到**正在播放的单曲页**
// ----------------------------------------------------------------------------
// 用户原话：「主页-分析和设置-分析，整个功能页不应该放到这里，直接将分析放到正在播放的那首歌
// 音乐上，播放就自动运行一次分析，在单曲播放页面上放置入口展示（动态、音质、假无损）。」
//
// 落点：数字音乐台「音质监控」#npAnal（完整指标与原判据保留），三类各一块：
//   ① 动态   —— TT DR / R128 LUFS / LRA / 样本峰值 / 真峰(dBTP，2026-10-27 加)
//   ② 音质   —— 削波 / 谱重心 / 信噪比 / THD+N（THD+N 对音乐素材通常「不适用」，如实显示）
//   ③ 假无损 —— 判定【四档：疑似有损 / 带宽受限（不判假）/ 证据不足 / 未发现】
//               + 截止 + 陡沿(dB) + 置信度 + 谱窗数，并把后端 reason 与三条前置限定词显示在卡上
//               （2026-10-27 修复轮：旧版只有「未发现转码痕迹」绿字，会被读成"文件是好的"）
// 数据源只有一个：GET /api/health?file=…（后端 track_health_json：一次解码出三份结论）。
//
// 自动触发（anlUpdateCur，由 400 ms 的 tick() 驱动）：
//   * **按路径去重**：同一首只触发一次 —— 暂停 / 恢复 / 拖动进度都不会重跑，换歌才跑；
//   * 只对**本地文件**跑（http(s) / UNC 网络共享 / .cda 一律跳过）；
//   * 异步 fetch（240 s 超时），不阻塞播放线程也不阻塞 UI（后端 WebUi 最多 6 个并发 worker）；
//   * 两层缓存：后端（路径+大小+mtime ⟹ cached=true，实测 <100 ms）
//              前端 ANL.cache[path]（切页 / 重绘 / 再打开单曲页都不再打端点）。
// 开关：ADV.autoAnl（conf 键 autoanl，缺省 1 = 开）。单曲页的小按钮与
//       设置-高级「播放时自动分析」是同一个值，两处都走 saveSettings()。
// 纪律：只读文件，不开任何音频设备（铁律 1）。
// ============================================================================
var ANL = {
  cur: '',                    // 分析条当前显示哪一首（= /api/status 的 current）
  cache: {},                  // path → health（前端缓存，切页不重打端点）
  busy: {},                   // path → true
  autoDone: '',               // 自动触发去重键（上一次已自动触发过的路径）
  t0: 0,                      // 本次发起时刻
  elapsed: 0,                 // 上次耗时（ms）
  cached: false,              // 上次是否命中后端缓存
  src: '',                    // 'front'（前端缓存）/ 'backend'（后端缓存）/ 'new'（真算）
  err: '',                    // 上次失败原因（空 = 没失败）
  stat: { runs: 0, hits: 0 }, // 后端计数器快照（/api/health 顺带返回，判据用）
  // 展开级别（2026-10-27 261027-05 A5）：0=折叠摘要（默认）/ 1=指标格 / 2=全部原始指标。
  // 同一首歌跨重绘保持（暂停、拖动进度、重进单曲页都不重置）；换歌由 anlUpdateCur 强制归 0。
  lvl: 0,
  // 2026-10-28（2.0 前，用户要求）：**模块总开关**。
  //   auto=0 只是"不自动跑"，单曲页那块卡还在（可手动点重新分析）；
  //   master=0 则是**整个模块关掉** —— 单曲页不渲染分析卡、右键菜单没有分析入口、
  //   所有 /api/health 请求都不发。conf 键 anlmaster（默认 1）。
  master: 1
};

// 分析可能跑几秒到几十秒（282 s 的 6 声道 FLAC 实测 2.3 s，DSD/长曲更久），
// 所以不能用 apiRaw 的 8 s 超时 —— 这里自带一个可配超时的 fetch。
function anlFetch(url, body, ms) {
  var opt = { method: 'POST' };
  if (body !== undefined) opt.body = body;
  var ctrl = (typeof AbortController !== 'undefined') ? new AbortController() : null;
  if (ctrl) opt.signal = ctrl.signal;
  var timer = 0;
  if (ctrl) timer = setTimeout(function () { ctrl.abort(); }, ms || 180000);
  return fetch(url, opt).then(function (r) {
    if (timer) clearTimeout(timer);
    if (!r.ok) return r.text().then(function (t) {
      var m = '';
      try { var j = JSON.parse(t); m = j.error || j.message || ''; } catch (e) { m = t; }
      throw new Error(m || ('HTTP ' + r.status));
    });
    return r.json();
  }, function (e) { if (timer) clearTimeout(timer); throw e; });
}

function anlNum(v, d) {
  if (v === undefined || v === null) return '—';
  if (typeof v !== 'number') return String(v);
  if (!isFinite(v)) return '—';
  if (v <= -299 && v > -1e6) return '不适用';    // −300 是「测不出」哨兵（见 quality.h）
  return v.toFixed(d === undefined ? 2 : d);
}

// 只在**本地文件**上自动分析：流媒体（http/https 等）、UNC 网络共享、CD 音轨都跳过。
// 理由：前两者要边下边解（代价不可控），.cda 只有 44 字节索引、根本没有可分析的音频。
function anlIsLocal(p) {
  p = String(p || '');
  if (!p) return false;
  if (/^[a-zA-Z][a-zA-Z0-9+.\-]*:\/\//.test(p)) return false;
  if (p.slice(0, 2) === '\\\\') return false;
  if (/\.cda$/i.test(p)) return false;
  return /^[a-zA-Z]:[\\\/]/.test(p);
}

// —— 一句人话（每块一行）——
function anlDynLine(h) {
  var dr = h.dr_db;
  if (!(dr > 0)) return { t: '动态：样本太短，未出结论', c: 'mut' };
  if (dr >= 14) return { t: '宽动态 · 轻响层次清楚', c: 'ok' };
  if (dr >= 9) return { t: '中等动态 · 流行/摇滚常见', c: '' };
  if (dr >= 6) return { t: '偏压缩 · 响度战争母带', c: 'warn' };
  return { t: '强压缩 · 几乎全程顶满', c: 'warn' };
}
function anlQualLine(h) {
  var q = h.quality || {};
  if (!q.available) return { t: '样本不足，未出结论', c: 'mut' };
  var F = qualFacts(h);
  // 2.2：素材无有效音频 ⟹ 一句话说清，不再给"干净 · 无明显削波"这种正面结论。
  if (F.noAudio) return { t: '素材无有效音频 · 谱类与 SNR 不展示', c: 'mut' };
  // ⚠️ 这里**不能**拿 snr_db 判"底噪大"：quality.h 的定义是「全轨 RMS − 本底（最低 10% 块电平）」，
  // 对压缩过的流行乐天然只有 15~20 dB —— 那是动态小，不是噪声大。第一版写成"本底偏高·噪声大"
  // 对着一首干净的 FLAC 报噪声（实测踩到），已删除该分支。
  // 2.4：headline **只认 clippedRuns**（连续 ≥3 样本）。clippedPct 是上界（含单样本越限），
  //      以前「pct ≥ 0.01 就报削波」会把 110/655 首稀疏单点满幅曲目误判成削波。
  var clip = qualClipText(q);
  var clip = qualClipText(q);
  if (Number(q.clipped_runs) > 0) return { t: clip.head + ' · 母带压过头', c: 'warn' };
  if (clip.sparse) return { t: '仅零星单样本越限，非连续削波', c: '' };
  if (q.stereo && Number(q.mono_loss_db) >= 60 && Number(q.channel_corr) < -0.9) {
    return { t: '左右极性相反（可能是老录音的真实极性，不一定是错误）', c: 'warn' };
  }
  // P3（2026-10-27 261027-05 C4）：这一行原来说「可能被低通/有损」—— 那是**第二处**无损性判定，
  // 口径还和 ③ 假无损卡不一样（这里只看 >10k 占比，那边看截止 + 陡沿 + 置信度）。
  // 用户要求「有损检测就是假无损检测，归类到一起」⟹ 音质这一块只说音质，不再碰有损。
  if (F.spectralOk && (q.hf_ratio_10k_db || 0) < -40) return { t: '高频偏少 · 能量集中在中低频', c: '' };
  if (F.spectralOk && (q.centroid_hz || 0) > 0 && q.centroid_hz < 700) return { t: '偏闷 · 能量集中在低频', c: '' };
  return { t: '干净 · 无明显削波', c: 'ok' };
}
// ③ 假无损的一句话（2026-10-27 修复轮）：**不再只给绿/红两个字**
// 后端 fake_verdict 四档，且每档都把判据量写在脸上（报告 §3「会误导用户的具体说法」）：
//   lossy        「疑似有损转码 · 截止 16.6 kHz、下降 40 dB」
//   band_limited 「有效带宽受限 19.9 kHz · 母带低通/升频，不判假」
//   unclear      「证据不足 · 素材过短（0.60 s）」/「… 置信度 43% < 50%」
//   none         「未发现 · 检出 21.0 kHz、下降 18 dB、置信 45% ⟹ 不构成判据」
// 旧版只有「未发现转码痕迹」+ 绿字，用户会读成"这文件是好的"。
function anlFakeLine(h) {
  var v = h.fake_verdict || (h.lossless_suspected ? 'lossy' : 'none');
  var cutHz = Number(h.cutoff_hz) || 0;
  var cut = cutHz > 0 ? (cutHz / 1000).toFixed(1) : '';
  var conf = Math.round((Number(h.confidence) || 0) * 100);
  var edge = Math.round(Number(h.edge_db) || 0);
  var fill = (typeof h.window_fill === 'number') ? Number(h.window_fill) : 1;
  var dur = Number(h.duration_sec) || 0;
  var durTxt = dur > 0 ? ((dur < 10 ? dur.toFixed(2) : dur.toFixed(1)) + ' s') : '很短';
  if (v === 'lossy') {
    return { t: tf('疑似有损转码 · 截止 {0} kHz、下降 {1} dB', [cut || '?', edge]), c: 'warn' };
  }
  if (v === 'band_limited') {
    return { t: tf('有效带宽受限 {0} kHz · 母带低通/升频，不判假', [cut]), c: '' };
  }
  // 2026-10-30：第五档「源本身有损」。它回答的是"问题不适用"，不是"证据不足"：
  // 假无损的定义是【有损内容被转制进无损容器】，源本身就是 mp3 时不存在这个前提。
  // 实测（天门 - レトロスペクティブ京都.mp3）：cutoff 15.4 kHz、置信度 0.42 ⟹ 旧实现给
  // "证据不足"，用户完全无从判断；现在直接说清"它是 mp3，不用查"。
  if (v === 'source_lossy') {
    return { t: tf('源本身是有损格式 · 不存在假无损问题', []) + (cut ? tf('（截止 {0} kHz 仅供参考）', [cut]) : ''), c: '' };
  }
  if (v === 'unclear') {
    if (fill < 0.5) return { t: tf('证据不足 · 素材过短（{0}），不构成判据', [durTxt]), c: 'mut' };
    return { t: tf('证据不足 · 置信度 {0}% < 50%，检出 {1} kHz', [conf, cut || '?']), c: 'mut' };
  }
  if (!(cutHz > 0)) return { t: '未发现转码痕迹 · 未检出频谱截止（满带宽）', c: 'ok' };
  return { t: tf('未发现转码痕迹 · 检出 {0} kHz、下降 {1} dB、置信 {2}% ⟹ 不构成判据', [cut, edge, conf]),
           c: (conf >= 40 ? 'mut' : 'ok') };
}

// ③ 假无损卡底部的限定词行（把后端 reason + 三条前置限定词摊开，原来是看不到的）
function anlFakeNote(h) {
  var parts = [];
  var fill = (typeof h.window_fill === 'number') ? Number(h.window_fill) : 1;
  var win = Number(h.spectral_windows) || 0;
  var dur = Number(h.duration_sec) || 0;
  var conf = Math.round((Number(h.confidence) || 0) * 100);
  var durTxt = dur > 0 ? ((dur < 10 ? dur.toFixed(2) : dur.toFixed(1)) + ' s') : '很短';
  if (fill > 0 && fill < 0.5) parts.push(tf('素材过短（{0}，谱窗仅 {1}% 有实样）只作参考', [durTxt, Math.round(fill * 100)]));
  else if (fill > 0 && fill < 0.999) parts.push(tf('素材偏短（{0}），置信度已打折', [durTxt]));
  if (win === 1) parts.push(t('仅 1 个谱窗'));
  if (conf >= 40 && conf < 50) parts.push(tf('置信度擦线（{0}% ≈ 50% 阈值）', [conf]));
  // 2026-11-02：前缀是本函数拼的中文，拼完再进 DOM 就过了 i18n 的整串匹配 ⟹ 这里先过 t()
  var head = parts.length ? ('【' + parts.join(' · ') + '】 ') : '';
  return head + String(h.reason || '');
}

// 指标格：2 列 × N 行，每格 = 指标名 + 数值 + 单位（人话解释进 title，与旧「分析」页口径一致）
function anlKV(rows) {
  var h = '<div class=npanal-kv>';
  rows.forEach(function (r) {
    h += '<div class=r' + (r[3] ? ' title="' + esc(r[3]) + '"' : '') + '>' +
         '<span class=k>' + esc(r[0]) + '</span>' +
         '<span class=v>' + esc(String(r[1])) + (r[2] ? ' <i>' + esc(r[2]) + '</i>' : '') + '</span></div>';
  });
  return h + '</div>';
}
function anlCardHtml(id, title, sub, line, kv, note) {
  return '<div class=npanal-card id=' + id + '>' +
         '<div class=ct>' + esc(title) + '<i>' + esc(sub) + '</i></div>' +
         '<div class="npanal-verdict ' + (line.c || '') + '">' + esc(line.t) + '</div>' + kv +
         (note ? '<div class=hint style="margin-top:6px;line-height:1.55">' + esc(note) + '</div>' : '') +
         '</div>';
}

// ============================================================================
// P1（2026-10-27 用户 261027-05 A5）：分析卡**三级折叠**
// ----------------------------------------------------------------------------
// 用户原话：「我不想他默认展示，请你做折叠，折叠状态下只显示第一行的内容，比如动态一栏只显示
// 『中等动态 流行/摇滚常见』，音质一栏『干净，无明显削波』。用户可以展开，展开之后显示的就是现在
// 这个内容。然后用户可以继续点击进行 2 度展开，2 度展开就是我们上个版本提供的全部完整信息。
// 任何情况下，都可以重新折叠回去。」
//
//   L0（默认，折叠）= 每块**一行摘要**，三行。摘要全部由真实指标算出（下面三个 anlXxxBrief），
//                    不是字面量文案 —— 换一首歌，摘要必然跟着变（判据就在这）。
//   L1（展开一次）  = 上一版的内容：三块 + 2×2 指标格 + 一句结论。
//   L2（展开两次）  = 上一版的**完整信息**：quality 段全部原始指标 + 无损性判定依据 + note/caveat。
//
// 状态：ANL.lvl（0/1/2）。同一首歌跨重绘保持（暂停/拖动/重进单曲页都不重置）；
//       换歌在 anlUpdateCur() 里强制回 0（用户要求「换歌要回到 L0」）。
// ============================================================================
var ANL_LVLS = ['摘要', '详细', '全部指标'];

// 摘要复用展开结果的既有 DR 分档，避免同一份指标出现两个结论。
function anlDynBrief(h) {
  return anlDynLine(h||{});
}

// ② 音质摘要：0 段削波 + SNR 高 ⟹「干净」；有段 ⟹「有 N 段削波」。
// 口径与 L1/L2 完全一致（削波只认连续 ≥3 样本的 clipped_runs，clipped_pct 只当上界），
// 所以不会出现「摘要说干净、展开说有削波段」这种自相矛盾。
function anlQualBrief(h) {
  var q = (h && h.quality) || {};
  var F = qualFacts(h);
  if (!q.available) return { t: '样本不足 · 未出结论', c: 'mut' };
  if (F.noAudio) return { t: '素材无有效音频', c: 'mut' };
  var runs = Number(q.clipped_runs) || 0;
  var snr = Number(q.snr_db);
  if (runs > 0) return { t: tf('有 {0} 段削波 · 母带压过头', [anlNum(runs, 0)]), c: 'warn' };
  if (Number(q.clipped_pct) > 0) return { t: '仅零星单样本越限 · 非连续削波', c: '' };
  if (isFinite(snr) && snr > 0 && snr < 60) {
    return { t: tf('无削波 · SNR {0} dB（流行母带常见）', [anlNum(snr, 0)]), c: '' };
  }
  return { t: '干净 · 无明显削波', c: 'ok' };
}

// ③ 无损性摘要：四档（未发现 / 带宽受限 / 疑似有损 / 证据不足）—— 与 L1 的「判定」格同一份数据。
// P3（用户 261027-05 C4）：「有损检测就是假无损检测，归类到一起」⟹ 全界面只有这一处判定，
// 音质那一行**不再出现**「可能被低通/有损」之类的第二重口径。
function anlFakeBrief(h) {
  var fv = (h && h.fake_verdict) || (h && h.lossless_suspected ? 'lossy' : 'none');
  var cutHz = Number(h && h.cutoff_hz) || 0;
  var cut = cutHz > 0 ? (cutHz / 1000).toFixed(1) + ' kHz' : '';
  if (fv === 'lossy') return { t: tf('疑似有损 · 截止 {0}', [cut || '?']), c: 'warn' };
  if (fv === 'band_limited') return { t: tf('带宽受限 · 截止 {0}', [cut || '?']), c: '' };
  if (fv === 'unclear') return { t: '证据不足 · 不构成判据', c: 'mut' };
  return { t: tf('未发现 · 带宽正常', []) + (cut ? tf('（{0}）', [cut]) : ''), c: 'ok' };
}

// 分析卡里那一格「声道」：>2 声道时把布局名当值、把"N ch"当单位（2 列格子很窄，写全会被省略号吃掉）
function chCellVal(h) {
  var n = Number(h && h.channels) || 0;
  if (!n) return '—';
  return chMark(n) || String(n);
}
function chCellUnit(h) {
  var n = Number(h && h.channels) || 0;
  if (!n) return '';
  return chMark(n) ? (n + ' ch') : 'ch';
}

// L0 的单行卡：标题在左、摘要贴右，**只有一行**（折叠态的全部内容）
function anlL0Card(id, title, sub, brief) {
  var plain = String(title).replace(/^[\u2460-\u2462]\s*/, '');
  // 2026-11-02（多语言审计）：title 是属性，文本节点替换碰不到；而 plain 去掉了序号「②」，
  //   它本身不是词条 ⟹ 提示里永远是「音质：…」。改用带序号的整标题（有词条）+ 可译的冒号。
  var tt = (t(title) !== title) ? t(title) : t(plain);
  return '<div class="npanal-card l0" id=' + id + ' title="' + esc(tt + t('：') + t(brief.t)) + '">' +
         '<div class=ct>' + esc(title) + '<i>' + esc(sub) + '</i></div>' +
         '<div class="npanal-verdict ' + (brief.c || '') + '">' + esc(brief.t) + '</div></div>';
}

// 半截文件警示条（BUG-3，2026-10-27 修复轮）。
// 后端把「容器声明时长 ≫ 实际解码时长」的文件从 ready 降为 status="incomplete"，原因写进 h.note；
// ok 仍然是 true（**指标照给**，这不是失败），所以这里不是错误分支而是**黄色提示 + 照常出数**。
// 为什么必须显示：实测把 FLAC 截到 50% 后，dur=14.942 s（真值 32.046 s），DR/LUFS/LRA 三项
// 全都"看起来正常"；用户若在拷贝/下载过程中分析，会拿到一份只有一半的结论而毫不知情。
function anlIncompleteHtml(h) {
  if (!h || !(h.incomplete === true || h.status === 'incomplete')) return '';
  var d = Number(h.declared_sec) || 0, a = Number(h.duration_sec) || 0;
  var txt = h.note || tf('文件可能不完整：容器声明 {0} s，实际解码 {1} s', [d.toFixed(3), a.toFixed(3)]);
  return '<div class=npanal-card id=npAnlWarn style="grid-column:1/-1;' +
         'border-color:var(--warn,#e6a23c);background:rgba(230,162,60,.10)">' +
         '<div class=ct style="color:var(--warn,#e6a23c)">⚠ 文件可能不完整' +
         '<i>只分析了已存在的部分</i></div>' +
         '<div class="npanal-verdict warn" style="color:var(--warn,#e6a23c)">' + esc(txt) + '</div>' +
         (d > 0 ? ('<div class=hint style="line-height:1.55">容器声明 ' + d.toFixed(3) +
                   ' s ／ 实际可解码 ' + a.toFixed(3) + ' s（' + (100 * a / d).toFixed(1) + '%）' +
                   '　·　结果仅覆盖当前可读取的片段</div>') : '') +
         '</div>';
}

// 三类结果 → 卡片的 HTML（单曲页分析条与右键「查看分析」弹窗**共用**这一个渲染器）
// path：可选的曲目路径，只用于判断"DSD 源"（.dsf/.dff）；缺省时用 ANL.cur。
// lvl （2026-10-27 261027-05 A5）：0=折叠摘要（默认）/ 1=指标格 / 2=全部原始指标。
// 传别的值一律按 0 处理 —— 「默认不展示」是用户明确要求，任何调用方都不该意外把它撑开。
function anlCardsHtml(h, path, lvl) {
  lvl = (lvl === 1 || lvl === 2) ? lvl : 0;
  var q = (h && h.quality) || {};
  var F = qualFacts(h, path);
  var clip = qualClipText(q);
  // ===== L0：每块一行摘要（三行）。全部由真实指标算出，见 anlDynBrief/anlQualBrief/anlFakeBrief =====
  if (lvl === 0) {
    return anlIncompleteHtml(h) +
      anlL0Card('npAnlDyn', '① 动态', 'TT DR / R128', anlDynBrief(h)) +
      anlL0Card('npAnlQual', '② 音质', '客观指标', anlQualBrief(h)) +
      anlL0Card('npAnlFake', '③ 假无损', '有损检测', anlFakeBrief(h));
  }
  // ① 动态（2026-10-27 修复轮）：峰值一拆两格 —— 「样本峰值」永远是样本点上的值（≤0 dBFS），
  //   真的判断过载要看 4× 过采样「真峰 dBTP」。标定实测 09_reverie 样本峰 0.00、真峰 +2.5，
  //   旧界面只显示 0.00 dBFS 会把"过载 2.5 dB"说成"刚好满幅"（报告 §5）。
  var tp = Number(h.true_peak_dbtp);
  var tpCell = ['真峰', anlNum(h.true_peak_dbtp, 2), 'dBTP',
                '4× 过采样算出的样本间真峰（dBTP）。样本峰永远 ≤ 0 dBFS，真峰可以 > 0 —— 判"母带是否过载"只能看这一格'];
  var dynNote = '';
  if (isFinite(tp) && Number(h.peak_dbfs) > -299 && tp > -0.05) {
    dynNote = tf('真峰 {0} dBTP ⟹ 母带已过载{1}；样本峰值 {2} dBFS 只反映样本点',
            [(tp > 0 ? '+' : '') + tp.toFixed(2),
             (tp > 0.3 ? tf('（样本之间超出满幅 {0} dB，重采样/加 EQ 后会削波）', [tp.toFixed(1)]) : ''),
             anlNum(h.peak_dbfs, 2)]);
  }
  var dyn = anlCardHtml('npAnlDyn', '① 动态', 'TT DR / R128', anlDynLine(h), anlKV([
    ['DR', anlNum(h.dr_db, 1), 'dB', 'TT 动态范围：最响与最轻段落的电平差（同一母带内越大越活，跨曲目不可直接比）。' +
                                  '多声道素材取【较低声道】（保守口径）'],
    ['LUFS', anlNum(h.lufs, 1), 'LUFS', 'EBU R128 积分响度；−14 左右是常见投放目标'],
    ['LRA', anlNum(h.lra, 1), 'LU', 'R128 响度范围：段落间起伏，越大动态越丰富'],
    ['样本峰值', anlNum(h.peak_dbfs, 2), 'dBFS', '只看样本点的峰值，永远 ≤ 0 dBFS；不能用来判是否过载（看下一格）'],
    tpCell
  ]), dynNote);
  // ② 音质（2026-10-27 置信度审查后的口径）：
  //   * 2.1 IMD 不在卡里（主流程恒不可用）；
  //   * 2.4 削波 headline 只认段数；占比一律标「上界，含单样本越限」，稀疏单点越限要说清；
  //   * 2.2 无有效音频 ⟹ 谱类/SNR/峰值因子不出现，改为一句「素材无有效音频」；
  //   * 2.8 THD+N 音乐素材不展示（0/655 可用），只有单基频素材才多一格；
  //   * 2.6 谱四项在 fs < 44.1k / DSD 化素材上直接隐藏，并给"跨段落可差 1.8×/10 dB"的范围提示。
  var qualCells = [];
  if (!q.available) {
    qualCells.push(['状态', '样本不足', '', '素材太短，未出结论']);
  } else {
    if (F.noAudio) qualCells.push(['素材', '无有效音频', '', F.noAudioWhy + ' —— 这种素材上谱类指标会输出一整套"看起来有效"的 0 值']);
    qualCells.push(['削波', clip.head, '',
                    tf('判据（段数）：连续 ≥3 样本 ≥ −0.0087 dBFS 才算一段；{0}', [clip.sub])]);
    if (!F.noAudio) qualCells.push(['峰值因子', anlNum(q.crest_db, 1), 'dB', '峰值 − RMS；派生量，全静音/纯直流会是 0 dB']);
    var specWhy = qualSpectralHide(F);
    if (specWhy) {
      qualCells.push(['谱类四项', '不展示', '', specWhy + '（谱重心 / 倾斜 / 95% 滚降 / >10k 占比）']);
    } else {
      qualCells.push(['谱重心', anlNum(q.centroid_hz, 0), 'Hz',
                      '能量重心的频率位置。同一首不同段落可差 1.8×，跨版本比较必须同段']);
      qualCells.push(['>10k', anlNum(q.hf_ratio_10k_db, 1), 'dB',
                      '同一首不同段落可差 10 dB；低电平素材的下限被量化噪声抬高 ⟹ 跨版本比较必须同段']);
    }
    if (!F.noAudio) {
      qualCells.push(['信噪比', anlNum(q.snr_db, 1), 'dB',
                      '相对同曲版本可比，不是器材信噪比指标（本底 = 100 ms 块 P10 = 最安静段落的音乐电平）']);
    }
    if (q.thdn_available) {
      qualCells.push(['THD+N', anlNum(q.thdn_db, 1), 'dB',
                      '只在单基频（测试音）上成立；16 bit 可测下限 −94 dB@0 dBFS / −37 dB@−60 dBFS']);
    }
    if (q.stereo && Number(q.channel_corr) > 0.99995 && Number(q.mono_loss_db) < 0.05) {
      qualCells.push(['立体声', '左右完全相同', '', '双声道但左右样本完全相同 = 等同单声道（库内 4 首如此），不是"立体声正常"']);
    }
  }
  var qual = anlCardHtml('npAnlQual', '② 音质', '客观指标', anlQualLine(h), anlKV(qualCells));
  // ③ 假无损的「判定」格改成四档文字（不再只有 疑似有损 / 未发现 两种），并把 reason 显示到卡上，
  //   另加「陡沿」一格 —— 它才是判有损的核心量，报告 §3 里"检出但边沿不陡"的场合全靠它说清。
  var VERD = { lossy: '疑似有损', band_limited: '带宽受限（不判假）', unclear: '证据不足', none: '未发现' };
  var fv = h.fake_verdict || (h.lossless_suspected ? 'lossy' : 'none');
  var win = Number(h.spectral_windows) || 0;
  var fake = anlCardHtml('npAnlFake', '③ 假无损', '有损检测 · 唯一判定处', anlFakeLine(h), anlKV([
    ['判定', VERD[fv] || VERD.none, '', '按截止位置（相对奈奎斯特）+ 谱峭壁 + 置信度联合判断：' +
     '截止 <78% ⟹ 疑似有损；78%~95% ⟹ 有效带宽受限（母带低通/升频，不判假）；"未发现"不等于绝对无损'],
    ['截止', h.cutoff_hz > 0 ? (h.cutoff_hz / 1000).toFixed(1) : '未检出', h.cutoff_hz > 0 ? 'kHz' : '',
     '能量真正结束的位置；CD 无损可到 ~20 kHz，128 kbps MP3 常在 15~16 kHz 断崖。未检出 = 没有明显陡沿，不是"没有高频"'],
    ['陡沿', h.edge_db > 0 ? Math.round(h.edge_db) : '—', h.edge_db > 0 ? 'dB' : '',
     '截止处 1/6 倍频程的能量落差（各谱窗取中位数）。>12 dB 才算陡；编码器实测 26~60 dB，老录音自然滚降只有 10~20 dB'],
    ['置信度', anlNum((h.confidence || 0) * 100, 0), '%',
     '判定强度（陡沿深度 × 各谱窗一致率）；<50% 时不给确定结论' + (win ? ('；本次用了 ' + win + ' 个谱窗') : '')],
    ['格式', (h.bits || '—') + ' bit', h.fs ? (h.fs >= 1000 ? (h.fs / 1000).toFixed(1) + ' kHz' : h.fs + ' Hz') : '',
     '容器里声明的位深 / 采样率（不等于母带质量）；另：fake24 标记 = 标称 24 bit 但有效位深不足'],
    // P2（用户 261027-05 A4「多声道就都标记出来就好了」）：分析卡里也标 —— 之前这一格完全没有声道，
    // 6 声道文件在卡上看起来和 2.0 一模一样。>2 声道一律带布局名（5.1 / 7.1 / 6ch）。
    ['声道', chCellVal(h), chCellUnit(h),
     '容器里声明的声道数；>2 声道会在后面标出布局名（5.1 / 7.1 / 6ch）。多声道素材的 DR 取【较低声道】（保守口径）']
  ]), anlFakeNote(h));
  // 不完整警示条排在最前：用户先看到"这份结论只有一部分"，再去看数字（顺序即语义）
  var out = anlIncompleteHtml(h) + dyn + qual + fake;
  // L2（再展开一次）= 上一版的**完整信息**：全部原始指标 + 无损性判定依据 + note/caveat
  if (lvl >= 2) out += anlDeepHtml(h, path);
  return out;
}

// ---------------------------------------------------------------------------
// L2 的正文：**上一版的全部完整信息**（原来那份「分析音质（19 项）」弹窗的内容，一字不减）
// 数据全部来自 /api/health 的 quality 段，前端一个数都不改（判据/口径都在 src/analysis/quality.h）。
// 为什么排成组：19 项平铺成一长列没人看得下去；分组后每组 4~8 格，2 列排布 ⟹ 一屏能读完。
// ---------------------------------------------------------------------------
function anlDeepGroup(title, rows) {
  return '<div class=npanal-det-g><div class=dh>' + esc(title) + '</div>' + anlKV(rows) + '</div>';
}
function anlDeepHtml(h, path) {
  var q = (h && h.quality) || {};
  var F = qualFacts(h, path);
  var clip = qualClipText(q);
  var specHide = qualSpectralHide(F);
  var shortDur = (F.dur > 0 && F.dur < 6);
  var parts = [];
  // 1. 动态 / 响度
  parts.push(anlDeepGroup('动态 / 响度（原始值）', [
    ['DR', anlNum(h.dr_db, 2), 'dB', 'TT 动态范围：最响与最轻段落的电平差；同一母带内越大越活，跨曲目不可直接比。多声道取较低声道'],
    ['LUFS', anlNum(h.lufs, 2), 'LUFS', 'EBU R128 积分响度；−14 左右是常见投放目标'],
    ['LRA', anlNum(h.lra, 2), 'LU', 'R128 响度范围：段落间起伏'],
    ['样本峰值', anlNum(h.peak_dbfs, 3), 'dBFS', '样本点峰值，永远 ≤ 0 dBFS（判过载要看真峰）'],
    ['真峰', anlNum(h.true_peak_dbtp, 2), 'dBTP', '4× 过采样样本间真峰；> 0 表示母带已过载'],
    ['动态 P95−P10', shortDur ? '不展示' : anlNum(q.dyn_p95_p10_db, 1), shortDur ? '' : 'dB',
     shortDur ? '素材 < 6 s：3 s 块至少要 2 块，短素材恒为 0' : '段落间起伏；含数字静音时偏大'],
    ['动态重心', anlNum(q.dyn_centroid_dbfs, 1), 'dBFS', '动态分布的加权中心（≈ 全程听感电平）'],
    ['静态段占比', anlNum(q.silence_pct, 2), '%', '低于 −60 dBFS 的 100 ms 块占比']
  ]));
  // 2. 失真 / 噪声
  parts.push(anlDeepGroup('失真 / 噪声', [
    ['THD+N', q.thdn_available ? anlNum(q.thdn_db, 1) : '不展示', q.thdn_available ? 'dB' : '',
     q.thdn_available ? ('单基频素材（测试音）上成立；16 bit 可测下限 −94 dB@0 dBFS / −37 dB@−60 dBFS')
                      : '真实曲库 0/655 可用：音乐没有可定义的基频（全部 multi_peak）⟹ 只在测试音场景显示'],
    ['非谐波残留', anlNum(q.non_tonal_db, 1), 'dB', '16 个最强谱峰之外的能量占比 = 噪声/失真/有损残留的上界'],
    ['本底噪声', anlNum(q.noise_floor_dbfs, 1), 'dBFS', '100 ms 块 P10 = 最安静段落的那段音乐电平，不是器材噪声底'],
    ['谱最小本底', anlNum(q.spectral_min_dbfs, 1), 'dBFS', '诊断用：平稳噪声准，密集音乐偏高'],
    ['信噪比', F.noAudio ? '不展示' : anlNum(q.snr_db, 1), F.noAudio ? '' : 'dB',
     '相对同曲版本可比，**不是**器材信噪比指标（本底取最安静段落）'],
    ['峰值因子', F.noAudio ? '不展示' : anlNum(q.crest_db, 1), F.noAudio ? '' : 'dB',
     '峰值 − RMS；全静音/纯直流会是 0 dB']
  ]));
  // 3. 频响
  parts.push(specHide
    ? anlDeepGroup('频响（不展示）', [['谱类四项', '不展示', '', specHide + '（谱重心 / 谱倾斜 / 95% 滚降 / >10k 占比）']])
    : anlDeepGroup('频响', [
        ['谱重心', anlNum(q.centroid_hz, 0), 'Hz', '能量重心的频率位置；同一首不同段落可差 1.8×'],
        ['谱倾斜', anlNum(q.tilt_db_per_oct, 2), 'dB/oct', '频谱倾斜；同一首不同段落会变'],
        ['95% 滚降点', anlNum(q.rolloff95_hz, 0), 'Hz', '累计能量到 95% 的频率；20 s vs 60 s 可差 13%'],
        ['>10 kHz 占比', anlNum(q.hf_ratio_10k_db, 1), 'dB', '同一首不同段落可差 10 dB；低电平素材被量化噪声抬高']
      ]));
  // 4. 削波 / 立体声
  parts.push(anlDeepGroup('削波 / 立体声', [
    ['削波段数（判据）', clip.head, '', '只有 clipped_runs / longest_clip_run 能当"真削波"的判据（连续 ≥3 样本 ≥ −0.0087 dBFS）'],
    ['削波占比（上界）', clip.sub, '', '门限 0.999 = −0.0087 dBFS：合法满幅正弦也会被计入 ⟹ 只能当上界'],
    ['削波样本', anlNum(q.clipped_samples, 0), '个', '含单样本越限（上界口径）'],
    ['最长连续', anlNum(q.longest_clip_run, 0), '样本', '最长的一段连续削波；< 3 样本不算一段'],
    ['峰值 / RMS', anlNum(q.peak_dbfs_q, 2) + ' / ' + anlNum(q.rms_dbfs, 2), 'dBFS', 'quality 段的峰值/RMS（与上面 DR 段同一份解码）'],
    ['声道相关', q.stereo ? anlNum(q.channel_corr, 4) : '非立体声', '', '1.0000 = 左右完全相同（等同单声道）；< −0.9 可能是极性反相'],
    ['单声道损失', q.stereo ? anlNum(q.mono_loss_db, 2) : '—', q.stereo ? 'dB' : '', qualStereoText(q)],
    ['side / mid', q.stereo ? anlNum(q.side_mid_db, 2) : '—', q.stereo ? 'dB' : '', '0 dB 也可能只是单边静音，不一定是"完全中置"']
  ]));
  // 5. 无损性判定依据（四档 + 判据量；P3：全界面唯一判定处）
  var VERD2 = { lossy: '疑似有损', band_limited: '带宽受限（不判假）', unclear: '证据不足', none: '未发现' };
  var fv2 = h.fake_verdict || (h.lossless_suspected ? 'lossy' : 'none');
  parts.push(anlDeepGroup('无损性判定依据（四档 · 唯一判定处）', [
    ['判定', VERD2[fv2] || VERD2.none, '', '截止 <78% 奈奎斯特 ⟹ 疑似有损；78%~95% ⟹ 带宽受限（不判假）；置信 <50% ⟹ 证据不足'],
    ['截止', h.cutoff_hz > 0 ? (h.cutoff_hz / 1000).toFixed(2) : '未检出', h.cutoff_hz > 0 ? 'kHz' : '', '能量真正结束的位置；未检出 = 没有明显陡沿，不是"没有高频"'],
    ['陡沿', h.edge_db > 0 ? anlNum(h.edge_db, 1) : '—', h.edge_db > 0 ? 'dB' : '', '截止处 1/6 倍频程落差（谱窗中位数）；>12 dB 才算陡'],
    ['置信度', anlNum((h.confidence || 0) * 100, 0), '%', '陡沿深度 × 各谱窗一致率；<50% 不给确定结论'],
    ['谱窗数', anlNum(h.spectral_windows, 0), '个', '参与判定的谱窗个数；只有 1 个时结论很弱'],
    ['谱窗填充', anlNum((h.window_fill === undefined ? 1 : h.window_fill) * 100, 0), '%', '有实样的谱窗比例；<50% 时只作参考'],
    ['fake24', h.fake24 ? '是' : '否', '', '标称 24 bit 但有效位深不足（把 16 bit 塞进 24 bit 容器）'],
    ['格式', (h.bits || '—') + ' bit', h.fs ? (h.fs >= 1000 ? (h.fs / 1000).toFixed(1) + ' kHz' : h.fs + ' Hz') : '',
     '容器里声明的位深 / 采样率（不等于母带质量）'],
    ['声道', chText(h.channels), '', '容器里声明的声道数 + 布局（5.1 / 7.1 …）；多声道素材 DR 取较低声道']
  ]));
  // 6. 说明与限定（后端 reason + 前端前置限定词 + 共有的 caveat）
  var notes = [];
  var fakeNote = anlFakeNote(h);
  if (fakeNote) notes.push(tf('无损性依据：{0}', [fakeNote]));
  if (h.note) notes.push(tf('文件完整性：{0}', [t(h.note)]));
  if (q.note) notes.push(tf('quality 备注：{0}', [t(q.note)]));   // 备注文本本身也要过一遍词典（后端给的是中文原串）
  notes.push('谱类四项同一首不同段落可差 1.8× / 10 dB ⟹ 跨版本比较必须同段；轻度的谐波失真本页测不出（IMD 需双音素材）。');
  return '<div class=npanal-det id=npAnlFull>' +
    '<div class=dh>全部原始指标 · 判定依据' +
      '<i>quality 段全部字段 + 无损性四档依据（数据来自 /api/health，前端不改一个数）</i></div>' +
    parts.join('') +
    '<div class=note>' + notes.map(function (s) { return esc(s); }).join('<br>') + '</div>' +
    '</div>';
}

// 骨架（分析中）：三块**保留各自的标题**（动态/音质/假无损），只把数字换成灰条 ——
// 这样"正在算什么"是明确的，也不会出现假数字（铁律 3：不许用假数据充数）。
function anlSkeletonHtml() {
  function one(title, sub) {
    return '<div class=npanal-card><div class=ct>' + esc(title) + '<i>' + esc(sub) + '</i></div>' +
           '<div class=npanal-skel style="width:58%;height:11px"></div>' +
           '<div class=npanal-skel style="width:92%;margin-top:7px"></div>' +
           '<div class=npanal-skel style="width:80%;margin-top:7px"></div></div>';
  }
  return one('① 动态', 'TT DR / R128') + one('② 音质', '客观指标') + one('③ 假无损', '有损转码检测');
}

function anlEmptyHtml(msg) {
  return '<div class="npanal-card" style="grid-column:1/-1;align-items:center;justify-content:center;color:var(--mut2);' +
         'font-size:11.5px;padding:14px">' + esc(msg) + '</div>';
}

// 状态行（只在状态变化或 250 ms 心跳时更新文字，不重建卡片 ⟹ 不闪）
function anlStatusText() {
  var p = ANL.cur;
  if (!p) return '播放一首歌就会自动分析一次';
  var nm = String(p).replace(/^.*[\\\/]/, '');
  if (ANL.busy[p]) return tf('正在分析 · {0} · 已 {1} s', [nm, (ANL.elapsed / 1000).toFixed(1)]);
  if (ANL.cache[p]) {
    // 三种来源必须分得清（判据「第二次 <100ms 或 cached=true」就靠这一行）：
    //   front   = 同一页面里的前端缓存（连 /api/health 都没打）
    //   backend = 打了后端但命中进程内缓存（cached=true，0 ms）
    //   new     = 真算了一遍
    var t = ANL.elapsed >= 1000 ? (ANL.elapsed / 1000).toFixed(2) + ' s' : anlNum(ANL.elapsed, 0) + ' ms';
    if (ANL.src === 'front') return tf('{0} · 已缓存（本次未重算）', [nm]);
    if (ANL.src === 'backend') return tf('{0} · 后端缓存命中 · {1}', [nm, t]);
    return tf('{0} · 首次计算 · {1}', [nm, t]);
  }
  if (ANL.err) return tf('分析失败：{0}', [ANL.err]);
  if (!ANL.auto) return '自动分析已关闭，可在专业模式设置中打开';
  return tf('等待分析 · {0}', [nm]);
}

// ---- 折叠控件（P1）：三级之间的按钮由这里生成/回收 ----
// 为什么在 JS 里建而不是写在 ui_page.h 的 markup 里：分析卡的 markup 是公共骨架，
// 本轮只被允许往 ui_page.h 追加**CSS**（另一条线同时在改别的区域）。放这里也保证
// 「没有结果时控件不存在」—— 空面板上摆一个「展开」按钮是骗人的。
function rlStudioMonitorHTML(){
  return '<p class=digitalFootnote>'+esc(t('以下指标反映音源文件，不是处理后的实时输出。'))+'</p><p class=digitalFootnote id=rlMonitorCueNote hidden>'+esc(t('当前播放为分轨片段；音质分析针对源文件。'))+'</p><div class=rlMonitorControls><label><input id=rlMonitorEnabled type=checkbox>'+esc(t('启用音质监控'))+'</label><label><input id=rlMonitorAuto type=checkbox>'+esc(t('播放时自动分析'))+'</label><button type=button class=btn id=npAnlNow hidden>'+esc(t('重试'))+'</button></div>'
    +'<div class=npanal id=npAnal data-studio-monitor=1 data-state=idle><div class=npanal-line id=npAnlCtl aria-label="'+esc(t('音频分析'))+'">'
    +[['npAnlDyn','动态'],['npAnlQual','音质'],['npAnlFake','假无损']].map(function(item){return '<button type=button class=npanal-summary id='+item[0]+'Summary aria-controls=npAnlGrid><span>'+esc(t(item[1]))+'</span><strong>'+esc(t('等待分析'))+'</strong></button>';}).join('')
    +'<button type=button class=npanal-btn id=npAnlMore aria-controls=npAnlGrid>'+esc(t('收起'))+'</button></div><div id=npAnlSt class=npanal-status role=status></div><div class=npanal-grid id=npAnlGrid></div></div>';
}
function rlAttachStudioMonitor(){
  if(!D('npAnal'))return;
  ANL.lvl=2;
  if(D('rlMonitorEnabled'))D('rlMonitorEnabled').onchange=function(){anlSetMaster(this.checked);rlPaintStudioMonitor();};
  if(D('rlMonitorAuto'))D('rlMonitorAuto').onchange=function(){anlSetAuto(this.checked);rlPaintStudioMonitor();};
  anlBindUi();rlPaintStudioMonitor();
}
function rlPaintStudioMonitor(frame){
  var file=frame&&frame.file||ANL.cur,current=rlLastStatus&&rlLastStatus.current===file?rlLastStatus:typeof rlConsole!=='undefined'&&rlConsole.state&&rlConsole.state.file===file?rlConsole.state:null;
  if(D('rlMonitorCueNote'))D('rlMonitorCueNote').hidden=!(current&&Number.isFinite(current.cueStart)&&current.cueStart>=0);
  if(frame&&frame.file!==undefined)anlUpdateCur(frame.file||'');
  if(D('rlMonitorEnabled'))D('rlMonitorEnabled').checked=!!ANL.master;
  if(D('rlMonitorAuto')){D('rlMonitorAuto').checked=!!ANL.auto;D('rlMonitorAuto').disabled=!ANL.master;}
  if(D('npAnlNow')){D('npAnlNow').disabled=!ANL.master||!ANL.cur||!!ANL.busy[ANL.cur];D('npAnlNow').hidden=!ANL.master||!ANL.cur||!ANL.err;}
  anlPaint();
}
function anlCtlBar() {return D('npAnlCtl');}
function anlSyncCtl(has) {
  var box=D('npAnal'),bar=D('npAnlCtl'),grid=D('npAnlGrid');
  if(!box||!bar)return;
  var expanded=has&&ANL.lvl>0;
  box.dataset.lvl=expanded?'2':'0';
  if(grid)grid.hidden=has&&!expanded;
  var labels=['动态','音质','假无损'],ids=['npAnlDyn','npAnlQual','npAnlFake'];
  var result=has&&ANL.cache[ANL.cur],busy=ANL.cur&&ANL.busy[ANL.cur];
  var brief=result?[anlDynBrief(result),anlQualBrief(result),anlFakeBrief(result)]:[];
  labels.forEach(function(label,i) {
    var node=D(ids[i]+'Summary'),value=node&&node.querySelector('strong');
    if(!node||!value)return;
    var message=brief[i]?t(brief[i].t):t(busy?'分析中':ANL.err?'分析失败':ANL.auto?'等待分析':'未分析');
    value.textContent=message;value.className=brief[i]?brief[i].c||'':'mut';
    node.title=t(label)+' · '+message+(result&&result.incomplete?' · '+t('文件可能不完整'):'');
    node.disabled=!has;node.setAttribute('aria-expanded',String(expanded));
    node.onclick=function(){ANL.lvl=2;anlPaint();var target=D(ids[i]);if(target)target.scrollIntoView({block:'nearest'});};
  });
  var button=D('npAnlMore');
  if(button){
    button.textContent=t(expanded?'收起':'展开');
    button.disabled=!has;button.setAttribute('aria-expanded',String(expanded));
    button.onclick=function(){anlSetLvl(expanded?0:2);};
  }
  if(result&&result.incomplete){var q=D('npAnlQualSummary');if(q){q.querySelector('strong').textContent=t('文件可能不完整');q.querySelector('strong').className='warn';}}
}
// 唯一的级别写入口：任何地方改级别都走它（并立即重绘）
function anlSetLvl(n) {
  n = (n === 1 || n === 2) ? n : 0;
  if (((ANL.lvl === 1 || ANL.lvl === 2) ? ANL.lvl : 0) === n) return;
  ANL.lvl = n;
  anlPaint();
}

// 2026-10-28：模块总开关关掉时把整块藏起来（连"自动分析已关闭"的提示框都不留）。
// 隐藏而不是删 DOM：用户重新打开时 anlPaint 会把它显示回来，不用重建节点。
function anlHideCard() {
  var box = D('npAnal');
  if (box) { box.style.display = 'none'; box.dataset.master = '0'; }
  var grid = D('npAnlGrid');
  if (grid) grid.innerHTML = '';
  var st = D('npAnlSt');
  if (st) st.textContent = '';
}

function anlPaint(full) {
  var grid = D('npAnlGrid');
  if (!grid) return;
  // 总开关关掉 ⟹ 整块不渲染（也不刷新状态行），避免"关了还杵着一个框"。
  var boxM = D('npAnal');
  if (!ANL.master) { anlHideCard(); return; }
  if (boxM && boxM.dataset.master === '0') { boxM.style.display = ''; boxM.dataset.master = '1'; }
  var p = ANL.cur;
  var h = p ? ANL.cache[p] : null;
  var busy = !!(p && ANL.busy[p]);
  if(D('npAnlNow')){D('npAnlNow').hidden=!ANL.err||!p;D('npAnlNow').disabled=busy;}
  var box = D('npAnal');
  if (box) box.dataset.state = !p ? 'idle' : (busy ? 'busy' : (h ? 'ready' : (ANL.err ? 'error' : 'idle')));
  var st = D('npAnlSt');
  if (st) st.textContent = anlStatusText();
  var btn = D('npAnlAuto');
  if (btn) {
    btn.classList.toggle('on', !!ANL.auto);
    btn.textContent = ANL.auto ? t('自动分析：开') : t('自动分析：关');
    btn.title = ANL.auto ? '关掉后播放不再自动分析（也可以在 设置-高级 改）'
                         : '打开后每次播放新的一首都会自动分析一次（也可以在 设置-高级 改）';
  }
  anlSyncCtl(!!h);                 // 折叠控件：有结果才出现（心跳也要刷，否则收起后按钮会残留）
  if (full === false) return;      // 心跳只刷状态行
  if (busy) { grid.innerHTML = anlSkeletonHtml(); return; }
  if (h) { grid.innerHTML = ANL.lvl>0?anlCardsHtml(h,p,2):''; return; }
  if (!p) { grid.innerHTML = anlEmptyHtml('还没有在播放 · 播放一首本地曲目就会自动分析（动态 / 音质 / 假无损）'); return; }
  if (ANL.err) { grid.innerHTML = anlEmptyHtml(tf('分析失败：{0}', [ANL.err])); return; }
  if (!ANL.auto) {
    grid.innerHTML = anlEmptyHtml('自动分析已关闭，可在专业模式设置中打开');
    return;
  }
  grid.innerHTML = anlEmptyHtml(tf('等待分析 · {0}', [String(p).replace(/^.*[\\\/]/, '')]));
}

// 发起一次分析（异步；调用方只需给路径）
function anlRun(path, force) {
  path = String(path || '');
  if (!path) return;
  // 2026-10-28：总开关关掉时一律不发起（自动和手动都算）——
  // 用户关掉模块的语义是"别动我的 CPU、别读我的文件"，不是"少读几次"。
  if (!ANL.master) return;
  if (ANL.busy[path]) return;
  if (!force && ANL.cache[path]) { ANL.src = 'front'; anlPaint(); return; }
  ANL.busy[path] = true;
  ANL.err = '';
  ANL.t0 = Date.now();
  ANL.elapsed = 0;
  if (ANL.cur !== path) { ANL.cur = path; }
  anlPaint();
  var url = '/api/health?file=' + encodeURIComponent(path) + (force ? '&fresh=1' : '');
  anlFetch(url, undefined, 240000).then(function (j) {
    ANL.busy[path] = false;
    var h = (j && j.health) || null;
    if(j&&j.ok&&h)ANL.cache[path]=h;
    if(ANL.cur!==path)return;
    ANL.elapsed = Date.now() - ANL.t0;
    if (typeof j.runs === 'number') ANL.stat.runs = j.runs;
    if (typeof j.hits === 'number') ANL.stat.hits = j.hits;
    if (!j || !j.ok || !h) {
      ANL.err = (j && j.error) || (h && h.error) || '未知原因';
      ANL.cached = false;
      ANL.src = '';
    } else {
      ANL.cache[path] = h;
      ANL.cached = !!j.cached;
      ANL.src = j.cached ? 'backend' : 'new';
      // elapsed 保留**前端实测的往返耗时**（缓存命中时后端 ms=0，网络往返仍有 ~10 ms）——
      // 判据要的正是这个"用户实际等待时间"，所以不把它抹成 0。
      ANL.err = '';
    }
    if (ANL.cur === path) anlPaint();
  }).catch(function (e) {
    ANL.busy[path] = false;
    if(ANL.cur!==path)return;
    ANL.elapsed = Date.now() - ANL.t0;
    ANL.err = (e && e.message ? e.message : String(e));
    if (ANL.cur === path) anlPaint();
  });
}

// 由 tick() 每 400 ms 驱动：换歌 / 开始播放 → 自动分析一次（按路径去重）
function anlUpdateCur(cur) {
  cur = String(cur || '');
  if (ANL.cur !== cur) {
    ANL.cur = cur;
    ANL.autoDone = '';
    ANL.err = '';
    ANL.cached = false;
    ANL.src = '';
    ANL.elapsed = 0;
    // P1（用户 261027-05 A5「换歌要回到 L0」）：换歌一律折叠回摘要。
    // 只在这一处归零 ⟹ 同一首歌的暂停/拖动/重绘/重进单曲页都保持用户选过的级别。
    ANL.lvl = D('npAnal')&&D('npAnal').dataset.studioMonitor==='1'?2:0;
    anlPaint();                       // 换歌：立刻把面板切到「这首歌」
  }
  if (!cur || !ANL.auto) return;
  if (ANL.autoDone === cur) return;   // ★ 去重：同一首只自动触发一次
  ANL.autoDone = cur;
  if (!anlIsLocal(cur)) return;       // 流媒体 / UNC / CD 音轨：不自动跑
  if (ANL.cache[cur]) {               // 前端已有结果：一次请求都不打（状态行要如实标「已缓存」）
    ANL.src = 'front'; ANL.elapsed = 0; anlPaint(); return;
  }
  anlRun(cur, false);
}

// 2026-10-28（用户 2.0 前要求）：模块**总开关**。
// 关掉 = 单曲页整块消失 + 不发任何 /api/health + 右键菜单没有分析入口。
function anlSetMaster(on) {
  ANL.master = on ? 1 : 0;
  ADV.anlMaster = ANL.master;
  var c = D('sp_anlmaster');
  if (c) c.checked = !!ANL.master;
  saveSettings();
  if (!ANL.master) {
    anlHideCard();
  } else {
    var box = D('npAnal');
    if (box) { box.style.display = ''; box.dataset.master = '1'; }
    anlPaint();
    // 重新打开且当前有本地曲目、又没分析过 ⟹ 补一次（只在 auto 也开着时才跑，不改 auto 语义）
    if (ANL.auto && ANL.cur && !ANL.cache[ANL.cur] && anlIsLocal(ANL.cur)) anlRun(ANL.cur, false);
  }
  var m2 = D('sp_anlmsg');
  if (m2) m2.textContent = analysisAutoStatus();
}

// 开关（单曲页按钮 / 设置-高级 / 都走这里，写同一个 adv 键并落盘）
function anlSetAuto(on) {
  ANL.auto = on ? 1 : 0;
  ADV.autoAnl = ANL.auto;
  var c = D('sp_autoanl');
  if (c) c.checked = !!ANL.auto;
  saveSettings();
  if (ANL.auto && ANL.cur) {          // 刚打开：当前这首立刻补一次（只有当它还没被分析过）
    if (ANL.autoDone !== ANL.cur) ANL.autoDone = ANL.cur;
    if (!ANL.cache[ANL.cur] && anlIsLocal(ANL.cur)) anlRun(ANL.cur, false);
  }
  anlPaint();
}

// 右键「查看分析（动态/音质/无损性）」：不在播放的曲目也能看。
// P3（2026-10-27 用户 261027-05 C4「有损检测就是假无损检测，归类到一起就行」）：
//   旧版有两个入口 ——「查看分析（动态/音质/假无损）」给三块卡，「分析音质（19 项）」另给一张长表，
//   两处都对"无损/有损"下结论（口径还不完全一样）⟹ 合并成这**一个**入口。
//   弹窗固定按 L2 打开（三块卡 + 全部原始指标 + 判定依据），信息一个都不少，只是不再分两处。
function openHealthModal(path) {
  if (!path) { toast('没有可分析的曲目'); return; }
  openModal('音频分析', '<div class=empty>正在分析…（流式分块解码，曲越长越久）</div>');
  var t0 = Date.now();
  anlFetch('/api/health?file=' + encodeURIComponent(path), undefined, 240000).then(function (j) {
    var h = (j && j.health) || null;
    if (!j || !j.ok || !h) {
      D('mbody').innerHTML = '<div class=empty>' + esc((j && j.error) || (h && h.error) || '分析失败') + '</div>';
      return;
    }
    ANL.cache[path] = h;
    var took = j.cached ? 0 : (Date.now() - t0);
    D('mbody').innerHTML =
      '<div class=hint style="margin-bottom:8px;word-break:break-all">' + esc(path) + '</div>' +
      '<div class=hint style="margin-bottom:10px">用时 ' +
        (took >= 1000 ? (took / 1000).toFixed(2) + ' s' : took + ' ms') +
        (j.cached ? '（后端缓存命中）' : '（本次新算）') +
        (h.reason ? '　·　' + esc(h.reason) : '') + '</div>' +
      '<div style="display:grid;grid-template-columns:1fr;gap:10px">' + anlCardsHtml(h, path, 2) + '</div>';
  }).catch(function (e) {
    D('mbody').innerHTML = '<div class=empty>分析失败：' + esc(e && e.message ? e.message : e) + '</div>';
  });
}

// 单曲页分析条的按钮绑定（元素随页面一起生成，绑定一次即可）
function anlBindUi() {
  var b = D('npAnlAuto');
  if (b) b.onclick = function () { anlSetAuto(!ANL.auto); };
  var r = D('npAnlNow');
  if (r) r.onclick = function () {
    var p = ANL.cur || (S.cur || '');
    if (!p) { toast('没有正在播放的曲目'); return; }
    anlRun(p, true);
  };
  if (D('npAnlGrid') && !ANL.cur) anlPaint();
}
anlBindUi();
// 「分析中」心跳：只刷状态行上的秒数（不重建卡片 ⟹ 骨架动画不闪）
setInterval(function () {
  if (!D('npAnal')) return;
  var p = ANL.cur;
  if (!p || !ANL.busy[p]) return;
  ANL.elapsed = Date.now() - ANL.t0;
  anlPaint(false);
}, 250);

// ============================================================================
// 关于与更新（261027-02 #5；后端 = src/net/update.h + src/net/update_apply.h）
// ----------------------------------------------------------------------------
// 端点契约：
//   GET  /api/update/check?force=1   **异步**：立刻回 {checking:true}，本页每 700 ms 轮询同一 URL
//   GET  /api/update/progress        下载进度（phase/downloaded/total/pct/speed/error）
//   GET  /api/update/state           当前版本 / 跳过版本 / 暂存 / 台账（上次更新结果）
//   POST /api/update/download        增量下载：只下 sha256 与本机不一致的文件
//   POST /api/update/apply           写台账 + 让程序退出；**重新打开 RLPlayer 才换文件**
//   POST /api/update/skip            {"version":"1.6.0"}；空串 = 取消跳过
// 为什么检查必须异步：后端 WebUi 是单线程 accept，一次清单往返最长 8 s，
//   同步做会把整个界面（含 /api/status、/api/ping）一起排住。
// 为什么 apply 之后要重启：rtaweb.exe 与 dll 正被本进程映射，运行中无法覆盖自己；
//   真正的替换发生在下次启动器启动、rtaweb 还没起来的那一刻（见 update_apply.h 头注）。
// ============================================================================
var UPD = { phase: 'idle', applyArm: false };

function updNum(n) {
  if (!n || n <= 0) return '—';
  if (n >= 1048576) return (n / 1048576).toFixed(1) + ' MB';
  if (n >= 1024) return (n / 1024).toFixed(1) + ' KB';
  return n + ' B';
}
function updSet(id, text) { var e = D(id); if (e) e.textContent = text; }

function updShowCheck(j) {
  if (!j) j = {};
  if (j.current) updSet('sp_updcur', j.current);
  if (UPD.recoveryBlocked) return;
  UPD.manualCn = j.cn || j.cnUrl || UPD.manualCn || '';
  UPD.manualGlobal = j.global || j.globalUrl || UPD.manualGlobal || 'https://github.com/okhuoyanyan/RLPlayer2/releases';
  UPD.manualCode = j.extractionCode || UPD.manualCode || '';
  var card = D('sp_updcard');
  var dl = D('sp_upddl'), cn = D('sp_updcn'), global = D('sp_updglobal'), apply = D('sp_updapply');
  if (!j.ok) {
    var why = updWhy(j);
    // 2026-11-16：updWhy 之后 why 已是本地化文案，quota_exceeded 字样不一定还在 ⟹
    //   判据要同时看 errorCode 与后端原始 error（旧前端契约就是靠这串字判的）。
    if (String(j.errorCode || '') === 'quota_exceeded' || String(j.error || '').indexOf('quota_exceeded') >= 0) {
      if (card) card.style.display = '';
      updSet('sp_updver', j.latest ? ('v' + j.latest) : '—');
      updSet('sp_updplan', t('服务器本月增量配额已用完，请到百度网盘或 GitHub 下载完整版，或下月重试。'));
      updSet('sp_updnotes', ''); updSet('sp_updfiles', UPD.manualCode ? tf('百度网盘提取码：{0}', [UPD.manualCode]) : '');
      if (dl) { dl.style.display = 'none'; dl.disabled = true; }
      if (apply) apply.style.display = 'none';
      if (cn) { cn.style.display = UPD.manualCn ? '' : 'none'; cn.disabled = !UPD.manualCn; }
      if (global) { global.style.display = ''; global.disabled = false; }
    } else if (card) card.style.display = 'none';
    updSet('sp_updstate', tf('检查失败：{0}', [why]));
    return;
  }
  if (!j.hasUpdate) {
    if (card) card.style.display = 'none';
    updSet('sp_updstate', j.upToDate ? (tf('已是最新版本 v{0}', [(j.current || j.latest)])) : ('当前 v' + j.current));
    return;
  }
  if (card) card.style.display = '';
  updSet('sp_updver', 'v' + j.latest);
  var manual = j.installMode === 'manual' || j.install_mode === 'manual';
  if (manual) {
    var reason = j.reason === 'major-upgrade' ? t('跨大版本更新需要重新安装完整版。') :
      (j.reason === 'webview2-update' ? t('本次包含 WebView2 更新，需要重新安装完整版。') : t('本次更新需要重新安装完整版。'));
    updSet('sp_updplan', reason + t(' 服务器只提供增量更新，不会自动下载完整包。') +
      (j.fullSize > 0 ? tf(' 完整包约 {0}。', [updNum(j.fullSize)]) : ''));
    updSet('sp_updfiles', UPD.manualCode ? tf('百度网盘提取码：{0}', [UPD.manualCode]) : '');
    if (dl) { dl.style.display = 'none'; dl.disabled = true; }
    if (apply) apply.style.display = 'none';
    if (cn) { cn.style.display = UPD.manualCn ? '' : 'none'; cn.disabled = !UPD.manualCn; }
    if (global) { global.style.display = UPD.manualGlobal ? '' : 'none'; global.disabled = !UPD.manualGlobal; }
  } else {
    var plan = '增量：需下载 ' + (j.filesToDownload | 0) + ' 个文件 / ' + (j.downloadText || '—') +
               tf('，跳过 {0} 个 / {1}', [(j.filesUnchanged | 0), (j.skippedText || '—')]);
    updSet('sp_updplan', plan);
    var list = j.files || [], fl = '', i;
    for (i = 0; i < list.length && i < 12; i++) fl += '+ ' + list[i].path + '  ' + updNum(list[i].size) + '\n';
    if (list.length > 12) fl += tf('… 还有 {0} 个\n', [(list.length - 12)]);
    updSet('sp_updfiles', fl);
    if (dl) { dl.style.display = ''; dl.disabled = (j.filesToDownload | 0) === 0; dl.textContent = '下载更新'; }
    if (cn) cn.style.display = 'none';
    if (global) global.style.display = 'none';
  }
  updSet('sp_updsize', j.publishedAt ? (tf('　发布 {0}', [j.publishedAt])) : '');
  updSet('sp_updnotes', j.notes || '（这次没有写更新说明）');
  var sk = D('sp_updskip'); if (sk) sk.style.display = '';
  updSet('sp_updstate', tf('发现新版本 v{0}', [j.latest]));
}

function updPollCheck() {
  api('/api/update/check').then(function (j) {
    var btn = D('sp_updcheck');
    if (j && j.checking) { setTimeout(updPollCheck, 700); return; }
    if (btn) btn.disabled = false;
    updShowCheck(j || {});
  }).catch(function () { var b = D('sp_updcheck'); if (b) b.disabled = false; });
}

function updCheck() {
  if (UPD.recoveryBlocked) return;
  updSet('sp_updstate', '正在检查…（最长 8 秒）');
  var btn = D('sp_updcheck'); if (btn) btn.disabled = true;
  api('/api/update/check?force=1').then(function (j) {
    if (j && j.checking) { setTimeout(updPollCheck, 700); return; }
    if (btn) btn.disabled = false;
    updShowCheck(j || {});
  }).catch(function (e) {
    if (btn) btn.disabled = false;
    updSet('sp_updstate', tf('检查失败：{0}', [(e && e.message ? e.message : e)]));
  });
}

function updPollProgress() {
  if (UPD.recoveryBlocked) return;
  api('/api/update/progress').then(function (p) {
    if (!p) { setTimeout(updPollProgress, 900); return; }
    var bar = D('sp_updbar');
    if (bar) bar.style.width = (p.pct || 0) + '%';
    if (p.phase === 'error') {
      var why = String(p.error || '未知原因'), quota = why.indexOf('quota_exceeded') >= 0;
      updSet('sp_updmsg', tf('下载失败：{0}', [why]) + (quota ? t(' 请改用百度网盘或 GitHub 手动下载完整版，或下月重试。') : ''));
      var b = D('sp_upddl'); if (b) { b.disabled = false; b.style.display = quota ? 'none' : ''; }
      if (quota) {
        var cn = D('sp_updcn'), global = D('sp_updglobal');
        if (cn) { cn.style.display = UPD.manualCn ? '' : 'none'; cn.disabled = !UPD.manualCn; }
        if (global) { global.style.display = ''; global.disabled = false; }
      }
      return;
    }
    if (p.phase === 'ready' || p.canApply) {
      if (bar) bar.style.width = '100%';
      updSet('sp_updmsg', tf('已就绪：{0} 个文件已逐个校验 sha256 并解压到暂存区。点「立即更新并重启」生效。', [(p.files | 0)]));
      var ap = D('sp_updapply'); if (ap) ap.style.display = '';
      var dl = D('sp_upddl'); if (dl) { dl.disabled = false; dl.style.display = 'none'; }
      return;
    }
    var label = { downloading: '下载中', verifying: '校验 sha256…', extracting: '解压中…',
                  'downloading-one-by-one': '逐文件补下…', 'downloading-objects': '下载增量对象…',
                  checking: '检查中…' }[p.phase] || p.phase;
    updSet('sp_updmsg', label + '  ' + (p.pct || 0) + '%   ' + updNum(p.downloaded) + ' / ' +
                        updNum(p.total) + '   ' + (p.speedText || ''));
    setTimeout(updPollProgress, 500);
  }).catch(function () { setTimeout(updPollProgress, 900); });
}

function updDownload() {
  if (UPD.recoveryBlocked) return;
  var btn = D('sp_upddl'); if (btn) btn.disabled = true;
  var barwrap = D('sp_updbarwrap'); if (barwrap) barwrap.style.display = '';
  updSet('sp_updmsg', '正在下载…');
  api('/api/update/download', '').then(function (j) {
    if (!j || !j.ok) {
      updSet('sp_updmsg', tf('下载没能开始：{0}', [((j && j.error) || '未知原因')]));
      if (btn) btn.disabled = false;
      return;
    }
    updPollProgress();
  }).catch(function (e) {
    updSet('sp_updmsg', tf('下载没能开始：{0}', [(e && e.message ? e.message : e)]));
    if (btn) btn.disabled = false;
  });
}

function updApply() {
  if (UPD.recoveryBlocked) {
    UPD.applyArm = false;
    updSet('sp_updmsg', tf('上次回滚未完成，备份已保留：{0}', [UPD.recoveryDetail || '']));
    return;
  }
  var btn = D('sp_updapply');
  if (!UPD.applyArm) {
    UPD.applyArm = true;
    if (btn) btn.textContent = '再点一次：更新并退出';
    updSet('sp_updmsg', '点击后程序会退出；重新打开 RLPlayer 就会用上新版本。设置 / 库 / 歌单 / 收藏 / 封面缓存都不会动。');
    setTimeout(function () { UPD.applyArm = false; if (btn && !btn.disabled) btn.textContent = '立即更新并重启'; }, 6000);
    return;
  }
  UPD.applyArm = false;
  if (btn) { btn.disabled = true; btn.textContent = '正在退出…'; }
  api('/api/update/apply', '').then(function (j) {
    if (!j || !j.ok) {
      updSet('sp_updmsg', tf('没能开始更新：{0}', [((j && j.error) || '未知原因')]));
      if (btn) { btn.disabled = false; btn.textContent = '立即更新并重启'; }
      return;
    }
    updSet('sp_updmsg', tf('程序正在退出…重新打开 RLPlayer 即完成更新（v{0}）。', [j.version]));
    toast('程序即将退出，重新打开即完成更新');
  }).catch(function (e) {
    updSet('sp_updmsg', tf('没能开始更新：{0}', [(e && e.message ? e.message : e)]));
    if (btn) { btn.disabled = false; btn.textContent = '立即更新并重启'; }
  });
}

function updSkip() {
  var v = ((D('sp_updver') || {}).textContent || '').replace(/^v/, '');
  if (!v || v === '—') { toast('先检查更新'); return; }
  api('/api/update/skip', JSON.stringify({ version: v })).then(function () {
    var card = D('sp_updcard'); if (card) card.style.display = 'none';
    updSet('sp_updstate', tf('已跳过 v{0}（删掉 rlplayer-update.conf 里的 skip= 可恢复提示）', [v]));
    toast(tf('已跳过 v{0}', [v]));
  }).catch(function () { toast('跳过失败'); });
}

function updRenderState(s) {
  if (!s) return;
  var wasBlocked = UPD.recoveryBlocked;
  UPD.recoveryBlocked = s.pendingPhase === 'rollback_failed';
  UPD.recoveryDetail = s.pendingDetail || '';
  if (s.current) updSet('sp_updcur', s.current);
  var failed = s.pendingPhase === 'rolled_back' || s.pendingPhase === 'apply_failed' || UPD.recoveryBlocked;
  var state = D('sp_updstate'); if (state) state.style.color = UPD.recoveryBlocked ? '#e66' : '';
  if (!UPD.recoveryBlocked && wasBlocked) {
    updSet('sp_updmsg','');var oldMsg=D('sp_updmsg');if(oldMsg)oldMsg.style.color='';
  }
  if (UPD.recoveryBlocked) {
    var text = tf('上次回滚未完成，备份已保留：{0}', [UPD.recoveryDetail]);
    updSet('sp_updstate', text);updSet('sp_updmsg', text);
    var msg = D('sp_updmsg'); if (msg) msg.style.color = '#e66';
    UPD.applyArm = false;
  } else if (s.pendingPhase === 'rolled_back') {
    updSet('sp_updstate', tf('上次更新失败已自动回滚：{0}', [UPD.recoveryDetail]));
  } else if (s.pendingPhase === 'apply_failed') {
    updSet('sp_updstate', tf('上次替换失败，恢复详情：{0}', [UPD.recoveryDetail]));
  } else if (s.pendingPhase === 'ready') {
    updSet('sp_updstate', tf('有已下载但还没生效的更新（v{0}），重启后生效', [(s.pendingVersion || '')]));
  }
  var ap = D('sp_updapply');
  if (ap) {ap.disabled=!!UPD.recoveryBlocked;if(UPD.recoveryBlocked)ap.style.display='none';}
  if (s.staged || UPD.recoveryBlocked) {
    var card = D('sp_updcard'); if (card) card.style.display = '';
    if (ap && !UPD.recoveryBlocked) ap.style.display = '';
    var dl = D('sp_upddl'); if (dl) dl.style.display = 'none';
    if (s.staged) updSet('sp_updver', 'v' + s.stagedVersion);
    if (!failed) updSet('sp_updstate', tf('已下好 v{0} 的更新包，点「立即更新并重启」生效', [s.stagedVersion]));
  }
}
function updBind() {
  if (D('sp_updcheck')) D('sp_updcheck').onclick = updCheck;
  if (D('sp_upddl')) D('sp_upddl').onclick = updDownload;
  if (D('sp_updcn')) D('sp_updcn').onclick = function () {
    api('/api/update/manual', JSON.stringify({ target: 'baidu' })).catch(function () {});
    openExternal(UPD.manualCn);
  };
  if (D('sp_updglobal')) D('sp_updglobal').onclick = function () {
    api('/api/update/manual', JSON.stringify({ target: 'github' })).catch(function () {});
    openExternal(UPD.manualGlobal);
  };
  if (D('sp_updapply')) D('sp_updapply').onclick = updApply;
  if (D('sp_updskip')) D('sp_updskip').onclick = updSkip;
  // 进页面先把当前版本与上次更新结果刷出来（不发网络）
  api('/api/update/state').then(function (s) {
    updRenderState(s);
  }).catch(function () {});
  // A network version check starts only after the user's Check button.
}


// ============================================================================
// 一键反馈（261027-01 #1；后端 = src/net/feedback.h，端点 = POST /api/feedback）
// ----------------------------------------------------------------------------
// 本地端点只创建后台任务，页面轮询任务状态；慢网络不会阻塞播放器其它本地 API。
// 同一表单在重试前复用幂等键，避免服务器已收到而客户端超时时生成重复工单。
// 为什么不用 window.prompt/confirm：CDP 自检下原生对话框会把页面挂住（本项目踩过），
// 统一走自绘 #modal 与页内状态行。
// 返回的七个字段全部原样列出：uploaded / status / path / sha / local / error / message。
// 无凭据时 uploaded=false 且 local=<本地绝对路径> —— 界面必须显示它，否则用户以为反馈丢了。
// ============================================================================
// 预览：把「会发送的」与「不会发送的」两栏摊开给用户看。
// 数据来自后端 /api/feedback/preview —— 它走的正是提交时那个 build_payload()，
// 所以预览里报的日志行数、脱敏规则数就是提交时的真实值，不是另拼的假清单。
function fbPreview(fields) {
  var field=typeof fields==='function'?fields:D;
  var box = field('sp_fbpreviewbox'), btn = field('sp_fbpreview'), msg = field('sp_fbmsg');
  if (!box) return;
  if (btn) btn.disabled = true;
  if (msg) msg.textContent = t('正在生成预览…');
  var payload = {
    type:  (field('sp_fbtype')  || {}).value || 'bug',
    title: ((field('sp_fbtitle') || {}).value || '').trim(),
    body:  ((field('sp_fbbody')  || {}).value || '').trim(),
    steps: ((field('sp_fbsteps') || {}).value || '').trim()
  };
  fetch('/api/feedback/preview', {
    method: 'POST', headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(payload)
  }).then(function (r) { return r.json(); }).then(function (j) {
    if (btn) btn.disabled = false;
    if (msg) msg.textContent = '';
    if (!j || !j.ok) {
      box.style.display = 'block';
      box.textContent = tf('预览失败：{0}', [((j && j.error) || '未知原因')]);
      return;
    }
    box.replaceChildren();
    function line(text,emphasis) {
      var item=document.createElement(emphasis?'strong':'div');
      item.textContent=text;box.appendChild(item);
    }
    var bytes=j.payload_bytes;
    line(t('会发送这些'),true);
    if(typeof bytes==='number' && Number.isSafeInteger(bytes) && bytes>=0) {
      var size=bytes<1024?bytes+' B':bytes<1048576?(bytes/1024).toFixed(1)+' KiB':(bytes/1048576).toFixed(2)+' MiB';
      line(tf('诊断大小：{0} / 8 MiB（{1} 字节）',[size,String(bytes)]));
      if(bytes>8*1024*1024)line(t('诊断超过 8 MiB，服务器无法接收这份诊断。'));
    }
    line(tf('· 你填的内容：类型「{0}」、标题、详细描述、复现步骤',[j.type_label]));
    line(tf('· 版本：RLPlayer {0}（{1}）',[j.version,j.build]));
    line(tf('· 系统：{0} · {1} · {2} 线程 · 内存 {3} MB',[j.os,j.cpu,j.cpu_threads,j.ram_total_mb]));
    line(tf('· 显卡：{0}',[j.gpu||t('（未探测）')]));
    line(tf('· 播放状态：{0} · 后端 {1}',[t(j.playing?'正在播放':'未播放'),j.backend||'-']));
    line(tf('· 设置快照（{0} 项）与输出端点',[j.settings_keys]));
    line(tf('· 日志尾：{0} 行 / {1} KB（播放日志 + 启动日志）',[j.log_lines,j.log_kb]));
    line(t('不会发送'),true);
    line(t('· 你的音乐文件、封面缓存、曲库索引'));
    line(tf('· 已自动打码：API Key、token、密码、Authorization 头（共 {0} 条规则）',[j.redaction_rules]));
    line(t('· 用户名统一替换为 <user>'));
    line(t('预览就是最终内容。确认无误再点「提交反馈」。'));

    box.style.display = 'block';
  }).catch(function (e) {
    if (btn) btn.disabled = false;
    if (msg) msg.textContent = '';
    box.style.display = 'block';
    box.textContent = tf('预览失败：{0}', [(e && e.message ? e.message : '网络错误')]);
  });
}

function fbShowResult(j, fields) {
  var field=typeof fields==='function'?fields:D;
  var msg = field('sp_fbmsg'), out = field('sp_fbresult');
  var errorText = String(j.error || '');
  if (errorText === '反馈服务响应无效，无法确认已提交') errorText = t(errorText);
  var lines = [
    'uploaded: ' + (j.uploaded ? 'true' : 'false'),
    'status:   ' + (j.status === undefined ? '-' : j.status),
    'path:     ' + (j.path || '-'),
    'sha:      ' + (j.sha || '-'),
    'local:    ' + (j.local || '-'),
    'error:    ' + (j.error || '-')
  ];
  // 2026-10-28：技术字段照旧原样保留（排查要用），但在它上面先给一段人话结论。
  var head = [];
  if (j.uploaded) {
    head.push(tf('✅ 已提交成功{0}', [(j.path ? (t('，编号 ') + j.path) : '')]));
    head.push(t('我们能看到你填的内容和附带的诊断信息，不需要你再单独发日志。'));
    head.push(t('有回访时请带上这个编号。'));
  } else if (j.local) {
    head.push(t('⚠️ 未能自动上传，反馈已保存到：'));
    head.push('　' + j.local);
    head.push(t('把这个文件发到 Q 群 895816505 即可，内容与自动上传的完全一样。'));
  } else {
    head.push(tf('❌ 提交失败{0}', [(errorText ? ('：' + errorText) : '')]));
    head.push(t('可以重试，或把下方内容截图发到 Q 群 895816505。'));
  }
  head.push('');
  head.push(t('—— 技术细节（反馈给开发者时贴这段即可）——'));
  if (out && typeof fields==='function') out.textContent=head.slice(0,-2).join('\n');
  else if (out) out.textContent = head.join('\n') + '\n' + lines.join('\n') + (j.message ? ('\n\n' + j.message) : '');
  if (msg) msg.textContent = j.uploaded ? t('已上传，感谢反馈！') : (j.local ? t('反馈已保存到本地') : t('提交失败'));
  toast(j.uploaded ? t('反馈已上传') : (j.local ? t('反馈已保存到本地') : (errorText || t('提交失败'))));
  if (j.uploaded) { fbSubmit.key = ''; fbSubmit.fingerprint = ''; }
  fbSubmit.busy = false;
}
function fbSubmit(fields) {
  var field=typeof fields==='function'?fields:D;
  var msg = field('sp_fbmsg'), out = field('sp_fbresult'), btn = field('sp_fbsubmit');
  if (fbSubmit.busy) return;
  var payload = {
    type:  (field('sp_fbtype')  || {}).value || 'bug',
    title: ((field('sp_fbtitle') || {}).value || '').trim(),
    body:  ((field('sp_fbbody')  || {}).value || '').trim(),
    steps: ((field('sp_fbsteps') || {}).value || '').trim()
  };
  if (!payload.title && !payload.body) { toast('标题和详细描述至少要填一个'); return; }
  var fingerprint = JSON.stringify(payload);
  if (fbSubmit.fingerprint !== fingerprint) {
    fbSubmit.fingerprint = fingerprint;
    fbSubmit.key = '';
  }
  if (!fbSubmit.key) {
    var bytes = new Uint8Array(16), hex = '';
    if (typeof crypto !== 'undefined' && crypto.getRandomValues) crypto.getRandomValues(bytes);
    else { var seed = String(Date.now()) + String(Math.random()); for (var i = 0; i < bytes.length; ++i) bytes[i] = seed.charCodeAt(i % seed.length) & 255; }
    for (var j = 0; j < bytes.length; ++j) hex += ('0' + bytes[j].toString(16)).slice(-2);
    fbSubmit.key = hex;
  }
  payload.idempotency_key = fbSubmit.key;
  if (out) out.textContent = '';
  if (msg) msg.textContent = t('正在后台打包并上传；慢网络下也可以继续使用播放器。');
  if (btn) btn.disabled = true;
  fbSubmit.busy = true;
  function poll(job) {
    fetch('/api/feedback/status?job=' + encodeURIComponent(job)).then(function (r) { return r.json(); }).then(function (state) {
      if (!state || !state.done) {
        if (msg) msg.textContent = t('反馈正在后台上传，请稍候…');
        setTimeout(function () { poll(job); }, 600);
        return;
      }
      fbSubmit.busy = false;
      if (btn) btn.disabled = false;
      fbShowResult(state.result || { uploaded: false, error: '反馈服务响应无效，无法确认已提交' }, fields);
    }).catch(function () {
      if (msg) msg.textContent = t('本地状态读取暂时失败，正在继续等待后台结果…');
      setTimeout(function () { poll(job); }, 1200);
    });
  }
  fetch('/api/feedback', {
    method: 'POST', headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(payload)
  }).then(function (r) { return r.json(); }).then(function (result) {
    if (result && result.accepted && result.job) { poll(result.job); return; }
    fbSubmit.busy = false;
    if (btn) btn.disabled = false;
    fbShowResult(result || { uploaded: false, error: '反馈服务响应无效，无法确认已提交' }, fields);
  }).catch(function (e) {
    fbSubmit.busy = false;
    if (btn) btn.disabled = false;
    var why = (e && e.message ? e.message : String(e));
    if (out) out.textContent = 'error: ' + why;
    if (msg) msg.textContent = '提交失败';
    toast(tf('反馈提交失败：{0}', [why]));
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
      diagMsg(tf('已导出 {0}（{1} KB）', [name, Math.round(blob.size / 1024)]));
      toast(tf('诊断包已导出：{0}', [name]));
    });
  }).catch(function (e) {
    diagMsg('');
    toast(tf('诊断包导出失败：{0}', [(e && e.message ? e.message : e)]));
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
    diagMsg(tf('已导出 {0}', [name]));
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
  if(D('sp_crossfeed'))ADV.crossfeed=D('sp_crossfeed').checked?1:0;
  if(D('sp_crossfeed_freq'))ADV.crossfeedFreq=Number(D('sp_crossfeed_freq').value);
  if(D('sp_crossfeed_level'))ADV.crossfeedLevel=Number(D('sp_crossfeed_level').value);
  if(D('sp_dacbits'))ADV.dacbits=Number(D('sp_dacbits').value);
  ADV.bits = parseInt(D('sp_bits').value, 10) || 0;
  ADV.taps = parseInt(D('sp_taps').value, 10) || 0;
  ADV.atten = parseFloat(D('sp_atten').value) || 0;
  ADV.transition = parseFloat(D('sp_transition').value) || 0;
  ADV.phase = D('sp_phase').value || '';
  ADV.modorder = parseInt(D('sp_modorder').value, 10) || 5;
  if (D('sp_modfamily')) ADV.modfamily = D('sp_modfamily').value || 'dsd';
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
  D('sRateFamilyPolicy').value = normalizeRateFamilyPolicy(D('sp_rateFamilyPolicy').value);
  syncRateFamilyPolicy();
  D('sFilt').value = D('sp_filt').value;
  D('sFiltNx').value = D('sp_filtNx').value;
  D('sShap').value = D('sp_shaper').value;
  D('sBuf').value = D('sp_buffer').value;
  D('sIsp').checked = D('sp_isp').checked;
  D('s2s').value = D('sp_2s').value;
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
  if (!ensurePlaybackSettings()) return Promise.resolve({ok:false,errorCode:'PLAYBACK_SETTINGS_NOT_READY'});
  if (!validateSdmCornerSettings()) return Promise.resolve({ok:false,errorCode:'SDM_CORNER_QUALITY'});
  collectSettings();
  return saveSettings(true).then(function (j) {
    // persistSettings already displays the failure. A click is not proof of a
    // committed write: only acknowledge success after the backend confirms it.
    if (!j || !j.ok) {if(j&&j.stale)toast(t('当前组已切换，请重新操作'));return j;}
    var extra = (D('sp_root') && D('sp_root').value.trim()) ? ('&rootpending=1') : '';
    toast(tf('设置已保存{0}', [extra]));
  });
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
  if (t.isContentEditable) return;
  // Keep native keyboard activation on buttons and disclosure summaries. A
  // focused mode or settings button must never turn Space into a playback action.
  if (e.key === ' ' && t.closest && t.closest('button,summary,a[href]')) return;
  if (t.closest && t.closest('#rlViewModes') && /^Arrow/.test(e.key)) {
    e.preventDefault();
    if (e.key === 'ArrowLeft' || e.key === 'ArrowRight') {
      var other = document.querySelector('#rlViewModes [data-rl-mode="' + (t.dataset.rlMode === 'pro' ? 'listen' : 'pro') + '"]');
      if (other) other.focus();
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
  else if (e.key === 'ArrowUp') { e.preventDefault(); setVol(Number(D('vol').value)+(e.shiftKey?.1:.5),true);commitVolumeGesture(); }
  else if (e.key === 'ArrowDown') { e.preventDefault(); setVol(Number(D('vol').value)-(e.shiftKey?.1:.5),true);commitVolumeGesture(); }
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

/* ---------------- 播放队列：排序 / 右键 / 添加面板（261027-03 #3 重构） ----------------
   用户原话：「播放队列页有严重BUG，出现了一个默认顺序，已经添加的歌曲不能删除。
                添加的逻辑也跟之前版本严重不同。请你认真分析、重构合理的播放队列功能。」

   重构后模型只有一句话：**队列 = 一张有序表 + 一个"正在播"指针；加/删/排只改表，播只改指针。**
     · 加（5 个入口，全部是**看得见的**按钮/菜单项）：
         ① 队列页「＋ 添加曲目」→ 搜索面板，点行即加入（本文件的 openQueueAddPanel）
         ② 曲目页 / 搜索页 每行「＋」（与专辑页同一套 .add-btn）
         ③ 专辑页「加入队列」=整张；专辑曲目行「＋」=单首
         ④ 曲目/专辑行右键菜单「加入播放队列」/「整张加入队列」
         ⑤ 把文件/文件夹拖进窗口
       —— 1.5.x 的入口其实一个都没少（已用 dist\RLPlayer-1.5.1…1.5.10 的内嵌脚本逐版比对），
          真正的病是**曲目页 300 行里 0 个可见控件、队列页 0 个**，用户只能靠猜右键。
     · 删（3 条路径）：行内「×」（**静止就可见**，不再 opacity:0）/ 行右键「从队列移除」/「清空队列」。
     · 排（一次动作 + 一键后悔）：下拉里只有**动作**（标题/艺术家/专辑/时长/年份/反转/随机打乱），
       排序后出现「↩ 撤销排序」（后端快照；队列结构一变快照即作废，按钮自己消失）。
       **不再有「默认顺序」这个伪选项** —— 它既不是动作、又是一直亮着的"状态"，选中还会弹
       "没有可恢复的排序"，用户读到的正是"凭空冒出来一个默认顺序"。

   三条硬约束（判据就是这三条）：
   ① **只换顺序，不换曲**：排序在前端算完，把目标顺序整列交给 POST /api/qorder，由后端把 index
      重新定位到"当前播的那一首"（QItem::same，含 CUE 区间）。前端绝不自己改 index。
   ② **可撤销**：第一次排序前后端把原顺序快照进 g_q.sortBase，POST /api/qsort?by=default 整列还原；
      /api/queue 回 canUndo，前端据此决定是否显示「↩ 撤销排序」（没有后悔药时按钮根本不出现）。
   ③ **顺序只有一份**：排序结果落回后端 items（与拖拽同一条通路），否则"看到的"和"播的"不一致。
   为什么排序在前端算：标题/艺术家必须走 localeCompare('zh') 才是音序（拼音）；后端无 ICU，
   用字节序会把中文按码点排开，用户一眼看出"乱序"。
   为什么随机模式与排序/拖拽/删除互不覆盖：随机模式另存一份"打乱前的原顺序"（g_q.base），退出
   随机时还原；用户一旦手动重排，后端会把那份快照作废 —— 否则"开随机→按标题排序→关随机"
   会把顺序一把推回添加顺序（旧实现实测 15/15 项跳回，用户感受就是"又冒出个默认顺序"）。 */
function queueItemLine(items, cues, i) {
  var f = items[i] || '';
  var c = cues[i] || [-1, -1];
  return (c && c[0] >= 0) ? (f + '\t' + c[0] + '\t' + c[1]) : f;
}
// 排序动作的显示名（toast 与下拉文案共用一份，避免两处说法不一致）
var QSORT_LABEL = { title: '标题 A→Z', artist: '艺术家 A→Z', album: '专辑 A→Z',
                    dur: '时长 短→长', year: '年份 新→旧', reverse: '反转顺序', shuffle: '随机打乱' };
function applyQueueSort(by) {
  if (!by) return;
  // ★ 排序前**现取**一份带元数据的队列，绝不依赖 S.q.metas：
  //   3 秒轮询的 loadQ() 会用不带 metas 的那份覆盖 S.q，此时所有排序键都是空串/0，
  //   排序会**静默变成 no-op**（261027-02 实测踩到：连续点标题/艺术家/时长，顺序纹丝不动）。
  api('/api/queue?meta=1').then(function (q) {
    var items = q.items || [], cues = q.cues || [], metas = q.metas || [];
    var n = items.length;
    if (!n) { toast('队列是空的'); return; }
    var idx = [];
    for (var i = 0; i < n; i++) idx.push(i);
    function meta(i) { return metas[i] || {}; }
    function txt(i, k) { return String(meta(i)[k] || ''); }
    if (by === 'title') {
      idx.sort(function (a, b) { return txt(a, 't').localeCompare(txt(b, 't'), 'zh'); });
    } else if (by === 'artist') {
      idx.sort(function (a, b) { return txt(a, 'a').localeCompare(txt(b, 'a'), 'zh'); });
    } else if (by === 'album') {
      idx.sort(function (a, b) { return txt(a, 'al').localeCompare(txt(b, 'al'), 'zh'); });
    } else if (by === 'dur') {
      idx.sort(function (a, b) { return (+meta(a).d || 0) - (+meta(b).d || 0); });
    } else if (by === 'year') {
      idx.sort(function (a, b) { return (+meta(b).y || 0) - (+meta(a).y || 0); });
    } else if (by === 'shuffle') {
      // 一次性洗牌：**只重排队列**，不改「播放模式=随机」那个持续状态（两者语义不同，绝不联动）
      for (var k = n - 1; k > 0; k--) {
        var r = Math.floor(Math.random() * (k + 1));
        var t = idx[k]; idx[k] = idx[r]; idx[r] = t;
      }
    } else if (by === 'reverse') {
      idx.reverse();
    }
    var lines = idx.map(function (i) { return queueItemLine(items, cues, i); });
    return api('/api/qorder', 'files=' + encodeURIComponent(lines.join('\n'))).then(function (j) {
      if (j && j.ok === false) { toast(j.error || '排序失败'); renderQueuePage(); return; }
      toast(tf('已按「{0}」重排 {1} 首（可点「↩ 撤销排序」还原）', [(QSORT_LABEL[by] || by), n]));
      renderQueuePage(); loadQ();
    });
  }).catch(function () { renderQueuePage(); });
}
// 撤销排序：后端把第一次排序前的快照整列还原（没有快照 → 如实报错，绝不假装排过）
function undoQueueSort() {
  api('/api/qsort?by=default', '').then(function (j) {
    if (j && j.ok === false) { toast(j.error || '没有可撤销的排序'); renderQueuePage(); return; }
    toast(tf('已撤销排序，还原为排序前那 {0} 首的顺序', [((j && j.n) || 0)]));
    renderQueuePage(); loadQ();
  }).catch(function () { renderQueuePage(); });
}
// 从队列移除一项：× 按钮与行右键菜单走**同一条通路**（两处各写一份必然走偏）
function removeQueueItem(i) {
  api('/api/qdel?i=' + i, '').then(function (j) {
    if (j && j.ok === false) { toast(j.error || '移除失败'); return; }
    toast('已从队列移除');
    loadQ();
    if (S.view === 'queue') renderQueuePage();
  }).catch(function () { toast('移除失败'); });
}
// 队列条目右键菜单（帮助浮层一直写着「右键队列条目 → 从队列移除」，1.5.x 其实没实现）
function showQueueRowMenu(x, y, i) {
  hideContextMenu();
  var m = document.createElement('div');
  m.className = 'ctxmenu';
  m.style.left = Math.max(4, x) + 'px';
  m.style.top = Math.max(4, y) + 'px';
  function item(label, fn, cls) {
    var d = document.createElement('div');
    d.className = 'ctxitem' + (cls ? (' ' + cls) : '');
    d.textContent = label;
    d.onclick = function (ev) { ev.stopPropagation(); hideContextMenu(); fn(); };
    m.appendChild(d);
  }
  var f = (S.q && S.q.items) ? S.q.items[i] : '';
  item('\u25b6 立即播放', function () { var c = (S.q.cues || [])[i] || [-1, -1]; playPath(f, c[0], c[1], i); });
  item('\u00d7 从队列移除', function () { removeQueueItem(i); }, 'danger');
  document.body.appendChild(m);
  _activeCtxMenu = m;
  var rr = m.getBoundingClientRect();
  if (x + rr.width > window.innerWidth - 6) m.style.left = Math.max(4, window.innerWidth - rr.width - 6) + 'px';
  if (y + rr.height > window.innerHeight - 6) m.style.top = Math.max(4, window.innerHeight - rr.height - 6) + 'px';
}

/* 「＋ 添加曲目」面板（261027-03 #3）：队列页上一个**不用右键、不用换页**就能加的入口。
   为什么要有它：用户原话「我根本没有添加进入队列的方法」—— 实测队列页上确实一个添加入口都没有。
   面板 = 搜索框 + 结果表（每行末「＋」，整行也可点）；加入的曲目一律**追加到队列末尾**，
   不动已在队列里的歌（与 /api/add 同一语义）。 */
function openQueueAddPanel() {
  if (!D('modal') || !D('mtitle') || !D('mbody')) return;
  D('mtitle').textContent = '添加到播放队列';
  D('mbody').innerHTML =
    '<div style="display:flex;gap:8px;align-items:center;margin-bottom:8px">' +
    '<input id=qAddQ spellcheck=false placeholder="搜索标题 / 艺术家 / 专辑（留空 = 前 40 首）"' +
    ' style="flex:1;height:32px;border-radius:7px;border:1px solid var(--line);background:var(--field);color:var(--txt);padding:0 10px;font-size:12.5px;outline:none">' +
    '</div>' +
    '<div id=qAddList style="max-height:min(52vh,420px);overflow:auto;border-top:1px solid var(--line)"></div>' +
    '<div style="margin-top:10px;font-size:11px;color:var(--mut2);line-height:1.8">' +
    '点行末「＋」或整行即可加入；加入的曲目**追加在队列末尾**，不会动已在队列里的歌。<br>' +
    '其他入口：曲目页/专辑页行内「＋」、右键曲目或专辑「加入播放队列」、把文件拖进窗口。</div>';
  D('modal').classList.add('on');
  var inp = D('qAddQ'), list = D('qAddList'), tmr = 0;
  function render(q) {
    if (!D('qAddList')) return;
    list.innerHTML = '<div style="padding:14px;color:var(--mut2);font-size:12px">搜索中…</div>';
    api('/api/lib/search?q=' + encodeURIComponent(q || '')).then(function (j) {
      if (!D('qAddList')) return;             // 面板已关掉 → 丢弃迟到响应
      var t = (j.tracks || []).slice(0, 40);
      if (!t.length) { list.innerHTML = '<div style="padding:14px;color:var(--mut2);font-size:12px">没有找到曲目</div>'; return; }
      var h = '';
      t.forEach(function (x, i) {
        h += '<div class=qaddr data-i=' + i + '>' +
             '<span class=qt1 title="' + esc(x.path) + '">' + esc(x.title || nam(x.path)) + '</span>' +
             '<span class=qa2>' + esc(x.artist || '') + (x.album ? (' \u00b7 ' + esc(x.album)) : '') + '</span>' +
             '<span class=qd>' + fmt(x.dur || 0) + '</span>' +
             '<button class="add-btn qaddbtn" data-i=' + i + ' title="加入播放队列" aria-label="加入播放队列">+</button></div>';
      });
      list.innerHTML = h;
      Array.prototype.forEach.call(list.querySelectorAll('.qaddr'), function (e) {
        var x = t[+e.dataset.i];
        function add() {
          api('/api/add', 'file=' + encodeURIComponent(x.path) + cueQuery(x.cs, x.ce)).then(function (rr) {
            if (rr && rr.ok === false) { toast(rr.error || '加入失败'); return; }
            toast(tf('已加入队列：{0}（队列共 {1} 首）', [(x.title || nam(x.path)), ((rr && rr.count) || '?')]));
            loadQ();
            if (S.view === 'queue') renderQueuePage();
            var b = e.querySelector('.qaddbtn');
            if (b) { b.textContent = '\u2713'; b.disabled = true; b.style.opacity = '1'; }
            e.classList.add('added');
          }).catch(function () { toast('加入失败'); });
        }
        e.onclick = add;
        var b = e.querySelector('.qaddbtn');
        if (b) b.onclick = function (ev) { ev.stopPropagation(); add(); };
      });
    }).catch(function () {
      if (D('qAddList')) list.innerHTML = '<div style="padding:14px;color:var(--mut2);font-size:12px">搜索失败</div>';
    });
  }
  inp.oninput = function () {
    var v = this.value;
    clearTimeout(tmr);
    tmr = setTimeout(function () { render(v); }, 250);
  };
  render('');
}

/* ---------------- 播放队列（主视图） ---------------- */
// Browser file inputs omit the parent directory. Ask only when a relative
// item actually needs it, and never derive an absolute path from the song library.
function m3uWindowsPath(value, folder) {
  var raw = String(value || '').trim();
  if (!raw || /[\u0000-\u001f]/.test(raw)) throw new Error('歌单包含不支持的路径或格式');
  if (/^file:/i.test(raw)) {
    var fileUrl = new URL(raw);
    if (fileUrl.search || fileUrl.hash) throw new Error('歌单包含不支持的路径或格式');
    raw = decodeURIComponent(fileUrl.pathname);
    if (fileUrl.hostname) raw = '//' + fileUrl.hostname + raw;
    else if (/^\/[a-z]:\//i.test(raw)) raw = raw.slice(1);
  }
  if (/[\u0000-\u001f]/.test(raw)) throw new Error('歌单包含不支持的路径或格式');
  raw = raw.replace(/\//g,'\\');
  if (/^\\\\[?.]\\/.test(raw) || /^[a-z]:[^\\]/i.test(raw) || /^\\(?!\\)/.test(raw) ||
      (/^[a-z][\w+.-]*:/i.test(raw) && !/^[a-z]:\\/i.test(raw))) throw new Error('歌单包含不支持的路径或格式');
  var match = /^(?:([a-z]:)\\|\\\\([^\\]+)\\([^\\]+)(?:\\|$))/i.exec(raw);
  if (!match) {
    if (!folder) return null;
    return m3uWindowsPath(folder.replace(/\\+$/,'') + '\\' + raw, '');
  }
  var root = match[1] || ('\\\\' + match[2] + '\\' + match[3]);
  var parts = [];
  raw.slice(match[0].length).split('\\').forEach(function (component) {
    if (component === '..') { if (parts.length) parts.pop(); }
    else if (component && component !== '.') {
      if (/[<>:"|?*]/.test(component)) throw new Error('歌单包含不支持的路径或格式');
      parts.push(component);
    }
  });
  return root + '\\' + parts.join('\\');
}
function parseTransportM3u(text, folder) {
  var items = [], needsFolder = false, cue = '';
  String(text).replace(/^\uFEFF/,'').split(/\r\n|\n|\r/).forEach(function (raw) {
    var line = raw.trim();
    if (!line) return;
    if (/^#EXT-X-/i.test(line)) throw new Error('歌单包含不支持的路径或格式');
    if (/^#RTA-CUE\b/i.test(line)) {
      var m = /^#RTA-CUE\s+(\S+)\s+(\S+)$/i.exec(line), a = m && Number(m[1]), b = m && Number(m[2]);
      if (!m || cue || !Number.isFinite(a) || !Number.isFinite(b) || a < 0 || (b !== -1 && b <= a)) throw new Error('歌单包含不支持的路径或格式');
      cue = '\t' + a + '\t' + b; return;
    }
    if (line[0] === '#') return;
    var item = m3uWindowsPath(line, folder);
    if (!item) needsFolder = true;
    items.push((item || '') + cue); cue = '';
  });
  if (!items.length || cue) throw new Error('歌单包含不支持的路径或格式');
  return {items:items, needsFolder:needsFolder};
}
function importTransportM3u(file) {
  if (!file) return;
  if (!/\.m3u8?$/i.test(file.name)) { toast(t('歌单包含不支持的路径或格式')); return; }
  if (file.size > 16*1024*1024) { toast(t('歌单过大')); return; }
  function preview(text, folder) {
    var parsed;
    try { parsed = parseTransportM3u(text, folder); }
    catch (_) { toast(t('歌单包含不支持的路径或格式')); return; }
    if (parsed.needsFolder && !folder) {
      askText({title:t('请输入歌单所在文件夹'), placeholder:'D:\\Music', onOk:function (value) {
        try {
          var absolute = m3uWindowsPath(value, '');
          if (!absolute) throw new Error();
          preview(text, absolute);
        } catch (_) { toast(t('歌单包含不支持的路径或格式')); }
      }});
      return;
    }
    var body = 'require_stopped=1&files=' + encodeURIComponent(parsed.items.join('\n'));
    if (body.length > 16*1024*1024) { toast(t('歌单过大')); return; }
    askConfirm({title:t('导入 M3U 到队列'),text:tf('将按歌单顺序替换当前队列，共 {0} 首。导入后可保存为歌单。',[parsed.items.length]),
      ok:t('替换队列'),onOk:function () {
        if (S.playing) { toast(t('请先停止播放，再替换队列。')); return; }
        api('/api/queue/set',body).then(function (j) {
          if (!j || !j.ok) { toast(j && j.error || t('导入失败')); return; }
          loadQ(); if (S.view === 'queue') renderQueuePage();
        }).catch(function () { toast(t('导入失败')); });
      }});
  }
  file.arrayBuffer().then(function (data) {
    var text = new TextDecoder('utf-8',{fatal:true}).decode(data); preview(text,'');
  }).catch(function () { toast(t('请将歌单另存为 UTF-8 编码。')); });
}

function renderQueuePage() {
  S.view = 'queue';
  // 2026-10-23（自检暴露）：标题原来只在 /api/queue 回来后才写 → 本地接口虽快，但慢的时候
  // 会出现"已经在队列页、标题还是上一页"的空窗。先同步写标题，再等数据补数量/按钮。
  head('播放队列', '', '');
  // meta=1：多要一份与 items 逐项对齐的 metas（标题/艺术家/专辑/时长/年份），排序要用；
  // canUndo：后端是否还留着"排序前快照" ⟹ 决定「↩ 撤销排序」按钮出不出来。
  // 普通 3 秒轮询仍走不带 meta 的 /api/queue（零额外开销）。
  api('/api/queue?meta=1').then(function (j) {
    if (S.view !== 'queue') return;   // 已切走 → 丢弃，别覆写当前页面
    S.q = { items: j.items || [], cues: j.cues || [], metas: j.metas || [],
            index: j.index || 0, canUndo: !!j.canUndo, shuffle: !!j.shuffle };
    var n = S.q.items.length;
    // #2（261026-09）：用户「从播放队列那里直接保存为歌单即可」⟹ 队列页顶部提供
    // 「保存为歌单」（写入 rlplayer-savedqueues.conf，含顺序与 CUE 区间，重启后仍在）。
    head('播放队列', n ? (n + ' 首') : '', '');
    // 工具条（261027-04 #1）：用户原话「主页-播放队列，'添加曲目'按钮和'保存为歌单'、'清空队列'
    //   按钮不在同一行，把'保存为歌单'、'清空队列'放到'添加曲目'右面」。
    // 旧版真因：后两个按钮塞在顶栏 head()（#tools，在 .top 里）→ 它们和下面的这条 .qhd 工具条
    //   天生两条行，永远对不齐。现在三个按钮**都进这条 .qhd**：添加曲目在左，保存/清空成组贴右，
    //   同一 flex 行 + 同一 align-items:center + 同一写死高度 30px ⟹ 三者 top 逐像素相同。
    //   窄窗（<1100px）允许整行 wrap，但左右两组各自 flex:none，换行后仍成组、不散。
    var h = '<div class=qhd>' +
            '<button class="btn pri" id=qAddBtn title="搜索曲库并加入队列（追加在末尾，不动已在队列里的歌）">\uff0b 添加曲目</button>';
    h += '<button type=button class=btn id=qImportM3u>' + esc(t('导入 M3U 到队列')) + '</button><input id=qM3uFile type=file accept=".m3u,.m3u8" hidden>';
    // 2026-10-27（铁律 11 抓到的真 bug，两次修都没修好，最终定案）：
    //   原方案把「保存为歌单 / 清空队列」放进 .qhdend 靠 margin-left:auto / 弹性占位推到最右。
    //   CDP 量得到（x=537 w=184 h=38，连红底诊断样式都生效），实机就是看不见 ——
    //   在真实 WebView2 的视口宽度下这组被挤出可视区（.qhd 实测宽 529，右组右缘 721）。
    //   结论：不要把「必须可见的主操作」交给剩余空间决定。
    //   改为紧跟「＋ 添加曲目」的顺序排布；窄窗靠 flex-wrap 整体换行，不会挤出视口。
    h += '<span class=qhdend>' +
         '<button class="btn pri" id=qSaveSq title="把当前队列原样存成一个歌单（含顺序与 CUE 区间，重启后仍在）">保存为歌单</button>' +
         '<button class=btn id=qClrAll title="清空整个播放队列（不影响正在播放的这一首）">清空队列</button>' +
         '</span>';
    if (n) {
      h += '<span class=qhdmid>' +
           '<span class=qhdlb>排序</span>' +
           '<select id=qSort class="track-sort sort-select" title="一次性重排队列顺序：只改顺序、不换曲，也不会改变正在播放的那一首">' +
           '<option value="">排序…</option>' +
           '<option value=title>标题 A→Z</option>' +
           '<option value=artist>艺术家 A→Z</option>' +
           '<option value=album>专辑 A→Z</option>' +
           '<option value=dur>时长 短→长</option>' +
           '<option value=year>年份 新→旧</option>' +
           '<option value=reverse>反转顺序</option>' +
           '<option value=shuffle>随机打乱</option>' +
           '</select>';
      if (S.q.canUndo)
        h += '<button class=btn id=qSortUndo title="把队列还原成排序前的那一版顺序">\u21a9 撤销排序</button>';
      if (S.q.shuffle)
        h += '<span class=qhdhint title="随机播放模式会物理打乱队列；关闭随机时不再改动你手动排好的顺序">随机播放中</span>';
      h += '</span>';
    }
    h += '</div>';
    if (!n) {
      D('list').innerHTML = h + '<div class=empty>队列是空的<br>' +
        '<span style="font-size:11.5px">点上面的「＋ 添加曲目」，或在曲目/专辑页点行内「＋」、' +
        '右键曲目「加入播放队列」、把文件拖进窗口</span></div>';
    } else {
      h += '<div class=qlist2>';
      S.q.items.forEach(function (f, i) {
        h += '<div class="qrow' + (i === S.q.index ? ' on' : '') + '" data-i=' + i + ' draggable=true>' +
             '<span class=qn>' + (i + 1) + '</span>' +
             '<span class=qt title="' + esc(f) + '">' + esc(nam(f)) + '</span>' +
             // 评审 #8：以前这里打完整物理路径，又长又不可读；只显示所在文件夹名，全路径放 tooltip
             '<span class=qp title="' + esc(f) + '">' + esc(parentFolder(f)) + '</span>' +
             // ★ 261027-03 #3：× 必须**静止可见**（CSS 里 base opacity .45）。旧版 opacity:0，
             //   只在鼠标精确悬停到那一行才浮现 ⟹ 用户看不到任何删除控件，报「不能删除」。
             '<button class=qdel data-i=' + i + ' title="从队列移除这首" aria-label="从队列移除">\u00d7</button></div>';
      });
      D('list').innerHTML = h + '</div>';
     Array.prototype.forEach.call(D('list').querySelectorAll('.qrow'), function (e) {
        var i = +e.dataset.i;
        e.onclick = function (ev) {
          if (ev.target.closest && ev.target.closest('.qdel')) return;
          var c = (S.q.cues || [])[i] || [-1, -1];
          playPath(S.q.items[i], c[0], c[1], i);
        };
        // 右键菜单（此前队列页没有 → 帮助浮层里那句「右键队列条目 → 从队列移除」是空头支票）
        e.oncontextmenu = function (ev) { ev.preventDefault(); ev.stopPropagation(); showQueueRowMenu(ev.clientX, ev.clientY, i); };
        e.ondragstart = function (ev) { ev.dataTransfer.setData('text/plain', String(i)); ev.dataTransfer.effectAllowed = 'move'; };
        e.ondragover = function (ev) { ev.preventDefault(); ev.dataTransfer.dropEffect = 'move'; };
        e.ondrop = function (ev) {
          ev.preventDefault(); ev.stopPropagation();
          var from = parseInt(ev.dataTransfer.getData('text/plain'), 10);
          if (isNaN(from) || from === i) return;
          api('/api/qmove?from=' + from + '&to=' + i, '').then(function () { renderQueuePage(); loadQ(); });
        };
        var d = e.querySelector('.qdel');
        if (d) d.onclick = function (ev) { ev.stopPropagation(); removeQueueItem(i); };
      });
    }
    if (D('qAddBtn')) D('qAddBtn').onclick = function () { openQueueAddPanel(); };
    D('qImportM3u').onclick = function () { D('qM3uFile').value=''; D('qM3uFile').click(); };
    D('qM3uFile').onchange = function () { importTransportM3u(this.files[0]); };
    if (D('qSort')) D('qSort').onchange = function () { var v = this.value; this.value = ''; if (v) applyQueueSort(v); };
    if (D('qSortUndo')) D('qSortUndo').onclick = function () { undoQueueSort(); };
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
    else if (!S.curMeta.hasCover && !S.coverCacheWatch) watchCurrentCachedCover(S.lastCur);
  }
  renderNp();
  // 261024-04 #7：进入单曲页立刻自动加载这首歌的 AI 整理（不必等 400ms 的 renderNp 轮询）
  npAutoSummary();
  // 整页控件是底栏/设置页的镜像：进来时刷一次，绝不显示与底栏不同的值
  applyRep(D('sRep') ? D('sRep').value : 0);
  syncVolUi();
  pollCastState(true);   // 261024-02 #5：进来就取一次投放状态（按钮文案/高亮由 applyCastUi 统一刷）
}
function closeNowPlaying() { stopCurrentCachedCover(); var pg = D('npPage'); if (pg) pg.classList.remove('on'); }

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
    h += '<button class=chip id=npEqChip title="' + esc(tf('当前 EQ：{0}（点击打开 EQ 设置）', [npFileName(eq)])) + '">' +
         '<svg width=11 height=11 viewBox="0 0 24 24" fill=none stroke=currentColor stroke-width=2 style="vertical-align:-1px"><path d="M4 21v-7M4 10V3M12 21v-9M12 8V3M20 21v-5M20 12V3M1 14h6M9 8h6M17 16h6"/></svg>EQ</button>';
  }
  if (ir) {
    h += '<button class=chip id=npIrChip title="' + esc(tf('当前 IR：{0}（点击打开 声音校准）', [npFileName(ir)])) + '">' +
         '<svg width=11 height=11 viewBox="0 0 24 24" fill=none stroke=currentColor stroke-width=2 style="vertical-align:-1px"><path d="M3 12h3l2-6 3 12 2-8 2 4h6"/></svg>IR</button>';
  }
  return h;
}
function npBindEqChips() {
  var eq = D('npEqChip'), ir = D('npIrChip');
  if (eq) eq.onclick = function () { closeNowPlaying(); rlOfferProfessional('eq'); };
  // #6：IR 属于「声音校准」页签，EQ 芯片进「EQ」页签（各进各的地方，不再都跳 EQ）
  if (ir) ir.onclick = function () { closeNowPlaying(); rlOfferProfessional('cal'); };
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
  // 2026-10-28（用户真机：AI 把《19:30》挂到不相干的《火花》上）：
  //   track 这一项**必须带上所属专辑名**，否则后端只能拿裸曲名去搜。
  //   本库的 WAV/自抓文件普遍零标签，裸曲名的搜索结果必然是垃圾。
  if (title) out.push({ k: 'track', label: '歌曲', name: title, artist: artist, album: album });
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
  // 2026-10-27（P3）：META.summary 可能是历史遗留的 0（见 aiSummaryOn 注释），改用统一判据
  return !!(aiSummaryOn() && m && (m.summaryAvailable || m.summaryCached || m.summaryCacheOk || META.hasLlmKey === true));
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
/* ---------------- 单曲页 AI 资料：**一个向右折叠的入口** ----------------
   2026-10-27（用户 261027-06 P3）用户原话：
     「单曲页面右面的 AI 整理功能，也不再需要分类歌曲、艺术家之类的。跟下面音频分析的处理方法一样，
       默认折叠。AI 功能默认向右折叠，点击可以弹出内容框即可。」
   旧实现：右侧四颗药丸「歌曲 / 专辑 / 艺术家 / 作品」，每颗点开才请求、才出抽屉 —— 用户说
           这些分类没必要，而且要一个个点。
   新实现：
     * 只有**一个**入口「AI 资料 ›」；折叠态在下面用一行摘要（已整理 · N 段 / 整理中… / 暂无资料）；
     * 点一下 = 从右侧滑出内容框（沿用既有 .npdrawer 抽屉，不新造浮层），内容**合并成一篇**：
       这首歌 → 这张专辑（含本地曲目表）→ 这位艺术家 →（古典时）作品版本，段间用细线分隔；
     * 再点一下（或点入口本身）= 收回；Esc / 右上关闭按钮同样可用；
     * 展开时把四类实体**并行**发起（summaryOnce 单飞 + 后端每任务一条线程），不串行等待。 */
var NP_AI_ORDER = ['track', 'release', 'person', 'work'];
function npAiKinds() {
  var tabs = NP_DATA.tabs || [], out = [];
  for (var i = 0; i < NP_AI_ORDER.length; ++i) {
    for (var j = 0; j < tabs.length; ++j) if (tabs[j].k === NP_AI_ORDER[i]) out.push(tabs[j]);
  }
  return out;
}
// 「几段」= 结构化字段条数 + 段落数（与内容框里看到的东西一一对应，不是编出来的数字）
function npAiSegCount(s) {
  if (!s) return 0;
  var n = 0;
  if (s.fields) { for (var k in s.fields) if (Object.prototype.hasOwnProperty.call(s.fields, k)) n++; }
  if (s.sections) n += s.sections.length;
  return n;
}
function npAiLoadedCount() {
  var tabs = npAiKinds(), seg = 0, okN = 0, busy = 0, errN = 0, idle = 0;
  for (var i = 0; i < tabs.length; ++i) {
    var st = NP_DATA.cache[tabs[i].k] || {};
    if (st.state === 'ok') { okN++; seg += npAiSegCount(st.data); }
    else if (st.state === 'loading') busy++;
    else if (st.state === 'error') errN++;
    else idle++;
  }
  return { seg: seg, ok: okN, busy: busy, err: errN, idle: idle };
}
// 折叠态那一行摘要：用户要求「折叠态显示一行摘要（比如 AI 资料：已整理 · 7 段）」
function npAiStateLine() {
  if (!npAiReady() && !NP_DATA.tabs.length) return '';
  var c = npAiLoadedCount();
  // 2026-10-29（管理员 261029-01 #3）：「你过分强调AI了，毫无必要。这里只需要显示『曲目信息』
  //   『整理中...』这两行信息就行了，多余的字只会影响观感。」
  //   ⟹ 状态行只保留「整理中…」与失败提示；『已整理 · N 段』『点开查看』这类多余描述全部去掉。
  if (c.busy > 0) return '整理中…';
  if (c.err > 0 && c.ok === 0) return '暂无资料 · 点开重试';
  if (c.ok > 0) return '';
  // 2026-10-29（FB-20261028-CMGW7 #4，用户：未开启AI整理不用显示在外面，点进去后提示在下面就行）：
  //   未开启时不再往入口下面挂文字；入口按钮本身保留，点进去才看到提示。
  if (!npAiReady()) return '';
  // 2026-10-29（261029-02 #4）：『点开查看』也是多余的一行，去掉；
  //   于是静态时卡片只有「曲目信息」一行，加载时多一行「整理中…」，正好两行。
  return '';
}
function npAiEntryHtml() {
  var c = npAiLoadedCount();
  var cls = c.ok > 0 ? ' ok' : (c.busy > 0 ? ' busy' : (c.err > 0 ? ' err' : ''));
  var isOpen = (NP_DATA.active === 'all' && npDrawerIsOpen());
  var line = npAiStateLine();
  return '<button class="npchip npai' + cls + (isOpen ? ' on' : '') + '" id=npAiEntry' +
         ' title="' + esc(isOpen ? '收起曲目信息'
             : (npAiReady() ? t('展开曲目信息（这首歌 · 专辑 · 艺术家 一起看）')
                             : '曲目信息未整理 —— 点开看怎么开启')) + '"' +
         ' style="justify-content:flex-start;gap:7px">' +
         // 2026-10-29（#3）：不再强调 AI —— 标题就写「曲目信息」，去掉彩色圆点与箭头（多余装饰）
         '<span style="flex:1;text-align:left">曲目信息</span>' +
         '</button>' +
         (line ? '<div id=npAiSum style="font-size:10.5px;line-height:1.45;color:var(--mut2);padding:1px 2px 0">' +
                 esc(line) + '</div>' : '');
}
function npDataRenderTabs() {
  var el = D('npTabs'); if (!el) return;
  var sig = npDataTabsSig() + '|' + npAiStateLine() + '|' + (NP_DATA.active === 'all' && npDrawerIsOpen() ? '1' : '0');
  if (sig === NP_DATA.tabSig) return;     // renderNp 每 400ms 调一次：没变化绝不重绘（否则悬停会闪）
  NP_DATA.tabSig = sig;
  el.innerHTML = npAiEntryHtml();
  var b = D('npAiEntry');
  if (b) b.onclick = npDataToggleAll;
}
// 点入口：收起 ↔ 展开（"看一眼就走"，不逼用户找关闭按钮）
function npDataToggleAll() {
  if (NP_DATA.active === 'all' && npDrawerIsOpen()) { npDrawerClose(); npDataRenderBody(); npDataRenderTabs(); return; }
  npDataActivateAll();
}
// 兼容旧调用点（预取/单飞回调里会传具体 kind）—— 现在统一按"全部"打开
function npDataToggle(kind) {
  if (NP_DATA.active === kind && npDrawerIsOpen()) { npDrawerClose(); npDataRenderBody(); return; }
  npDataActivate(kind);
}
function npDataStatHtml(kind, st) {
  // 261024-05 A12：AI 关闭时「专辑」标签只有本地曲目 —— 不能显示永远等不到的"整理中…"
  if (kind === 'release' && !npAiReady()) {
    var ln = ((st && st.local && st.local.tracks) || []).length;
    return ln ? (tf('本地曲目 · {0} 首', [ln])) : '本地曲库';
  }
  if (!st || st.state === 'idle' || st.state === 'loading') {
    if (kind === 'release' && st && st.local) return '本地曲目已就绪 · AI 整理中…';
    return '整理中…';
  }
  if (st.state === 'error') return '暂无资料';
  if (st.state === 'local') {
    var n0 = ((st.local && st.local.tracks) || []).length;
    return n0 ? (tf('本地曲目 · {0} 首', [n0])) : '本地曲库';
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
    ? (tf('已生成 {0} 字{1}', [chars, (sec ? (' · ' + sec) : '')]))
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
      statEl.textContent = statTxt || (tf('已生成 {0} 字', [chars]));
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
      statEl.textContent = statTxt || (tf('已生成 {0} 字', [chars]));
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

  // 2026-10-27（P3）：合并内容框里没有"单个 kind 的流式块"，交给整块重绘（sig 里带了 chars）
  if (NP_DATA.active === 'all') {
    npDataRenderBody();
  } else if (NP_DATA.active === kind) {
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
  var sub = (d.from_cache ? (tf('缓存 · {0}', [(d.generated_at ? fmtDate(d.generated_at) : '')])) : 'Discogs 实时') +
            ' · 不同演绎 ' + (typeof d.distinctCount === 'number' ? d.distinctCount : perfs.length) +
            tf(' / 发行版本 {0}', [(typeof d.releaseCount === 'number' ? d.releaseCount : perfs.length)]);
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
  // 2026-10-27（P3）：'all' = 合并内容框 —— 头部只写「AI 整理 · 当前曲名」+ 一行总状态。
  if (kind === 'all') {
    var k0 = D('npDrawerKind'), n0 = D('npDrawerName'), s0 = D('npDataStat');
    if (k0) k0.textContent = 'AI 整理';
    if (n0) n0.textContent = String((S.curMeta && S.curMeta.title) || (D('npt') && D('npt').textContent) || '');
    if (s0) s0.textContent = npAiStateLine();
    return;
  }
  var t = kind ? (npDataTab(kind) || { label: '', name: '' }) : { label: '', name: '' };
  var k = D('npDrawerKind'), n = D('npDrawerName'), s = D('npDataStat');
  if (k) k.textContent = t.label || '资料';
  if (n) n.textContent = t.name || '';
  if (s) s.textContent = kind ? npDataStatHtml(kind, st) : '';
}
// 合并内容框用：某一段的「进度」一行（不抢版面，只在还没内容时出现）
function npAiProgressHtml(st) {
  var p = (st && st.progress) || {};
  var raw = (st && typeof st.streamText === 'string') ? st.streamText : sanitizeStreamText(p.text);
  var clean = sanitizeStreamText(raw);
  if (/^\s*\{/.test(clean)) clean = '';
  var chars = (typeof p.chars === 'number') ? p.chars : (clean ? clean.length : 0);
  var sec = (typeof p.elapsedMs === 'number' && p.elapsedMs >= 0) ? (p.elapsedMs / 1000).toFixed(1) + 's' : '';
  var h = '<div class=npdhint>' + esc(p.phase || '正在整理…') +
          (chars > 0 ? (tf(' · 已 {0} 字', [chars])) : '') + (sec ? (' · ' + sec) : '') + '</div>';
  if (clean) h += '<div class=npdstream-text style="max-height:120px;overflow:auto;white-space:pre-wrap;font-size:11.5px;color:var(--txt2);margin-top:4px">' + esc(clean) + '</div>';
  return h;
}
// 合并内容框：这首歌 → 这张专辑（含本地曲目表）→ 这位艺术家 →（古典）作品。
// 段与段之间一条细线 + 一个极小标题（10px）。**不再有需要点开的分类** —— 一屏往下读即可。
function npAiBodyHtml() {
  var tabs = npAiKinds(), h = '', shown = 0;
  for (var i = 0; i < tabs.length; ++i) {
    var t = tabs[i];
    var st = NP_DATA.cache[t.k] || { state: 'idle' };
    var inner = '';
    if (t.k === 'release') {
      inner = npReleaseBody(st);                 // 本地曲目表 + AI 文字（状态无关）
    } else if (t.k === 'work') {
      if (st.state === 'ok') inner = npDataWorkBody(st.data);
      else if (st.state === 'error') inner = '<div class=npdhint>未在 Discogs 检索到该作品。</div>';
      else return;                               // work 是可选段：没数据就不占位
    } else if (st.state === 'ok' && summHas(st.data)) {
      inner = npDataSummaryBody(st.data, t.k);
    } else if (st.state === 'loading' || st.state === 'idle') {
      inner = npAiProgressHtml(st);
    } else if (st.state === 'error') {
      inner = '<div class=npdhint>未取到资料' + (st.error ? ('（' + esc(st.error) + '）') : '') + '</div>';
    }
    if (!inner) continue;
    if (shown) h += '<div style="height:1px;background:var(--line);margin:14px 0"></div>';
    // 段落小标题只写"这一段是什么"，不再重复抽屉头已显示的名字（用户：不要分类、不要重复）
    var segTitle = (t.k === 'track') ? '这首歌'
                 : (t.k === 'release') ? '这张专辑'
                 : (t.k === 'person') ? '这位艺术家'
                 : (t.k === 'work') ? '这个作品' : (t.label || '资料');
    h += '<div style="font-size:10px;letter-spacing:1.1px;color:var(--mut2);font-weight:700;margin-bottom:6px">' +
         segTitle + (t.k === 'release' && t.name ? (' · ' + esc(t.name)) : '') + '</div>' + inner;
    shown++;
  }
  if (!shown) {
    return '<div class=npdhint>' + (npAiReady()
      ? '正在整理这首歌的资料…（内容框会自动刷新，不用再点）'
      : 'AI 整理未开启。<b>设置 → 元数据</b> 里打开「AI 增强」并填入 API Key 后，'
                            + '这一块会自动整理这首歌、专辑与艺术家的资料。') + '</div>';
  }
  h += '<div style="margin-top:16px;display:flex;gap:8px;flex-wrap:wrap">' +
       '<button class="btn" id=npAiRefreshTrack style="font-size:11px;padding:3px 10px" ' +
       'title="只重跑这首歌的 AI 整理（一次模型调用）">重新整理这首歌</button></div>';
  return h;
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
  // 2026-10-27（P3）：合并内容框走独立渲染器（一屏读完，无分类）。
  if (kind === 'all') {
    var sigA = 'all';
    var tabsA = npAiKinds();
    for (var ia = 0; ia < tabsA.length; ++ia) {
      var stA = NP_DATA.cache[tabsA[ia].k] || {};
      sigA += '|' + tabsA[ia].k + ':' + stA.state + ':' +
              (stA.local ? ((stA.local.tracks || []).length) : '-') + ':' +
              (stA.data ? (stA.data.generated_at || (stA.data.summaryCached ? 'c' : 'f')) : '') + ':' +
              (stA.progress ? (stA.progress.chars || 0) : 0) + ':' + ((stA.streamText || '').length);
    }
    npDataHeader('all', null);
    if (sigA === NP_DATA.bodyKey && !force) return;
    NP_DATA.bodyKey = sigA;
    var keepA = body.scrollTop || 0;
    body.innerHTML = npAiBodyHtml();
    bindCopyChips(body);
    if (keepA) body.scrollTop = keepA;
    var rbA = D('npAiRefreshTrack');
    if (rbA) rbA.onclick = function () { npDataEnsure('track', true); };
    var rfA = D('npDataRefresh');               // npReleaseBody 自带的「重新整理」= 重跑专辑
    if (rfA) rfA.onclick = function () { npDataEnsure('release', true); };
    var woA = D('npWorkOpen'); if (woA) woA.onclick = npOpenWorks;
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
  // 只缓存成功结果：扫描尚未结束时的未找到/失败不能永久挡住稍后入库的专辑。
  if (NPLIB.key === key && NPLIB.album) { cb(NPLIB.album); return; }
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
  return aiSummaryOn() && (META.hasLlmKey === true || META.summaryEnabled === true);
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
    var statTxt = (chars > 0 || sec) ? (tf('已生成 {0} 字{1}', [chars, (sec ? (' · ' + sec) : '')])) : '';
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
  if (npDataVisible(kind)) npDataRenderBody(true);
  npLocalAlbum(t.name, t.artist || '', function (alb) {
    var cur = NP_DATA.cache[kind];
    if (!cur || cur.localKey !== key) return;            // 切歌/换实体 → 丢弃
    if (cur.data) cur.data.__album = alb || null;
    cur.local = alb || null;
    cur.localState = alb ? 'ok' : 'none';
    if (npDataVisible(kind)) npDataRenderBody(true);
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
  // 2026-10-28（用户真机：《19:30》被挂到不相干的《火花》上）：
  //   单曲检索带上**所属专辑名**。专辑名来自 npDataTargets(m) 里的 m.album
  //   （也就是专辑页已经确定下来的那个名字），**不用文件夹名去猜**。
  //   kind=release 时 name 本身就是专辑名，不需要再带 album。
  var albQ = '';
  if (kind === 'track') {
    var an = (t && t.album) || '';
    if (!an) {                               // 兜底：从同一份 tabs 里的 release 项取
      var tabs = NP_DATA.tabs || [];
      for (var i = 0; i < tabs.length; ++i) if (tabs[i].k === 'release') { an = tabs[i].name || ''; break; }
    }
    if (an) albQ = '&album=' + encodeURIComponent(an);
  }
  return '/api/entity/summary?kind=' + kind + '&name=' + encodeURIComponent((t && t.name) || '') +
         ((t && t.artist) ? ('&artist=' + encodeURIComponent(t.artist)) : '') + albQ +
         '&lang=' + encodeURIComponent(I18N.lang || 'zh') +
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
  if (!aiSummaryOn()) return;
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
  } else if (st && st.state === 'loading' && !npDrawerIsOpen() && !npAiStateLine()) {
    // 2026-10-27（P3）：折叠入口下面已经有一行摘要（"整理中… · 已 N 段"），
    // 这一行只在摘要算不出来时兜底，避免右栏出现两句意思相同的话。
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
      if (npDataVisible(kind)) npDataRenderBody();
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
  if (npDataVisible(kind)) npDataRenderBody();

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
    if (npDataVisible(kind)) npDataRenderBody();
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
  // 261024-05 A12：AI 关闭时专辑直接是"本地曲库"态 —— 点开就有曲目，不发任何 AI 请求
  if (kind === 'release' && !npDataCan(S.curMeta) && st.state !== 'ok') st.state = 'local';
  npDataRenderTabs();
  npDataRenderBody(true);  // 强制刷一次：占位/本地列表立刻出现（点一下就有反馈）
  npDrawerOpen();
  if (kind === 'release') npLocalAlbumLoad(kind);   // 先本地曲目，AI 文本后到再补
  npDataEnsure(kind, false);
}
/* 2026-10-27（用户 261027-06 P3）：点「AI 资料 ›」= 展开合并内容框。
   一次把四类实体**全部并行**发起（每类一条 summaryOnce；后端每个任务一条线程）：
     * 这首歌   → /api/entity/summary?kind=track
     * 这张专辑 → kind=release（同时读本地曲库的曲目表，先本地后 AI）
     * 这位艺术家 → kind=person
     * 古典时还有 kind=work（走 /api/entity/versions，Discogs）
   为什么一次全发：用户抱怨的正是"要点开好几个分类、一个个等"。展开即并发，谁先回来先显示。 */
function npDataActivateAll() {
  NP_DATA.active = 'all';
  var tabs = npAiKinds();
  for (var i = 0; i < tabs.length; ++i) {
    if (tabs[i].k === 'release') {
      var st = NP_DATA.cache['release'] || (NP_DATA.cache['release'] = { state: 'idle' });
      if (!npDataCan(S.curMeta) && st.state !== 'ok') st.state = 'local';
    }
  }
  npDataRenderTabs();
  npDataRenderBody(true);
  npDrawerOpen();
  npLocalAlbumLoad('release');
  for (var j = 0; j < tabs.length; ++j) npDataEnsure(tabs[j].k, false);
  npDataRenderBody();
}
// 当前内容框是否在展示某一类（'all' 的时候四类都算在内 —— 数据到了要能重绘）
function npDataVisible(kind) {
  if (!kind) return false;
  if (NP_DATA.active === kind) return true;
  return NP_DATA.active === 'all' && !!npDataTab(kind);
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
  // 2026-10-29（261029-02 #4）：npRailCap 元素已删除（用户要求只留两行），这里不再赋值。
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
  // 2026-10-27（P3）：'all' 是合成状态（合并内容框），只要还有任何一类资料就不收。
  var activeStill = (NP_DATA.active === 'all' && tabs.length > 0);
  for (var i = 0; i < tabs.length; ++i) if (tabs[i].k === NP_DATA.active) activeStill = true;
  if (NP_DATA.active && !activeStill) npDrawerClose();
  npDataRenderTabs();
  // 261024-01 #3b ③「默认隐藏」：抽屉不打开、不占版面；
  // 261024-02 追加：但**专辑资料允许后台预取一次**（Lead 指定，改善"点开要等 10~30s"的体感）。
  npPrefetchRelease();
  // 261024-04 #7：进入单曲页自动加载**这首歌**的 summary（不需要点标签）
  npAutoSummary();
  if (NP_DATA.active === 'all') {
    // 合并内容框开着：每一类都保持"有就显示、没有就发起"（与展开时同一条纪律）
    for (var k2 = 0; k2 < tabs.length; ++k2) npDataEnsure(tabs[k2].k, false);
  } else if (NP_DATA.active) {
    npDataEnsure(NP_DATA.active, false);
  }
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
  syncNpWaveform();
  D('npTitle').textContent = (D('npt') && D('npt').textContent) || '未播放';
  var sub = (D('npart') && D('npart').textContent) || '';
  var st = (D('nps') && D('nps').textContent) || '';
  // P2（261027-05 A4）：单曲页副标题也要标多声道。底栏那行由 tick() 每 400 ms 刷，
  // 元数据刚回来时它还带着上一拍的文字 ⟹ 这里补一次（已含标记就不重复拼）。
  var chm = chMark(S.curCh);
  if (chm && st.indexOf(chm) < 0) st = st + '  ·  ' + chm;
  D('npSub').textContent = sub + (st ? '  ·  ' + st : '');
  if (D('npKind')) D('npKind').textContent = npPlayingKindText(S.curMeta);
  if (D('npSetCover')) D('npSetCover').disabled = !S.lastCur;
  if (D('npResetCover')) {
    D('npResetCover').disabled = !S.lastCur;
    D('npResetCover').style.display = S.curMeta && S.curMeta.coverSource === 'track_manual' ? '' : 'none';
  }
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
/* ---------------- 歌词显示选项 + 右键菜单（2026-10-30 用户 261029-03 #4 #5） ----------------
   参考 QQ 音乐：字号 / 时间偏移 / 翻译 / 音译 / 日语假名 / 繁体。
   四条 AI 类（翻译·音译·假名·繁体）都走 /api/lyrics/ai，结果按「任务+语言+歌词内容」缓存到
   data\lyrics-cache，同一首歌第二次点开是秒出、不再花钱。
   为什么不在本地做繁体/音译：一简对多繁要按词义选字（发→髮/發），拼音/罗马字也要词典；
   内置码表只能给出"最常见的那个字"，歌词里必然出错。 */
var LYVIEW = { size: 1, offsetMs: 0, trans: 0, translit: 0, furi: 0, trad: 0, lang: "", data: {} };
var LY_SIZE_PX = [13, 15, 18, 22];
var LY_SIZE_TXT = ["小", "标准", "大", "特大"];
function lyricViewLoad() {
  try {
    var s = localStorage.getItem("rta-lyview");
    if (s) { var o = JSON.parse(s); for (var k in o) if (k in LYVIEW && k !== "data") LYVIEW[k] = o[k]; }
  } catch (e) { }
}
function lyricViewSave() {
  try {
    localStorage.setItem("rta-lyview", JSON.stringify({
      size: LYVIEW.size, offsetMs: LYVIEW.offsetMs, trans: LYVIEW.trans,
      translit: LYVIEW.translit, furi: LYVIEW.furi, trad: LYVIEW.trad, lang: LYVIEW.lang
    }));
  } catch (e) { }
}
// 歌词内容指纹：换歌/换词就作废已翻译结果（否则会把上一首的译文贴到这一首）
// 2026-11-01（用户 261101 #1）：很多下载来的 LRC 把「原文 译文」写在同一行
// （实测：教えて！ もっとあなたの事 告诉我更多 关于你的事）。界面照原样显示就成了
// 「原词和翻译挤在一行」—— 用户在截图里指的就是这个。
// 这里只在**显示时**拆成两行：歌词文本、时间轴、高亮索引都不动（LY.lines 保持原样）。
// 判据（宁可少拆，不可误拆）：
//   ① 左边必须有假名或拉丁字母（说明它是原文）；
//   ② 右边必须**没有**假名也没有拉丁字母（说明它只可能是译文）；
//   ③ 右边至少 2 个汉字；不足 4 个汉字时还必须含「简体专用字」，避免把「君の名は 世界」拆开。
var LY_CN_ONLY = /[你这们说时让给还没关于对从会来过么样爱见开关门东车马鸟书长写认识觉边远样]/;
function lyricSplitPair(s) {
  var t = String(s || '').trim();
  if (!t) return { main: t, sub: '' };
  var KANA = /[\u3040-\u30ff]/, LATIN = /[A-Za-z]/, CJK = /[\u3400-\u9fff]/;
  if (!KANA.test(t) && !LATIN.test(t)) return { main: t, sub: '' };   // 纯中文行：不分
  var best = -1;
  for (var i = 0; i < t.length; i++) {
    var c = t[i];
    if (c !== ' ' && c !== '\u3000' && c !== '/' && c !== '\uff0f') continue;
    var left = t.slice(0, i).trim(), right = t.slice(i + 1).trim();
    if (!left || !right) continue;
    if (KANA.test(right) || LATIN.test(right)) continue;
    if (!(KANA.test(left) || LATIN.test(left))) continue;
    var han = right.replace(/[^\u3400-\u9fff]/g, '').length;
    if (han < 2) continue;
    if (han < 4 && !LY_CN_ONLY.test(right)) continue;
    best = i;                                                         // 取**第一个**合法切点：
    break;                                                            // 译文内部还有空格时，
                                                                      // 取最后一个会把译文劈成两半（实测踩到）
  }
  if (best < 0) return { main: t, sub: '' };
  return { main: t.slice(0, best).trim(), sub: t.slice(best + 1).trim() };
}
function lyricKey() {
  if (!LY.lines.length) return "";
  // Full request content, rather than first line + length: equal-length songs
  // often share an opening line. JSON also preserves line boundaries exactly.
  return JSON.stringify(LY.lines.map(function (line) { return line.s; }));
}
function lyricTargetLang() {
  if (LYVIEW.lang) return LYVIEW.lang;
  return (I18N && I18N.lang && I18N.lang !== "zh") ? I18N.lang : "zh";
}
// 该任务当前可用的结果行（指纹不匹配就当没有）
function lyricExtra(kind) {
  var d = LYVIEW.data[kind];
  if (!d || d.key !== lyricKey() || d.lang !== lyricTargetLang() || !d.lines || !d.lines.length) return null;
  return d.lines;
}
function applyLyricView() {
  var box = D("npLyric"); if (!box) return;
  box.style.setProperty("--ly-size", LY_SIZE_PX[LYVIEW.size] + "px");
  // 清掉 dataset.n 强制重建：字号/译文/繁体都会改变每一行的 DOM
  box.dataset.n = "";
  renderNpLyric();
}
function lyricAi(kind) {
  var lines = [];
  for (var i = 0; i < LY.lines.length; i++) lines.push(LY.lines[i].s);
  if (!lines.length) { toast("还没有歌词"); return; }
  // 界面里的开关名（trans/furi）与后端任务名（translate/furigana）不是一套词，
  // 直接透传会 400「kind 非法」（实测踩到）。这里显式映射。
  var apiKind = { trans: "translate", translit: "translit", furi: "furigana", trad: "trad" }[kind] || kind;
  // Capture before starting HTTP. A late success or failure belongs to this
  // exact request, never to whichever song/language happens to be open later.
  var requestKey = lyricKey(), requestLang = lyricTargetLang();
  var requests = LYVIEW.requests || (LYVIEW.requests = {});
  var requestId = (requests[kind] || 0) + 1;
  requests[kind] = requestId;
  function currentRequest() {
    return requests[kind] === requestId && lyricKey() === requestKey && lyricTargetLang() === requestLang;
  }
  var body = "kind=" + encodeURIComponent(apiKind) +
             "&lang=" + encodeURIComponent(requestLang) +
             "&lines=" + encodeURIComponent(JSON.stringify(lines));
  toast(kind === "translate" ? "正在翻译歌词…" : "正在处理歌词…");
  // 不能用 api()：apiRaw 里硬编码 8 秒超时（那是给普通设置/列表请求定的）。
  // 一首 40 行歌词要 1~3 分钟（模型逐行推理），8 秒必然被 abort，用户只会看到"处理失败"。
  // 这里单独给 180 秒，并按"非幂等"处理：失败就是失败，不自动重放（重放会重复花钱）。
  var ctrl = (typeof AbortController !== "undefined") ? new AbortController() : null;
  var opt = { method: "POST", body: body };
  if (ctrl) opt.signal = ctrl.signal;
  var timer = setTimeout(function () { if (ctrl) ctrl.abort(); }, 180000);
  fetch("/api/lyrics/ai", opt).then(function (r) {
    return r.text().then(function (txt) {
      var j = null;
      try { j = JSON.parse(txt); } catch (e) { }
      if (!r.ok || !j) {
        throw new Error((j && j.error) ? j.error : ("HTTP " + r.status));
      }
      return j;
    });
  }).then(function (j) {
    clearTimeout(timer);
    if (!currentRequest()) return;
    if (!j.ok) {
      // 2026-10-30 实测踩到：请求失败时开关留在"开"⟹ 用户再点一次变成"关"，
      //   界面上什么都不会发生（既没译文也没提示），看起来像"点了没反应"。
      //   失败必须把开关退回关，让下一次点击还能重新发起。
      LYVIEW[kind] = 0; lyricViewSave(); applyLyricView();
      toast(j.error || "处理失败");
      return;
    }
    LYVIEW.data[kind] = { key: requestKey, lang: requestLang, lines: j.lines || [] };
    applyLyricView();
    toast(j.cached ? "已载入（缓存）" : "完成");
  }).catch(function (e) {
    clearTimeout(timer);
    if (!currentRequest()) return;
    LYVIEW[kind] = 0; lyricViewSave(); applyLyricView();   // 同上：失败即回退开关
    toast((e && e.message) ? e.message : "处理失败（网络或模型不可用）");
  });
}
function lyricToggle(kind) {
  if (LYVIEW[kind]) {
    LYVIEW[kind] = 0; lyricViewSave(); applyLyricView(); return;
  }
  LYVIEW[kind] = 1; lyricViewSave();
  // 已有同内容缓存就直接显示，否则现算
  if (lyricExtra(kind)) { applyLyricView(); return; }
  lyricAi(kind);
}
function showLyricMenu(x, y) {
  hideContextMenu();
  var m = document.createElement("div");
  m.className = "ctxmenu";
  m.style.left = Math.max(4, x) + "px";
  m.style.top = Math.max(4, y) + "px";
  m.style.minWidth = "188px";
  function head(t) {
    var d = document.createElement("div");
    d.className = "ctxhead"; d.textContent = t; m.appendChild(d);
  }
  function item(label, on, fn) {
    var d = document.createElement("div");
    d.className = "ctxitem" + (on ? " on" : "");
    // 勾选符必须**单独一个 span**：写进同一个文本节点会变成「✓ 标准」，
    // i18n 按整串查表永远查不到（实测英文界面里"标准"没被译）。
    var mk = document.createElement("span");
    mk.className = "ctxmk";
    mk.textContent = on ? "\u2713 " : "\u3000";
    d.appendChild(mk);
    d.appendChild(document.createTextNode(label));
    d.onclick = function (ev) { ev.stopPropagation(); hideContextMenu(); fn(); };
    m.appendChild(d);
  }
  function sep() { var d = document.createElement("div"); d.className = "ctxsep"; m.appendChild(d); }
  head("字号");
  for (var i = 0; i < LY_SIZE_PX.length; i++) {
    (function (idx) {
      item(LY_SIZE_TXT[idx], LYVIEW.size === idx, function () { LYVIEW.size = idx; lyricViewSave(); applyLyricView(); });
    })(i);
  }
  sep(); head("时间偏移（正 = 歌词提前）");
  [-500, -200, 0, 200, 500].forEach(function (ms) {
    item((ms > 0 ? "+" : "") + (ms / 1000).toFixed(1) + " s", LYVIEW.offsetMs === ms,
      function () { LYVIEW.offsetMs = ms; lyricViewSave(); applyLyricView(); });
  });
  sep();
  item("翻译歌词", !!LYVIEW.trans, function () { lyricToggle("trans"); });
  item("音译歌词", !!LYVIEW.translit, function () { lyricToggle("translit"); });
  item("日语假名机器标注", !!LYVIEW.furi, function () { lyricToggle("furi"); });
  item("显示繁体歌词", !!LYVIEW.trad, function () { lyricToggle("trad"); });
  // 目标语言作为尾注（单独一行）：拼进「翻译歌词（EN）」会让整串查不到译文
  (function () {
    var d = document.createElement("div");
    d.className = "ctxnote";
    d.textContent = "\u2192 " + lyricTargetLang().toUpperCase();   // 语言中性：不写死任何一种语言的说明文字
    m.appendChild(d);
  })();
  document.body.appendChild(m);
  _activeCtxMenu = m;
  var rr = m.getBoundingClientRect();
  if (x + rr.width > window.innerWidth - 6) m.style.left = Math.max(4, window.innerWidth - rr.width - 6) + "px";
  if (y + rr.height > window.innerHeight - 6) m.style.top = Math.max(4, window.innerHeight - rr.height - 6) + "px";
}

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
  // 2026-10-30：字号/译文/音译/假名/繁体任一变化都必须重建 DOM，
  // 否则用户点了右键菜单「翻译歌词」看不到任何变化（旧 key 只含行数与有无时间轴）。
  var key = lyricKey() + ':' + lyricTargetLang() + ':' + (timed ? 1 : 0) + ':' + LYVIEW.size + ':' +
            (LYVIEW.trans ? 1 : 0) + (LYVIEW.translit ? 1 : 0) + (LYVIEW.furi ? 1 : 0) + (LYVIEW.trad ? 1 : 0);
  if (box.dataset.n !== key) {
    box.dataset.n = key;
    var h = timed ? ''
      : '<div class=ly-hint title="这份歌词没有 [mm:ss] 时间轴，无法按播放位置高亮/跟随">纯文本歌词（无时间轴）· 不会自动跟随滚动</div>';
    // 2026-10-30（#4/#5）：每行可带三类附加行 + 假名注音。附加行只读 LYVIEW.data，
    // 没算过就是 null ⟹ 与旧行为完全一致（不多占一行高度）。
    var lvTr = LYVIEW.trans ? lyricExtra('trans') : null;
    var lvPh = LYVIEW.translit ? lyricExtra('translit') : null;
    var lvFu = LYVIEW.furi ? lyricExtra('furi') : null;
    var lvTd = LYVIEW.trad ? lyricExtra('trad') : null;
    LY.lines.forEach(function (l, i) {
      var raw = (lvTd && lvTd[i]) ? lvTd[i] : l.s;
      // 双语 LRC 的「原文 译文」在这里拆开（纯显示层，不动时间轴与高亮）
      var pair = lyricSplitPair(raw);
      var body = pair.main;
      if (lvFu && lvFu[i]) body = '<span class=ly-furi>' + esc(lvFu[i]) + '</span>' + esc(body);
      else body = esc(body);
      h += '<div class=npline data-i=' + i + '>' + body;
      if (lvTr && lvTr[i]) h += '<div class=ly-tr>' + esc(lvTr[i]) + '</div>';
      else if (pair.sub) h += '<div class=ly-tr>' + esc(pair.sub) + '</div>';
      if (lvPh && lvPh[i]) h += '<div class=ly-ph>' + esc(lvPh[i]) + '</div>';
      h += '</div>';
    });
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
  // 2026-10-30（#5「时间偏移」）：正偏移 = 歌词提前（与 LRC [offset:±ms] 同号约定）。
  // 放在这里而不是改 LY.lines[i].t：改时间戳会让「切歌/重载」后偏移叠加，越调越偏。
  var pos = (S.pos || 0) + (LYVIEW.offsetMs / 1000);
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
// 歌词右键菜单（2026-10-30 #5）：绑在 #npLyric 上，不影响其它区域的菜单。
(function bindLyricMenu() {
  var box = D('npLyric'); if (!box) return;
  box.addEventListener('contextmenu', function (ev) {
    if (!LY.lines.length) return;      // 没歌词时给系统菜单（用户可能想复制/检查）
    ev.preventDefault();
    ev.stopPropagation();
    showLyricMenu(ev.clientX, ev.clientY);
  });
})();
lyricViewLoad();
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
      toast(tf('已加入 {0} 项', [n]));
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
// 2026-11-13 反馈 FB-20261113-A916DFDBD3「CD无法拖入播放」：
// 拖文件夹时浏览器会把 desktop.ini / 封面 jpg / 说明 txt 一起给过来。旧实现照单全传，
// 后端也不筛，于是 desktop.ini 进了队列，播到它才报"ffmpeg 解不了"——用户看到的
// 就是"CD 拖不进去"。这里先按扩展名过滤，跳过的数量如实告诉用户。
var RL_DROP_EXT = { wav:1, wave:1, flac:1, alac:1, m4a:1, mp3:1, aac:1, ogg:1, opus:1,
                    wma:1, ape:1, wv:1, aif:1, aiff:1, aifc:1, caf:1, mka:1,
                    dsf:1, dff:1, cue:1, iso:1, cda:1 };
function rlDropExt(name) {
  var m = /\.([A-Za-z0-9]+)$/.exec(String(name || ''));
  return m ? m[1].toLowerCase() : '';
}
function dropFiles(e) {
  var paths = pathsFromDrop(e);
  if (paths.length) { addMany(paths); return; }
  var files = e.dataTransfer.files ? Array.prototype.slice.call(e.dataTransfer.files) : [];
  if (!files.length) return;
  var skipped = 0;
  files = files.filter(function (f) {
    if (RL_DROP_EXT[rlDropExt(f.name)]) return true;
    ++skipped; return false;
  });
  if (!files.length) {
    toast(skipped ? tf('跳过 {0} 个非音频文件', [skipped]) : '没有可加入的文件');
    return;
  }
  if (skipped) toast(tf('跳过 {0} 个非音频文件', [skipped]));
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

/* ---------------- 搜索框占位符（BUG-2：窄窗不许硬裁） ----------------
   2026-10-27 真窗口扫描：960 宽时顶栏搜索框只剩 ~125px，长占位「搜索专辑 / 艺术家 / 曲目…」
   被裁在字符中间（截图上是「搜索专辑 / 艺」），且 input 的 placeholder 天然没有省略号。
   判据不用魔法断点：量一下"这行字实际多宽"，装不下才换短文案（换字号/DPI/主题都不会误判）。 */
function phTextWidth(q, txt) {
  var m = D('phMeasure');
  if (!m) {
    m = document.createElement('span');
    m.id = 'phMeasure';
    m.style.cssText = 'position:absolute;left:-9999px;top:-9999px;visibility:hidden;' +
                      'white-space:nowrap;pointer-events:none';
    document.body.appendChild(m);
  }
  var cs = getComputedStyle(q);
  m.style.font = cs.font || '';
  m.style.fontSize = cs.fontSize;
  m.style.fontFamily = cs.fontFamily;
  m.style.letterSpacing = cs.letterSpacing;
  m.textContent = txt;
  return m.offsetWidth || 0;
}
function syncSearchPh() {
  var q = D('q');
  if (!q) return;
  var FULL = t('搜索专辑 / 艺术家 / 曲目…'), SHORT = t('搜索…');
  var cs = getComputedStyle(q);
  var avail = (q.clientWidth || 0) - (parseFloat(cs.paddingLeft) || 0) -
              (parseFloat(cs.paddingRight) || 0) - 2;   // -2：左右边框
  var need = phTextWidth(q, FULL);
  var ph;
  if (need > 0 && avail > 0) ph = (avail >= need) ? FULL : SHORT;
  else ph = ((window.innerWidth || 0) < 1100) ? SHORT : FULL;   // 量不出来时的兜底
  if (q.placeholder !== ph) q.placeholder = ph;
}
syncSearchPh();
window.addEventListener('resize', syncSearchPh);

/* ---------------- 启动 ---------------- */
bindTips();           // #6（261027-03）：? 帮助浮层改由固定层统一出，启动时挂一次事件委托
initFavs();
// #6（261026-08）：左栏「保存的队列」列表（我的最爱下面）启动就渲染，重启后能立刻看到
renderSavedQueues();
if (D('npFavBtn')) D('npFavBtn').onclick = function (ev) { if (S.lastCur) toggleFav(S.lastCur, ev); };
if (D('npSetCover')) D('npSetCover').onclick = function () {
  if (!S.lastCur) return;
  S.coverEditFile = S.lastCur;
  D('npCoverFile').value = '';
  D('npCoverFile').click();
};
if (D('npCoverFile')) D('npCoverFile').onchange = function (event) {
  var image = event.target.files && event.target.files[0];
  var file = S.coverEditFile;
  if (!image || !file) return;
  saveTrackCover(file, image).then(function () { toast('歌曲封面已设置'); })
    .catch(function (error) { toast(tf('设置失败：{0}', [error.message || String(error)])); });
};
if (D('npResetCover')) D('npResetCover').onclick = function () {
  var file = S.lastCur;
  if (!file) return;
  resetTrackCover(file).then(function () { toast('已恢复自动封面'); })
    .catch(function (error) { toast(tf('设置失败：{0}', [error.message || String(error)])); });
};
// 2.0 UI-1：单曲页「加入队列」与「推送到设备」移到底部控制排（与音量同一行，图标展示）
function initNpBottomControls() {
  var oldAdd = document.querySelector('.npinfo #npAddQ');
  if (oldAdd && oldAdd.parentElement) oldAdd.parentElement.remove();
  var vol = document.querySelector('.npvol');
  if (vol && !D('npAddQ')) {
    var addBtn = document.createElement('button');
    addBtn.className = 'k npctl-ibtn';
    addBtn.id = 'npAddQ';
    addBtn.title = '把当前曲目加入播放队列（追加在末尾；不改正在播的那首）';
    addBtn.innerHTML = '<svg width=16 height=16 viewBox="0 0 24 24" fill=none stroke=currentColor stroke-width=1.8 stroke-linecap=round stroke-linejoin=round><path d="M4 6h10M4 12h10M4 18h6"/><path d="M19 8v6M16 11h6"/></svg>';
    var castBtn = document.createElement('button');
    castBtn.className = 'k npctl-ibtn npcast';
    castBtn.id = 'npCastBtn';
    castBtn.title = '把当前曲目推送到 UPnP / DLNA 设备';
    castBtn.innerHTML = '<svg width=16 height=16 viewBox="0 0 24 24" fill=none stroke=currentColor stroke-width=1.8 stroke-linecap=round stroke-linejoin=round><path d="M2 16.1A5 5 0 0 1 5.9 20"/><path d="M2 12.05A9 9 0 0 1 9.95 20"/><path d="M2 8V6a2 2 0 0 1 2-2h16a2 2 0 0 1 2 2v12a2 2 0 0 1-2 2h-6"/><path d="M2 20h.01"/></svg>';
    vol.insertBefore(castBtn, vol.firstChild);
    vol.insertBefore(addBtn, castBtn);
  }
}
initNpBottomControls();
if (D('npAddQ')) D('npAddQ').onclick = function () {
  if (!S.lastCur) { toast('还没有在播放'); return; }
  api('/api/add', 'file=' + encodeURIComponent(S.lastCur)).then(function (j) {
    if (j && j.ok === false) { toast(j.error || '加入失败'); return; }
    var n = (j && j.count) || 0;
    toast(tf('已加入队列：{0}{1}', [nam(S.lastCur), (n ? ('（共 ' + n + ' 首）') : '')]));
    loadQ();
  }).catch(function () { toast('加入失败'); });
};
if (D('npCastBtn')) D('npCastBtn').onclick = castToggle;
loadFilterPhases();   // 功能2：相位预设真相源（/api/filters）
fillMetaProviders();   // P1.5：数据源注册表（/api/providers）驱动元数据分组源开关
// 2026-10-28（P0）：**必须在启动时加载 conf**。
//   此前这里没有调用 loadSettings()，于是全站设置控件都停在 HTML 默认值上：
//   用户看到的「深色 / 自动 / 5 阶 / DSD256…」其实是硬编码默认，不是他的 conf；
//   而他每点一次「保存设置」，就把这些默认值写回 conf，**覆盖掉自己的配置**。
//   anlmaster / autoanl 之所以「改了不生效」，根子都在这一行缺失。
loadSettings();
// 2026-10-30（用户 261029-03）：界面语言。放在 loadSettings 之后 ——
//   它要读 conf 里的 uilang（"auto" = 跟随系统），而那个值由 loadSettings 解析；
//   但即使 loadSettings 还没回来，i18nBoot 也会先用 /api/i18n 的默认值把界面译好。
i18nBoot();
loadRates();
// The saved preference arrives asynchronously. Start in the library and let
// the first successful read choose the landing page only if navigation is untouched.
syncQuickStartVisibility();
loadAlbums('');
setInterval(function () {
  if(rlHomeActive()&&PLAYBACK_SETTINGS.ready&&!rlConsole.busy)rlConsole.refresh().catch(function(){});
}, 2500);
loadQ();
loadSrcConfig();
tick();
setInterval(tick, 400);
setInterval(loadQ, 3000);
setInterval(loadLibStats, 2000);
)JSEOF";
}

inline const char* rta_ui_script() {
    static const std::string script = std::string(rta_ui_waveform_js()) + "\n" + rta_ui_workbench_state_js() + "\n" + rta_ui_eq_console_js() + "\n" + rta_ui_studio_js() + "\n" + rta_ui_listening_home_js() + "\n" + rta_ui_listening_library_js() + "\n" + rta_ui_core_script();
    return script.c_str();
}

} // namespace rta::net
