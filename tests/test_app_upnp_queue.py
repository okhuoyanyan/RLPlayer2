import tempfile
"""Actual rtaweb + loopback DLNA renderer, no audio devices or real music."""
import pathlib,subprocess,sys,threading,http.server,urllib.request,urllib.parse,html,re,json,time,wave,struct,shutil,socket
root=pathlib.Path(__file__).resolve().parents[1];evidence=root/'build-round-evidence/upnp';evidence.mkdir(parents=True,exist_ok=True)
runtime_root=pathlib.Path(tempfile.gettempdir())/'rlp-tests';runtime_root.mkdir(parents=True,exist_ok=True)
home=pathlib.Path(tempfile.mkdtemp(prefix='upnp-',dir=runtime_root));shutil.copy2(sys.argv[1],home/'rtaweb.exe')
(home/'rlplayer-settings.conf').write_text('backend=upnp\nmetaon=0\nmetallm=0\nanlmaster=0\nrate=src\ngpuvendor=off\nroomcorrauto=0\n',encoding='utf-8')
(home/'rlplayer-playlist.txt').write_text('RTAQUEUE1\nindex=0\nrepeat=0\nshuffle=0\n',encoding='utf-8')
for i in [1,2]:
 with wave.open(str(home/f'测试 & {i}.wav'),'wb') as f:f.setnchannels(2);f.setsampwidth(2);f.setframerate(44100);f.writeframes(struct.pack('<hh',i,-i)*4410)
records=[];lock=threading.Lock();session={'uri':'','since':0.,'count':0,'fault':False};checks=0
class Handler(http.server.BaseHTTPRequestHandler):
 def log_message(self,*args):pass
 def answer(self,text,status=200):
  data=text.encode();self.send_response(status);self.send_header('Content-Length',str(len(data)));self.send_header('Content-Type','text/xml');self.end_headers();self.wfile.write(data)
 def do_GET(self):self.answer('<root><device><friendlyName>Offline Loopback Renderer</friendlyName><modelName>Test</modelName><serviceList><service><serviceType>urn:schemas-upnp-org:service:AVTransport:1</serviceType><controlURL>/av</controlURL></service><service><serviceType>urn:schemas-upnp-org:service:ConnectionManager:1</serviceType><controlURL>/cm</controlURL></service></serviceList></device></root>')
 def do_POST(self):
  body=self.rfile.read(int(self.headers.get('Content-Length','0'))).decode();action=self.headers.get('SOAPAction','').split('#')[-1].strip('"');payload=''
  if action=='GetProtocolInfo':payload='<Source></Source><Sink>http-get:*:audio/wav;rate=44100;channels=2:*,http-get:*:audio/wav;rate=176400;channels=2:*,http-get:*:audio/wav;rate=192000;channels=2:*</Sink>'
  elif action=='SetAVTransportURI':
   session['uri']=html.unescape(re.search(r'<CurrentURI>(.*?)</CurrentURI>',body,re.S).group(1));session['since']=time.monotonic();session['count']+=1
   title=html.unescape(body);records.append({'uri':session['uri'],'title':title,'time':time.monotonic()})
  elif action=='Play':
   uri=session['uri'];session['since']=time.monotonic()
   def fetch():
    try:
     with urllib.request.urlopen(uri,timeout=10) as r:b=r.read()
     records[-1]['bytes']=len(b);records[-1]['rate']=struct.unpack_from('<I',b,24)[0];records[-1]['wav']=b.hex()[:120]
    except Exception as e:records[-1]['error']=str(e)
   
   if not session['fault']:threading.Thread(target=fetch,daemon=True).start()
  elif action=='GetTransportInfo':payload='<CurrentTransportState>'+('STOPPED' if session['fault'] or time.monotonic()-session['since']>2.2 else 'PLAYING')+'</CurrentTransportState><CurrentTransportStatus>'+('ERROR_OCCURRED' if session['fault'] else 'OK')+'</CurrentTransportStatus><CurrentSpeed>1</CurrentSpeed>'
  elif action=='GetPositionInfo':payload='<Track>1</Track><TrackDuration>00:00:01</TrackDuration><RelTime>00:00:01</RelTime>'
  elif action=='GetMediaInfo':payload='<NrTracks>1</NrTracks><MediaDuration>00:00:01</MediaDuration>'
  self.answer(f'<s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/"><s:Body><u:{action}Response xmlns:u="urn:schemas-upnp-org:service:AVTransport:1">{payload}</u:{action}Response></s:Body></s:Envelope>')
server=http.server.ThreadingHTTPServer(('127.0.0.1',0),Handler);threading.Thread(target=server.serve_forever,daemon=True).start()
with socket.socket() as sk:sk.bind(('127.0.0.1',0));port=sk.getsockname()[1]
base=f'http://127.0.0.1:{port}';dev=f'http://127.0.0.1:{server.server_port}/desc';log=(home/'stdout.log').open('wb');proc=subprocess.Popen([str(home/'rtaweb.exe'),'--port',str(port)],cwd=home,stdout=log,stderr=log,creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
def req(path,data=None):
 data=None if data is None else urllib.parse.urlencode(data).encode()
 with urllib.request.urlopen(urllib.request.Request(base+path,data=data),timeout=20) as r:return json.load(r)
def check(ok,name):
 global checks
 checks+=1;print(('PASS ' if ok else 'FAIL ')+name,flush=True)
 if not ok:raise AssertionError(name)
def until(predicate,seconds=15):
 deadline=time.monotonic()+seconds
 while time.monotonic()<deadline:
  if predicate():return
  time.sleep(.05)
 raise AssertionError('timeout')
def reset(mode):
 req('/api/stop',{});req('/api/clear',{});req('/api/playmode?mode='+str(mode),{});records.clear();session['fault']=False
 for i in [1,2]:req('/api/add',{'file':str(home/f'测试 & {i}.wav')})
def play(extra={}):return req('/api/play',{'file':str(home/'测试 & 1.wav'),'backend':'upnp','upnpdev':dev,'rate':'src','dsd':'0','metaon':'0','metallm':'0','anlmaster':'0',**extra})
for _ in range(200):
 try:req('/api/status');break
 except Exception:time.sleep(.05)
try:
 # FB-20261113-A916DFDBD3「CD无法拖入播放」：拖文件夹时 desktop.ini / 封面图会被一起传上来。
 # 旧实现不筛扩展名，它们照样入队，播到才报 ffmpeg "Invalid data found"——用户看到的是"拖不进去"。
 (home/'desktop.ini').write_text('[.ShellClassInfo]',encoding='ascii')
 r=req('/api/add',{'file':str(home/'desktop.ini')})
 check(r.get('ok') is False and '不是可播放' in r.get('error',''),'non-audio file rejected by /api/add')
 with urllib.request.urlopen(urllib.request.Request(base+'/api/upload?name=desktop.ini',
   data=b'[.ShellClassInfo]',method='POST'),timeout=20) as u:up=json.load(u)
 check(up.get('ok') is False and up.get('skipped') is True,'non-audio file rejected by /api/upload')
 with urllib.request.urlopen(urllib.request.Request(base+'/api/upload?name=ok.wav',
   data=(home/'测试 & 1.wav').read_bytes(),method='POST'),timeout=20) as u:up2=json.load(u)
 check(up2.get('ok') is True and up2.get('path','').lower().endswith('.wav'),'audio upload still accepted')
 reset(0);check(play()['ok'],'actual app starts UPnP on loopback renderer');until(lambda:len(records)>=2);until(lambda:not req('/api/status')['playing']);check(len(records)==2,'sequential natural end advances once then stops at queue end');check(all(r.get('rate')==44100 and r.get('bytes')==17684 for r in records),'UTF-8 ampersand raw WAV path is fetched unchanged at source rate')
 reset(2);play();until(lambda:len(records)>=2);req('/api/stop',{});check('测试 &amp; 1.wav' in records[0]['title'] and '测试 &amp; 1.wav' in records[1]['title'],'single-repeat naturally starts the same track again');count=len(records);time.sleep(.8);check(len(records)==count,'manual stop prevents further queue advance')
 reset(1);play();until(lambda:len(records)>=3);req('/api/stop',{});check(len(records)>=3 and '测试 &amp; 1.wav' in records[2]['title'],'list-repeat wraps to first track')
 reset(0);check(play({'upnpUpsample':'1','rate':'176400','filter':'poly-sinc-tight-lp'})['ok'],'actual app accepts upsampling settings');until(lambda:records and records[0].get('rate')==176400);req('/api/stop',{});check(records[0]['rate']==176400,'actual renderer receives prepared 176.4k WAV from production Pipeline')
 reset(0);session['fault']=True;play();until(lambda:not req('/api/status')['playing'],12);check(len(records)==1,'renderer early stop/error does not advance queue')
 reset(0);play();until(lambda:len(records)==1);req('/api/queue/set',{'files':str(home/'测试 & 1.wav')+'\n'+str(home/'测试 & 2.wav')+'\t0\t0.05'});before=len(records);answer=req('/api/skip?dir=1',{});time.sleep(.2);check(not answer['ok'] and 'CUE' in answer['error'] and len(records)==before,'actual skip retains CUE interval and rejects whole-image playback')
 print(json.dumps({'checks':checks,'failures':0}),flush=True)
finally:
 try:req('/api/stop',{});req('/api/quit',{})
 except Exception:pass
 try:proc.wait(timeout=20)
 except subprocess.TimeoutExpired:raise RuntimeError('isolated app did not exit after /api/quit; process left running, no force kill')
 log.close();server.shutdown();(evidence/'queue-results.json').write_text(json.dumps({'checks':checks,'records':records},ensure_ascii=False,indent=2),encoding='utf-8')
