#pragma once
namespace rta::net {
inline const char* rta_ui_eq_console_css(){return R"EQCONSOLE(
.rlEqToolbar,.rlEqBankTools{display:flex;align-items:center;gap:10px;flex-wrap:wrap}.rlEqToolbar{justify-content:space-between;margin-bottom:10px}.rlEqToolbar label{font-size:12px}.rlEqToolbar input[type=number]{width:90px;padding:6px;border:1px solid var(--desk-line);background:var(--desk-field);color:var(--desk-text);border-radius:3px}.rlEqStatus{font-size:11px;color:var(--desk-muted);margin:9px 0}.rlEqMessage{font-size:12px;color:var(--desk-brass);min-height:18px;line-height:1.5}.rlEqMessage[data-error=true]{color:var(--rl-error,#ffbdad)}.rlEqcv{position:relative;width:100%;padding:10px;border:1px solid var(--desk-line);border-radius:5px;background:var(--desk-field);--line:var(--desk-line);--acc:var(--desk-brass);--mut2:var(--desk-muted)}.rlEqsvg{display:block;width:100%;height:auto;touch-action:none;overflow:visible}.rlEqhit{fill:transparent;stroke:none;cursor:grab}.rlEqnode{fill:var(--desk-brass);stroke:var(--desk-wood);stroke-width:1.5;pointer-events:none}.rlEqnode.off{opacity:.3}.rlEqnode.on{stroke:var(--desk-text);stroke-width:2}.rlEqlegend{display:flex;gap:8px 18px;flex-wrap:wrap;color:var(--desk-muted);font-size:10px;margin-top:9px}.rlEqlegend b{color:var(--desk-text);font-weight:500}.rlEqtip{position:absolute;display:none;pointer-events:none;padding:6px 8px;border:1px solid var(--desk-line);background:var(--desk-panel);border-radius:4px;font-size:11px;color:var(--desk-text);z-index:4}.rlEqtip.on{display:block}
/* 2026-11-12：我的均衡 / OPRA 置顶折叠；调音台独立于 A–D 组。 */
.rlEqFold{border:1px solid var(--desk-line);border-radius:6px;background:var(--desk-panel);margin:0 0 12px;overflow:hidden}
.rlEqFoldHead{display:flex;align-items:center;gap:12px;width:100%;padding:11px 13px;border:0;background:none;color:var(--desk-text);text-align:start;cursor:pointer;font:inherit}
.rlEqFoldHead:hover{background:var(--desk-field)}
.rlEqFoldHead>span{font:600 15px/1.3 var(--rl-serif,Georgia,serif);white-space:nowrap}
.rlEqFoldHead>small{color:var(--desk-muted);font-size:11px;flex:1;min-width:0;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.rlEqFoldHead>b{color:var(--desk-brass);font-size:11px;font-weight:500;white-space:nowrap}
.rlEqFoldBody{padding:0 13px 13px;border-top:1px solid var(--desk-line)}
.rlEqFoldBody[hidden]{display:none}
.rlEqMain{margin-top:4px;padding-top:14px;border-top:1px solid var(--desk-line)}
.rlEqMainFold{margin-top:4px}.rlEqMainFold .rlEqFoldBody{padding-top:14px}
.rlEqToolbarBtns{display:flex;gap:8px;flex-wrap:wrap}
.rlEqBankTools>label{font-size:11px;color:var(--desk-muted)}
.rlEqBankTools>input{flex:1 1 180px}
.rlEqOpraSave{display:flex;gap:10px;align-items:center;flex-wrap:wrap;margin-top:12px;padding:11px 12px;border:1px solid var(--desk-line);border-radius:5px;background:var(--desk-field)}
.rlEqOpraSave[hidden]{display:none}.rlEqOpraSave p{flex-basis:100%;margin:0}.rlEqOpraSave label{font-size:11px;color:var(--desk-muted)}.rlEqOpraSave select,.rlEqOpraSave input{min-height:32px;padding:6px 8px;border:1px solid var(--desk-line);border-radius:3px;background:var(--desk-field);color:var(--desk-text)}.rlEqOpraSave input{flex:1 1 200px;min-width:120px}.rlEqrows{margin-top:12px;max-height:380px;overflow:auto;border:1px solid var(--desk-line);border-radius:5px}.rlEqhd,.rlEqr{display:grid;grid-template-columns:30px minmax(86px,1.3fr) minmax(72px,1fr) minmax(64px,.8fr) minmax(86px,1fr) 38px;gap:8px;align-items:center;min-width:490px;padding:6px 10px}.rlEqhd{position:sticky;top:0;background:var(--desk-panel);font-size:10px;color:var(--desk-muted);z-index:1}.rlEqhd>*{text-align:center!important}.rlEqr{border-top:1px solid var(--desk-line);font-size:12px}.rlEqr.sel{background:rgba(217,184,117,.16);background:color-mix(in srgb,var(--desk-brass) 16%,var(--desk-panel))}.rlEqn{font:11px ui-monospace,Consolas,monospace;color:var(--desk-brass)}.rlEqc{min-width:0}.rlEqc input,.rlEqc select{font:12px ui-monospace,Consolas,monospace!important;width:100%;min-width:0;border:1px solid var(--desk-line);border-radius:3px;padding:5px 6px;background:var(--desk-field);color:var(--desk-text)}.rlEqc input[type=checkbox]{width:15px;height:15px;accent-color:var(--desk-brass)}.rlEqc.num,.rlEqhd .num{text-align:end}.rlEqc.c,.rlEqhd .c{text-align:center}.rlEqBank{border-top:1px solid var(--desk-line);padding-top:16px;margin-top:20px}.rlEqBankTools select,.rlEqBankTools input,.rlEqOpra input{min-height:34px;padding:7px 10px;min-width:0;border:1px solid var(--desk-line);background:var(--desk-field);color:var(--desk-text);border-radius:3px;font-size:12px}.rlEqBankTools select{min-width:140px;max-width:100%}.rlEqBankTools input{flex:1;min-width:130px}.rlEqBankGrid{display:grid;grid-template-columns:repeat(8,minmax(0,1fr));gap:5px;margin-top:9px}.rlEqBankSlot{padding:6px 8px;background:var(--desk-field);border:1px solid var(--desk-line);color:var(--desk-muted);border-radius:4px;text-align:start;min-width:0;font-size:10px}.rlEqBankSlot[aria-pressed=true]{border-color:var(--desk-brass);color:var(--desk-brass)}.rlEqBankSlot[data-active=true]{box-shadow:inset 0 2px var(--desk-brass)}.rlEqBankSlot b{display:block;overflow:hidden;white-space:nowrap;text-overflow:ellipsis;font-size:11px;font-weight:500;margin-bottom:2px} .rlEqBankSlot i{display:none!important}.rlEqBankSlot span,.rlEqBankSlot i{display:block;font-style:normal;font-size:10px;line-height:1.45;color:var(--desk-muted);white-space:nowrap;overflow:hidden;text-overflow:ellipsis}.rlEqOpra{border-top:1px solid var(--desk-line);padding-top:16px;margin-top:20px}.rlEqOpra h3{font:600 16px var(--rl-serif,Georgia,serif);margin:0 0 8px}.rlEqOpra input{width:100%}.rlEqOpraResults{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:7px;margin:10px 0;max-height:245px;overflow:auto}.rlEqModel,.rlEqOpraPreset{padding:10px 12px;border:1px solid var(--desk-line);border-radius:4px;background:var(--desk-field);color:var(--desk-text);text-align:start;font-size:12px}.rlEqModel[aria-pressed=true]{border-color:var(--desk-brass)}.rlEqOpraPreset strong{display:block;margin-bottom:6px}.rlEqOpraPreset p{font-size:11px;line-height:1.5;color:var(--desk-muted);margin:5px 0 10px}.rlEqOpraPresets{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:10px}.rlEqOpra a{color:var(--desk-brass)}
.rlEqOpraActions{display:flex;gap:7px;flex-wrap:wrap}.rlEqOpraActions .btn{min-height:28px;padding:4px 10px;font-size:11px}.rlEqOpraPreset[data-preview=true]{border-color:var(--desk-brass);box-shadow:inset 0 0 0 1px var(--desk-brass)}
.rlEqBankHint{flex:1 1 220px;font-size:11px;color:var(--desk-muted);line-height:1.5}.rlEqBankSlot b[data-eq-name]{cursor:text}.rlEqRenameInput{width:100%;box-sizing:border-box;font:600 13px/1.3 var(--rl-serif,Georgia,serif);padding:3px 5px;border:1px solid var(--desk-brass);border-radius:3px;background:var(--desk-field);color:var(--desk-text)}
/* 3.3.9：二级窗口必须整屏居中（旧样式没写 inset/margin，部分 WebView2 版本会贴到左上角）。 */
.rlEqDialog{box-sizing:border-box;position:fixed;inset:0;margin:auto;width:min(440px,calc(100vw - 32px));max-height:calc(100dvh - 40px);overflow:auto;padding:0;border:1px solid var(--desk-line);border-radius:10px;background:var(--desk-panel);color:var(--desk-text);box-shadow:0 22px 80px #0008}.rlEqDialog::backdrop{background:#100e0bc9}.rlEqDialogCard{margin:0;padding:18px}.rlEqDialogCard header{display:flex;align-items:flex-start;justify-content:space-between;gap:12px;margin-bottom:14px}.rlEqDialogCard h2{font:600 19px/1.3 var(--rl-serif,Georgia,serif);margin:0}.rlEqDialogCard header p{font-size:11px;line-height:1.5;color:var(--desk-muted);margin:5px 0 0;min-height:16px}.rlEqDialogClose{border:0;background:none;color:var(--desk-muted);font-size:22px;line-height:1;cursor:pointer;padding:0 4px}.rlEqDialogField{display:flex;flex-direction:column;gap:6px;font-size:11px;color:var(--desk-muted);margin:0 0 12px}.rlEqDialogField select,.rlEqDialogField input{min-height:34px;padding:7px 9px;border:1px solid var(--desk-line);border-radius:4px;background:var(--desk-field);color:var(--desk-text);font:inherit}.rlEqDialogCard footer{display:flex;justify-content:flex-end;gap:8px;border-top:1px solid var(--desk-line);padding-top:13px;margin-top:4px}
@container(max-width:1050px){.rlEqBankGrid{grid-template-columns:repeat(4,minmax(0,1fr))}}@container(max-width:650px){.rlEqBankGrid{grid-template-columns:repeat(2,minmax(0,1fr))}.rlEqOpraResults,.rlEqOpraPresets{grid-template-columns:1fr}.rlEqToolbar{align-items:flex-start}}
)EQCONSOLE";}
inline const char* rta_ui_eq_console_js(){return R"EQCONSOLE(
var RL_EQ_STATE = null;
var RL_EQ_TIMER = 0;

function rlEqRowsEl() { return document.querySelectorAll('#rlEqRows .rlEqr'); }
// 频率输入框接受友好写法：'20 Hz' / '1.02 kHz' / '1020' / '1.02k'
function rlEqParseFreq(s) {
  var t = String(s == null ? '' : s).toLowerCase().replace(/\s+/g, '');
  var m = /^([0-9]*\.?[0-9]+)(k|khz|hz)?$/.exec(t);
  if (!m) return NaN;
  var f = parseFloat(m[1]);
  if (m[2] === 'k' || m[2] === 'khz') f *= 1000;
  return f;
}
// 友好显示：<1 kHz 用 Hz（20 Hz），>=1 kHz 用 kHz（1.02 kHz / 10.0 kHz）
function rlEqFreqText(f) {
  f = +f || 0;
  if (f >= 10000) return (f / 1000).toFixed(1) + ' kHz';
  if (f >= 1000) return (f / 1000).toFixed(2) + ' kHz';
  return Math.round(f) + ' Hz';
}
function rlEqClampFreq(f) { return Math.max(10, Math.min(20000, +f || 1000)); }
function rlEqRowBand(tr) {
  var f = tr.querySelector('.rlEqF'), g = tr.querySelector('.rlEqG'), q = tr.querySelector('.rlEqQ');
  var t = tr.querySelector('.rlEqT'), on = tr.querySelector('.rlEqOn');
  var fv = rlEqParseFreq(f.value);
  if (!isFinite(fv) || fv <= 0) fv = 1000;
  var fclamped = rlEqClampFreq(fv);
  // 超出 10 Hz–20 kHz 的输入不放任停在界外：立刻回写合法值，避免"界面 20001、实际 20000"两套口径。
  if (Math.abs(fclamped - fv) > 1e-9) f.value = rlEqFreqText(fclamped);
  return {
    i: +tr.dataset.i, type: t.value, freq: fclamped,
    gain: Math.max(-24, Math.min(24, parseFloat(g.value) || 0)),
    q: Math.max(0.1, Math.min(20, parseFloat(q.value) || 1.41)),
    on: on.checked
  };
}
function rlEqCollectBands() {
  var out = [];
  Array.prototype.forEach.call(rlEqRowsEl(), function (tr) { out.push(rlEqRowBand(tr)); });
  return out;
}
function rlEqBandsParam() {
  return rlEqCollectBands().map(function (b) {
    return b.i + ',' + b.type + ',' + b.freq + ',' + b.gain + ',' + b.q + (b.on ? '' : ',off');
  }).join(';');
}
// 频响曲线：后端 /api/rlEq 直接给 [频率, dB] 点（与 C++ 设计公式同源，前端不抄第二份）
// 261024-03 #5 HIFI 化：曲线上叠 20 个频段节点（可拖/滚轮/双击），并做增量更新
//（增量很关键：拖拽时 rlEqApply 每次回来都重建 SVG 会丢掉 pointer capture，拖到一半就断）
var RL_EQ_VIEW = null;      // 曲线坐标映射（拖拽换算要用）
var RL_EQ_DRAG = null;      // {i, mode:'g'|'q', y0, q0}
var RL_EQ_LASTCLICK = null; // {i, t}：自己判双击（不依赖宿主是否派发 dblclick）
function rlEqFreqX(f) {
  var v = RL_EQ_VIEW; if (!v) return 0;
  return v.PADL + (Math.log(Math.max(20, Math.min(20000, f)) / 20) / Math.log(1000)) * (v.W - v.PADL - v.PADR);
}
function rlEqGainY(db) {
  var v = RL_EQ_VIEW; if (!v) return 0;
  return v.PADT + (v.maxDb - db) / (v.maxDb - v.minDb) * (v.H - v.PADT - v.PADB);
}
function rlEqCurveFreqAt(x) {
  var v = RL_EQ_VIEW; if (!v) return 1000;
  var t = (x - v.PADL) / (v.W - v.PADL - v.PADR);
  return rlEqClampFreq(20 * Math.pow(1000, Math.max(0, Math.min(1, t))));
}
function rlEqCurveGainAt(y) {
  var v = RL_EQ_VIEW; if (!v) return 0;
  var t = (y - v.PADT) / (v.H - v.PADT - v.PADB);
  return Math.max(-24, Math.min(24, v.maxDb - Math.max(0, Math.min(1, t)) * (v.maxDb - v.minDb)));
}
/* #3（2026-10-27 用户 261026-09）：「PEQ 里面的波形和下面的表格，需要根据软件宽度自动匹配宽度」。
   以前这里 W 写死 640、CSS 又给 .rlEqcv 加 max-width:720px ⟹ 1600px 的窗口右边一整片空白。
   现在 W 由容器的真实客户区宽度算出（不是靠 CSS 把 640 拉伸 —— 那样横轴刻度会跟着变形），
   并在宽度变化时用 ResizeObserver 重算（见下文 rlEqObserveWidth）。 */
var RL_EQ_PTS = null;    // 最近一次曲线点（宽度变了要按新宽度重画）
var RL_EQ_W = 0;         // 当前逻辑宽度（= 容器内容宽度），用于判断"值得重画"
var RL_EQ_RO = null;     // ResizeObserver（只观察 #rlEqCurve 一个节点）
function rlEqContentWidth(box) {
  // clientWidth 含 padding；.rlEqcv 的左右 padding 各 10px ⟹ content = clientWidth - 20
  var w = (box && box.clientWidth) ? (box.clientWidth - 20) : 0;
  return Math.max(180, Math.floor(w || 0) || 640);
}
var RL_EQ_RO_BOX = null;
function rlEqObserveWidth() {
  var box = D('rlEqCurve'); if (!box) return;
  // 设置页每次重渲染都会重建 #rlEqCurve 节点：观察者要跟着换目标，否则观察的是一个已脱离文档的旧节点
  if (RL_EQ_RO && RL_EQ_RO_BOX === box) return;
  if (RL_EQ_RO) { try { RL_EQ_RO.disconnect(); } catch (e) {} RL_EQ_RO = null; }
  RL_EQ_RO_BOX = box;
  if (!window.ResizeObserver) return;               // 老引擎：靠下面的 window resize 兜底
  RL_EQ_RO = new ResizeObserver(function () {
    var w = rlEqContentWidth(box);
    if (Math.abs(w - RL_EQ_W) < 4) return;            // 高度由 viewBox 比例决定 ⟹ 不会自激循环
    if (RL_EQ_PTS) rlEqDrawCurve(RL_EQ_PTS);
  });
  RL_EQ_RO.observe(box);
}
// 兜底：宿主的窗口缩放（WebView2 里 ResizeObserver 已经能覆盖，双保险不冲突）
window.addEventListener('resize', function () { if (RL_EQ_PTS) rlEqOnResize(); });
function rlEqOnResize() {
  var box = D('rlEqCurve'); if (!box) return;
  if (Math.abs(rlEqContentWidth(box) - RL_EQ_W) >= 4 && RL_EQ_PTS) rlEqDrawCurve(RL_EQ_PTS);
}
function rlEqDrawCurve(points) {
  var box = D('rlEqCurve'); if (!box) return;
  if (!points || !points.length) {
    box.innerHTML = '<div style="font-size:11.5px;color:var(--mut2);padding:12px 0">频响曲线（等待数据…）</div>';
    RL_EQ_VIEW = null; RL_EQ_PTS = null;
    return;
  }
  RL_EQ_PTS = points;
  var W = rlEqContentWidth(box);
  // 高度跟着宽度走：宽窗不至于把曲线压成一条细带（1600px 时约 210px 高）
  var H = Math.round(Math.min(220, Math.max(120, W * 0.19)));
  var PADL = 36, PADR = 10, PADT = 10, PADB = 18;
  RL_EQ_W = W;
  var maxDb = 6, minDb = -6;
  points.forEach(function (p) { maxDb = Math.max(maxDb, p[1]); minDb = Math.min(minDb, p[1]); });
  rlEqCollectBands().forEach(function(b){maxDb=Math.max(maxDb,b.gain);minDb=Math.min(minDb,b.gain);});
  maxDb = Math.ceil(maxDb / 3) * 3; minDb = Math.floor(minDb / 3) * 3;
  RL_EQ_VIEW = { W: W, H: H, PADL: PADL, PADR: PADR, PADT: PADT, PADB: PADB, maxDb: maxDb, minDb: minDb };

  var g = '';
  for (var d = Math.ceil(minDb / 3) * 3; d <= maxDb; d += 3) {
    var major=(d % 6 === 0);
    g += '<line x1="' + PADL + '" y1="' + rlEqGainY(d).toFixed(1) + '" x2="' + (W - PADR) + '" y2="' + rlEqGainY(d).toFixed(1) +
         '" stroke="var(--line)" stroke-width="1" stroke-dasharray="' + (d === 0 ? '4 3' : (major ? '2 4' : '1 5')) + '" opacity="' + (d === 0 ? '0.95' : (major ? '0.5' : '0.25')) + '"/>';
    if (major) g += '<text x="' + (PADL - 4) + '" y="' + (rlEqGainY(d) + 3).toFixed(1) + '" font-size="9" fill="var(--mut2)" text-anchor="end">' + d + '</text>';
  }
  // 2026-11-12：专业 EQ 的刻度不再只有 20/100/1k/10k/20k。
  // 1/3 倍频程细网格 + 10 个主刻度标签，横轴才不会像"玩具 EQ"。
  var minorFreq=[25,31.5,40,63,80,125,160,250,315,400,630,800,1250,1600,2500,3150,4000,6300,8000,12500,16000];
  minorFreq.forEach(function(f){
    var x=rlEqFreqX(f);
    g += '<line x1="'+x.toFixed(1)+'" y1="'+PADT+'" x2="'+x.toFixed(1)+'" y2="'+(H-PADB)+'" stroke="var(--line)" stroke-width="1" stroke-dasharray="1 5" opacity=".28"/>';
  });
  [[20, '20'], [50, '50'], [100, '100'], [200, '200'], [500, '500'], [1000, '1k'], [2000, '2k'], [5000, '5k'], [10000, '10k'], [20000, '20k']].forEach(function (fk) {
    var x = rlEqFreqX(fk[0]);
    g += '<line x1="' + x.toFixed(1) + '" y1="' + PADT + '" x2="' + x.toFixed(1) + '" y2="' + (H - PADB) + '" stroke="var(--line)" stroke-width="1" stroke-dasharray="2 4" opacity=".55"/>';
    g += '<text x="' + x.toFixed(1) + '" y="' + (H - 4) + '" font-size="9" fill="var(--mut2)" text-anchor="middle">' + fk[1] + '</text>';
  });
  var pts = points.map(function (p) { return rlEqFreqX(p[0]).toFixed(1) + ',' + rlEqGainY(p[1]).toFixed(1); }).join(' ');

  var svg = box.querySelector('svg.rlEqsvg');
  if (!svg) {
    box.innerHTML =
      '<svg class=rlEqsvg viewBox="0 0 ' + W + ' ' + H + '" preserveAspectRatio="none" data-w="' + W + '">' +
        '<g class=rlEqgrid></g>' +
        '<polyline class=rlEqline fill=none stroke="var(--acc)" stroke-width=1.8 stroke-linejoin=round stroke-linecap=round points=""/>' +
        '<g class=rlEqnodes></g>' +
      '</svg>' +
      '<div class=rlEqlegend><span><b>拖拽节点</b> 水平=频率 · 垂直=增益</span><span><b>Shift+拖 / 滚轮</b> 改 Q</span>' +
      '<span><b>双击</b> 该段复位</span><span><b>↑↓←→</b> 微调（Shift 加速）</span></div>' +
      '<div class=rlEqtip id=rlEqTip></div>';
    svg = box.querySelector('svg.rlEqsvg');
    rlEqBindCurve();
    rlEqObserveWidth();
  }
  // 宽度变了要同步 viewBox（否则 grid 坐标与比例对不上）
  svg.setAttribute('viewBox', '0 0 ' + W + ' ' + H);
  svg.setAttribute('data-w', String(W));
  svg.querySelector('.rlEqgrid').innerHTML = g;
  svg.querySelector('.rlEqline').setAttribute('points', pts);
  rlEqSyncNodes();
}
/* ---------------- PEQ：行渲染 / 选中 / 曲线节点交互（261024-03 #5） ---------------- */
function rlEqSelectRow(tr) {
  Array.prototype.forEach.call(rlEqRowsEl(), function (x) { x.classList.toggle('sel', x === tr); });
}
function rlEqRowTipText(b) {
  return '<b>' + rlEqFreqText(b.freq) + '</b> · ' + (b.gain >= 0 ? '+' : '') + b.gain.toFixed(1) +
         ' dB · Q ' + b.q.toFixed(2) + ' · ' + b.type + (b.on ? '' : t('（已关）')); 
}
// tooltip 定位：有事件用鼠标位置；没有（滚轮/键盘）就用节点自身位置
function rlEqTipAt(html, ev, node) {
  var tip = D('rlEqTip'), box = D('rlEqCurve');
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
function rlEqTipHide() { var t = D('rlEqTip'); if (t) t.classList.remove('on'); }
function rlEqResetBand(i) {
  var tr = document.querySelector('#rlEqRows .rlEqr[data-i="' + i + '"]'); if (!tr) return;
  tr.querySelector('.rlEqG').value = '0.0';
  tr.querySelector('.rlEqQ').value = '1.41';
  rlEqSyncNodes();
  rlEqScheduleApply(100);
  rlEqMsg(tf('第 {0} 段已复位（0.0 dB / Q 1.41）', [i]));
}
function rlEqSvgPoint(ev) {
  var box = D('rlEqCurve');
  var svg = box && box.querySelector('svg.rlEqsvg');
  if (!svg || !RL_EQ_VIEW) return null;
  var r = svg.getBoundingClientRect();
  if (!r.width || !r.height) return null;
  return { x: (ev.clientX - r.left) / r.width * RL_EQ_VIEW.W,
           y: (ev.clientY - r.top) / r.height * RL_EQ_VIEW.H };
}
function rlEqNodeRow(i) { return document.querySelector('#rlEqRows .rlEqr[data-i="' + i + '"]'); }
function rlEqBindCurve() {
  var box = D('rlEqCurve'); if (!box) return;
  var svg = box.querySelector('svg.rlEqsvg'); if (!svg) return;
  var pick = function (ev) {
    var t = ev.target;
    while (t && t !== svg) { if (t.dataset && t.dataset.i) return t; t = t.parentNode; }
    return null;
  };
  svg.addEventListener('pointermove', function (ev) {
    if (RL_EQ_DRAG) return;
    var node = pick(ev);
    if (node) {
      var tr = rlEqNodeRow(node.dataset.i);
      if (tr) rlEqTipAt(rlEqRowTipText(rlEqRowBand(tr)), ev);
      return;
    }
    var p = rlEqSvgPoint(ev); if (!p) return;
    var f = rlEqCurveFreqAt(p.x), pts = (RL_EQ_STATE && RL_EQ_STATE.points) || [], db = null;
    for (var i = 0; i < pts.length; i++) { if (pts[i][0] >= f) { db = pts[i][1]; break; } }
    if (db === null && pts.length) db = pts[pts.length - 1][1];
    rlEqTipAt('<b>' + rlEqFreqText(f) + '</b>' + (db === null ? '' : t(' · 曲线 ') + (db >= 0 ? '+' : '') + db.toFixed(1) + ' dB'), ev);
  });
  svg.addEventListener('pointerleave', function () { if (!RL_EQ_DRAG) rlEqTipHide(); });
  svg.addEventListener('dblclick', function (ev) {
    var node = pick(ev); if (!node) return;
    ev.preventDefault();
    rlEqResetBand(+node.dataset.i);
  });
  svg.addEventListener('wheel', function (ev) {
    var node = pick(ev); if (!node) return;
    ev.preventDefault();
    var tr = rlEqNodeRow(node.dataset.i); if (!tr) return;
    var qEl = tr.querySelector('.rlEqQ'), v = parseFloat(qEl.value) || 1.41;
    v = Math.max(0.1, Math.min(20, v * Math.pow(1.15, ev.deltaY > 0 ? 1 : -1)));
    qEl.value = v.toFixed(2);
    rlEqSyncNodes();
    rlEqTipAt(rlEqRowTipText(rlEqRowBand(tr)), null, node);
    rlEqScheduleApply(100);
  }, { passive: false });
  svg.addEventListener('pointerdown', function (ev) {
    var node = pick(ev); if (!node) return;
    ev.preventDefault();
    var i = +node.dataset.i, tr = rlEqNodeRow(i); if (!tr) return;
    rlEqSelectRow(tr);
    // 双击 = 该段复位。自己在 pointerdown 里判（350ms 内同一节点），
    // 因为 pointerdown 里 preventDefault + setPointerCapture 会让宿主的 dblclick 时序不可靠。
    var now = Date.now();
    if (RL_EQ_LASTCLICK && RL_EQ_LASTCLICK.i === i && now - RL_EQ_LASTCLICK.t < 350) {
      RL_EQ_LASTCLICK = null;
      rlEqResetBand(i);
      RL_EQ_DRAG = null;
      return;
    }
    RL_EQ_LASTCLICK = { i: i, t: now };
    RL_EQ_DRAG = { i: i, mode: ev.shiftKey ? 'q' : 'g', y0: ev.clientY,
                 q0: parseFloat(tr.querySelector('.rlEqQ').value) || 1.41 };
    try { svg.setPointerCapture(ev.pointerId); } catch (e) { }
    var c = svg.querySelector('.rlEqnode[data-i="' + i + '"]'); if (c) c.classList.add('on');
  });
  svg.addEventListener('pointermove', function (ev) {
    if (!RL_EQ_DRAG) return;
    var tr = rlEqNodeRow(RL_EQ_DRAG.i); if (!tr) return;
    if (RL_EQ_DRAG.mode === 'q') {
      var qv = Math.max(0.1, Math.min(20, RL_EQ_DRAG.q0 * Math.pow(2, (RL_EQ_DRAG.y0 - ev.clientY) / 60)));
      tr.querySelector('.rlEqQ').value = qv.toFixed(2);
    } else {
      var p = rlEqSvgPoint(ev); if (!p) return;
      tr.querySelector('.rlEqF').value = rlEqFreqText(rlEqCurveFreqAt(p.x));
      tr.querySelector('.rlEqG').value = rlEqCurveGainAt(p.y).toFixed(1);
    }
    rlEqSyncNodes();
    var node = svg.querySelector('.rlEqnode[data-i="' + RL_EQ_DRAG.i + '"]');
    rlEqTipAt(rlEqRowTipText(rlEqRowBand(tr)), ev, node);
    rlEqScheduleApply(100);           // 拖拽 100ms 节流
  });
  var endDrag = function () {
    if (!RL_EQ_DRAG) return;
    var i = RL_EQ_DRAG.i;
    RL_EQ_DRAG = null;
    var c = svg.querySelector('.rlEqnode[data-i="' + i + '"]'); if (c) c.classList.remove('on');
    rlEqTipHide();
    clearTimeout(RL_EQ_TIMER); rlEqApply().catch(function(){});   // 松手立刻落一次，不等节流窗口
  };
  svg.addEventListener('pointerup', endDrag);
  svg.addEventListener('pointercancel', endDrag);
}
// 把 20 个节点同步到当前行值（拖拽/键盘/滚轮后立即反映，不等后端）
function rlEqSyncNodes() {
  var box = D('rlEqCurve');
  var svg = box && box.querySelector('svg.rlEqsvg');
  if (!svg || !RL_EQ_VIEW) return;
  var host = svg.querySelector('.rlEqnodes'); if (!host) return;
  var bands = rlEqCollectBands();
  if (!bands.length && RL_EQ_STATE && RL_EQ_STATE.bands) bands = RL_EQ_STATE.bands;
  if (host.querySelectorAll('circle.rlEqhit').length !== bands.length) {
    var h = '';
    bands.forEach(function (b, i) {
      h += '<circle class=rlEqhit data-i=' + (i + 1) + ' r=13></circle>'
         + '<circle class="rlEqnode' + (b.on === false ? ' off' : '') + '" data-i=' + (i + 1) + ' r=4.5></circle>';
    });
    host.innerHTML = h;
  }
  Array.prototype.forEach.call(host.querySelectorAll('circle.rlEqnode'), function (c) {
    var i = +c.dataset.i - 1, b = bands[i];
    if (!b) return;
    var cx = rlEqFreqX(b.freq).toFixed(1), cy = rlEqGainY(b.gain).toFixed(1);
    c.setAttribute('cx', cx); c.setAttribute('cy', cy);
    c.classList.toggle('off', b.on === false);
    var hit = host.querySelector('circle.rlEqhit[data-i="' + (i + 1) + '"]');
    if (hit) { hit.setAttribute('cx', cx); hit.setAttribute('cy', cy); }
  });
}
function rlEqRenderRows(j) {
  var box = D('rlEqRows'); if (!box) return;
  // 表头与数据行同处 #rlEqRows：同一个内容宽度 ⟹ 同一套列轨道（这是"对齐"的根）
  var h = '<div class=rlEqhd><span>段</span><span class=num>频率</span><span class=num>增益 dB</span>'
        + '<span class=num>Q</span><span>类型</span><span class=c>启用</span></div>';
  (j.bands || []).forEach(function (b, i) {
    var sel = '<select class=rlEqT>'
      + '<option value=PK' + (b.type === 'PK' ? ' selected' : '') + '>峰值 PK</option>'
      + '<option value=LS' + (b.type === 'LS' ? ' selected' : '') + '>低架 LS</option>'
      + '<option value=HS' + (b.type === 'HS' ? ' selected' : '') + '>高架 HS</option></select>';
    h += '<div class=rlEqr data-i=' + (i + 1) + ' tabindex=0>'
       + '<span class=rlEqn>' + (i + 1 < 10 ? '0' : '') + (i + 1) + '</span>'
       + '<span class=rlEqc><input class=rlEqF type=text spellcheck=false value="' + esc(rlEqFreqText(b.freq)) + '"></span>'
       + '<span class="rlEqc num"><input class=rlEqG type=number step=0.1 min=-24 max=24 value="' + (+b.gain).toFixed(1) + '"></span>'
       + '<span class="rlEqc num"><input class=rlEqQ type=number step=0.01 min=0.1 max=20 value="' + (+b.q).toFixed(2) + '"></span>'
       + '<span class=rlEqc>' + sel + '</span>'
       + '<span class="rlEqc c"><input class=rlEqOn type=checkbox' + (b.on ? ' checked' : '') + '></span>'
       + '</div>';
  });
  // 重建表格会移除正在聚焦的输入；Edge 可能因此触发一次 change。用 rendering 闸门挡住，
  // 否则切组/刷新会把“重建 UI”误当成用户编辑，凭空发一次 /api/peq。
  RL_EQ.rendering = true;
  box.innerHTML = h;
  Array.prototype.forEach.call(box.querySelectorAll('.rlEqr'), function (tr) {
    var inputs = tr.querySelectorAll('input,select');
    Array.prototype.forEach.call(inputs, function (el) {
      el.addEventListener('change', function () { rlEqScheduleApply(); });
      el.addEventListener('input', function () { rlEqScheduleApply(); });
      el.addEventListener('focus', function () { rlEqSelectRow(tr); });
    });
    tr.addEventListener('click', function () { rlEqSelectRow(tr); });
    tr.addEventListener('focus', function () { rlEqSelectRow(tr); });
    // 键盘：↑↓ 增益 0.1 dB（Shift 1 dB）/ ←→ 频率 ±1%（Shift ±5%）
    tr.addEventListener('keydown', function (ev) {
      var k = ev.key, g = tr.querySelector('.rlEqG'), f = tr.querySelector('.rlEqF');
      var fast = ev.shiftKey;
      if (k === 'ArrowUp' || k === 'ArrowDown') {
        ev.preventDefault();
        var d = (k === 'ArrowUp' ? 1 : -1) * (fast ? 1 : 0.1);
        g.value = (Math.max(-24, Math.min(24, (parseFloat(g.value) || 0) + d))).toFixed(1);
      } else if (k === 'ArrowLeft' || k === 'ArrowRight') {
        if (ev.target !== tr && ev.target.tagName === 'INPUT' && ev.target.type === 'text') return; // 文本输入框里让光标移动
        ev.preventDefault();
        var mul = (k === 'ArrowRight' ? 1 : -1) * (fast ? 0.05 : 0.01);
        var fv = rlEqParseFreq(f.value) || 1000;
        f.value = rlEqFreqText(rlEqClampFreq(fv * (1 + mul)));
      } else return;
      ev.stopPropagation();
      rlEqSyncNodes();
      rlEqScheduleApply(100);
    });
  });
  rlEqSyncNodes();
  RL_EQ.rendering = false;
}
// 自动前级（防削波）：把前级设为 -ceil(曲线峰值)。曲线峰值优先取后端点集（架式/叠加后的真实峰值），
// 没有点集时退回"各段增益最大值"。
// 前端本地 RBJ 幅频（与 src/dsp/peq.h 同一组公式）。拖动时先本地画，后端返回后以服务端点集为准。
function rlEqBandDb(b,fs,freq){
 if(!b||b.on===false||!Number.isFinite(Number(b.gain))||Math.abs(Number(b.gain))<1e-9)return 0;
 var f0=Math.min(Math.max(Number(b.freq)||1000,10),0.49*fs),q=Math.min(Math.max(Number(b.q)||1.41,.1),20),g=Math.min(Math.max(Number(b.gain),-24),24);
 var A=Math.pow(10,g/40),w0=2*Math.PI*f0/fs,cw=Math.cos(w0),sw=Math.sin(w0),alpha=sw/(2*q);
 var b0,b1,b2,a0,a1,a2;
 if(b.type==='LS'){var tsa=2*Math.sqrt(A)*alpha;b0=A*((A+1)-(A-1)*cw+tsa);b1=2*A*((A-1)-(A+1)*cw);b2=A*((A+1)-(A-1)*cw-tsa);a0=(A+1)+(A-1)*cw+tsa;a1=-2*((A-1)+(A+1)*cw);a2=(A+1)+(A-1)*cw-tsa;}
 else if(b.type==='HS'){var tsh=2*Math.sqrt(A)*alpha;b0=A*((A+1)+(A-1)*cw+tsh);b1=-2*A*((A-1)+(A+1)*cw);b2=A*((A+1)+(A-1)*cw-tsh);a0=(A+1)-(A-1)*cw+tsh;a1=2*((A-1)-(A+1)*cw);a2=(A+1)-(A-1)*cw-tsh;}
 else {b0=1+alpha*A;b1=-2*cw;b2=1-alpha*A;a0=1+alpha/A;a1=-2*cw;a2=1-alpha/A;}
 var inv=Math.abs(a0)>1e-300?1/a0:0;b0*=inv;b1*=inv;b2*=inv;a1*=inv;a2*=inv;
 var fn=Math.min(Math.max(freq/(fs*0.5),0),1),w=Math.PI*fn,z1r=Math.cos(-w),z1i=Math.sin(-w),z2r=Math.cos(-2*w),z2i=Math.sin(-2*w);
 var nr=b0+b1*z1r+b2*z2r,ni=b1*z1i+b2*z2i,dr=1+a1*z1r+a2*z2r,di=a1*z1i+a2*z2i;
 return 20*Math.log10((Math.hypot(nr,ni)+1e-300)/(Math.hypot(dr,di)+1e-300));
}
function rlEqLocalPoints(value,fs){
 fs=Number(fs)||Number(RL_EQ_STATE&&RL_EQ_STATE.fs)||48000;
 var out=[];if(!value||Number(value.bypass)){for(var i=0;i<160;i++)out.push([20*Math.pow(1000,i/159),0]);return out;}
 var pb=Number(value.preamp)||0;
 for(var k=0;k<160;k++){var f=20*Math.pow(1000,k/159),db=pb;(value.bands||[]).forEach(function(b){db+=rlEqBandDb(b,fs,f);});out.push([f,db]);}
 return out;
}
function rlEqBandsParamFrom(value){return (value&&value.bands||[]).map(function(b,i){return (i+1)+','+(b.type||'PK')+','+(+b.freq)+','+(+b.gain)+','+(+b.q)+(b.on===false||b.on===0?' ,off':'');}).join(';').replace(/ ,/g,',');}
function rlEqPreviewLocal(){try{var v=rlEqRead(),pts=rlEqLocalPoints(v);if(RL_EQ_STATE)RL_EQ_STATE.points=pts;rlEqDrawCurve(pts);}catch(e){}}
function rlEqAutoPreamp(){
 var value;try{value=rlEqRead();}catch(e){rlEqMsg(e.message,true);return Promise.resolve();}
 var probe=JSON.parse(JSON.stringify(value));probe.preamp=0;var peak=0;rlEqLocalPoints(probe).forEach(function(p){peak=Math.max(peak,p[1]);});
 if(peak>23.5){rlEqMsg('增益峰值超过前级范围，请降低各段增益',true);return Promise.resolve();}
 value.preamp=-Math.max(0,Math.ceil(peak+.5));D('rlEqPreamp').value=value.preamp;RL_EQ.editVersion++;
 return rlEqCommit(value,true).then(function(){rlEqMsg(tf('已按曲线峰值 {0} dB 设置前级：{1} dB',[peak.toFixed(1),value.preamp]));}).catch(function(e){rlEqMsg(e.message,true);});
}
var RL_EQ={bank:[],selected:'1',source:'custom',slot:'',revision:'',editing:false,rendering:false,editVersion:0,history:{},read:null,index:null,indexRead:null,searchVersion:0,product:'',presets:[],loadedId:'',opraPreview:null,saveIntent:'custom',bankTimer:0,rename:null};
function rlEqConsoleHTML(){return '<div class=rlEqConsole>'
 +'<section class="rlEqFold" id=rlEqMyFold data-open=1><button type=button class=rlEqFoldHead id=rlEqMyToggle aria-expanded=true><span>我的均衡</span><small>16 组独立保存 · 点击卡片立即载入</small><b>收起</b></button><div class=rlEqFoldBody id=rlEqMyBody>'
 +'<div class=rlEqBankGrid id=rlEqBankGrid></div>'
 +'<div class=rlEqBankTools><span class=rlEqBankHint>双击卡片标题可改名；保存时再选择栏位和名称。</span><button class=btn type=button id=rlEqDelete>删除选中位置</button></div>'
 +'<p class=rlEqMessage id=rlEqBankMsg role=status aria-live=polite></p>'
 +'<p class=digitalFootnote>点击任意卡片立即载入并显示曲线；空位可作为保存目标。耳机 EQ 与自定义 EQ 在「播放台」的快速入口里互斥。</p></div></section>'
 +'<section class="rlEqFold" id=rlEqOpraFold data-open=0><button type=button class=rlEqFoldHead id=rlEqOpraToggle aria-expanded=false><span>耳机校准 · OPRA</span><small>先试听，确定后再保存到我的均衡</small><b>展开</b></button><div class=rlEqFoldBody id=rlEqOpraBody hidden>'
 +'<p class=digitalFootnote>搜索品牌或型号，点一条校准会先载入当前均衡并画出曲线；确定后再保存到选中的空位。<a href="https://opra.roon.app/" target=_blank rel="noopener noreferrer">查看来源</a></p>'
 +'<input id=rlEqOpraSearch type=search autocomplete=off placeholder="搜索耳机品牌或型号"><p class=rlEqMessage id=rlEqOpraMessage role=status></p><div class=rlEqOpraResults id=rlEqOpraResults></div><div class=rlEqOpraPresets id=rlEqOpraPresets></div>'
 +'</div></section>'
 +'<section class="rlEqFold rlEqMainFold" id=rlEqMainFold data-open=1><button type=button class=rlEqFoldHead id=rlEqMainToggle aria-expanded=true><span>20 段参数均衡</span><small>独立均衡 · 不绑定 A–D 组</small><b>收起</b></button><div class=rlEqFoldBody id=rlEqMainBody>'
 +'<div class=rlEqToolbar><label class=digitalCheck><input id=rlEqEnabled type=checkbox>启用均衡</label><label>前级增益 · dB <input id=rlEqPreamp type=number min=-24 max=24 step=.1></label><div class=rlEqToolbarBtns><button class="btn pri" type=button id=rlEqSave>保存均衡</button><button class=btn type=button id=rlEqAutoPre title="按当前曲线峰值自动降低前级，给提升留出余量，避免数字削波">按峰值防削波</button><button class=btn type=button id=rlEqUndo>撤销</button><button class=btn type=button id=rlEqRestore>一键复原</button></div></div>'
 +'<p class=rlEqStatus id=rlEqStat></p><div class=rlEqcv id=rlEqCurve aria-label="均衡响应与可拖动频段"></div><div class=rlEqrows id=rlEqRows></div><p class=rlEqMessage id=rlEqMsg role=status aria-live=polite></p></div></section>'
 +'<dialog class=rlEqDialog id=rlEqSaveDialog aria-labelledby=rlEqSaveDialogTitle><form method=dialog class=rlEqDialogCard><header><div><h2 id=rlEqSaveDialogTitle>保存均衡</h2><p id=rlEqSaveDialogNote></p></div><button class=rlEqDialogClose value=cancel aria-label=关闭>×</button></header><label class=rlEqDialogField>保存到<select id=rlEqSaveSlot></select></label><label class=rlEqDialogField>名称<input id=rlEqSaveName maxlength=64 autocomplete=off placeholder="给这组均衡起个名字"></label><p class=rlEqMessage id=rlEqSaveDialogMsg role=status></p><footer><button class=btn value=cancel>取消</button><button class="btn pri" type=button id=rlEqSaveCommit>保存</button></footer></form></dialog></div>';}
function rlEqMsg(message,error){var e=D('rlEqMsg');if(e){e.textContent=t(message||'');e.dataset.error=String(!!error);}}
function rlEqSnapshot(value){return JSON.parse(JSON.stringify({bypass:Number(value.bypass)||0,preamp:Number(value.preamp)||0,bands:(value.bands||[]).map(function(b,i){return {i:i+1,on:b.on===true||b.on===1||b.on==='1',type:b.type,freq:Number(b.freq),gain:Number(b.gain),q:Number(b.q)};})}));}
function rlEqStatusRender(j){var on=!(j.bypass===true||Number(j.bypass)===1),active=(j.bands||[]).filter(function(b){return b.on&&Number(b.gain)!==0;}).length;if(!RL_EQ.loadedId&&!RL_EQ.opraPreview){rlSetText('rlEqStat',t('未载入均衡：先在「我的均衡」或「耳机校准」载入一组，启用后才会在播放台生效'));return;}rlSetText('rlEqStat',on?tf('均衡已启用 · {0} / 20 段改变增益 · 前级 {1} dB',[active,Number(j.preamp||0).toFixed(1)]):t('均衡已关闭'));}
function rlEqRender(j){if(!j||!D('rlEqEnabled'))return;j=JSON.parse(JSON.stringify(j));while((j.bands||[]).length<20){var i=j.bands.length;j.bands.push({i:i+1,on:false,type:'PK',freq:20*Math.pow(1000,i/19),gain:0,q:1.41});}RL_EQ_STATE=j;D('rlEqEnabled').checked=!(j.bypass===true||Number(j.bypass)===1);D('rlEqPreamp').value=j.preamp||0;rlEqRenderRows(j);rlEqDrawCurve(j.points||[]);rlEqStatusRender(j);}
function rlEqSync(state,busy){var panel=D('rlStudioPanel_eq');if(panel)panel.inert=!!RL_STUDIO.switching;if(!D('rlEqEnabled')||!state||!state.peq)return;var id=rlCompare.activeId||'A',signature=JSON.stringify(state.peq);
 // 2026-11-12：删掉「当前组 A」标签。EQ 是独立工作区；A–D 只负责载入已保存的均衡。
 if(RL_EQ.slot!==id){RL_EQ.slot=id;RL_EQ.editing=false;RL_EQ.editVersion++;clearTimeout(RL_EQ_TIMER);RL_EQ.revision='';RL_EQ.source='custom';RL_EQ.loadedId='';}
 var activeSlot=rlCompare.slots.find(function(s){return s.id===id;}),activeEq=activeSlot&&activeSlot.eqId||'';
 if(activeEq)RL_EQ.loadedId=String(activeEq);else if(RL_EQ.loadedId!=='working'&&!RL_EQ.opraPreview)RL_EQ.loadedId='';
 if(!RL_EQ.editing&&signature!==RL_EQ.revision){RL_EQ.revision=signature;rlEqRender(state.peq);}
 var eqReady=!!(RL_EQ.loadedId||RL_EQ.opraPreview);
 if(!eqReady)D('rlEqEnabled').checked=false;
 D('rlEqUndo').disabled=!!busy||!(RL_EQ.history[id]||[]).length;D('rlEqEnabled').disabled=!!busy||!eqReady;D('rlEqEnabled').title=eqReady?t('只作用于已载入的均衡；切到播放台后仍按已载入的均衡生效'):t('请先载入一组均衡，启用才会生效');['rlEqSave','rlEqDelete'].forEach(function(key){if(D(key))D(key).disabled=!!busy;});
}
function rlEqRead(){var bands=[];Array.from(rlEqRowsEl()).forEach(function(row){var freq=rlEqParseFreq(row.querySelector('.rlEqF').value),gain=Number(row.querySelector('.rlEqG').value),q=Number(row.querySelector('.rlEqQ').value);if(!Number.isFinite(freq)||freq<10||freq>20000||!Number.isFinite(gain)||gain<-24||gain>24||!Number.isFinite(q)||q<.1||q>20)throw Error(t('请检查频率、增益和 Q 的有效范围'));bands.push({i:Number(row.dataset.i),on:row.querySelector('.rlEqOn').checked,type:row.querySelector('.rlEqT').value,freq:freq,gain:gain,q:q});});var preamp=Number(D('rlEqPreamp').value);if(!Number.isFinite(preamp)||preamp<-24||preamp>24)throw Error(t('前级增益须在 −24 至 24 dB 之间'));if(bands.length!==20)throw Error(t('当前均衡未完整读取，请恢复后重试'));return {bypass:D('rlEqEnabled').checked?0:1,preamp:preamp,bands:bands};}
var RL_EQ_WRITE={busy:false,pending:null,tail:Promise.resolve()},RL_EQ_REFRESH_TIMER=0;
function rlEqScheduleApply(ms){if(RL_STUDIO.switching||RL_EQ.rendering)return;
 // 手动改频段即形成“工作副本”：它也是一组已载入当前 PEQ 的均衡，切到播放台会照常生效；
 // 只有完全平直、从未编辑/载入的状态才不允许用「启用均衡」凭空开一个空 EQ。
 if(!RL_EQ.loadedId&&!RL_EQ.opraPreview){RL_EQ.loadedId='working';if(D('rlEqEnabled'))D('rlEqEnabled').checked=true;}
 // 先认领当前组：否则 rlStudioSync 的下一次 rlEqSync 会把刚启动的防抖计时器当成“旧组残留”清掉，
 // 表现为第一次改 EQ 后一直停在「编辑中」，请求永远不发。
 RL_EQ.slot=rlCompare.activeId||'A';
 RL_EQ.editing=true;RL_EQ.editVersion++;rlEqPreviewLocal();clearTimeout(RL_EQ_TIMER);RL_EQ_TIMER=setTimeout(function(){rlEqApply().catch(function(){});},ms===undefined?100:ms);}
function rlEqApply(){clearTimeout(RL_EQ_TIMER);var value;try{value=rlEqRead();}catch(e){rlEqMsg(e.message,true);return Promise.reject(e);}if(RL_EQ_WRITE.busy){RL_EQ_WRITE.pending=value;return Promise.resolve(value);}return rlEqWriteNext(value);}
function rlEqWriteNext(value){
 RL_EQ_WRITE.busy=true;var p=rlEqCommit(value,true);RL_EQ_WRITE.tail=p.catch(function(){});
 function done(){RL_EQ_WRITE.busy=false;if(RL_EQ_WRITE.pending){var v=RL_EQ_WRITE.pending;RL_EQ_WRITE.pending=null;rlEqWriteNext(v);}}
 p.then(done,done);return p;
}
// 2026-11-12：EQ 编辑改走单次 /api/peq（不再走 workbench profile 的 5 次读改写链）。
// /api/peq 热更新 PEQ 并把当前 PEQ 同步进当前组，但不重建整条音频管线 ⟹ 拖动才跟得上手。
function rlEqCommit(value,remember){
 var id=rlCompare.activeId,version=RL_EQ.editVersion,previous=rlConsole.state&&rlConsole.state.peq?rlEqSnapshot(rlConsole.state.peq):null;
 RL_EQ.editing=true;rlEqMsg('应用中…');
 var body='bypass='+(Number(value.bypass)?1:0)+'&preamp='+(Number(value.preamp)||0)+'&bands='+encodeURIComponent(rlEqBandsParamFrom(value));
 function request(){var call=(typeof apiRaw==='function')?apiRaw:(typeof apiAct==='function'?apiAct:api);return call('/api/peq',body).then(function(j){if(!j||!j.ok)throw Error(j&&j.error||t('均衡应用失败'));return j;});}
 return request().then(function(j){
  if(remember&&previous){var history=RL_EQ.history[id]||(RL_EQ.history[id]=[]);history.push(previous);if(history.length>32)history.shift();}
  // 真实后端返回完整 peq_state；测试替身可能只回 {ok:true}，此时用刚提交的值补齐，避免界面/组状态断档。
  var applied=(j&&Array.isArray(j.bands))?j:{bypass:Number(value.bypass)?1:0,preamp:Number(value.preamp)||0,bands:value.bands,points:rlEqLocalPoints(value)};
  RL_EQ_STATE=applied;if(rlConsole.state)rlConsole.state.peq=applied;
  // 让 A–D 卡片/对比逻辑立即看到当前组的实时 PEQ；权威刷新在 1.2s 后仍会覆盖。
  if(typeof rlCompare!=='undefined'){var slot=rlCompare.slots.find(function(s){return s.id===rlCompare.activeId;});if(slot){slot.peq=applied;slot.eqId='';slot.eqName='';}}
  if(version===RL_EQ.editVersion){RL_EQ.editing=false;RL_EQ.source='custom';RL_EQ.revision='';rlEqDrawCurve(applied.points||[]);rlEqStatusRender(applied);rlEqMsg('均衡已应用，可继续调整');}
  clearTimeout(RL_EQ_REFRESH_TIMER);RL_EQ_REFRESH_TIMER=setTimeout(function(){if(typeof rlConsole!=='undefined')rlConsole.refresh(true).catch(function(){});if(typeof rlCompare!=='undefined')rlCompare.refresh(true).catch(function(){});},1200);
  return j;
 }).catch(function(e){
  if(version===RL_EQ.editVersion){RL_EQ.editing=false;RL_EQ.revision='';if(rlConsole.state&&rlConsole.state.peq)rlEqRender(rlConsole.state.peq);}
  rlEqMsg(e.message,true);throw e;
 });
}
function rlEqFlush(){
 if(RL_EQ_WRITE.pending){var v=RL_EQ_WRITE.pending;RL_EQ_WRITE.pending=null;return RL_EQ_WRITE.busy?RL_EQ_WRITE.tail.then(function(){return rlEqWriteNext(v);}):rlEqWriteNext(v);}
 return RL_EQ_WRITE.busy?RL_EQ_WRITE.tail:Promise.resolve();
}
function rlEqUndo(){var history=RL_EQ.history[rlCompare.activeId]||[];if(!history.length)return Promise.resolve();var value=history.pop();RL_EQ.editVersion++;return rlEqCommit(value,false).catch(function(e){history.push(value);throw e;});}
function rlEqRestore(){
 clearTimeout(RL_EQ_TIMER);RL_EQ.loadedId='';RL_EQ.opraPreview=null;
 var bands=rlEqCollectBands().map(function(b){return {i:b.i,on:true,type:'PK',freq:b.freq,gain:0,q:1.41};});
 var value={bypass:1,preamp:0,bands:bands};
 RL_EQ.editVersion++;
 return rlEqCommit(value,true).then(function(){
  // 复原必须把界面也拉回平直：表格与曲线都重画，避免"声音复原了、界面还亮着旧增益"。
  RL_EQ_STATE=Object.assign({},RL_EQ_STATE||{},value,{points:rlEqLocalPoints({bypass:1,preamp:0,bands:bands})});
  rlEqRender(RL_EQ_STATE);rlEqMsg('已一键复原：全部频段 0 dB、前级 0 dB、均衡关闭');
 }).catch(function(e){rlEqMsg(e.message,true);});
}
function rlEqLoadBank(){return apiGetRaw('/api/workbench/eqpresets').then(function(j){if(!j||!j.ok||!Array.isArray(j.slots))throw Error(j&&j.error||t('无法读取我的均衡'));RL_EQ.bank=j.slots;rlEqPaintBank();if(typeof rlEqPaintOpraSlots==='function')rlEqPaintOpraSlots();rlStudioQuickEq();return j;});}
function rlEqPaintBank(){
 var grid='',activeSlot=rlCompare.slots.find(function(s){return s.id===rlCompare.activeId;}),activeEq=activeSlot&&activeSlot.eqId||'';
 for(var i=1;i<=16;i++){
  var id=String(i),slot=RL_EQ.bank.find(function(s){return s.id===id;}),has=!!(slot&&slot.peq),name=has?slot.name:t('空位');
  var active=has&&activeEq===id;
  grid+='<button type=button class=rlEqBankSlot data-eq-slot='+id+' aria-pressed='+(id===RL_EQ.selected)+' data-active='+(active?'true':'false')+' title="'+esc(has?t('点击载入；双击标题改名'):t('空位：保存时可选这个位置'))+'"><b data-eq-name="'+id+'">'+esc(String(i).padStart(2,'0')+' · '+name)+'</b><span>'+esc(has?t(slot.source==='opra'?'耳机校准':'自定义均衡'):t('空位'))+'</span><i>'+esc(active?t('已载入'):has?t('点击载入'):t('可保存到此位'))+'</i></button>';
 }
 var host=D('rlEqBankGrid');if(host&&!RL_EQ.rename)host.innerHTML=grid;
 var selected=RL_EQ.bank.find(function(s){return s.id===RL_EQ.selected;});
 if(!selected){selected=RL_EQ.bank.find(function(s){return !s.peq;})||RL_EQ.bank[0];RL_EQ.selected=selected&&selected.id||'1';}
 if(D('rlEqDelete'))D('rlEqDelete').disabled=!(selected&&selected.peq);
}
function rlEqBankSelect(id,load){RL_EQ.selected=String(id);rlEqPaintBank();if(load!==false){var slot=RL_EQ.bank.find(function(s){return s.id===String(id);});if(slot&&slot.peq)rlEqPresetLoad(id);}}
function rlEqBankAction(action,id,extra){
 var slotId=rlCompare.activeId,body=rlCompare.form(Object.assign({action:action,id:id,slotId:slotId},extra||{}));
 return rlEqFlush().then(function(){return rlStudioRun(function(){
  if(slotId!==rlCompare.activeId)throw Error(t('当前组已切换，请重新操作'));
  return apiAct('/api/workbench/eqpresets',body.toString()).then(function(j){
   if(!j||!j.ok)throw Error(j&&j.error||t('均衡操作失败'));
   return Promise.all([rlEqLoadBank(),rlCompare.refresh(true),rlConsole.refresh(true)]).then(function(){return loadSettings();}).then(function(){RL_EQ.editing=false;RL_EQ.revision='';RL_EQ.selected=String(id);if(action==='load'||action==='save')RL_EQ.loadedId=String(id);if(action==='delete'&&RL_EQ.loadedId===String(id))RL_EQ.loadedId='';rlEqSync(rlConsole.state,false);return j;});
  });
 },action==='save'?'均衡已保存':action==='delete'?'均衡已删除':action==='rename'?'均衡已改名':'均衡已载入');}).catch(function(e){rlEqMsg(e.message,true);throw e;});
}
function rlEqPresetLoad(id){RL_EQ.selected=String(id);rlEqPaintBank();return rlEqBankAction('load',String(id)).catch(function(){});}
function rlEqSaveSlots(selectId,current){
 var html='';for(var i=1;i<=16;i++){var id=String(i),slot=RL_EQ.bank.find(function(s){return s.id===id;}),label=String(i).padStart(2,'0')+' · '+(slot&&slot.peq?slot.name:t('空位'));html+='<option value="'+id+'">'+esc(label)+'</option>';}
 var sel=D(selectId);if(!sel)return;sel.innerHTML=html;sel.value=String(current||(RL_EQ.bank.find(function(s){return !s.peq;})||{id:'1'}).id);
}
function rlEqOpenDialog(intent,note){
 var dlg=D('rlEqSaveDialog');if(!dlg)return;
 RL_EQ.saveIntent=intent;
 var title=D('rlEqSaveDialogTitle'),noteNode=D('rlEqSaveDialogNote');
 if(title)title.textContent=intent==='opra'?t('保存耳机校准'):t('保存均衡');
 if(noteNode)noteNode.textContent=note||'';
 D('rlEqSaveDialogMsg').textContent='';
 rlEqSaveSlots('rlEqSaveSlot',intent==='opra'?(RL_EQ.bank.find(function(s){return !s.peq;})||{id:RL_EQ.selected}).id:RL_EQ.selected);
 var name=D('rlEqSaveName');
 if(name)name.value=intent==='opra'&&RL_EQ.opraPreview?RL_EQ.opraPreview.name:(RL_EQ.bank.find(function(s){return s.id===RL_EQ.selected;})||{}).name||'';
 if(typeof dlg.showModal==='function')dlg.showModal();else dlg.setAttribute('open','');
 setTimeout(function(){if(name){name.focus();name.select();}},30);
}
function rlEqOpenSave(){
 if(!RL_EQ.loadedId&&!RL_EQ.opraPreview){rlEqMsg('请先载入一组均衡，或从耳机校准试听一条。',true);return;}
 var note=RL_EQ.opraPreview?tf('正在试听：{0}',[RL_EQ.opraPreview.name]):'';
 rlEqOpenDialog(RL_EQ.opraPreview?'opra':'custom',note);
}
function rlEqSaveDialogCommit(){
 var slot=String((D('rlEqSaveSlot')||{}).value||''),name=((D('rlEqSaveName')||{}).value||'').trim();
 if(!slot){rlEqMsg('请选择保存位置。',true);return;}
 if(!name){D('rlEqSaveDialogMsg').textContent=t('请填写名称。');D('rlEqSaveName').focus();return;}
 var dlg=D('rlEqSaveDialog');
 if(RL_EQ.saveIntent==='opra'){rlEqOpraSaveNow(slot,name).then(function(ok){if(ok&&dlg&&dlg.open)dlg.close();});return;}
 var value;try{value=rlEqRead();}catch(e){rlEqMsg(e.message,true);return;}
 RL_EQ.selected=slot;
 rlEqBankAction('save',slot,{name:name,peq:value,source:RL_EQ.source}).then(function(){if(dlg&&dlg.open)dlg.close();}).catch(function(){});
}
function rlEqRenameSlot(id){
 var slot=RL_EQ.bank.find(function(s){return s.id===String(id);});if(!slot||!slot.peq)return;
 var card=document.querySelector('[data-eq-slot="'+id+'"]'),title=card&&card.querySelector('[data-eq-name]');if(!title)return;
 RL_EQ.rename={id:String(id)};
 var input=document.createElement('input');input.className='rlEqRenameInput';input.maxLength=64;input.value=slot.name;input.setAttribute('aria-label','均衡名称');title.replaceWith(input);input.focus();input.select();
 var done=false;
 function finish(save){if(done)return;done=true;var name=input.value.trim();if(input.parentNode){var b=document.createElement('b');b.dataset.eqName=String(id);b.textContent=String(id).padStart(2,'0')+' · '+(name||slot.name);input.replaceWith(b);}RL_EQ.rename=null;rlEqPaintBank();if(save&&name&&name!==slot.name)rlEqBankAction('rename',String(id),{name:name}).catch(function(){});}
 input.addEventListener('keydown',function(e){if(e.key==='Enter'){e.preventDefault();finish(true);}else if(e.key==='Escape'){e.preventDefault();finish(false);}});
 // 延迟挂 blur：创建当帧的焦点切换/测试环境的失焦不能把输入框立刻收掉。
 setTimeout(function(){if(input.parentNode)input.addEventListener('blur',function(){finish(true);});},0);
}
function rlEqRefresh(){if(RL_EQ.read)return RL_EQ.read;RL_EQ.read=Promise.all([rlEqLoadBank(),rlConsole.refresh()]).then(function(){rlEqSync(rlConsole.state,false);if(RL_EQ_PTS)rlEqDrawCurve(RL_EQ_PTS);}).catch(function(e){rlEqMsg(e.message,true);}).finally(function(){RL_EQ.read=null;});return RL_EQ.read;}
function rlEqFoldSet(which,open){
 var body=D(which==='opra'?'rlEqOpraBody':which==='main'?'rlEqMainBody':'rlEqMyBody'),head=D(which==='opra'?'rlEqOpraToggle':which==='main'?'rlEqMainToggle':'rlEqMyToggle');
 if(body)body.hidden=!open;if(head){head.setAttribute('aria-expanded',String(open));var b=head.querySelector('b');if(b)b.textContent=t(open?'收起':'展开');}
 try{localStorage.setItem('rlplayer-eq-fold-'+which,open?'1':'0');}catch(e){}
}
function rlAttachEqConsole(){
 if(!D('rlEqEnabled'))return;
 D('rlEqEnabled').onchange=function(){rlEqScheduleApply(0);};
 D('rlEqPreamp').oninput=function(){rlEqScheduleApply();};
 D('rlEqAutoPre').onclick=rlEqAutoPreamp;
 D('rlEqUndo').onclick=function(){rlEqUndo().catch(function(){});};
 D('rlEqRestore').onclick=function(){rlEqRestore();};
 D('rlEqSave').onclick=rlEqOpenSave;D('rlEqSaveCommit').onclick=rlEqSaveDialogCommit;
 D('rlEqDelete').onclick=function(){var slot=RL_EQ.bank.find(function(s){return s.id===RL_EQ.selected;});if(!(slot&&slot.peq))return;function del(){rlEqBankAction('delete',slot.id).catch(function(){});}if(typeof askConfirm==='function')askConfirm({title:t('删除均衡'),ok:t('删除'),text:tf('删除已保存的均衡「{0}」？只删这个存储位，不影响当前声音。',[slot.name]),onOk:del});else del();};
 D('rlEqBankGrid').onclick=function(e){var b=e.target.closest('[data-eq-slot]');if(!b)return;clearTimeout(RL_EQ.bankTimer);RL_EQ.bankTimer=setTimeout(function(){rlEqBankSelect(b.dataset.eqSlot,true);},220);};
 D('rlEqBankGrid').addEventListener('dblclick',function(e){var name=e.target.closest('[data-eq-name]');if(!name)return;e.preventDefault();e.stopPropagation();clearTimeout(RL_EQ.bankTimer);RL_EQ.bankTimer=0;rlEqRenameSlot(name.dataset.eqName);});
 D('rlEqMyToggle').onclick=function(){rlEqFoldSet('my',D('rlEqMyBody').hidden);};
 D('rlEqOpraToggle').onclick=function(){rlEqFoldSet('opra',D('rlEqOpraBody').hidden);};
 D('rlEqMainToggle').onclick=function(){rlEqFoldSet('main',D('rlEqMainBody').hidden);};
 try{if(localStorage.getItem('rlplayer-eq-fold-my')==='0')rlEqFoldSet('my',false);if(localStorage.getItem('rlplayer-eq-fold-opra')==='1')rlEqFoldSet('opra',true);if(localStorage.getItem('rlplayer-eq-fold-main')==='0')rlEqFoldSet('main',false);}catch(e){}
 D('rlEqOpraSearch').oninput=function(){rlEqSearch(this.value);};
 rlEqPaintOpraSlots();
 rlEqLoadBank().catch(function(e){rlEqMsg(e.message,true);});
}
function rlEqOpraMessage(message,error){var e=D('rlEqOpraMessage');if(e){e.textContent=t(message||'');e.dataset.error=String(!!error);}}
function rlEqGetIndex(){if(RL_EQ.index)return Promise.resolve(RL_EQ.index);if(RL_EQ.indexRead)return RL_EQ.indexRead;RL_EQ.indexRead=apiGetRaw('/api/opra/index').then(function(j){if(!j||!j.ok||!Array.isArray(j.items))throw Error(j&&j.error||t('耳机数据库加载失败'));RL_EQ.index=j.items;return j.items;}).finally(function(){RL_EQ.indexRead=null;});return RL_EQ.indexRead;}
function rlEqSearch(query){var q=String(query||'').trim(),version=++RL_EQ.searchVersion;RL_EQ.product='';D('rlEqOpraPresets').innerHTML='';if(!q){D('rlEqOpraResults').innerHTML='';rlEqOpraMessage('输入品牌或型号开始查找');return Promise.resolve();}rlEqOpraMessage('正在查找耳机…');return rlEqGetIndex().then(function(index){if(version!==RL_EQ.searchVersion)return;var tokens=q.toLowerCase().split(/\s+/).filter(Boolean),all=index.filter(function(item){var text=[item[0],item[1],item[2]].join(' ').toLowerCase();return tokens.every(function(token){return text.indexOf(token)>=0;});}),hits=all.slice(0,30);rlEqOpraMessage(all.length?tf('找到 {0} 款耳机，显示 {1} 款',[all.length,hits.length]):t('未找到匹配的耳机型号'));D('rlEqOpraResults').innerHTML=hits.map(function(item){return '<button class=rlEqModel type=button data-product="'+esc(item[0])+'" aria-pressed=false>'+esc(item[1]+' · '+item[2])+'</button>';}).join('');D('rlEqOpraResults').onclick=function(e){var b=e.target.closest('[data-product]');if(!b)return;var item=hits.find(function(x){return x[0]===b.dataset.product;});if(item)rlEqProduct(item);};}).catch(function(e){if(version===RL_EQ.searchVersion)rlEqOpraMessage(e.message,true);});}
function rlEqOpraType(t){t=String(t||'').toLowerCase();if(t==='low_shelf'||t==='lowshelf'||t==='ls')return 'LS';if(t==='high_shelf'||t==='highshelf'||t==='hs')return 'HS';return 'PK';}
function rlEqPaintOpraSlots(){
 var sel=D('rlEqOpraSlot');if(!sel)return;
 var html='';for(var i=1;i<=16;i++){var id=String(i),slot=RL_EQ.bank.find(function(s){return s.id===id;}),label=String(i).padStart(2,'0')+' · '+(slot&&slot.peq?slot.name:t('空位'));html+='<option value="'+id+'">'+esc(label)+'</option>';}
 var old=sel.value;sel.innerHTML=html;
 var empty=RL_EQ.bank.find(function(s){return !s.peq;});
 sel.value=old||(empty&&empty.id)||'1';
}
function rlEqProduct(item){
 var product=item[0];RL_EQ.product=product;
 Array.from(D('rlEqOpraResults').querySelectorAll('[data-product]')).forEach(function(b){b.setAttribute('aria-pressed',String(b.dataset.product===product));});
 D('rlEqOpraPresets').innerHTML='';rlEqOpraMessage('正在读取校准方案…');
 return apiGetRaw('/api/opra/presets?productId='+encodeURIComponent(product)+'&full=1').then(function(j){
  if(RL_EQ.product!==product)return;
  if(!j||!j.ok||!Array.isArray(j.items))throw Error(j&&j.error||t('校准方案读取失败'));
  RL_EQ.presets=j.items;rlEqOpraMessage(j.items.length?tf('已读取 {0} 条真实校准方案；点「试听」先载入并看曲线',[j.items.length]):t('该型号暂无可用校准'));
  D('rlEqOpraPresets').innerHTML=j.items.map(function(p){return '<article class=rlEqOpraPreset><strong>'+esc(p.author||'OPRA')+'</strong><p>'+esc(p.details||p.id)+'</p><p>'+esc(tf('前级 {0} dB · {1} 段',[p.gainDb,p.bandsCount]))+'</p><div class=rlEqOpraActions><button class="btn pri" type=button data-opra-preset="'+esc(p.id)+'">试听</button><button class=btn type=button data-opra-save="'+esc(p.id)+'" disabled>保存</button></div></article>';}).join('');
  rlEqPaintOpraPreview();
  D('rlEqOpraPresets').onclick=function(e){var save=e.target.closest('[data-opra-save]');if(save){if(!RL_EQ.opraPreview||RL_EQ.opraPreview.presetId!==save.dataset.opraSave){rlEqOpraMessage('请先试听这条校准，再保存。',true);return;}rlEqOpenDialog('opra',tf('正在试听：{0}',[RL_EQ.opraPreview.name]));return;}var b=e.target.closest('[data-opra-preset]');if(b)rlEqPreviewOpra(product,b.dataset.opraPreset,item[1]+' '+item[2]);};
 }).catch(function(e){if(RL_EQ.product===product)rlEqOpraMessage(e.message,true);});
}
function rlEqPreviewOpra(product,presetId,productLabel){
 var p=(RL_EQ.presets||[]).find(function(x){return x.id===presetId;});
 if(!p||!Array.isArray(p.bands)){rlEqOpraMessage('这条校准没有可用的频段数据，请换一条。',true);return Promise.resolve();}
 if(p.bands.length>20){rlEqOpraMessage('这条校准超过 20 段，当前 PEQ 无法无损试听。',true);return Promise.resolve();}
 var bands=p.bands.map(function(b,i){return {i:i+1,on:true,type:rlEqOpraType(b.type),freq:Math.max(10,Math.min(20000,Number(b.frequency)||1000)),gain:Math.max(-24,Math.min(24,Number(b.gain)||0)),q:Math.max(.1,Math.min(20,Number(b.q)||.707))};});
 var value={bypass:0,preamp:Math.max(-24,Math.min(24,Number(p.gainDb)||0)),bands:bands};
 var name=(productLabel+' · '+(p.author||'OPRA')+(p.details?' · '+p.details:'')).slice(0,64);
 RL_EQ.opraPreview={product:product,presetId:presetId,name:name,value:value};
 RL_EQ.loadedId='';rlEqPaintOpraPreview();
 rlEqOpraMessage('正在试听：'+name);
 return rlEqCommit(value,false).then(function(){
  RL_EQ.source='opra';rlEqPaintOpraPreview();rlEqSync(rlConsole.state,false);
  rlEqOpraMessage('试听已载入。满意后点这条校准旁的「保存」，再选择栏位和名称。');
 }).catch(function(e){rlEqOpraMessage(e.message,true);});
}
function rlEqPaintOpraPreview(){
 var pv=RL_EQ.opraPreview;
 document.querySelectorAll('[data-opra-preset]').forEach(function(b){var on=!!(pv&&b.dataset.opraPreset===pv.presetId);b.textContent=t(on?'试听中':'试听');b.setAttribute('aria-pressed',String(on));});
 document.querySelectorAll('[data-opra-save]').forEach(function(b){b.disabled=!(pv&&b.dataset.opraSave===pv.presetId);});
 document.querySelectorAll('.rlEqOpraPreset').forEach(function(card){var b=card.querySelector('[data-opra-preset]');card.dataset.preview=String(!!(pv&&b&&b.dataset.opraPreset===pv.presetId));});
}
function rlEqOpraSaveNow(slot,name){
 var pv=RL_EQ.opraPreview;if(!pv){rlEqOpraMessage('请先试听一条校准。',true);return Promise.resolve(false);}
 slot=String(slot||(D('rlEqSaveSlot')||{}).value||'');name=String(name||(D('rlEqSaveName')||{}).value||'').trim();var id=rlCompare.activeId;
 if(!slot){rlEqOpraMessage('请选择保存位置。',true);return Promise.resolve(false);}
 if(!name){rlEqOpraMessage('请填写名称。',true);return Promise.resolve(false);}
 return rlEqFlush().then(function(){return rlStudioRun(function(){
  if(id!==rlCompare.activeId)throw Error(t('当前组已切换，请重新操作'));
  var body=rlCompare.form({action:'import',id:slot,slotId:id,productId:pv.product,presetId:pv.presetId,name:name});
  return apiAct('/api/workbench/eqpresets',body.toString()).then(function(j){
   if(!j||!j.ok)throw Error(j&&j.error||t('耳机校准保存失败'));
   return Promise.all([rlEqLoadBank(),rlCompare.refresh(true),rlConsole.refresh(true)]).then(function(){return loadSettings();}).then(function(){
    RL_EQ.source='opra';RL_EQ.selected=slot;RL_EQ.loadedId=slot;RL_EQ.revision='';RL_EQ.editing=false;RL_EQ.opraPreview=null;rlEqPaintBank();rlEqPaintOpraPreview();rlEqSync(rlConsole.state,false);rlEqOpraMessage(tf('已保存到我的均衡位置 {0}（{1}）',[slot,name]));return true;
   });
  });
 },'耳机校准已保存');}).catch(function(e){rlEqOpraMessage(e.message,true);return false;});
}
function rlEqImport(product,preset,name){var id=rlCompare.activeId,empty=RL_EQ.bank.find(function(s){return !s.peq;});if(!empty){rlEqOpraMessage('16 个位置已满，请先删除一组均衡再导入',true);return Promise.resolve();}return rlEqFlush().then(function(){return rlStudioRun(function(){if(id!==rlCompare.activeId)throw Error(t('当前组已切换，请重新操作'));rlEqOpraMessage('正在导入耳机校准…');var body=rlCompare.form({action:'import',id:empty.id,slotId:id,productId:product,presetId:preset,name:(name+' · '+((RL_EQ.presets.find(function(p){return p.id===preset;})||{}).author||'OPRA')).slice(0,64)});return apiAct('/api/workbench/eqpresets',body.toString()).then(function(j){if(!j||!j.ok)throw Error(j&&j.error||t('耳机校准导入失败'));return Promise.all([rlEqLoadBank(),rlCompare.refresh(true),rlConsole.refresh(true)]).then(function(){return loadSettings();}).then(function(){RL_EQ.source='opra';RL_EQ.selected=empty.id;RL_EQ.revision='';RL_EQ.editing=false;rlEqPaintBank();rlEqSync(rlConsole.state,false);rlEqOpraMessage(tf('已导入当前组 {0}，保存于位置 {1}',[id,empty.id]));return j;});});},'耳机校准已导入当前组');}).catch(function(e){rlEqOpraMessage(e.message,true);});}
)EQCONSOLE";}
} // namespace rta::net
