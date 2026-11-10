const fs=require('node:fs'),path=require('node:path'),vm=require('node:vm');
const root=process.env.RLPLAYER_TEST_SOURCE_ROOT||path.join(__dirname,'..');
const source=fs.readFileSync(path.join(root,'src/net/ui_script.h'),'utf8');
function fn(n){const a=source.indexOf('function '+n+'(');return source.slice(a,source.indexOf('\n}',a)+2);}
let checks=0,failures=0;
function check(ok,label){++checks;if(!ok)++failures;console.log((ok?'PASS ':'FAIL ')+label);}
(async()=>{for(const lang of ['en','ja','fr','ru','es','ar']){
 const map=JSON.parse(fs.readFileSync(path.join(root,'data/i18n/'+lang+'.json'),'utf8'));
 const search={clientWidth:900,placeholder:''};let measured='';
 const sc={D:()=>search,t:s=>map[s]||s,window:{innerWidth:1300},getComputedStyle:()=>({paddingLeft:'0',paddingRight:'0'}),phTextWidth:(_q,s)=>{measured=s;return 500;}};
 vm.createContext(sc);vm.runInContext(fn('syncSearchPh'),sc);sc.syncSearchPh();
 check(search.placeholder===map['搜索专辑 / 艺术家 / 曲目…']&&measured===search.placeholder,lang+' resize measures and renders localized full search prompt');
 search.clientWidth=100;sc.syncSearchPh();check(search.placeholder===map['搜索…'],lang+' narrow resize immediately renders localized short prompt');
 search.clientWidth=900;sc.syncSearchPh();check(search.placeholder===map['搜索专辑 / 艺术家 / 曲目…'],lang+' resize back restores localized full prompt');
 const elems={sp_indev:{innerHTML:''},rcnt:{},qlist:{innerHTML:''},sp_autoanl:{},sp_anlmsg:{}};
 const c={I18N:{map},S:{},ADV:{},ANL:{auto:0},D:id=>elems[id]||null,
  esc:s=>s,updateNextTrackInfo:()=>{},PM_PENDING:0,saveSettings:()=>{},
  api:u=>Promise.resolve(u==='/api/queue'?{items:[]}:{devices:[{id:'dev',name:'Test Audio Device',isDefault:true}]}),
  anlSetAuto:on=>c.ANL.auto=on?1:0,anlHideCard:()=>{},anlPaint:()=>{}};
 vm.createContext(c);for(const n of ['i18nNorm','i18nGet','t','tf','loadInputDevices','loadQ'])vm.runInContext(fn(n),c);
 if(source.includes('function analysisAutoStatus('))vm.runInContext(fn('analysisAutoStatus'),c);
 await c.loadInputDevices();const suffix=map['（默认）'];
 check(elems.sp_indev.innerHTML.includes('Test Audio Device'+suffix),lang+' device name stays verbatim and default suffix is translated');
 c.loadQ();await new Promise(r=>setImmediate(r));
 check(elems.qlist.innerHTML.includes(map['右键专辑或曲目可加入']||'__MISSING__'),lang+' empty queue tip is translated at render time');
 const start=source.indexOf("  if (D('sp_autoanl')) D('sp_autoanl').onchange = function () {");
 vm.runInContext(source.slice(start,source.indexOf('\n  };',start)+5),c);
 elems.sp_autoanl.onchange.call({checked:false});
 check(elems.sp_anlmsg.textContent.includes(map['关']||'__MISSING__'),lang+' auto-analysis off state parameter is translated');
 if(lang!=='ja')check(!/[\u3400-\u9fff]/.test(elems.sp_anlmsg.textContent),lang+' dynamic analysis status has no Chinese UI text');
 const disabled=map['模块已关闭：单曲页不显示分析卡，也不会再请求分析数据'];
 c.ANL.master=0;c.ANL.auto=0;
 check(c.analysisAutoStatus()===disabled,lang+' initial disabled module describes no cards or analysis requests');
 elems.sp_autoanl.onchange.call({checked:true});
 check(elems.sp_anlmsg.textContent===disabled,lang+' auto toggle cannot claim analysis is shown while master is off');
 vm.runInContext(fn('anlSetMaster'),c);c.anlSetMaster(true);
 check(elems.sp_anlmsg.textContent===c.analysisAutoStatus(),lang+' enabling master uses the same localized status as initial render');
 c.anlSetMaster(false);
 check(elems.sp_anlmsg.textContent===disabled,lang+' disabling master immediately uses the localized disabled explanation');
}console.log(JSON.stringify({checks,failures}));process.exitCode=failures?1:0;})();
