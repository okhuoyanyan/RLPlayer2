# -*- coding: utf-8 -*-
"""RLPlayer 管理后台页面（macOS 风格，2026-11-09 重做）。

约束：
- 无外部资源（CDN/字体/图标），全内联，符合生产 CSP 'self'。
- 默认只显示打码 IP；揭示完整 IP 走 /admin/api/reveal-ip 并写审计。
- 登录只收用户名 + 密码。
- Windows / macOS 分开：存储配额、外部点击、反馈都必须能按平台区分。
"""

LOGIN_PAGE = r"""<!doctype html>
<html lang="zh-CN"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width">
<title>登录 · RLPlayer 管理后台</title>
<style>
*{box-sizing:border-box}
body{margin:0;min-height:100vh;font:13px/1.5 -apple-system,BlinkMacSystemFont,"SF Pro Text","PingFang SC","Microsoft YaHei",sans-serif;
 background:radial-gradient(1200px 600px at 20% 0%,#e8f0ff 0%,#f5f5f7 45%,#f5f5f7 100%);color:#1d1d1f;display:grid;place-items:center}
.wrap{width:100%;max-width:360px;padding:24px}
.brand{text-align:center;margin-bottom:20px}
.brand b{display:block;font-size:26px;font-weight:700;letter-spacing:-.3px}
.brand span{color:#6e6e73;font-size:12px}
.card{background:rgba(255,255,255,.86);backdrop-filter:saturate(180%) blur(20px);-webkit-backdrop-filter:saturate(180%) blur(20px);
 border:1px solid rgba(0,0,0,.06);border-radius:16px;box-shadow:0 12px 40px rgba(0,0,0,.08);padding:26px 24px}
.card h1{margin:0 0 18px;font-size:17px;font-weight:600;text-align:center}
label{display:block;margin:0 0 5px;font-weight:500;font-size:12px;color:#6e6e73}
input[type=text],input[type=password]{width:100%;padding:9px 11px;border:1px solid #d2d2d7;border-radius:10px;font:inherit;background:#fff;color:#1d1d1f}
input:focus{outline:0;border-color:#0071e3;box-shadow:0 0 0 3px rgba(0,113,227,.16)}
.field{margin-bottom:15px}
.btn{width:100%;padding:10px 12px;border:0;border-radius:10px;background:#0071e3;color:#fff;font:inherit;font-weight:600;cursor:pointer}
.btn:hover{background:#0062c4}
.err{background:#fdecec;border-radius:10px;padding:9px 12px;margin:0 0 15px;color:#c62828;font-size:12px}
.foot{margin-top:16px;text-align:center;color:#86868b;font-size:12px}
</style></head>
<body><div class="wrap">
<div class="brand"><b>RLPlayer</b><span>管理后台 · 更新与反馈服务</span></div>
<div class="card">
<h1>登录</h1>
<form method="post" action="/admin/login">
<input type="hidden" name="csrf" value="__CSRF__">
__ERROR__
<div class="field"><label for="u">用户名</label>
<input id="u" name="username" type="text" required autocomplete="username" autofocus></div>
<div class="field"><label for="p">密码</label>
<input id="p" name="password" type="password" required autocomplete="current-password"></div>
<button class="btn" type="submit">登录</button>
</form>
</div>
<div class="foot">仅限管理员使用 · 连续失败会被限速</div>
</div></body></html>"""


DASHBOARD = r"""<!doctype html>
<html lang="zh-CN"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width">
<meta name="csrf" content="__CSRF__">
<title>RLPlayer 管理后台</title>
<style>
*{box-sizing:border-box}
body{margin:0;font:13px/1.5 -apple-system,BlinkMacSystemFont,"SF Pro Text","PingFang SC","Microsoft YaHei",sans-serif;
 background:#f5f5f7;color:#1d1d1f;-webkit-font-smoothing:antialiased}
a{color:#0071e3;text-decoration:none}a:hover{color:#0058b0}
#topbar{position:fixed;top:0;left:0;right:0;height:52px;display:flex;align-items:center;gap:12px;padding:0 18px;
 background:rgba(255,255,255,.78);backdrop-filter:saturate(180%) blur(20px);-webkit-backdrop-filter:saturate(180%) blur(20px);
 border-bottom:1px solid #e5e5ea;z-index:30}
#topbar b{font-size:14px;font-weight:700;letter-spacing:-.2px}
#topbar .ver{font-size:11px;color:#86868b;background:#f2f2f7;border-radius:6px;padding:2px 7px}
#topbar .sp{flex:1}
.chip{display:inline-flex;align-items:center;gap:7px;background:#f2f2f7;border-radius:20px;padding:4px 10px 4px 5px;font-size:12px;color:#3a3a3c}
.chip i{width:22px;height:22px;border-radius:50%;background:#0071e3;color:#fff;font-style:normal;font-size:11px;
 display:grid;place-items:center;font-weight:600;text-transform:uppercase}
#sidebar{position:fixed;top:52px;bottom:0;left:0;width:196px;padding:12px 8px;
 background:rgba(250,250,252,.72);backdrop-filter:saturate(180%) blur(20px);-webkit-backdrop-filter:saturate(180%) blur(20px);
 border-right:1px solid #e5e5ea;z-index:20}
#sidebar a{display:flex;align-items:center;gap:9px;padding:8px 11px;margin:2px 0;border-radius:9px;color:#3a3a3c;font-size:13px}
#sidebar a:hover{background:rgba(0,0,0,.045)}
#sidebar a.active{background:#0071e3;color:#fff;font-weight:600}
#sidebar .dot{width:7px;height:7px;border-radius:50%;background:#c7c7cc}
#sidebar a.active .dot{background:#fff}
#sidebar .sep{height:1px;background:#e5e5ea;margin:10px 12px}
#content{margin:52px 0 0 196px;padding:22px 26px 48px;min-height:calc(100vh - 52px)}
h1{font-size:26px;font-weight:700;letter-spacing:-.4px;margin:0 0 4px}
h1 .sub{display:block;font-size:12px;font-weight:400;color:#86868b;letter-spacing:0;margin-top:4px}
.notice{background:rgba(255,255,255,.9);border-radius:12px;padding:10px 14px;margin:0 0 16px;color:#1d1d1f;font-size:12px;
 box-shadow:0 1px 2px rgba(0,0,0,.04)}
.notice.warn{background:#fff8e6;color:#8a5a00}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(190px,1fr));gap:14px;margin-bottom:16px}
.kpi{background:#fff;border:1px solid #e8e8ed;border-radius:14px;padding:14px 16px;box-shadow:0 1px 2px rgba(0,0,0,.04)}
.kpi .t{color:#6e6e73;font-size:12px;display:flex;justify-content:space-between;align-items:center;gap:6px}
.kpi .n{font-size:25px;font-weight:600;letter-spacing:-.5px;margin-top:8px;font-variant-numeric:tabular-nums}
.kpi .m{color:#86868b;font-size:11.5px;margin-top:4px;min-height:17px}
.card{background:#fff;border:1px solid #e8e8ed;border-radius:14px;box-shadow:0 1px 2px rgba(0,0,0,.04);margin-bottom:16px;overflow:hidden}
.card>.hd{display:flex;align-items:center;gap:10px;padding:12px 16px;border-bottom:1px solid #f0f0f2}
.card>.hd h2{margin:0;font-size:13px;font-weight:600}
.card>.hd .sp{flex:1}
.card>.bd{padding:14px 16px}
.card>.bd.flush{padding:0}
table.widefat{width:100%;border-collapse:collapse}
.widefat th,.widefat td{padding:9px 14px;text-align:left;vertical-align:middle;border-bottom:1px solid #f2f2f7}
.widefat thead th{font-size:11.5px;font-weight:600;color:#86868b;white-space:nowrap;background:#fff}
.widefat tbody tr:hover{background:#fafafc}
.widefat tbody tr:last-child td{border-bottom:0}
.num{text-align:right;font-variant-numeric:tabular-nums;white-space:nowrap}
.mono{font-family:ui-monospace,SFMono-Regular,Menlo,Consolas,monospace;font-size:12px}
.muted{color:#86868b}
.tag{display:inline-block;padding:1px 8px;border-radius:20px;font-size:11.5px;font-weight:500;background:#f2f2f7;color:#6e6e73}
.tag.ok{background:#e7f8ee;color:#12833f}
.tag.no{background:#fdecec;color:#c62828}
.tag.win{background:#e9f2ff;color:#0a5fbf}
.tag.mac{background:#f3ecff;color:#7231c9}
.tag.warn{background:#fff4e0;color:#8a5a00}
.button{display:inline-block;padding:5px 12px;border:1px solid #d2d2d7;border-radius:8px;background:#fff;color:#1d1d1f;
 font:inherit;font-size:12px;cursor:pointer;line-height:1.5}
.button:hover{background:#f5f5f7}
.button.pri{background:#0071e3;border-color:#0071e3;color:#fff}
.button.pri:hover{background:#0062c4}
.button.danger{color:#c62828;border-color:#f0c4c4;background:#fff}
.button.danger:hover{background:#fdecec}
.button:disabled{opacity:.45;cursor:default}
input[type=text],input[type=search],input[type=password],select{padding:6px 10px;border:1px solid #d2d2d7;border-radius:8px;
 font:inherit;font-size:12px;background:#fff;color:#1d1d1f}
input:focus,select:focus{outline:0;border-color:#0071e3;box-shadow:0 0 0 3px rgba(0,113,227,.14)}
.toolbar{display:flex;flex-wrap:wrap;gap:8px;align-items:center}
.toolbar .sp{flex:1}
.seg{display:inline-flex;background:#f2f2f7;border-radius:9px;padding:2px}
.seg button{border:0;background:transparent;color:#3a3a3c;padding:5px 12px;font:inherit;font-size:12px;border-radius:7px;cursor:pointer}
.seg button.on{background:#fff;color:#1d1d1f;font-weight:600;box-shadow:0 1px 2px rgba(0,0,0,.12)}
.bar{height:6px;background:#f2f2f7;border-radius:3px;overflow:hidden;min-width:90px}
.bar i{display:block;height:100%;background:#0071e3;border-radius:3px}
.bar.mac i{background:#8e4ec6}
.bar.warn i{background:#e0a100}.bar.bad i{background:#d70015}
.spark{display:flex;align-items:flex-end;gap:3px;height:44px}
.spark i{flex:1;background:#b9d7ff;min-height:2px;border-radius:2px 2px 0 0}
.spark.mac i{background:#dcc7f2}
.pager{display:flex;gap:8px;align-items:center;justify-content:flex-end;padding:10px 16px;border-top:1px solid #f2f2f7}
.split{display:grid;grid-template-columns:1fr 1fr;gap:16px}
@media(max-width:1100px){.split{grid-template-columns:1fr}}
pre.detail{white-space:pre-wrap;background:#fafafc;border:1px solid #ececf1;border-radius:10px;padding:12px;max-height:340px;overflow:auto;margin:0;font-size:12px}
.formrow{display:flex;gap:8px;flex-wrap:wrap;align-items:center}
.formrow label{font-weight:600;font-size:12px;color:#6e6e73}
.hidden{display:none!important}
.toast{position:fixed;right:18px;bottom:18px;background:#1d1d1f;color:#fff;padding:10px 15px;border-radius:10px;font-size:12px;
 opacity:0;transform:translateY(8px);transition:.2s;z-index:50;max-width:360px;box-shadow:0 8px 30px rgba(0,0,0,.2)}
.toast.on{opacity:1;transform:none}
.storage-row{display:flex;align-items:center;gap:12px;margin:10px 0}
.storage-row .lbl{width:180px;font-size:12px;color:#3a3a3c;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.storage-row .val{width:210px;text-align:right;font-size:12px;color:#6e6e73;font-variant-numeric:tabular-nums}
.storage-row .bar{flex:1}
</style></head>
<body>
<div id="topbar"><b>RLPlayer</b><span class="ver">服务端 __VERSION__</span><span class="sp"></span>
<span class="chip"><i>__INITIAL__</i>__USER__</span>
<button class="button" id="btn-refresh">刷新</button><button class="button" id="btn-logout">退出</button></div>
<div id="sidebar">
<a href="#overview" data-view="overview" class="active"><span class="dot"></span>概览</a>
<a href="#update" data-view="update"><span class="dot"></span>增量更新</a>
<a href="#feedback" data-view="feedback"><span class="dot"></span>反馈</a>
<a href="#blacklist" data-view="blacklist"><span class="dot"></span>黑名单</a>
<div class="sep"></div>
<a href="#users" data-view="users"><span class="dot"></span>用户</a>
</div>
<div id="content">
<h1 id="page-title">概览<span class="sub" id="page-sub"></span></h1>
<div id="notice" class="notice hidden"></div>

<div id="view-overview">
  <div class="grid" id="ov-kpis"></div>
  <div class="split">
    <div class="card"><div class="hd"><h2>近 14 天对象流量</h2><span class="sp"></span><span class="muted" id="ov-flow-sum"></span></div>
      <div class="bd"><div class="spark" id="ov-flow-spark"></div><div class="muted" style="margin-top:6px;font-size:11px" id="ov-flow-axis"></div></div></div>
    <div class="card"><div class="hd"><h2>近 14 天反馈</h2><span class="sp"></span><span class="muted" id="ov-fb-sum"></span></div>
      <div class="bd"><div class="spark" id="ov-fb-spark"></div><div class="muted" style="margin-top:6px;font-size:11px" id="ov-fb-axis"></div></div></div>
  </div>
  <div class="card"><div class="hd"><h2>存储配额（Windows / macOS 各 5 GB）</h2><span class="sp"></span><span class="muted" id="ov-storage-sum"></span></div>
    <div class="bd" id="ov-storage"></div></div>
  <div class="card"><div class="hd"><h2>最近事件</h2><span class="sp"></span><a class="button" href="/admin/api/export.csv">导出 CSV</a></div>
    <div class="bd flush" id="ov-events"></div></div>
</div>

<div id="view-update" class="hidden">
  <div class="grid" id="up-kpis"></div>
  <div class="card"><div class="hd"><h2>下载次数</h2><span class="sp"></span>
      <div class="seg" id="up-range"><button data-r="day" class="on">日</button><button data-r="week">周</button><button data-r="month">月</button></div></div>
    <div class="bd"><div class="toolbar">
      <select id="up-platform"><option value="all">全部平台</option><option value="windows">Windows</option><option value="mac">macOS</option></select>
      <div class="seg" id="up-dim"><button data-d="ip" class="on">按 IP</button><button data-d="client">按设备</button></div>
      <input type="search" id="up-q" placeholder="搜索 IP / 设备 ID 前缀" size="20">
      <button class="button" id="up-search">筛选</button>
      <span class="sp"></span><span class="muted" id="up-dl-sum"></span></div></div>
    <div class="bd flush" id="up-dl-table"></div>
    <div class="pager"><span class="muted" id="up-dl-page"></span>
      <button class="button" id="up-dl-prev">上一页</button><button class="button" id="up-dl-next">下一页</button></div>
  </div>
  <div class="card"><div class="hd"><h2>使用流量</h2><span class="sp"></span>
      <div class="seg" id="up-range2"><button data-r="day" class="on">日</button><button data-r="week">周</button><button data-r="month">月</button></div></div>
    <div class="bd"><div class="toolbar">
      <select id="up-platform2"><option value="all">全部平台</option><option value="windows">Windows</option><option value="mac">macOS</option></select>
      <div class="seg" id="up-dim2"><button data-d="ip" class="on">按 IP</button><button data-d="client">按设备</button></div>
      <input type="search" id="up-q2" placeholder="搜索 IP / 设备 ID 前缀" size="20">
      <button class="button" id="up-search2">筛选</button>
      <span class="sp"></span><span class="muted" id="up-bw-sum"></span></div></div>
    <div class="bd flush" id="up-bw-table"></div>
    <div class="pager"><span class="muted" id="up-bw-page"></span>
      <button class="button" id="up-bw-prev">上一页</button><button class="button" id="up-bw-next">下一页</button></div>
  </div>
  <div class="split">
    <div class="card"><div class="hd"><h2>来源区域</h2><span class="sp"></span>
        <div class="seg" id="up-region-range"><button data-r="day" class="on">日</button><button data-r="week">周</button><button data-r="month">月</button></div></div>
      <div class="bd flush" id="up-region"></div></div>
    <div class="card"><div class="hd"><h2>外部点击</h2><span class="sp"></span>
        <div class="seg" id="up-ext-range"><button data-r="day" class="on">日</button><button data-r="week">周</button><button data-r="month">月</button></div></div>
      <div class="bd" id="up-ext"></div>
      <div class="hd" style="border-top:1px solid #f0f0f2"><h2>24 小时报错</h2><span class="sp"></span><span class="muted" id="up-err-sum"></span></div>
      <div class="bd flush" id="up-errors"></div></div>
  </div>
  <div class="card"><div class="hd"><h2>存储与版本</h2><span class="sp"></span><span class="muted" id="up-storage-sum"></span></div>
    <div class="bd" id="up-storage"></div></div>
</div>

<div id="view-feedback" class="hidden">
  <div class="grid" id="fb-kpis"></div>
  <div class="card"><div class="hd"><h2>反馈工单</h2><span class="sp"></span>
      <div class="seg" id="fb-platform"><button data-p="all" class="on">全部</button><button data-p="windows">Windows</button><button data-p="mac">macOS</button></div>
      <div class="seg" id="fb-range"><button data-r="day">日</button><button data-r="week">周</button><button data-r="month" class="on">月</button><button data-r="all">全部</button></div></div>
    <div class="bd"><div class="muted" id="fb-hint" style="font-size:12px"></div></div>
    <div class="bd flush" id="fb-table"></div>
    <div class="bd"><pre class="detail" id="fb-detail">点「查看」显示正文。</pre></div>
  </div>
</div>

<div id="view-blacklist" class="hidden">
  <div class="grid" id="bl-kpis"></div>
  <div class="card"><div class="hd"><h2>加入黑名单</h2></div>
    <div class="bd"><div class="formrow">
      <label>类型</label><select id="bl-kind"><option value="ip">IP / 网段</option><option value="client">设备 ID</option></select>
      <input type="text" id="bl-value" placeholder="183.7.144.182 / 183.7.144.0/24 / 183.7.144.* / 设备ID" size="32">
      <input type="text" id="bl-note" placeholder="备注（可选）" size="20">
      <button class="button pri" id="bl-add">加入</button></div>
      <p class="muted" style="margin:10px 0 0;font-size:12px">命中黑名单的客户端仍能检查更新与提交反馈，但下载清单/对象会收到 429 配额提示，
      播放器按既有逻辑显示百度网盘 / GitHub 手动下载入口，不会完全禁用软件。</p></div>
  </div>
  <div class="card"><div class="hd"><h2>黑名单列表</h2><span class="sp"></span><span class="muted" id="bl-sum"></span></div>
    <div class="bd flush" id="bl-table"></div></div>
</div>

<div id="view-users" class="hidden">
  <div class="card"><div class="hd"><h2>管理员账号</h2></div><div class="bd flush" id="us-table"></div></div>
  <div class="card"><div class="hd"><h2>修改我的密码</h2></div><div class="bd"><div class="formrow">
      <input type="password" id="pw-old" placeholder="当前密码" autocomplete="current-password">
      <input type="password" id="pw-new" placeholder="新密码（至少 8 位）" autocomplete="new-password">
      <button class="button pri" id="pw-save">保存</button></div></div></div>
</div>
</div>
<div class="toast" id="toast"></div>
<script>
var csrf=document.querySelector('meta[name=csrf]').content;
var state={range:'day',platform:'all',dimension:'ip',q:'',page:0,
  bw:{range:'day',platform:'all',dimension:'ip',q:'',page:0},
  regionRange:'day',extRange:'day',fbRange:'month',fbPlatform:'all'};
var VIEWS=['overview','update','feedback','blacklist','users'];
var TITLES={overview:['概览','Windows 与 macOS 的更新、反馈、存储与配额'],
  update:['增量更新','本月流量、下载次数、使用流量、来源区域、黑名单与存储'],
  feedback:['反馈','按平台分开的 BUG 与建议，拉取时不会混淆'],
  blacklist:['黑名单','手动禁止下载，但仍引导到网盘'],
  users:['用户','管理员账号']};
var KIND={object_download:'对象下载',api:'API 请求',feedback:'反馈',update_report:'更新回执',
  external_click:'外链点击',admin_login:'管理员登录',admin_login_failed:'登录失败',audit:'审计',
  quota_warning:'配额预警',blocked_download:'黑名单拦截',blacklist_add:'加入黑名单',
  blacklist_remove:'解除拉黑',admin_password_changed:'修改密码',publish_macos:'macOS 发布',
  publish_macos_object:'macOS 上传对象',publish_denied:'发布被拒'};

function el(tag,cls,text){var e=document.createElement(tag);if(cls)e.className=cls;if(text!==undefined&&text!==null)e.textContent=String(text);return e;}
function kindLabel(k){return KIND[k]||k;}
function fmtB(n){n=Number(n||0);if(n<1024)return n+' B';var u=['KB','MB','GB','TB'],i=-1;do{n/=1024;i++;}while(n>=1024&&i<u.length-1);return (n>=100?n.toFixed(0):n.toFixed(1))+' '+u[i];}
function fmtN(n){return new Intl.NumberFormat().format(Number(n||0));}
function fmtT(s){if(!s)return '—';var d=new Date(s);if(isNaN(d))return s;function p(n){return (n<10?'0':'')+n;}
  return d.getFullYear()+'-'+p(d.getMonth()+1)+'-'+p(d.getDate())+' '+p(d.getHours())+':'+p(d.getMinutes());}
function platTag(p){return p==='mac'?el('span','tag mac','macOS'):el('span','tag win','Windows');}
function statusTag(s){return el('span','tag '+(s>=400?'no':'ok'),String(s));}
function toast(msg){var t=document.getElementById('toast');t.textContent=msg;t.className='toast on';
  clearTimeout(toast.h);toast.h=setTimeout(function(){t.className='toast';},2600);}
function notice(msg,warn){var n=document.getElementById('notice');if(!msg){n.className='notice hidden';n.textContent='';return;}
  n.className='notice'+(warn?' warn':'');n.textContent=msg;}
async function api(path,opts){opts=opts||{};opts.cache='no-store';
  if(opts.body&&typeof opts.body!=='string')opts.body=JSON.stringify(opts.body);
  if(opts.body)opts.headers=Object.assign({'Content-Type':'application/json','X-CSRF-Token':csrf},opts.headers||{});
  var r=await fetch(path,opts);
  if(r.status===401){location='/admin/login';throw new Error('未登录');}
  var j=null;try{j=await r.json();}catch(e){}
  if(!r.ok)throw new Error((j&&j.error)||('HTTP '+r.status));
  return j||{};}
function table(headers,rows){
  var t=el('table','widefat');
  var th=el('thead'),tr=el('tr');
  headers.forEach(function(h){tr.appendChild(el('th',h.cls||'',h.t||h));});
  th.appendChild(tr);t.appendChild(th);
  var tb=el('tbody');
  if(!rows.length){var r=el('tr'),c=el('td','muted','暂无数据');c.colSpan=headers.length;r.appendChild(c);tb.appendChild(r);}
  rows.forEach(function(row){var tr=el('tr');row.forEach(function(cell){var td=el('td',cell&&cell.cls?cell.cls:'');
    if(cell&&cell.node){td.appendChild(cell.node);}else if(cell&&cell.html!==undefined){td.innerHTML=cell.html;}
    else{td.textContent=(cell&&cell.t!==undefined)?cell.t:(cell===undefined?'':cell);}tr.appendChild(td);});tb.appendChild(tr);});
  t.appendChild(tb);return t;}
function kpi(title,value,meta,tag){
  var d=el('div','kpi'),t=el('div','t'),n=el('div','n'),m=el('div','m');
  t.appendChild(el('span','',title));if(tag)t.appendChild(el('span','tag '+(tag.cls||''),tag.t));
  n.textContent=value;m.textContent=meta||'';d.appendChild(t);d.appendChild(n);d.appendChild(m);return d;}
function spark(box,values,labels,cls){
  box.className='spark'+(cls?' '+cls:'');box.replaceChildren();var max=1;
  values.forEach(function(v){if(v>max)max=v;});
  values.forEach(function(v,i){var b=el('i');b.style.height=Math.max(2,Math.round(v/max*42))+'px';
    b.title=(labels[i]||'')+'：'+fmtB(v);box.appendChild(b);});}
function bar(ratio,cls){
  var w=el('div','bar'+(cls?' '+cls:''));var i=el('i',ratio>=0.9?'bad':ratio>=0.8?'warn':'');
  i.style.width=Math.min(100,Math.round(ratio*100))+'%';w.appendChild(i);return w;}

function setView(name){
  if(VIEWS.indexOf(name)<0)name='overview';
  VIEWS.forEach(function(v){document.getElementById('view-'+v).className=(v===name?'':'hidden');});
  var links=document.querySelectorAll('#sidebar a');
  for(var i=0;i<links.length;i++)links[i].className=(links[i].getAttribute('data-view')===name?'active':'');
  document.getElementById('page-title').firstChild.textContent=TITLES[name][0];
  document.getElementById('page-sub').textContent=TITLES[name][1];
  if(location.hash!=='#'+name)history.replaceState(null,'','#'+name);
  notice('');
  if(name==='overview')loadOverview();
  if(name==='update')loadUpdate();
  if(name==='feedback')loadFeedback();
  if(name==='blacklist')loadBlacklist();
  if(name==='users')loadUsers();}

function renderStorage(box,storage,extra){
  box.replaceChildren();
  var rows=[['Windows 更新','windows','win'],['macOS 更新','macos','mac'],['反馈附件','feedback','']];
  rows.forEach(function(item){
    var s=storage[item[1]]||{used:0,limit:1};
    var row=el('div','storage-row');
    row.appendChild(el('div','lbl',item[0]));
    row.appendChild(bar(s.limit?Math.min(1,s.used/s.limit):0,item[2]));
    row.appendChild(el('div','val',fmtB(s.used)+' / '+fmtB(s.limit)+'（'+(s.limit?(100*s.used/s.limit).toFixed(1):'0')+'%）'));
    box.appendChild(row);});
  var total=storage.total||{used:0,limit:1};
  box.appendChild(el('div','muted','物理合计 '+fmtB(total.used)+' / 上限 '+fmtB(total.limit)+'；更新目录 '+fmtB(storage.updateTree||0)));
  if(extra)box.appendChild(extra);}

async function loadOverview(){
  try{
    var s=await api('/admin/api/overview');
    var tw=s.traffic.windows||{},tm=s.traffic.mac||{};
    var k=[];
    k.push(kpi('本月流量 · Windows',fmtB(tw.objectBytes||0),'API '+fmtB(tw.apiBytes||0),{t:'WIN',cls:'win'}));
    k.push(kpi('本月流量 · macOS',fmtB(tm.objectBytes||0),'API '+fmtB(tm.apiBytes||0),{t:'MAC',cls:'mac'}));
    k.push(kpi('存储合计',fmtB(s.storage.total.used),'Windows '+fmtB(s.storage.windows.used)+' · macOS '+fmtB(s.storage.macos.used)));
    k.push(kpi('24 小时报错',fmtN(s.errors24h),'外链：百度 '+(s.external.baidu||0)+' · GH-Win '+(s.external.githubWindows||0)+' · GH-Mac '+(s.external.githubMacos||0)));
    k.push(kpi('反馈',fmtN(s.feedback.month),'今日 '+s.feedback.day+' · 本周 '+s.feedback.week+' · 累计 '+s.feedback.total));
    k.push(kpi('黑名单',fmtN(s.blacklist.total),'24h 拦截 '+(s.blocked24h||0)+' 次'));
    document.getElementById('ov-kpis').replaceChildren.apply(document.getElementById('ov-kpis'),k);
    var days=s.trend.days||[];
    spark(document.getElementById('ov-flow-spark'),s.trend.objectBytes||[],days);
    document.getElementById('ov-flow-axis').textContent=days.length?days[0]+' → '+days[days.length-1]:'—';
    document.getElementById('ov-flow-sum').textContent='合计 '+fmtB((s.trend.objectBytes||[]).reduce(function(a,b){return a+b;},0));
    spark(document.getElementById('ov-fb-spark'),s.trend.feedback||[],days);
    document.getElementById('ov-fb-axis').textContent=days.length?days[0]+' → '+days[days.length-1]:'—';
    document.getElementById('ov-fb-sum').textContent='合计 '+fmtN((s.trend.feedback||[]).reduce(function(a,b){return a+b;},0))+' 条';
    document.getElementById('ov-storage-sum').textContent='合计 '+fmtB(s.storage.total.used);
    renderStorage(document.getElementById('ov-storage'),s.storage,
      el('div','muted','增量更新自 v'+(s.incrementalSince||'3.2.2')+' 起支持；Windows 清单 '+(s.releases||[]).length+' 个，macOS 清单 '+(s.macosReleases||[]).length+' 个'));
    document.getElementById('ov-events').replaceChildren(table(
      [{t:'时间'},{t:'类型'},{t:'状态',cls:'num'},{t:'IP'},{t:'路径'},{t:'字节',cls:'num'}],
      (s.recentEvents||[]).map(function(e){return [fmtT(e.time),kindLabel(e.kind),{node:statusTag(e.status)},e.ip,e.path,fmtB(e.bytes)];})));
    notice(s.traffic.warning80?'本月流量已用到 80% 以上，请留意配额。':'');}
  catch(e){toast(e.message);}}

function segBind(id,attr,cb){
  var box=document.getElementById(id);if(!box)return;
  box.addEventListener('click',function(ev){var b=ev.target.closest('button');if(!b)return;
    var btns=box.querySelectorAll('button');for(var i=0;i<btns.length;i++)btns[i].className='';
    b.className='on';cb(b.getAttribute(attr));});}

async function loadUpdate(){
  try{
    var o=await api('/admin/api/overview');
    var k=[];
    k.push(kpi('本月对象流量',fmtB(o.traffic.objectBytes),
      'Windows '+fmtB(o.traffic.windows.objectBytes)+' · macOS '+fmtB(o.traffic.mac.objectBytes)));
    k.push(kpi('本月 API 流量',fmtB(o.traffic.apiBytes),'上限 '+fmtB(o.traffic.apiLimit)));
    k.push(kpi('外部点击',fmtN(o.external.total),'百度 '+(o.external.baidu||0)+' · GH-Win '+(o.external.githubWindows||0)+' · GH-Mac '+(o.external.githubMacos||0)));
    k.push(kpi('对象存储',fmtB(o.storage.objects),'Windows '+fmtB(o.storage.windows.used)+' / macOS '+fmtB(o.storage.macos.used)));
    k.push(kpi('24 小时报错',fmtN(o.errors24h),o.traffic.incrementalOnly90?'≥90% 仅增量':o.traffic.warning80?'≥80% 预警':'配额正常'));
    k.push(kpi('活动预留',fmtN(o.traffic.reserved),'下载中的保守预留字节'));
    document.getElementById('up-kpis').replaceChildren.apply(document.getElementById('up-kpis'),k);
    renderStorage(document.getElementById('up-storage'),o.storage,storageVersions(o));
    loadDownloads();loadBandwidth();loadRegions();loadExternal();loadErrors();}
  catch(e){toast(e.message);}}

function storageVersions(o){
  var wrap=el('div');
  var rows=[];
  (o.releases||[]).forEach(function(r){rows.push(['Windows v'+r.version,fmtT(r.publishedAt),r.files+' 个文件',
    r.installMode==='manual'?'手动安装':'增量更新']);});
  (o.macosReleases||[]).forEach(function(r){rows.push(['macOS v'+r.version,fmtT(r.publishedAt),r.files+' 个文件',
    r.installMode==='manual'?'手动安装':'增量更新']);});
  wrap.appendChild(el('div','muted','增量更新自 v'+(o.incrementalSince||'3.2.2')+' 起支持；服务器只存增量对象，不存 FULL 包。'));
  wrap.appendChild(table([{t:'版本'},{t:'发布时间'},{t:'清单文件数'},{t:'模式'}],rows));
  return wrap;}

async function loadDownloads(){
  try{
    var q='/admin/api/downloads?range='+state.range+'&platform='+state.platform+'&dimension='+state.dimension+
      '&q='+encodeURIComponent(state.q)+'&limit=20&offset='+state.page*20;
    var j=await api(q);
    var rows=(j.rows||[]).map(function(r){
      var op=el('span','');
      if(state.dimension==='ip'){
        if(r.blocked)op.appendChild(el('span','tag no','已拉黑'));
        else{var b=el('button','button danger','拉黑');b.onclick=function(){blacklistValue(r.value);};op.appendChild(b);}
      }else{op.appendChild(el('span','muted','—'));}
      return [r.value+(r.blocked?' [已拉黑]':''),(r.reserved?'内网/保留':(r.country||'未知')),
        (r.province&&r.province!=='0'?r.province:'—')+(r.city&&r.city!=='0'?' · '+r.city:''),
        {t:fmtN(r.requests),cls:'num'},{t:fmtB(r.bytes),cls:'num'},fmtT(r.last),{node:op}];});
    document.getElementById('up-dl-table').replaceChildren(table(
      [{t:'IP / 设备ID'},{t:'国家'},{t:'省 / 市'},{t:'下载次数',cls:'num'},{t:'流量',cls:'num'},{t:'最近'},{t:'操作'}],rows));
    document.getElementById('up-dl-sum').textContent='共 '+fmtN(j.total)+' 项 · 次数 '+fmtN(j.totals.requests)+' · 流量 '+fmtB(j.totals.bytes);
    document.getElementById('up-dl-page').textContent='第 '+(state.page+1)+' / '+Math.max(1,j.pages)+' 页';}
  catch(e){toast(e.message);}}

async function loadBandwidth(){
  try{
    var b=state.bw;
    var q='/admin/api/downloads?range='+b.range+'&platform='+b.platform+'&dimension='+b.dimension+
      '&q='+encodeURIComponent(b.q)+'&limit=20&offset='+b.page*20+'&sort=bytes';
    var j=await api(q);
    var rows=(j.rows||[]).map(function(r){
      var op=el('span','');
      if(b.dimension==='ip'){
        if(r.blocked)op.appendChild(el('span','tag no','已拉黑'));
        else{var bb=el('button','button danger','拉黑');bb.onclick=function(){blacklistValue(r.value);};op.appendChild(bb);}
      }else{op.appendChild(el('span','muted','—'));}
      return [r.value+(r.blocked?' [已拉黑]':''),(r.reserved?'内网/保留':(r.country||'未知')),
        (r.province&&r.province!=='0'?r.province:'—'),{t:fmtB(r.bytes),cls:'num'},
        {t:fmtN(r.requests),cls:'num'},fmtT(r.last),{node:op}];});
    document.getElementById('up-bw-table').replaceChildren(table(
      [{t:'IP / 设备ID'},{t:'国家'},{t:'省'},{t:'流量',cls:'num'},{t:'次数',cls:'num'},{t:'最近'},{t:'操作'}],rows));
    document.getElementById('up-bw-sum').textContent='共 '+fmtN(j.total)+' 项 · 流量 '+fmtB(j.totals.bytes);
    document.getElementById('up-bw-page').textContent='第 '+(b.page+1)+' / '+Math.max(1,j.pages)+' 页';}
  catch(e){toast(e.message);}}

async function loadRegions(){
  try{
    var j=await api('/admin/api/regions?range='+state.regionRange);
    var max=1;(j.rows||[]).forEach(function(r){if(r.bytes>max)max=r.bytes;});
    var rows=(j.rows||[]).map(function(r){var n=el('div','bar'),i=el('i');i.style.width=Math.round(r.bytes/max*100)+'%';n.appendChild(i);
      return [r.reserved?'内网/保留':(r.country||'未知'),(r.province&&r.province!=='0'?r.province:'—'),
        {t:fmtN(r.requests),cls:'num'},{t:fmtB(r.bytes),cls:'num'},{node:n}];});
    document.getElementById('up-region').replaceChildren(table(
      [{t:'国家'},{t:'省'},{t:'请求',cls:'num'},{t:'流量',cls:'num'},{t:'占比'}],rows));}
  catch(e){toast(e.message);}}

async function loadExternal(){
  try{
    var j=await api('/admin/api/external?range='+state.extRange);
    var box=document.getElementById('up-ext');box.replaceChildren();
    var items=[['百度网盘 · Win/Mac 共用',j.baidu||0,''],
               ['GitHub · Windows',j.githubWindows||0,'win'],
               ['GitHub · macOS',j.githubMacos||0,'mac']];
    var max=1;items.forEach(function(x){if(x[1]>max)max=x[1];});
    items.forEach(function(x){
      var row=el('div','storage-row');
      row.appendChild(el('div','lbl',x[0]));
      row.appendChild(bar(x[1]/max,x[2]));
      row.appendChild(el('div','val',fmtN(x[1])+' 次'));
      box.appendChild(row);});
    box.appendChild(el('div','muted','外链点击只统计跳转次数，真实下载不归服务器统计。'));}
  catch(e){toast(e.message);}}

async function loadErrors(){
  try{
    var j=await api('/admin/api/errors?hours=24');
    document.getElementById('up-err-sum').textContent='共 '+fmtN(j.total)+' 条';
    document.getElementById('up-errors').replaceChildren(table(
      [{t:'时间'},{t:'类型'},{t:'状态',cls:'num'},{t:'IP'},{t:'路径'},{t:'版本'}],
      (j.rows||[]).map(function(e){return [fmtT(e.time),kindLabel(e.kind),{node:statusTag(e.status)},e.ip,e.path,e.version||'—'];})));
  }catch(e){toast(e.message);}}

async function loadFeedback(){
  try{
    var j=await api('/admin/api/feedback?range='+state.fbRange+'&platform='+state.fbPlatform+'&limit=100');
    var k=[kpi('今日',fmtN(j.counts.day),''),kpi('本周',fmtN(j.counts.week),''),
           kpi('本月',fmtN(j.counts.month),''),kpi('累计',fmtN(j.counts.total),'')];
    document.getElementById('fb-kpis').replaceChildren.apply(document.getElementById('fb-kpis'),k);
    var tabs=document.querySelectorAll('#fb-platform button');
    var labels={all:'全部',windows:'Windows',mac:'macOS'};
    for(var i=0;i<tabs.length;i++){
      var p=tabs[i].getAttribute('data-p');
      var n=(j.byPlatform&&j.byPlatform[p])||0;
      tabs[i].textContent=labels[p]+'（'+n+'）';
      tabs[i].className=(p===state.fbPlatform?'on':'');
    }
    document.getElementById('fb-hint').textContent='平台标签由提交入口决定（/feedback/macos 强制 macOS）；旧客户端如果 os 写的是 macOS，也会自动归到 macOS。';
    var rows=(j.rows||[]).map(function(f){
      var ops=el('span','');
      var b=el('button','button','查看');b.onclick=function(){showFeedback(f.id);};ops.appendChild(b);
      if(f.diagStored){var a=el('a','button','附件');a.href='/admin/api/feedback/attachment?id='+encodeURIComponent(f.id);a.style.marginLeft='6px';ops.appendChild(a);}
      return [fmtT(f.time),f.id,{node:platTag(f.platform)},'v'+(f.version||'?'),f.os||'—',
        (f.diagStored?fmtB(f.diagBytes):'未存储'),f.title||'—',{node:ops}];});
    document.getElementById('fb-table').replaceChildren(table(
      [{t:'时间'},{t:'工单'},{t:'平台'},{t:'版本'},{t:'系统'},{t:'附件',cls:'num'},{t:'标题'},{t:'操作'}],rows));}
  catch(e){toast(e.message);}}

async function showFeedback(id){
  try{var j=await api('/admin/api/feedback/detail?id='+encodeURIComponent(id));
    var head='['+(j.platform==='mac'?'macOS':'Windows')+'] '+j.id+' · v'+j.version+' · '+j.os+'\n\n';
    document.getElementById('fb-detail').textContent=head+(j.text||'(空)');}
  catch(e){toast(e.message);}}

function blacklistValue(value){
  if(!value){toast('无法识别该 IP');return;}
  if(!confirm('把 '+value+' 加入黑名单？\n命中后下载会返回 429，播放器显示百度/GitHub 手动下载入口（软件仍可用）。'))return;
  api('/admin/api/blacklist',{method:'POST',body:{kind:'ip',value:value,note:'来自下载列表'}})
    .then(function(){toast('已加入黑名单');loadDownloads();loadBandwidth();})
    .catch(function(e){toast(e.message);});}

async function loadBlacklist(){
  try{
    var j=await api('/admin/api/blacklist');
    var ips=0,cls=0;(j.rows||[]).forEach(function(r){if(r.kind==='ip')ips++;else cls++;});
    document.getElementById('bl-kpis').replaceChildren.apply(document.getElementById('bl-kpis'),[
      kpi('黑名单总数',fmtN(j.total),'IP/网段 '+ips+' · 设备 '+cls),
      kpi('24 小时拦截',fmtN(j.blocked24h||0),'返回 429 + 网盘入口')]);
    var rows=(j.rows||[]).map(function(r){
      var del=el('button','button danger','移除');del.onclick=async function(){
        if(!confirm('确认移除 '+r.value+' ？'))return;
        try{await api('/admin/api/blacklist/remove',{method:'POST',body:{id:r.id}});toast('已移除');loadBlacklist();}
        catch(e){toast(e.message);}};
      return [r.kind==='ip'?'IP / 网段':'设备 ID',r.value,r.note||'—',fmtT(r.created),r.createdBy||'—',{node:del}];});
    document.getElementById('bl-table').replaceChildren(table(
      [{t:'类型'},{t:'值'},{t:'备注'},{t:'加入时间'},{t:'操作人'},{t:'操作'}],rows));
    document.getElementById('bl-sum').textContent='共 '+fmtN(j.total)+' 条';}
  catch(e){toast(e.message);}}

async function loadUsers(){
  try{
    var j=await api('/admin/api/users');
    document.getElementById('us-table').replaceChildren(table(
      [{t:'用户名'},{t:'角色'},{t:'创建时间'},{t:'说明'}],
      (j.rows||[]).map(function(u){return [u.username+(u.username===j.current?'（我）':''),
        u.role==='admin'?'管理员':u.role,fmtT(u.created),u.username===j.current?'当前会话':''];})));
  }catch(e){toast(e.message);}}

document.getElementById('btn-logout').onclick=async function(ev){ev.preventDefault();
  try{await api('/admin/api/logout',{method:'POST',body:{}});}catch(e){}
  location='/admin/login';};
document.getElementById('btn-refresh').onclick=function(ev){ev.preventDefault();setView(location.hash.slice(1)||'overview');};
var navlinks=document.querySelectorAll('#sidebar a');
for(var i=0;i<navlinks.length;i++)navlinks[i].addEventListener('click',function(ev){ev.preventDefault();setView(this.getAttribute('data-view'));});
segBind('up-range','data-r',function(r){state.range=r;state.page=0;loadDownloads();});
segBind('up-range2','data-r',function(r){state.bw.range=r;state.bw.page=0;loadBandwidth();});
segBind('up-dim','data-d',function(d){state.dimension=d;state.page=0;loadDownloads();});
segBind('up-dim2','data-d',function(d){state.bw.dimension=d;state.bw.page=0;loadBandwidth();});
segBind('up-region-range','data-r',function(r){state.regionRange=r;loadRegions();});
segBind('up-ext-range','data-r',function(r){state.extRange=r;loadExternal();});
segBind('fb-range','data-r',function(r){state.fbRange=r;loadFeedback();});
segBind('fb-platform','data-p',function(p){state.fbPlatform=p;loadFeedback();});
document.getElementById('up-platform').onchange=function(){state.platform=this.value;state.page=0;loadDownloads();};
document.getElementById('up-platform2').onchange=function(){state.bw.platform=this.value;state.bw.page=0;loadBandwidth();};
document.getElementById('up-search').onclick=function(){state.q=document.getElementById('up-q').value.trim();state.page=0;loadDownloads();};
document.getElementById('up-search2').onclick=function(){state.bw.q=document.getElementById('up-q2').value.trim();state.bw.page=0;loadBandwidth();};
document.getElementById('up-q').addEventListener('keydown',function(e){if(e.key==='Enter')document.getElementById('up-search').click();});
document.getElementById('up-q2').addEventListener('keydown',function(e){if(e.key==='Enter')document.getElementById('up-search2').click();});
document.getElementById('up-dl-prev').onclick=function(){if(state.page>0){state.page--;loadDownloads();}};
document.getElementById('up-dl-next').onclick=function(){state.page++;loadDownloads();};
document.getElementById('up-bw-prev').onclick=function(){if(state.bw.page>0){state.bw.page--;loadBandwidth();}};
document.getElementById('up-bw-next').onclick=function(){state.bw.page++;loadBandwidth();};
document.getElementById('bl-add').onclick=async function(){
  var kind=document.getElementById('bl-kind').value,value=document.getElementById('bl-value').value.trim(),
      note=document.getElementById('bl-note').value.trim();
  if(!value){toast('请填写 IP / 网段 / 设备 ID');return;}
  try{await api('/admin/api/blacklist',{method:'POST',body:{kind:kind,value:value,note:note}});
    toast('已加入黑名单');document.getElementById('bl-value').value='';document.getElementById('bl-note').value='';loadBlacklist();}
  catch(e){toast(e.message);}};
document.getElementById('pw-save').onclick=async function(){
  var oldp=document.getElementById('pw-old').value,np=document.getElementById('pw-new').value;
  if(!oldp||!np){toast('请填写当前密码与新密码');return;}
  try{await api('/admin/api/password',{method:'POST',body:{current:oldp,next:np}});
    toast('密码已更新');document.getElementById('pw-old').value='';document.getElementById('pw-new').value='';}
  catch(e){toast(e.message);}};
setTimeout(function(){setView(location.hash.slice(1)||'overview');},0);
setInterval(function(){if((location.hash.slice(1)||'overview')==='overview')loadOverview();},15000);
</script></body></html>"""


def render_login(csrf: str, error: str = "") -> str:
    message = '<div class="err">%s</div>' % error if error else ""
    return LOGIN_PAGE.replace("__CSRF__", csrf).replace("__ERROR__", message)


def render_dashboard(csrf: str, user: str, version: str) -> str:
    initial = (user or "?")[:1].upper()
    return (DASHBOARD.replace("__CSRF__", csrf)
            .replace("__USER__", user)
            .replace("__INITIAL__", initial)
            .replace("__VERSION__", version))
