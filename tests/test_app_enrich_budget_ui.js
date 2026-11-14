// Exact structured deadline result through the production API adapter.
const fs=require('node:fs'),vm=require('node:vm'),path=require('node:path');
const root=path.join(__dirname,'..'),source=fs.readFileSync(path.join(root,'src/net/ui_script.h'),'utf8');
const a=source.indexOf('function apiRaw('),fn=source.slice(a,source.indexOf('\n}',a)+2);
let checks=0,failed=0;function check(ok,label){checks++;if(!ok)failed++;console.log((ok?'PASS ':'FAIL ')+label);}
(async()=>{for(const lang of ['en','ja','fr','ru','es','ar']){
 const dict=JSON.parse(fs.readFileSync(path.join(root,'data/i18n/'+lang+'.json'),'utf8'));let reply,delay;
 const c={AbortController,setTimeout:(f,ms)=>{delay=ms;return 1;},clearTimeout:()=>{},t:s=>dict[s]||s,fetch:async()=>({ok:true,json:async()=>reply})};vm.createContext(c);vm.runInContext(fn,c);
 for(const ok of [false,true]){reply={ok,budgetExceeded:true,errorCode:'ENRICH_BUDGET_EXCEEDED',fields:ok?['title']:[],note:'raw diagnostic'};const result=await c.apiRaw('/api/enrich?file=fixture');
 check(result.message===dict['元数据网络整理已达到时间预算，可重试；已有结果不会丢失'],lang+' structured deadline result localizes');
 check(result.ok===ok&&result.note==='raw diagnostic'&&result.fields.length===(ok?1:0),lang+' partial accepted result and diagnostic are preserved');
 check(delay===22000,lang+' frontend waits for server budget result instead of aborting at 8s');}
 reply={ok:false,errorCode:'DEVICE_FAILURE',message:'D:/device/raw'};const result=await c.apiRaw('/api/status');
 check(result.message==='D:/device/raw'&&delay===8000,lang+' unrelated diagnostics and ordinary request timeout stay unchanged');
}console.log(JSON.stringify({checks,failed}));process.exitCode=failed?1:0;})();
