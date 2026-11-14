#pragma once

namespace rta::net {
inline const char* rta_ui_listening_settings_css() {
    return R"RLDAILY(
html[data-rl-mode=listen] #setpage #rlListenSettings {display:block;max-width:900px;margin:0 auto;}
#rlListenSettings [hidden] {display:none !important;}
#rlListenSettings #rlDailyMenu>button {margin-top:18px;}
html[data-rl-mode=listen] #setpage #rlListenSettings #rlSettingsBack:not([hidden]) {display:inline-flex;margin:0 0 18px;}
#rlListenSettings .rlDailyDetail {max-width:620px;padding:clamp(16px,3.2vw,32px);border:1px solid var(--line);border-radius:14px;background:var(--card,var(--phbg));}
.rlDailyDetail h2 {font-size:clamp(21px,3vw,31px);margin:0 0 18px;}
.rlDailyDetail h3 {font-size:15px;margin:22px 0 8px;}
.rlDailyDetail p,.rlDailyDetail li {line-height:1.6;overflow-wrap:anywhere;}
.rlDailyField {display:grid;gap:6px;margin:14px 0;}
.rlDailyField label {font-weight:600;}
.rlDailyField :is(input,select,textarea) {width:100%;max-width:100%;min-width:0;box-sizing:border-box;background:var(--phbg);color:var(--fg);border:1px solid var(--line);padding:9px 11px;border-radius:7px;font:inherit;}
.rlDailyField textarea {resize:vertical;min-height:5em;}
.rlDailyActions {display:flex;align-items:center;gap:8px;flex-wrap:wrap;margin:14px 0;}
.rlDailyActions button[aria-pressed=true]{color:var(--rl-title,var(--fg));outline:2px solid var(--rl-accent,var(--acc));outline-offset:1px;}
.rlDailyStatus:empty,.rlDailyResult:empty {display:none;}
.rlDailyStatus {font-size:13px;color:var(--rl-muted,var(--mut));}
.rlDailyResult {white-space:pre-line;}
#rlFolderList {margin:8px 0;padding-inline-start:20px;}
#rlPresetPreview {padding:13px 16px;border-inline-start:2px solid var(--rl-accent,var(--acc));background:var(--phbg);}
#rlPresetPreview p {margin:0 0 10px;}
@media(max-width:740px){#rlListenSettings .rlDailyDetail {padding:14px;}.rlDailyActions .btn {white-space:normal;}}
)RLDAILY";
}
}

