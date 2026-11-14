// Exercise production settings save failure branches, without network or DOM.
const fs=require('node:fs'),path=require('node:path'),vm=require('node:vm');
const root=process.env.RLPLAYER_TEST_SOURCE_ROOT||path.join(__dirname,'..');
const source=fs.readFileSync(path.join(root,'src/net/ui_script.h'),'utf8');
const stateSource=fs.readFileSync(path.join(root,'src/net/ui_workbench_state.h'),'utf8').match(/return R"WORKBENCH\(([\s\S]*?)\)WORKBENCH";/)[1];
function fn(name){const a=source.indexOf('function '+name+'(');if(a<0)throw Error('Missing production helper '+name);return source.slice(a,source.indexOf('\n}',a)+2);}
let checks=0,failures=0;
let unhandled=0;process.on('unhandledRejection',()=>++unhandled);
function check(ok,label){++checks;if(!ok)++failures;console.log((ok?'PASS ':'FAIL ')+label);}
async function run(lang,failed,debounced=false,manual=false){
 const notices=[],map=JSON.parse(fs.readFileSync(path.join(root,'data/i18n/'+lang+'.json'),'utf8'));
 const c={SET_KEYS:[],S:{vol:-60,rawVolumeLocked:false},PLAYBACK_SETTINGS:{ready:true},VOLUME_WRITE:{sequence:0,current:null,confirmed:-60,statusBarrier:0},settingsSaveQueue:Promise.resolve(),URLSearchParams,ANL:{auto:0,master:0},META:{},META_PROVIDERS:[],META_SRC:{},
  D:id=>id==='sLoud'?{checked:false}:null,collectSettings:()=>{},aiSummaryOn:()=>false,window:{},clearTimeout:()=>{},setTimeout:f=>{c.timer=f;},
  api:()=>failed==='network'?Promise.reject(Error('offline')):Promise.resolve(failed?{ok:false,error:'写入失败'}:{ok:true}),toast:s=>notices.push(s),
  t:s=>map[s]||s,tf:(s,args)=>(map[s]||s).replace(/\{(\d+)\}/g,(_,i)=>args[i])};
 vm.createContext(c);
 vm.runInContext(stateSource,c);
 // Keep the production defaults and validators, including both independent
 // SDM and cleanup gates. Only HTTP and the surrounding DOM are fixtures.
 const defaults=source.indexOf('var ADV = {');if(defaults<0)throw Error('Production ADV defaults missing');
 vm.runInContext(source.slice(defaults,source.indexOf('\n};',defaults)+3),c);
 for(const name of ['ensurePlaybackSettings','isRawVolumeLocked','confirmedVolumeForSave','processingSettingsValid','validateProcessingSettings','processingSettingsParam','sdmCornerSettingsValid','validateSdmCornerSettings','persistSettings','saveSettings','applySettings'])vm.runInContext(fn(name),c);
 if(manual)c.applySettings();else c.saveSettings(!debounced);if(debounced)c.timer();await new Promise(r=>setImmediate(r));
 return notices;
}
// A deterministic transport seam executes the production API wrapper, settings
// snapshot, and workbench models together. Held requests do not apply to the
// fake server until explicitly released, so response-only delays cannot pass.
async function parameterContracts(){
 const clone=x=>JSON.parse(JSON.stringify(x));
 const spin=async predicate=>{const end=Date.now()+3000;while(!predicate()){if(Date.now()>end)throw Error('Parameter contract did not reach its controlled barrier');await new Promise(r=>setImmediate(r));}};
 async function fixture(){
  const saved={rate:'88200',filter:'poly-sinc-short-lp',dsd:'0',backend:'null',device:'null',volume:'-65',cleanup_auto:'0',cleanup_noise_db:'-95',upnpdev:'',theme:'dark',uilang:'en'};
  const server={saved,active:'A',revision:1,peq:{bypass:1,preamp:0,bands:[]},slots:{A:{rate:'88200',filter:'poly-sinc-short-lp',cleanup_auto:'0'},B:{rate:'176400',filter:'poly-sinc-gauss-medium',cleanup_auto:'1'}}};
  const writes=[],applied=[],notices=[],reads=[];let timer=null,held=null,holdNext=false;
  const fields={sRate:{value:'88200',type:'select-one'},sFilt:{value:'poly-sinc-short-lp',type:'select-one'},sDSD:{value:'0',type:'select-one'},sLoud:{checked:false}};
  const c={Promise,URLSearchParams,Date,console,SET_KEYS:[['sRate','rate'],['sFilt','filter'],['sDSD','dsd']],S:{vol:-65,theme:'dark',rawVolumeLocked:false},PLAYBACK_SETTINGS:{ready:true},VOLUME_WRITE:{sequence:0,current:null,confirmed:-65,statusBarrier:0},settingsSaveQueue:Promise.resolve(),ANL:{auto:0,master:0},META:{},META_PROVIDERS:[],META_SRC:{},window:{},localStorage:{getItem:()=>null},D:id=>fields[id]||null,aiSummaryOn:()=>false,collectSettings:()=>{},toast:x=>notices.push(x),t:x=>x,tf:(x,args)=>x.replace(/\{(\d+)\}/g,(_,i)=>args[i]),setTimeout:f=>{timer=f;return 1;},clearTimeout:()=>{timer=null;},_capsCache:null};
  const state=()=>({ok:true,settings:clone(saved),peq:clone(server.peq),file:'Q:/parameter-contract.wav',revision:String(server.revision),queueIndex:0,cueStart:-1,cueEnd:-1,position:0,duration:30,playing:true,paused:true});
  const profiles=()=>({ok:true,active:server.active,slots:['A','B','C','D'].map(id=>({id,name:server['name'+id]||id,settings:clone(server.slots[id]||server.slots.A),peq:clone(server.peq)})),loop:{}});
  c.apiGetRaw=async u=>{reads.push(u);return u==='/api/workbench/profiles'?profiles():u==='/api/settings'?{ok:true,kv:clone(saved)}:state();};
  c.loadSettings=async()=>{fields.sRate.value=saved.rate;fields.sFilt.value=saved.filter;fields.sDSD.value=saved.dsd;c.ADV.cleanupAuto=Number(saved.cleanup_auto);c.ADV.cleanupNoiseDb=Number(saved.cleanup_noise_db);c.ADV.upnpDev=saved.upnpdev;return {ok:true,kv:clone(saved)};};
  function commit(u,p){
   if(u==='/api/settings'){Object.assign(saved,p);for(const key of c.rlCompare.keys)if(key in p)server.slots[server.active][key]=p[key];if(Object.keys(p).some(k=>c.rlSettingsFence.keys.includes(k)))server.revision++;}
   else if(u==='/api/workbench/profile'){
    if(p.action==='rename')server['name'+p.id]=p.name;
    else if(p.action==='select'){server.active=p.id;Object.assign(saved,server.slots[p.id]);server.revision++;}
    else{for(const [k,v] of Object.entries(p))if(!['action','id'].includes(k)){saved[k]=v;server.slots[server.active][k]=v;}server.revision++;}
   }else if(u==='/api/workbench/apply'){if(p.expected_revision!==String(server.revision))return {ok:false,error:'stale revision'};for(const [k,v] of Object.entries(p))if(!k.startsWith('expected_'))saved[k]=v;server.revision++;}
   else if(u==='/api/preset/apply'){Object.assign(saved,{rate:'src',filter:'',dsd:'0'});Object.assign(server.slots[server.active],{rate:'src',filter:'',dsd:'0'});server.revision++;}
   applied.push(u);return {ok:true};
  }
  c.apiRaw=(url,body)=>{const u=url.split('?')[0],p=Object.fromEntries(new URLSearchParams(body));writes.push({u,p});if(holdNext){holdNext=false;return new Promise(resolve=>{held=()=>{held=null;resolve(commit(u,p));};});}return Promise.resolve(commit(u,p));};
  vm.createContext(c);vm.runInContext(stateSource,c);
  const defaults=source.indexOf('var ADV = {');vm.runInContext(source.slice(defaults,source.indexOf('\n};',defaults)+3),c);
  Object.assign(c.ADV,{backend:'null',device:'null',cleanupAuto:0,cleanupNoiseDb:-95});
  for(const n of ['api','apiAct','ensurePlaybackSettings','isRawVolumeLocked','confirmedVolumeForSave','processingSettingsValid','validateProcessingSettings','processingSettingsParam','sdmCornerSettingsValid','validateSdmCornerSettings','persistSettings','saveSettings','applySettings','syncUpnpDev'])vm.runInContext(fn(n),c);
  await c.rlConsole.refresh();await c.rlCompare.refresh();
  return {c,server,writes,applied,notices,reads,fields,hold:()=>{holdNext=true;},waitHeld:()=>spin(()=>!!held),release:()=>{if(!held)throw Error('No held parameter HTTP');held();},fire:async()=>{const f=timer;timer=null;if(!f)throw Error('No captured settings timer');f();await c.settingsSaveQueue;await new Promise(r=>setImmediate(r));}};
 }
 let f=await fixture();f.fields.sRate.value='176400';const ordinary=await f.c.saveSettings(true);
 check(ordinary.ok&&f.server.saved.rate==='176400','unchanged-group explicit legacy DSP edits retain their normal save semantics');
 f=await fixture();f.c.S.theme='light';f.c.ADV.uiLang='ja';f.c.saveSettings(false);await f.c.rlCompare.update({cleanup_auto:'1'},'A');await f.fire();
 check(f.server.saved.cleanup_auto==='1'&&f.server.saved.theme==='light'&&f.server.saved.uilang==='ja','a captured theme/language preference cannot revert a newer parameter intent');
 check(!Object.hasOwn(f.writes.at(-1).p,'cleanup_auto')&&!Object.hasOwn(f.writes.at(-1).p,'volume'),'a stale DSP snapshot sends neither its old processing fields nor incidental gain');
 f=await fixture();f.c.S.theme='light';f.c.saveSettings(false);await f.c.syncUpnpDev('http://fixture.invalid/new-renderer.xml');await f.fire();
 check(f.server.saved.upnpdev==='http://fixture.invalid/new-renderer.xml'&&f.server.saved.theme==='light','new scoped renderer selection survives an older full preference snapshot');
 check(!Object.hasOwn(f.writes.at(-1).p,'upnpdev')&&Object.hasOwn(f.writes.at(-1).p,'cleanup_auto'),'per-key rebasing preserves valid legacy DSP while omitting only the superseded address');
 f=await fixture();f.c.S.theme='blue';f.c.saveSettings(false);await f.c.rlConsole.apply({backend:'null',device:'new-owned-null'});await f.fire();
 check(f.server.saved.device==='new-owned-null'&&f.server.saved.theme==='blue','new workbench backend/device selection survives an older preference snapshot');
 check(!Object.hasOwn(f.writes.at(-1).p,'device'),'a superseded global output key is not sent by a late full form');
 f=await fixture();f.c.S.theme='light';f.c.saveSettings(false);await f.c.rlCompare.load('B');await f.fire();
 check(f.server.active==='B'&&f.server.saved.rate==='176400'&&f.server.saved.cleanup_auto==='1'&&f.server.saved.theme==='light','a captured A form cannot apply its DSP to a later selected B group');
 f=await fixture();f.fields.sRate.value='176400';f.c.saveSettings(false);const epoch=f.c.rlSettingsFence.epoch;await f.c.rlCompare.rename('A','Renamed without DSP');await f.fire();
 check(f.c.rlSettingsFence.epoch===epoch&&f.server.saved.rate==='176400'&&Object.hasOwn(f.writes.at(-1).p,'rate'),'pure group rename does not cancel a legitimate captured DSP edit');
 f=await fixture();f.hold();const older=f.c.saveSettings(true);await f.waitHeld();const newer=f.c.rlCompare.update({cleanup_auto:'1'},'A');await new Promise(r=>setImmediate(r));
 check(f.writes.filter(w=>w.u==='/api/workbench/profile').length===0,'a new group HTTP cannot overtake an already issued held legacy HTTP');
 f.release();await Promise.all([older,newer]);
 check(f.server.saved.cleanup_auto==='1'&&f.applied.join(',')==='/api/settings,/api/workbench/profile','an already issued older form finishes before the newer group intent, without replay');
 f=await fixture();f.hold();const pending=f.c.rlCompare.update({cleanup_auto:'1'},'A');await f.waitHeld();const explicit=f.c.applySettings();f.release();const results=await Promise.all([pending,explicit]);
 check(results[1].stale&&!results[1].ok&&results[1].preferencesSaved&&f.server.saved.cleanup_auto==='1','explicit stale Apply reports partial preference success and does not overwrite pending group DSP');
 check(!f.notices.some(x=>x.startsWith('设置已保存'))&&f.reads.filter(u=>u==='/api/workbench/state').length>=3,'stale explicit Apply reads back real state and never announces applied success');
 f=await fixture();f.c.S.theme='blue';f.c.saveSettings(false);await f.c.api('/api/preset/apply?profile=bitperfect','');await f.fire();
 check(f.server.saved.rate==='src'&&f.server.saved.filter===''&&f.server.saved.theme==='blue','the real legacy preset endpoint advances intent before a delayed preference form, without polling-dependent protection');
 check(f.c.rlSettingsFence.pending===0,'every parameter lease is released after HTTP and readback completion');
}
(async()=>{for(const lang of ['en','ja','fr','ru','es','ar']){
 const errors=await run(lang,true);check(errors.length===1,lang+' failed save has explicit feedback');
 if(lang!=='ja')check(errors.length===1&&!/[\u3400-\u9fff]/.test(errors[0]),lang+' injected write failure is translated');
 check((await run(lang,false)).length===0,lang+' successful autosave stays quiet');
 const offline=await run(lang,'network');check(offline.length===1&&(lang==='ja'||!/[\u3400-\u9fff]/.test(offline[0])),lang+' rejected HTTP request has translated feedback and is handled');
 check((await run(lang,true,true)).length===1,lang+' debounced save also reports errors');
 const manualError=await run(lang,true,false,true);check(manualError.length===1&&errors.length===1&&manualError[0]===errors[0],lang+' manual failed save reports the same error and never announces success');
 const manualOk=await run(lang,false,false,true);check(manualOk.length===1,lang+' manual successful save announces completion once');
}await parameterContracts();check(unhandled===0,'save failures leave no unhandled promise rejections');if(checks<57)throw Error('No complete settings regression was executed');console.log(JSON.stringify({checks,failures,unhandled}));process.exitCode=failures?1:0;})().catch(error=>{console.error(error);process.exitCode=1;});
