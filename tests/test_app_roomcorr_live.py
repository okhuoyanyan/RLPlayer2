"""Opt-in isolated actual backend simulation+auto-application integration.

PORT RUNTIME_DIR, only private runtime-app-* under the development workspace.
Every measurement has src=null; every playback has backend=null. No devices.
"""
import pathlib,sys,urllib.request,urllib.parse,urllib.error,json,time,wave,hashlib,struct
port=int(sys.argv[1]);home=pathlib.Path(sys.argv[2]).resolve();base=pathlib.Path('D:/RLPlayer-Develop-20261103').resolve()
assert 19000<=port<20000 and home.is_relative_to(base) and home.name.startswith('runtime-app-')
url=f'http://127.0.0.1:{port}';checks=0;records=[]
def req(path,data=None):
 try:
  with urllib.request.urlopen(urllib.request.Request(url+path,data=None if data is None else urllib.parse.urlencode(data).encode()),timeout=10) as r:return r.status,json.load(r)
 except urllib.error.HTTPError as r:return r.code,json.load(r)
def check(ok,name):
 global checks
 checks+=1;print(('PASS ' if ok else 'FAIL ')+name,flush=True)
 if not ok:raise AssertionError(name)
def until(path,predicate,timeout=15):
 t=time.monotonic()+timeout
 while time.monotonic()<t:
  _,v=req(path)
  if predicate(v):return v
  time.sleep(.025)
 raise AssertionError('status failed to settle')
_,settings=req('/api/settings');check(settings['kv'].get('backend')=='null' and settings['kv'].get('metaon')=='0','private settings select null device and disable metadata')
_,initial=req('/api/status');check(not initial['playing'],'private backend initially idle')
params={'confirm':'1','src':'null','layout':'2.0','fs':'8000','seconds':'.5','levelDb':'-60','target':'flat'}
bad=dict(params);bad.pop('confirm');code,result=req('/api/roomcorr/measure',bad);check(code==400 and not result['ok'],'missing confirmation cannot create measurement worker')
code,started=req('/api/roomcorr/measure',params);check(code==200 and started['ok'] and started['simulated'],'actual measurement endpoint starts explicit no-device simulator')
code,busy=req('/api/roomcorr/measure',params);check(code==400 and not busy['ok'],'parallel measurement start is rejected rather than queued')
done=until('/api/roomcorr/status',lambda j:not j.get('running'));records.append(done)
check(done.get('stepName')=='done' and not done.get('lastError'),'simulated session completes and reports no error')
correction=home/'data/roomcorr/correction-2.0.json';check(correction.exists(),'actual worker writes correction into isolated exe data directory')
original=correction.read_bytes();j=json.loads(original);check(j.get('layout')=='2.0' and len(j.get('channels',[]))==2,'generated correction has full two-channel layout')
code,download=req('/api/roomcorr/download?layout=2.0');check(code==200 and download['channels']==j['channels'],'download returns actual persisted correction')
wav=home/'generated-silent.wav'
with wave.open(str(wav),'wb') as f:f.setnchannels(2);f.setsampwidth(2);f.setframerate(8000);f.writeframes(bytes(8000*4))
def play(extra):
 req('/api/clear',{});req('/api/repeat?rate=0',{})
 code,reply=req('/api/play',{'file':str(wav),'backend':'null','volume':'-60','dsd':'0','rate':'src','filter':'auto','gpu':'off','gpuvendor':'off','autoanl':'0','anlmaster':'0','metaon':'0','metallm':'0','roomcorrauto':'1','ir':'',**extra})
 check(code==200 and reply['ok'],'explicit null playback request is accepted')
 st=until('/api/status',lambda x:not x.get('playing') and x.get('current')==str(wav) and (x.get('error') or x.get('pos',0)>.9))
 check(not st.get('error'),'null playback completes without device or DSP error')
 _,signal=req('/api/signal');check(signal['path']['deviceId']=='null','actual signal confirms null output sink');records.append(signal);return signal['path']
p=play({});check(p['roomCorr']['applied'] and p['roomCorr']['layout']=='2.0' and p['conv'],'matching generated correction actually enters application convolution path')
p=play({'roomcorrauto':'0'});check(not p['roomCorr']['applied'] and not p['conv'],'automatic correction setting off prevents convolution')
def float_ir(path,values):
 data=struct.pack('<'+'f'*len(values),*values);fmt=struct.pack('<HHIIHH',3,1,8000,32000,4,32)
 path.write_bytes(b'RIFF'+struct.pack('<I',36+len(data))+b'WAVEfmt '+struct.pack('<I',16)+fmt+b'data'+struct.pack('<I',len(data))+data)
manual=home/'manual-good.wav';float_ir(manual,[.5,0,0,0]);p=play({'ir':str(manual)})
check(p['roomCorr']['manual'] and not p['roomCorr']['applied'] and p['conv'],'valid manual mono IR takes priority and actually enters convolution')
invalid=home/'manual-nan.wav';float_ir(invalid,[float('nan'),0,0,0]);req('/api/clear',{})
req('/api/play',{'file':str(wav),'backend':'null','volume':'-60','dsd':'0','rate':'src','gpu':'off','gpuvendor':'off','metaon':'0','anlmaster':'0','ir':str(invalid),'roomcorrauto':'1'})
bad=until('/api/status',lambda x:not x.get('playing') and bool(x.get('error')))
check(bad['error']=='Manual FIR is invalid; playback was not started.','invalid float WAV IR fails explicitly before rendering')
records.append({'manual_invalid_status':bad})
p=play({});check(p['roomCorr']['applied'] and p['conv'],'next valid automatic correction recovers after manual IR failure')
code,started=req('/api/roomcorr/measure',params);check(started['simulated'],'cancel test also uses no-device simulator');req('/api/roomcorr/cancel',{})
cancelled=until('/api/roomcorr/status',lambda x:not x.get('running'));records.append(cancelled)
check(cancelled.get('stepName')=='cancelled','actual cancel endpoint reaches cancelled state')
check(correction.read_bytes()==original,'cancelled measurement preserves previous complete correction bytes')
check(req('/api/status')[1]['ok'],'backend remains responsive after measurement cancellation')
out=base/'evidence/app/roomcorr-live-results.json';out.write_text(json.dumps({'checks':checks,'records':records,'fixture_correction_sha256':hashlib.sha256(original).hexdigest(),'scope':'src=null only; backend=null only; generated private files; no microphone/DAC/physical acoustic verification'},ensure_ascii=False,indent=2),encoding='utf-8')
print(json.dumps({'checks':checks,'failures':0}))
