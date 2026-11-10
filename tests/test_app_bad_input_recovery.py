"""Explicit isolated runtime integration: malformed HTTP, paths and null playback.

Not registered as an automatic CTest: requires PORT and a generated runtime.
Every play explicitly selects backend=null, -60dB, PCM, no GPU/analysis/online AI.
"""
import pathlib,sys,json,socket,time,urllib.request,urllib.parse,urllib.error,wave
port=int(sys.argv[1]);runtime=pathlib.Path(sys.argv[2]).resolve();allowed=pathlib.Path('D:/RLPlayer-Develop-20261103').resolve()
if not(19000<=port<20000 and runtime.is_relative_to(allowed) and runtime.name.startswith('runtime-app-')):raise SystemExit('Private runtime and QA port required')
base=f'http://127.0.0.1:{port}';checks=0
def check(ok,label):
 global checks
 checks+=1;print(('PASS ' if ok else 'FAIL ')+label,flush=True)
 if not ok:raise AssertionError(label)
def request(route,values=None):
 data=None if values is None else urllib.parse.urlencode(values).encode()
 with urllib.request.urlopen(urllib.request.Request(base+route,data=data),timeout=15) as r:return json.load(r)
def raw(payload):
 with socket.create_connection(('127.0.0.1',port),timeout=3) as s:
  s.sendall(payload);s.shutdown(socket.SHUT_WR);parts=[]
  while True:
   try:b=s.recv(65536)
   except ConnectionResetError:break
   if not b:break
   parts.append(b)
 return b''.join(parts)
def wait(predicate,seconds=12):
 until=time.monotonic()+seconds
 while time.monotonic()<until:
  value=request('/api/status')
  if predicate(value):return value
  time.sleep(.05)
 raise AssertionError('state did not settle')
check(request('/api/status')['playing'] is False,'runtime starts idle')
settings=request('/api/settings')['kv'];check(settings.get('backend')=='null' and settings.get('metaon')=='0' and settings.get('anlmaster')=='0','isolated settings disable devices, online metadata and analysis')
host=f'Host: 127.0.0.1:{port}\r\n'.encode()
for fragment,code in [(b'Content-Length: nope\r\n',400),(b'Content-Length: 16777217\r\n',413),(b'Content-Length: 9\r\n',400)]:
 response=raw(b'POST /api/noop HTTP/1.1\r\n'+host+fragment+b'Connection: close\r\n\r\nx')
 check(response.startswith(f'HTTP/1.1 {code}'.encode()),'malformed/incomplete/oversized body is rejected with expected HTTP status')
 check(request('/api/status')['ok'] is True,'next healthy request succeeds after malformed HTTP')
for route in ['/api/meta?file=%FF','/api/files?dir=%FF']:
 try:reply=request(route)
 except urllib.error.HTTPError as e:reply=json.load(e)
 check(reply.get('ok') is False and bool(reply.get('error')),'invalid UTF-8 path is rejected explicitly')
 check(request('/api/status')['ok'] is True,'invalid path leaves service responsive')
for value in ['NaN','inf','1e309','not-a-number']:
 try:request('/api/seek?pos='+value,{})
 except urllib.error.HTTPError as e:check(e.code==400,'nonfinite or malformed seek is rejected before state mutation')
 else:raise AssertionError('invalid seek accepted')
fixtures=runtime/'fixtures';fixtures.mkdir(exist_ok=True)
valid=fixtures/'recovery-silence.wav'
with wave.open(str(valid),'wb') as f:f.setnchannels(2);f.setsampwidth(2);f.setframerate(44100);f.writeframes(bytes(44100*4))
damaged=fixtures/'recovery-truncated.wav';damaged.write_bytes(b'RIFF\xff\xff\xff\xffWAVEfmt ')
def play(file):
 request('/api/clear',{});request('/api/repeat?rate=0',{})
 return request('/api/play',{'file':str(file),'backend':'null','volume':'-60','dsd':'0','rate':'src','filter':'auto','gpu':'off','gpuvendor':'off','autoanl':'0','anlmaster':'0','metallm':'0','metaon':'0','shaper':'none'})
for broken in [fixtures/'missing-recovery.wav',damaged]:
 play(broken);bad=wait(lambda j:not j['playing'] and bool(j.get('error')))
 check(str(broken)==bad.get('current') and bool(bad['error']),'failed file decode reports error and returns to stopped state')
 play(valid);good=wait(lambda j:not j['playing'] and j.get('current')==str(valid) and not j.get('error') and j.get('pos',0)>.9)
 check(good['outRate']==44100 and good['dur']>=.99,'next generated file completes through null sink after prior decoder failure')
 check(request('/api/signal')['path']['device']=='Null 输出（不出声）','actual signal proves null sink, not an audio endpoint')
request('/api/stop',{});check(request('/api/status')['playing'] is False,'stop remains responsive after decode recovery sequence')
print(json.dumps({'checks':checks,'failures':0,'audio_sink':'null','external_network':False}),flush=True)
