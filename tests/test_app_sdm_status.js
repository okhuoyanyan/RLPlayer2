// Actual signal-chain renderer with requested/effective/mock direct states.
const fs=require('node:fs'),vm=require('node:vm'),path=require('node:path');
const root=path.join(__dirname,'..'),source=fs.readFileSync(path.join(root,'src/net/ui_script.h'),'utf8');
function extract(name){const a=source.indexOf('function '+name+'(');if(a<0)throw Error('Missing production signal dependency: '+name);return source.slice(a,source.indexOf('\n}',a)+2);}
let checks=0,failed=0;function check(ok,label){checks++;if(!ok)failed++;console.log((ok?'PASS ':'FAIL ')+label);}
async function render(lang,p){const dict=JSON.parse(fs.readFileSync(path.join(root,'data/i18n/'+lang+'.json'),'utf8'));
 const nodes={mtitle:{},modal:{classList:{add:()=>{}}},mbody:{}};const c={D:id=>nodes[id],S:{},ADV:{},
 api:async()=>({ok:true,path:Object.assign({inRate:44100,inBits:16,inCh:2,outRate:176400,outBits:24,outCh:2,volume:-60,buffer:100,src:'offline.wav',dsd:64,dsdRate:2822400},p)}),
 esc:s=>String(s),nam:s=>s,chMark:()=>false,chLayout:()=>'',t:s=>dict[s]||s,tf:(s,args)=>(dict[s]||s).replace(/\{(\d+)\}/g,(_,i)=>args[i])};
 vm.createContext(c);
 const cleanupState=source.match(/^var CLEANUP_UI = .*;$/m);if(!cleanupState)throw Error('Missing production cleanup observer state');vm.runInContext(cleanupState[0],c);
 // The settings panel is absent here. The real observer still runs, and its
 // production painter naturally returns when sp_cleanup_status does not exist.
 for(const n of ['cleanupStatusText','paintCleanupStatus','observeCleanupReport','volText','sdmOrderText','showChain'])vm.runInContext(extract(n),c);
 c.showChain();await new Promise(r=>setImmediate(r));const html=nodes.mbody.innerHTML;
 if(typeof html!=='string'||html.includes('<div class=empty>读取失败</div>'))throw Error(lang+' signal renderer failed: '+html);
 return {html,dict};}
(async()=>{for(const lang of ['en','ja','fr','ru','es','ar']){
 let r=await render(lang,{modOrderRequested:7,modOrder:5});
 const fmt=(key,args)=>(r.dict[key]||key).replace(/\{(\d+)\}/g,(_,i)=>args[i]);
 check(r.html.includes(fmt('请求 {0} 阶 · 实际 {1} 阶 SDM',[7,5])),lang+' guarded effective five is shown beside requested seven');
 r=await render(lang,{modOrderRequested:7,modOrder:0});check(r.html.includes(fmt('请求 {0} 阶 · 实际阶数待确认',[7])),lang+' unobserved effective order is not guessed as five or seven');
 r=await render(lang,{modOrderRequested:0,modOrder:0,directSdm:true});check(r.html.includes(r.dict['未调制（1-bit 直通）']||'未调制（1-bit 直通）'),lang+' DirectSDM is not falsely labeled as a fifth order modulator');
 r=await render(lang,{modOrderRequested:5,modOrder:5,modFamily:'ahm'});check(r.html.includes('1-bit')&&!r.html.includes('6~8 bit'),lang+' AHM signal output remains explicitly one bit');
 r=await render(lang,{modOrderRequested:7,modOrder:5,outRate:2822400,carrier:176400});
 check(r.html.includes('176400 Hz / 24 bit')&&!r.html.includes('2822400 Hz / 24 bit'),lang+' output endpoint rate is the DoP carrier, not the internal one-bit rate');
}console.log(JSON.stringify({checks,failed}));process.exitCode=failed?1:0;})();
