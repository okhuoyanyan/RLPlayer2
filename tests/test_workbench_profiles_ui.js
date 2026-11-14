"use strict";
// Execute the production state model against a persistent, isolated HTTP contract.
// No audio device or RLPlayer executable is started by this test.
const fs=require('node:fs'),path=require('node:path'),vm=require('node:vm'),assert=require('node:assert/strict');
const root=process.env.RLPLAYER_TEST_SOURCE_ROOT||path.resolve(__dirname,'..');
const script=fs.readFileSync(path.join(root,'src/net/ui_workbench_state.h'),'utf8').match(/return R"WORKBENCH\(([\s\S]*?)\)WORKBENCH";/)[1];
let checks=0;function check(label,fn){fn();checks++;console.log('PASS '+label);}const copy=v=>JSON.parse(JSON.stringify(v));
function fixture(server,legacy){
 const initial={filter:'poly-sinc-short-lp',filterNx:'',rate:'88200',rateFamilyPolicy:'prefer',dsd:'0',dsdnative:'0',twostage:'0',modorder:'5',modfamily:'dsd'};
 const peq={bypass:1,preamp:0,bands:Array.from({length:20},(_,i)=>({i:i+1,on:true,type:'PK',freq:20*Math.pow(1000,i/19),gain:0,q:1.41}))};
 server=server||{active:'A',slots:['A','B','C','D'].map(id=>({id,name:id,settings:copy(initial),peq:copy(peq),eqId:'',eqName:'',migrated:false})),loop:{enabled:false,start:0,end:0,file:''},state:{ok:true,settings:{...initial,backend:'null',device:'Fixture Null',volume:'-64.5'},peq,file:'Q:/Fixture.wav',position:12.5,duration:40,cueStart:0,cueEnd:40,queueIndex:0,revision:'1',playing:true,paused:false}};
 const requests=[],writes=[],store=new Map();if(legacy)store.set('rlplayer-comparison-v1',JSON.stringify(legacy));
 const env={Promise,URLSearchParams,Date,console,window:{},localStorage:{getItem:k=>store.get(k)||null,setItem:()=>{throw Error('legacy write forbidden');}},t:x=>x,ready:true,ensurePlaybackSettings:()=>env.ready,_capsCache:null,loadSettings:()=>Promise.resolve({ok:true}),failApply:false,
 apiGetRaw:async url=>{requests.push(url);if(env.failRead)throw Error('read unavailable');if(url==='/api/workbench/profiles')return copy({ok:true,active:server.active,slots:server.slots,loop:server.loop});return copy(server.state);},
 apiAct:async(url,body)=>{const p=Object.fromEntries(new URLSearchParams(body));writes.push({url,...p});if(env.failApply)return {ok:false,error:'Unsupported format'};
  const active=()=>server.slots.find(s=>s.id===server.active),safe=()=>Object.fromEntries(Object.entries(server.state.settings).filter(([k])=>!['backend','device','volume'].includes(k))),snapshot=()=>Object.assign(active(),{settings:safe(),peq:copy(server.state.peq),migrated:true});
  if(url==='/api/workbench/profile'){
   const target=server.slots.find(s=>s.id===p.id);if(!target)return {ok:false,error:'unknown slot'};
   if(p.action==='rename')target.name=p.name;
   if(p.action==='migrate'&&!target.migrated){target.settings=Object.fromEntries(Object.entries(p).filter(([k])=>!['action','id','name','peq'].includes(k)));target.name=p.name;if(p.peq)target.peq=JSON.parse(p.peq);target.migrated=true;}
   if(p.action==='select'){snapshot();server.active=p.id;server.state.settings={...server.state.settings,...copy(target.settings)};server.state.peq=copy(target.peq);}
   if(p.action==='update'){if(p.id!==server.active)return {ok:false,error:'active changed'};Object.assign(server.state.settings,Object.fromEntries(Object.entries(p).filter(([k])=>!['action','id','peq'].includes(k))));if(p.peq)server.state.peq=JSON.parse(p.peq);snapshot();}
  }else if(url==='/api/workbench/loop'){if(p.file!==server.state.file)return {ok:false,error:'track changed'};server.loop={...p,start:Number(p.start),end:Number(p.end),enabled:p.enabled==='1'};
  }else{if(p.expected_file!==server.state.file||p.expected_revision!==String(server.state.revision))return {ok:false,error:'stale'};Object.assign(server.state.settings,Object.fromEntries(Object.entries(p).filter(([k])=>!['expected_file','expected_revision','peq','position'].includes(k))));if(p.peq)server.state.peq=JSON.parse(p.peq);if(p.position)server.state.position=Number(p.position);snapshot();}
  server.state.revision=String(Number(server.state.revision)+1);return {ok:true};}};
 vm.createContext(env);vm.runInContext(script,env);return {env,server,writes,requests,store};
}
(async()=>{
 let f=fixture(),ui=f.env.rlConsole,c=f.env.rlCompare;
 check('boot is read only and performs no device or configuration action',()=>{assert.equal(f.writes.length,0);assert.equal(f.requests.length,0);assert.equal(c.slots.length,4);});
 await ui.refresh();await c.refresh();
 check('four real independent server groups initialize without a manual save',()=>{assert.equal(c.activeId,'A');assert.equal(c.slots.length,4);assert(c.slots.every(s=>s.settings));assert.equal(f.writes.length,0);});
 check('comparison snapshots exclude global volume, backend and device',()=>{const settings=c.selectedSettings(ui.state.settings);for(const k of ['volume','backend','device'])assert(!Object.hasOwn(settings,k));});
 for(const [i,id] of ['A','B','C','D'].entries()){await c.load(id);await c.update({filter:'filter-'+id,rate:String(88200*(i+1)),taps:String(i*1024)},id);}
 check('A through D retain four distinct automatically saved parameter sets',()=>{assert.deepEqual(Array.from(c.slots,s=>s.settings.filter),['filter-A','filter-B','filter-C','filter-D']);});
 await c.load('A');check('letter selection applies the corresponding saved parameters',()=>{assert.equal(c.activeId,'A');assert.equal(ui.state.settings.filter,'filter-A');assert.equal(ui.state.settings.rate,'88200');});
 await c.move(1);check('next group switches to B',()=>{assert.equal(c.activeId,'B');assert.equal(ui.state.settings.filter,'filter-B');});
 await c.move(-1);check('previous group returns to A',()=>assert.equal(c.activeId,'A'));
 const before=copy(f.server);f.env.failApply=true;await assert.rejects(c.load('C'),/Unsupported format/);
 check('failed selection retains active slot, real parameters and clears pending flag',()=>{assert.equal(c.activeId,'A');assert.equal(ui.state.settings.filter,'filter-A');assert.equal(c.saving,false);assert.equal(c.error,'Unsupported format');assert.deepEqual(f.server,before);});
 f.env.failApply=false;await c.load('D');await c.rename('D','<img src=x onerror=alert(1)>');
 check('names remain inert data and preserve all parameters',()=>{assert.equal(c.slots[3].name,'<img src=x onerror=alert(1)>');assert.equal(c.slots[3].settings.filter,'filter-D');});
 const restarted=fixture(f.server);await restarted.env.rlCompare.refresh();await restarted.env.rlConsole.refresh();
 check('groups and active choice survive a fresh runtime through server persistence',()=>{assert.equal(restarted.env.rlCompare.activeId,'D');assert.equal(restarted.env.rlCompare.slots[0].settings.filter,'filter-A');assert.equal(restarted.env.rlConsole.state.settings.filter,'filter-D');});
 const n=f.writes.length;await assert.rejects(c.update({rate:'176400'},'A'),/当前组已切换/);
 check('a delayed edit cannot write to a newer active group',()=>assert.equal(f.writes.length,n));
 await c.setLoop({enabled:1,file:f.server.state.file,start:4.25,end:8.5,repeat:1});
 check('fragment stores exact second boundaries and repetition policy',()=>{assert.equal(c.loop.start,4.25);assert.equal(c.loop.end,8.5);assert.equal(c.loop.repeat,'1');});
 f.env.ready=false;await assert.rejects(ui.apply({rate:176400}),/读取播放设置/);
 check('unrestored settings cannot issue any audio mutation',()=>assert.equal(f.writes.length,n+1));
 f.env.ready=true;ui.busy=true;await assert.rejects(ui.apply({rate:176400}),/正在切换/);ui.busy=false;
 check('concurrent switch is refused before mutation',()=>assert.equal(f.writes.length,n+1));
 const old={version:1,activeId:'B',slots:[{id:'A',name:'Legacy A',settings:{filter:'old-A',rate:'src'},peq:{bypass:1,preamp:0,bands:[]}},{id:'B',name:'Legacy B',settings:{filter:'old-B',rate:'176400'},savedAt:5}]};
 f=fixture(undefined,old);await f.env.rlCompare.refresh();
 check('old 335 slots migrate with exact names and requested active group',()=>{assert.equal(f.env.rlCompare.activeId,'B');assert.equal(f.env.rlCompare.slots[0].name,'Legacy A');assert.equal(f.env.rlCompare.slots[1].settings.filter,'old-B');});
 check('migration keeps original local storage evidence unchanged',()=>assert.equal(f.store.get('rlplayer-comparison-v1'),JSON.stringify(old)));
 const mutations=f.writes.length;await f.env.rlCompare.refresh();check('migration is idempotent during repeated polling',()=>assert.equal(f.writes.length,mutations));
 const fresh=fixture(f.server,old);await fresh.env.rlCompare.refresh();check('migrated server flags prevent replay after restart',()=>assert.equal(fresh.writes.length,0));
 f=fixture(undefined,{version:1,slots:[{id:'A',name:'unsafe',settings:{volume:'0',filter:'wrong'}}]});await f.env.rlCompare.refresh();check('legacy global device or volume fields reject the whole unsafe slot',()=>assert.equal(f.writes.length,0));
 f=fixture();await f.env.rlConsole.refresh();const read=f.env.apiGetRaw;f.env.apiGetRaw=async url=>{f.server.state.queueIndex=1;f.server.state.cueStart=20;f.server.state.cueEnd=40;return read(url);};await assert.rejects(f.env.rlConsole.apply({filter:'x'}),/曲目已切换/);
 check('same-file CUE identity change cannot rebind an older parameter click',()=>assert.equal(f.writes.length,0));
 f=fixture();await f.env.rlConsole.refresh();const queue=f.env.rlWorkbenchQueue;let order=[];await Promise.all([queue.run(async()=>{order.push('A');throw Error('fixture');}).catch(()=>{}),queue.run(async()=>{order.push('B');})]);
 check('a failed queued action does not replay or block a later action',()=>{assert.deepEqual(order,['A','B']);assert.equal(queue.pending,0);});
 f=fixture();await f.env.rlConsole.refresh();let releaseState;const getState=f.env.apiGetRaw,oldState=copy(f.server.state);f.env.apiGetRaw=url=>{if(url==='/api/workbench/state'&&!releaseState)return new Promise(resolve=>{releaseState=()=>resolve(oldState);});return getState(url);};const olderStateRead=f.env.rlConsole.refresh();f.server.state.settings.filter='new-authoritative';await f.env.rlConsole.refresh(true);releaseState();await olderStateRead;
 check('a late pre-mutation state response cannot overwrite the newer authoritative read',()=>assert.equal(f.env.rlConsole.state.settings.filter,'new-authoritative'));
 f=fixture();await f.env.rlCompare.refresh();let releaseProfiles;const getProfiles=f.env.apiGetRaw,oldProfiles=copy({ok:true,active:f.server.active,slots:f.server.slots,loop:f.server.loop});f.env.apiGetRaw=url=>{if(url==='/api/workbench/profiles'&&!releaseProfiles)return new Promise(resolve=>{releaseProfiles=()=>resolve(oldProfiles);});return getProfiles(url);};const olderProfileRead=f.env.rlCompare.refresh();f.server.active='C';await f.env.rlCompare.refresh(true);releaseProfiles();await olderProfileRead;
 check('late profile polling cannot restore a previous active group after a fresh read',()=>assert.equal(f.env.rlCompare.activeId,'C'));
 console.log(JSON.stringify({checks,failures:0,actualAudioFrames:0}));
})().catch(e=>{console.error(e);process.exitCode=1;});
