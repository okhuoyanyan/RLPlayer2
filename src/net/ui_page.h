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
         "--warn:#fbbf24;--danger:#e8654b;"
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
         "--warn:#b45309;--danger:#dc2626;"
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
         "--warn:#b45309;--danger:#c2410c;"
         "}";
    h += "*{box-sizing:border-box;margin:0;padding:0}html,body{height:100%}";
    h += "body{font:13px/1.5 Inter,'Segoe UI Variable','Segoe UI',system-ui,sans-serif;background:var(--bg);color:var(--txt);overflow:hidden;-webkit-font-smoothing:antialiased}";
    h += "button{font:inherit;color:inherit;background:none;border:0;cursor:pointer}input,select{font:inherit;color:inherit}";
    h += "a{color:inherit;text-decoration:none}";
    h += "#app{display:grid;grid-template-columns:212px 1fr 378px;grid-template-rows:52px 1fr 88px;height:100vh}";
    h += "#app.noq{grid-template-columns:212px 1fr 0}#app.noq #right{display:none}";
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
    h += ".ahead .kind{font-size:10px;letter-spacing:1.4px;color:var(--mut2);font-weight:600}";
    h += ".ahead h1{font-size:26px;line-height:1.2;font-weight:750;letter-spacing:-.4px;margin:5px 0 7px;word-break:break-word}";
        h += ".ahead .sub{font-size:12.5px;color:var(--mut);margin-bottom:13px}";
    h += ".bio-box{font-size:13px;line-height:1.75;color:var(--txt2);white-space:pre-wrap;word-break:break-word}";
    h += ".bio-box.collapsed{max-height:140px;overflow:hidden;-webkit-mask-image:linear-gradient(180deg,#000 65%,transparent)}";
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
    h += ".bar{grid-column:1/-1;display:grid;grid-template-columns:minmax(200px,300px) 1fr minmax(170px,232px);align-items:center;gap:18px;padding:0 18px;background:var(--barbg);border-top:1px solid var(--line)}";
    h += ".np{display:flex;align-items:center;gap:11px;min-width:0;cursor:pointer;border-radius:10px;padding:4px 6px;margin-left:-6px;transition:.14s}";
    h += ".np:hover{background:var(--hov)}";
    h += ".cv2{width:56px;height:56px;border-radius:9px;flex:none;background:var(--cvbg) center/cover no-repeat;border:1px solid var(--line);box-shadow:0 3px 14px var(--shadow)}";
    h += ".np .tx{min-width:0}.np .t1{font-size:12.5px;font-weight:600;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}";
    h += ".np .t2{font-size:10.5px;color:var(--mut2);white-space:nowrap;overflow:hidden;text-overflow:ellipsis;margin-top:2px}";
    h += ".ctr{display:flex;flex-direction:column;align-items:center;gap:7px}.keys{display:flex;align-items:center;gap:6px}";
    h += "#bRep{position:relative}";
    // 正在播放整页
    // 2026-10-23 用户反馈「单曲播放页面四周有黑框」：
    //   宿主（launcher）为了让 WM_NCHITTEST 拿到边缘缩放热区，把 WebView 内缩了 5px（kBandInset），
    //   那 5px 由宿主按**主题 --bg** 上色。普通页面看不出来，但这个整页是全屏封面/深色底，
    //   于是四周露出一圈与页面不同的边带 = 用户看到的"黑框"。
    //   修法：把边带**延伸到页面外侧**（负 inset + padding 补偿），让页面内容盖住那条缝；
    //   同时保留 5px 给宿主做缩放热区（负 inset 只是视觉外扩，不抢命中测试）。
    h += "#npPage{position:fixed;top:-6px;left:-6px;right:-6px;bottom:-6px;z-index:70;display:none;background:var(--bg);overflow:hidden;padding:6px;box-sizing:border-box}";
    h += "#npPage.on{display:block}";
    // 拖拽条：贴顶 34px，透明，不挡下方内容（z-index 高于背景、低于关闭按钮）
    h += "#npPage .npgrip{position:absolute;top:0;left:0;right:0;height:34px;z-index:5;cursor:grab;background:transparent}";
    h += "#npPage .npgrip:active{cursor:grabbing}";
    h += ".npbg{position:absolute;inset:-60px;background-size:cover;background-position:center;filter:blur(64px) saturate(1.15) brightness(.38);transform:scale(1.08)}";
    h += ".npveil{position:absolute;inset:0;background:linear-gradient(180deg,rgba(8,9,12,.35),rgba(8,9,12,.82))}";
    h += ".npwrap{position:relative;height:100%;display:grid;grid-template-columns:minmax(280px,420px) 1fr;gap:64px;align-items:center;padding:48px 84px 110px;z-index:2}";
    h += ".npctl{position:absolute;left:0;right:0;bottom:34px;display:flex;flex-direction:column;align-items:center;gap:12px;z-index:3}";
    // 时长行的 "/" 必须与下面播放键的中心严格对齐（用户 2026-10-22 UI3）。
    // 为什么「整行居中」不够：播放键两侧按钮数不同（左 2 右 2 但按钮宽度不同），
    // 更早的版本整行 flex 居中时播放键本身并不在容器正中 —— 那正是用户看到
    // 「/ 没有跟播放键对齐」的原因（第十二轮改成"居中"只解决了整行，没解决这一层）。
    // 现在的做法：时长行与播放键行【共用同一条 3 列网格 + 同一个宽度】，
    //   列 = 1fr | auto | 1fr。
    //   左右 1fr 由网格保证严格等宽 ⟹ 中间列的水平中心 == 容器水平中心；
    //   时长行的 "/" 放在中间列，播放键也放在中间列 ⟹ 两者中心必然重合。
    // 宽度必须两边一样（都取 --npctl-w），否则两个容器中心不同，对齐又跑了。
    h += ":root{--npctl-w:min(720px,70vw)}";
    h += ".npkeys{display:grid;grid-template-columns:1fr auto 1fr;align-items:center;gap:14px;width:var(--npctl-w)}";
    h += ".npkeys .grp{display:flex;align-items:center;gap:14px}";
    h += ".npkeys .grp.l{justify-content:flex-end}.npkeys .grp.r{justify-content:flex-start}";
    h += ".nptime{font-size:12px;color:var(--mut);display:grid;grid-template-columns:1fr auto 1fr;align-items:center;gap:14px;font-variant-numeric:tabular-nums;width:var(--npctl-w)}";
    h += ".nptime .tside{display:flex;align-items:center;gap:6px}";
    h += ".nptime .tside.l{justify-content:flex-end}.nptime .tside.r{justify-content:flex-start}";
    h += ".npseek{width:min(720px,70vw);height:4px;border-radius:2px;background:rgba(255,255,255,.18);position:relative;cursor:pointer}";
    h += ".npseek>i{position:absolute;left:0;top:0;bottom:0;border-radius:2px;background:var(--acc);width:0}";
    h += ".nptime .tlab{font-size:10px;color:var(--mut2);letter-spacing:.4px}";
    h += ".nptime .tval{min-width:42px;text-align:center;font-weight:600;color:var(--txt2)}";
    h += ".nptime .tsep{opacity:.45;margin:0 1px;text-align:center;min-width:10px}";
    // 整页的音量条（用户 UI2：单曲播放页缺音量调节）
    h += ".npvol{display:flex;align-items:center;gap:9px;color:var(--mut);width:min(380px,42vw)}";
    h += ".npvol input[type=range]{flex:1;accent-color:var(--acc)}";
    h += ".npvol .vlab{font-size:11px;min-width:44px;text-align:right;font-variant-numeric:tabular-nums;color:var(--txt2)}";
    h += ".npcover img{width:100%;aspect-ratio:1/1;object-fit:cover;border-radius:14px;box-shadow:0 24px 70px rgba(0,0,0,.6)}";
    h += ".nptit{font-size:26px;font-weight:700;margin-top:26px;line-height:1.3}";
    h += ".npsub{font-size:13px;color:var(--mut);margin-top:8px;line-height:1.7}";
    h += ".nplyric{height:58vh;overflow:auto;scrollbar-width:none;text-align:center}";
    h += ".nplyric::-webkit-scrollbar{display:none}";
    h += ".npline{font-size:17px;line-height:2.4;color:var(--mut2);transition:.18s}";
    h += ".npline.on{color:var(--txt);font-weight:650;transform:scale(1.04)}";
    h += ".npclose{position:absolute;top:18px;left:18px;width:36px;height:36px;border-radius:50%;background:rgba(255,255,255,.12);display:flex;align-items:center;justify-content:center;color:var(--txt);z-index:3}";
    h += ".npclose:hover{background:rgba(255,255,255,.16)}";
    // 浅色/浅蓝主题：整页跟随主题，Windows 原生标题栏（已按同一主题上色）才不会
    // 出现"上面一条浅色、下面一整块深色"的割裂感。
    h += "html[data-theme=light] .npbg,html[data-theme=blue] .npbg{filter:blur(64px) saturate(1.05) brightness(1.12)}";
    h += "html[data-theme=light] .npveil{background:linear-gradient(180deg,rgba(244,246,249,.62),rgba(244,246,249,.95))}";
    h += "html[data-theme=blue] .npveil{background:linear-gradient(180deg,rgba(233,242,255,.64),rgba(233,242,255,.96))}";
    h += "html[data-theme=light] .npclose,html[data-theme=blue] .npclose{background:rgba(15,23,42,.08);color:var(--txt)}";
    h += "html[data-theme=light] .npclose:hover,html[data-theme=blue] .npclose:hover{background:rgba(15,23,42,.15)}";
    h += "html[data-theme=light] .npseek,html[data-theme=blue] .npseek{background:rgba(15,23,42,.16)}";
    h += "html[data-theme=light] .npcover img,html[data-theme=blue] .npcover img{box-shadow:0 24px 70px rgba(15,23,42,.22)}";
    h += ".k{width:32px;height:32px;border-radius:9px;display:grid;place-items:center;color:var(--mut);transition:.13s}";
    h += ".k:hover{background:var(--hov2);color:var(--txt)}";
    h += ".k.big{width:40px;height:40px;border-radius:50%;background:linear-gradient(180deg,var(--acc),var(--acc2));color:#fff;box-shadow:0 3px 16px var(--acc-weak)}";
    h += ".k.big:hover{filter:brightness(1.1)}";
    h += ".prog{width:100%;display:flex;align-items:center;gap:9px;font-size:10.5px;color:var(--mut2);font-variant-numeric:tabular-nums;max-width:560px}";
    h += ".track{flex:1;height:5px;border-radius:3px;background:var(--track);overflow:hidden;cursor:pointer;position:relative}";
    h += ".track i{display:block;height:100%;border-radius:3px;background:linear-gradient(90deg,var(--acc),var(--acc2));transition:width .18s linear;width:0}";
    h += ".vol{display:flex;align-items:center;gap:8px;justify-content:flex-end}.vol input[type=range]{width:88px;accent-color:var(--acc)}";
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
    h += "#lyricbox{height:100%;overflow:auto;padding:18px 20px;text-align:center}";
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
    h += "#modal{position:fixed;inset:0;z-index:40;background:rgba(0,0,0,.45);backdrop-filter:blur(3px);display:none;place-items:center}";
    h += "#modal.on{display:grid}";
    h += ".mcard{width:min(600px,92vw);max-height:84vh;overflow:auto;background:var(--bg2);border:1px solid var(--line);border-radius:14px;box-shadow:0 24px 70px var(--shadow)}";
    h += ".mhead{display:flex;align-items:center;gap:9px;padding:14px 18px;border-bottom:1px solid var(--line);font-weight:650}";
    h += ".mbody{padding:16px 18px 22px}";
    h += ".chain{display:flex;flex-direction:column;gap:0}";
    h += ".cnode{display:flex;gap:12px;align-items:flex-start;padding:9px 0}";
    h += ".cnode .dot{width:9px;height:9px;border-radius:50%;background:var(--acc);margin-top:6px;flex:none;box-shadow:0 0 12px var(--acc-weak)}";
    h += ".cnode.gpu .dot{background:var(--ok);box-shadow:0 0 12px var(--ok-line)}";
    h += ".cnode .k{font-size:10px;letter-spacing:1.1px;color:var(--mut2);font-weight:600;white-space:nowrap}";
    h += ".cnode > div:last-child{min-width:0}";
    h += ".cnode .v{font-size:12.5px;color:var(--txt);margin-top:1px}";
    h += ".cline{width:1px;height:14px;background:var(--line);margin-left:4px}";
    // 设置页
    h += "#setpage{display:none;padding:20px 24px 60px}";
    h += "#setpage.on{display:block}";
    h += ".sgroup{margin-bottom:22px;border:1px solid var(--line);border-radius:12px;background:var(--bg2);overflow:hidden}";
    h += ".sghead{display:flex;align-items:center;gap:9px;padding:11px 16px;border-bottom:1px solid var(--line);font-weight:650;font-size:12.5px;background:var(--bg3)}";
    h += ".sghead .hint{font-weight:400;font-size:11px;color:var(--mut2)}";
    h += ".sgbody{padding:14px 16px;display:grid;grid-template-columns:repeat(auto-fill,minmax(260px,1fr));gap:14px 20px}";
    h += ".sfld{min-width:0}";
    h += ".sfld label{display:block;font-size:11px;color:var(--mut);margin-bottom:5px;font-weight:500}";
    h += ".sfld .hint{font-size:10.5px;color:var(--mut2);margin-top:4px;line-height:1.5}";
    h += ".sfld select,.sfld input[type=text],.sfld input[type=number]{width:100%;padding:7px 10px;border-radius:8px;border:1px solid var(--line);background:var(--field);font-size:12px;transition:.15s}";
    h += ".sfld select:focus,.sfld input:focus{outline:0;border-color:var(--acc);box-shadow:0 0 0 3px var(--acc-weak)}";
    h += ".sfld.wide{grid-column:1/-1}";
    h += ".sfld .chk{margin:0}";
    // 设置页分页（对齐 HQPlayer 的 Inputs / Outputs / PCM / SDM / Advanced）
    h += ".stabs{display:flex;gap:4px;padding:10px 0 12px;margin:-20px 0 14px;border-bottom:1px solid var(--line);position:sticky;top:0;background:var(--bg);z-index:6}";
    h += ".stab{padding:7px 15px;border-radius:8px;font-size:12px;color:var(--mut);border:1px solid transparent;transition:.13s}";
    h += ".stab:hover{background:var(--hov);color:var(--txt2)}";
    h += ".stab.on{background:var(--acc-weak);color:var(--acc-text);border-color:rgba(59,130,246,.32);font-weight:600}";
    h += ".spane{display:none}.spane.on{display:block}";
    h += ".trimgrid{display:grid;grid-template-columns:repeat(4,minmax(0,1fr));gap:10px}";
    h += ".setfoot{display:flex;gap:10px;align-items:center;padding:4px 2px 0}";
    h += ".setfoot .sp{flex:1}";
    h += ".meter-bar{height:6px;border-radius:3px;background:var(--track);overflow:hidden;position:relative;margin:4px 0 2px}";
    h += ".meter-val{height:100%;width:0%;background:linear-gradient(90deg,var(--acc),#22c55e);transition:width .12s ease;border-radius:3px}";
    h += ".meter-val.clip{background:var(--err)}";
    h += ".in-stat-grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(180px,1fr));gap:8px 12px;font-size:11.5px;color:var(--txt2)}";
    h += ".in-stat-item{background:var(--field);padding:7px 10px;border-radius:8px;border:1px solid var(--line)}";
    h += ".in-stat-item .lbl{display:block;font-size:10px;color:var(--mut2);margin-bottom:2px}";
    h += ".in-stat-item .val{font-weight:600;font-variant-numeric:tabular-nums}";
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
    h += "<div class=nav data-v=works><svg viewBox='0 0 24 24' fill='none' stroke='currentColor' stroke-width='1.8'><path d='M9 18V5l12-2v13'/><circle cx='6' cy='18' r='3'/><circle cx='18' cy='16' r='3'/></svg>古典作品</div>";
    h += "<div class=nav data-v=tracks><svg viewBox='0 0 24 24' fill='none' stroke='currentColor' stroke-width='1.8'><path d='M14.5 4.5v11.2'/><circle cx='12' cy='17.5' r='2.8'/><path d='M14.5 4.5c2.6.6 4 2 4.5 4.2'/></svg>曲目</div>";
    h += "<div class=nav data-v=files><svg viewBox='0 0 24 24' fill='none' stroke='currentColor' stroke-width='1.8'><path d='M3 7.5a2 2 0 012-2h4l1.8 2H19a2 2 0 012 2v8a2 2 0 01-2 2H5a2 2 0 01-2-2z'/></svg>文件夹</div>";
    h += "<div class=nav data-v=remote><svg viewBox='0 0 24 24' fill='none' stroke='currentColor' stroke-width='1.8'><path d='M7 18a4.5 4.5 0 010-9 6 6 0 0111.6 1.6A3.7 3.7 0 0118 18z'/></svg>流媒体</div>";
    h += "<div class=nav data-v=eq><svg viewBox='0 0 24 24' fill='none' stroke='currentColor' stroke-width='1.8'><path d='M4 21v-7M4 10V3M12 21v-9M12 8V3M20 21v-5M20 12V3M1 14h6M9 8h6M17 16h6'/></svg>EQ 与房间校正</div>";
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
    // 不点开信号链也能一眼看到「本次是否逐位」；颜色由 ui_script 按 direct/yes/no 三态设置。
    h += "<div id=bpBadge style=\"font-size:10.5px;font-weight:700;letter-spacing:.4px;margin-left:12px;white-space:nowrap\" title=\"\"></div>";
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
    h += "<span class=badge id=fmt>--</span><span class='badge g' id=gpub style='display:none'>GPU</span>";
    h += "<svg width=14 height=14 viewBox='0 0 24 24' fill=none stroke=currentColor stroke-width=2 opacity=.55><path d='M3 10v4h4l5 4V6L7 10H3z'/><path d='M16.5 8.5a5 5 0 010 7'/></svg>";
    h += "<input id=vol type=range min=-60 max=0 step=0.5 value=0></div></div>";
    // ---------- 正在播放（点封面/曲名打开） ----------
    h += "<div id=npPage>";
    h += "<div class=npbg id=npbg></div><div class=npveil></div>";
    // 2026-10-23 用户反馈「单曲播放页面无法拖动窗口（主页面可以）」：
    //   主页面的拖拽热区在顶栏，而 npPage 是 fixed 全屏覆盖层、把顶栏盖住 → 没有可拖区域。
    //   这里加一条覆盖层自己的拖拽条：透明、贴顶、高度 34px，鼠标按下时通过
    //   chrome.webview.postMessage("drag") 交给 launcher 的 WM_NCLBUTTONDOWN/HTCAPTION。
    h += "<div class=npgrip id=npGrip title='拖拽移动窗口'></div>";
    h += "<button class=npclose id=npClose title='返回'><svg width=16 height=16 viewBox='0 0 24 24' fill=none stroke=currentColor stroke-width=2><path d='M15 5l-7 7 7 7'/></svg></button>";
    h += "<div class=npwrap>";
    h += "<div><div class=npcover><img id=npCover alt=''></div>";
    // npMeta：元数据补全信息芯片容器（year/genre/albumArtist/已补全），由 ui_script 的 renderNp 填充。
    // 只进整页，底栏不加（避免拥挤）；样式只用布局属性，颜色交给 .badge 自身的 CSS 变量。
    h += "<div style='display:flex;align-items:center;gap:10px;margin-top:20px'>";
    h += "<div class=nptit id=npTitle style='margin:0'>未播放</div>";
    h += "<button class=fav-btn id=npFavBtn title='收藏' style='padding:4px;display:none'><svg width=20 height=20 viewBox='0 0 24 24' fill=none stroke='currentColor' stroke-width=2><path d='M20.84 4.61a5.5 5.5 0 00-7.78 0L12 5.67l-1.06-1.06a5.5 5.5 0 00-7.78 7.78l1.06 1.06L12 21.23l7.78-7.78 1.06-1.06a5.5 5.5 0 000-7.78z'/></svg></button>";
    h += "</div><div class=npsub id=npSub></div>";
    h += "<div id=npMeta style='display:flex;flex-wrap:wrap;gap:6px;margin-top:8px'></div></div>";
    h += "<div class=nplyric id=npLyric><div class=npline>还没有在播放</div></div>";
    h += "</div>";
    // 播放控件：整页也要能操作，否则进来就必须退出
    h += "<div class=npctl>";
    h += "<div class=npseek id=npSeek title='点击跳转'><i id=npBar></i></div>";
    // 时长行：3 列网格，中间列居中放「当前 x:xx / 总时长 y:yy」里的分隔符 "/"。
    // 因为左右两列都是 1fr（等宽），中间列的中心 == 容器中心；下面 .npkeys 的播放键
    // 也落在中间列中心 ⟹ 两者严格对齐（用户 UI3；详见 CSS 注）。
    h += "<div class=nptime>";
    h += "<div class='tside l'><span class=tlab>当前</span><span class=tval id=npPos>0:00</span></div>";
    h += "<span class=tsep id=npSep>/</span>";
    h += "<div class='tside r'><span class=tlab>总时长</span><span class=tval id=npDur>0:00</span></div>";
    h += "</div>";
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
    h += "</div></div>";
    // 音量行：滑块本身是 #vol 的镜像（同一个 S.vol 真相源，见 setVol）
    h += "<div class=npvol><svg width='14' height='14' viewBox='0 0 24 24' fill=none stroke='currentColor' stroke-width='2' opacity=.6><path d='M3 10v4h4l5 4V6L7 10H3z'/><path d='M16.5 8.5a5 5 0 010 7'/></svg>";
    h += "<input id=npVol type=range min=-60 max=0 step=0.5 value=0 title='音量（dB）'>";
    h += "<span class=vlab id=npVolLab>0.0 dB</span></div></div>";
    h += "</div>";

    h += "<div id=modal><div class=mcard><div class=mhead><b id=mtitle>信号链</b><span class=sp></span><button class=ibtn id=mclose><svg width=15 height=15 viewBox='0 0 24 24' fill=none stroke=currentColor stroke-width=2><path d='M18 6L6 18M6 6l12 12'/></svg></button></div><div class=mbody id=mbody></div></div></div>";
    h += "<div class=toast id=toast></div></div>";
    h += "<script src=/app.js></script></body></html>";
    return h;
}

} // namespace rta::net