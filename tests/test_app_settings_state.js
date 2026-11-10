// Exercise production settings save failure branches, without network or DOM.
const fs=require('node:fs'),path=require('node:path'),vm=require('node:vm');
const root=process.env.RLPLAYER_TEST_SOURCE_ROOT||path.join(__dirname,'..');
const source=fs.readFileSync(path.join(root,'src/net/ui_script.h'),'utf8');
function fn(name){const a=source.indexOf('function '+name+'(');return source.slice(a,source.indexOf('\n}',a)+2);}
let checks=0,failures=0;
let unhandled=0;process.on('unhandledRejection',()=>++unhandled);
function check(ok,label){++checks;if(!ok)++failures;console.log((ok?'PASS ':'FAIL ')+label);}
async function run(lang,failed,debounced=false,manual=false){
 const notices=[],map=JSON.parse(fs.readFileSync(path.join(root,'data/i18n/'+lang+'.json'),'utf8'));
 const c={SET_KEYS:[],S:{vol:-60},ADV:{},ANL:{auto:0,master:0},META:{},META_PROVIDERS:[],META_SRC:{},
  D:id=>id==='sp_root'?null:({checked:false}),collectSettings:()=>{},aiSummaryOn:()=>false,window:{},clearTimeout:()=>{},setTimeout:f=>{c.timer=f;},
  api:()=>failed==='network'?Promise.reject(Error('offline')):Promise.resolve(failed?{ok:false,error:'写入失败'}:{ok:true}),toast:s=>notices.push(s),
  t:s=>map[s]||s,tf:(s,args)=>(map[s]||s).replace(/\{(\d+)\}/g,(_,i)=>args[i])};
 vm.createContext(c);if(source.includes('function persistSettings('))vm.runInContext(fn('persistSettings'),c);
 for(const name of ['sdmCornerSettingsValid','validateSdmCornerSettings'])if(source.includes('function '+name+'('))vm.runInContext(fn(name),c);
 vm.runInContext(fn('saveSettings'),c);vm.runInContext(fn('applySettings'),c);
 if(manual)c.applySettings();else c.saveSettings(!debounced);if(debounced)c.timer();await new Promise(r=>setImmediate(r));
 return notices;
}
(async()=>{for(const lang of ['en','ja','fr','ru','es','ar']){
 const errors=await run(lang,true);check(errors.length===1,lang+' failed save has explicit feedback');
 if(lang!=='ja')check(errors.length===1&&!/[\u3400-\u9fff]/.test(errors[0]),lang+' injected write failure is translated');
 check((await run(lang,false)).length===0,lang+' successful autosave stays quiet');
 const offline=await run(lang,'network');check(offline.length===1&&(lang==='ja'||!/[\u3400-\u9fff]/.test(offline[0])),lang+' rejected HTTP request has translated feedback and is handled');
 check((await run(lang,true,true)).length===1,lang+' debounced save also reports errors');
 const manualError=await run(lang,true,false,true);check(manualError.length===1&&errors.length===1&&manualError[0]===errors[0],lang+' manual failed save reports the same error and never announces success');
 const manualOk=await run(lang,false,false,true);check(manualOk.length===1,lang+' manual successful save announces completion once');
}check(unhandled===0,'save failures leave no unhandled promise rejections');console.log(JSON.stringify({checks,failures,unhandled}));process.exitCode=failures?1:0;})();
