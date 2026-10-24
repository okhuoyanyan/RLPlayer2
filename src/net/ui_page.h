// ui_page.h — RLPlayer 界面骨架（单页应用外壳）
//
// 设计取向：Roon 式「左导航 + 主内容 + 右队列 + 底部播放条」。
// 与 HQPlayer 的差别：以【专辑】为一等公民，而不是让用户翻目录树；搜索是全局的。
//
// 主题：全部颜色走 CSS 变量，html[data-theme] 切换三套主题（深色 / 白色 / 浅蓝）。
// 规律：组件样式里【不允许再出现硬编码颜色】，否则换主题时必然漏掉某块 ——
// 这是本轮加主题时定的规矩。
#pragma once
#include <string>

namespace rta::net {

inline std::string rta_ui_page() {
    std::string h;
    h += "<!doctype html><html lang=zh data-theme=dark><head><meta charset=utf-8>";
    h += "<meta name=viewport content='width=device-width,initial-scale=1'>";
    h += "<title>RLPlayer</title>";
    // theme-color 决定 Edge「应用窗口」标题栏/边框的颜色；不设就是系统白，跟深色本体割裂。
    h += "<meta name=theme-color content='#0c0e13'>";
    h += "<link rel=icon type='image/png' href='/favicon.png'>";
    h += "<style>";
    // ---------- 主题变量 ----------
    h += ":root{color-scheme:dark;"
         "--bg:#08090c;--bg2:#0c0e13;--bg3:#11141b;"
         "--topbg:linear-gradient(180deg,#12151c,#0b0d11);"
         "--barbg:linear-gradient(180deg,#11141b,#0a0c10);"
         "--line:rgba(255,255,255,.075);"
         "--hov:rgba(255,255,255,.055);--hov2:rgba(255,255,255,.11);"
         "--field:rgba(6,8,11,.9);--crumb:rgba(8,9,12,.94);"
         "--txt:#e9edf5;--txt2:#c3ccdb;--mut:#828ea3;--mut2:#5b6478;"
         "--acc:#3b82f6;--acc2:#8b5cf6;--acc-weak:rgba(59,130,246,.16);--acc-text:#9dc0ff;"
         "--nav-on:linear-gradient(90deg,rgba(59,130,246,.24),rgba(59,130,246,.04));--nav-on-text:#cfe0ff;"
         "--cvbg:#151a24;--phbg:linear-gradient(135deg,#1a2233,#12161f);--ph-txt:rgba(255,255,255,.14);"
         "--shadow:rgba(0,0,0,.45);--track:rgba(255,255,255,.09);--toast:rgba(22,28,38,.97);"
         "--ok:#34d399;--ok-bg:rgba(52,211,153,.14);--ok-line:rgba(52,211,153,.3);--ok-text:#6ee7b7;"
         "--warn:#fbbf24;--danger:#e8654b;--bp-direct:#a78bfa;"
         "}";
    // 白色主题
    h += "html[data-theme=light]{color-scheme:light;"
         "--bg:#f4f6f9;--bg2:#ffffff;--bg3:#eef1f6;"
         "--topbg:linear-gradient(180deg,#ffffff,#f3f5f9);"
         "--barbg:linear-gradient(180deg,#ffffff,#f0f3f8);"
         "--line:rgba(15,23,42,.10);"
         "--hov:rgba(15,23,42,.05);--hov2:rgba(15,23,42,.09);"
         "--field:#ffffff;--crumb:rgba(244,246,249,.94);"
         "--txt:#0f172a;--txt2:#334155;--mut:#5a6b82;--mut2:#8794a8;"
         "--acc:#2563eb;--acc2:#7c3aed;--acc-weak:rgba(37,99,235,.12);--acc-text:#1d4ed8;"
         "--nav-on:linear-gradient(90deg,rgba(37,99,235,.13),rgba(37,99,235,.02));--nav-on-text:#1e40af;"
         "--cvbg:#e6ecf5;--phbg:linear-gradient(135deg,#e6ecf6,#dae2ee);--ph-txt:rgba(15,23,42,.20);"
         "--shadow:rgba(15,23,42,.13);--track:rgba(15,23,42,.10);--toast:rgba(255,255,255,.98);"
         "--ok:#059669;--ok-bg:rgba(5,150,105,.10);--ok-line:rgba(5,150,105,.25);--ok-text:#047857;"
         "--warn:#b45309;--danger:#dc2626;--bp-direct:#6d28d9;"
         "}";
    // 浅蓝主题
    h += "html[data-theme=blue]{color-scheme:light;"
         "--bg:#e9f2ff;--bg2:#f7fbff;--bg3:#dcebfe;"
         "--topbg:linear-gradient(180deg,#ffffff,#e9f4ff);"
         "--barbg:linear-gradient(180deg,#f7fbff,#e4f0ff);"
         "--line:rgba(12,60,120,.13);"
         "--hov:rgba(12,60,120,.06);--hov2:rgba(12,60,120,.11);"
         "--field:#ffffff;--crumb:rgba(233,242,255,.94);"
         "--txt:#0b2545;--txt2:#1e3a5f;--mut:#4a6b93;--mut2:#7d97b8;"
         "--acc:#0d6efd;--acc2:#3b9bff;--acc-weak:rgba(13,110,253,.13);--acc-text:#0b5ed7;"
         "--nav-on:linear-gradient(90deg,rgba(13,110,253,.16),rgba(13,110,253,.02));--nav-on-text:#0a58ca;"
         "--cvbg:#d9e9ff;--phbg:linear-gradient(135deg,#dbeafe,#cde2ff);--ph-txt:rgba(11,37,69,.20);"
         "--shadow:rgba(11,37,69,.15);--track:rgba(11,37,69,.10);--toast:rgba(255,255,255,.98);"
         "--ok:#0f766e;--ok-bg:rgba(15,118,110,.10);--ok-line:rgba(15,118,110,.25);--ok-text:#0b5f59;"
         "--warn:#b45309;--danger:#c2410c;--bp-direct:#4338ca;"
         "}";
    h += "*{box-sizing:border-box;margin:0;padding:0}html,body{height:100%}";
    h += "body{font:13px/1.5 Inter,'Segoe UI Variable','Segoe UI',system-ui,sans-serif;background:var(--bg);color:var(--txt);overflow:hidden;-webkit-font-smoothing:antialiased}";
    h += "button{font:inherit;color:inherit;background:none;border:0;cursor:pointer}input,select{font:inherit;color:inherit}";
    h += "a{color:inherit;text-decoration:none}";
    // 2026-10-23 用户 261023-04 #8：深色主题下曲目页「默认排序」下拉的**菜单列表**看不清。
    // 原因：<select> 本体有 color:var(--txt)（浅色字），但 <option> 没有显式 background —— 弹层
    // 在某些宿主（WebView2 / 高对比主题）按系统浅底渲染，于是浅字压浅底。
    // 修法：全局给 select / option 都定死 color+background（三套主题都走 CSS 变量），
    //       不依赖浏览器对 <option> 的默认配色。:checked 项再给强调色，键盘/鼠标都看得清。
    h += "select{color:var(--txt);background-color:var(--field)}";
    h += "select option{background-color:var(--bg2);color:var(--txt)}";
    // 选中项不写死「白字 + 强调蓝」（#3b82f6 底上白字对比度只有 3.68:1）；交给系统高亮，
    // 其余选项统一 bg2/txt，深色 16:1 / 浅色 14:1，菜单怎么弹都读得清。
    h += "select option:checked{background-color:var(--acc-weak);color:var(--acc-text)}";
    h += "select option:disabled{color:var(--mut2)}";
    h += "#app{display:grid;grid-template-columns:212px 1fr 378px;grid-template-rows:52px 1fr 88px;height:100vh}";
    h += "#app.noq{grid-template-columns:212px 1fr 0}#app.noq #right{display:none}";
    // 2026-10-23（UI 评审 #1/#3/#9/#18）：窄窗口（≤1240px）右侧队列/歌词抽屉会吃掉 378px，
    // 把主区挤到 434px → 曲目名被压成 0 宽、专辑网格只剩 2 列。窄窗默认收起抽屉，主区优先。
    h += "@media (max-width:1240px){#app{grid-template-columns:200px minmax(0,1fr) 0}#app #right{display:none}}";
    // 窄窗口下曲目行按「歌名 > 艺术家 > 规格」优先级收列，绝不把歌名压成 0 宽（评审 #3/#18 的根因兜底）
    h += ".trow-track .ar,.trow-track .bd{overflow:hidden}";
    h += "@media (max-width:1400px){.trow-track{grid-template-columns:28px 24px minmax(120px,1.8fr) minmax(0,1fr) 0px 58px!important}}";
    h += "@media (max-width:1120px){.trow-track{grid-template-columns:28px 24px minmax(0,1fr) minmax(0,.9fr) 0px 58px!important}}";
    
    // 顶栏
    // 顶栏与内容同底色、无分隔线：整块界面连成一片（QQ 音乐式一体化）。
    h += ".top{grid-column:1/-1;display:flex;align-items:center;gap:10px;padding:0 10px 0 16px;background:var(--bg);border-bottom:0;position:relative;z-index:9;min-width:0}";
    h += ".hsep{width:1px;height:18px;background:var(--line);margin:0 2px;flex:none}";
    h += ".ptitle{font-weight:650;font-size:14px;color:var(--txt);white-space:nowrap;overflow:hidden;text-overflow:ellipsis;max-width:220px;flex:none}";
    h += ".pcnt{font-size:11px;color:var(--mut2);white-space:nowrap;flex:none}";
    h += ".ptools{display:flex;align-items:center;gap:4px;flex:none}";
    h += ".wctl{display:none;align-items:center;margin-left:2px;margin-right:-10px;flex:none}";
    h += "html.webview2 .wctl{display:flex}";
    h += ".wbtn{width:44px;height:52px;border-radius:0;display:grid;place-items:center;color:var(--mut);transition:.12s}";
    h += ".wbtn:hover{background:var(--hov);color:var(--txt)}";
    h += ".wbtn.close:hover{background:#e81123;color:#fff}";
    h += ".wbtn svg{width:12px;height:12px}";
    // ---------- 261024-04 #1：窗口边缘拉伸热区（页面发起 resize） ----------
    // 背景：WebView2 的 3 个子窗口属于别的进程，宿主无法 subclass，鼠标事件全被它们吃掉 ⟹
    // 原生的边缘 hit-test 收不到鼠标。改成「页面发起 resize」：页面在最外 5px 铺 8 条不可见热区，
    // mousedown 时 postMessage('resize:<方向>')，宿主（launcher.cpp）据此走 WM_NCLBUTTONDOWN。
    // 这里只负责**光标形状**与命中区域；真正的判定在 ui_script 的 document 捕获阶段按坐标做
    //（这样 CDP 的合成 mousedown 派给 document 也能触发，与真实鼠标行为一致）。
    // z-index 取最大：必须盖在弹层 / 单曲页 / 设置页之上，否则在那些页面上又拖不动窗口。
    h += "html.webview2 #rzHot{position:fixed;inset:0;z-index:2147483000;pointer-events:none}";
    h += "html.webview2 #rzHot i{position:fixed;display:block;pointer-events:auto;background:transparent}";
    h += "html.webview2 #rzHot .rz-l{left:0;top:0;bottom:0;width:5px;cursor:ew-resize}";
    h += "html.webview2 #rzHot .rz-r{right:0;top:0;bottom:0;width:5px;cursor:ew-resize}";
    h += "html.webview2 #rzHot .rz-t{left:0;right:0;top:0;height:5px;cursor:ns-resize}";
    h += "html.webview2 #rzHot .rz-b{left:0;right:0;bottom:0;height:5px;cursor:ns-resize}";
    h += "html.webview2 #rzHot .rz-tl{left:0;top:0;width:10px;height:10px;cursor:nwse-resize}";
    h += "html.webview2 #rzHot .rz-tr{right:0;top:0;width:10px;height:10px;cursor:nesw-resize}";
    h += "html.webview2 #rzHot .rz-bl{left:0;bottom:0;width:10px;height:10px;cursor:nesw-resize}";
    h += "html.webview2 #rzHot .rz-br{right:0;bottom:0;width:10px;height:10px;cursor:nwse-resize}";
    h += ".logo{width:27px;height:27px;border-radius:8px;background:linear-gradient(135deg,var(--acc),var(--acc2));display:grid;place-items:center;font-weight:800;font-size:13px;color:#fff;box-shadow:0 2px 14px var(--acc-weak);flex:none}";
    h += ".ttl{font-weight:650;letter-spacing:.2px;flex:none}.ttl small{display:block;font-size:9px;font-weight:500;color:var(--mut2);letter-spacing:1.3px;margin-top:-3px}";
    h += ".search{flex:1;max-width:460px;margin-left:14px;position:relative}";
    h += ".search input{width:100%;padding:8px 12px 8px 32px;border-radius:9px;border:1px solid var(--line);background:var(--field);transition:.15s;font-size:12.5px}";
    h += ".search input:focus{outline:0;border-color:var(--acc);box-shadow:0 0 0 3px var(--acc-weak)}";
    h += ".search svg{position:absolute;left:10px;top:9px;opacity:.4}";
    h += ".sp{flex:1}.ibtn{width:32px;height:32px;border-radius:8px;display:grid;place-items:center;color:var(--mut);transition:.15s}";
    h += ".ibtn:hover{background:var(--hov2);color:var(--txt)}";
    // 主题切换
    h += ".themes{display:flex;gap:6px;align-items:center;margin-right:4px}";
    h += ".sw{width:18px;height:18px;border-radius:50%;border:1px solid var(--line);cursor:pointer;transition:.15s;position:relative}";
    h += ".sw:hover{transform:scale(1.12)}";
    h += ".sw.on{box-shadow:0 0 0 2px var(--acc)}";
    h += ".sw[data-theme=dark]{background:linear-gradient(135deg,#1b2130,#0a0b0e)}";
    h += ".sw[data-theme=light]{background:linear-gradient(135deg,#ffffff,#e6e9ef)}";
    h += ".sw[data-theme=blue]{background:linear-gradient(135deg,#e9f2ff,#bcd9ff)}";
    // 侧栏
    h += ".side{background:var(--bg2);border-right:1px solid var(--line);padding:10px 8px;overflow:auto;display:flex;flex-direction:column}";
    h += ".side .sp{flex:1;min-height:12px}";
    h += ".side .nav svg{width:16px;height:16px;flex:none}";
    h += ".nav{display:flex;align-items:center;gap:10px;padding:9px 12px;border-radius:9px;color:var(--mut);cursor:pointer;font-weight:500;transition:.13s;white-space:nowrap;overflow:hidden;font-size:12.5px}";
    h += ".nav:hover{background:var(--hov);color:var(--txt)}";
    h += ".nav.on{background:var(--nav-on);color:var(--nav-on-text)}";
    h += ".nav svg{flex:none}.sec{margin:16px 12px 6px;font-size:10px;letter-spacing:1.2px;color:var(--mut2);font-weight:600}";
    // 主区
    h += "#app.setmode .crumb{display:none}";
    h += ".main{overflow:auto;position:relative}";
    h += ".main>.pad{padding:18px}";
    h += ".btn{padding:6px 13px;border-radius:8px;background:var(--hov);border:1px solid var(--line);font-size:12px;transition:.14s;white-space:nowrap}";
    h += ".btn:hover{background:var(--hov2)}";
    // 禁用态只动透明度与指针 —— 三个主题下都不需要额外配色（标签写回批任务进行中用）
    h += ".btn:disabled{opacity:.45;pointer-events:none}";
    h += ".btn.pri{background:linear-gradient(180deg,var(--acc),var(--acc2));border-color:transparent;color:#fff;font-weight:600;box-shadow:0 2px 12px var(--acc-weak)}";
    h += ".btn.pri:hover{filter:brightness(1.08)}";
    // 专辑网格
    h += ".grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(152px,1fr));gap:16px;padding:18px}";
    h += ".card{cursor:pointer;transition:.16s;border-radius:11px;padding:7px;margin:-7px}";
    h += ".card:hover{background:var(--hov)}";
    h += ".card .cv{aspect-ratio:1;border-radius:9px;background:var(--cvbg) center/cover no-repeat;border:1px solid var(--line);position:relative;overflow:hidden;box-shadow:0 4px 16px var(--shadow)}";
    h += ".card .cv .ph{position:absolute;inset:0;display:grid;place-items:center;font-size:30px;font-weight:800;color:var(--ph-txt);background:var(--phbg)}";
    h += ".card .cv img{position:absolute;inset:0;width:100%;height:100%;object-fit:cover;display:block}";
    h += ".card .cv .pl{position:absolute;right:7px;bottom:7px;width:32px;height:32px;border-radius:50%;background:var(--acc);display:grid;place-items:center;opacity:0;transform:translateY(5px);transition:.16s;box-shadow:0 3px 12px var(--shadow)}";
    h += ".card:hover .cv .pl{opacity:1;transform:none}";
    h += ".card .nm{font-size:12.5px;font-weight:600;margin-top:9px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}";
    h += ".card .ar{font-size:11px;color:var(--mut2);white-space:nowrap;overflow:hidden;text-overflow:ellipsis;margin-top:2px}";
    // 专辑头
    h += ".ahead{display:flex;gap:20px;padding:22px 22px 16px;align-items:flex-end}";
    h += ".ahead .cv{width:168px;height:168px;border-radius:12px;background:var(--cvbg) center/cover no-repeat;border:1px solid var(--line);flex:none;box-shadow:0 10px 34px var(--shadow)}";
    h += ".ahead .info{min-width:0;padding-bottom:5px}";
    // 无封面艺术家的首字母占位必须居中在圆形/方形头像内（261024-01 顺带修：以前字母跑到圆外左上角）
    h += ".ahead .cv{position:relative;overflow:hidden}";
    h += ".ahead .cv .ph{position:absolute;inset:0;display:grid;place-items:center;font-size:46px;font-weight:800;color:var(--ph-txt);background:var(--phbg)}";
    h += ".ahead .kind{font-size:10px;letter-spacing:1.4px;color:var(--mut2);font-weight:600}";
    h += ".ahead h1{font-size:26px;line-height:1.2;font-weight:750;letter-spacing:-.4px;margin:5px 0 7px;word-break:break-word}";
        h += ".ahead .sub{font-size:12.5px;color:var(--mut);margin-bottom:13px}";
    h += ".bio-box{font-size:13px;line-height:1.75;color:var(--txt2);white-space:pre-wrap;word-break:break-word}";
    h += ".bio-box.collapsed{max-height:140px;overflow:hidden;-webkit-mask-image:linear-gradient(180deg,#000 65%,transparent)}";
    // 261024-04 #4b：资料块（AI 整理 / 简介 / 风格 / 来源）原来**没有任何 CSS**（.sec-card 只在标记里出现），
    // 于是专辑页的正文左边界几乎贴到卡片边（实测 0px）。这里补齐卡片外观 + 与设置页 .sgbody 同量级的内边距
    // （设置页卡片 = .sgroup 边框 + .sgbody padding 14px 16px）。左内边距取 18px，判据要求 ≥16px。
    h += ".sec-card{padding:14px 18px;border:1px solid var(--line);border-radius:12px;background:var(--bg2);min-width:0}";
    h += ".sec-card .sec-title{font-size:12px;font-weight:600;letter-spacing:1px;color:var(--mut2);margin-bottom:8px}";
    // 资料整理失败：只留一行「可点击重试」，绝不 toast（261024-04 #7）
    h += ".summ-retry{display:inline-flex;align-items:center;gap:6px;font-size:11.5px;color:var(--mut);cursor:pointer;padding:3px 0}";
    h += ".summ-retry:hover{color:var(--acc-text)}";
    // 专辑页顶部「后台整理曲目资料」的细进度条（不打扰：2px 线 + 一行小字，完成后自己消失）
    h += ".albprog{position:sticky;top:0;z-index:5;display:flex;align-items:center;gap:10px;padding:6px 22px;background:var(--bg);border-bottom:1px solid var(--line)}";
    h += ".albprog .t{font-size:11px;color:var(--mut2);white-space:nowrap}";
    h += ".albprog .bar{flex:1;height:2px;border-radius:2px;background:var(--track);overflow:hidden}";
    h += ".albprog .bar i{display:block;height:100%;width:0;background:var(--acc);transition:width .25s ease}";
    h += ".chip-copy{display:inline-flex;align-items:center;gap:4px;font-size:11.5px;padding:4px 10px;border-radius:6px;background:var(--hov);border:1px solid var(--line);color:var(--txt2);cursor:pointer;transition:.14s;white-space:nowrap}";
    h += ".chip-copy:hover{background:var(--hov2);color:var(--txt)}";
    // 曲目行
    h += ".trow{display:grid;grid-template-columns:34px 1fr 88px 58px;gap:11px;align-items:center;padding:8px 20px;cursor:pointer;border-radius:7px;margin:0 10px;font-size:12.5px}";
    h += ".trow:hover{background:var(--hov)}";
    h += ".trow.on{background:var(--acc-weak)}";
    h += ".trow .n{color:var(--mut2);text-align:right;font-variant-numeric:tabular-nums;font-size:11.5px}";
    h += ".trow .tt{min-width:0;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}";
    h += ".trow .ar{color:var(--mut2);min-width:0;white-space:nowrap;overflow:hidden;text-overflow:ellipsis;font-size:11.5px}";
    h += ".trow .du{color:var(--mut2);text-align:right;font-variant-numeric:tabular-nums;font-size:11.5px}";
    h += ".trow .bd{font-size:9.5px;color:var(--acc-text);text-align:right;letter-spacing:.3px}";
    // 通用列表
    h += ".row{display:flex;align-items:center;gap:11px;padding:9px 18px;cursor:pointer;transition:background .1s;border-bottom:1px solid var(--line)}";
    h += ".row:hover{background:var(--hov)}.row .ico{width:20px;text-align:center;flex:none;opacity:.75}";
    h += ".row .nm{flex:1;min-width:0;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}.row .meta{font-size:11px;color:var(--mut2);flex:none}";
    h += ".rowdir .nm{color:var(--acc-text)}";
    h += ".empty{padding:44px 20px;text-align:center;color:var(--mut2);font-size:12.5px;line-height:2}";
    // 文件夹：地址栏
    h += ".pathbar{display:flex;gap:8px;align-items:center;padding:12px 18px;border-bottom:1px solid var(--line);position:sticky;top:0;background:var(--bg);z-index:4}";
    h += ".pb{width:30px;height:30px;border-radius:8px;border:1px solid var(--line);background:var(--hov);font-size:13px;flex:none}";
    h += ".pb:hover{background:var(--hov2)}";
    h += ".pin{flex:1;min-width:0;padding:7px 11px;border-radius:8px;border:1px solid var(--line);background:var(--field);font-size:12px;font-family:ui-monospace,Consolas,monospace}";
    h += ".pin:focus{outline:0;border-color:var(--acc);box-shadow:0 0 0 3px var(--acc-weak)}";
    h += ".psel{max-width:200px;padding:7px 9px;border-radius:8px;border:1px solid var(--line);background:var(--field);font-size:12px}";
    h += ".frows{padding-bottom:20px}";
    h += ".row .nm.dir{color:var(--acc-text);font-weight:550}";
    // 播放队列主视图
    h += ".qlist2{padding:4px 0 24px}";
    h += ".qrow{display:flex;align-items:center;gap:12px;padding:9px 18px;cursor:pointer;border-bottom:1px solid var(--line);transition:background .1s}";
    h += ".qrow:hover{background:var(--hov)}";
    h += ".qrow.on{background:var(--acc-weak)}.qrow.on .qt{color:var(--acc-text);font-weight:650}";
    h += ".qrow .qn{width:26px;text-align:right;font-size:11px;color:var(--mut2);flex:none;font-variant-numeric:tabular-nums}";
    h += ".qrow .qt{flex:1;min-width:0;white-space:nowrap;overflow:hidden;text-overflow:ellipsis;font-size:12.5px}";
    h += ".qrow .qp{font-size:11px;color:var(--mut2);flex:none;max-width:38%;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}";
    h += ".qrow .qdel{width:24px;height:24px;border-radius:6px;color:var(--mut2);flex:none;opacity:0;transition:.12s}";
    h += ".qrow:hover .qdel{opacity:1}.qrow .qdel:hover{background:var(--hov2);color:var(--danger)}";
    // 拖拽提示
    h += "#dropmask{position:fixed;inset:0;z-index:90;display:none;align-items:center;justify-content:center;background:rgba(8,9,12,.72);backdrop-filter:blur(2px)}";
    h += "#dropmask.on{display:flex}";
    h += "#dropmask>div{border:2px dashed var(--acc);border-radius:16px;padding:42px 64px;font-size:16px;color:var(--acc-text);background:rgba(59,130,246,.08)}";
    // 右侧
    h += "#right{background:var(--bg2);border-left:1px solid var(--line);display:flex;flex-direction:column;overflow:hidden}";
    h += ".rhead{display:flex;align-items:center;gap:8px;padding:10px 14px;border-bottom:1px solid var(--line);font-size:12px;font-weight:600;color:var(--txt2);flex:none;min-height:42px}";
    h += ".rhead .cnt{font-weight:400;color:var(--mut2);font-size:11px}.rbody{flex:1;overflow:auto}";
    h += ".qi{display:flex;gap:9px;padding:8px 14px;cursor:pointer;border-left:3px solid transparent;font-size:12.5px;align-items:center}";
    h += ".qi:hover{background:var(--hov)}";
    h += ".qi.on{background:var(--acc-weak);border-left-color:var(--acc);color:var(--acc-text)}";
    h += ".qi .n{color:var(--mut2);width:20px;flex:none;text-align:right;font-variant-numeric:tabular-nums;font-size:11px}";
    h += ".qi .t{flex:1;min-width:0;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}";
    // 右栏快速设置
    h += ".set{padding:14px;display:none}.set.on{display:block}.fld{margin-bottom:14px}";
    h += ".fld label{display:block;font-size:11px;color:var(--mut);margin-bottom:5px;font-weight:500}";
    h += ".fld select,.fld input{width:100%;padding:7px 10px;border-radius:8px;border:1px solid var(--line);background:var(--field);transition:.15s;font-size:12px}";
    h += ".fld select:focus,.fld input:focus{outline:0;border-color:var(--acc)}";
    h += ".chk{display:flex;align-items:center;gap:8px;font-size:12px;color:var(--txt2);cursor:pointer;margin-bottom:11px}.chk input{width:15px;height:15px;accent-color:var(--acc)}";
    h += ".rootline{display:flex;align-items:center;gap:6px;font-size:11.5px;color:var(--mut);padding:5px 0;border-bottom:1px solid var(--line)}";
    h += ".rootline span{flex:1;min-width:0;white-space:nowrap;overflow:hidden;text-overflow:ellipsis;direction:rtl;text-align:left}";
    h += ".rootline button{color:var(--mut2);font-size:15px;line-height:1;padding:0 6px}";
    h += ".rootline button:hover{color:var(--danger)}";
    // 播放条
    // 2026-10-23 用户 261023-04 #1（底栏严重错误）：现场是「控制键被推到最右、中间只有一行黄色非 bit-perfect、
    // 信号链/歌词/音量整组不见了」。根因：底栏是 3 列 grid，但 #bpBadge 作为第 4 个子项插在 .np 与 .ctr 之间，
    // grid 自动排布把 .ctr 放到第 3 列、把 .vol 挤到第 2 行（88px 行高之外 = 看不见）。
    // 修法（判据：控制组中心与底栏中心偏差 < 8px；bLyric/bChain/音量任何宽度都可见）：
    //   ① 三列改为 minmax(0,1fr) | auto | minmax(0,1fr)：左右两列严格等宽 ⟹ 中间控制组水平居中；
    //   ② bit-perfect 状态改为紧凑 chip（#bpBadge），收进右侧状态簇，不再占中间列；
    //   ③ 窄屏压缩音量条/隐藏格式徽标，但歌词、信号链、音量三件套始终保留。
    h += ".bar{grid-column:1/-1;display:grid;grid-template-columns:minmax(0,1fr) auto minmax(0,1fr);align-items:center;gap:14px;padding:0 16px;background:var(--barbg);border-top:1px solid var(--line)}";
    h += ".np{display:flex;align-items:center;gap:11px;min-width:0;max-width:340px;justify-self:start;cursor:pointer;border-radius:10px;padding:4px 6px;margin-left:-6px;transition:.14s}";
    h += ".np:hover{background:var(--hov)}";
    h += ".cv2{width:56px;height:56px;border-radius:9px;flex:none;background:var(--cvbg) center/cover no-repeat;border:1px solid var(--line);box-shadow:0 3px 14px var(--shadow)}";
    h += ".np .tx{min-width:0}.np .t1{font-size:12.5px;font-weight:600;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}";
    h += ".np .t2{font-size:10.5px;color:var(--mut2);white-space:nowrap;overflow:hidden;text-overflow:ellipsis;margin-top:2px}";
    h += ".ctr{display:flex;flex-direction:column;align-items:center;gap:7px;width:clamp(300px,34vw,540px);flex:none}.keys{display:flex;align-items:center;gap:6px}";
    // bit-perfect 状态 chip（#bpBadge）：紧凑小标签，颜色区分三态；空态不占位。
    // 颜色全部走主题变量：--ok-text / --warn / --bp-direct（三套主题各一份，浅色底也够对比度）。
    h += "#bpBadge{display:none;align-items:center;font-size:9.5px;font-weight:700;letter-spacing:.4px;padding:2px 7px;border-radius:5px;border:1px solid var(--line);background:var(--hov);white-space:nowrap;line-height:1.5}";
    h += "#bpBadge.bp-yes{display:inline-flex;color:var(--ok-text);border-color:var(--ok-line);background:var(--ok-bg)}";
    h += "#bpBadge.bp-no{display:inline-flex;color:var(--warn);border-color:currentColor;background:var(--hov)}";
    h += "#bpBadge.bp-direct{display:inline-flex;color:var(--bp-direct);border-color:currentColor;background:var(--hov)}";
    // 窄屏（1024 级）：中间控制组收窄、右侧只压缩音量条与格式徽标 —— 歌词/信号链/音量必须还在。
    h += "@media (max-width:1180px){.bar{gap:10px;padding:0 12px}.ctr{width:300px}.np{max-width:260px}.np .cv2{width:48px;height:48px}.vol{gap:6px}.vol input[type=range]{width:64px}#fmt{display:none}}";
    h += "#bRep{position:relative}";
    // 正在播放整页（2026-10-23 用户 261023-04 #5：仍有黑边框）
    // 根因更正：1.3.8 的「负 inset + padding 补偿」在几何上不可能盖住那条缝 ——
    //   #npPage 是 WebView 内的覆盖层，CSS 只能超出**页面视口**，超不出 WebView 控件本身；
    //   而 launcher 为了让 WM_NCHITTEST 拿到边缘缩放热区，把 WebView 内缩了 kBandInset(5 逻辑像素)，
    //   这条边带由宿主用主题 --bg 上色，WebView 永远盖不到它。不同 DPI 下边带是 5~8 物理像素，
    //   页面边缘却仍是模糊封面的彩色像素 ⟹ 视觉上就是一圈黑框。
    // 现在的修法：让页面**最外圈像素自己等于 --bg**（8px 纯色 + 64px 柔和内阴影形成暗角），
    //   宿主边带与 --bg 逐字节同色（launcher.cpp 的 kBgDark/kBgLight/kBgBlue 与 ui_page 的 --bg 一致），
    //   接缝两侧颜色差 ≈ 0，任何 DPI/窗口尺寸都看不出来；缩放热区仍由宿主边带保留，不受影响。
    h += "#npPage{position:fixed;inset:0;z-index:70;display:none;background:var(--bg);overflow:hidden}";
    h += "#npPage.on{display:block}";
    // 边带同色暗角：最外 8px 纯 --bg（覆盖宿主 5~8 物理像素的边带），再向内 64px 柔和过渡。
    // 用「8px 实心边框 + 内阴影」而不是两层 box-shadow：边框像素是纯 --bg（逐像素实测 08090C），
    // 保证宿主 5px 边带与页面外圈逐字节同色；内阴影从边框内侧开始做暗角渐隐。
    // 2026-10-24（用户实机：这条"同色外圈 + 暗角"反而变成明显的黑边过渡）：
// 宿主已改为 WebView 满幅（launcher kBandInset=0），页面不需要再遮任何边带 → 整块删除。
h += "#npPage .npedge{display:none}";
    // 拖拽条：贴顶 34px，透明，不挡下方内容（z-index 高于背景、低于关闭按钮）
    h += "#npPage .npgrip{position:absolute;top:0;left:0;right:0;height:34px;z-index:5;cursor:grab;background:transparent}";
    h += "#npPage .npgrip:active{cursor:grabbing}";
    h += ".npbg{position:absolute;inset:-60px;background-size:cover;background-position:center;filter:blur(64px) saturate(1.15) brightness(.38);transform:scale(1.08)}";
    h += ".npveil{position:absolute;inset:0;background:linear-gradient(180deg,rgba(8,9,12,.35),rgba(8,9,12,.82))}";
    // 三段式网格（261024-01 #3b，用户原话「丑的令人震惊」）：
    //   行1 封面+标题 / 行2 歌词（左）+ 资料标签栏（右，默认只露标签）/ 行3 控制区。
    //   资料内容改为右侧抽屉，默认完全不出现在版面上；歌词与资料永远**左右相邻**，绝不上下堆叠。
    //   旧版四行（资料整行 + 歌词整行）已删除 —— 那正是用户说的"上下排列丑到不忍直视"。
    // 内容列限宽并居中：hero / 歌词+标签栏 / 控制区 共用同一条左右边界（对齐判据靠这个，而不是靠肉眼）
    h += ".npgrid{position:relative;z-index:2;height:100%;display:grid;grid-template-rows:auto minmax(0,1fr) auto;gap:18px;padding:44px 40px 18px;box-sizing:border-box;max-width:1240px;margin:0 auto}";
    h += ".npgrid.nolyric #npLyric{display:none}";
    // 行2：左歌词 + 右标签栏（标签栏无可用资料时整列收起，歌词自动吃满）
    h += ".npbody{display:grid;grid-template-columns:minmax(0,1fr) auto;gap:22px;min-height:0;align-items:stretch}";
    // 261024-05 A12：AI 关闭时「专辑」标签也要常驻（本地曲目）——无歌词时歌词列 display:none，
    // 药丸卡会被当成第一列拉伸成整行（截图可见），这里显式钉在右列并靠右，保持与有歌词时同一位置。
    h += ".nprail{display:none;min-height:0;align-self:start;grid-column:2;grid-row:1;justify-self:end}";
    h += ".nprail.on{display:block}";
    // 资料卡：右侧一张紧凑浮卡（caption + 三个标签），比"贯穿全高的分隔线 + 飘着的药丸"更像正式组件
    h += ".nprailcard{display:flex;flex-direction:column;gap:5px;padding:9px;border-radius:14px;background:rgba(255,255,255,.045);border:1px solid rgba(255,255,255,.08);backdrop-filter:blur(8px);min-width:104px}";
    h += ".nprailcap{font-size:9.5px;letter-spacing:1.5px;color:var(--mut2);font-weight:700;padding:0 4px 3px}";
    h += ".npchipwrap{display:flex;flex-direction:column;align-items:stretch;gap:4px;min-height:0}";
    // 标签（歌曲/专辑/艺术家/作品）：默认只露这一列药丸，点开才从右侧滑出抽屉。
    h += ".npchip{display:flex;align-items:center;gap:8px;justify-content:space-between;padding:7px 10px;border-radius:9px;font-size:12px;color:var(--mut);background:transparent;border:1px solid transparent;cursor:pointer;transition:.14s;white-space:nowrap;line-height:1.5;text-align:left}";
    h += ".npchip:hover{color:var(--txt);background:rgba(255,255,255,.09)}";
    h += ".npchip.on{color:var(--acc-text);background:var(--acc-weak);border-color:var(--acc-weak);font-weight:600}";
    // 261024-02 #5：单曲页「推送到设备」按钮（和元数据芯片同一行；投放中高亮）
    h += ".npcast{display:inline-flex;align-items:center;gap:0;font-size:11.5px;padding:4px 10px;border-radius:12px;background:var(--bg3);color:var(--txt2);border:1px solid var(--line);cursor:pointer;transition:.15s;white-space:nowrap}";
    h += ".npcast:hover{background:var(--hov);color:var(--txt);border-color:var(--acc)}";
    h += ".npcast.on{background:var(--ok-bg);color:var(--ok-text);border-color:var(--ok-line);font-weight:600}";
    h += "#castBadge{display:none;align-items:center;color:var(--ok-text);border-color:var(--ok-line);background:var(--ok-bg);cursor:default}";
    // 设备选择浮层
    h += ".castpick{display:flex;flex-direction:column;gap:6px}";
    h += ".castpick-row{display:flex;flex-direction:column;align-items:flex-start;gap:2px;text-align:left;padding:10px 12px;border-radius:10px;border:1px solid var(--line);background:var(--bg3);cursor:pointer;transition:.14s;width:100%}";
    h += ".castpick-row:hover{border-color:var(--acc);background:var(--hov)}";
    h += ".castpick-row .cpn{font-size:13px;font-weight:600;color:var(--txt)}";
    h += ".castpick-row .cph{font-size:11px;color:var(--mut2)}";
    // 261024-04 #7：自动加载的一行状态（后台整理中 / 失败可点击重试）
    h += ".nprailhint{font-size:10.5px;line-height:1.5;color:var(--mut2);padding:3px 2px 0}";
    h += ".nprailhint .nprailerr{color:var(--warn);cursor:pointer}";
    h += ".nprailhint .nprailerr:hover{text-decoration:underline}";
    h += ".nprailhint .nprailbusy{color:var(--mut2)}";
    h += ".npchip .dot{width:5px;height:5px;border-radius:50%;background:currentColor;opacity:.4;flex:none;transition:.2s}";
    h += ".npchip.ok .dot{background:var(--ok);opacity:1}";
    h += ".npchip.busy .dot{animation:npdot 1.1s ease-in-out infinite}";
    h += ".npchip.err .dot{opacity:.9;background:var(--warn)}";
    h += "@keyframes npdot{0%,100%{opacity:.25}50%{opacity:1}}";
    h += "@media (prefers-reduced-motion:reduce){.npchip.busy .dot{animation:none}}";
    // 资料抽屉（默认隐藏，transform 滑入 / 滑出；不改变版面尺寸 ⟹ 歌词不会被挤压跳动）
    h += ".npdrawerbg{position:absolute;inset:0;z-index:5;background:rgba(0,0,0,.34);opacity:0;pointer-events:none;transition:opacity .2s}";
    h += ".npdrawerbg.on{opacity:1;pointer-events:auto}";
    h += ".npdrawer{position:absolute;top:0;right:0;bottom:0;z-index:6;width:min(460px,46vw);display:flex;flex-direction:column;background:var(--bg2);border-left:1px solid var(--line);box-shadow:-28px 0 70px rgba(0,0,0,.5);transform:translateX(102%);visibility:hidden;transition:transform .22s cubic-bezier(.4,0,.2,1),visibility .22s}";
    h += ".npdrawer.on{transform:none;visibility:visible}";
    h += ".npdhead{display:flex;align-items:center;gap:9px;padding:16px 16px 12px;border-bottom:1px solid var(--line);flex:none;min-width:0}";
    h += ".npdkicker{font-size:9.5px;letter-spacing:1.2px;font-weight:700;color:var(--acc-text);background:var(--acc-weak);border-radius:5px;padding:2px 6px;flex:none}";
    h += ".npdname{font-size:13.5px;font-weight:650;color:var(--txt);min-width:0;flex:1 1 auto;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}";
    h += ".npdclose{width:26px;height:26px;border-radius:50%;display:grid;place-items:center;color:var(--mut);flex:none;transition:.13s}";
    h += ".npdclose:hover{background:var(--hov2);color:var(--txt)}";
    h += ".nphero{display:grid;grid-template-columns:auto minmax(0,1fr);gap:26px;align-items:center;min-height:0}";
    h += ".npcover{height:clamp(132px,24vh,236px)}";
    h += ".npcover img{height:100%;width:auto;aspect-ratio:1/1;object-fit:cover;border-radius:14px;box-shadow:0 24px 70px rgba(0,0,0,.6);display:block}";
    h += ".npinfo{min-width:0}";
    h += ".npkind{font-size:10px;letter-spacing:1.6px;color:var(--mut2);font-weight:600;margin-bottom:8px}";
    h += ".nptitleline{display:flex;align-items:center;gap:10px;min-width:0}";
    h += ".nptit{font-size:clamp(19px,2.6vh,27px);font-weight:700;line-height:1.3;min-width:0;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}";
    h += ".npsub{font-size:13px;color:var(--mut);margin-top:8px;line-height:1.6;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}";
    h += ".npmline{display:flex;align-items:center;gap:6px;flex-wrap:wrap;margin-top:10px;min-height:22px}";
    // 时长行的 "/" 必须与下面播放键的中心严格对齐（用户 2026-10-22 UI3）。
    // 为什么「整行居中」不够：播放键两侧按钮数不同（左 2 右 2 但按钮宽度不同），
    // 更早的版本整行 flex 居中时播放键本身并不在容器正中 —— 那正是用户看到
    // 「/ 没有跟播放键对齐」的原因（第十二轮改成"居中"只解决了整行，没解决这一层）。
    // 现在的做法：时长行与播放键行【共用同一条 3 列网格 + 同一个宽度】，
    //   列 = 1fr | auto | 1fr。
    //   左右 1fr 由网格保证严格等宽 ⟹ 中间列的水平中心 == 容器水平中心；
    //   时长行的 "/" 放在中间列，播放键也放在中间列 ⟹ 两者中心必然重合。
    // 宽度必须两边一样（都取 --npctl-w），否则两个容器中心不同，对齐又跑了。
    // 整页控制组宽度：261024-01 起控制键与时间/音量同处一行 ⟹ 必须收窄（旧 70vw 在新布局里会溢出）
    h += ":root{--npctl-w:min(460px,40vw)}";
    h += ".npkeys{display:grid;grid-template-columns:1fr auto 1fr;align-items:center;gap:14px;width:var(--npctl-w)}";
    h += ".npkeys .grp{display:flex;align-items:center;gap:14px}";
    h += ".npkeys .grp.l{justify-content:flex-end}.npkeys .grp.r{justify-content:flex-start}";
    // 261024-01 #3b：时间不再是"当前 x:xx / 总时长 y:yy"两条对称列（那是上下堆叠时代的对齐补丁），
    // 现在是控制行左侧的一个紧凑整体，与控制键/音量同高。
    h += ".nptime{font-size:12px;color:var(--mut2);display:flex;align-items:center;gap:6px;font-variant-numeric:tabular-nums;white-space:nowrap}";
    h += ".nptime .tval{font-weight:600;color:var(--txt2);min-width:0}";
    h += ".nptime .tsep{opacity:.45}";
    h += ".npseek{width:100%;height:5px;border-radius:3px;background:rgba(255,255,255,.16);position:relative;cursor:pointer;transition:height .15s}";
    h += ".npseek:hover{height:7px}";
    h += ".npseek>i{position:absolute;left:0;top:0;bottom:0;border-radius:2px;background:var(--acc);width:0}";
    h += ".nptime .tlab{font-size:10px;color:var(--mut2);letter-spacing:.4px}";
    h += ".nptime .tval{min-width:42px;text-align:center;font-weight:600;color:var(--txt2)}";
    h += ".nptime .tsep{opacity:.45;margin:0 1px;text-align:center;min-width:10px}";
    // 整页音量（用户 UI2：单曲播放页缺音量调节；261024-01 #3b ①：并入控制行，不再单独占底部一行）
    h += ".npvol{display:flex;align-items:center;gap:8px;color:var(--mut);width:auto;min-width:150px}";
    h += ".npvol input[type=range]{width:104px;accent-color:var(--acc)}";
    h += ".npvol .vlab{font-size:11px;min-width:46px;text-align:right;font-variant-numeric:tabular-nums;color:var(--txt2)}";
    // 行3：歌词区（独立滚动，min-height:0 保证网格行真的能压缩 —— 这是 1024x640 下
    // 歌词压到进度条上的根因：旧实现 .nplyric 固定 58vh 且控件绝对定位，二者必然重叠）
    h += ".nplyric2{--nppad:0px;min-height:0;overflow:auto;text-align:center;scrollbar-width:none;padding:2px 0;-webkit-mask-image:linear-gradient(180deg,transparent 0,#000 26px,#000 calc(100% - 26px),transparent 100%);mask-image:linear-gradient(180deg,transparent 0,#000 26px,#000 calc(100% - 26px),transparent 100%)}";
    // 无歌词/纯音乐时把提示垂直居中，避免大片空白显得像没加载出来
    h += ".nplyric2.empty{display:flex;flex-direction:column;align-items:center;justify-content:center}";
    h += ".nplyric2::-webkit-scrollbar{display:none}";
    // 261024-02 #4：上下留半屏空白，让**第一行和最后一行也能滚到容器正中**
    //（否则首行贴顶、末行贴底，"active 行居中"在两端的数学目标不可达）
    // 261024-03 #3：上下留白改由 JS 按【真实行高】写进 --nppad（= (容器高 - 行高)/2）。
    // 旧实现写死 calc(50% - 22px)，22px 只是某个字号下的一半行高：893x587 档行高 27px 时
    // 少留 8.7px ⟹ 首行/末行永远差 8.7/9.4px 才到正中（实测数字见 tools/lyr_dpr.js 输出）。
    h += ".nplyric2::before,.nplyric2::after{content:'';display:block;height:var(--nppad,0px);pointer-events:none}";
    // 纯文本歌词（无时间轴）的显式提示：旧版只是"不滚动"，用户看起来像坏了
    h += ".ly-hint{position:sticky;top:0;z-index:2;font-size:10.5px;letter-spacing:.3px;color:var(--mut2);background:var(--bg);border-bottom:1px solid var(--line);padding:3px 0;margin-bottom:4px;pointer-events:none}";
    h += ".npline{font-size:clamp(13px,1.9vh,17px);line-height:2.05;color:var(--mut);transition:.18s}";
    h += ".npline.on{color:var(--txt);font-weight:650;transform:scale(1.03)}";
    // 行3：控制区（261024-01 #3b ①：音量不再是底部独立一行）——
    //   进度条一行；下面**同一行**放「时间（左）/ 控制键（中）/ 音量（右）」，
    //   三列都居中于同一条竖轴，整页底部只剩这一块控制区。
    h += ".npctl2{min-height:0;display:grid;grid-template-rows:auto auto;gap:12px;justify-items:center}";
    h += ".npctlrow{display:grid;grid-template-columns:minmax(0,1fr) auto minmax(0,1fr);align-items:center;gap:16px;width:100%}";
    h += ".npctlrow .nptime{justify-self:start}";
    h += ".npctlrow .npkeys{justify-self:center}";
    h += ".npctlrow .npvol{justify-self:end}";
    h += ".npdstat{font-size:10.5px;color:var(--mut2);white-space:nowrap;flex:none}";
    h += ".npdbody{flex:1;min-height:0;overflow:auto;padding:14px 16px 22px;font-size:12.5px;line-height:1.7;color:var(--txt2);scrollbar-width:thin}";
    h += ".npdbody::-webkit-scrollbar{width:6px}";
    h += ".npdbody::-webkit-scrollbar-thumb{background:var(--line);border-radius:3px}";
    h += ".npdsec{margin-bottom:8px}";
    h += ".npdsec h4{font-size:12px;color:var(--txt);margin:0 0 4px;font-weight:650}";
    h += ".npdbody p{margin:0 0 5px}";
    // 标签值网格（261024-01 #4）：label 小字灰、value 主色；长文本（简介/曲目列表/制作人员…）独占整行。
    // 三处复用：单曲页抽屉 / 专辑页简介 / 艺术家页资料 —— 一致性靠同一份样式。
    h += ".efields{display:grid;grid-template-columns:repeat(auto-fill,minmax(178px,1fr));gap:13px 22px}";
    h += ".efrow{min-width:0}";
    h += ".efrow.wide{grid-column:1/-1}";
    h += ".efrow .lb{font-size:10px;letter-spacing:.8px;color:var(--mut2);font-weight:600;margin-bottom:3px}";
    h += ".efrow .vl{font-size:12.5px;line-height:1.72;color:var(--txt);word-break:break-word}";
    h += ".efrow.wide .vl{color:var(--txt2);white-space:pre-wrap}";
    h += ".efrow .vl a{color:var(--acc-text)}";
    // 261024-02 #3：本地曲目列表（序号 / 曲名 / 时长 三列，等宽数字保证时长右对齐）
    h += ".ef-tracklist{display:flex;flex-direction:column;gap:1px;margin-top:2px}";
    h += ".eft-row{display:grid;grid-template-columns:26px minmax(0,1fr) auto;gap:10px;align-items:baseline;padding:3px 0;border-bottom:1px solid rgba(255,255,255,.05)}";
    h += ".eft-row:last-child{border-bottom:0}";
    h += ".eft-n{font-family:ui-monospace,Consolas,monospace;font-size:11px;color:var(--mut2);text-align:right}";
    h += ".eft-t{color:var(--txt);overflow:hidden;text-overflow:ellipsis;white-space:nowrap}";
    h += ".eft-d{font-family:ui-monospace,Consolas,monospace;font-size:11.5px;color:var(--mut);font-variant-numeric:tabular-nums}";
    h += ".trlist-head{display:flex;align-items:baseline;gap:8px;padding:2px 4px 8px;border-bottom:1px solid var(--line);margin-bottom:4px}";
    h += ".trlist-head .lb{font-size:11px;letter-spacing:1px;color:var(--mut2);font-weight:700}";
    h += ".trlist-head .sp{flex:1}";
    h += ".trlist-head .sum{font-size:11.5px;color:var(--mut);font-variant-numeric:tabular-nums}";
    // 261024-02 #2：资料里可点的外链（点击交给宿主用系统浏览器打开）
    h += ".eflink{color:var(--acc-text);text-decoration:none;border-bottom:1px dashed currentColor;cursor:pointer;word-break:break-all}";
    h += ".eflink:hover{color:var(--acc)}";
    h += "html[data-theme=light] .eft-row,html[data-theme=blue] .eft-row{border-bottom-color:var(--line)}";
    h += ".npdskel i{display:block;height:9px;border-radius:5px;margin-bottom:8px;background:linear-gradient(90deg,var(--hov),var(--hov2) 45%,var(--hov));background-size:220% 100%;animation:npskel 1.4s ease-in-out infinite}";
    h += "@keyframes npskel{0%{background-position:0 0}100%{background-position:-220% 0}}";
    h += "@media (prefers-reduced-motion:reduce){.npdskel i{animation:none}}";
    h += ".npdhint{font-size:11px;color:var(--mut2);line-height:1.6}";
    h += ".npdwork{display:flex;align-items:center;gap:10px;padding:5px 0;border-bottom:1px solid rgba(255,255,255,.07);font-size:11.5px;color:var(--txt2)}";
    h += ".npdwork .wy{font-family:ui-monospace,Consolas,monospace;color:var(--mut);width:38px;flex:none}";
    h += ".npdwork .wt{flex:1;min-width:0;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}";
    h += ".npdwork .wp{color:var(--mut2);flex:none;max-width:34%;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}";
    h += ".npclose{position:absolute;top:16px;left:16px;width:36px;height:36px;border-radius:50%;background:rgba(255,255,255,.12);display:flex;align-items:center;justify-content:center;color:var(--txt);z-index:4}";
    h += ".npclose:hover{background:rgba(255,255,255,.16)}";
    // 窄/矮窗口（用户 261023-04 #6 的 1024x640）：压缩行高与留白，但行间距不小于 12px
    h += "@media (max-height:720px){.npgrid{padding:36px 24px 10px;gap:12px}.nphero{gap:16px}.npcover{height:clamp(108px,22vh,164px)}.npkind{margin-bottom:5px}.nptit{font-size:19px}.npsub{font-size:11.5px;margin-top:5px}.npmline{margin-top:6px;min-height:20px}.npctl2{gap:8px}.npdbody{font-size:12px;line-height:1.6}}";
    h += "@media (max-width:1120px){.npgrid{padding:38px 24px 12px}.nphero{gap:16px}.npcover{height:clamp(108px,20vh,180px)}}";
    // 浅色/浅蓝主题：整页跟随主题，Windows 原生标题栏（已按同一主题上色）才不会
    // 出现"上面一条浅色、下面一整块深色"的割裂感。
    h += "html[data-theme=light] .npbg,html[data-theme=blue] .npbg{filter:blur(64px) saturate(1.05) brightness(1.12)}";
    h += "html[data-theme=light] .npveil{background:linear-gradient(180deg,rgba(244,246,249,.62),rgba(244,246,249,.95))}";
    h += "html[data-theme=blue] .npveil{background:linear-gradient(180deg,rgba(233,242,255,.64),rgba(233,242,255,.96))}";
    h += "html[data-theme=light] .npclose,html[data-theme=blue] .npclose{background:rgba(15,23,42,.08);color:var(--txt)}";
    h += "html[data-theme=light] .npclose:hover,html[data-theme=blue] .npclose:hover{background:rgba(15,23,42,.15)}";
    h += "html[data-theme=light] .npseek,html[data-theme=blue] .npseek{background:rgba(15,23,42,.16)}";
    h += "html[data-theme=light] .npcover img,html[data-theme=blue] .npcover img{box-shadow:0 24px 70px rgba(15,23,42,.22)}";
    h += "html[data-theme=light] .nprailcard,html[data-theme=blue] .nprailcard{background:rgba(15,23,42,.04);border-color:rgba(15,23,42,.10)}";
    h += "html[data-theme=light] .npdwork,html[data-theme=blue] .npdwork{border-bottom-color:var(--line)}";
    h += ".k{width:32px;height:32px;border-radius:9px;display:grid;place-items:center;color:var(--mut);transition:.13s}";
    h += ".k:hover{background:var(--hov2);color:var(--txt)}";
    h += ".k.big{width:40px;height:40px;border-radius:50%;background:linear-gradient(180deg,var(--acc),var(--acc2));color:#fff;box-shadow:0 3px 16px var(--acc-weak)}";
    h += ".k.big:hover{filter:brightness(1.1)}";
    h += ".prog{width:100%;display:flex;align-items:center;gap:9px;font-size:10.5px;color:var(--mut2);font-variant-numeric:tabular-nums;max-width:560px}";
    h += ".track{flex:1;height:5px;border-radius:3px;background:var(--track);overflow:hidden;cursor:pointer;position:relative}";
    h += ".track i{display:block;height:100%;border-radius:3px;background:linear-gradient(90deg,var(--acc),var(--acc2));transition:width .18s linear;width:0}";
    h += ".vol{display:flex;align-items:center;gap:8px;justify-content:flex-end;justify-self:end;min-width:0}.vol input[type=range]{width:88px;accent-color:var(--acc)}";
    h += ".badge{font-size:9.5px;padding:2px 7px;border-radius:5px;background:var(--acc-weak);color:var(--acc-text);border:1px solid var(--acc-weak);font-weight:600;letter-spacing:.3px;white-space:nowrap}";
    h += ".badge.g{background:var(--ok-bg);color:var(--ok-text);border-color:var(--ok-line)}";
    h += ".chip{display:inline-flex;align-items:center;gap:5px;font-size:11px;padding:3px 9px;border-radius:12px;background:var(--bg3);color:var(--txt2);border:1px solid var(--line);cursor:pointer;transition:.15s;user-select:none}";
    h += ".chip:hover{background:var(--hov);color:var(--txt);border-color:var(--acc)}";
    h += ".toast{position:fixed;left:50%;transform:translateX(-50%);bottom:104px;z-index:21;background:var(--toast);border:1px solid var(--line);border-radius:9px;padding:9px 17px;font-size:12px;opacity:0;transition:.25s;pointer-events:none;box-shadow:0 8px 26px var(--shadow)}";
    h += ".toast.on{opacity:1;bottom:114px}";
    h += ".scanbar{height:3px;background:var(--acc-weak);border-radius:2px;overflow:hidden;margin:8px 18px 0}";
    h += ".scanbar i{display:block;height:100%;background:linear-gradient(90deg,var(--acc),var(--acc2));width:0;transition:width .3s}";
    // 右栏标签页
    h += ".rtab{padding:4px 10px;border-radius:7px;font-size:11.5px;color:var(--mut);transition:.13s}";
    h += ".rtab:hover{background:var(--hov);color:var(--txt)}";
    h += ".rtab.on{background:var(--acc-weak);color:var(--acc-text);font-weight:600}";
    // 歌词
    h += "#lyricbox{--lyrpad:0px;height:100%;overflow:auto;padding:18px 20px;text-align:center;position:relative}";
    // 同上：留白按真实行高算（旧 calc(50% - 18px) 在行高 34px 时也差 1px，行高变化后误差更大）
    h += "#lyricbox::before,#lyricbox::after{content:'';display:block;height:var(--lyrpad,0px)}";
    h += ".ly{font-size:13.5px;line-height:2.1;color:var(--mut2);transition:.2s;cursor:pointer;border-radius:6px;padding:1px 6px}";
    h += ".ly:hover{background:var(--hov);color:var(--mut)}";
    h += ".ly.on{color:var(--txt);font-weight:600;font-size:15px}";
    h += ".ly.none{color:var(--mut2);font-size:12px;line-height:1.9}";
    // 播放列表
    h += ".plrow{display:flex;align-items:center;gap:8px;padding:8px 14px;cursor:pointer;font-size:12.5px;border-bottom:1px solid var(--line)}";
    h += ".plrow:hover{background:var(--hov)}";
    h += ".plrow .nm{flex:1;min-width:0;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}";
    h += ".plrow .n{font-size:11px;color:var(--mut2)}";
    h += ".plrow button{color:var(--mut2);font-size:14px;padding:0 5px}";
    h += ".plrow button:hover{color:var(--danger)}";
    // 弹层
    h += "#modal{position:fixed;inset:0;z-index:40;background:rgba(0,0,0,.45);backdrop-filter:blur(3px);display:none;place-items:center;padding:12px;box-sizing:border-box;overflow:auto}";
    h += "#modal.on{display:grid}";
    h += ".mcard{width:min(600px,92vw);max-height:calc(100vh - 56px);display:flex;flex-direction:column;overflow:hidden;background:var(--bg2);border:1px solid var(--line);border-radius:14px;box-shadow:0 24px 70px var(--shadow)}";
    h += ".mhead{display:flex;align-items:center;gap:9px;padding:14px 18px;border-bottom:1px solid var(--line);font-weight:650;flex:none}";
    h += ".mbody{padding:16px 18px 22px;flex:1;min-height:0;overflow:auto}";
    h += ".chain{display:flex;flex-direction:column;gap:0}";
    h += ".cnode{display:flex;gap:12px;align-items:flex-start;padding:9px 0}";
    h += ".cnode .dot{width:9px;height:9px;border-radius:50%;background:var(--acc);margin-top:6px;flex:none;box-shadow:0 0 12px var(--acc-weak)}";
    h += ".cnode.gpu .dot{background:var(--ok);box-shadow:0 0 12px var(--ok-line)}";
    h += ".cnode .k{font-size:10px;letter-spacing:1.1px;color:var(--mut2);font-weight:600;white-space:nowrap}";
    h += ".cnode > div:last-child{min-width:0}";
    h += ".cnode .v{font-size:12.5px;color:var(--txt);margin-top:1px}";
    h += ".cline{width:1px;height:14px;background:var(--line);margin-left:4px}";
    // 设置页
    h += "#setpage{display:none;padding:20px 24px 104px}";   // 底部留出 sticky 保存栏的高度 + 呼吸位（UI 评审：窄窗底栏压字）
    h += "#setpage.on{display:block}";
    h += ".sgroup{margin-bottom:22px;border:1px solid var(--line);border-radius:12px;background:var(--bg2);overflow:visible}";
    h += ".sghead{display:flex;align-items:center;gap:9px;padding:11px 16px;border-bottom:1px solid var(--line);font-weight:650;font-size:12.5px;background:var(--bg3);border-radius:11px 11px 0 0}";
    h += ".sghead .hint{font-weight:400;font-size:11px;color:var(--mut2)}";
    h += ".sgbody{padding:14px 16px;display:grid;grid-template-columns:repeat(auto-fill,minmax(260px,1fr));gap:14px 20px}";
    h += ".sfld{min-width:0}";
    h += ".sfld label{display:block;font-size:11px;color:var(--mut);margin-bottom:5px;font-weight:500}";
    h += ".sfld .hint{font-size:10.5px;color:var(--mut2);margin-top:4px;line-height:1.5}";
    // ===== 20 段 PEQ（261024-03 #5：文字对齐 + HIFI 交互） =====
    // 为什么弃 table：th/td 的列宽由内容撑开，表头与输入框天生不同宽（用户看到的"文字没对齐"）。
    // 现在表头与每一行共用同一个【内容无关】的列模板（px + fr，没有 auto）⟹ 列位置逐像素一致。
    // 表格与曲线同宽（720px）：列宽不至于被拉成"一大片空白输入框"，也不会跟上面的曲线错位
    h += ".peqwrap{--peqcols:36px minmax(0,1.3fr) minmax(0,1fr) minmax(0,.9fr) 96px 40px;max-width:720px}";
    h += ".peqhd,.peqr{display:grid;grid-template-columns:var(--peqcols);gap:0 8px;align-items:center}";
    // 表头必须跟数据行【同一个滚动容器、同一个内容宽度】：否则滚动条宽度 + 表头自带边框
    // 会让两边的可用宽度差 8~10px，fr 列随之变宽，逐列中心偏差线性放大到 9px（实测 1.7/4.9/7.7/9.0）。
    h += ".peqhd{position:sticky;top:0;z-index:3;background:var(--field);border-bottom:1px solid var(--line);padding:7px 8px;font-size:11px;font-weight:600;color:var(--mut2);letter-spacing:.2px}";
    h += ".peqhd>span{padding:0 6px;white-space:nowrap}";
    h += ".peqhd>.num,.peqc.num{text-align:right}";
    h += ".peqhd>.c,.peqc.c{text-align:center}";
    h += ".peqrows{max-height:340px;overflow:auto;border:1px solid var(--line);border-radius:8px}";
    h += ".peqr{padding:3px 8px;border-bottom:1px solid rgba(255,255,255,.05)}";
    h += ".peqr:last-child{border-bottom:0}";
    h += ".peqr:hover{background:var(--hov)}";
    h += ".peqr.sel{background:var(--acc-weak);box-shadow:inset 2px 0 0 var(--acc)}";
    h += ".peqr:focus{outline:0;background:var(--hov)}";
    h += ".peqn{font-family:ui-monospace,Consolas,monospace;font-size:11px;color:var(--mut2);font-variant-numeric:tabular-nums;text-align:right;padding:0 6px}";
    h += ".peqr.sel .peqn{color:var(--acc-text);font-weight:700}";
    h += ".peqc{padding:0 6px;min-width:0}";
    // 覆盖 .sfld input 的 31px 高 / 7px 内边距（特异性 0,3,1 > 0,2,1）
    h += ".peqwrap .peqc input[type=text],.peqwrap .peqc input[type=number],.peqwrap .peqc select{width:100%;height:24px;min-height:24px;box-sizing:border-box;padding:0 6px;border-radius:5px;border:1px solid var(--line);background:var(--field);color:var(--txt);font-size:11.5px;font-family:inherit;line-height:22px;font-variant-numeric:tabular-nums}";
    h += ".peqwrap .peqc input[type=text],.peqwrap .peqc input[type=number]{text-align:right}";
    h += ".peqwrap .peqc input:focus,.peqwrap .peqc select:focus{outline:0;border-color:var(--acc);box-shadow:0 0 0 2px var(--acc-weak)}";
    h += ".peqwrap .peqc input[type=checkbox]{width:14px;height:14px;min-height:0;margin:0;accent-color:var(--acc)}";
    h += ".peqcv{position:relative;max-width:720px;background:var(--field);border:1px solid var(--line);border-radius:8px;padding:6px 10px 2px;user-select:none}";
    h += ".peqcv svg.peqsvg{display:block;width:100%;height:auto;touch-action:none}";
    h += ".peqnode{fill:var(--acc);stroke:var(--bg);stroke-width:1.4;cursor:ns-resize}";
    h += ".peqnode:hover,.peqnode.on{fill:var(--acc2);stroke-width:2}";
    h += ".peqnode.off{fill:var(--mut2);opacity:.5}";
    h += ".peqhit{fill:transparent;cursor:ns-resize}";
    h += ".peqtip{position:absolute;z-index:6;pointer-events:none;background:var(--toast);border:1px solid var(--line);border-radius:7px;padding:4px 8px;font-size:11px;color:var(--txt);box-shadow:0 6px 18px var(--shadow);white-space:nowrap;opacity:0;transition:opacity .1s}";
    h += ".peqtip.on{opacity:1}";
    h += ".peqlegend{display:flex;gap:14px;flex-wrap:wrap;font-size:10.5px;color:var(--mut2);padding:5px 2px 0}";
    h += ".peqlegend b{color:var(--txt2);font-weight:600}";
    // 261024-04 #3：PEQ 旁路的语义必须一眼看懂 —— 组标题下常驻一行「PEQ 当前：生效 / 已旁路」。
    // 生效用正文色（不是绿色：绿色在本项目里已经专指 bit-perfect / GPU），旁路用弱化色。
    h += ".peqstat{display:flex;align-items:center;gap:8px;font-size:12px;color:var(--txt)}";
    h += ".peqstat .tag{font-size:10.5px;font-weight:700;letter-spacing:.3px;padding:2px 8px;border-radius:9px;border:1px solid var(--line);background:var(--hov);color:var(--txt2);flex:none}";
    h += ".peqstat.bypass{color:var(--mut)}";
    h += ".peqstat.bypass .tag{color:var(--mut2);border-style:dashed}";
    h += ".peqstat .sub{font-size:11px;color:var(--mut2)}";

    // 2026-10-24（用户 261024-01 #5）：「密码框跟其他框样式不一样」的根因 ——
    // 旧选择器只列了 text/number，password 完全没命中任何一条规则 ⟹ 落到浏览器默认样式
    // （无内边距、方角、原生边框、宽度不铺满）。这里把所有文本类 input 一起列全，逐项对齐。
    h += ".sfld select,.sfld input[type=text],.sfld input[type=number],.sfld input[type=password],.sfld input[type=url],.sfld input[type=search]{width:100%;padding:7px 10px;border-radius:8px;border:1px solid var(--line);background:var(--field);font-size:12px;font-family:inherit;line-height:1.45;color:var(--txt);transition:.15s;min-height:31px}";
    h += ".sfld select:focus,.sfld input:focus{outline:0;border-color:var(--acc);box-shadow:0 0 0 3px var(--acc-weak)}";
    h += ".sfld input::placeholder{color:var(--mut2);opacity:1}";
    // 2026-10-24（用户 261024-01 #7）：长说明不再铺在页面上 —— 收进 (?) 图标，hover/focus 才显示。
    h += ".tip{display:inline-flex;align-items:center;justify-content:center;width:15px;height:15px;margin-left:5px;border-radius:50%;border:1px solid var(--line2,var(--line));color:var(--mut2);font-size:9.5px;font-weight:700;line-height:1;cursor:help;vertical-align:1px;position:relative;user-select:none;flex:none}";
    h += ".tip:hover,.tip:focus{border-color:var(--acc);color:var(--acc);outline:0}";
    h += ".tip::after{content:attr(data-tip);position:absolute;left:50%;bottom:calc(100% + 8px);transform:translateX(-50%);width:max-content;max-width:290px;background:var(--bg3);color:var(--txt2);border:1px solid var(--line);border-radius:9px;padding:8px 10px;font-size:11px;line-height:1.6;font-weight:400;white-space:normal;text-align:left;box-shadow:0 10px 28px var(--shadow);opacity:0;visibility:hidden;transition:opacity .12s,transform .12s;z-index:40;pointer-events:none}";
    h += ".tip:hover::after,.tip:focus::after{opacity:1;visibility:visible}";
    // 分组标题的 (?) 改成向下展开：向上会被吸顶的页签条盖住（实测第一个分组就是这样）。
    h += ".sghead .tip::after{bottom:auto;top:calc(100% + 8px)}";
    h += "@media (max-width:1200px){.tip::after{max-width:min(240px,42vw)}}";
    h += ".sfld.wide{grid-column:1/-1}";
    h += ".sfld .chk{margin:0}";
    // 元数据「高级选项」：左右两列卡片（左=数据源，右=DeepSeek 接入 + 标签写回）。
    // 2026-10-23 用户 261023-04 #3：原来左边只有一行按钮、右边一整竖排，左右失衡又挤。
    h += ".meta-adv-grid{display:grid;grid-template-columns:minmax(0,1fr) minmax(0,1fr);gap:16px;align-items:start}";
    h += ".meta-card{border:1px solid var(--line);border-radius:10px;background:var(--bg2);padding:14px 16px;min-width:0}";
    h += ".meta-card .mh{display:flex;align-items:baseline;gap:8px;font-size:12.5px;font-weight:650;color:var(--txt);margin-bottom:10px;padding-bottom:8px;border-bottom:1px solid var(--line)}";
    h += ".meta-card .mh.sub{margin-top:16px}";
    h += ".meta-src-grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(210px,1fr));gap:10px 14px}";
    h += ".meta-card .chk{margin-bottom:0}";
    // 2026-10-24 #5：DeepSeek API Key 也是 password，之前落在 .sfld 之外没有任何输入框样式
    //（和内嵌表单里那个密码框同病）。同样逐项对齐到文本框。
    h += ".meta-card input[type=text],.meta-card input[type=password]{width:100%;padding:7px 10px;border-radius:8px;border:1px solid var(--line);background:var(--field);font-size:12px;font-family:inherit;line-height:1.45;color:var(--txt);min-height:31px;transition:.15s}";
    h += ".meta-card input:focus{outline:0;border-color:var(--acc);box-shadow:0 0 0 3px var(--acc-weak)}";
    h += "@media (max-width:1100px){.meta-adv-grid{grid-template-columns:1fr}}";
    // 设置页分页（对齐 HQPlayer 的 Inputs / Outputs / PCM / SDM / Advanced）
    h += ".stabs{display:flex;gap:4px;padding:10px 0 12px;margin:-20px 0 14px;border-bottom:1px solid var(--line);position:sticky;top:0;background:var(--bg);z-index:6}";
    h += ".stab{padding:7px 15px;border-radius:8px;font-size:12px;color:var(--mut);border:1px solid transparent;transition:.13s}";
    h += ".stab:hover{background:var(--hov);color:var(--txt2)}";
    h += ".stab.on{background:var(--acc-weak);color:var(--acc-text);border-color:rgba(59,130,246,.32);font-weight:600}";
    h += ".spane{display:none}.spane.on{display:block}";
    h += ".trimgrid{display:grid;grid-template-columns:repeat(4,minmax(0,1fr));gap:10px}";
    // 2026-10-24（用户 261024-01 #5）：流媒体多服务器列表 + 行内操作
    h += ".srvlist{display:flex;flex-direction:column;gap:8px}";
    h += ".srvrow{display:grid;grid-template-columns:9px minmax(0,1fr) auto;align-items:center;gap:9px 12px;padding:10px 13px;border:1px solid var(--line);border-radius:10px;background:var(--field);transition:.13s}";
    h += ".srvrow.on{border-color:var(--acc);background:var(--hov);box-shadow:0 0 0 1px var(--acc-weak)}";
    h += ".srvrow:hover{background:var(--hov)}";
    h += ".srvrow.on:hover{background:var(--hov2,var(--hov))}";
    h += ".srvdots{width:9px;height:9px;border-radius:50%;background:var(--track);flex:none}";
    h += ".srvrow.on .srvdots{background:var(--ok);box-shadow:0 0 9px var(--ok-line)}";
    h += ".srvmain{min-width:0}";
    h += ".srvname{display:flex;align-items:center;gap:7px;font-size:12.5px;font-weight:650;color:var(--txt);min-width:0}";
    h += ".srvname .nm{overflow:hidden;text-overflow:ellipsis;white-space:nowrap}";
    h += ".srvurl{font-size:11px;color:var(--mut2);margin-top:3px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}";
    h += ".srvacts{display:flex;gap:6px;align-items:center;flex-wrap:wrap;justify-content:flex-end}";
    h += ".srvtag{font-size:10px;font-weight:600;letter-spacing:.2px;padding:2px 8px;border-radius:9px;border:1px solid var(--line);background:var(--hov);color:var(--mut2);white-space:nowrap}";
    h += ".srvtag.on{background:var(--ok-bg);border-color:var(--ok-line);color:var(--ok-text)}";
    h += ".srvtag.kind{background:var(--acc-weak);border-color:rgba(59,130,246,.28);color:var(--acc-text)}";
    h += ".srvbtn{font-size:11.5px;color:var(--mut);padding:4px 9px;border-radius:7px;border:1px solid transparent;transition:.13s;white-space:nowrap}";
    h += ".srvbtn:hover{background:var(--hov);color:var(--txt);border-color:var(--line)}";
    h += ".srvbtn.pri{color:var(--acc-text);border-color:rgba(59,130,246,.3);background:var(--acc-weak)}";
    h += ".srvbtn.pri:hover{color:var(--acc-text);background:var(--acc-weak);border-color:var(--acc)}";
    h += ".srvbtn.del:hover{color:var(--danger);border-color:var(--danger)}";
    h += ".srvempty{padding:16px;text-align:center;color:var(--mut2);font-size:12px;border:1px dashed var(--line);border-radius:10px;background:var(--hov)}";
    h += ".srvform{display:grid;grid-template-columns:repeat(auto-fill,minmax(220px,1fr));gap:12px 18px;margin-top:12px;padding:14px 15px;border:1px solid var(--acc);border-radius:10px;background:var(--hov)}";
    h += ".srvform .sfld.wide{grid-column:1/-1}";
    h += ".srvform .mh{grid-column:1/-1;display:flex;align-items:center;gap:8px;font-size:12px;font-weight:650;color:var(--txt);margin-bottom:2px}";
    h += "@media (max-width:820px){.srvrow{grid-template-columns:9px minmax(0,1fr)}.srvacts{grid-column:2/-1;justify-content:flex-start}}";
    // 本地音乐库统计条（信息密度：一眼看到规模与上次扫描结果，不再靠一段说明）
    h += ".libstats{display:grid;grid-template-columns:repeat(auto-fill,minmax(150px,1fr));gap:10px 14px;font-size:11.5px;color:var(--txt2)}";
    h += ".libstats .it{background:var(--field);border:1px solid var(--line);border-radius:9px;padding:9px 12px;min-width:0}";
    h += ".libstats .lbl{display:block;font-size:10px;color:var(--mut2);margin-bottom:3px}";
    h += ".libstats .val{font-weight:650;font-size:14px;color:var(--txt);font-variant-numeric:tabular-nums;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}";
    h += "@media (max-width:1100px){#crumb,#sub{display:none}.search{max-width:320px}}";
    h += ".setfoot{display:flex;gap:10px;align-items:center;padding:12px 2px 10px;position:sticky;bottom:0;background:var(--bg);border-top:1px solid var(--line);z-index:6}";
    h += ".setfoot .sp{flex:1}";
    h += ".meter-bar{height:6px;border-radius:3px;background:var(--track);overflow:hidden;position:relative;margin:4px 0 2px}";
    h += ".meter-val{height:100%;width:0%;background:linear-gradient(90deg,var(--acc),#22c55e);transition:width .12s ease;border-radius:3px}";
    h += ".meter-val.clip{background:var(--err)}";
    // 2026-10-23 用户 261023-04 #10/#12：「采集/投放实时状态」原来是一个 260px 宽的窄列里再套
    // auto-fill 网格 —— 外层 .sgbody 把整块塞进单个网格单元，内层只剩 1 列，于是所有指标竖排成长条。
    // 修法：① 标记里给状态网格加 .sfld.wide（跨满 .sgbody 所有列）；② 卡片横向 auto-fill（宽屏 4~6 个/行、
    // 窄屏自动换行）；③ 每卡「小标题 + 大数值」两级字号，数值用 tabular-nums 对齐。
    h += ".in-stat-grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(200px,1fr));gap:10px 14px;font-size:11.5px;color:var(--txt2)}";
    h += ".in-stat-item{background:var(--field);padding:10px 13px;border-radius:9px;border:1px solid var(--line);min-width:0}";
    h += ".in-stat-item .lbl{display:block;font-size:10px;color:var(--mut2);margin-bottom:4px;letter-spacing:.2px}";
    h += ".in-stat-item .val{font-weight:650;font-variant-numeric:tabular-nums;font-size:15px;line-height:1.3;color:var(--txt)}";
    h += ".in-stat-item .val.sm{font-size:12px;font-weight:600;word-break:break-all}";
    h += "@media (max-width:1200px){.in-stat-grid{grid-template-columns:repeat(auto-fill,minmax(160px,1fr))}}";
    // 收藏 / EQ 卡片 / 调音卡片 / 芯片 / 排序
    h += ".fav-btn{background:none;border:none;cursor:pointer;padding:2px 4px;display:inline-flex;align-items:center;justify-content:center;color:var(--mut2);border-radius:4px;transition:color .15s,transform .1s}";
    h += ".fav-btn:hover{color:#e05666;transform:scale(1.15)}";
    h += ".fav-btn.on{color:#e05666}";
    h += ".fav-btn.on svg{fill:#e05666;stroke:#e05666}";
    h += ".model-card{background:var(--card);border:1px solid var(--line);border-radius:8px;padding:12px 14px;cursor:pointer;transition:all .15s ease}";
    h += ".model-card:hover{border-color:var(--acc);background:var(--hov)}";
    h += ".model-card.active{border-color:var(--acc);box-shadow:0 0 0 1.5px var(--acc);background:var(--hov)}";
    h += ".preset-card{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:14px 16px;display:flex;flex-direction:column;justify-content:space-between;transition:all .15s ease;cursor:pointer}";
    h += ".tc-curve{padding:2px 0}";
    h += ".tc-curve svg{max-width:100%}";
    h += ".preset-card:hover{border-color:var(--line2);box-shadow:0 2px 10px rgba(0,0,0,.08)}";
    h += ".preset-card.active{border-color:var(--acc);box-shadow:0 0 0 2px var(--acc);background:var(--hov)}";
    h += ".brand-chip{font-size:12px;padding:5px 12px;border-radius:16px;background:var(--hov);border:1px solid var(--line);color:var(--txt);cursor:pointer;transition:all .15s}";
    h += ".brand-chip:hover{border-color:var(--acc);color:var(--acc)}";
    h += ".track-sort{font-size:12px;height:26px;background:var(--hov);border:1px solid var(--line);border-radius:5px;color:var(--txt);padding:0 8px;cursor:pointer}";
    // 滚动条
    h += "::-webkit-scrollbar{width:9px;height:9px}::-webkit-scrollbar-thumb{background:var(--track);border-radius:5px}";
    h += "::-webkit-scrollbar-track{background:transparent}";
    h += "</style></head><body>";
    h += "<div id=app>";

    // ---------- 顶栏 ----------
    h += "<div class=top id=topbar><div class=logo>R</div><div class=ttl>RLPlayer<small>HIGH-FIDELITY ENGINE</small></div>";
    h += "<span class=hsep></span><b id=pth class=ptitle>专辑</b><span id=cnt class=pcnt></span>";
    h += "<div class=search><svg width=14 height=14 viewBox='0 0 24 24' fill=none stroke=currentColor stroke-width=2><circle cx='11' cy='11' r='7'/><path d='M20 20l-3.5-3.5'/></svg>";
    h += "<input id=q placeholder='搜索专辑 / 艺术家 / 曲目…' autocomplete=off spellcheck=false></div><div class=sp></div>";
    h += "<span id=tools class=ptools></span>";
    h += "<div class=themes><button class='sw on' data-theme=dark title='深色'></button>";
    h += "<button class=sw data-theme=light title='白色'></button><button class=sw data-theme=blue title='浅蓝'></button></div>";
    h += "<button class=ibtn id=bHelp title='快捷键与技巧'><svg width=16 height=16 viewBox='0 0 24 24' fill=none stroke=currentColor stroke-width=2><circle cx='12' cy='12' r='9'/><path d='M9.5 9a2.5 2.5 0 015 .5c0 1.5-2.5 2-2.5 3.5'/><circle cx='12' cy='17' r='.7' fill='currentColor'/></svg></button>";
    h += "<button class=ibtn id=bScan title='重新扫描音乐库'><svg width=16 height=16 viewBox='0 0 24 24' fill=none stroke=currentColor stroke-width=2><path d='M21 12a9 9 0 1 1-3-6.7'/><path d='M21 4v5h-5'/></svg></button>";
    h += "<button class=ibtn id=tq title='显示或隐藏队列'><svg width=17 height=17 viewBox='0 0 24 24' fill=none stroke=currentColor stroke-width=2><path d='M4 6h16M4 12h16M4 18h10'/></svg></button>";
    h += "<div class=wctl><button class=wbtn id=wMin title='最小化'><svg viewBox='0 0 24 24' fill=none stroke=currentColor stroke-width=2><path d='M5 12h14'/></svg></button>";
    h += "<button class=wbtn id=wMax title='最大化'><svg viewBox='0 0 24 24' fill=none stroke=currentColor stroke-width=2><rect x='6' y='6' width='12' height='12' rx='1.5'/></svg></button>";
    h += "<button class='wbtn close' id=wClose title='关闭'><svg viewBox='0 0 24 24' fill=none stroke=currentColor stroke-width=2><path d='M6 6l12 12M18 6L6 18'/></svg></button></div></div>";

    // ---------- 侧栏（只保留真正的入口；播放列表就用播放队列） ----------
    h += "<div class=side>";
    h += "<div class='nav on' data-v=albums><svg viewBox='0 0 24 24' fill='none' stroke='currentColor' stroke-width='1.8'><circle cx='12' cy='12' r='9'/><circle cx='12' cy='12' r='2.5'/></svg>专辑</div>";
    h += "<div class=nav data-v=artists><svg viewBox='0 0 24 24' fill='none' stroke='currentColor' stroke-width='1.8'><circle cx='12' cy='8' r='3.6'/><path d='M4.5 20.5c0-4.1 3.4-6.8 7.5-6.8s7.5 2.7 7.5 6.8'/></svg>艺术家</div>";
    // 2026-10-23 用户 261023-04 #9：「古典作品」不再占左导航（作品/版本推荐入口移到单曲页「作品」tab）。
    // 实现仍保留 loadWorksView/openWorkVersions/renderWorkVersions，只是不再有导航入口。
    h += "<div class=nav data-v=tracks><svg viewBox='0 0 24 24' fill='none' stroke='currentColor' stroke-width='1.8'><path d='M14.5 4.5v11.2'/><circle cx='12' cy='17.5' r='2.8'/><path d='M14.5 4.5c2.6.6 4 2 4.5 4.2'/></svg>曲目</div>";
    h += "<div class=nav data-v=files><svg viewBox='0 0 24 24' fill='none' stroke='currentColor' stroke-width='1.8'><path d='M3 7.5a2 2 0 012-2h4l1.8 2H19a2 2 0 012 2v8a2 2 0 01-2 2H5a2 2 0 01-2-2z'/></svg>文件夹</div>";
    h += "<div class=nav data-v=remote><svg viewBox='0 0 24 24' fill='none' stroke='currentColor' stroke-width='1.8'><path d='M7 18a4.5 4.5 0 010-9 6 6 0 0111.6 1.6A3.7 3.7 0 0118 18z'/></svg>流媒体</div>";
    // 2026-10-23 用户 261023-04 #2：EQ 与房间校正**不再留在左导航**，改为设置页的一个页签（data-g=eq）。
    // （261023-03 曾把它做成独立主导航页；本轮用户明确要求「放到设置里面单开一页，不是放到主页里面」。）
    h += "<div class=sec>播放</div>";
    h += "<div class=nav data-v=queue><svg viewBox='0 0 24 24' fill='none' stroke='currentColor' stroke-width='1.8'><path d='M4 6h10M4 12h10M4 18h7'/><path d='M17 15.5V7l4-1v8.5'/><circle cx='16' cy='16.5' r='1.6' fill='currentColor' stroke='none'/><circle cx='20' cy='15' r='1.6' fill='currentColor' stroke='none'/></svg>播放队列</div>";
    h += "<div class=nav data-v=recent><svg viewBox='0 0 24 24' fill='none' stroke='currentColor' stroke-width='1.8'><circle cx='12' cy='12' r='8.5'/><path d='M12 7.2V12l3.2 2'/></svg>最近播放</div>";
    h += "<div class=sp></div>";
    h += "<div class=sec>系统</div>";
    h += "<div class=nav data-v=settings><svg viewBox='0 0 24 24' fill='none' stroke='currentColor' stroke-width='1.8'><circle cx='12' cy='12' r='3.2'/><path d='M12 3.5v2.2M12 18.3v2.2M4.4 7.9l1.9 1.1M17.7 15l1.9 1.1M4.4 16.1l1.9-1.1M17.7 9l1.9-1.1'/></svg>设置</div>";
    h += "</div>";

    // ---------- 主区 ----------
    h += "<div class=main>";
    h += "<div class=scanbar id=scanbar style='display:none'><i id=scanfill></i></div>";
    h += "<div id=list></div>";
    h += "<div id=setpage></div>";
    h += "</div>";

    // ---------- 右侧（只在浏览 / 队列 / 最近播放时显示） ----------
    h += "<div id=right><div class=rhead>";
    h += "<button class='rtab on' data-t=q>队列</button><button class=rtab data-t=l>歌词</button>";
    h += "<span class=sp></span><span class=cnt id=rcnt></span>";
    h += "<button class=ibtn id=qclr title='清空队列' style='width:26px;height:26px'><svg width=13 height=13 viewBox='0 0 24 24' fill=none stroke=currentColor stroke-width=2><path d='M18 6L6 18M6 6l12 12'/></svg></button></div>";
    h += "<div class=rbody><div id=qlist></div><div id=lyricbox style='display:none'></div></div>";
    h += "</div>";

    // 参数控件只作为【状态源】存在（隐藏）。可见入口统一在设置页，
    // 免得右边和设置页各有一份、两边不一致（本轮用户反馈的就是这个问题）。
    h += "<div id=quickCtl style='display:none'>";
    h += "<select id=sDSD>";
    h += "<option value=0>PCM</option>";
    h += "<option value=dop64>DSD64 · DoP</option><option value=dop128>DSD128 · DoP</option>";
    h += "<option value=dop256>DSD256 · DoP</option><option value=dop512>DSD512 · DoP</option>";
    h += "<option value=dop1024>DSD1024 · DoP</option>";
    h += "<option value=nat64>DSD64 · Native</option><option value=nat128>DSD128 · Native</option>";
    h += "<option value=nat256>DSD256 · Native</option><option value=nat512>DSD512 · Native</option>";
    h += "<option value=nat1024>DSD1024 · Native</option>";
    h += "</select>";
    h += "<div id=devName></div>";
    // 首次默认 = follow（跟随系统）。理由见 src/audio/rate_pick.h 的 "follow" 注：
    // 默认 max 会挑设备最高率（例如 384k），而声卡当前配置常常是 48k，独占按 384k
    // 开流在某些设备上直接不出声（用户 2026-10-22 功能1 的原话）。
    h += "<select id=sRate><option value=follow>跟随系统</option><option value=max>自动（设备最高）</option><option value=src>源采样率</option>";
    for (int r : {44100, 48000, 88200, 96000, 176400, 192000, 352800, 384000, 705600, 768000}) {
        h += "<option value=" + std::to_string(r) + ">" + std::to_string(r) + "</option>";
    }
    h += "</select>";
    h += "<select id=sFilt>FILTER_OPTS</select>";
    h += "<select id=sFiltNx>FILTER_OPTS</select>";
    h += "<select id=sShap>SHAPER_OPTS</select>";
    h += "<input id=sBuf type=number value=100>";
    h += "<input type=checkbox id=sLoud><input type=checkbox id=sIsp checked><input type=checkbox id=s2s checked>";
    h += "<select id=sGpu><option value=auto>自动</option><option value=amd>AMD</option><option value=nvidia>NVIDIA</option><option value=off>关闭</option></select>";
    h += "<input type=checkbox id=sShuf>";
    h += "<select id=sRep><option value=0>关闭</option><option value=1>列表循环</option><option value=2>单曲循环</option></select>";
    h += "<input id=sRoot>";
    h += "<button id=bAddRoot></button><button id=bRescan></button>";
    h += "<div id=roots2></div>";
    h += "<button id=bUpnp></button><button id=bNet></button><div id=upnpList></div><div id=netMsg></div>";
    // 流媒体服务器控件已移到「高级」页（这里不再保留副本，避免重复 id）
    h += "</div>";

    // ---------- 播放条 ----------
    h += "<div class=bar><div class='np' id=npOpen title='打开正在播放'>";
    h += "<div class=cv2 id=cv></div><div class=tx><div class=t1 id=npt>未播放</div><div class=t2 id=npart></div>";
    h += "<div class=t2 id=nps>选择一个专辑开始</div></div></div>";
    // 2026-10-23（用户 261023-02 #1）：底栏常驻 bit-perfect 徽标。
    // 2026-10-23（用户 261023-04 #1）：它**不能**再作为底栏 grid 的第 4 个子项（那会把控制组挤到最右、
    // 把右侧状态簇挤到第二行）。现在作为紧凑 chip 收进 .vol 状态簇，由 CSS 控制显隐与配色。
    h += "<div class=ctr><div class=keys>";
    // 循环方式（关 / 列表 / 单曲）：放在播放键左边，凑齐五个键
    h += "<button class=k id=bRep title='循环方式'><svg width=17 height=17 viewBox='0 0 24 24' fill=none stroke=currentColor stroke-width=2>";
    h += "<path d='M4 9V7a2 2 0 012-2h11'/><path d='M14 2.5L17.5 5 14 7.5'/><path d='M20 15v2a2 2 0 01-2 2H7'/><path d='M10 21.5L6.5 19 10 16.5'/></svg>";
    h += "<span id=repOne style='position:absolute;font-size:9px;font-weight:700;display:none'>1</span></button>";
    h += "<button class=k id=bPrev title='上一首'><svg width=17 height=17 viewBox='0 0 24 24' fill=currentColor><path d='M6 5h2v14H6zM20 5v14l-11-7z'/></svg></button>";
    h += "<button class='k big' id=bPause title='暂停 / 继续'><svg width=15 height=15 viewBox='0 0 24 24' fill=currentColor><path id=icP d='M7 5h4v14H7zM13 5h4v14h-4z'/></svg></button>";
    h += "<button class=k id=bNext title='下一首'><svg width=17 height=17 viewBox='0 0 24 24' fill=currentColor><path d='M16 5h2v14h-2zM4 5v14l11-7z'/></svg></button>";
    h += "<button class=k id=bStop title='停止'><svg width=15 height=15 viewBox='0 0 24 24' fill=currentColor><rect x='6' y='6' width='12' height='12' rx='2'/></svg></button>";
    h += "</div><div class=prog><span id=tpos title='当前时间'>0:00</span><div class=track id=seek title='点击跳转'><i id=bar></i></div><span id=tdur title='总时长'>0:00</span></div></div>";
    h += "<div class=vol><button class=ibtn id=bLyric title='歌词'><svg width=16 height=16 viewBox='0 0 24 24' fill=none stroke=currentColor stroke-width=1.8><path d='M4 5h16M4 10h16M4 15h10'/><circle cx='18' cy='17' r='2.5'/></svg></button>";
    h += "<button class=ibtn id=bChain title='信号链'><svg width=16 height=16 viewBox='0 0 24 24' fill=none stroke=currentColor stroke-width=1.8><path d='M9 12a3 3 0 016 0'/><path d='M6 12H3M21 12h-3'/><circle cx=12 cy=12 r=3'/></svg></button>";
    h += "<span id=bpBadge title=''></span>";
    h += "<span class=badge id=fmt>--</span><span class='badge g' id=gpub style='display:none'>GPU</span>";
    // 261024-02 #5：投放中的底栏徽标（与单曲页按钮、流媒体页推送区块同源 /api/cast/state）
    h += "<span class=badge id=castBadge style='display:none' title='正在投放'>UPnP</span>";
    h += "<svg width=14 height=14 viewBox='0 0 24 24' fill=none stroke=currentColor stroke-width=2 opacity=.55><path d='M3 10v4h4l5 4V6L7 10H3z'/><path d='M16.5 8.5a5 5 0 010 7'/></svg>";
    h += "<input id=vol type=range min=-60 max=0 step=0.5 value=0></div></div>";
    // ---------- 正在播放（点封面/曲名打开） ----------
    // 布局：CSS Grid 四行（2026-10-23 用户 261023-04 #6）——
    //   .nphero 封面+标题区 / .npdata 资料区 / #npLyric 歌词区 / .npctl2 进度控制区。
    //   任何尺寸下四行互不重叠：每行 min-height:0 + 行内滚动，控件不再绝对定位。
    h += "<div id=npPage>";
    h += "<div class=npbg id=npbg></div><div class=npveil></div>";
    // #5：页面最外圈刷成 --bg（暗角），与宿主 5px 边带同色 ⟹ 接缝不可见（见上方 CSS 注）
    h += "<div class=npedge></div>";
    // 2026-10-23 用户反馈「单曲播放页面无法拖动窗口（主页面可以）」：
    //   主页面的拖拽热区在顶栏，而 npPage 是 fixed 全屏覆盖层、把顶栏盖住 → 没有可拖区域。
    //   这里加一条覆盖层自己的拖拽条：透明、贴顶、高度 34px，鼠标按下时通过
    //   chrome.webview.postMessage("drag") 交给 launcher 的 WM_NCLBUTTONDOWN/HTCAPTION。
    h += "<div class=npgrip id=npGrip title='拖拽移动窗口'></div>";
    h += "<button class=npclose id=npClose title='返回'><svg width=16 height=16 viewBox='0 0 24 24' fill=none stroke=currentColor stroke-width=2><path d='M15 5l-7 7 7 7'/></svg></button>";
    h += "<div class=npgrid id=npGrid>";
    // 行1：封面 + 标题区（标题/副题/元数据芯片/EQ 芯片）
    h += "<div class=nphero>";
    h += "<div class=npcover><img id=npCover alt=''></div>";
    h += "<div class=npinfo>";
    h += "<div class=npkind id=npKind>正在播放</div>";
    h += "<div class=nptitleline><div class=nptit id=npTitle>未播放</div>";
    h += "<button class=fav-btn id=npFavBtn title='收藏' style='padding:4px;display:none'><svg width=20 height=20 viewBox='0 0 24 24' fill=none stroke='currentColor' stroke-width=2><path d='M20.84 4.61a5.5 5.5 0 00-7.78 0L12 5.67l-1.06-1.06a5.5 5.5 0 00-7.78 7.78l1.06 1.06L12 21.23l7.78-7.78 1.06-1.06a5.5 5.5 0 000-7.78z'/></svg></button></div>";
    h += "<div class=npsub id=npSub></div>";
    // npMeta：元数据补全芯片（year/genre/albumArtist/已补全）+ 当前 EQ/IR 芯片，由 renderNp 填充
    h += "<div class=npmline id=npMeta></div>";
    h += "</div></div>";
    // 行2（261024-01 #3b ②③）：**左歌词 / 右资料标签栏**，左右排列，绝不上下堆叠。
    //   资料默认完全隐藏，右侧只露「歌曲 / 专辑 / 艺术家 / 作品」标签；
    //   点标签 → 从右侧滑出抽屉看内容（.npdrawer，默认 translateX(102%) + visibility:hidden）。
    h += "<div class=npbody id=npBody>";
    h += "<div class=nplyric2 id=npLyric><div class=npline>还没有在播放</div></div>";
    h += "<aside class=nprail id=npRail><div class=nprailcard><div class=nprailcap id=npRailCap>AI 整理</div>";
    h += "<div class=npchipwrap id=npTabs></div>";
    // 261024-04 #7：自动加载的状态/失败重试行（正常态 display:none，不打扰版面）
    h += "<div id=npSummHint class=nprailhint style='display:none'></div></div></aside>";
    h += "</div>";
    // 行3：控制区（整页也要能操作，否则进来就必须退出）
    h += "<div class=npctl2>";
    h += "<div class=npseek id=npSeek title='点击跳转'><i id=npBar></i></div>";
    // 261024-01 #3b ①：音量不再单独一行 —— 「时间 / 控制键 / 音量」同处一行（三列网格，中列恒居中）。
    h += "<div class=npctlrow>";
    h += "<div class=nptime><span class=tval id=npPos>0:00</span><span class=tsep id=npSep>/</span><span class=tval id=npDur>0:00</span></div>";
    h += "<div class=npkeys>";
    // 左组：循环方式 + 上一首。循环按钮直接把 #sRep 的值改掉并下发（唯一真相源，
    // 见 ui_script 的 cycleRep()——整页与底栏/设置页共用同一个值，不各存一份）。
    h += "<div class='grp l'>";
    h += "<button class=k id=npRep title='循环方式'><svg width='18' height='18' viewBox='0 0 24 24' fill=none stroke='currentColor' stroke-width='2'>";
    h += "<path d='M4 9V7a2 2 0 012-2h11'/><path d='M14 2.5L17.5 5 14 7.5'/><path d='M20 15v2a2 2 0 01-2 2H7'/><path d='M10 21.5L6.5 19 10 16.5'/></svg>";
    h += "<span id=npRepOne style='position:absolute;font-size:10px;font-weight:700;display:none;margin-top:-18px;margin-left:22px'>1</span></button>";
    h += "<button class=k id=npPrev title='上一首'><svg width='18' height='18' viewBox='0 0 24 24' fill='currentColor'><path d='M6 5h2v14H6zM20 5v14l-11-7z'/></svg></button>";
    h += "</div>";
    h += "<button class='k big' id=npPlay title='暂停 / 继续'><svg width='16' height='16' viewBox='0 0 24 24' fill='currentColor'><path id='npIcP' d='M7 5h4v14H7zM13 5h4v14h-4z'/></svg></button>";
    h += "<div class='grp r'>";
    h += "<button class=k id=npNext title='下一首'><svg width='18' height='18' viewBox='0 0 24 24' fill='currentColor'><path d='M16 5h2v14h-2zM4 5v14l11-7z'/></svg></button>";
    h += "<button class=k id=npStop title='停止'><svg width='15' height='15' viewBox='0 0 24 24' fill='currentColor'><rect x='6' y='6' width='12' height='12' rx='2'/></svg></button>";
    h += "</div>";   // .grp.r
    h += "</div>";   // .npkeys —— 音量必须和 npkeys 同级，才是"同一行"
    // 音量并入同一控制行（#3b ①）；滑块本身是 #vol 的镜像（同一个 S.vol 真相源，见 setVol）
    h += "<div class=npvol><svg width='14' height='14' viewBox='0 0 24 24' fill=none stroke='currentColor' stroke-width='2' opacity=.6><path d='M3 10v4h4l5 4V6L7 10H3z'/><path d='M16.5 8.5a5 5 0 010 7'/></svg>";
    h += "<input id=npVol type=range min=-60 max=0 step=0.5 value=0 title='音量（dB）'>";
    h += "<span class=vlab id=npVolLab>0.0 dB</span></div>";
    h += "</div>";   // .npctlrow
    h += "</div>";   // .npctl2
    // 关：.npgrid → #npPage
    h += "</div>";
    // 资料抽屉（#3b ③）：默认隐藏；点右侧标签才滑出。层级在 .npgrid 之上、#modal 之下。
    h += "<div class=npdrawerbg id=npDrawerBg></div>";
    h += "<aside class=npdrawer id=npDrawer>";
    h += "<div class=npdhead><span class=npdkicker id=npDrawerKind>资料</span>";
    h += "<div class=npdname id=npDrawerName></div>";
    h += "<span class=npdstat id=npDataStat></span>";
    h += "<button class=npdclose id=npDataClose title='关闭（Esc）'><svg width=14 height=14 viewBox='0 0 24 24' fill=none stroke=currentColor stroke-width=2><path d='M18 6L6 18M6 6l12 12'/></svg></button>";
    h += "</div><div class=npdbody id=npDataBody></div></aside>";
    h += "</div>";

    h += "<div id=modal><div class=mcard><div class=mhead><b id=mtitle>信号链</b><span class=sp></span><button class=ibtn id=mclose><svg width=15 height=15 viewBox='0 0 24 24' fill=none stroke=currentColor stroke-width=2><path d='M18 6L6 18M6 6l12 12'/></svg></button></div><div class=mbody id=mbody></div></div></div>";
    h += "<div class=toast id=toast></div></div>";
    h += "<script src=/app.js></script></body></html>";
    return h;
}

} // namespace rta::net