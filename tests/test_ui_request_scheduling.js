"use strict";
// Production UI functions + a controlled delayed transport. No sockets,
// executable, user configuration or audio endpoint is opened.
const fs=require('node:fs'),vm=require('node:vm'),path=require('node:path');
const root=process.env.RLPLAYER_TEST_SOURCE_ROOT||path.resolve(__dirname,'..');
const source=fs.readFileSync(path.join(root,'src/net/ui_script.h'),'utf8');
function fn(name){const at=source.search(new RegExp('^function '+name+'\\(', 'm'));if(at<0)throw Error(name);return source.slice(at,source.indexOf('\n}',at)+2);}
const flush=async()=>{for(let i=0;i<10;i++)await Promise.resolve();};
let checks=0,failed=0;function check(ok,label){++checks;if(!ok)++failed;console.log((ok?'PASS ':'FAIL ')+label);}
function transport(){
 const calls=[],rows=[],toasts=[];
 const c={Promise,URLSearchParams,encodeURIComponent,Object,Error,setTimeout:f=>{f();return 1;},
  apiRaw:async u=>{calls.push(u);if(u.startsWith('/api/sq/add?'))rows.push(u);if(c.fail){c.fail=false;throw c.failure||Error('reply lost after append');}return {ok:true,count:rows.length};},
  cueQuery:()=>'',toast:m=>toasts.push(m),tf:(s,a)=>s+' '+a.join(','),renderSavedQueues:()=>{},S:{view:'albums'},rlSettingsFence:{kind:()=>null},fail:true};
 vm.createContext(c);vm.runInContext(fn('api')+'\n'+fn('apiAct')+'\n'+fn('addTrackToPlaylist'),c);
 return {c,calls,rows,toasts};
}
function streamingTransport(name,mode='stall',status=200){
 const timers=new Map(),calls=[];let nextTimer=0;
 const c={Promise,AbortController,Error,Object,JSON,String,PLAYBACK_SETTINGS:{ready:true},playbackErrorText:s=>s,t:s=>s,
  setTimeout:(callback,delay)=>{const id=++nextTimer;timers.set(id,{callback,delay});return id;},clearTimeout:id=>timers.delete(id),
  fetch:async(u,opt)=>{calls.push({u,opt});const body=()=>{
   if(mode==='success')return Promise.resolve({ok:true});
   if(mode==='invalid')return Promise.reject(Error('invalid JSON'));
   return new Promise((resolve,reject)=>{const abort=()=>reject(Object.assign(Error('aborted body'),{name:'AbortError'}));if(opt.signal.aborted)abort();else opt.signal.addEventListener('abort',abort,{once:true});});
  };if(mode==='network')throw Error('offline');return {ok:status>=200&&status<300,status,json:body,text:body};},
  rlSettingsFence:{kind:()=>null},statusRequest:0,statusApplied:0,statusInFlight:null};
 vm.createContext(c);vm.runInContext(fn('apiRaw')+'\n'+fn('apiGetRaw')+'\n'+fn('api')+'\n'+fn('tick'),c);
 const fire=delay=>{for(const [id,timer] of [...timers])if(timer.delay===delay){timers.delete(id);timer.callback();}};
 const observe=p=>{const state={settled:false};p.then(value=>{state.settled=true;state.value=value;},error=>{state.settled=true;state.error=error;});return state;};
 return {c,timers,calls,fire,observe,request:()=>observe(c[name]('/api/status'))};
}
(async()=>{
 let f=transport();await f.c.addTrackToPlaylist('Fixture',{path:'fixture.wav'});
 check(f.rows.length===1,'a lost append reply cannot add the same track twice');
 check(f.toasts.some(s=>s.includes('结果未知')),'lost mutation reply is visible as uncertain, never reported as successful');
 for(const u of ['/api/lib/enqueue?k=album','/api/sq/play?name=fixture','/api/roomcorr/measure?duration=3','/api/future-action']){
  f=transport();let err;try{await f.c.api(u);}catch(e){err=e;}
  check(f.calls.length===1&&err&&err.uncertain,'URL mutation or unknown action never auto-replays: '+u);
 }
 f=transport();await f.c.api('/api/status');check(f.calls.length===2,'reviewed read still retries after transient transport failure');
 f=transport();let explicit;try{await f.c.apiAct('/api/status');}catch(e){explicit=e;}
 check(f.calls.length===1&&explicit.uncertain,'explicit action contract disables retry even without a body');
 f=transport();f.c.failure=Object.assign(Error('denied'),{status:403});try{await f.c.api('/api/status');}catch(e){}
 check(f.calls.length===1,'client errors are never retried');
 f=transport();await f.c.api('/api/future-read',undefined,{idempotent:true});check(f.calls.length===2,'explicitly reviewed idempotent request retains retry support');

 for(const name of ['apiRaw','apiGetRaw']){
  let stream=streamingTransport(name),result=stream.request();await flush();
  check(!result.settled&&stream.timers.size===1,name+': response headers do not end the body timeout');
  stream.fire(8000);await flush();
  check(result.settled&&result.error&&stream.calls[0].opt.signal.aborted,name+': stalled JSON body aborts and settles within the request budget');
  check(stream.timers.size===0,name+': aborted body leaves no live timeout');
  for(const mode of ['success','invalid','network']){
   stream=streamingTransport(name,mode);result=stream.request();await flush();
   check(result.settled&&stream.timers.size===0&&!stream.calls[0].opt.signal.aborted,name+': '+mode+' completion clears the timeout');
  }
 }
 let stream=streamingTransport('apiRaw','stall',503),result=stream.request();await flush();
 check(!result.settled&&stream.timers.size===1,'HTTP error body remains covered by its timeout');
 stream.fire(8000);await flush();
 check(result.settled&&result.error&&result.error.status===503&&stream.timers.size===0,'stalled HTTP error body settles with its status after abort');
 stream=streamingTransport('apiGetRaw','stall',403);result=stream.request();await flush();
 check(result.settled&&result.error.status===403&&stream.timers.size===0,'GET HTTP refusal clears its timeout without reading a body');
 stream=streamingTransport('apiRaw');result=stream.observe(stream.c.tick());for(let i=0;i<20;i++)stream.c.tick();await flush();
 stream.fire(8000);await flush();stream.fire(200);await flush();
 check(stream.calls.length===2,'stalled status body allows exactly one reviewed read retry');
 stream.fire(8000);await flush();
 check(result.settled&&stream.c.statusInFlight===null,'two stalled status bodies release the shared poll request');
 stream.c.tick();await flush();
 check(stream.calls.length===3,'status polling resumes after response-body timeouts');

 let pending=[],heads=[],errors=[],nodes={q:{value:''}};
 let c={Promise,encodeURIComponent,api:u=>new Promise((resolve,reject)=>pending.push({u,resolve,reject})),S:{view:'albums'},D:id=>nodes[id]||null,head:(...v)=>heads.push(v),tf:(s,a)=>s+' '+a.join(','),renderTracksView:()=>{},renderPanelError:()=>errors.push(1),trackListRequest:0};
 vm.createContext(c);vm.runInContext(fn('loadTracks'),c);
 c.loadTracks('old');c.loadTracks('new');pending[1].resolve({tracks:[{title:'new'}]});await flush();pending[0].resolve({tracks:[{title:'old'}]});await flush();
 check(c.S.currentTracks[0].title==='new'&&c.S.tracksQuery==='new','late search response cannot overwrite current search results');
 c.loadTracks('failed-old');c.loadTracks('working-new');pending[3].resolve({tracks:[{title:'working-new'}]});pending[2].reject(Error('late'));await flush();
 check(!errors.length&&c.S.currentTracks[0].title==='working-new','stale search failure cannot replace newer content with an error panel');
 c.loadTracks('leaving');c.S.view='settings';pending[4].resolve({tracks:[{title:'wrong'}]});await flush();check(c.S.currentTracks[0].title==='working-new','search response cannot replace another page');

 pending=[];heads=[];errors=[];nodes={list:{innerHTML:'CURRENT_SETTINGS_VIEW'}};
 c={Promise,api:u=>new Promise((resolve,reject)=>pending.push({u,resolve,reject})),S:{view:'albums'},D:id=>nodes[id]||null,head:(...v)=>heads.push(v),tf:(s,a)=>s+' '+a.join(','),artaWarmBackground:()=>{},renderPanelError:()=>errors.push(1),artistListRequest:0};
 vm.createContext(c);vm.runInContext(fn('loadArtists'),c);c.loadArtists('old');c.S.view='settings';pending[0].resolve({albums:[]});await flush();
 check(nodes.list.innerHTML==='CURRENT_SETTINGS_VIEW','artist response cannot overwrite settings after navigation');
 c.loadArtists('old-search');c.loadArtists('new-search');pending[2].resolve({albums:[]});await flush();const latestHead=JSON.stringify(heads.at(-1));pending[1].resolve({albums:[]});await flush();
 check(JSON.stringify(heads.at(-1))===latestHead&&latestHead.includes('new-search'),'late artist search cannot restore its old heading or filter');
 c.loadArtists('failed-old');c.loadArtists('active');pending[3].reject(Error('late'));await flush();check(!errors.length,'stale artist failure cannot replace active results');

 pending=[];c={api:u=>new Promise((resolve,reject)=>pending.push({u,resolve,reject})),statusRequest:0,statusApplied:0,statusInFlight:null};vm.createContext(c);vm.runInContext(fn('tick'),c);
 const first=c.tick();for(let i=1;i<20;i++)c.tick();
 check(pending.length===1,'20 poll ticks during a stalled reply share one request');
 check(c.tick()===first,'concurrent status callers share the same completion');
 pending[0].resolve({ok:false});await flush();c.tick();check(pending.length===2,'polling resumes after a rejected status payload');
 pending[1].reject(Error('offline'));await flush();c.tick();check(pending.length===3,'polling resumes after a network failure');
 pending[2].resolve({ok:false});await flush();
 console.log(JSON.stringify({checks,failed,actualAudioFrames:0}));process.exitCode=failed?1:0;
})().catch(e=>{console.error(e);process.exitCode=2;});
