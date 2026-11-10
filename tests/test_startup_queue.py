"""Private Null-only queue/session regressions. Never exercise unsafe legacy valid-queue startup skip."""
from __future__ import annotations
import argparse,json,os,pathlib,shutil,socket,subprocess,tempfile,time,urllib.parse,urllib.request,urllib.error,wave,threading,http.server,html,re,hashlib

def main():
 p=argparse.ArgumentParser();p.add_argument('--exe',type=pathlib.Path,required=True);p.add_argument('--evidence-dir',type=pathlib.Path);a=p.parse_args();root=(a.evidence_dir or pathlib.Path(tempfile.mkdtemp(prefix='rlplayer-startup-queue-'))).resolve();root.mkdir(parents=True,exist_ok=True)
 if (root/'rtaweb.exe').exists():raise RuntimeError('evidence directory must be fresh')
 shutil.copy2(a.exe.resolve(),root/'rtaweb.exe');media=root/'media';media.mkdir();profile=root/'profile';profile.mkdir();rows=[]
 for name,seconds in [('one',4),('two',4),('short',.7)]:
  with wave.open(str(media/(name+'.wav')),'wb') as f:f.setparams((2,2,48000,int(seconds*48000),'NONE','not compressed'));f.writeframes(bytes(int(seconds*48000)*4))
 (root/'rlplayer-settings.conf').write_text('backend=null\nvolume=-60\nrate=src\nisp=0\ngpuvendor=off\nroomcorrauto=0\nmetaon=0\nmetallm=0\nanlmaster=0\ncleanup=0\n')
 (root/'rlplayer-library.conf').write_text(str(media)+'\n');(root/'rlplayer-llm.conf').write_text('on=0\n');(root/'rlplayer-playlist.txt').write_text('# RLPlayer playlist v1\nindex=0\nrepeat=0\nshuffle=0\n')
 with socket.socket() as s:s.bind(('127.0.0.1',0));port=s.getsockname()[1]
 base='http://127.0.0.1:'+str(port)
 def req(route,data=None):
  body=None if data is None else urllib.parse.urlencode(data).encode();request=urllib.request.Request(base+route,data=body,headers={'Origin':base})
  with urllib.request.urlopen(request,timeout=20) as f:return json.load(f)
 def check(ok,label,detail=None):
  row={'pass':bool(ok),'label':label,'detail':detail};rows.append(row);(root/'results.json').write_text(json.dumps(rows,indent=2,ensure_ascii=False),encoding='utf8');print(json.dumps(row,ensure_ascii=False),flush=True)
 def wait(predicate):
  deadline=time.monotonic()+12
  while True:
   st=req('/api/status')
   if predicate(st):return st
   if time.monotonic()>deadline:raise AssertionError('private Null status timeout '+json.dumps(st))
   time.sleep(.01)
 def queue(names):return req('/api/queue/set',{'files':'\n'.join(str(media/(name+'.wav')) for name in names)})
 def play(name,db):return req('/api/play',{'file':str(media/(name+'.wav')),'backend':'null','rate':'src','volume':str(db),'bits':'24','dsd':'0','isp':'0','gpuvendor':'off','roomcorrauto':'0','cleanup':'0','eq':'','ir':'','loudness':'0'})
 env={**os.environ,'APPDATA':str(profile),'LOCALAPPDATA':str(profile),'USERPROFILE':str(profile),'TEMP':str(profile),'TMP':str(profile),'RLPLAYER_LIBRARY_DIR':str(root),'RTA_NO_DEVICE':'1','RTA_NET_TESTS':'0','RLPLAYER_UPDATE_ALLOW_LOCAL':'1','RLPLAYER_UPDATE_MANIFEST':'http://127.0.0.1:1/api/v1/update/windows/latest'}
 # This renderer is entirely loopback HTTP; it never opens an audio endpoint.
 records=[];receiver={'uri':'','started':0.0};expected_wavs={hashlib.sha256(x.read_bytes()).hexdigest() for x in media.glob('*.wav')}
 class Renderer(http.server.BaseHTTPRequestHandler):
  def log_message(self,*args):pass
  def answer(self,text):
   data=text.encode();self.send_response(200);self.send_header('Content-Type','text/xml');self.send_header('Content-Length',str(len(data)));self.end_headers();self.wfile.write(data)
  def do_GET(self):self.answer('<root><device><friendlyName>Queue volume loopback only</friendlyName><serviceList><service><serviceType>urn:schemas-upnp-org:service:AVTransport:1</serviceType><controlURL>/av</controlURL></service><service><serviceType>urn:schemas-upnp-org:service:ConnectionManager:1</serviceType><controlURL>/cm</controlURL></service></serviceList></device></root>')
  def do_POST(self):
   body=self.rfile.read(int(self.headers.get('Content-Length','0'))).decode();action=self.headers.get('SOAPAction','').split('#')[-1].strip('\"');payload=''
   if action=='GetProtocolInfo':payload='<Source></Source><Sink>http-get:*:audio/wav;rate=48000;bits=16;channels=2:*</Sink>'
   elif action=='SetAVTransportURI':receiver['uri']=html.unescape(re.search(r'<CurrentURI>(.*?)</CurrentURI>',body,re.S)[1])
   elif action=='Play':
    uri=receiver['uri'];assert urllib.parse.urlsplit(uri).hostname=='127.0.0.1','test renderer must never fetch a LAN/external address'
    with urllib.request.urlopen(uri,timeout=15) as response:data=response.read()
    records.append({'bytes':len(data),'sha256':hashlib.sha256(data).hexdigest(),'exact_private_wav':hashlib.sha256(data).hexdigest() in expected_wavs});receiver['started']=time.monotonic()
   elif action=='GetTransportInfo':payload='<CurrentTransportState>'+('PLAYING' if time.monotonic()-receiver['started']<1 else 'STOPPED')+'</CurrentTransportState><CurrentTransportStatus>OK</CurrentTransportStatus><CurrentSpeed>1</CurrentSpeed>'
   elif action=='GetPositionInfo':payload='<Track>1</Track><TrackDuration>00:00:04</TrackDuration><RelTime>00:00:04</RelTime>'
   elif action=='GetMediaInfo':payload='<NrTracks>1</NrTracks><MediaDuration>00:00:04</MediaDuration>'
   self.answer('<s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/"><s:Body><u:'+action+'Response xmlns:u="urn:schemas-upnp-org:service:AVTransport:1">'+payload+'</u:'+action+'Response></s:Body></s:Envelope>')
 renderer=http.server.ThreadingHTTPServer(('127.0.0.1',0),Renderer);serverthread=threading.Thread(target=renderer.serve_forever,daemon=True);serverthread.start();dev='http://127.0.0.1:'+str(renderer.server_port)+'/description.xml'
 with (root/'backend.log').open('wb') as log:
  child=subprocess.Popen([str(root/'rtaweb.exe'),'--port',str(port)],cwd=root,env=env,stdout=log,stderr=log,creationflags=subprocess.CREATE_NO_WINDOW)
  try:
   deadline=time.monotonic()+25
   while True:
    try:req('/api/ping');break
    except OSError:
     if child.poll() is not None or time.monotonic()>deadline:raise RuntimeError('private backend failed to listen')
     time.sleep(.02)
   empty=req('/api/skip?dir=1',{});before=req('/api/queue');st=req('/api/status')
   guarded=empty.get('ok') is False and empty.get('needsInitialPlay') is True
   check(guarded and not st.get('playing') and st.get('pos',0)==0,'empty startup skip has explicit needsInitialPlay guard, no frames',empty)
   if guarded:
    # Even a partial future regression must not open a device: these first
    # uninitialized queue entries do not exist and fail before sink creation.
    # No valid WAV is ever started before explicit backend=null initialization.
    queue(['missing-one','missing-two']);before=req('/api/queue');disk=(root/'rlplayer-playlist.txt').read_bytes();answers=[req('/api/skip?dir='+str(d),{}) for d in (1,-1)];after=req('/api/queue');st=req('/api/status')
    check(all(x.get('ok') is False and x.get('needsInitialPlay') is True for x in answers) and before==after and disk==(root/'rlplayer-playlist.txt').read_bytes() and not st.get('playing') and st.get('pos',0)==0,'nonempty restored queue startup skips preserve queue/index/file and render zero frames',answers)
   else:print('SAFETY: legacy valid-queue startup skip NOT executed (would select default audio endpoint)',flush=True)
   req('/api/play',{'backend':'null','volume':'-60'});invalid=req('/api/skip?dir=1',{}) if guarded else None
   if guarded:check(invalid.get('needsInitialPlay') is True,'rejected explicit play does not initialize session',invalid)
   queue(['one','two']);check(play('one',-60).get('ok'),'explicit Null/-60 first play accepted');wait(lambda s:s.get('current')==str(media/'one.wav') and s.get('pos',0)>.03)
   req('/api/volume?db=-80',{});skip=req('/api/skip?dir=1',{});st=wait(lambda s:s.get('current')==str(media/'two.wav') and s.get('pos',0)>.03)
   check(skip.get('ok') and st.get('volDb')==-80 and st.get('startDb')==-80,'manual skip retains live -80 dB and new pipeline baseline',{'volDb':st.get('volDb'),'startDb':st.get('startDb'),'info':st.get('info')});req('/api/stop',{})
   queue(['short','two']);play('short',-60);wait(lambda s:s.get('current')==str(media/'short.wav') and s.get('pos',0)>.03);req('/api/volume?db=-80',{});st=wait(lambda s:s.get('current')==str(media/'two.wav') and s.get('pos',0)>.03)
   check(st.get('volDb')==-80 and st.get('startDb')==-80,'natural next track retains live -80 dB and new pipeline baseline',{'volDb':st.get('volDb'),'startDb':st.get('startDb')})
   queue(['one','two']);play('one',-70);st=wait(lambda s:s.get('current')==str(media/'one.wav') and s.get('pos',0)>.03)
   check(st.get('volDb')==-70 and st.get('startDb')==-70,'new explicit play -70 overrides previous session -80',{'volDb':st.get('volDb'),'startDb':st.get('startDb')})
   req('/api/stop',{});req('/api/volume?db=-90',{});skip=req('/api/skip?dir=1',{});st=wait(lambda s:s.get('current')==str(media/'two.wav') and s.get('pos',0)>.03)
   check(skip.get('ok') and st.get('volDb')==-90 and st.get('startDb')==-90,'initialized stopped skip retains most recent live -90',{'volDb':st.get('volDb'),'startDb':st.get('startDb')})
   req('/api/stop',{})
   for route in ('/api/volume','/api/volume?db=','/api/volume?db=nan','/api/volume?db=inf','/api/volume?db=-inf','/api/volume?db=-60junk'):
    req('/api/volume?db=-90',{});code=200
    try:answer=req(route,{})
    except urllib.error.HTTPError as error:code=error.code;answer=json.load(error)
    st=req('/api/status');check(code==400 and answer.get('ok') is False and st.get('volDb')==-90,'invalid runtime volume rejected without state change: '+route,{'http':code,'answer':answer,'volDb':st.get('volDb')})
   for db in (-120,0):
    answer=req('/api/volume?db='+str(db),{});st=req('/api/status');check(answer.get('ok') and st.get('volDb')==db,'explicit runtime volume boundary remains legal: '+str(db),st.get('volDb'))
   if guarded:
    baseconf=(root/'rlplayer-settings.conf').read_text()
    for saved in (None,'nan','inf','-inf','-60junk','1','-121',''):
     (root/'rlplayer-settings.conf').write_text(baseconf.replace('volume=-60\n','') if saved is None else baseconf.replace('volume=-60','volume='+saved))
     body={'file':str(media/'one.wav'),'backend':'null','rate':'src','bits':'24','dsd':'0','isp':'0','gpuvendor':'off','roomcorrauto':'0','cleanup':'0'}
     req('/api/play',body);st=wait(lambda s:s.get('current')==str(media/'one.wav') and s.get('pos',0)>.03)
     check(st.get('volDb')==-60 and st.get('startDb')==-60,'invalid persisted volume safely defaults to -60: '+repr(saved),{'volDb':st.get('volDb'),'startDb':st.get('startDb')});req('/api/stop',{})
    (root/'rlplayer-settings.conf').write_text(baseconf)
   req('/api/lib/scan',{});deadline=time.monotonic()+15
   while req('/api/lib/status').get('scanning'):
    if time.monotonic()>deadline:raise AssertionError('private synthetic library scan timed out')
    time.sleep(.02)
   albums=req('/api/lib/albums').get('albums',[])
   if not albums:raise AssertionError('private synthesized album missing')
   req('/api/lib/play',{'k':albums[0]['k'],'i':'0','backend':'null','volume':'-75','rate':'src','bits':'24','dsd':'0','isp':'0','gpuvendor':'off','roomcorrauto':'0','cleanup':'0'});st=wait(lambda st:st.get('playing') and st.get('pos',0)>.03)
   check(st.get('volDb')==-75 and st.get('startDb')==-75,'explicit album play seeds its own -75 session volume',{'volDb':st.get('volDb'),'startDb':st.get('startDb')})
   req('/api/volume?db=-85',{});q=req('/api/queue');target=q['items'][1];req('/api/skip?dir=1',{});st=wait(lambda st:st.get('current')==target and st.get('pos',0)>.03)
   check(st.get('volDb')==-85 and st.get('startDb')==-85,'album session skip retains live -85 baseline',{'volDb':st.get('volDb'),'startDb':st.get('startDb')});req('/api/stop',{})
   # UPnP checks control-state continuity. Its existing byte-exact source path
   # does not apply PCM volume, so these checks do not claim renderer attenuation.
   def upnpplay(db):return req('/api/play',{'file':str(media/'one.wav'),'backend':'upnp','upnpdev':dev,'volume':str(db),'rate':'src','dsd':'0','isp':'0','gpuvendor':'off','roomcorrauto':'0','cleanup':'0'})
   queue(['one','two']);count=len(records);answer=upnpplay(-60);st=wait(lambda st:st.get('current')==str(media/'one.wav') and len(records)>count)
   check(answer.get('ok') and st.get('volDb')==-60,'explicit UPnP session seeds requested -60 rather than old atomic state',{'volDb':st.get('volDb'),'fetches':len(records)})
   req('/api/volume?db=-80',{});count=len(records);answer=req('/api/skip?dir=1',{});st=wait(lambda st:st.get('current')==str(media/'two.wav') and len(records)>count)
   check(answer.get('ok') and st.get('volDb')==-80,'UPnP manual skip preserves live control volume -80',{'volDb':st.get('volDb'),'fetches':len(records)})
   req('/api/stop',{});queue(['one','two']);count=len(records);upnpplay(-65);wait(lambda st:st.get('current')==str(media/'one.wav') and len(records)>count);req('/api/volume?db=-80',{});count=len(records)
   st=wait(lambda st:st.get('current')==str(media/'two.wav') and len(records)>count)
   check(st.get('volDb')==-80,'UPnP natural queue advance preserves live control volume -80',{'volDb':st.get('volDb'),'fetches':len(records)})
   check(len(records)>=4 and all(row['exact_private_wav'] for row in records),'loopback renderer fetched exact private WAVs without PCM volume conversion',records)
   req('/api/stop',{})
  finally:
   if child.poll() is None:
    req('/api/stop',{});req('/api/quit',{});child.wait(timeout=20)
   renderer.shutdown();renderer.server_close();serverthread.join(timeout=5)
 summary={'pass':all(r['pass'] for r in rows),'checks':len(rows),'failures':sum(not r['pass'] for r in rows),'audio_backend':'Null and loopback HTTP mock only; dangerous legacy startup case omitted','rows':rows};(root/'summary.json').write_text(json.dumps(summary,indent=2),encoding='utf8');print(json.dumps(summary),flush=True);return 0 if summary['pass'] else 1
if __name__=='__main__':raise SystemExit(main())
