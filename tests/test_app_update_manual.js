const fs=require('node:fs'),path=require('node:path'),vm=require('node:vm');
const root=process.env.RLPLAYER_TEST_SOURCE_ROOT||path.join(__dirname,'..');
const source=fs.readFileSync(path.join(root,'src/net/ui_script.h'),'utf8');
function fn(name){const a=source.indexOf('function '+name+'(');if(a<0)throw Error('missing '+name);return source.slice(a,source.indexOf('\n}',a)+2);}
let checks=0,failures=0;function ck(ok,label){checks++;if(!ok){failures++;console.log('FAIL '+label);}else console.log('PASS '+label);}
(async()=>{for(const lang of ['en','ja','fr','ru','es','ar']){
 const map=JSON.parse(fs.readFileSync(path.join(root,'data/i18n/'+lang+'.json'),'utf8'));
 const nodes={};for(const id of ['sp_updcur','sp_updstate','sp_updcard','sp_updver','sp_updplan','sp_updfiles','sp_updsize','sp_updnotes','sp_upddl','sp_updapply','sp_updcn','sp_updglobal','sp_updskip','sp_updmsg','sp_updbar'])nodes[id]={textContent:'',style:{},disabled:false};
 const c={I18N:{map},UPD:{recoveryBlocked:false},D:id=>nodes[id]||null,t:s=>map[s]||s,tf:(s,a)=>(map[s]||s).replace(/\{(\d+)\}/g,(_,i)=>a[i])};vm.createContext(c);
 for(const n of ['updNum','updSet','updWhy','updShowCheck','updPollProgress'])vm.runInContext(fn(n),c);
 c.updShowCheck({ok:true,hasUpdate:true,latest:'4.0.0',installMode:'manual',reason:'major-upgrade',cn:'https://pan.baidu.com/s/fixture',global:'https://github.com/okhuoyanyan/RLPlayer2/releases/v4.0.0',extractionCode:'test',fullSize:409276280});
 ck(nodes.sp_updplan.textContent.includes(map['跨大版本更新需要重新安装完整版。']),lang+' major upgrade is manual');
 ck(nodes.sp_updcn.style.display===''&&nodes.sp_updglobal.style.display==='',lang+' both regional manual links are visible');
 ck(nodes.sp_upddl.style.display==='none'&&nodes.sp_updapply.style.display==='none',lang+' manual update cannot start automatic download or apply');
 ck(nodes.sp_updfiles.textContent.includes('test'),lang+' extraction code is shown');
 ck(c.UPD.manualCn.startsWith('https://pan.baidu.com/')&&c.UPD.manualGlobal.startsWith('https://github.com/'),lang+' manual destinations are preserved');
 c.updShowCheck({ok:true,hasUpdate:true,latest:'3.3.0',installMode:'manual',reason:'webview2-update',cn:'https://pan.baidu.com/s/fixture',global:'https://github.com/fixture'});
 ck(nodes.sp_updplan.textContent.includes(map['本次包含 WebView2 更新，需要重新安装完整版。']),lang+' WebView2 update is manual');
 c.updShowCheck({ok:true,hasUpdate:true,latest:'3.2.3',installMode:'incremental',filesToDownload:1,filesUnchanged:8,downloadText:'1 MB',skippedText:'2 MB',files:[{path:'rtaweb.exe',size:100}]});
 ck(nodes.sp_upddl.style.display===''&&!nodes.sp_upddl.disabled&&nodes.sp_updcn.style.display==='none'&&nodes.sp_updglobal.style.display==='none',lang+' ordinary update remains incremental');
 c.updShowCheck({ok:false,error:'quota_exceeded；请到百度网盘或 GitHub 下载完整版'});
 ck(nodes.sp_updcard.style.display===''&&nodes.sp_updstate.textContent.includes('quota_exceeded'),lang+' quota failure stays explicit');
 ck(nodes.sp_updcn.style.display===''&&nodes.sp_updglobal.style.display===''&&nodes.sp_upddl.style.display==='none',lang+' quota exhaustion offers manual external downloads only');
 c.api=()=>Promise.resolve({phase:'error',error:'quota_exceeded'});c.updPollProgress();await new Promise(r=>setImmediate(r));
 ck(nodes.sp_updmsg.textContent.includes(map[' 请改用百度网盘或 GitHub 手动下载完整版，或下月重试。']),lang+' download quota error also offers manual guidance');
}
console.log(JSON.stringify({checks,failures}));process.exitCode=failures?1:0;})().catch(e=>{console.error(e);process.exitCode=1;});
