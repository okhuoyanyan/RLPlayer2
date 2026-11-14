#pragma once

namespace rta::net {

inline const char* rta_ui_workbench_state_js() {
    return R"WORKBENCH(
// One transport tail owns parameter-changing HTTP only. Reads and UI refreshes
// run outside it; an intent epoch invalidates older captured settings forms.
var rlSettingsFence={epoch:0,pending:0,version:0,versions:{},tail:Promise.resolve(),waiters:[],
 keys:['filter','filterNx','rate','rateFamilyPolicy','dsd','dsdnative','directsdm','shaper','twostage','taps','atten','transition','phase','modorder','modfamily','modcut','moddith','oobgain','ahmbits','sdmfilt','sdmfiltnx','isp','eq','ir','irgpu','roomcorrauto','loudness','rgmode','rgclip','cleanup','cleanup_auto','cleanup_strength','cleanup_noise_db','dsdkind','dsdconv','dsd2stage','bits','channels','trim','backend','device','buffer','gpuvendor','upnpdev','ramp','opraproduct','oprapreset'],
 group:function(){return typeof rlCompare==='undefined'?'':rlCompare.activeId||'';},
 signature:function(){var s=typeof rlConsole!=='undefined'&&rlConsole.state;return JSON.stringify([this.keys.map(function(k){return s&&s.settings&&s.settings[k];}),s&&s.peq]);},
 busy:function(){return this.pending>0||(typeof rlConsole!=='undefined'&&rlConsole.busy)||(typeof rlCompare!=='undefined'&&rlCompare.parameterSaving)||(typeof rlWorkbenchQueue!=='undefined'&&rlWorkbenchQueue.active&&rlWorkbenchQueue.active.parameter);},
 mark:function(values){var self=this,names=typeof values==='string'?Array.from(new URLSearchParams(values).keys()):Object.keys(values||{});names.forEach(function(key){if(key!=='volume')self.versions[key]=++self.version;});},
 capture:function(){return {epoch:this.epoch,group:this.group(),signature:this.signature(),versions:Object.assign({},this.versions),busy:!!this.busy()};},
 begin:function(values){var lease={ready:this.tail,ended:false};this.mark(values);this.epoch++;this.pending++;if(typeof rlWorkbenchQueue!=='undefined'&&rlWorkbenchQueue.active)rlWorkbenchQueue.active.parameter=true;return lease;},
 end:function(lease){if(!lease||lease.ended)return;lease.ended=true;this.pending--;if(!this.pending){var waits=this.waiters.splice(0);waits.forEach(function(done){done();});}},
 http:function(action){var next=this.tail.then(action,action);this.tail=next.then(function(){},function(){});return next;},
 legacy:function(action,stamp){var self=this;function enqueue(){if(!stamp&&self.pending)return new Promise(function(done){self.waiters.push(done);}).then(enqueue);return self.http(action);}return enqueue();},
 stale:function(stamp){return !!stamp&&(stamp.busy||stamp.epoch!==this.epoch||stamp.group!==this.group()||this.busy());},
 groupKeys:function(){return (typeof rlCompare!=='undefined'&&rlCompare.keys||this.keys).concat(['opraproduct','oprapreset']);},
 reconcile:function(data,stamp){var self=this,removed=[],groupKeys=this.groupKeys(),stale=this.stale(stamp);if(!stamp)return {removed:removed,group:false};Array.from(data.keys()).forEach(function(key){if((stale&&groupKeys.indexOf(key)>=0)||(self.versions[key]||0)>(stamp.versions[key]||0)){data.delete(key);removed.push(key);}});var group=removed.some(function(key){return groupKeys.indexOf(key)>=0;});if(group)data.delete('volume');return {removed:removed,group:group};},
 hasParameters:function(body){var p=new URLSearchParams(body);return this.keys.some(function(k){return p.has(k);});},
 peqSignature:function(p){return JSON.stringify(p?{bypass:!!Number(p.bypass),preamp:Number(p.preamp),bands:(p.bands||[]).map(function(b){return [b.i,b.on===true||b.on===1||b.on==='1',b.type,Number(b.freq),Number(b.gain),Number(b.q)];})}:null);},
 kind:function(url,body){var path=url.split('?')[0],p=new URLSearchParams(body||'');
  if(path==='/api/workbench/profile')return p.get('action')==='rename'?'metadata':'parameter';
  if(path==='/api/workbench/eqpresets'){var action=p.get('action');if(action==='delete')return 'metadata';if(action==='save'){if(!p.has('peq'))return 'metadata';try{var state=typeof rlConsole!=='undefined'&&rlConsole.state;if(state&&this.peqSignature(JSON.parse(p.get('peq')))===this.peqSignature(state.peq))return 'metadata';}catch(e){}}return 'parameter';}
  if(path==='/api/peq/preset')return p.get('action')==='load'?'parameter':'metadata';
  if(path==='/api/workbench/apply'||path==='/api/preset/apply'||path==='/api/opra/apply')return 'parameter';
  if(path==='/api/peq'&&body!==undefined&&body!==null&&String(body)!=='')return 'parameter';return '';
 },
 request:function(action,kind,lease,body){var self=this;if(kind==='metadata')return self.legacy(action,null);lease=lease||self.begin(body);return lease.ready.then(function(){return self.http(action);}).finally(function(){self.end(lease);});},
 readback:function(){return rlConsole.refresh(true).then(function(){return loadSettings();}).then(function(){return rlCompare.refresh(true);}).catch(function(){return null;});}
};
var rlConsole = {
  state:null, busy:false, error:'', listeners:[], read:null, generation:0,
  subscribe:function(fn) { if (this.listeners.indexOf(fn)<0) this.listeners.push(fn); },
  notify:function() {
    this.listeners.forEach(function(fn) { try { fn(); } catch(e) { console.error('Workbench UI',e); } });
    if(typeof rlCompare!=='undefined')rlCompare.notify();
  },
  refresh:function(force) {
    var self=this;if(self.read&&!force)return self.read;
    var generation=++self.generation;
    var request=apiGetRaw('/api/workbench/state').then(function(j) {
      if(!j||!j.ok||!j.settings)throw Error(j&&j.error||t('无法读取当前播放参数'));
      if(generation===self.generation){self.state=j;self.error='';}return j;
    }).catch(function(e){if(generation===self.generation)self.error=e.message||t('无法读取当前播放参数');throw e;})
      .finally(function(){if(self.read===request)self.read=null;self.notify();});
    self.read=request;return request;
  },
  apply:function(patch) {
    var self=this, initiatedIdentity=self.state&&JSON.stringify([self.state.file,self.state.queueIndex,self.state.cueStart,self.state.cueEnd]);
    if(!ensurePlaybackSettings())return Promise.reject(Error(t('正在读取播放设置，请稍候')));
    if(self.busy)return Promise.reject(Error(t('正在切换，请稍候')));
    if(!patch || typeof patch!=='object' || !Object.keys(patch).length)return Promise.reject(Error(t('没有需要应用的更改')));
    var lease=rlSettingsFence.begin(patch);self.busy=true;self.error='';self.notify();
    var submitted=false;
    return lease.ready.then(function(){return self.refresh(true);}).then(function(current) {
      if(initiatedIdentity && JSON.stringify([current.file,current.queueIndex,current.cueStart,current.cueEnd])!==initiatedIdentity)
        throw Error(t('曲目已切换，请重新选择方案'));
      var body=new URLSearchParams();
      Object.keys(patch).forEach(function(key) {
        var value=patch[key];
        if(value!==undefined && value!==null)body.set(key,key==='peq'?JSON.stringify(value):String(value));
      });
      body.set('expected_file',current.file||'');
      body.set('expected_revision',String(current.revision));
      submitted=true;
      return apiAct('/api/workbench/apply',body.toString(),lease);
    }).then(function(j) {
      if(!j||!j.ok)throw Error(j&&j.error||t('切换失败'));
      return self.refresh(true);
    }).then(function(state) {
      // Restore the existing application's sole playback-parameter model from the saved result.
      return loadSettings().then(function(j) {
        if(!j || !j.ok)throw Error(t('切换已提交，参数同步失败，请刷新'));
        _capsCache=null;
        return typeof rlCompare!=='undefined'?rlCompare.refresh(true).then(function(){return state;}):state;
      });
    }).catch(function(e) {
      self.error=e.message||t('切换失败');
      // A timeout never resends a state-changing operation. Read its authoritative outcome.
      if(submitted)return self.refresh(true).catch(function(){}).then(function() {
        self.error=e.message||t('切换失败');throw e;
      });
      throw e;
    }).finally(function() {rlSettingsFence.end(lease);self.busy=false;self.notify();});
  },
  profile:function(name,style) {
    var patch={profile:name};if(style && name!=='bitperfect')patch.style=style;
    return this.apply(patch);
  }
};
// A single promise chain orders EQ changes, automatic parameter writes and slot switches.
// Failed requests are observed once and never replayed as mutations.
var rlWorkbenchQueue={tail:Promise.resolve(),pending:0,active:null,run:function(action){
  var self=this,token={parameter:false};self.pending++;
  function begin(){self.active=token;return action();}
  var next=self.tail.then(begin,begin);self.tail=next.catch(function(){});
  return next.finally(function(){self.pending--;if(self.active===token)self.active=null;});
}};
var rlCompare = {
  slots:[],activeId:'',loop:{enabled:false,start:0,end:0,file:''},saving:false,parameterSaving:false,error:'',listeners:[],read:null,generation:0,migrationRead:null,
  storageKey:'rlplayer-comparison-v1',legacy:null,migrationAttempted:false,
  keys:['filter','filterNx','rate','rateFamilyPolicy','dsd','dsdnative','directsdm','shaper','twostage',
    'taps','atten','transition','phase','modorder','modfamily','modcut','moddith','oobgain','ahmbits',
    'sdmfilt','sdmfiltnx','isp','eq','ir','irgpu','roomcorrauto','loudness','rgmode','rgclip','cleanup',
    'cleanup_auto','cleanup_strength','cleanup_noise_db','dsdkind','dsdconv','dsd2stage','bits','channels','trim'],
  subscribe:function(fn){if(this.listeners.indexOf(fn)<0)this.listeners.push(fn);},
  notify:function(){this.listeners.forEach(function(fn){try{fn();}catch(e){console.error('Comparison UI',e);}});},
  copy:function(value){return JSON.parse(JSON.stringify(value));},
  selectedSettings:function(settings){var out={};this.keys.forEach(function(k){if(settings&&settings[k]!==undefined)out[k]=String(settings[k]);});return out;},
  signature:function(settings,peq){return JSON.stringify([this.keys.map(function(k){return settings&&settings[k]!==undefined?String(settings[k]):null;}),
    peq?{bypass:Number(peq.bypass),preamp:Number(peq.preamp),bands:(peq.bands||[]).map(function(b){return [b.i,b.on===true||b.on===1||b.on==='1',b.type,Number(b.freq),Number(b.gain),Number(b.q)];})}:null]);},
  dirty:function(){var s=this.slots.find(function(s){return s.id===rlCompare.activeId;});return !!s&&!!rlConsole.state&&this.signature(s.settings,s.peq)!==this.signature(rlConsole.state.settings,rlConsole.state.peq);},
  form:function(patch){var body=new URLSearchParams();Object.keys(patch).forEach(function(k){var v=patch[k];if(v!==undefined&&v!==null)body.set(k,k==='peq'?JSON.stringify(v):String(v));});return body;},
  readServer:function(){var self=this,generation=++self.generation;return apiGetRaw('/api/workbench/profiles').then(function(j){
    if(!j||!j.ok||!Array.isArray(j.slots)||j.slots.length!==4)throw Error(j&&j.error||t('无法读取对比配置'));
    if(generation===self.generation){self.slots=j.slots;self.activeId=j.active;self.loop=j.loop||self.loop;self.error='';self.notify();}return j;
  });},
  refresh:function(force){var self=this;if(self.read&&!force)return self.read;
    var request=self.readServer().then(function(j){return self.migrateLegacy().then(function(){return j;});})
      .catch(function(e){self.error=e.message||t('无法读取对比配置');throw e;})
      .finally(function(){if(self.read===request)self.read=null;self.notify();});self.read=request;return request;
  },
  restore:function(){
    var saved=null;try{saved=JSON.parse(localStorage.getItem(this.storageKey)||'null');}catch(e){}
    this.legacy=saved&&saved.version===1&&Array.isArray(saved.slots)?saved:null;
    // Legacy storage is evidence for a one-time import, never the live source of truth.
    this.slots=['A','B','C','D'].map(function(id){return {id:id,name:id,settings:null,peq:null};});
  },
  migrateLegacy:function(){
    var self=this;if(self.migrationRead)return self.migrationRead;if(self.migrationAttempted||!self.legacy)return Promise.resolve();
    var selected='',chain=Promise.resolve();
    self.legacy.slots.forEach(function(old){
      var slot=old&&self.slots.find(function(s){return s.id===old.id;});
      if(!slot||slot.migrated||!old.settings||typeof old.settings!=='object')return;
      var keys=Object.keys(old.settings);
      if(!keys.length||!keys.every(function(k){return self.keys.indexOf(k)>=0&&['string','number','boolean'].indexOf(typeof old.settings[k])>=0&&String(old.settings[k]).length<32768;}))return;
      var patch=self.selectedSettings(old.settings);patch.action='migrate';patch.id=old.id;patch.name=String(old.name||old.id).slice(0,48);
      if(old.peq&&Array.isArray(old.peq.bands)&&old.peq.bands.length<=20)patch.peq=old.peq;
      chain=chain.then(function(){return apiAct('/api/workbench/profile',self.form(patch).toString()).then(function(j){
        if(!j||!j.ok)throw Error(j&&j.error||t('旧配置迁移失败'));
        if(self.legacy.activeId===old.id)selected=old.id;
      });});
    });
    self.migrationRead=chain.then(function(){self.migrationAttempted=true;
      if(selected)return apiAct('/api/workbench/profile',self.form({action:'select',id:selected}).toString()).then(function(j){if(!j||!j.ok)throw Error(j&&j.error||t('旧配置迁移失败'));});
    }).then(function(){return self.readServer();}).then(function(){return rlConsole.refresh(true);}).finally(function(){self.migrationRead=null;});return self.migrationRead;
  },
  mutate:function(patch,synchronize){
    var self=this;
    if(!ensurePlaybackSettings())return Promise.reject(Error(t('正在读取播放设置，请稍候')));
    if(self.saving||self.migrationRead||rlConsole.busy)return Promise.reject(Error(t('正在切换，请稍候')));
    var lease=patch.action==='rename'?null:rlSettingsFence.begin(patch);self.parameterSaving=!!lease;self.saving=true;self.error='';self.notify();
    return (lease?lease.ready:Promise.resolve()).then(function(){return apiAct('/api/workbench/profile',self.form(patch).toString(),lease);}).then(function(j){
      if(!j||!j.ok)throw Error(j&&j.error||t('切换失败'));return self.readServer();
    }).then(function(){if(synchronize!==false)return rlConsole.refresh(true).then(function(s){return loadSettings().then(function(){_capsCache=null;return s;});});return self.slots;})
      .catch(function(e){self.error=e.message||t('切换失败');return self.readServer().catch(function(){}).then(function(){return rlConsole.refresh(true).catch(function(){});}).then(function(){self.error=e.message||t('切换失败');throw e;});})
      .finally(function(){rlSettingsFence.end(lease);self.parameterSaving=false;self.saving=false;self.notify();});
  },
  load:function(id,fast){if(!['A','B','C','D'].includes(id))return Promise.reject(Error(t('未知的对比配置')));return this.mutate({action:'select',id:id},fast===true?false:undefined);},
  rename:function(id,name){var text=String(name||'').trim().slice(0,48);if(!text)return Promise.reject(Error(t('请填写配置名称')));return this.mutate({action:'rename',id:id,name:text},false);},
  update:function(patch,id){
    var self=this;if(id&&id!==self.activeId)return Promise.reject(Error(t('当前组已切换，请重新操作')));
    var body=self.copy(patch);body.action='update';body.id=id||self.activeId;return self.mutate(body);
  },
  move:function(delta){var ids=['A','B','C','D'],index=ids.indexOf(this.activeId);return this.load(ids[(Math.max(0,index)+(delta<0?-1:1)+4)%4]);},
  setLoop:function(loop){var self=this,body=self.form(loop);
    return apiAct('/api/workbench/loop',body.toString()).then(function(j){if(!j||!j.ok)throw Error(j&&j.error||t('片段设置失败'));return self.readServer();});
  }
};
rlCompare.restore();
function rlOpenInlineLibrary() {
  var dialog=D('rlLibraryDialog');
  if(!dialog){
    dialog=document.createElement('dialog');dialog.id='rlLibraryDialog';dialog.className='rlInlineLibrary';
    dialog.setAttribute('aria-labelledby','rlLibraryDialogTitle');
    dialog.innerHTML='<form method=dialog><div class=rlLibraryHead><h2 id=rlLibraryDialogTitle>添加音乐目录</h2><button class=btn value=close aria-label=关闭>×</button></div>'
      +'<label for=rlLibraryPath>音乐文件夹</label><input id=rlLibraryPath type=text dir=auto spellcheck=false>'
      +'<div class=rlLibraryActions><button type=button class=btn id=rlLibraryPick>选择文件夹</button><button type=button class="btn pri" id=rlLibraryAdd>添加并扫描</button></div>'
      +'<p id=rlLibraryInlineStatus role=status></p></form>';
    document.body.appendChild(dialog);
    D('rlLibraryPick').hidden=!WV;
    D('rlLibraryPick').onclick=function(){wcmd('pick-music-folder');};
    D('rlLibraryAdd').onclick=function(){
      var path=D('rlLibraryPath').value.trim(),button=this,status=D('rlLibraryInlineStatus');
      if(!path){D('rlLibraryPath').focus();return;}
      button.disabled=true;status.textContent=t('正在保存目录…');
      apiAct('/api/lib/roots','add='+encodeURIComponent(path)).then(function(j){
        if(!j||!j.ok)throw Error(j&&j.error||t('添加失败'));
        return apiAct('/api/lib/scan','');
      }).then(function(j){
        if(!j||!j.ok)throw Error(j&&j.error||t('目录已保存，扫描未能开始'));
        D('rlLibraryPath').value='';status.textContent=t('已添加，正在扫描');loadLibStats();
      }).catch(function(e){status.textContent=e.message;}).finally(function(){button.disabled=false;});
    };
    dialog.addEventListener('click',function(e){if(e.target===dialog)dialog.close();});
    applyI18n(dialog);
  }
  if(!dialog.open)dialog.showModal();
  D('rlLibraryPath').focus();
}
if(window.chrome&&window.chrome.webview)window.chrome.webview.addEventListener('message',function(e){
  var prefix='music-folder:';
  if(typeof e.data!=='string'||e.data.indexOf(prefix)!==0)return;
  var field=D('rlLibraryPath'),path=e.data.slice(prefix.length);
  if(field&&field.isConnected&&path){field.value=path;field.focus();}
  else if(typeof rlReceiveMusicFolder==='function')rlReceiveMusicFolder(path);
});

)WORKBENCH";
}

} // namespace rta::net
