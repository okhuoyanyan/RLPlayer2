// Actual settings validation/save and exact product error class; no network/device/profile.
const fs=require('node:fs'),vm=require('node:vm'),path=require('node:path');
const root=process.env.RLPLAYER_TEST_SOURCE_ROOT||path.join(__dirname,'..');const source=fs.readFileSync(path.join(root,'src/net/ui_script.h'),'utf8');
const stateSource=fs.readFileSync(path.join(root,'src/net/ui_workbench_state.h'),'utf8').match(/return R"WORKBENCH\(([\s\S]*?)\)WORKBENCH";/)[1];
function fn(n){const a=source.indexOf('function '+n+'(');if(a<0)throw new Error('Missing production dependency: '+n);return source.slice(a,source.indexOf('\n}',a)+2);}
const advStart=source.indexOf('var ADV = {');if(advStart<0)throw new Error('Missing production ADV defaults');
const advDefaults=source.slice(advStart,source.indexOf('\n}',advStart)+2);
let checks=0,failures=0;function ck(v,s){checks++;failures+=!v;console.log((v?'PASS ':'FAIL ')+s);}
const quality='SDM_CORNER_QUALITY: One-bit noise-shaping corners below 100 kHz can distort the audio band; use 0/default 100 kHz or the EC family.';
(async()=>{for(const lang of ['en','ja','fr','ru','es','ar']){
 const map=JSON.parse(fs.readFileSync(path.join(root,'data/i18n/'+lang+'.json'),'utf8'));const notices=[];let calls=0,collects=0,lastBody='';let fields={sp_modcut:{value:'20'},sp_modfamily:{value:'dsd'}};
 // SDM validation uses the real persistence/volume helpers with an already
 // confirmed -60 dB collaborator. Gesture races belong to the whole-page test.
 const c={I18N:{map},S:{vol:-60,rawVolumeLocked:false},PLAYBACK_SETTINGS:{ready:true},ANL:{auto:0,master:0},META:{},META_PROVIDERS:[],META_SRC:{},SET_KEYS:[],window:{},
  URLSearchParams,settingsSaveQueue:Promise.resolve(),VOLUME_WRITE:{sequence:0,current:null,confirmed:-60,statusBarrier:0},
  D:id=>fields[id]||(id==='sLoud'?{checked:false,value:''}:null),toast:s=>notices.push(s),clearTimeout:()=>{},setTimeout:()=>0,
  api:async(_u,body)=>{calls++;lastBody=body;return {ok:true};},aiSummaryOn:()=>false,collectSettings:()=>{collects++;}};vm.createContext(c);vm.runInContext(stateSource,c);vm.runInContext(advDefaults,c);
 for(const n of ['i18nNorm','i18nGet','t','tf','playbackErrorText','playbackSettingsMessage','playbackSettingsBlocked','ensurePlaybackSettings','sdmCornerSettingsValid','validateSdmCornerSettings','processingSettingsValid','validateProcessingSettings','processingSettingsParam','isRawVolumeLocked','confirmedVolumeForSave','persistSettings','saveSettings','applySettings'])vm.runInContext(fn(n),c);
 c.PLAYBACK_SETTINGS.ready=false;const notLoaded=await c.saveSettings(true);ck(notLoaded&&!notLoaded.ok&&notLoaded.errorCode==='PLAYBACK_SETTINGS_NOT_READY'&&calls===0,lang+' unread playback settings reject before SDM/save side effects');const notApplied=await c.applySettings();ck(notApplied&&!notApplied.ok&&calls===0&&collects===0,lang+' unread settings reject Apply before collecting or writing defaults');c.PLAYBACK_SETTINGS.ready=true;
 for(const family of ['dsd','chebyshev','ahm'])for(const value of [20,50,99.9,-1,201,NaN,Infinity])ck(!c.sdmCornerSettingsValid(value,family),lang+' non-EC invalid/unverified corner rejected '+family+' '+value);
 for(const value of [0,100,150,200])ck(c.sdmCornerSettingsValid(value,'dsd'),lang+' supported/default corner accepted '+value);
 ck(c.sdmCornerSettingsValid(20,'ec')&&c.sdmCornerSettingsValid(50,'ec'),lang+' EC can retain its unused low-corner value');
 await c.applySettings();ck(calls===0&&collects===0&&c.ADV.modcut===0&&fields.sp_modcut.value==='20',lang+' refused save has no side effects and preserves exact input');
 ck(notices.at(-1)===map['SDM 拐点必须为 0 或 100～200 kHz；EC 族不使用此值，但仍须是 0～200 的有限数字。输入已保留，请修正后再保存。'],lang+' refused save has localized correction guidance');
 fields={};c.ADV.modcut=20;calls=0;await c.saveSettings(true);ck(calls===0&&c.ADV.modcut===20,lang+' restored low legacy setting is not silently changed or re-saved');
 c.ADV.modfamily='ec';calls=0;await c.saveSettings(true);ck(calls===1&&lastBody.includes('modcut=20')&&lastBody.includes('modfamily=ec'),lang+' legitimate EC save preserves requested unused value');
 ck(lastBody.includes('cleanup_strength=0.5')&&lastBody.includes('cleanup_noise_db=-101.1'),lang+' absent optional cleanup controls use actual production defaults');
 // The same real saveSettings must obey both independent validation gates.
 c.ADV.modcut=150;c.ADV.modfamily='dsd';
 const guidance='清理强度须为 0～1，噪声底须为 -120～-60 dBFS，且必须是有限数字。请修正后再保存。';
 for(const [key,value] of [['sp_cleanup_strength',''],['sp_cleanup_strength','-0.1'],['sp_cleanup_strength','1.01'],['sp_cleanup_strength','NaN'],['sp_cleanup_strength','Infinity'],['sp_cleanup_noise','-121'],['sp_cleanup_noise','-59'],['sp_cleanup_noise','NaN'],['sp_cleanup_noise','Infinity']]){
  fields={sp_modcut:{value:'150'},sp_modfamily:{value:'dsd'},sp_cleanup_strength:{value:'0.5'},sp_cleanup_noise:{value:'-101.1'}};fields[key].value=value;calls=0;
  const result=await c.saveSettings(true);
  ck(result&&result.ok===false&&result.errorCode==='PROCESSING_INPUT'&&calls===0,lang+' valid SDM corner cannot bypass cleanup refusal '+key+'='+value);
  ck(fields[key].value===value&&c.ADV.cleanupStrength===.5&&c.ADV.cleanupNoiseDb===-101.1,lang+' cleanup refusal preserves exact field and committed defaults '+key+'='+value);
  ck(notices.at(-1)===map[guidance],lang+' cleanup refusal uses actual localized production guidance '+key+'='+value);
 }
 fields.sp_modcut.value='20';fields.sp_cleanup_noise.value='-59';calls=0;
 const bothInvalid=await c.saveSettings(true);
 ck(bothInvalid&&bothInvalid.errorCode==='SDM_CORNER_QUALITY'&&calls===0&&fields.sp_modcut.value==='20'&&fields.sp_cleanup_noise.value==='-59',lang+' simultaneous invalid values retain SDM refusal precedence without side effects');
 fields={};c.ADV.modcut=150;c.ADV.cleanupStrength=0;c.ADV.cleanupNoiseDb=-120;calls=0;
 const boundary=await c.saveSettings(true);
 ck(boundary&&boundary.ok&&calls===1&&lastBody.includes('cleanup_strength=0')&&lastBody.includes('cleanup_noise_db=-120')&&lastBody.includes('modcut=150'),lang+' valid cleanup lower bounds serialize exactly with supported SDM corner');
 ck(c.playbackErrorText(quality)===map['低于 100 kHz 的 1-bit 噪声整形拐点可能造成音频带失真，已拒绝输出。请选择 0／默认 100 kHz，或使用 EC。'],lang+' exact backend quality refusal localizes');
 ck(c.playbackErrorText('异常: '+quality)===(map['渲染失败：{0}']||'MISSING').replace('{0}',map['低于 100 kHz 的 1-bit 噪声整形拐点可能造成音频带失真，已拒绝输出。请选择 0／默认 100 kHz，或使用 EC。']),lang+' actual backend exception prefix and quality message both localize');
 ck(c.playbackErrorText('SDM corner must be finite and positive')===map['SDM 拐点必须为 0 或 100～200 kHz；EC 族不使用此值，但仍须是 0～200 的有限数字。输入已保留，请修正后再保存。'],lang+' exact backend nonfinite corner refusal localizes');
 const raw='D:/'+quality+'.wav';ck(c.playbackErrorText(raw)===raw,lang+' arbitrary filename is not translated as an error code');
}ck(source.includes("ADV.modfamily = D('sp_modfamily').value || 'dsd'"),'settings collector saves the selected family, including EC');
console.log(JSON.stringify({checks,failures}));process.exitCode=failures?1:0;})();
