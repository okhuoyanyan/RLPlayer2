const { spawn } = require('child_process');
const fs=require('fs'), path=require('path'), os=require('os');
const URL_=process.argv[2];
const EDGE=['C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\msedge.exe','C:\\Program Files\\Microsoft\\Edge\\Application\\msedge.exe'].find(p=>fs.existsSync(p));
const PORT=9460+Math.floor(Math.random()*60);
const udd=path.join(os.tmpdir(),'edge_play_'+Date.now());
const child=spawn(EDGE,['--headless=new','--disable-gpu','--no-first-run','--user-data-dir='+udd,'--remote-debugging-port='+PORT,'--window-size=1400,900','about:blank'],{stdio:'ignore'});
const sleep=ms=>new Promise(r=>setTimeout(r,ms));
let pass=0, fail=0;
const ck=(ok,what,detail)=>{ console.log((ok?'  [PASS] ':'  [FAIL] ')+what+(detail?'  '+detail:'')); ok?pass++:fail++; };
(async()=>{
  let page=null;
  for(let i=0;i<60&&!page;i++){try{const j=await(await fetch('http://127.0.0.1:'+PORT+'/json/list')).json();page=j.filter(t=>t.type==='page')[0];}catch(e){}if(!page)await sleep(250);}
  const ws=new WebSocket(page.webSocketDebuggerUrl);await new Promise(r=>{ws.onopen=r;});
  let id=0;const pend=new Map();
  ws.onmessage=ev=>{const m=JSON.parse(ev.data);if(m.id&&pend.has(m.id)){pend.get(m.id)(m.result);pend.delete(m.id);}};
  const send=(m2,p)=>new Promise(res=>{const i=++id;pend.set(i,res);ws.send(JSON.stringify({id:i,method:m2,params:p||{}}));});
  const ev=async e=>{const r=await send('Runtime.evaluate',{expression:e,awaitPromise:true,returnByValue:true}); if(r.exceptionDetails) throw new Error(JSON.stringify(r.exceptionDetails.text)); return r.result&&r.result.value;};
  const api=async(u,body)=>{ const e = body===undefined
      ? "fetch('"+u+"').then(r=>r.json())"
      : "fetch('"+u+"',{method:'POST',body:'"+body+"'}).then(r=>r.json())";
    return await ev(e); };
  await send('Page.enable');await send('Runtime.enable');
  await send('Page.navigate',{url:URL_});await sleep(2500);

  // 1) 用 Null 后端（不出声），入队一首测试曲
  await api('/api/settings','backend=null');
  await api('/api/clear','');
  await api('/api/add','file=' + encodeURIComponent('D:\\RTA-hqplayer\\build\\testdata\\t1.wav'));
  await ev("fetch('/api/settings').then(r=>r.json()).then(j=>{var b=document.getElementById('sp_backend'); if(b){b.value='null';b.dispatchEvent(new Event('change'));}})");
  await sleep(400);

  // 2) 设定采样率 96000，点播放
  await ev("document.getElementById('sRate').value='96000'");
  await ev("document.getElementById('bPause').click()");
  await sleep(1200);
  let st = await api('/api/status');
  ck(!!st.playing, '点播放后开始播放', 'playing='+st.playing);
  ck(Number(st.outRate)===96000, '采样率 96000 生效', 'outRate='+st.outRate);

  // 3) 停止
  await ev("document.getElementById('bStop').click()");
  await sleep(900);
  st = await api('/api/status');
  ck(!st.playing, '停止后不再播放', 'playing='+st.playing);

  // 4) 改采样率为 48000，直接再点播放（不换歌）
  await ev("document.getElementById('sRate').value='48000'");
  await ev("document.getElementById('bPause').click()");
  await sleep(1400);
  st = await api('/api/status');
  ck(!!st.playing, '停止后点播放能重新起播', 'playing='+st.playing);
  ck(Number(st.outRate)===48000, '新采样率在下一次播放生效', 'outRate='+st.outRate);

  // 5) 还原
  await ev("document.getElementById('bStop').click()");
  await api('/api/settings','backend=' + encodeURIComponent(await ev("(function(){var e=document.getElementById('sRate'); return 'wasapi';})()")));
  await api('/api/clear','');
  console.log('');
  console.log(pass + ' passed, ' + fail + ' failed');
  ws.close();child.kill();process.exit(fail?1:0);
})().catch(e=>{console.error('ERR '+e.message);process.exit(2);});