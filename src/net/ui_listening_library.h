#pragma once

namespace rta::net {
inline const char* rta_ui_listening_library_js() {
    return R"LISTENLIB(
function rlListeningFolderActive() {
  var d=D('rlListeningDialog');return !!(d&&d.open&&rlListening.task==='library');
}
function rlListeningFolderBusyPaint() {
  if(!rlListeningFolderActive())return;
  ['rlListeningFolderPath','rlListeningFolderPick','rlListeningFolderAdd','rlListeningFolderScan'].forEach(function(id){
    var n=D(id);if(n)n.disabled=!!rlListening.folderBusy||(id==='rlListeningFolderScan'&&!(rlListening.folderRoots||[]).length);
  });
  D('rlListeningFolderList').querySelectorAll('button').forEach(function(b){b.disabled=!!rlListening.folderBusy;});
}
function rlListeningPaintFolders(j) {
  var roots=Array.isArray(j.roots)?j.roots:[];
  rlListening.folderRoots=roots.slice();rlListening.foldersLoaded=true;S.roots=roots.slice();
  var hint=D('rlListeningLibraryHint');if(hint)hint.hidden=roots.length>0;
  if(!rlListeningFolderActive())return;
  rlSetText('rlListeningFolderCount',tf('已添加 {0} 个音乐文件夹',[roots.length]));
  var list=D('rlListeningFolderList'),key=JSON.stringify([roots,I18N.lang]);
  if(list.rlKey!==key){
    list.rlKey=key;list.replaceChildren();
    if(!roots.length){var empty=document.createElement('li');empty.textContent=t('还没有添加音乐文件夹。');list.appendChild(empty);}
    roots.forEach(function(path){
      var row=document.createElement('li'),label=document.createElement('span'),remove=document.createElement('button');
      label.textContent=path;label.title=path;remove.type='button';remove.className='btn';remove.textContent=t('移除');
      remove.setAttribute('aria-label',tf('移除音乐文件夹：{0}',[path]));
      remove.onclick=function(){rlListeningChangeFolder('remove',path);};row.append(label,remove);list.appendChild(row);
    });
  }
  rlSetText('rlListeningFolderStatus',j.scanning?tf('正在整理音乐：{0} / {1}',[j.scanned||0,j.total||0]):tf('音乐库共有 {0} 首曲目',[j.tracks||0]));
  rlListeningFolderBusyPaint();
}
function rlListeningFetchFolders(force) {
  if(rlListening.foldersLoading&&!force)return Promise.resolve();
  var readGeneration=(rlListening.folderReadGeneration||0)+1;
  rlListening.folderReadGeneration=readGeneration;
  rlListening.foldersLoading=true;
  return apiGetRaw('/api/lib/stats').then(function(j){
    if(!j||j.ok===false||!Array.isArray(j.roots))throw Error(j&&j.error||t('音乐文件夹读取失败，请重试。'));
    if(readGeneration===rlListening.folderReadGeneration)rlListeningPaintFolders(j);return j;
  }).catch(function(e){
    if(readGeneration===rlListening.folderReadGeneration&&rlListeningFolderActive())rlListeningSay('rlListeningDialogMessage',e.message||t('音乐文件夹读取失败，请重试。'),true);
  }).finally(function(){if(readGeneration===rlListening.folderReadGeneration)rlListening.foldersLoading=false;});
}
function rlListeningOpenLibrary(opener) {
  if(!D('rlListeningDialog'))rlListeningInstallDialog();
  var dialog=D('rlListeningDialog');if(dialog.open)dialog.close();
  ++rlListening.dialogGeneration;
  rlListening.task='library';rlListening.draft={};rlListening.opener=opener||document.activeElement;
  rlSetText('rlListeningDialogTitle',t('音乐文件夹'));D('rlListeningDialogApply').hidden=true;rlSetText('rlListeningDialogCancel',t('完成'));
  D('rlListeningDialogBody').innerHTML='<p class=rlListeningHint>选择保存音乐的文件夹，添加后会自动整理歌曲。</p>'
    +'<label class=rlListeningField><span>添加音乐文件夹</span><div class=rlListeningFolderInput>'
    +'<input type=text id=rlListeningFolderPath autocomplete=off spellcheck=false placeholder="选择或粘贴音乐文件夹路径">'
    +'<button type=button class=btn id=rlListeningFolderPick>选择文件夹</button></div></label>'
    +'<div class=rlListeningFolderActions><button type=button class="btn pri" id=rlListeningFolderAdd>添加到音乐库</button>'
    +'<button type=button class=btn id=rlListeningFolderScan>刷新音乐库</button></div>'
    +'<p class=rlListeningHint id=rlListeningFolderCount></p><ul class=rlListeningFolderList id=rlListeningFolderList></ul>'
    +'<p class=rlListeningHint id=rlListeningFolderStatus role=status></p>'
    +'<p class=rlListeningHint>移除只会更改音乐库列表，音乐文件会保留。</p>';
  var nativePicker=typeof WV!=='undefined'&&WV;
  D('rlListeningFolderPick').hidden=!nativePicker;
  D('rlListeningFolderPick').onclick=function(){if(nativePicker)wcmd('pick-music-folder');};
  D('rlListeningFolderAdd').onclick=function(){rlListeningChangeFolder('add');};
  D('rlListeningFolderScan').onclick=function(){rlListeningScanFolders();};
  D('rlListeningFolderPath').onkeydown=function(e){if(e.key==='Enter'){e.preventDefault();e.stopPropagation();rlListeningChangeFolder('add');}};
  rlListeningSay('rlListeningDialogMessage','');applyI18n(dialog);dialog.showModal();
  if(rlListening.folderRoots)rlListeningPaintFolders({roots:rlListening.folderRoots,tracks:S.libStats&&S.libStats.tracks||0});
  rlListeningFetchFolders(true);D('rlListeningFolderPath').focus();
  if(rlListening.folderTimer)clearInterval(rlListening.folderTimer);
  rlListening.folderTimer=setInterval(function(){
    if(!rlListeningFolderActive()){clearInterval(rlListening.folderTimer);rlListening.folderTimer=null;return;}
    if(!rlListening.folderBusy)rlListeningFetchFolders();
  },1600);
}
function rlListeningFolderPicked(path) {
  if(!rlListeningFolderActive())return false;
  var field=D('rlListeningFolderPath');if(field&&typeof path==='string'&&path){field.value=path;field.focus();}
  return true;
}
function rlListeningChangeFolder(action,path) {
  if(rlListening.folderBusy||!rlListeningFolderActive())return;
  var field=D('rlListeningFolderPath');path=String(path===undefined?field.value:path).trim();
  if(!path){rlListeningSay('rlListeningDialogMessage',t('请先选择音乐文件夹。'),true);field.focus();return;}
  rlListening.folderBusy=true;rlListeningFolderBusyPaint();rlListeningSay('rlListeningDialogMessage',t('正在更新音乐库…'));
  var generation=rlListening.dialogGeneration;
  rlListening.folderReadGeneration=(rlListening.folderReadGeneration||0)+1;
  var saved=false;
  return apiAct('/api/lib/roots',action+'='+encodeURIComponent(path)).then(function(j){
    if(!j||j.ok!==true)throw Error(j&&j.error||t('音乐文件夹更新失败，请重试。'));
    saved=true;if(action==='add'&&field&&field.isConnected)field.value='';
    return apiAct('/api/lib/scan','');
  }).then(function(j){
    if(!j||j.ok!==true)throw Error(j&&j.error||t('扫描失败'));
    if(rlListeningSameDialog(generation,'library'))rlListeningSay('rlListeningDialogMessage',t(action==='add'?'已添加，正在整理歌曲。':'已移除，正在更新歌曲列表。'));
  }).catch(function(e){
    if(rlListeningSameDialog(generation,'library'))rlListeningSay('rlListeningDialogMessage',(saved?t('文件夹列表已保存。')+' ':'')+(e.message||t('音乐文件夹更新失败，请重试。')),true);
  }).finally(function(){rlListening.folderBusy=false;rlListeningFolderBusyPaint();rlListeningFetchFolders(true);if(typeof loadLibStats==='function')loadLibStats();});
}
function rlListeningScanFolders() {
  if(rlListening.folderBusy||!rlListeningFolderActive())return;
  rlListening.folderBusy=true;rlListeningFolderBusyPaint();rlListeningSay('rlListeningDialogMessage',t('正在整理音乐…'));
  var generation=rlListening.dialogGeneration;
  return apiAct('/api/lib/scan','').then(function(j){
    if(!j||j.ok!==true)throw Error(j&&j.error||t('扫描失败'));
    if(rlListeningSameDialog(generation,'library'))rlListeningSay('rlListeningDialogMessage',t('正在整理歌曲，完成后会自动显示。'));
  }).catch(function(e){if(rlListeningSameDialog(generation,'library'))rlListeningSay('rlListeningDialogMessage',e.message||t('扫描失败'),true);})
    .finally(function(){rlListening.folderBusy=false;rlListeningFolderBusyPaint();rlListeningFetchFolders();if(typeof loadLibStats==='function')loadLibStats();});
}
)LISTENLIB";
}
} // namespace rta::net
