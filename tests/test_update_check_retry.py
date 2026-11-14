"""Completed update errors are cached without becoming an up-to-date result.

The real HTTP/API cases use only private empty libraries, Null/-60 settings and
loopback responses. No Play, download, GUI, credential or production access.
The extracted start_check probe adds a deterministic clock for the five-minute
boundary; it is supplemental to the real application/API regression.
The rollback fixture is the actually published 3.2.2 signed metadata, not a signing key. Rollback is
checked before expiry by the production parser, so its date does not age this case.
It is the original canonical Windows /latest response (383341 bytes), SHA256
e226d57b0559d0bf208a840daf2268631d13393070aa2ddf9bd5c4be061a1ba1.
Its manual-download retrieval code is public release information, not a credential.
"""
from __future__ import annotations
import argparse
import hashlib
import http.server
import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
import tempfile
import threading
import time
import urllib.request

SOURCE = Path(__file__).resolve().parents[1]
COMPILER = 'C:/Program Files/AMD/ROCm/7.2/bin/clang++.exe'


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2), encoding='utf8')


def function_text(text, marker):
    begin = text.index(marker)
    pos = text.index('{', begin) + 1
    depth = 1
    while depth:
        depth += (text[pos] == '{') - (text[pos] == '}')
        pos += 1
    return text[begin:pos]


def source_probe(root, compiler):
    header = (SOURCE/'src/net/update.h').read_text(encoding='utf8')
    actual = function_text(header, 'inline bool start_check(bool force)')
    code = r'''
#include <atomic>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
struct Result { bool good=false; bool ok()const{return good;} };
struct State { std::mutex mtx; bool checking=false,hasChecked=false; Result cr;
 unsigned long long checkedMs=0;std::string phase="idle"; } shared;
State& state(){return shared;}constexpr unsigned long long kCheckTtlMs=300000;
std::atomic<unsigned long long> clockMs{1000};
unsigned long long now_ms(){return clockMs.load();}
std::atomic<int> requests{0};std::mutex gateMx;std::condition_variable gate;
bool held=false,success=false;
void check_worker(){++requests;{std::unique_lock<std::mutex> l(gateMx);gate.wait(l,[]{return !held;});}
 std::lock_guard<std::mutex> l(shared.mtx);shared.cr.good=success;shared.hasChecked=true;
 shared.checkedMs=now_ms();shared.checking=false;}
'''+actual+r'''
void finish(){for(;;){bool active;{std::lock_guard<std::mutex> l(shared.mtx);active=shared.checking;}
 if(!active)break;std::this_thread::yield();}}
void reset(bool good){finish();success=good;requests=0;clockMs=1000;
 std::lock_guard<std::mutex> l(shared.mtx);shared.hasChecked=false;shared.cr.good=false;}
int failures=0;
void check(bool ok,const char*name,int actual){std::cout<<(ok?"PASS ":"FAIL ")<<name<<" actual="<<actual<<"\n";failures+=!ok;}
int main(){
 for(bool good:{false,true}){reset(good);for(int i=0;i<20;++i){start_check(false);finish();}
  check(requests==1,good?"success twenty polls use one request":"error twenty polls use one request",requests);
  check(shared.cr.good==good,"cache preserves original decision",shared.cr.good);
  start_check(true);finish();check(requests==2,"force bypasses completed-result cache",requests);}
 reset(false);start_check(false);finish();clockMs=300999;start_check(false);finish();
 check(requests==1,"error is cached through TTL minus one millisecond",requests);
 clockMs=301000;start_check(false);finish();check(requests==2,"error rechecks at exact TTL",requests);
 reset(true);start_check(false);finish();clockMs=301000;start_check(false);finish();
 check(requests==2,"success also rechecks at exact TTL",requests);
 reset(false);{std::lock_guard<std::mutex> l(gateMx);held=true;}start_check(false);
 while(requests<1)std::this_thread::yield();
 for(int i=0;i<20;++i){start_check(false);start_check(true);}
 check(requests==1,"inflight normal and force remain single flight",requests);
 {std::lock_guard<std::mutex> l(gateMx);held=false;}gate.notify_all();finish();
 return failures?1:0;
}
'''
    cpp, exe = root/'start-check-extracted.cpp', root/'start-check-extracted.exe'
    cpp.write_text(code, encoding='utf8')
    build = subprocess.run([compiler, '-std=c++23', str(cpp), '-o', str(exe)], capture_output=True, timeout=90)
    (root/'source-build.log').write_bytes(build.stdout+build.stderr)
    build.check_returncode()
    run = subprocess.run([str(exe)], capture_output=True, timeout=20)
    (root/'source-run.log').write_bytes(run.stdout+run.stderr)
    result = {'pass': run.returncode == 0, 'exit': run.returncode,
              'header_sha256': hashlib.sha256(header.encode()).hexdigest(),
              'function_sha256': hashlib.sha256(actual.encode()).hexdigest(),
              'output': run.stdout.decode('utf8', errors='replace')}
    write_json(root/'source-results.json', result)
    print(json.dumps(result), flush=True)
    return result


def api(base, route, post=False):
    request = urllib.request.Request(base+route, data=b'{}' if post else None,
                                     headers={'Origin': base, 'Content-Type': 'application/json'})
    with urllib.request.urlopen(request, timeout=15) as response:
        return json.load(response)


def owns_port(port, pid):
    # An ephemeral-port race must never let a test inspect/quit another instance.
    netstat = Path(os.environ['SystemRoot'])/'System32/netstat.exe'
    result = subprocess.run([str(netstat), '-ano', '-p', 'tcp'], capture_output=True,
                            text=True, errors='replace', timeout=10)
    result.check_returncode()
    for line in result.stdout.splitlines():
        fields = line.split()
        if (len(fields) >= 5 and fields[0] == 'TCP'
                and fields[1].rsplit(':', 1)[-1] == str(port)
                and fields[2].endswith(':0') and fields[-1] == str(pid)):
            return True
    return False


def integration(root, executable, case, poll_interval):
    home = root/case
    home.mkdir()
    shutil.copy2(executable, home/'rtaweb.exe')
    empty, profile = home/'empty', home/'profile'
    empty.mkdir(); profile.mkdir()
    (home/'rlplayer-library.conf').write_text(str(empty)+'\n', encoding='utf8')
    (home/'rlplayer-settings.conf').write_text(
        'backend=null\nvolume=-60\nmetaon=0\nmetallm=0\nanlmaster=0\n'
        'roomcorrauto=0\ncleanup=0\nisp=0\ngpuvendor=off\n', encoding='utf8')
    (home/'rlplayer-llm.conf').write_text('on=0\n', encoding='utf8')
    (home/'rlplayer-playlist.txt').write_text('# RLPlayer playlist v1\nindex=0\nrepeat=0\nshuffle=0\n', encoding='utf8')
    body = (SOURCE/'tests/fixtures/update_check_retry_rollback_3_2_2.json').read_bytes() if case == 'rollback' else b'{"ok":false,"error":"private unavailable response"}'
    expected = 'rollback' if case == 'rollback' else 'http'
    rows, replies = [], []
    lock, release = threading.Lock(), threading.Event()
    response_done = threading.Event()
    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self, *unused): pass
        def do_GET(self):
            if self.path != '/api/v1/update/windows/latest':
                self.send_error(404); return
            with lock:
                replies.append({'path': self.path, 'status': 200 if case == 'rollback' else 503})
            if not release.wait(12):
                self.send_error(504); return
            self.send_response(200 if case == 'rollback' else 503)
            self.send_header('Content-Type', 'application/json')
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            response_done.set()
    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    with socket.socket() as sock:
        sock.bind(('127.0.0.1', 0)); port = sock.getsockname()[1]
    base = f'http://127.0.0.1:{port}'
    environment = {**os.environ, 'RTA_NO_DEVICE': '1', 'RTA_NET_TESTS': '0',
        'RLPLAYER_LIBRARY_DIR': str(home), 'RLPLAYER_UPDATE_DIR': str(home),
        'RLPLAYER_UPDATE_ALLOW_LOCAL': '1',
        'RLPLAYER_UPDATE_MANIFEST': f'http://127.0.0.1:{server.server_port}/api/v1/update/windows/latest',
        'APPDATA': str(profile), 'LOCALAPPDATA': str(profile), 'USERPROFILE': str(profile),
        'TEMP': str(profile), 'TMP': str(profile)}
    def count():
        with lock: return len(replies)
    def wait_count(want):
        deadline = time.monotonic()+5
        while count() < want:
            if time.monotonic() > deadline: raise AssertionError('external GET did not start')
            time.sleep(.01)
    def check(ok, label, detail):
        rows.append({'pass': bool(ok), 'label': label, 'detail': detail})
        write_json(home/'checks.json', rows)
        print(json.dumps({'case': case, **rows[-1]}), flush=True)
    def error_preserved(answer):
        decision = 'http-error' if expected == 'http' else expected
        return answer.get('ok') is False and answer.get('decision') == decision and answer.get('errorCode') == expected and bool(answer.get('error')) and answer.get('upToDate') is False and answer.get('hasUpdate') is False
    with (home/'backend.log').open('wb') as log:
        child = subprocess.Popen([str(home/'rtaweb.exe'), '--port', str(port)], cwd=home,
                                 env=environment, stdout=log, stderr=log,
                                 creationflags=subprocess.CREATE_NO_WINDOW)
        try:
            deadline = time.monotonic()+30
            while True:
                try:
                    if not owns_port(port, child.pid): raise OSError('owned listener not ready')
                    api(base, '/api/ping'); break
                except OSError:
                    if child.poll() is not None or time.monotonic() > deadline:
                        raise RuntimeError('private backend did not listen')
                    time.sleep(.03)
            first = api(base, '/api/update/check'); wait_count(1)
            inflight = [api(base, '/api/update/check'+('?force=1' if n % 2 else '')) for n in range(20)]
            check(first.get('checking') and count() == 1 and all(j.get('checking') for j in inflight),
                  'normal and force polls share one inflight request', {'gets': count()})
            release.set()
            if not response_done.wait(5): raise AssertionError('private response did not finish')
            time.sleep(.2)
            polls = []
            for _ in range(20):
                polls.append(api(base, '/api/update/check'))
                time.sleep(poll_interval)
            check(count() == 1, 'twenty completed-error UI polls add no external GET', {'gets': count()})
            check(all(error_preserved(j) and not j.get('checking') and j.get('cached') for j in polls),
                  'cached errors remain errors, never up-to-date', polls[-1])
            # Hold the explicit retry response so forced concurrent polls are
            # observable without timing guesses about the network worker.
            release.clear(); response_done.clear(); before = count()
            api(base, '/api/update/check?force=1'); wait_count(before+1)
            retry = [api(base, '/api/update/check?force=1') for _ in range(20)]
            check(count() == before+1 and all(j.get('checking') for j in retry),
                  'explicit force retries once and remains single flight', {'before': before, 'gets': count()})
            release.set()
            if not response_done.wait(5): raise AssertionError('private forced response did not finish')
            time.sleep(.2)
            final = api(base, '/api/update/check')
            check(count() == before+1 and error_preserved(final) and not final.get('checking'),
                  'forced retry result is cached with original error', {'gets': count(), 'check': final})
            state, queue = api(base, '/api/status'), api(base, '/api/queue')
            check(not state.get('playing') and state.get('pos', 0) == 0 and queue.get('items') == [],
                  'empty queue and zero playback throughout', {'playing': state.get('playing'), 'pos': state.get('pos'), 'items': queue.get('items')})
        finally:
            release.set()
            try:
                if child.poll() is None:
                    if not owns_port(port, child.pid):
                        raise RuntimeError(f'port ownership lost for private PID {child.pid}; no quit/kill sent')
                    api(base, '/api/quit', post=True)
                    try: child.wait(30)
                    except subprocess.TimeoutExpired:
                        raise RuntimeError(f'owned PID {child.pid} did not quit; no kill attempted')
            finally:
                server.shutdown(); server.server_close(); thread.join(5)
    check(child.returncode == 0, 'private backend exits normally', child.returncode)
    result = {'case': case, 'pass': all(r['pass'] for r in rows), 'checks': rows,
              'external_gets': count(), 'responses': replies, 'backend_exit': child.returncode,
              'audio_play_requested': False, 'download_requested': False}
    write_json(home/'results.json', result)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exe', type=Path)
    parser.add_argument('--source-only', action='store_true')
    parser.add_argument('--compiler', default=COMPILER)
    parser.add_argument('--evidence-dir', type=Path)
    parser.add_argument('--poll-interval', type=float, default=.7)
    args = parser.parse_args()
    if not args.source_only and (not args.exe or not args.exe.is_file()):
        parser.error('--exe is required unless --source-only')
    root = (args.evidence_dir or Path(tempfile.mkdtemp(prefix='update-check-retry-'))).resolve()
    root.mkdir(parents=True, exist_ok=True)
    if any(root.iterdir()): parser.error('--evidence-dir must be fresh and empty')
    result = {'source': source_probe(root, args.compiler), 'integration': []}
    if not args.source_only:
        for case in ('http503', 'rollback'):
            result['integration'].append(integration(root, args.exe.resolve(), case, args.poll_interval))
    result['pass'] = result['source']['pass'] and all(c['pass'] for c in result['integration'])
    write_json(root/'results.json', result)
    print('RESULTS='+str(root/'results.json'), flush=True)
    return 0 if result['pass'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
