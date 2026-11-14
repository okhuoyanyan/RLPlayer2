const fs=require('node:fs'),path=require('node:path'),vm=require('node:vm');
const root=process.env.RLPLAYER_TEST_SOURCE_ROOT||path.join(__dirname,'..');const source=fs.readFileSync(path.join(root,'src/net/ui_script.h'),'utf8');
function fn(n){const a=source.indexOf('function '+n+'(');return a<0?'':source.slice(a,source.indexOf('\n}',a)+2);}
let checks=0,failures=0;function ck(v,s){checks++;failures+=!v;console.log((v?'PASS ':'FAIL ')+s);}
for(const lang of ['en','ja','fr','ru','es','ar']){
 const map=JSON.parse(fs.readFileSync(path.join(root,'data/i18n/'+lang+'.json'),'utf8'));let open=true;
 const nodes={nps:{textContent:'old'},npart:{textContent:'Private artist'},npSub:{textContent:'old stopped caption'},npPage:{classList:{contains:()=>open}}};
 const c={I18N:{map},D:id=>nodes[id]||null,chSuffix:()=>''};vm.createContext(c);for(const n of ['i18nNorm','i18nGet','t','tf','playbackErrorText'])vm.runInContext(fn(n),c);
 if(!c.playbackErrorText)c.playbackErrorText=s=>s;
 if(fn('syncPlaybackCaption'))vm.runInContext(fn('syncPlaybackCaption'),c);else c.syncPlaybackCaption=j=>{nodes.nps.textContent=j.playing?'播放中':'已停止';};
 const message=c.playbackErrorText('Manual FIR is invalid; playback was not started.');
 c.syncPlaybackCaption({error:message,playing:false,outRate:0},2);
 ck(nodes.nps.textContent===message,lang+' actual localized error outranks generic stopped caption');
 ck(nodes.npSub.textContent==='Private artist  ·  '+message,lang+' already-open track page reflects asynchronous failure immediately');
 c.syncPlaybackCaption({error:'Device/path unchanged',playing:false},2);ck(nodes.npSub.textContent.includes('Device/path unchanged'),lang+' raw diagnostic remains verbatim');
 c.syncPlaybackCaption({error:'',playing:true,outRate:48000,outBits:24},2);ck(nodes.nps.textContent.includes('48000 Hz / 24 bit')&&!nodes.npSub.textContent.includes(message),lang+' recovered playback clears prior error and keeps format');
 open=false;nodes.npSub.textContent='closed';c.syncPlaybackCaption({error:'new error'},2);ck(nodes.npSub.textContent==='closed',lang+' closed track page is not rewritten');
}
ck(source.includes('syncPlaybackCaption(j, srcCh);'),'production tick invokes the error-priority caption renderer');
console.log(JSON.stringify({checks,failures}));process.exitCode=failures?1:0;
