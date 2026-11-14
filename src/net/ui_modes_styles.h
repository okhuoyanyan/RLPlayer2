#pragma once

// Presentation-only materials for the two workspaces. The native window owns
// the outer background and resize surfaces; the player owns its status colors.
inline const char* rta_ui_modes_css() {
    return R"RLMODES(
html[data-rl-mode] {
  --rl-serif: Georgia, "Noto Serif CJK SC", "Songti SC", "SimSun", serif;
  --rl-error:#e7a99b;
  --rl-wood: linear-gradient(90deg,#36251f 0%,#694832 31%,#533a2d 57%,#31221e 100%);
  --rl-grain: repeating-radial-gradient(ellipse 11px 118px at 85% 48%,
    transparent 0 3px,rgba(246,209,147,.13) 3.2px 3.7px,transparent 4px 7px);
}
html[data-rl-mode][data-theme=light],html[data-rl-mode][data-theme=blue] {
  --rl-error:#a5332a;
}
html[data-rl-mode=listen] {
  --rl-ground:#101c1e; --rl-side:#122124; --rl-panel:#1c3032; --rl-raised:#213739;
  --rl-field:#132629; --rl-title:#eff4ed; --rl-copy:#ccdad3; --rl-muted:#adc2bc;
  --rl-line:#3a5050; --rl-accent:#b9dcc6; --rl-ink:#132f26; --rl-link:#c4e3d0;
  --rl-tint:#2b4443; --rl-record-label:#9db9ad;
}
html[data-rl-mode=listen][data-theme=light] {
  --rl-ground:#f3f6f1; --rl-side:#edf2ed; --rl-panel:#ffffff; --rl-raised:#e6efe8;
  --rl-field:#ffffff; --rl-title:#182e29; --rl-copy:#344f46; --rl-muted:#4f665d;
  --rl-line:#b8c9be; --rl-accent:#295d4c; --rl-ink:#ffffff; --rl-link:#205a46;
  --rl-tint:#dceade; --rl-record-label:#97b9a7;
}
html[data-rl-mode=listen][data-theme=blue] {
  --rl-ground:#eaf2f7; --rl-side:#e3edf3; --rl-panel:#f8fcfe; --rl-raised:#dceaf0;
  --rl-field:#ffffff; --rl-title:#193746; --rl-copy:#2c4e5c; --rl-muted:#476371;
  --rl-line:#acc3cd; --rl-accent:#2d6271; --rl-ink:#ffffff; --rl-link:#24566a;
  --rl-tint:#d3e5ed; --rl-record-label:#9dbbc0;
}
html[data-rl-mode=pro] {
  --rl-ground:#201d19; --rl-side:#28231e; --rl-panel:#302b23; --rl-raised:#3b3329;
  --rl-field:#211e1a; --rl-title:#f4e9d4; --rl-copy:#e1d5c1; --rl-muted:#c2afa0;
  --rl-line:#635546; --rl-accent:#e5c38a; --rl-ink:#342618; --rl-link:#eed1a0;
  --rl-tint:#493d2e; --rl-record-label:#b18c54;
}
html[data-rl-mode=pro][data-theme=light] {
  --rl-ground:#f5efe2; --rl-side:#ede4d3; --rl-panel:#fbf7ec; --rl-raised:#eae0ca;
  --rl-field:#fffdf6; --rl-title:#342c24; --rl-copy:#4e4337; --rl-muted:#685749;
  --rl-line:#c6b69b; --rl-accent:#70542d; --rl-ink:#fff9ef; --rl-link:#684a24;
  --rl-tint:#e8dabc; --rl-record-label:#b58c56;
}
html[data-rl-mode=pro][data-theme=blue] {
  --rl-ground:#e9edf0; --rl-side:#e1e5e6; --rl-panel:#f6f7f4; --rl-raised:#dce2e1;
  --rl-field:#fbfcfb; --rl-title:#253642; --rl-copy:#374c55; --rl-muted:#50616b;
  --rl-line:#b5bfc0; --rl-accent:#735631; --rl-ink:#fff9ee; --rl-link:#664925;
  --rl-tint:#e5dccb; --rl-record-label:#a98858;
}

/* Shell and legacy panels share the local palette without changing ISO/GPU,
   bit-perfect, warning, or meter semantics. */
html[data-rl-mode] #app {
  --bg2:var(--rl-side); --bg3:var(--rl-raised); --field:var(--rl-field);
  --card:var(--rl-panel); --txt:var(--rl-title); --txt2:var(--rl-copy);
  --mut:var(--rl-muted); --mut2:var(--rl-muted); --line:var(--rl-line);
  --acc:var(--rl-accent); --acc2:var(--rl-accent); --acc-text:var(--rl-link);
  --acc-weak:var(--rl-tint); --nav-on:var(--rl-tint); --nav-on-text:var(--rl-title);
  --rl-gutter:clamp(16px,2.6vw,44px);
}
html[data-rl-mode] #app > .main { min-width:0; background:var(--rl-ground); color:var(--rl-title); }
html[data-rl-mode] #app > .top {
  border-bottom:1px solid var(--rl-line); background:var(--rl-side); color:var(--rl-title);
}
html[data-rl-mode] #app > .side,html[data-rl-mode] #right { background:var(--rl-side); }
html[data-rl-mode] #app > .side .nav { border-radius:6px; }
html[data-rl-mode] #app > .bar { background:var(--rl-side); color:var(--rl-title); }
html[data-rl-mode] #app :is(.btn.pri,.k.big) {
  background:var(--rl-accent); color:var(--rl-ink); box-shadow:none;
}
html[data-rl-mode] #app .nav-badge { color:var(--rl-ink); }
html[data-rl-mode] #topbar :is(.wbtn,.ibtn),html[data-rl-mode] #app>.bar .k {
  transition-property:background-color,color,box-shadow;
}
html[data-rl-mode] #app .stabs,html[data-rl-mode] #app .setfoot { background:var(--rl-ground); }
html[data-rl-mode] #app .stabs {
  overflow-x:auto; overscroll-behavior-inline:contain; scrollbar-width:thin;
  scroll-padding-inline:6px; max-width:100%;
}
html[data-rl-mode] #app .stab { flex:none; }
html[data-rl-mode] #app .stab.on { color:var(--rl-ink); background:var(--rl-accent); }
#rlViewModes {
  display:inline-flex; align-items:center; flex:0 0 auto; gap:2px;
  border:1px solid var(--rl-line); border-radius:24px; padding:3px;
  background:var(--rl-field); max-width:100%;
}
#rlViewModes [data-rl-mode] {
  display:inline-flex; align-items:center; justify-content:center; min-height:27px;
  padding:3px 12px; color:var(--rl-muted); line-height:1.2; font-weight:550;
  white-space:nowrap; border:0; border-radius:20px;
}
#rlViewModes [aria-pressed=true] { background:var(--rl-accent); color:var(--rl-ink); }
#rlViewModes [aria-pressed=false]:hover { background:var(--rl-tint); color:var(--rl-title); }
html[data-rl-mode] #app :is(button,summary,.nav,select,input,a):focus-visible {
  outline:2px solid var(--rl-link); outline-offset:2px;
}
html[data-rl-mode] #topbar .search { min-width:0; }

#rlListenHome {
  container:rlhome / inline-size; width:100%; max-width:1380px; margin-inline:auto;
  padding:clamp(18px,3.5vh,43px) var(--rl-gutter) 0; color:var(--rl-title);
}
.rlListenHeading,.rlQueueHeading {
  display:flex; align-items:baseline; justify-content:space-between; flex-wrap:wrap;
  gap:7px 18px; min-width:0;
}
.rlListenHeading { margin-block-end:clamp(14px,2.4vh,24px); }
.rlListenHeading h1 {
  font-size:clamp(28px,3.15vw,46px); font-weight:650; line-height:1.22; letter-spacing:-.045em;
  overflow-wrap:anywhere;
}
#rlListenDevice { color:var(--rl-copy); font-size:13px; overflow-wrap:anywhere; max-width:100%; }
.rlQueueHeading>.btn,#rlListenDevice {
  max-width:100%; white-space:normal; overflow-wrap:anywhere; text-align:start;
}
.rlListenGrid { display:grid; grid-template-columns:minmax(0,1fr); gap:20px 28px; }
.rlListenHero {
  display:grid; grid-template-columns:minmax(0,.82fr) minmax(0,1.1fr); align-items:center;
  gap:clamp(14px,3cqi,40px); min-width:0; padding:clamp(17px,2.8cqi,36px);
  background:var(--rl-panel); border:1px solid var(--rl-line); border-radius:10px;
}
.rlListenCover {
  display:block; position:relative; isolation:isolate; align-self:center;
  width:100%; aspect-ratio:1; overflow:hidden; border-radius:3px;
  background-color:var(--rl-raised); background-position:center; background-repeat:no-repeat;
  background-size:cover; box-shadow:0 6px 18px rgba(0,0,0,.22);
}
.rlListenCover img { display:block; width:100%; height:100%; object-fit:cover; }
.rlListenCover:not(.has-art):not(:has(img)) {
  background:linear-gradient(139deg,var(--rl-tint),var(--rl-field));
}
/* The grooves denote a missing sleeve; an artist's actual cover is unobscured. */
.rlListenCover:not(.has-art):not(:has(img))::before {
  content:""; position:absolute; inset:9%; border-radius:50%; pointer-events:none;
  background:
    radial-gradient(circle,#171a18 0 1.4%,#e3dac2 1.6% 2%,var(--rl-record-label) 2.1% 22%,transparent 22.4%),
    conic-gradient(from 18deg,transparent 0deg,rgba(255,255,255,.12) 52deg,transparent 89deg 170deg,rgba(255,255,255,.11) 221deg,transparent 268deg),
    repeating-radial-gradient(circle,#191b19 0 1.3px,#313431 1.4px 1.8px,#1c1e1b 2.2px 3.4px);
  box-shadow:0 3px 8px #0007,inset 0 0 0 1px #161915;
}
.rlListenInfo,.rlListenQueue,.rlListenActions { min-width:0; }
.rlListenStatus { display:block; font-size:12px; font-weight:600; color:var(--rl-muted); }
.rlListenTitle {
  display:-webkit-box; -webkit-box-orient:vertical; -webkit-line-clamp:3; overflow:hidden;
  margin-block:9px 5px; font-size:clamp(24px,3.7cqi,52px); line-height:1.2;
  font-weight:650; letter-spacing:-.045em; overflow-wrap:anywhere;
}
.rlListenMeta { color:var(--rl-copy); line-height:1.6; overflow-wrap:anywhere; }
.rlListenActions { display:flex; flex-wrap:wrap; gap:8px; margin-block-start:clamp(12px,2.1cqi,28px); }
.rlListenActions .btn { min-height:35px; white-space:normal; }
.rlListenQueue { align-self:stretch; padding:clamp(10px,1.5cqi,20px) 0; }
.rlQueueHeading h2 { font-size:20px; font-weight:650; line-height:1.35; }
.rlQueueHeading { padding-block-end:9px; border-bottom:1px solid var(--rl-line); }
.rlQueueHeading :is(button,small,span), #rlListenQueueList>p { color:var(--rl-muted); }
#rlListenQueueList { display:grid; align-content:start; }
#rlListenQueueList>p { padding-block:16px; line-height:1.65; }
.rlListenQueueRow {
  display:flex; align-items:baseline; width:100%; gap:12px; min-width:0; min-height:39px;
  text-align:start; padding:9px 4px; border-bottom:1px solid var(--rl-line); color:var(--rl-copy);
}
.rlListenQueueRow:is(button,[href]):hover { background:var(--rl-tint); color:var(--rl-title); }
.rlListenQueueRow:is(.on,[aria-current=true],[aria-current=step]) .rlQueueTitle {
  color:var(--rl-link); font-weight:700;
}
.rlQueueNumber { color:var(--rl-muted); min-width:2ch; font-variant-numeric:tabular-nums; font-size:12px; }
.rlQueueTitle { overflow:hidden; text-overflow:ellipsis; white-space:nowrap; min-width:0; }
#rlHomeShelf { margin-block-start:clamp(18px,2.5cqi,30px); }
.rlShelfItems {
  display:grid; grid-template-columns:repeat(auto-fit,minmax(min(100%,180px),1fr)); gap:8px 18px;
}
.rlShelfItem {
  display:flex; align-items:center; gap:12px; min-width:0; padding:9px 2px;
  color:var(--rl-title); text-align:start; border-bottom:1px solid var(--rl-line);
}
.rlShelfItem:hover { background:var(--rl-tint); }
.rlShelfCover {
  display:grid; place-items:center; flex:0 0 53px; aspect-ratio:1; overflow:hidden; border-radius:3px;
  background:var(--rl-raised);
}
.rlShelfCover:empty,.rlShelfCover:has(img[hidden]) {
  background:
    radial-gradient(circle,var(--rl-record-label) 0 11%,transparent 12%),
    radial-gradient(circle,#282c28 0 52%,transparent 54%),var(--rl-raised);
}
.rlShelfCover img { width:100%; height:100%; object-fit:cover; }
.rlShelfCover img[hidden] { display:none; }
.rlShelfItem>span:last-child { min-width:0; }
.rlShelfItem strong,.rlShelfItem small {
  display:block; overflow:hidden; text-overflow:ellipsis; white-space:nowrap;
}
.rlShelfItem small { margin-block-start:2px; color:var(--rl-muted); }
@container rlhome (min-width:730px) {
  .rlListenGrid { grid-template-columns:minmax(0,1.9fr) minmax(220px,.8fr); }
}
@container rlhome (max-width:470px) {
  .rlListenHero { grid-template-columns:minmax(80px,.6fr) minmax(0,1fr); padding:12px; gap:12px; }
  .rlListenTitle { font-size:23px; } .rlListenMeta { font-size:13px; }
  .rlListenActions .btn { font-size:12px; padding-inline:9px; }
}
@container rlhome (max-width:290px) {
  .rlListenHero { grid-template-columns:1fr; }
  .rlListenCover { width:min(100%,180px); justify-self:center; }
}
#rlQuickSetup {
  margin:clamp(16px,2.5vh,30px) auto 25px; width:calc(100% - 2 * var(--rl-gutter));
  max-width:calc(1380px - 2 * var(--rl-gutter)); color:var(--rl-title);
  border-top:1px solid var(--rl-line); border-bottom:1px solid var(--rl-line);
}
#rlQuickSetup>summary { padding:15px 4px; color:var(--rl-copy); font-weight:600; cursor:pointer; }
#rlQuickSetup>summary:hover { color:var(--rl-link); }
#rlQuickSetup .quick-start { max-width:none; padding:6px 4px 22px; }

#rlListenSettings,#rlSettingsBack,#rlProPath { display:none; }
html[data-rl-mode=listen] #setpage[data-rl-settings=home] #rlListenSettings { display:block; }
html[data-rl-mode=listen] #setpage[data-rl-settings=home] :is(.stabs,.spane,.setfoot) { display:none; }
html[data-rl-mode=listen] #setpage[data-rl-settings=detail] #rlSettingsBack {
  display:inline-flex; align-items:center; gap:7px; margin-block-end:10px;
  color:var(--rl-link); font-weight:600; cursor:pointer;
}
#rlListenSettings { max-width:980px; padding-block:8px 20px; }
#rlListenSettings h1,#rlListenSettings h2 { margin-block-end:14px; color:var(--rl-title); }
#rlListenSettings>p { margin-block:0 16px; color:var(--rl-muted); }
#rlListenSettings>.btn { margin-block-start:14px; }
.rlSettingTasks { display:grid; grid-template-columns:repeat(auto-fit,minmax(min(100%,220px),1fr)); gap:12px; }
.rlSettingTasks>button {
  display:flex; flex-direction:column; justify-content:flex-start; align-items:flex-start;
  gap:6px; min-width:0; min-height:77px; padding:15px 16px; text-align:start;
  color:var(--rl-title); background:var(--rl-panel); border:1px solid var(--rl-line); border-radius:7px;
}
.rlSettingTasks>button:hover { background:var(--rl-tint); border-color:var(--rl-link); }
.rlSettingTasks strong { font-size:15px; } .rlSettingTasks small { color:var(--rl-muted); font-size:12px; }

/* A two-cheek instrument enclosure. Grain is confined to the cheeks, outside
   the reading surface. The time ruler marks the real seek control. */
html[data-rl-mode=pro] #app > .side {
  padding-inline-start:14px;
  background:
    var(--rl-grain) left top / 7px 100% no-repeat,
    var(--rl-wood) left top / 7px 100% no-repeat,var(--rl-side);
}
html[data-rl-mode=pro] .rlListenHero {
  position:relative; border-radius:5px; padding-inline:clamp(22px,3.1cqi,44px);
  background:
    var(--rl-grain) left top / 9px 100% no-repeat,var(--rl-wood) left top / 9px 100% no-repeat,
    var(--rl-grain) right bottom / 9px 100% no-repeat,var(--rl-wood) right top / 9px 100% no-repeat,
    var(--rl-panel);
}
html[data-rl-mode=pro] .rlListenCover:is(.has-art,:has(img)) {
  --rl-disc-peek:min(8%,15px); overflow:visible;
}
html[data-rl-mode=pro] .rlListenCover:is(.has-art,:has(img))::after {
  content:""; position:absolute; right:calc(-1 * var(--rl-disc-peek)); top:5%;
  width:90%; aspect-ratio:1; border-radius:50%; pointer-events:none;
  clip-path:inset(0 0 0 calc(100% - var(--rl-disc-peek)));
  background:conic-gradient(from 35deg,transparent 0deg,rgba(255,255,255,.13) 64deg,transparent 100deg 280deg),
    repeating-radial-gradient(circle,#151813 0 1.3px,#3d4139 1.5px 2px,#181b15 2.3px 3.4px);
}
html[data-rl-mode=pro] .rlListenCover img { border-radius:inherit; }
html[data-rl-mode=pro] :is(.rlListenHeading h1,.rlListenTitle,.rlQueueHeading h2,.rlProPath h2) {
  font-family:var(--rl-serif); font-weight:650; letter-spacing:.01em;
}
html[data-rl-mode=pro] #app .sghead { color:var(--rl-title); border-start-start-radius:5px; border-start-end-radius:5px; }
html[data-rl-mode=pro] #app :is(.sgroup,.stabs .stab,.qs-profile,.qs-caps) { border-radius:5px; }
html[data-rl-mode=pro] #app > .bar {
  background:var(--rl-side); border-top-color:var(--rl-line);
}
html[data-rl-mode=pro] #app #seek {
  background:repeating-linear-gradient(90deg,var(--rl-line) 0 1px,transparent 1px 10%),
    var(--rl-tint);
}
html[data-rl-mode=pro] #app #seek i { background:var(--rl-accent); }
html[data-rl-mode=pro] #app :is(#tpos,#tdur) { color:var(--rl-link); font-variant-numeric:tabular-nums; }
html[data-rl-mode=pro] #rlProPath {
  display:block; padding:16px clamp(14px,2.3cqi,27px); margin-block-start:17px;
  background:var(--rl-panel); border:1px solid var(--rl-line); border-radius:5px;
}
.rlProPath h2 { margin-block:0 10px; font-size:19px; }
.rlPathNodes { display:flex; flex-wrap:wrap; gap:8px 15px; align-items:stretch; }
.rlPathNode {
  flex:1 1 140px; min-width:0; padding:7px 0 7px 12px; border-inline-start:2px solid var(--rl-line);
}
.rlPathNode small,.rlPathNode strong { display:block; overflow-wrap:anywhere; }
.rlPathNode small { color:var(--rl-muted); font-size:12px; }
.rlPathNode strong { margin-block-start:4px; color:var(--rl-title); font-variant-numeric:tabular-nums; }
html[data-rl-mode=pro] #rlViewModes { border-radius:6px; }
html[data-rl-mode=pro] #rlViewModes [data-rl-mode] { border-radius:4px; }
html[data-rl-mode=pro][dir=rtl] #app > .side {
  padding-inline-start:14px;
  background:var(--rl-grain) right top / 7px 100% no-repeat,var(--rl-wood) right top / 7px 100% no-repeat,var(--rl-side);
}
@media(max-width:1060px) {
  #rlViewModes [data-rl-mode] { padding-inline:8px; }
  html[data-rl-mode] #topbar { gap:6px; }
  html[data-rl-mode] #topbar>.sp { flex:0 1 4px; }
  html[data-rl-mode] #topbar .search { margin-inline-start:4px; }
}
@media(max-width:815px) {
  html[data-rl-mode] #topbar :is(.pcnt,.ptitle,.hsep) { display:none; }
  html[data-rl-mode] #topbar .search { flex:1 1 96px; }
  html[data-rl-mode] #app,html[data-rl-mode] #app.noq {
    grid-template-columns:clamp(118px,20vw,155px) minmax(0,1fr) 0;
  }
}
@media(max-width:660px) {
  html[data-rl-mode] #app,html[data-rl-mode] #app.noq {
    grid-template-columns:clamp(95px,22vw,125px) minmax(0,1fr) 0;
    grid-template-rows:83px minmax(0,1fr) 104px;
  }
  html[data-rl-mode] #topbar { flex-wrap:wrap; align-content:center; gap:3px 5px; padding:4px 8px; }
  html[data-rl-mode] #topbar>* { order:2; }
  html[data-rl-mode] #topbar :is(.logo,.ttl,#rlViewModes,.wctl) { order:0; }
  html[data-rl-mode] #rlViewModes { margin-inline-start:auto; }
  html[data-rl-mode] #topbar .sp { order:1; flex:0 0 100%; height:0; }
  html[data-rl-mode] #topbar .ttl small { display:none; }
  html[data-rl-mode] #topbar .wctl { margin:0; }
  html[data-rl-mode] #topbar .wbtn { width:29px; height:29px; }
  html[data-rl-mode] #topbar .ptools { max-width:25%; overflow-x:auto; }
  html[data-rl-mode] #topbar .search { margin:0; max-width:none; }
  html[data-rl-mode] #topbar .ibtn { width:26px; height:28px; }
  html[data-rl-mode] #app>.side { padding-inline-end:3px; }
  html[data-rl-mode] #app>.side .nav { gap:5px; padding-inline:4px; font-size:12px; }
  html[data-rl-mode] #app>.bar {
    grid-template-columns:minmax(0,1fr) auto; grid-template-rows:minmax(0,1fr) minmax(0,1.25fr);
    gap:1px 8px; padding:3px 9px 5px;
  }
  html[data-rl-mode] #app>.bar .np { grid-row:1; grid-column:1; min-width:0; max-width:100%; }
  html[data-rl-mode] #app>.bar .np .cv2 { width:33px; height:33px; }
  html[data-rl-mode] #app>.bar .vol { grid-row:1; grid-column:2; gap:4px; }
  html[data-rl-mode] #app>.bar .vol input { width:48px; }
  html[data-rl-mode] #app>.bar .ctr { grid-row:2; grid-column:1/-1; width:min(94%,320px); justify-self:center; }
  html[data-rl-mode] #app>.bar .keys .k { height:26px; width:30px; }
  html[data-rl-mode] #app>.bar .keys .big { height:30px; width:30px; }
  html[data-rl-mode] #setpage { padding-inline:12px; }
}
@media(prefers-reduced-motion:reduce) {
  #rlListenHome *,#rlViewModes button,.rlSettingTasks button { transition:none; animation:none; }
}
@media(max-width:440px) {
  html[data-rl-mode] #topbar .ttl { display:none; }
  html[data-rl-mode] #rlViewModes { min-width:0; max-width:calc(100% - 130px); }
  #rlViewModes [data-rl-mode] {
    min-width:0; padding-inline:6px; font-size:12px; white-space:normal; overflow-wrap:anywhere;
  }
}
@media(forced-colors:active) {
  #rlViewModes [aria-pressed=true] { outline:2px solid Highlight; outline-offset:-2px; }
  .rlListenCover:not(.has-art):not(:has(img)) { border:1px solid CanvasText; }
}
/* The native range supplies the entire hit area. The line and thumb follow
   the real transport position, including keyboard and dragging previews. */
html[data-rl-mode] #app>.bar .ctr { gap:2px; }
html[data-rl-mode] #app>.bar .prog { max-width:none; }
html[data-rl-mode] #app #seek,html[data-rl-mode] #npSeek {
  position:relative; box-sizing:border-box; height:28px; overflow:visible; border:0;
  border-radius:4px; background:transparent; transition:none; direction:ltr;
}
html[data-rl-mode] :is(#seek,#npSeek)::before,html[data-rl-mode] :is(#seek,#npSeek)>i {
  content:""; position:absolute; left:0; top:50%; height:7px;
  transform:translateY(-50%); border-radius:4px; pointer-events:none;
}
html[data-rl-mode] :is(#seek,#npSeek)::before { right:0; background:var(--rl-line); }
html[data-rl-mode=pro] :is(#seek,#npSeek)::before {
  background:repeating-linear-gradient(90deg,var(--rl-accent) 0 1px,transparent 1px 10%),var(--rl-line);
}
html[data-rl-mode] :is(#seek,#npSeek)>i { background:var(--rl-accent); bottom:auto; transition:none; }
html[data-rl-mode] :is(#seek,#npSeek):has(.transportSeek:enabled)>i::after {
  position:absolute; content:""; right:0; top:50%; width:12px; height:12px;
  transform:translate(50%,-50%); border-radius:50%; background:var(--rl-accent);
}
html[data-rl-mode] .transportSeek {
  position:absolute; inset:0; width:100%; height:100%; padding:0; margin:0;
  opacity:0; cursor:pointer; touch-action:pan-y;
}
html[data-rl-mode] :is(#seek,#npSeek):has(.transportSeek:focus-visible) {
  outline:2px solid var(--rl-accent); outline-offset:1px;
}
html[data-rl-mode] .transportVolume {
  display:inline-flex; align-items:center; gap:5px; white-space:nowrap;
  font-variant-numeric:tabular-nums;
}
html[data-rl-mode] #volFloor {
  display:inline-grid; place-items:center; min-width:26px; height:30px; padding:0;
  background:transparent; color:var(--rl-muted); border:0; border-radius:4px; cursor:pointer;
}
html[data-rl-mode] #volFloor:hover,html[data-rl-mode] #volFloor[aria-pressed=true] { color:var(--rl-accent); }
html[data-rl-mode] #volFloor:disabled { opacity:.55; cursor:default; }
html[data-rl-mode] #volFloor .volQuiet,html[data-rl-mode] #volFloor[aria-pressed=true] .volSound { display:none; }
html[data-rl-mode] #volFloor[aria-pressed=true] .volQuiet { display:block; }
html[data-rl-mode] #barVolDb:not([hidden]) { display:inline-block; min-width:7.2ch; color:var(--rl-title); font-size:12px; text-align:end; }
.transportKeys { color:var(--mut2); font-size:12px; margin:.85em 0 0; }
@media(min-width:1181px) {
  html[data-rl-mode] #app>.bar .ctr { width:clamp(470px,42vw,700px); }
}
@media(max-width:660px) {
  html[data-rl-mode] #app>.bar .ctr { width:100%; }
  html[data-rl-mode] .transportVolume { gap:2px; }
  html[data-rl-mode] #volFloor { min-width:22px; }
  html[data-rl-mode] #barVolDb:not([hidden]) { font-size:11px; }
}
@media(forced-colors:active) {
  html[data-rl-mode] :is(#seek,#npSeek)::before { background:GrayText; }
  html[data-rl-mode] :is(#seek,#npSeek)>i { background:Highlight; }
}
)RLMODES";
}

