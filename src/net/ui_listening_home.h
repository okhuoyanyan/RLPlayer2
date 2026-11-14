#pragma once

namespace rta::net {

inline const char* rta_ui_listening_home_css() {
    return R"LISTENHOME(
#rlListeningControls {margin:0 0 clamp(18px,2.4cqi,30px);color:var(--rl-title)}
html[data-rl-mode=pro] #rlListeningControls {display:none}
.rlListeningChoices {display:grid;grid-template-columns:repeat(4,minmax(0,1fr));gap:9px}
.rlListeningChoice {min-width:0;position:relative;display:flex;flex-direction:column;align-items:flex-start;justify-content:center;gap:9px;padding:15px 17px;background:var(--rl-panel);color:var(--rl-title);border:1px solid var(--rl-line);border-radius:9px;text-align:start;font:inherit}
button.rlListeningChoice {cursor:pointer;padding-inline-end:34px}
button.rlListeningChoice::after {content:'›';position:absolute;inset-inline-end:14px;top:50%;transform:translateY(-50%);font-size:23px;font-weight:400;color:var(--rl-muted)}
button.rlListeningChoice:hover {background:var(--rl-tint);border-color:var(--rl-muted)}
.rlListeningChoice>span,.rlListeningDeviceTitle>label {font-size:12px;color:var(--rl-muted)}
.rlListeningChoice>strong {display:block;max-width:100%;font-size:15px;font-weight:600;line-height:1.3;overflow:hidden;white-space:nowrap;text-overflow:ellipsis}
.rlListeningDeviceTitle {display:flex;align-items:center;justify-content:space-between;gap:10px;width:100%}
.rlListeningRefresh {border:0;background:transparent;color:var(--rl-muted);font:inherit;font-size:16px;line-height:1;cursor:pointer;padding:2px 3px;margin:-3px -3px -3px 0}
#rlListeningDevice {display:block;border:0;border-radius:0;background:transparent;color:var(--rl-title);font-family:inherit;font-size:15px;font-weight:600;line-height:1.3;width:100%;max-width:100%;min-width:0;margin:0;padding:0 18px 0 0;cursor:pointer;text-overflow:ellipsis}
#rlListeningDevice option,#rlListeningDevice optgroup {background:var(--rl-panel);color:var(--rl-title)}
.rlListeningChoice:focus-visible,.rlListeningRefresh:focus-visible,#rlListeningDevice:focus-visible,#rlListeningDialog :is(button,select,input):focus-visible {outline:2px solid var(--rl-accent);outline-offset:3px}
#rlListeningControls [disabled] {cursor:wait;opacity:.55}
.rlListeningNotice {min-height:0;margin:0;font-size:12px;line-height:1.6;color:var(--rl-muted)}
.rlListeningNotice:not(:empty) {margin:8px 2px 0}
.rlListeningNotice[data-error=true],#rlListeningDialogMessage[data-error=true] {color:var(--rl-error)}
#rlListeningDialog {box-sizing:border-box;width:min(520px,calc(100vw - 36px));max-height:calc(100dvh - 36px);margin:auto;padding:0;overflow:auto;color:var(--rl-title);background:var(--rl-panel);border:1px solid var(--rl-line);border-radius:15px;box-shadow:0 18px 80px #0007;text-align:start;font-family:inherit;font-size:14px;line-height:1.55}
#rlListeningDialog::backdrop {background:#071112a6;backdrop-filter:blur(7px)}
#rlListeningDialog [hidden] {display:none!important}
#rlListeningDialog[open] {animation:rlListeningAppear 150ms ease-out}
.rlListeningDialogHeader {display:flex;align-items:center;justify-content:space-between;gap:18px;padding:22px 24px 15px;border-bottom:1px solid var(--rl-line)}
.rlListeningDialogHeader h2 {font-size:21px;font-weight:600;margin:0;line-height:1.3}
.rlListeningClose {border:0;background:transparent;color:var(--rl-muted);font-family:inherit;font-size:23px;line-height:1;padding:2px 5px;cursor:pointer}
#rlListeningDialogBody {padding:20px 24px 8px}
.rlListeningField {display:flex;flex-direction:column;gap:9px;margin:0 0 18px;color:var(--rl-copy);font-size:13px}
.rlListeningField select {box-sizing:border-box;width:100%;min-height:40px;border:1px solid var(--rl-line);border-radius:6px;padding:8px 10px;font-family:inherit;font-size:14px;background:var(--rl-field);color:var(--rl-title)}
.rlListeningToggle {display:flex;align-items:center;justify-content:space-between;gap:15px;padding:3px 0 7px;color:var(--rl-title);font-size:14px}
.rlListeningToggle input {accent-color:var(--rl-accent);width:19px;height:19px;cursor:pointer}
.rlListeningHint {color:var(--rl-muted);font-size:12px;line-height:1.7;margin:7px 0 13px}
.rlListeningOptions {display:grid;grid-template-columns:1fr;gap:8px;margin:0}
.rlListeningOption {display:grid;grid-template-columns:1fr auto;gap:5px 14px;min-height:66px;text-align:start;padding:13px 15px;border:1px solid var(--rl-line);border-radius:8px;background:var(--rl-field);color:var(--rl-title);font:inherit;cursor:pointer}
.rlListeningOption strong {font-size:14px;font-weight:600}
.rlListeningOption small {grid-column:1;color:var(--rl-muted);font-size:12px;line-height:1.5}
.rlListeningOption::after {content:'';grid-column:2;grid-row:1/3;align-self:center;width:15px;height:15px;border:1px solid var(--rl-muted);border-radius:50%}
.rlListeningOption[aria-pressed=true] {border-color:var(--rl-accent);background:var(--rl-tint)}
.rlListeningOption[aria-pressed=true]::after {background:var(--rl-accent);border-color:var(--rl-accent);box-shadow:inset 0 0 0 4px var(--rl-panel)}
.rlListeningOption:disabled {opacity:.5;cursor:default}
.rlListeningStyle {grid-template-columns:repeat(3,minmax(0,1fr));gap:8px}
.rlListeningStyle .rlListeningOption {display:flex;flex-direction:column;align-items:center;text-align:center;gap:7px;padding:18px 8px;min-height:82px}
.rlListeningStyle .rlListeningOption::after {display:none}
.rlListeningStyle .rlListeningOption small {font-size:11px}
.rlListeningDialogFooter {padding:0 24px 22px}
#rlListeningDialogMessage {color:var(--rl-muted);font-size:12px;line-height:1.6;margin:5px 0 13px;min-height:19px}
.rlListeningDialogButtons {display:flex;justify-content:flex-end;gap:8px;flex-wrap:wrap}
.rlListeningDialogButtons .btn {min-height:37px;padding:8px 17px}
.rlListeningLibraryHint {margin:12px 1px 0;color:var(--rl-muted);font-size:13px;line-height:1.7}
#rlListeningFoldersButton {margin-inline-start:auto;white-space:nowrap;min-height:35px}
#rlListeningDialog input[type=text] {box-sizing:border-box;width:100%;min-height:42px;border:1px solid var(--rl-line);border-radius:6px;padding:10px 11px;background:var(--rl-field);color:var(--rl-title);font:inherit}
.rlListeningFolderInput {display:flex;gap:8px;align-items:center}
.rlListeningFolderInput input {min-width:0;flex:1}
.rlListeningFolderInput button {flex:none;min-height:42px;white-space:nowrap}
.rlListeningFolderList {list-style:none;margin:8px 0 18px;padding:0}
.rlListeningFolderList li {display:flex;align-items:center;gap:12px;padding:12px 0;border-bottom:1px solid var(--rl-line)}
.rlListeningFolderList li>span {flex:1;min-width:0;overflow-wrap:anywhere;line-height:1.6}
.rlListeningFolderList button {flex:none;white-space:nowrap}
.rlListeningFolderActions {display:flex;gap:8px;flex-wrap:wrap;margin:10px 0}
.rlListeningNetworkScan {margin-block-start:-4px}
@keyframes rlListeningAppear {from{opacity:0;transform:translateY(5px)}to{opacity:1;transform:translateY(0)}}
@media(prefers-reduced-motion:reduce) {#rlListeningDialog[open]{animation:none}#rlListeningDialog::backdrop{backdrop-filter:none}}
@container rlhome (max-width:750px) {.rlListeningChoices{grid-template-columns:repeat(2,minmax(0,1fr))}.rlListeningChoice{padding:13px 14px}}
@container rlhome (max-width:350px) {.rlListeningChoices{grid-template-columns:1fr}.rlListeningChoice{gap:6px}}
@media(max-width:420px) {.rlListeningDialogHeader{padding:18px 18px 13px}#rlListeningDialogBody{padding:17px 18px 6px}.rlListeningDialogFooter{padding:0 18px 18px}.rlListeningStyle{gap:5px}.rlListeningStyle .rlListeningOption{padding:15px 5px}}
@media(forced-colors:active) {.rlListeningOption[aria-pressed=true]{outline:2px solid Highlight}.rlListeningOption[aria-pressed=true]::after{background:Highlight}}
)LISTENHOME";
}

inline const char* rta_ui_listening_home_js() {
    return R"LISTENHOME(
var rlListening = {
  devices:[], remoteDevices:[], devicesLoaded:false, devicesLoading:false,
  deviceGeneration:0, remoteGeneration:0, capsGeneration:0, capsKey:'', caps:null,
  capsLoading:false, subscribed:false, localBackend:'wasapi', localDevice:'',
  task:'', draft:null, opener:null, working:false, dialogGeneration:0
};
function rlListeningSettings() {
  return typeof rlConsole!=='undefined' && rlConsole.state && rlConsole.state.settings || {};
}
function rlListeningBusy() {
  return rlListening.working || typeof rlConsole==='undefined' || rlConsole.busy;
}
function rlListeningSay(id,message,error) {
  var node=D(id);if(!node)return;
  node.textContent=message||'';node.dataset.error=error?'true':'false';
}
function rlListeningProfile(kv) {
  var state=typeof rlConsole!=='undefined'&&rlConsole.state||{};
  var declared=state.profile||state.mode||kv.ui_profile;
  if(['bitperfect','pcm','dsd','custom'].indexOf(declared)>=0)return declared;
  var dsd=String(kv.dsd||'0'), direct=String(kv.directsdm||'0')==='1';
  if(direct || (!parseInt(dsd,10) && (kv.rate||'src')==='src' && !kv.filter)) return 'bitperfect';
  if(parseInt(dsd.replace(/^(nat|dop)/,''),10)>0) return 'dsd';
  return (kv.rate||'src')!=='src' ? 'pcm' : 'custom';
}
function rlListeningStyle(kv) {
  var state=typeof rlConsole!=='undefined'&&rlConsole.state||{};
  var declared=state.style||kv.ui_style;
  if(['warm','balanced','sharp'].indexOf(declared)>=0)return declared;
  if(declared==='custom')return '';
  var filter=String(kv.sdmfilt||kv.filter||'');
  if(/gauss/i.test(filter))return 'warm';
  if(/(?:long|ext)/i.test(filter))return 'sharp';
  if(filter==='auto'||/short|medium/i.test(filter))return 'balanced';
  return '';
}
function rlListeningProfileName(profile) {
  return t(({bitperfect:'原音直通',pcm:'PCM 升频',dsd:'DSD 升频',custom:'自定义播放方式'})[profile]||'自定义播放方式');
}
function rlListeningStyleName(style) {
  return t(({warm:'温和',balanced:'均衡',sharp:'锐利'})[style]||'自定风格');
}
function rlInstallListeningControls() {
  var home=D('rlListenHome');if(!home)return;
  var heading=home.querySelector('.rlListenHeading');
  if(heading&&!D('rlListeningFoldersButton')){
    var folders=document.createElement('button');folders.type='button';folders.id='rlListeningFoldersButton';folders.className='btn';
    folders.textContent=t('音乐文件夹');folders.setAttribute('aria-haspopup','dialog');
    folders.onclick=function(){rlListeningOpenLibrary(folders);};heading.appendChild(folders);
  }
  if(!D('rlListeningControls')) {
    var rail=document.createElement('section');rail.id='rlListeningControls';rail.setAttribute('aria-label',t('聆听设置'));
    rail.innerHTML='<div class=rlListeningChoices>'
      +'<div class=rlListeningChoice><div class=rlListeningDeviceTitle><label for=rlListeningDevice>当前设备</label>'
      +'<button type=button class=rlListeningRefresh id=rlListeningDeviceRefresh title=刷新设备 aria-label=刷新设备>↻</button></div>'
      +'<select id=rlListeningDevice aria-label=当前设备 disabled><option>正在读取设备</option></select></div>'
      +'<button type=button class=rlListeningChoice data-rl-listen-task=output aria-haspopup=dialog><span>输出</span><strong id=rlListeningOutputName></strong></button>'
      +'<button type=button class=rlListeningChoice data-rl-listen-task=profile aria-haspopup=dialog><span>播放方式</span><strong id=rlListeningProfileName></strong></button>'
      +'<button type=button class=rlListeningChoice data-rl-listen-task=style aria-haspopup=dialog><span>听音风格</span><strong id=rlListeningStyleName></strong></button>'
      +'</div><p class=rlListeningNotice id=rlListeningNotice role=status aria-live=polite></p>'
      +'<p class=rlListeningLibraryHint id=rlListeningLibraryHint hidden>添加你的音乐文件夹，歌曲会出现在这里。</p>';
    var grid=home.querySelector('.rlListenGrid');home.insertBefore(rail,grid||home.firstChild);
    rail.addEventListener('click',function(e){var card=e.target.closest('[data-rl-listen-task]');if(card)rlOpenListeningCard(card.dataset.rlListenTask,card);});
    D('rlListeningDevice').onchange=rlListeningChooseDevice;
    D('rlListeningDeviceRefresh').onclick=function(){rlListeningLoadDevices(true);};
    applyI18n(rail);
  }
  if(!D('rlListeningDialog'))rlListeningInstallDialog();
  if(!rlListening.subscribed && typeof rlConsole!=='undefined') {
    rlListening.subscribed=true;rlConsole.subscribe(rlPaintListeningControls);
  }
  if(!rlListening.devicesLoaded&&!rlListening.devicesLoading)rlListeningLoadDevices(false);
  rlPaintListeningControls();
  if(!rlListening.foldersLoaded&&!rlListening.foldersLoading)rlListeningFetchFolders();
}
function rlListeningInstallDialog() {
  var dialog=document.createElement('dialog');dialog.id='rlListeningDialog';
  dialog.setAttribute('aria-labelledby','rlListeningDialogTitle');dialog.setAttribute('aria-describedby','rlListeningDialogMessage');
  dialog.innerHTML='<header class=rlListeningDialogHeader><h2 id=rlListeningDialogTitle></h2>'
    +'<button type=button class=rlListeningClose id=rlListeningDialogClose aria-label=关闭 title=关闭>×</button></header>'
    +'<div id=rlListeningDialogBody></div><footer class=rlListeningDialogFooter><p id=rlListeningDialogMessage role=status aria-live=polite></p>'
    +'<div class=rlListeningDialogButtons><button type=button class=btn id=rlListeningDialogCancel>取消</button>'
    +'<button type=button class="btn pri" id=rlListeningDialogApply>应用</button></div></footer>';
  document.body.appendChild(dialog);
  D('rlListeningDialogClose').onclick=D('rlListeningDialogCancel').onclick=function(){dialog.close();};
  D('rlListeningDialogApply').onclick=rlListeningApplyCard;
  dialog.addEventListener('click',function(event){
    if(event.target!==dialog)return;var r=dialog.getBoundingClientRect();
    if(event.clientX<r.left||event.clientX>r.right||event.clientY<r.top||event.clientY>r.bottom)dialog.close();
  });
  dialog.addEventListener('close',function(){
    if(dialog.open)return; // A queued close event belongs to the previous card.
    rlListening.task='';rlListening.draft=null;
    var opener=rlListening.opener;rlListening.opener=null;if(opener&&opener.isConnected)opener.focus();
  });
  applyI18n(dialog);
}
function rlListeningDeviceRows(kv) {
  if(kv.backend==='upnp') return rlListening.remoteDevices.map(function(d){return {backend:'upnp',device:d.id,name:d.name};});
  var rows=rlListening.devices.slice();
  if(kv.backend==='null')rows.unshift({backend:'null',device:'null',name:t('离线处理')});
  return rows;
}
function rlListeningSelected(kv,rows) {
  var backend=kv.backend||'wasapi',id=backend==='upnp'?(kv.upnpdev||''):(kv.device||'');
  return rows.findIndex(function(d){return d.device===id&&(d.backend===backend || d.backend==='wasapi'&&backend==='wasapi-shared');});
}
function rlListeningFillDevices(select,kv) {
  if(!select)return;
  var rows=rlListeningDeviceRows(kv),selected=rlListeningSelected(kv,rows);
  var key=JSON.stringify([rows,selected,kv.backend,kv.device,kv.upnpdev,I18N.lang]);
  if(select.rlKey===key)return;select.rlKey=key;select.rlRows=rows;select.replaceChildren();
  rows.forEach(function(d,i){select.add(new Option(d.name,String(i)));});
  if(selected<0) {
    var label=kv.backend==='upnp'?t('选择网络播放器'):kv.device?t('上次选择的设备'):t('默认设备');
    select.add(new Option(label,'current'));selected=select.options.length-1;
  }
  select.selectedIndex=selected;
}
function rlPaintListeningControls() {
  var rail=D('rlListeningControls');if(!rail)return;
  if(typeof rlMode!=='undefined'&&rlMode==='pro'&&D('rlListeningDialog')&&D('rlListeningDialog').open)D('rlListeningDialog').close();
  var kv=rlListeningSettings(),ready=typeof PLAYBACK_SETTINGS!=='undefined'&&PLAYBACK_SETTINGS.ready&&Object.keys(kv).length>0,profile=rlListeningProfile(kv),backend=kv.backend||'wasapi';
  var disabled=rlListeningBusy()||!ready;
  rail.querySelectorAll('button').forEach(function(b){b.disabled=disabled;});
  rlListeningFillDevices(D('rlListeningDevice'),kv);
  D('rlListeningDevice').disabled=disabled||rlListening.devicesLoading||backend==='upnp'&&!rlListening.remoteDevices.length;
  D('rlListeningDeviceRefresh').disabled=disabled||rlListening.devicesLoading;
  rlSetText('rlListeningOutputName',t(backend==='null'?'离线处理':backend==='upnp'?'网络播放器':backend==='wasapi-shared'?'本机 · 共享播放':'本机 · 独占播放'));
  rlSetText('rlListeningProfileName',ready?rlListeningProfileName(profile):t('正在读取'));
  rlSetText('rlListeningStyleName',profile==='bitperfect'?t('保持原音'):rlListeningStyleName(rlListeningStyle(kv)));
  var style=rail.querySelector('[data-rl-listen-task=style]');
  style.disabled=disabled||profile==='bitperfect';style.title=profile==='bitperfect'?t('原音直通时保持源样本，听音风格不参与。'):'';
  if(backend!=='upnp'&&backend!=='null'){rlListening.localBackend=backend;rlListening.localDevice=kv.device||'';}
  var key=backend+'|'+(kv.device||'');
  if(ready&&key!==rlListening.capsKey)rlListeningLoadCaps(key);
  if(D('rlListeningDialog')&&D('rlListeningDialog').open)rlListeningUpdateDialog();
}
function rlListeningLoadDevices(scan) {
  if(rlListening.devicesLoading)return;
  var generation=++rlListening.deviceGeneration;rlListening.devicesLoading=true;rlPaintListeningControls();
  return apiGetRaw('/api/devices').then(function(j){
    if(generation!==rlListening.deviceGeneration)return;
    if(!j||!j.ok)throw Error(j&&j.error||t('设备读取失败，请重试。'));
    var rows=[{backend:'wasapi',device:'',name:t('默认设备')}];
    (j.devices||[]).forEach(function(d){if(d&&d.id)rows.push({backend:'wasapi',device:d.id,name:d.name||t('播放设备')});});
    (j.asioDrivers||[]).forEach(function(d){if(d&&d.clsid)rows.push({backend:'asio',device:d.clsid,name:d.name||t('设备专属连接')});});
    rlListening.devices=rows;rlListening.devicesLoaded=true;
    if(scan)rlListeningSay('rlListeningNotice',t('设备列表已刷新。'));
    if(rlListeningSettings().backend==='upnp')return rlListeningLoadRemote(!!scan);
  }).catch(function(error){rlListeningSay('rlListeningNotice',error.message||t('设备读取失败，请重试。'),true);})
    .finally(function(){if(generation===rlListening.deviceGeneration){rlListening.devicesLoading=false;rlPaintListeningControls();}});
}
function rlListeningLoadRemote(scan) {
  var generation=++rlListening.remoteGeneration;
  return apiGetRaw('/api/cast/devices'+(scan?'?scan=1':'')).then(function(j){
    if(generation!==rlListening.remoteGeneration)return;
    if(!j||!j.ok)throw Error(j&&j.error||t('网络播放器读取失败。'));
    rlListening.remoteDevices=(j.devices||[]).map(function(d){
      return {id:d.location||d.friendlyName||d.name||d.host||'',name:(d.friendlyName||d.name||d.host||t('网络播放器'))+(d.model?' · '+d.model:'')};
    }).filter(function(d){return d.id;});
    if(rlListening.task==='output'&&rlListening.draft&&rlListening.draft.route==='network')rlListeningFillOutputDevice();
    rlPaintListeningControls();
  });
}
function rlListeningLoadCaps(key) {
  rlListening.capsKey=key;rlListening.caps=null;rlListening.capsLoading=true;
  var generation=++rlListening.capsGeneration;
  apiGetRaw('/api/preset/caps').then(function(j){if(generation===rlListening.capsGeneration&&j&&j.ok)rlListening.caps=j;})
    .catch(function(){})
    .finally(function(){if(generation===rlListening.capsGeneration){rlListening.capsLoading=false;rlListeningUpdateDialog();}});
}
function rlListeningChooseDevice() {
  var select=D('rlListeningDevice'),device=select.rlRows&&select.rlRows[Number(select.value)],kv=rlListeningSettings();
  if(!device||rlListeningBusy()){rlListeningFillDevices(select,kv);return;}
  var patch=device.backend==='upnp'?{backend:'upnp',upnpdev:device.device}:
    {backend:device.backend==='wasapi'&&kv.backend==='wasapi-shared'?'wasapi-shared':device.backend,device:device.device};
  rlListeningSubmit(patch,t('输出设备已更新。'));
}
function rlListeningOpenBody(task) {
  var kv=rlListeningSettings();
  if(task==='output') {
    var offline=kv.backend==='null',remote=kv.backend==='upnp';
    rlListening.draft={route:offline?'offline':remote?'network':'local',exclusive:kv.backend!=='wasapi-shared',backend:kv.backend,device:kv.device||'',upnpdev:kv.upnpdev||''};
    return '<label class=rlListeningField><span>播放到</span><select id=rlListeningRoute><option value=local>本机播放</option><option value=network>网络播放器</option>'
      +(offline?'<option value=offline>离线处理</option>':'')+'</select></label>'
      +'<label class=rlListeningField id=rlListeningOutputDeviceField><span>当前设备</span><select id=rlListeningOutputDevice></select></label>'
      +'<button type=button class="btn rlListeningNetworkScan" id=rlListeningNetworkScan hidden>发现网络播放器</button>'
      +'<label class=rlListeningToggle><span>独占设备</span><input id=rlListeningExclusive type=checkbox></label>'
      +'<p class=rlListeningHint id=rlListeningOutputHint></p>';
  }
  if(task==='profile') {
    rlListening.draft={profile:rlListeningProfile(kv),style:rlListeningStyle(kv)||'balanced'};
    return '<div class=rlListeningOptions>'
      +[['bitperfect','原音直通','保持源采样率与原始声音。'],['pcm','PCM 升频','按当前设备的能力选择输出规格。'],['dsd','DSD 升频','将 PCM 或 DSD 音乐升频为 DSD 输出。']].map(function(row){
        return '<button type=button class=rlListeningOption data-rl-listening-profile='+row[0]+' aria-pressed=false><strong>'+esc(t(row[1]))+'</strong><small>'+esc(t(row[2]))+'</small></button>';
      }).join('')+'</div><p class=rlListeningHint id=rlListeningCapabilityHint></p><button type=button class=btn id=rlListeningCapsRetry>重新读取设备能力</button>';
  }
  rlListening.draft={style:rlListeningStyle(kv)||'balanced'};
  return '<div class="rlListeningOptions rlListeningStyle">'
    +[['warm','温和','柔和耐听，适合放松'],['balanced','均衡','自然舒展，适合日常聆听'],['sharp','锐利','清晰利落，细节更鲜明']].map(function(row){
      return '<button type=button class=rlListeningOption data-rl-listening-style='+row[0]+' aria-pressed=false><strong>'+esc(t(row[1]))+'</strong><small>'+esc(t(row[2]))+'</small></button>';
    }).join('')+'</div><p class=rlListeningHint>选你喜欢的声音即可，不同歌曲和设备的听感会有差异。</p>';
}
function rlOpenListeningCard(task,opener) {
  if(task==='library'){rlListeningOpenLibrary(opener);return;}
  if(rlListeningBusy()||['output','profile','style'].indexOf(task)<0)return;
  if(task==='style'&&rlListeningProfile(rlListeningSettings())==='bitperfect')return;
  var dialog=D('rlListeningDialog');if(!dialog)rlListeningInstallDialog();dialog=D('rlListeningDialog');
  if(dialog.open)dialog.close();
  ++rlListening.dialogGeneration;
  rlListening.task=task;rlListening.opener=opener||document.activeElement;
  D('rlListeningDialogApply').hidden=false;rlSetText('rlListeningDialogCancel',t('取消'));
  rlSetText('rlListeningDialogTitle',t(({output:'输出',profile:'选择播放方式',style:'听音风格'})[task]));
  D('rlListeningDialogBody').innerHTML=rlListeningOpenBody(task);
  rlListeningSay('rlListeningDialogMessage','');
  if(task==='output')rlListeningBindOutput();
  else D('rlListeningDialogBody').onclick=function(event){
    if(event.target.closest('#rlListeningCapsRetry')){var kv=rlListeningSettings();rlListeningLoadCaps((kv.backend||'wasapi')+'|'+(kv.device||''));rlListeningUpdateDialog();return;}
    var profile=event.target.closest('[data-rl-listening-profile]'),style=event.target.closest('[data-rl-listening-style]');
    if(profile&&!profile.disabled)rlListening.draft.profile=profile.dataset.rlListeningProfile;
    if(style&&!style.disabled)rlListening.draft.style=style.dataset.rlListeningStyle;
    rlListeningUpdateDialog();
  };
  applyI18n(dialog);dialog.showModal();rlListeningUpdateDialog();
  var first=dialog.querySelector('select,button[aria-pressed=true],button.rlListeningOption:not(:disabled)');
  if(first)first.focus();
}
function rlListeningBindOutput() {
  var draft=rlListening.draft;
  D('rlListeningRoute').value=draft.route;
  D('rlListeningExclusive').checked=draft.exclusive;
  D('rlListeningRoute').onchange=function(){draft.route=this.value;rlListeningFillOutputDevice();if(this.value==='network')rlListeningDiscoverRemote(false);};
  D('rlListeningExclusive').onchange=function(){draft.exclusive=this.checked;rlListeningUpdateDialog();};
  D('rlListeningOutputDevice').onchange=function(){
    var device=this.rlRows&&this.rlRows[Number(this.value)];if(!device)return;
    if(draft.route==='network')draft.upnpdev=device.device;
    else {draft.backend=device.backend;draft.device=device.device;if(device.backend==='asio')draft.exclusive=true;}
    rlListeningUpdateDialog();
  };
  D('rlListeningNetworkScan').onclick=function(){rlListeningDiscoverRemote(true);};
  rlListeningFillOutputDevice();
  if(draft.route==='network')rlListeningDiscoverRemote(false);
}
function rlListeningFillOutputDevice() {
  var draft=rlListening.draft;if(!draft||rlListening.task!=='output')return;
  var select=D('rlListeningOutputDevice'),remote=draft.route==='network';
  var chosen=remote?{backend:'upnp',upnpdev:draft.upnpdev}:{backend:draft.backend==='asio'?'asio':'wasapi',device:draft.route==='local'&&draft.device==='null'?rlListening.localDevice:draft.device};
  if(draft.route==='local'&&['wasapi','wasapi-shared','asio'].indexOf(draft.backend)<0){draft.backend=rlListening.localBackend;draft.device=rlListening.localDevice;chosen={backend:draft.backend,device:draft.device};}
  rlListeningFillDevices(select,chosen);rlListeningUpdateDialog();
}
function rlListeningDiscoverRemote(scan) {
  var generation=rlListening.dialogGeneration;
  var button=D('rlListeningNetworkScan');if(button)button.disabled=true;
  rlListeningSay('rlListeningDialogMessage',t('正在发现网络播放器…'));
  rlListeningLoadRemote(scan).then(function(){
    if(rlListeningSameDialog(generation,'output'))rlListeningSay('rlListeningDialogMessage',rlListening.remoteDevices.length?'':t('暂未发现网络播放器，请确认它已开启并连接同一网络。'));
  }).catch(function(error){if(rlListeningSameDialog(generation,'output'))rlListeningSay('rlListeningDialogMessage',error.message||t('发现失败，请重试。'),true);})
    .finally(function(){if(button&&button.isConnected)button.disabled=false;});
}
function rlListeningDsdBase() {
  var file=typeof rlConsole!=='undefined'&&rlConsole.state&&rlConsole.state.file;
  if(!file)return 44100;
  if(S.metaFile!==file||!S.curMeta)return 0;
  var rate=Number(S.curMeta.sr||S.curMeta.rate||S.curMeta.sampleRate);
  if(!Number.isFinite(rate)||rate<=0)return 0;
  return rate%48000===0?48000:44100;
}
function rlListeningProfileAvailable(profile) {
  if(['bitperfect','pcm','dsd'].indexOf(profile)<0)return false;
  var kv=rlListeningSettings(),backend=kv.backend||'wasapi',caps=rlListening.caps&&rlListening.caps.device;
  if(profile==='bitperfect')return true;
  if(backend==='null')return true;
  if(backend==='wasapi-shared')return false;
  if(backend==='upnp'||!caps||!caps.maxRate||!Array.isArray(caps.rates)||!caps.rates.length)return false;
  if(profile==='pcm')return true;
  var base=rlListeningDsdBase();
  return base>0&&[64,128,256,512,1024].some(function(level){return caps.rates.indexOf(base*level/16)>=0;});
}
function rlListeningUpdateDialog() {
  var dialog=D('rlListeningDialog'),draft=rlListening.draft;if(!dialog||!dialog.open||!draft)return;
  var busy=rlListeningBusy(),task=rlListening.task,apply=D('rlListeningDialogApply');
  if(task==='library'){rlListeningFolderBusyPaint();return;}
  dialog.querySelectorAll('#rlListeningDialogBody input,#rlListeningDialogBody select').forEach(function(e){e.disabled=busy;});
  if(task==='profile') {
    dialog.querySelectorAll('[data-rl-listening-profile]').forEach(function(button){
      var key=button.dataset.rlListeningProfile;button.disabled=busy||!rlListeningProfileAvailable(key);button.setAttribute('aria-pressed',String(draft.profile===key));
    });
    var dsd=rlListeningProfileAvailable('dsd'),pcm=rlListeningProfileAvailable('pcm');
    var shared=rlListeningSettings().backend==='wasapi-shared';
    rlSetText('rlListeningCapabilityHint',t(rlListening.capsLoading?'正在确认当前设备能力。':shared?'可在「输出」中开启独占设备，再选择升频。':!pcm||!rlListeningDsdBase()?'当前输出的升频能力尚未确认。':!dsd?'当前输出支持 PCM 升频，尚未确认 DSD 输出能力。':'按设备能力匹配规格，保留当前音量。'));
    if(D('rlListeningCapsRetry'))D('rlListeningCapsRetry').disabled=busy||rlListening.capsLoading;
    apply.disabled=busy||!rlListeningProfileAvailable(draft.profile);
  } else if(task==='style') {
    var straight=rlListeningProfile(rlListeningSettings())==='bitperfect';
    dialog.querySelectorAll('[data-rl-listening-style]').forEach(function(button){button.disabled=busy||straight;button.setAttribute('aria-pressed',String(draft.style===button.dataset.rlListeningStyle));});
    apply.disabled=busy||straight;
    if(straight)rlListeningSay('rlListeningDialogMessage',t('当前保持原声，听音风格暂不可用。'));
  } else {
    var remote=draft.route==='network',offline=draft.route==='offline',native=draft.backend==='asio'&&!remote&&!offline;
    D('rlListeningOutputDeviceField').hidden=offline;
    D('rlListeningNetworkScan').hidden=!remote;
    var exclusive=D('rlListeningExclusive');exclusive.disabled=busy||remote||offline||native;exclusive.checked=native||!remote&&!offline&&draft.exclusive;
    rlSetText('rlListeningOutputHint',t(offline?'离线处理，不打开音频设备。':remote?'由网络播放器负责实际输出。':native?'设备直连':draft.exclusive?'独占播放会占用当前设备，其他软件的声音可能暂停。':'共享播放可与其他软件同时使用当前设备。'));
    var select=D('rlListeningOutputDevice');
    apply.disabled=busy||remote&&!draft.upnpdev||!offline&&select.value==='current';
  }
  rlSetText('rlListeningDialogApply',t(busy?'正在切换…':'应用'));
}
function rlListeningApplyCard() {
  var task=rlListening.task,draft=rlListening.draft;
  if(!draft||rlListeningBusy()||D('rlListeningDialogApply').disabled)return;
  var patch,message;
  if(task==='profile') {patch={profile:draft.profile};if(draft.profile!=='bitperfect')patch.style=draft.style;message=rlListeningProfileName(draft.profile);}
  else if(task==='style'){patch={style:draft.style};message=rlListeningStyleName(draft.style);}
  else {
    if(draft.route==='offline')patch={backend:'null',device:'null'};
    else if(draft.route==='network')patch={backend:'upnp',upnpdev:draft.upnpdev};
    else patch={backend:draft.backend==='asio'?'asio':draft.exclusive?'wasapi':'wasapi-shared',device:draft.device==='null'?'':draft.device};
    message=t('输出已更新。');
  }
  rlListeningSubmit(patch,message,true);
}
function rlListeningSameDialog(generation,task) {
  var dialog=D('rlListeningDialog');
  return !!(dialog&&dialog.open&&rlListening.dialogGeneration===generation&&rlListening.task===task);
}
function rlListeningSubmit(patch,message,closeCard) {
  if(rlListeningBusy())return;
  var generation=rlListening.dialogGeneration,task=rlListening.task;
  rlListening.working=true;rlListeningSay('rlListeningDialogMessage',t('正在切换…'));rlListeningSay('rlListeningNotice','');rlPaintListeningControls();
  return rlConsole.apply(patch).then(function(result){
    if(!result||result.ok!==true)throw Error(result&&result.error||t('切换未完成，请重试。'));
    if(closeCard&&rlListeningSameDialog(generation,task))D('rlListeningDialog').close();
    rlListeningSay('rlListeningNotice',message);
  }).catch(function(error){
    var msg=error.message||t('切换失败，请重试。');
    rlListeningSay('rlListeningNotice',msg,true);
    if(rlListeningSameDialog(generation,task))rlListeningSay('rlListeningDialogMessage',msg,true);
  }).finally(function(){
    rlListening.working=false;
    var select=D('rlListeningDevice');if(select)select.rlKey='';
    rlPaintListeningControls();
  });
}
)LISTENHOME";
}

} // namespace rta::net
