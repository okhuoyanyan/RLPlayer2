"""Explicit opt-in integration check against an isolated, non-playing runtime.

Usage: python test_app_http_persistence.py PORT RUNTIME_DIRECTORY
Never starts playback. Refuses a runtime outside RLPlayer-Develop-20261103.
"""
import concurrent.futures
import ctypes
import json
import pathlib
import sys
import urllib.error
import urllib.parse
import urllib.request

port=int(sys.argv[1]);runtime=pathlib.Path(sys.argv[2]).resolve()
allowed=pathlib.Path('D:/RLPlayer-Develop-20261103').resolve()
if not runtime.is_relative_to(allowed) or not runtime.name.startswith('runtime-app-'):
    raise SystemExit('Explicit isolated application runtime required')
base=f'http://127.0.0.1:{port}'
def request(route,values=None,headers=None):
    data=None if values is None else urllib.parse.urlencode(values).encode()
    with urllib.request.urlopen(urllib.request.Request(base+route,data=data,headers=headers or {}),timeout=5) as r:
        return json.load(r)
assert request('/api/status')['playing'] is False
checks=0
def check(ok,label):
    global checks
    checks+=1
    print(('PASS ' if ok else 'FAIL ')+label,flush=True)
    if not ok:raise AssertionError(label)
try:request('/api/quit')
except urllib.error.HTTPError as e:check(e.code==405,'GET quit is rejected by actual backend method guard')
else:raise AssertionError('GET quit unexpectedly accepted')
check(request('/api/status')['playing'] is False,'rejected request leaves isolated server healthy and idle')
request('/api/settings',{'backend':'null','volume':'-60'})
before=(runtime/'rlplayer-settings.conf').read_bytes()
try:request('/api/settings',{'theme':'light'},{'Origin':'http://attacker.example.invalid'})
except urllib.error.HTTPError as e:check(e.code==403,'foreign browser Origin is rejected by the real settings endpoint')
else:raise AssertionError('Foreign Origin unexpectedly accepted')
check((runtime/'rlplayer-settings.conf').read_bytes()==before,'rejected browser Origin cannot modify settings bytes')
check(request('/api/settings',{'theme':'dark'},{'Origin':base}).get('ok') is True,'same-origin browser settings POST remains allowed')
for _ in range(10):
    with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
        responses=list(pool.map(lambda body:request('/api/settings',body),[{'theme':'dark'},{'uilang':'es'}]))
    check(all(r.get('ok') for r in responses),'parallel partial POSTs succeed')
    kv=request('/api/settings')['kv']
    check(kv.get('backend')=='null' and kv.get('volume')=='-60' and kv.get('theme')=='dark' and kv.get('uilang')=='es','parallel partial updates retain safe backend, volume and both keys')
target=runtime/'rlplayer-settings.conf';old=target.read_bytes()
kernel=ctypes.WinDLL('kernel32',use_last_error=True)
kernel.CreateFileW.argtypes=[ctypes.c_wchar_p,ctypes.c_ulong,ctypes.c_ulong,ctypes.c_void_p,ctypes.c_ulong,ctypes.c_ulong,ctypes.c_void_p]
kernel.CreateFileW.restype=ctypes.c_void_p
kernel.CloseHandle.argtypes=[ctypes.c_void_p]
handle=kernel.CreateFileW(str(target),0x80000000,3,None,3,0,None)
if handle in (None,ctypes.c_void_p(-1).value):raise ctypes.WinError(ctypes.get_last_error())
try:
    result=request('/api/settings',{'theme':'light'})
    check(result.get('ok') is False,'actual HTTP endpoint reports replacement lock failure')
    check(target.read_bytes()==old,'actual endpoint preserves byte-for-byte settings under replacement lock')
finally:kernel.CloseHandle(handle)
check(request('/api/settings',{'theme':'dark'}).get('ok') is True,'actual endpoint recovers after releasing lock')
check(request('/api/status')['playing'] is False,'integration suite never starts playback')
print(json.dumps({'checks':checks,'failures':0,'parallel_rounds':10}))
