const fs=require('node:fs'),path=require('node:path'),vm=require('node:vm');
const root=process.env.RLPLAYER_TEST_SOURCE_ROOT||path.join(__dirname,'..');const src=fs.readFileSync(path.join(root,'src/net/ui_script.h'),'utf8');
function fn(n){const a=src.indexOf('function '+n+'(');return a<0?'':src.slice(a,src.indexOf('\n}',a)+2);}
let checks=0,failures=0;function ck(ok,s){checks++;failures+=!ok;console.log((ok?'PASS ':'FAIL ')+s);}
(async()=>{for(const lang of ['en','ja','fr','ru','es','ar']){
 const map=JSON.parse(fs.readFileSync(path.join(root,'data/i18n/'+lang+'.json'),'utf8'));const nodes={};for(const id of ['sp_updstate','sp_updmsg','sp_updapply','sp_updcard','sp_upddl','sp_updcur','sp_updver'])nodes[id]={textContent:'',style:{},disabled:false};
 let calls=0;const c={I18N:{map},UPD:{phase:'idle',applyArm:false},D:id=>nodes[id]||null,api:()=>{calls++;return Promise.resolve({ok:true,version:'3.2.1'});},setTimeout:()=>0,toast:()=>{}};vm.createContext(c);
 for(const n of ['i18nNorm','i18nGet','t','tf','updSet','updNum','updShowCheck','updApply','updCheck','updDownload','updPollProgress'])vm.runInContext(fn(n),c);
 if(fn('updRenderState'))vm.runInContext(fn('updRenderState'),c);else {const a=src.indexOf("api('/api/update/state').then(function (s) {");const b=src.indexOf('  }).catch',a);vm.runInContext('var updRenderState=function(s){'+src.slice(a+"api('/api/update/state').then(function (s) {".length,b)+'}',c);}
 const detail='private backup D:/fixture/old.dll; remaining=2';const bad={pendingPhase:'rollback_failed',pendingDetail:detail,staged:true,stagedVersion:'3.2.1'};c.updRenderState(bad);const expected=(map['上次回滚未完成，备份已保留：{0}']||'MISSING').replace('{0}',detail);
 ck(nodes.sp_updstate.textContent===expected,lang+' rollback failure stays explicit even with staged update');
 ck(nodes.sp_updstate.style.color==='#e66'&&nodes.sp_updmsg.textContent===expected,lang+' recovery failure is visibly red and preserves raw backup detail');
 ck(nodes.sp_updapply.disabled&&nodes.sp_updapply.style.display==='none',lang+' incomplete rollback cannot offer restart/apply');
 c.UPD.applyArm=true;c.updApply();ck(calls===0&&!c.UPD.applyArm,lang+' direct apply invocation cannot bypass recovery guard');
 const before=nodes.sp_updstate.textContent;c.updShowCheck({ok:true,hasUpdate:true,latest:'3.2.2'});ck(nodes.sp_updstate.textContent===before,lang+' late update check cannot erase recovery diagnostic');
 c.updCheck();c.updDownload();c.updPollProgress();ck(calls===0,lang+' check/download/progress cannot reenable apply during failed recovery');
 c.updRenderState({pendingPhase:'apply_failed',pendingDetail:detail,staged:true,stagedVersion:'3.2.1'});ck(nodes.sp_updstate.textContent===(map['上次替换失败，恢复详情：{0}']||'MISSING').replace('{0}',detail),lang+' apply failure never claims rollback succeeded');
 c.updRenderState({pendingPhase:'ready',pendingVersion:'3.2.1',staged:true,stagedVersion:'3.2.1'});ck(!c.UPD.recoveryBlocked&&!nodes.sp_updapply.disabled&&nodes.sp_updapply.style.display==='',lang+' a fresh ready state still permits legitimate apply');
 c.updRenderState({pendingPhase:'rolled_back',pendingDetail:detail,staged:false});ck(nodes.sp_updstate.textContent===(map['上次更新失败已自动回滚：{0}']||'MISSING').replace('{0}',detail),lang+' successful rollback keeps its distinct result');
 await new Promise(r=>setImmediate(r));
}console.log(JSON.stringify({checks,failures}));process.exitCode=failures?1:0;})();
