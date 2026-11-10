// Production local-album orchestration and markup, with delayed offline HTTP.
const fs=require('node:fs'),vm=require('node:vm'),path=require('node:path');
const source=fs.readFileSync(path.join(process.env.RLPLAYER_TEST_SOURCE_ROOT||path.join(__dirname,'..'),'src/net/ui_script.h'),'utf8');
function extract(name){const a=source.indexOf('function '+name+'(');if(a<0)throw Error(name);return source.slice(a,source.indexOf('\n}',a)+2);}
let checks=0,failed=0;function check(ok,label){checks++;if(!ok)failed++;console.log((ok?'PASS ':'FAIL ')+label);}
const settle=()=>new Promise(r=>setImmediate(r));
function setup(){const pending=[],urls=[];const c={NP_DATA:{tabs:[{k:'release',name:'Offline Album',artist:'Offline Artist'}],cache:{},inflight:{},req:1,live:{v:1}},S:{curMeta:{}},META:{hasLlmKey:false,summaryEnabled:false},NPLIB:{key:'',album:null},
 NP_AI_ORDER:['track','release','person','work'],aiSummaryOn:()=>false,npDataVisible:()=>true,npDataRenderTabs:()=>{},npDataRenderBody:()=>{},npDrawerOpen:()=>{},
 api:url=>{urls.push(url);return new Promise((resolve,reject)=>pending.push({resolve,reject}));},
 renderFieldGrid:()=>'',renderSummaryContent:()=>{throw Error('AI renderer must not run while disabled');},
 fmtDurLong:n=>'seconds:'+n,esc:s=>String(s).replaceAll('&','&amp;').replaceAll('<','&lt;').replaceAll('>','&gt;').replaceAll('"','&quot;')};
 vm.createContext(c);for(const n of ['npDataCan','npDataTab','npAiKinds','npLocalAlbum','npLocalKeyOf','npReleaseOpts','npReleaseBody','efTracklistHtml','npLocalAlbumLoad','npDataEnsure','npDataActivate','npDataActivateAll'])vm.runInContext(extract(n),c);
 return {c,pending,urls};}
(async()=>{
 {const {c,pending,urls}=setup();c.npDataActivate('release');check(pending.length===1&&urls[0].startsWith('/api/lib/albums?'),'AI off still starts local album lookup');
 pending.shift().resolve({albums:[{name:'Offline Album',artist:'Offline Artist',k:'private-key'}]});await settle();
 check(urls[1]==='/api/lib/album?k=private-key','matched local album loads real track detail');
 pending.shift().resolve({ok:true,tracks:[{n:1,title:'First <local>',dur:12},{n:2,title:'Second',dur:34}],artist:'Offline Artist'});await settle();
 const st=c.NP_DATA.cache.release,h=c.npReleaseBody(st);
 check(st.state==='local'&&st.localState==='ok','AI off reaches local ready state');
 check(h.includes('First &lt;local&gt;')&&h.includes('Second')&&(h.match(/class=eft-row/g)||[]).length===2,'local tracks render once and escape markup');
 check(h.includes('seconds:12')&&h.includes('seconds:34'),'track durations come from local detail');
 check(!h.includes('整理中')&&!h.includes('读取本地'),'ready local list does not claim indefinite AI/loading');
 check(urls.length===2&&!urls.some(u=>u.includes('/summary')),'no AI HTTP requests occur');
 c.npDataActivate('release');check(urls.length===2,'ready local result is reused without redundant fetch');}
 {const {c,pending,urls}=setup();c.npDataActivate('release');pending.shift().resolve({albums:[]});await settle();
 check(c.NP_DATA.cache.release.localState==='none','missing local album leaves loading state');
 c.npDataActivate('release');check(urls.length===2,'reopening retries previous miss so newly scanned albums can appear');
 if(pending.length){pending.shift().resolve({albums:[{name:'Offline Album',artist:'Offline Artist',k:'new'}]});await settle();pending.shift().resolve({ok:true,tracks:[{title:'After scan',dur:1}]});await settle();}
 check(c.npReleaseBody(c.NP_DATA.cache.release).includes('After scan'),'previous miss can recover without reloading application');}
 {const {c,pending,urls}=setup();c.npDataActivate('release');pending.shift().reject(Error('offline transient'));await settle();
 c.npDataActivate('release');check(urls.length===2,'network failure permits retry');}
 {const {c,pending}=setup();c.npDataActivate('release');const old=pending.shift();c.NP_DATA.cache.release={localKey:'new-selection',localState:'loading'};
 old.resolve({albums:[{name:'Offline Album',artist:'Offline Artist',k:'old'}]});await settle();pending.shift().resolve({ok:true,tracks:[{title:'Old result'}]});await settle();
 check(!c.NP_DATA.cache.release.local,'late album detail does not replace another selection');}
 {const {c,pending,urls}=setup();c.META.hasLlmKey=true;c.S.curMeta.summaryCached=true;
 c.NP_DATA.tabs.push({k:'track',name:'Offline Song'},{k:'person',name:'Offline Artist'});c.npDataActivateAll();
 check(urls.length===1&&urls[0].startsWith('/api/lib/albums?'),'current merged drawer also fetches local list while AI switch is off, even with key/cache availability flags');
 pending.shift().resolve({albums:[{name:'Offline Album',artist:'Offline Artist',k:'merged'}]});await settle();pending.shift().resolve({ok:true,tracks:[{title:'Merged local row',dur:8}]});await settle();
 check(c.npReleaseBody(c.NP_DATA.cache.release).includes('Merged local row')&&!urls.some(u=>u.includes('/summary')),'merged drawer displays local rows without any AI request');}
 console.log(JSON.stringify({checks,failed}));process.exitCode=failed?1:0;
})().catch(e=>{console.error(e);process.exitCode=1;});
