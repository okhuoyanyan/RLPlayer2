"""Opt-in real backend product policy: generated PCM/DSF, explicit null output only."""
import pathlib,sys,urllib.request,urllib.parse,urllib.error,json,time,wave,struct
port=int(sys.argv[1]);home=pathlib.Path(sys.argv[2]).resolve();base=pathlib.Path('D:/RLPlayer-Develop-20261103').resolve()
assert 19000<=port<20000 and home.is_relative_to(base) and home.name.startswith('runtime-app-')
u=f'http://127.0.0.1:{port}';checks=0;records=[]
def req(path,params=None):
 with urllib.request.urlopen(urllib.request.Request(u+path,data=None if params is None else urllib.parse.urlencode(params).encode()),timeout=10) as r:return json.load(r)
def ck(v,s):
 global checks
 checks+=1;print(('PASS ' if v else 'FAIL ')+s,flush=True)
 if not v:raise AssertionError(s)
def wait(file,fail):
 end=time.monotonic()+30
 while time.monotonic()<end:
  st=req('/api/status')
  if st.get('current')==str(file) and not st['playing'] and (bool(st.get('error')) if fail else st.get('dur',0)>0 and st.get('pos',0)>=st['dur']*.9):return st
  time.sleep(.02)
 raise AssertionError('state not settled')
ck(req('/api/settings')['kv'].get('backend')=='null','private runtime is configured for null output')
pcm=home/'corner-zero.wav'
with wave.open(str(pcm),'wb') as w:w.setnchannels(2);w.setsampwidth(2);w.setframerate(44100);w.writeframes(bytes(2205*4))
count=141120;block=4096;payload=bytes([0x69])*(block*2*((count//8+block-1)//block));size=28+52+12+len(payload)
dsf=home/'corner-idle.dsf';dsf.write_bytes(b'DSD '+struct.pack('<QQQ',28,size,0)+b'fmt '+struct.pack('<QIIIIIIQII',52,1,0,2,2,2822400,1,count,block,0)+b'data'+struct.pack('<Q',12+len(payload))+payload)
log=home/'rlplayer-playback.log'
for file,target in [(pcm,64),(dsf,128)]:
 for corner,family,rejected in [(20,'dsd',True),(50,'dsd',True),(99.9,'dsd',True),(100,'dsd',False),(0,'dsd',False),(20,'ec',False)]:
  req('/api/clear',{});req('/api/repeat?rate=0',{})
  before=log.stat().st_size if log.exists() else 0
  params={'file':str(file),'backend':'null','volume':'-60','dsd':str(target),'modfamily':family,'modorder':'5','modcut':str(corner),'filter':'short-lp','gpu':'off','gpuvendor':'off','autoanl':'0','anlmaster':'0','metallm':'0','metaon':'0','roomcorrauto':'0','ir':'','directsdm':'0','shaper':'none'}
  ck(req('/api/play',params)['ok'],'explicit null request accepted for asynchronous validation')
  st=wait(file,rejected);tail=log.read_bytes()[before:].decode('utf-8',errors='replace')
  if rejected:
   ck('SDM_CORNER_QUALITY:' in st.get('error',''),'known-distorting corner has exact refusal code '+file.suffix+' '+str(corner))
   ck('PLAY v' not in tail and 'SINK_OPEN' not in tail and 'RENDER_DONE' not in tail,'refused corner never reaches PLAY/sink/render '+file.suffix+' '+str(corner))
  else:
   ck(not st.get('error') and 'RENDER_DONE' in tail,'default/supported/EC configuration completes actual null processing '+file.suffix+' '+str(corner)+' '+family)
   signal=req('/api/signal');ck(signal['path'].get('deviceId')=='null' and signal['path'].get('dsd')==target,'actual product signal confirms explicit null DoP target')
  records.append({'file':file.name,'corner_khz':corner,'family':family,'rejected':rejected,'status':st,'log_tail':tail})
# The UI restores settings then sends its explicit play parameters. An omitted
# API modcut means default, so it is not a valid probe of a restored UI profile.
req('/api/settings',{'modcut':'20','modfamily':'dsd'})
restored=req('/api/settings')['kv'];ck(restored.get('modcut')=='20','legacy profile preserves saved low value rather than silently replacing it')
req('/api/clear',{});req('/api/play',{'file':str(pcm),'backend':'null','volume':'-60','dsd':'64','modcut':restored['modcut'],'modfamily':restored['modfamily'],'filter':'short-lp','roomcorrauto':'0','metaon':'0','anlmaster':'0','gpu':'off','gpuvendor':'off'})
legacy=wait(pcm,True);ck('SDM_CORNER_QUALITY:' in legacy['error'],'restored low legacy profile is refused using actual explicit UI play contract')
req('/api/settings',{'modcut':'0','modfamily':'dsd'})
(base/'evidence/app/sdm-corner-live-results.json').write_text(json.dumps({'checks':checks,'records':records,'legacy':legacy,'scope':'Generated zero PCM and balanced idle DSF; null sink only. This is product admission/recovery, not repair of the original 20/50 kHz NTF.'},ensure_ascii=False,indent=2),encoding='utf-8')
print(json.dumps({'checks':checks,'failures':0}),flush=True)
