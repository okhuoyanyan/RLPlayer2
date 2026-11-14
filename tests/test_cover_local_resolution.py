"""Actual HTTP album-cover policy: trusted local art, cold/warm and ownership.

Images are numeric PNG synthesis; all processes and media are private, offline.
"""
import argparse
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
import sys
import threading
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request
import wave
from test_track_cover_resolution import png, fnv

sys.stdout.reconfigure(encoding='utf-8')


def startup_migration_cases(args,base,check):
    """Same queue-only upgrade trigger, through an actual private application.

    Target index deliberately retains inferred metadata. Only the startup Meta
    migration before Library.load can restore the confirmed album identity.
    """
    # Keep the version+SHA filenames below Win32 MAX_PATH even when the safe
    # runner's evidence directory itself has a long descriptive name.
    short=Path(tempfile.mkdtemp(prefix='pm-',dir=Path(tempfile.gettempdir()).anchor))
    base.parent.mkdir(parents=True,exist_ok=True)
    (base.parent/'startup-roots.json').write_text(json.dumps(dict(root=str(short))),encoding='utf-8')
    for existing in [False,True]:
        versions=short/('c' if existing else 'm')
        current=versions/'RLPlayer-3.3.1-win64'
        old=versions/('RLPlayer-3.2.2-win64.previous-'+'a'*32)
        music=versions/'music';music.mkdir(parents=True);current.mkdir();old.mkdir()
        song=music/'unlabeled.wav'
        with wave.open(str(song),'wb') as f:f.setparams((2,2,44100,0,'NONE','not compressed'));f.writeframes(bytes(4410*4))
        for source,name in [(args.exe,'rtaweb.exe'),(args.decoder,'ffmpeg.exe'),(args.probe,'ffprobe.exe')]:shutil.copy2(source.resolve(),current/name)
        path=str(song);key=fnv('Confirmed album\x1f'+str(music).lower())
        trackkey=fnv(path.replace('/','\\').lower());metakey=fnv(path)
        album_image,track_image,meta_image,current_image=[png(seed,64) for seed in [71,73,79,83]]
        def put(root,leaf,data):
            target=root/leaf;target.parent.mkdir(parents=True,exist_ok=True);target.write_bytes(data);return target
        def typed(root,folder,logical,image):
            slug=logical.removeprefix('track:');digest=hashlib.sha256(image).hexdigest();blob=slug+'.'+digest+'.blob'
            put(root,folder+'/'+blob,image)
            record=dict(version=1,key=logical,source='manual',blob=blob,sha256=digest,bytes=len(image))
            return put(root,folder+'/'+slug+'.jpg.source.json',json.dumps(record,separators=(',',':')).encode())
        typed(old,'covercache',key,album_image);typed(old,'trackcovercache','track:'+trackkey,track_image)
        metadata=dict(path=path,cover='covers/'+metakey+'.png',fields={name:dict(value=value,source='user',ts=1,confidence=1) for name,value in [('album','Confirmed album'),('artist','Confirmed artist'),('albumArtist','Confirmed artist')]})
        put(old,'rlplayer-meta/tracks/'+metakey+'.json',json.dumps(metadata,separators=(',',':')).encode())
        put(old,'rlplayer-meta/covers/'+metakey+'.png',meta_image)
        put(old,'covercache/'+key+'.none',b'provider miss must stay behind')
        put(old,'rlplayer-playlist.txt',('RTAQUEUE1\nindex=0\nrepeat=0\nshuffle=0\nitem='+path+'\n').encode())
        row=[path,'0',str(song.stat().st_size),'music','','','Unlabeled','1','0','0','44100','16','2','0.100','0','','','0','0','0','directory_inferred','pcm_s16le']
        put(current,'rlplayer-index.tsv',('#libIndexV\t6\n#sourceDepthV\t1\n'+'\t'.join(row)+'\n').encode())
        put(current,'rlplayer-library.conf',(str(music)+'\n').encode())
        put(current,'rlplayer-settings.conf',b'backend=null\nmetaon=0\nmetallm=0\nanlmaster=0\nisp=0\ngpuvendor=off\nvolume=-60\n')
        expected_album='Current choice' if existing else 'Confirmed album'
        preserved={}
        if existing:
            metadata['cover']='';metadata['fields']['album']['value']=expected_album
            ownmeta=put(current,'rlplayer-meta/tracks/'+metakey+'.json',json.dumps(metadata,separators=(',',':')).encode())
            ownrecord=typed(current,'trackcovercache','track:'+trackkey,current_image)
            preserved={p:p.read_bytes() for p in [ownmeta,ownrecord]}
        temporary=current/'temp';temporary.mkdir()
        env={**os.environ,'PATH':str(Path(os.environ['SystemRoot'])/'System32'),'TEMP':str(temporary),'TMP':str(temporary),'RLPLAYER_LIBRARY_DIR':str(current),'RLPLAYER_UPDATE_DIR':str(current),'USERPROFILE':str(current/'profile'),'APPDATA':str(current/'profile'),'LOCALAPPDATA':str(current/'profile'),'RTA_NET_TESTS':'0','RTA_NO_DEVICE':'1','RLPLAYER_UPDATE_ALLOW_LOCAL':'1','RLPLAYER_UPDATE_MANIFEST':'http://127.0.0.1:1/api/v1/update/windows/latest'}
        before=None
        for restart in range(2):
            with socket.socket() as s:s.bind(('127.0.0.1',0));port=s.getsockname()[1]
            url=f'http://127.0.0.1:{port}'
            def req(route,body=None):
                try:
                    with urllib.request.urlopen(urllib.request.Request(url+route,data=body),timeout=20) as r:return r.status,r.read()
                except urllib.error.HTTPError as e:return e.code,e.read()
            logfile=(current/f'app-{restart}.log').open('wb')
            child=subprocess.Popen([str(current/'rtaweb.exe'),'--port',str(port)],cwd=current,env=env,stdout=logfile,stderr=logfile,creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
            try:
                deadline=time.monotonic()+30
                while True:
                    if child.poll() is not None:raise RuntimeError('private migration backend exited')
                    try:req('/api/ping');break
                    except OSError:
                        if time.monotonic()>deadline:raise
                        time.sleep(.03)
                albums=json.loads(req('/api/lib/albums')[1])['albums'];album=next((a for a in albums if a['name']==expected_album),None)
                check(bool(album) and album['identitySource']=='user_assigned',f'startup-before-library-load-restores-identity-existing={existing}-restart={restart}')
                image=req('/api/cover?file='+urllib.parse.quote(path,safe=''))
                check(image==(200,current_image if existing else track_image),f'startup-track-image-preserves-exact-manual-priority-existing={existing}-restart={restart}')
                if not existing:
                    check(req('/api/lib/cover?k='+key)==(200,album_image),f'startup-missing-album-manual-image-restored-restart={restart}')
                else:check(all(p.read_bytes()==b for p,b in preserved.items()),f'startup-never-overwrites-current-records-restart={restart}')
                check(not (current/'covercache'/(key+'.none')).exists(),f'provider-negative-not-migrated-existing={existing}-restart={restart}')
            finally:
                if child.poll() is None:req('/api/stop',b'');req('/api/quit',b'');child.wait(timeout=30)
                logfile.close()
            after={str(p.relative_to(current)):(hashlib.sha256(p.read_bytes()).hexdigest(),p.stat().st_mtime_ns) for directory in ['covercache','trackcovercache','rlplayer-meta','history-artwork'] for p in (current/directory).rglob('*') if p.is_file()}
            if before is not None:check(after==before,f'second-startup-artwork-files-and-timestamps-unchanged-existing={existing}')
            before=after


def main():
    parser=argparse.ArgumentParser()
    for name in ['exe','encoder','decoder','probe','evidence-dir']:
        parser.add_argument('--'+name,type=Path,required=True)
    parser.add_argument('--compiler',type=Path,default=Path('C:/Program Files/AMD/ROCm/7.2/bin/clang++.exe'))
    parser.add_argument('--startup-only',action='store_true')
    args=parser.parse_args();output=args.evidence_dir.resolve();output.mkdir(parents=True,exist_ok=False)
    home=Path(tempfile.mkdtemp(prefix='rlpcl-',dir=Path(tempfile.gettempdir()).anchor))
    (output/'runtime.json').write_text(json.dumps(dict(root=str(home))),encoding='utf-8')
    if args.startup_only:
        checks=[]
        def check(ok,label,**details):checks.append(dict(passed=bool(ok),label=label,**details));print(('PASS ' if ok else 'FAIL ')+label,flush=True)
        startup_migration_cases(args,home/'profile-startup',check)
        (output/'results.json').write_text(json.dumps(dict(checks=checks,passed=sum(c['passed'] for c in checks),failed=sum(not c['passed'] for c in checks)),ensure_ascii=False,indent=2),encoding='utf-8')
        return 1 if any(not c['passed'] for c in checks) else 0
    media=home/'media';media.mkdir();temporary=home/'temp';temporary.mkdir()
    for key,dest in [('exe','rtaweb.exe'),('decoder','ffmpeg-real.exe'),('probe','ffprobe.exe')]:shutil.copy2(getattr(args,key).resolve(),home/dest)
    # Delay only the isolated external image-extraction tool. Six cold image
    # requests must not fill all browser connections ahead of the JSON request.
    delay=home/'delay.cpp';delay.write_text(r'''
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>
int main(){wchar_t path[MAX_PATH];GetModuleFileNameW(nullptr,path,MAX_PATH);std::wstring p=path;p=p.substr(0,p.find_last_of(L"\\/"))+L"\\ffmpeg-real.exe";std::wstring cmd=GetCommandLineW();size_t n=0;if(cmd[0]==L'"'){n=cmd.find(L'"',1)+1;}else n=cmd.find(L' ');cmd=L"\""+p+L"\""+(n==std::wstring::npos?L"":cmd.substr(n));Sleep(1200);STARTUPINFOW si{};si.cb=sizeof(si);PROCESS_INFORMATION pi{};if(!CreateProcessW(nullptr,cmd.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW,nullptr,nullptr,&si,&pi))return 2;CloseHandle(pi.hThread);WaitForSingleObject(pi.hProcess,INFINITE);DWORD result;GetExitCodeProcess(pi.hProcess,&result);CloseHandle(pi.hProcess);return int(result);}
''',encoding='utf-8')
    subprocess.run([str(args.compiler),'-std=c++20','-O2',str(delay),'-o',str(home/'ffmpeg.exe')],check=True)
    paths={};images={name:png(31+i,64) for i,name in enumerate(['embed','dir','manual','meta','inferred','mixed'])}
    def track(label,folder,album,embedded=None):
        directory=media/folder;directory.mkdir(exist_ok=True);wav=directory/(label+'.wav')
        with wave.open(str(wav),'wb') as f:f.setparams((2,2,44100,0,'NONE','not compressed'));f.writeframes(bytes(4410*4))
        path=directory/(label+' & 曲.flac');command=[str(args.encoder.resolve()),'-v','error','-y','-i',str(wav)]
        if embedded:
            pic=directory/(label+'.png');pic.write_bytes(embedded);command+=['-i',str(pic),'-map','0:a','-map','1:v','-c:a','flac','-c:v','copy','-disposition:v','attached_pic']
        command+=['-metadata','artist=Numeric fixture artist','-metadata','title='+label]
        if album:command+=['-metadata','album='+album]
        command+=[str(path)];subprocess.run(command,check=True,capture_output=True);wav.unlink()
        if embedded:pic.unlink()
        paths[label]=path
    for i in range(6):track('embed'+str(i),'embed'+str(i),'Owned embedded '+str(i),images['embed'])
    track('manualMember','embed0','Owned embedded 0')
    track('dir','directory','Owned directory');(paths['dir'].parent/'cover.png').write_bytes(images['dir'])
    track('mixedA','mixed','Unrelated album A');track('mixedB','mixed','Unrelated album B');(paths['mixedA'].parent/'cover.png').write_bytes(images['mixed'])
    track('inferred','unidentified',None,images['inferred'])
    track('inferredDir','unidentified-directory',None);(paths['inferredDir'].parent/'cover.png').write_bytes(images['dir'])
    track('yearOwn','2023',None,images['inferred']);track('yearBlank','2023',None)
    track('yearAlpha','2023','Year mixed Alpha');track('yearBeta','2023','Year mixed Beta')
    (paths['yearOwn'].parent/'cover.png').write_bytes(images['mixed'])
    for i in range(7):track('seventh'+str(i),'seventh','Seventh-only cover',images['embed'] if i==6 else None)
    track('meta','meta','Meta-only album');meta=home/'rlplayer-meta';(meta/'covers').mkdir(parents=True)
    track('corrupt','corrupt','Owned corrupt image');(paths['corrupt'].parent/'cover.png').write_bytes(bytes(2048))
    (meta/'covers'/(fnv(str(paths['meta']))+'.png')).write_bytes(images['meta'])
    (home/'rlplayer-library.conf').write_text(str(media)+'\n',encoding='utf-8')
    (home/'rlplayer-settings.conf').write_text('backend=null\nmetaon=0\nmetallm=0\nanlmaster=0\nisp=0\ngpuvendor=off\nvolume=-60\n',encoding='utf-8')
    env={**os.environ,'PATH':str(Path(os.environ['SystemRoot'])/'System32'),'TEMP':str(temporary),'TMP':str(temporary),'RLPLAYER_LIBRARY_DIR':str(home),'RLPLAYER_UPDATE_DIR':str(home),'USERPROFILE':str(home/'profile'),'APPDATA':str(home/'profile'),'LOCALAPPDATA':str(home/'profile'),'RTA_NET_TESTS':'0','RTA_NO_DEVICE':'1','RLPLAYER_UPDATE_ALLOW_LOCAL':'1','RLPLAYER_UPDATE_MANIFEST':'http://127.0.0.1:1/api/v1/update/windows/latest'}
    checks=[];base='';process=None;log=None
    def request(route,data=None,content_type=None):
        body=urllib.parse.urlencode(data).encode() if isinstance(data,dict) else data
        headers={'Content-Type':content_type} if content_type else {}
        try:
            with urllib.request.urlopen(urllib.request.Request(base+route,data=body,headers=headers),timeout=30) as r:return r.status,r.headers.get('Content-Type'),r.read()
        except urllib.error.HTTPError as e:return e.code,e.headers.get('Content-Type'),e.read()
    def api(route,data=None):return json.loads(request(route,data)[2])
    def check(ok,label,**details):checks.append(dict(passed=bool(ok),label=label,**details));print(('PASS ' if ok else 'FAIL ')+label,flush=True)
    def until(condition,seconds=35):
        end=time.monotonic()+seconds
        while time.monotonic()<end:
            value=condition()
            if value:return value
            time.sleep(.04)
        raise AssertionError('private HTTP deadline')
    def start(round):
        nonlocal base,process,log
        with socket.socket() as s:s.bind(('127.0.0.1',0));port=s.getsockname()[1]
        base=f'http://127.0.0.1:{port}';log=(home/f'app-{round}.log').open('wb');process=subprocess.Popen([str(home/'rtaweb.exe'),'--port',str(port)],cwd=home,env=env,stdout=log,stderr=log,creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
        def ready():
            if process.poll() is not None:raise RuntimeError('private application exited')
            try:return api('/api/ping')
            except OSError:return False
        until(ready)
    def stop():
        if process and process.poll() is None:
            api('/api/stop',{});api('/api/quit',{});process.wait(timeout=30)
        if log:log.close()
    try:
        start(0);api('/api/lib/scan',{});until(lambda:not api('/api/lib/status')['scanning'] and api('/api/lib/status')['tracks']==25)
        albums=api('/api/lib/albums')['albums'];keys={}
        for label,path in paths.items():
            record=api('/api/meta?file='+urllib.parse.quote(str(path),safe=''))
            # The independent library API identifies the exact browsing group.
            item=next(t for t in api('/api/lib/search?q=')['tracks'] if t['path']==str(path))
            album=next(a for a in albums if a['name']==item['album'])
            keys[label]=album['k']
        for label,expected,kind in [('embed0',images['embed'],'embed'),('dir',images['dir'],'dir'),('inferred',images['inferred'],'embed'),('inferredDir',images['dir'],'dir'),('yearOwn',images['inferred'],'embed'),('seventh0',images['embed'],'embed')]:
            status,mime,b=request('/api/lib/cover?k='+keys[label])
            check(status==200 and b==expected,'trusted-'+kind+'-album-http-returns-original-image',status=status,bytes=len(b))
            row=next(a for a in api('/api/lib/albums')['albums'] if a['k']==keys[label]);detail=api('/api/lib/album?k='+keys[label])
            check(all(a.get('cv') and a.get('cvSrc')==kind for a in [row,detail]),kind+'-list-detail-image-source-agree')
            if label in ['inferred','inferredDir','yearOwn']:
                check(row['identitySource']=='directory_inferred' and not row['onlineCoverEligible'],'local-group-picture-never-promotes-online-identity-'+label)
        for label in ['mixedA','mixedB','yearAlpha','yearBeta','meta']:
            row=next(a for a in api('/api/lib/albums')['albums'] if a['k']==keys[label]);status,_,b=request('/api/lib/cover?k='+keys[label])
            check(status==404 and not b and not row['cv'],label+'-must-not-borrow-directory-or-song-only-art')
        check(request('/api/cover?file='+urllib.parse.quote(str(paths['yearBlank']),safe=''))[0]==404,'inferred-blank-song-does-not-inherit-group-embedded-or-public-year-directory-image')
        status,_,body=request('/api/lib/cover?k='+keys['corrupt']);row=next(a for a in api('/api/lib/albums')['albums'] if a['k']==keys['corrupt'])
        check(status==404 and not body and not row['cv'] and row['cvSrc']=='none','known-invalid-local-image-cannot-advertise-cover')
        quoted=urllib.parse.quote(str(paths['embed0']),safe='')
        check(api('/api/cover/set?file='+quoted,images['manual']).get('ok'),'manual-track-cover-commits')
        check(request('/api/cover?file='+quoted)[2]==images['manual'],'manual-track-precedes-embedded')
        check(api('/api/cover/reset?file='+quoted,b'').get('ok'),'explicit-track-reset-commits')
        check(request('/api/cover?file='+quoted)[2]==images['embed'],'reset-removes-manual-choice-and-keeps-song-owned-embedded-art')
        check(api('/api/lib/cover/set?k='+keys['embed0'],images['manual']).get('ok'),'manual-album-cover-commits')
        check(request('/api/lib/cover?k='+keys['embed0'])[2]==images['manual'],'manual-album-precedes-model-local')
        member=urllib.parse.quote(str(paths['manualMember']),safe='')
        check(request('/api/cover?file='+member)[2]==images['manual'],'exact-group-manual-picture-inherited-by-tagged-member-without-own-art')
        member_meta=api('/api/meta?file='+member)
        check(member_meta['albumIdentitySource']=='tagged','group-manual-fallback-does-not-change-member-identity')
        # Own embedded art stays above even an explicit album picture.
        check(request('/api/cover?file='+quoted)[2]==images['embed'],'song-owned-embedded-precedes-album-manual')
        stop();start(1)
        # Six deliberately slow cold image requests; JSON must still complete
        # before the tool's1.2s delay, while warm hits do no extraction.
        with concurrent.futures.ThreadPoolExecutor(max_workers=6) as pool:
            futures=[pool.submit(request,'/api/lib/cover?k='+keys['embed'+str(i)]) for i in range(6)]
            time.sleep(.08);t=time.monotonic();rows=api('/api/lib/albums')['albums'];elapsed=time.monotonic()-t
            check(elapsed<.8,'six-slow-cold-images-do-not-block-album-json',seconds=elapsed)
            cold=[f.result()[0] for f in futures]
        def warmed():return all(request('/api/lib/cover?k='+keys['embed'+str(i)])[0]==200 for i in range(6))
        until(warmed)
        for i in range(6):
            label='embed'+str(i);expected=images['manual'] if i==0 else images['embed'];status,_,b=request('/api/lib/cover?k='+keys[label]);row=next(a for a in api('/api/lib/albums')['albums'] if a['k']==keys[label])
            check(status==200 and b==expected and row['cv'] and row['cvSrc']==('manual' if i==0 else 'embed'),'cold-warm-source-and-exact-image-'+label)
        (home/'cold-status.json').write_text(json.dumps(cold),encoding='utf-8')
        stop();startup_migration_cases(args,home/'profile-startup',check)
    finally:
        stop();(output/'results.json').write_text(json.dumps(dict(checks=checks,passed=sum(c['passed'] for c in checks),failed=sum(not c['passed'] for c in checks),child_exit=process.returncode if process else None),ensure_ascii=False,indent=2),encoding='utf-8')
    print(json.dumps(dict(checks=len(checks),failures=sum(not c['passed'] for c in checks),evidence=str(output/'results.json'))),flush=True)
    return 1 if any(not c['passed'] for c in checks) else 0


if __name__=='__main__':raise SystemExit(main())
