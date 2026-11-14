"""Generated audio discs -> packaged decoders -> library -> actual Null playback.

No music download, physical drive, default endpoint, or production service is used.
Every launched process belongs to a retained, unique private installation.
"""
import argparse
import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import urllib.parse
import urllib.request


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--exe', type=Path, required=True)
    p.add_argument('--sacd', type=Path, required=True)
    p.add_argument('--dvd', type=Path, required=True)
    p.add_argument('--library', type=Path, required=True)
    p.add_argument('--encoder', type=Path, required=True)
    p.add_argument('--decoder', type=Path, required=True)
    p.add_argument('--probe', type=Path, required=True)
    a = p.parse_args()
    for key, value in vars(a).items():
        setattr(a, key, value.resolve())
    source = Path(__file__).resolve().parents[1]
    home = Path(tempfile.mkdtemp(prefix='rlplayer-disc-e2e-')).resolve()
    print('ISO_E2E_PRIVATE_INSTALL='+str(home), flush=True)
    fixtures, media = home/'fixtures', home/'media'
    fixtures.mkdir(); media.mkdir()
    env = {**os.environ, 'RTA_NO_DEVICE': '1', 'RTA_NET_TESTS': '0',
           'APPDATA': str(home/'profile'), 'LOCALAPPDATA': str(home/'profile'),
           'USERPROFILE': str(home/'profile'),
           'RLPLAYER_UPDATE_ALLOW_LOCAL': '1',
           'RLPLAYER_UPDATE_MANIFEST': 'http://127.0.0.1:1/api/v1/update/windows/latest'}
    subprocess.run([str(a.sacd), '--emit-fixtures', str(fixtures)], env=env, check=True)
    subprocess.run([sys.executable, '-B', str(source/'tests/make_iso_fixture.py'),
                    str(fixtures), str(a.encoder)], env=env, check=True)
    for exe, dest in [(a.exe, 'rtaweb.exe'), (a.dvd, 'test_dvd_audio.exe'),
                      (a.library, 'test_image_library.exe'),
                      (a.decoder, 'ffmpeg.exe'), (a.probe, 'ffprobe.exe')]:
        shutil.copy2(exe, home/dest)
    decoder_env = {**env, 'PATH': str(Path(os.environ['SystemRoot'])/'System32')}
    with (home/'dvd-reader.log').open('wb') as log:
        subprocess.run([str(home/'test_dvd_audio.exe'), str(fixtures/'synthetic-dvda.iso')],
                       cwd=home, env=decoder_env, stdout=log, stderr=log, check=True, timeout=60)
    with (home/'image-library.log').open('wb') as log:
        subprocess.run([str(home/'test_image_library.exe'), str(fixtures/'sacd-raw.iso'),
                        str(fixtures/'sacd-dst.iso'), str(fixtures/'synthetic-dvda.iso')],
                       cwd=home, env=decoder_env, stdout=log, stderr=log, check=True, timeout=60)
    for name in ['sacd-raw.iso', 'sacd-dst.iso', 'synthetic-dvda.iso',
                 'synthetic-dvda-udf102.iso', 'synthetic-dvda-udf150.iso']:
        shutil.copy2(fixtures/name, media/name)
    # An absent config asks Library::load to discover the Windows Music folders,
    # including Public Music. Seed the real production config with only this
    # generated folder so the test never inventories the developer's collection.
    (home/'rlplayer-library.conf').write_text(str(media)+'\n', encoding='utf8')
    (home/'rlplayer-settings.conf').write_text(
        'backend=null\nvolume=-60\nisp=0\ngpuvendor=off\nrate=src\ndsd=0\n'
        'roomcorrauto=0\nmetaon=0\nmetallm=0\nanlmaster=0\ncleanup=0\n', encoding='utf8')
    checks, played, keys = [], [], set()
    def check(ok, label):
        print(('PASS ' if ok else 'FAIL ')+label, flush=True)
        checks.append({'label': label, 'pass': bool(ok)})
        if not ok:
            raise AssertionError(label)
    for restart in range(2):
        with socket.socket() as listener:
            listener.bind(('127.0.0.1', 0)); port = listener.getsockname()[1]
        base = f'http://127.0.0.1:{port}'
        def req(route, data=None):
            body = None if data is None else urllib.parse.urlencode(data).encode()
            with urllib.request.urlopen(urllib.request.Request(base+route, data=body), timeout=30) as response:
                return json.load(response)
        log = (home/f'app-{restart}.log').open('wb')
        child = subprocess.Popen([str(home/'rtaweb.exe'), '--port', str(port)], cwd=home,
                                 env=decoder_env, stdout=log, stderr=log,
                                 creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
        try:
            deadline = time.monotonic()+30
            while True:
                if child.poll() is not None:
                    raise RuntimeError('private rtaweb exited during startup')
                try:
                    req('/api/ping'); break
                except (OSError, ValueError):
                    if time.monotonic()>deadline: raise
                    time.sleep(.03)
            if not restart:
                check(req('/api/lib/roots', {'add': str(media)}).get('ok'), 'private disc folder accepted')
                check(req('/api/lib/scan', {}).get('ok'), 'actual app disc scan accepted')
                deadline = time.monotonic()+30
                while True:
                    state = req('/api/lib/status')
                    if not state['scanning'] and state['tracks'] == 23: break
                    if time.monotonic()>deadline: raise AssertionError('scan: '+str(state))
                    time.sleep(.03)
            tracks = req('/api/lib/search?q=')['tracks']
            current = {t['path'] for t in tracks}
            check(len(tracks)==23 and len(current)==23, f'actual HTTP library has twenty-three unique tracks, restart={restart}')
            check(all(t['dur']>0 and t['sr']>0 and t['bits']>0 for t in tracks), 'actual HTTP track format metadata valid')
            if restart:
                check(current==keys, 'all area/group/track identities survive actual process restart')
                break
            keys = current
            for track in tracks:
                req('/api/stop', {}); req('/api/clear', {})
                result = req('/api/play', {'file': track['path'], 'backend':'null', 'rate':'src',
                    'volume':'-60', 'bits':'24', 'dsd':'0', 'isp':'0', 'gpuvendor':'off',
                    'metaon':'0', 'metallm':'0', 'anlmaster':'0', 'roomcorrauto':'0', 'cleanup':'0'})
                check(result.get('ok'), 'actual Null playback accepts '+track['path'].split('#')[-1])
                deadline = time.monotonic()+15
                while True:
                    status = req('/api/status')
                    if not status['playing']: break
                    if time.monotonic()>deadline: raise AssertionError('private Null playback did not finish')
                    time.sleep(.01)
                played.append({'path':track['path'], 'status':status})
                check(not status.get('error') and status.get('pos',0)>0,
                      'actual decoder renders nonzero frames without error: '+track['path'].split('#')[-1])
        finally:
            if child.poll() is None:
                try: req('/api/stop', {}); req('/api/quit', {})
                except OSError: pass
                child.wait(timeout=20)
            log.close()
            check(child.returncode == 0, 'private application exits cleanly')
    output = {'home':str(home),'checks':checks,'played':played,'packaged_decoder':str(a.decoder)}
    (home/'results.json').write_text(json.dumps(output, ensure_ascii=False, indent=2), encoding='utf8')
    print(json.dumps({'evidence':str(home/'results.json'),'checks':len(checks),'tracks_played':len(played)}), flush=True)


if __name__ == '__main__':
    main()
