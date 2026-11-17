"use strict";
// New PCM controls are exercised through production state/serializer/preset
// functions. This fixture never starts an app or opens an audio device.
const fs=require('node:fs'),vm=require('node:vm'),path=require('node:path');
const root=process.env.RLPLAYER_TEST_SOURCE_ROOT||path.resolve(__dirname,'..');
const core=fs.readFileSync(path.join(root,'src/net/ui_script.h'),'utf8'),studio=fs.readFileSync(path.join(root,'src/net/ui_studio.h'),'utf8');
const state=fs.readFileSync(path.join(root,'src/net/ui_workbench_state.h'),'utf8').match(/return R"WORKBENCH\(([\s\S]*?)\)WORKBENCH";/)[1];
function fn(s,name){const at=s.search(new RegExp('^function '+name+'\\(', 'm'));if(at<0)throw Error(name);return s.slice(at,s.indexOf('\n}',at)+2);}
let checks=0,failed=0;const check=(ok,s)=>{checks++;if(!ok)failed++;console.log((ok?'PASS ':'FAIL ')+s);};
(async()=>{
 let nodes={},notices=[],patches=[];
 const c={Promise,Object,Number,String,URLSearchParams,window:{},clearTimeout:()=>{},PLAYBACK_SETTINGS:{ready:true},RL_STUDIO:{switching:false,draft:{},editVersion:0},D:id=>nodes[id]||null,t:s=>s,toast:s=>notices.push(s),rlStudioSetValue:()=>{},rlStudioFlush:()=>{patches.push({...c.RL_STUDIO.draft});c.RL_STUDIO.draft={};return Promise.resolve();}};
 vm.createContext(c);vm.runInContext(state,c);
 const start=core.indexOf('var ADV = {');vm.runInContext(core.slice(start,core.indexOf('\n};',start)+3),c);
 for(const name of ['processingSettingsValid','validateProcessingSettings','processingSettingsParam'])vm.runInContext(fn(core,name),c);
 vm.runInContext(fn(studio,'rlStudioCrossfeedPreset'),c);
 let p=new URLSearchParams(c.processingSettingsParam());
 check(p.get('crossfeed')==='0'&&p.get('crossfeed_freq')==='700'&&p.get('crossfeed_level')==='12','first-run headphone processing is disabled with light 700 Hz / 12 dB defaults');
 check(p.get('dacbits')==='0','DAC effective bit depth defaults to output container');
 c.ADV.crossfeed=1;c.ADV.crossfeedFreq=650;c.ADV.crossfeedLevel=10;c.ADV.dacbits=18;p=new URLSearchParams(c.processingSettingsParam());
 check(p.get('crossfeed')==='1'&&p.get('crossfeed_freq')==='650'&&p.get('crossfeed_level')==='10'&&p.get('dacbits')==='18','legacy save and playback serializer preserve all four PCM settings');
 for(const key of ['dacbits','crossfeed','crossfeed_freq','crossfeed_level']){
  check(c.rlCompare.keys.includes(key)&&c.rlSettingsFence.keys.includes(key),key+' belongs to independent groups and stale-settings protection');
 }
 const settings={dacbits:20,crossfeed:1,crossfeed_freq:700,crossfeed_level:12,volume:-60};const selected=c.rlCompare.selectedSettings(settings);
 check(selected.dacbits==='20'&&selected.crossfeed==='1'&&selected.crossfeed_freq==='700'&&selected.crossfeed_level==='12'&&!Object.hasOwn(selected,'volume'),'group capture preserves audio controls without changing master volume');
 for(const [freq,level,valid] of [[300,3,true],[1200,15,true],[299,12,false],[1201,12,false],[700,2.9,false],[700,15.1,false],['',12,false],[700,'',false],[NaN,12,false],[700,Infinity,false]]){
  nodes={sp_crossfeed_freq:{value:freq},sp_crossfeed_level:{value:level}};check(c.validateProcessingSettings()===valid,'validation '+freq+' Hz / '+level+' dB = '+valid);
 }
 nodes={};await c.rlStudioCrossfeedPreset('light');check(JSON.stringify(patches.at(-1))===JSON.stringify({crossfeed:'1',crossfeed_freq:'700',crossfeed_level:'12'}),'light preset submits exactly 700 Hz / 12 dB to current group');
 await c.rlStudioCrossfeedPreset('standard');check(patches.at(-1).crossfeed==='1'&&patches.at(-1).crossfeed_level==='6','standard preset submits 6 dB');
 await c.rlStudioCrossfeedPreset('off');check(JSON.stringify(patches.at(-1))===JSON.stringify({crossfeed:'0'}),'off disables processing while retaining the user numeric preference');
 const count=patches.length;await c.rlStudioCrossfeedPreset('unexpected');c.RL_STUDIO.switching=true;await c.rlStudioCrossfeedPreset('light');c.RL_STUDIO.switching=false;c.PLAYBACK_SETTINGS.ready=false;await c.rlStudioCrossfeedPreset('standard');check(patches.length===count,'unknown presets, active group switches and unread settings cannot submit writes');
 for(const lang of ['en','ja','fr','ru','es','ar']){const d=JSON.parse(fs.readFileSync(path.join(root,'data/i18n',lang+'.json'),'utf8'));check(['DAC 有效位深','自动（跟随输出容器）','耳机交叉馈送会改变声像和频响；默认关闭。','轻度 · 12 dB','标准 · 6 dB','交叉馈送频率须为 300～1200 Hz，交叉衰减须为 3～15 dB。'].every(k=>d[k]&&d[k]!==k),lang+' explains effective bits and optional headphone processing');}
 console.log(JSON.stringify({checks,failed,actualAudioFrames:0}));process.exitCode=failed?1:0;
})().catch(e=>{console.error(e);process.exitCode=2;});
