"""Real HTTP library/play API -> loopback-only renderer -> exact supplied WAV bytes.

All fixtures are numeric synthesis; no device is opened, no SSDP is performed.
The private server process exits through /api/quit; never force-kill a process.
"""
import argparse
import hashlib
import html
import http.server
import json
import os
from pathlib import Path
import re
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import urllib.parse
import urllib.request


def main():
    ap = argparse.ArgumentParser()
    for name in ['exe', 'sacd', 'cast', 'encoder', 'decoder', 'probe']:
        ap.add_argument('--'+name, type=Path, required=True)
    args = ap.parse_args()
    home = Path(tempfile.mkdtemp(prefix='iso-cast-')).resolve()
    print('ISO_CAST_PRIVATE_INSTALL='+str(home), flush=True)
    media, temporary, fixtures = home/'media', home/'temp', home/'fixtures'
    media.mkdir(); temporary.mkdir(); fixtures.mkdir()
    env = {**os.environ, 'PATH': str(Path(os.environ['SystemRoot'])/'System32'),
           'TEMP':str(temporary), 'TMP':str(temporary), 'USERPROFILE':str(home/'profile'),
           'APPDATA':str(home/'profile'), 'LOCALAPPDATA':str(home/'profile'),
           # Safe runners set these independently of APPDATA/working directory.
           # Bind them to the installation containing our private library conf.
           'RLPLAYER_LIBRARY_DIR':str(home), 'RLPLAYER_UPDATE_DIR':str(home),
           'RTA_NO_DEVICE':'1', 'RTA_NET_TESTS':'0', 'RLPLAYER_UPDATE_ALLOW_LOCAL':'1',
           'RLPLAYER_UPDATE_MANIFEST':'http://127.0.0.1:1/api/v1/update/windows/latest'}
    for key, dest in [('exe','rtaweb.exe'),('cast','test_cast_iso.exe'),('decoder','ffmpeg.exe'),('probe','ffprobe.exe')]:
        shutil.copy2(getattr(args,key).resolve(),home/dest)
    subprocess.run([str(args.sacd.resolve()),'--emit-fixtures',str(fixtures)],env=env,check=True)
    subprocess.run([sys.executable,'-B',str(Path(__file__).with_name('make_iso_fixture.py')),str(fixtures),str(args.encoder.resolve())],env=env,check=True)
    with (home/'cast-reader.log').open('wb') as log:
        subprocess.run([str(home/'test_cast_iso.exe'),str(fixtures),'full'],cwd=home,env=env,stdout=log,stderr=log,check=True,timeout=60)
    for image in fixtures.glob('*.iso'):shutil.copy2(image,media/image.name)
    (home/'rlplayer-library.conf').write_text(str(media)+'\n',encoding='utf-8')
    (home/'rlplayer-settings.conf').write_text('backend=null\nvolume=-60\ngpuvendor=off\nisp=0\nmetaon=0\nmetallm=0\nanlmaster=0\ncleanup=0\nroomcorrauto=0\n',encoding='utf-8')
    records, checks, library_diagnostics = [], [], {}
    current = {}
    class Renderer(http.server.BaseHTTPRequestHandler):
        def log_message(self,*args): pass
        def answer(self,text):
            b=text.encode();self.send_response(200);self.send_header('Content-Length',str(len(b)));self.send_header('Content-Type','text/xml');self.end_headers();self.wfile.write(b)
        def do_GET(self):
            self.answer('<root><device><friendlyName>ISO offline loopback renderer</friendlyName><modelName>Numeric fixture receiver</modelName><serviceList><service><serviceType>urn:schemas-upnp-org:service:AVTransport:1</serviceType><controlURL>/av</controlURL></service><service><serviceType>urn:schemas-upnp-org:service:ConnectionManager:1</serviceType><controlURL>/cm</controlURL></service></serviceList></device></root>')
        def do_POST(self):
            body=self.rfile.read(int(self.headers.get('Content-Length','0'))).decode()
            action=self.headers.get('SOAPAction','').split('#')[-1].strip('"');payload=''
            if action=='GetProtocolInfo':
                tuples=[(48000,16,2),(96000,24,2),(192000,24,2),(176400,24,2),(176400,24,6)]
                payload='<Source></Source><Sink>'+','.join(f'http-get:*:audio/wav;rate={r};bits={b};channels={c}:*' for r,b,c in tuples)+'</Sink>'
            elif action=='SetAVTransportURI':
                current.clear();current.update(uri=html.unescape(re.search(r'<CurrentURI>(.*?)</CurrentURI>',body,re.S)[1]),didl=html.unescape(body));records.append(current.copy())
            elif action=='Play':
                record=records[-1]
                try:
                    uri=current['uri'];assert urllib.parse.urlsplit(uri).hostname=='127.0.0.1','renderer must fetch loopback only'
                    with urllib.request.urlopen(uri,timeout=20) as response:
                        content=response.read();record['content_type']=response.headers['Content-Type']
                    record['bytes']=content;record['sha256']=hashlib.sha256(content).hexdigest()
                    request=urllib.request.Request(uri,headers={'Range':'bytes=44-143'})
                    with urllib.request.urlopen(request,timeout=20) as response:record['range_ok']=response.status==206 and response.read()==content[44:144]
                except Exception as error:record['error']=repr(error)
            elif action=='GetTransportInfo':payload='<CurrentTransportState>PLAYING</CurrentTransportState><CurrentTransportStatus>OK</CurrentTransportStatus><CurrentSpeed>1</CurrentSpeed>'
            elif action=='GetPositionInfo':payload='<Track>1</Track><TrackDuration>00:00:01</TrackDuration><RelTime>00:00:01</RelTime>'
            elif action=='GetMediaInfo':payload='<NrTracks>1</NrTracks><MediaDuration>00:00:01</MediaDuration>'
            self.answer(f'<s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/"><s:Body><u:{action}Response xmlns:u="urn:schemas-upnp-org:service:AVTransport:1">{payload}</u:{action}Response></s:Body></s:Envelope>')
    renderer=http.server.ThreadingHTTPServer(('127.0.0.1',0),Renderer)
    threading.Thread(target=renderer.serve_forever,daemon=True).start()
    with socket.socket() as s:s.bind(('127.0.0.1',0));port=s.getsockname()[1]
    base=f'http://127.0.0.1:{port}';dev=f'http://127.0.0.1:{renderer.server_port}/description.xml'
    def request(route,data=None):
        data=None if data is None else urllib.parse.urlencode(data).encode()
        with urllib.request.urlopen(urllib.request.Request(base+route,data=data),timeout=30) as response:return json.load(response)
    def check(ok,label):
        checks.append(dict(pass_=bool(ok),label=label));print(('PASS ' if ok else 'FAIL ')+label,flush=True)
        if not ok:raise AssertionError(label)
    def until(predicate,seconds=30):
        deadline=time.monotonic()+seconds
        while time.monotonic()<deadline:
            result=predicate()
            if result:return result
            time.sleep(.02)
        raise AssertionError('timeout waiting for private loopback session')
    def wav(b):
        assert b[:4]==b'RIFF' and b[8:12]==b'WAVE'
        fmt,channels,rate,_,_,bits=struct.unpack_from('<HHIIHH',b,20)
        assert fmt==1 and len(b)==44+struct.unpack_from('<I',b,40)[0]
        return rate,bits,channels,b[44:]
    log=(home/'app.log').open('wb')
    child=subprocess.Popen([str(home/'rtaweb.exe'),'--port',str(port)],cwd=home,env=env,stdout=log,stderr=log,creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
    try:
        def ready():
            if child.poll() is not None:raise AssertionError('private app exited during startup')
            try:return request('/api/ping')
            except OSError:return False
        until(ready)
        library_diagnostics.update(configured_directory=env['RLPLAYER_LIBRARY_DIR'],
                                   roots=request('/api/lib/stats')['roots'])
        print('ISO_CAST_LIBRARY_ROOTS='+json.dumps(library_diagnostics,ensure_ascii=False),flush=True)
        check(request('/api/lib/scan',{}).get('ok'),'actual API scans only private numeric images')
        def library_ready():
            status=request('/api/lib/status')
            if status!=library_diagnostics.get('status'):
                print('ISO_CAST_LIBRARY_STATUS='+json.dumps(status,ensure_ascii=False),flush=True)
                library_diagnostics['status']=status
            return not status['scanning'] and status['tracks']==23
        until(library_ready)
        tracks=request('/api/lib/search?q=')['tracks']
        check(len(tracks)==23,'same HTTP library exposes all twenty-three area/group tracks')
        for track in tracks:
            path=track['path'];is_dsd='#sacd=' in path
            request('/api/stop',{});request('/api/clear',{})
            before=len(records)
            result=request('/api/play',dict(file=path,backend='upnp',upnpdev=dev,dsd='64' if is_dsd else '0',rate='src',upnpUpsample='0',cleanup='0',isp='0',gpuvendor='off',metaon='0',metallm='0',anlmaster='0',roomcorrauto='0'))
            check(result.get('ok'),'unified library play accepts '+Path(path).name)
            def fetched():
                state=request('/api/cast/state')
                if state['error']:raise AssertionError(state['error'])
                return state if len(records)>before and records[-1].get('bytes') and state['requestCount']>0 else False
            state=until(fetched);record=records[-1];rate,bits,channels,data=wav(record['bytes'])
            check(record.get('range_ok') and record.get('content_type')=='audio/wav','actual renderer GET/Range returns complete stable WAV')
            if is_dsd:
                area,number=path.split('#sacd=')[1].split(':');number=int(number)
                check((rate,bits,channels)==(176400,24,2 if area=='2ch' else 6),'SACD DoP format and area channel count match supplied bytes')
                planes=[bytearray() for _ in range(channels)]
                for index in range(len(data)//(3*channels)):
                    for channel in range(channels):
                        low,high,marker=data[(index*channels+channel)*3:(index*channels+channel+1)*3]
                        assert marker==(0xfa if index%2 else 5)
                        planes[channel].extend([high,low])
                if 'sacd-raw.iso' in path:
                    expected=bytes((i*channels+c)*13+f*17+(number-1)*29+(41 if area=='mch' else 0)&255 for c in range(channels) for f in range(3) for i in range(4704))
                else:expected=(fixtures/(Path(path.split('#')[0]).name+'-'+area+'-'+str(number)+'.dsdref')).read_bytes()
                check(b''.join(planes)==expected,'every restored raw/DST DSD bit and first/last byte match source')
                check(state['softwareBitPerfect'],'SACD supply reports software bit preservation')
            else:
                group,_,number=map(int,path.split('#dvda=')[1].split(':'))
                frames=1200 if group==3 else 320 if group==2 else 480
                check((rate,bits,channels)==(96000 if group==2 else 48000,24 if group==2 else 16,2),'DVD supply declares actual source integer precision')
                expected=b''.join(((int(((i+(0 if group==3 else (number-1)*frames))*43+c*991)%20001)-10000)<<(8 if group==2 else 0)).to_bytes(bits//8,'little',signed=True) for i in range(frames) for c in range(2))
                check(data==expected,'every DVD LPCM/MLP sample and selected track first/last sample match numeric formula')
                check(state['softwareBitPerfect'],'DVD unchanged supply reports software bit preservation')
            record['source']=path;record['state']=state
        # Positive processing path through the same unified API and production Pipeline.
        for image,group in [('synthetic-dvda.iso',1),('synthetic-dvda-udf150.iso',3)]:
            request('/api/stop',{});request('/api/clear',{});before=len(records)
            result=request('/api/play',dict(file=str(media/image)+f'#dvda={group}:1:1',backend='upnp',upnpdev=dev,rate='192000',bits='24',upnpUpsample='1',cleanup='1',cleanup_strength='0.8',cleanup_noise_db='-70',dsd='0',metaon='0',metallm='0',anlmaster='0',isp='0',gpuvendor='off',roomcorrauto='0'))
            check(result.get('ok'),'DVD processed cast accepted')
            state=until(lambda:request('/api/cast/state') if len(records)>before and records[-1].get('bytes') else False)
            rate,bits,channels,data=wav(records[-1]['bytes'])
            check(rate==192000 and bits==24 and len(data)>0 and state['cleanupApplied'] and not state['softwareBitPerfect'],'actual DVD LPCM/MLP supply applies upsampling and recording cleanup')
        request('/api/stop',{});request('/api/clear',{})
        denied=request('/api/cast/start',dict(file=str(media/'sacd-raw.iso')+'#sacd=2ch:1',dev=dev,dop='0'))
        check(not denied.get('ok') and 'DoP' in denied.get('error',''),'actual cast API explicitly refuses SACD PCM conversion without DoP')
        check(not list(temporary.glob('rtacast_*')),'all prepared WAV/AOB files removed after session stop')
    finally:
        if child.poll() is None:
            try:request('/api/stop',{});request('/api/quit',{})
            except OSError:pass
            child.wait(timeout=30)
        log.close();renderer.shutdown()
        output=dict(home=str(home),checks=checks,child_exit=child.returncode,library_diagnostics=library_diagnostics,records=[{k:v for k,v in record.items() if k!='bytes'} for record in records])
        (home/'results.json').write_text(json.dumps(output,ensure_ascii=False,indent=2),encoding='utf-8')
    check(child.returncode==0,'private actual application exits normally')
    (home/'results.json').write_text(json.dumps(output,ensure_ascii=False,indent=2),encoding='utf-8')
    print(json.dumps(dict(checks=len(checks),records=len(records),evidence=str(home/'results.json'))),flush=True)


if __name__=='__main__':main()
