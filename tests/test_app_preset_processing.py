"""Actual preset preservation, including a short digital-zero session on the Null sink."""
import tempfile
import wave
import urllib.error
import pathlib,sys,shutil,subprocess,socket,time,urllib.request,urllib.parse,json
root=pathlib.Path(__file__).resolve().parents[1]
runtime_root=pathlib.Path(tempfile.gettempdir())/'rlp-tests';runtime_root.mkdir(parents=True,exist_ok=True)
home=pathlib.Path(tempfile.mkdtemp(prefix='preset-',dir=runtime_root));shutil.copy2(sys.argv[1],home/'rtaweb.exe');(home/'rlplayer-settings.conf').write_text('backend=null\nmetaon=0\nmetallm=0\n',encoding='utf-8');(home/'rlplayer-playlist.txt').write_text('',encoding='utf-8')
with socket.socket() as sk:sk.bind(('127.0.0.1',0));port=sk.getsockname()[1]
base=f'http://127.0.0.1:{port}';log=(home/'stdout.log').open('wb');proc=subprocess.Popen([str(home/'rtaweb.exe'),'--port',str(port)],cwd=home,stdout=log,stderr=log,creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0));checks=0;failures=0
def req(path,data=None):
 with urllib.request.urlopen(urllib.request.Request(base+path,data=None if data is None else urllib.parse.urlencode(data).encode()),timeout=20) as r:return json.load(r)
def check(ok,name):
 global checks,failures
 checks+=1;failures+=not ok;print(('PASS ' if ok else 'FAIL ')+name,flush=True)
def seed():
 req('/api/settings',{'eq':'user-eq.txt','ir':'user-room.wav','trim':'1,2,0,0,0,0,0,0','roomcorrauto':'1','cleanup':'1','upnpUpsample':'1','rgmode':'track'});req('/api/peq',{'bypass':'0','preamp':'3'})
for _ in range(200):
 try:req('/api/status');break
 except Exception:time.sleep(.05)
try:
 seed();answer=req('/api/preset/apply?profile=pcm&preserve_processing=1',{});kv=req('/api/settings')['kv'];check(answer['ok'] and kv.get('eq')=='user-eq.txt' and kv.get('ir')=='user-room.wav' and kv.get('trim')=='1,2,0,0,0,0,0,0','quickstart PCM preserves user EQ, IR and channel trims when explicitly requested');check(not req('/api/peq')['bypass'],'preserve-processing PCM keeps PEQ enabled')
 seed();req('/api/preset/apply?profile=pcm',{});kv=req('/api/settings')['kv'];check(kv.get('eq')=='' and kv.get('ir')=='','legacy preset API retains historical EQ/IR clearing behavior')
 req('/api/settings',{'volume':'-64.5'})
 url='/api/preset/apply?profile=bitperfect&keep_volume=1&require_stopped=1'
 result=req(url,{})['applied']
 check(float(req('/api/settings')['kv']['volume'])==-64.5 and result['volume']==-64.5,'listening preset preserves fractional playback volume on disk and in the result')
 check(not result['bitPerfectOk'],'a retained gain is disclosed as an impediment to bit-perfect playback')
 silence=home/'zero.wav'
 with wave.open(str(silence),'wb') as wav:
  wav.setparams((2,2,44100,0,'NONE',''));wav.writeframes(bytes(44100*4*3))
 started=req('/api/play',{'file':str(silence),'backend':'null','rate':'src','volume':'-64.5','gpu':'0','roomcorrauto':'0','isp':'0'})
 check(started['ok'],'digital silence starts on a private Null sink')
 try:
  for state in ('playing','paused'):
   if state=='paused':
    req('/api/pause',{})
    for _ in range(70):
     if req('/api/status')['paused']:break
     time.sleep(.02)
    check(req('/api/status')['paused'],'server acknowledges pause before testing its guard')
   before=req('/api/settings')['kv']
   try:req(url,{});rejected=False
   except urllib.error.HTTPError as e:rejected=e.code==409 and not json.load(e)['ok']
   check(rejected and before==req('/api/settings')['kv'],'preset is rejected with unchanged settings while '+state)
   before_queue=req('/api/queue')['items']
   try:req('/api/queue/set',{'require_stopped':'1','files':'different.wav'});blocked=False
   except urllib.error.HTTPError as e:blocked=e.code==409 and not json.load(e)['ok']
   check(blocked and before_queue==req('/api/queue')['items'],'playlist import preserves the active queue while '+state)
 finally:req('/api/stop',{})
 imported=req('/api/queue/set',{'require_stopped':'1','files':str(silence)+'\t1\t2\n'+str(silence)})
 q=req('/api/queue')
 check(imported.get('count')==2 and q['cues'][0]==[1,2] and q['items']==[str(silence)]*2,'stopped import preserves order and CUE offsets without starting playback')
 seed();answer=req('/api/preset/apply?profile=bitperfect&preserve_processing=1',{});kv=req('/api/settings')['kv'];check(answer['ok'] and kv.get('eq')=='' and kv.get('ir')=='' and kv.get('roomcorrauto')=='0' and kv.get('cleanup')=='0' and kv.get('upnpUpsample')=='0','bit-perfect always disables automatic correction, cleanup and UPnP upsampling despite preservation flag');check(req('/api/peq')['bypass'],'bit-perfect bypasses persistent PEQ without deleting user bands');check(kv.get('rgmode')=='off','bit-perfect disables ReplayGain processing')
 for key,value in [('cleanup_strength','nan'),('cleanup_strength','1.5'),('cleanup_noise_db','-59'),('cleanup_noise_db','garbage')]:
  rejected=False
  try:req('/api/settings',{key:value})
  except urllib.error.HTTPError as e:rejected=e.code==400 and not json.load(e)['ok']
  check(rejected,'backend rejects invalid '+key+'='+value+' before persistence')
 print(json.dumps({'checks':checks,'failures':failures}),flush=True)
finally:
 try:req('/api/quit',{})
 except Exception:pass
 proc.wait(timeout=20);log.close()
sys.exit(1 if failures else 0)
