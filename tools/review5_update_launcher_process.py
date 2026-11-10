"""Actual private launcher + actual updater + synthetic child processes. No install/network/audio stream."""
import argparse,ctypes,hashlib,json,os,pathlib,shutil,socket,subprocess,sys,time
sys.stdout.reconfigure(encoding='utf-8',errors='replace')
p=argparse.ArgumentParser();p.add_argument('--launcher',type=pathlib.Path,required=True);p.add_argument('--serve',type=pathlib.Path,required=True);p.add_argument('--crash',type=pathlib.Path,required=True);p.add_argument('--loader',type=pathlib.Path,required=True);p.add_argument('--evidence-dir',type=pathlib.Path,required=True);a=p.parse_args();a.evidence_dir.mkdir(parents=True,exist_ok=False)
u=ctypes.WinDLL('user32',use_last_error=True);k=ctypes.WinDLL('kernel32',use_last_error=True);CB=ctypes.WINFUNCTYPE(ctypes.c_bool,ctypes.c_void_p,ctypes.c_void_p);u.EnumWindows.argtypes=[CB,ctypes.c_void_p];u.GetWindowThreadProcessId.argtypes=[ctypes.c_void_p,ctypes.POINTER(ctypes.c_ulong)];u.GetClassNameW.argtypes=[ctypes.c_void_p,ctypes.c_wchar_p,ctypes.c_int];u.PostMessageW.argtypes=[ctypes.c_void_p,ctypes.c_uint,ctypes.c_size_t,ctypes.c_ssize_t];k.CreateFileW.argtypes=[ctypes.c_wchar_p,ctypes.c_ulong,ctypes.c_ulong,ctypes.c_void_p,ctypes.c_ulong,ctypes.c_ulong,ctypes.c_void_p];k.CreateFileW.restype=ctypes.c_void_p;k.CloseHandle.argtypes=[ctypes.c_void_p]
def safe_text(path):
 try:return path.read_text(encoding='utf-8',errors='replace')
 except OSError:return ''
def windows(pid):
 found=[]
 @CB
 def cb(hwnd,param):
  process=ctypes.c_ulong();u.GetWindowThreadProcessId(hwnd,ctypes.byref(process))
  if process.value==pid:
   name=ctypes.create_unicode_buffer(256);u.GetClassNameW(hwnd,name,256);found.append((hwnd,name.value))
  return True
 u.EnumWindows(cb,None);return found
def digest(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def phase(root):
 try:return dict(x.partition('=')[::2] for x in (root/'updates/pending.txt').read_text(encoding='utf-8').splitlines() if '=' in x).get('phase')
 except (OSError,UnicodeError):return None
results=[]
for case in ['early-quit','normal-start','crash-restore-success','crash-missing-backup','crash-locked-backup']:
 root=a.evidence_dir/case;root.mkdir();shutil.copy2(a.launcher,root/'RLPlayer.exe');shutil.copy2(a.loader,root/'WebView2Loader.dll');shutil.copy2(a.serve,root/'rtaweb.exe');old_sha=digest(root/'rtaweb.exe');new_sha=digest(a.crash)
 private=root/'private';private.mkdir();(root/'rlplayer-settings.conf').write_bytes(b'PRIVATE-SYNTHETIC-SETTINGS\n');settings=(root/'rlplayer-settings.conf').read_bytes();backup=root/'updates/9.9.9/backup/rtaweb.exe'
 if case not in ['early-quit','normal-start']:
  stage=root/'updates/9.9.9/staging';stage.mkdir(parents=True);shutil.copy2(a.crash,stage/'rtaweb.exe');(stage/'complete.txt').write_text('file=rtaweb.exe\t'+str(a.crash.stat().st_size)+'\t'+new_sha+'\n',encoding='utf-8');(root/'updates/pending.txt').write_text('phase=ready\nversion=9.9.9\nfrom=3.2.0\nstaging='+str(stage)+'\nfile=rtaweb.exe\n',encoding='utf-8')
 with socket.socket() as so:
  for port in range(19320,19999):
   try:so.bind(('127.0.0.1',port));break
   except OSError:continue
  else:raise RuntimeError('no private QA port')
 env={**os.environ,'RLPLAYER_PORT':str(port),'RLPLAYER_UPDATE_DIR':str(root),'RLPLAYER_LIBRARY_DIR':str(private),'APPDATA':str(private),'LOCALAPPDATA':str(private),'TEMP':str(private),'TMP':str(private),'RTA_NO_DEVICE':'1','RTA_NET_TESTS':'0'}
 info=subprocess.STARTUPINFO();info.dwFlags|=subprocess.STARTF_USESHOWWINDOW;info.wShowWindow=0
 process=subprocess.Popen([str(root/'RLPlayer.exe')],cwd=root,env=env,startupinfo=info);handle=None;injected=False;close_sent=False;dialogs=0;started=time.monotonic()
 while process.poll() is None and time.monotonic()-started<35:
  if not injected and case in ['crash-missing-backup','crash-locked-backup'] and phase(root)=='applied' and backup.is_file():
   if case=='crash-missing-backup':backup.unlink();injected=True
   else:
    handle=k.CreateFileW(str(backup),0x80000000,0,None,3,0x80,None)
    if handle in (None,ctypes.c_void_p(-1).value):raise RuntimeError('backup sharing lock failed')
    injected=True
  current_log=safe_text(root/'launcher.log')
  ui_ready='已用内置 WebView2 打开界面' in current_log or '已用 Edge app 模式打开界面' in current_log
  for hwnd,cls in windows(process.pid):
   if cls=='#32770':u.PostMessageW(hwnd,0x10,0,0);dialogs+=1
   if cls=='RLPlayerTray' and not close_sent and (ui_ready or case=='early-quit'):
    # Own PID's tray exit command only. It cooperatively POSTs quit to its own child.
    u.PostMessageW(hwnd,0x111,1003,0);close_sent=True
  time.sleep(.002)
 if handle is not None:k.CloseHandle(handle)
 if process.poll() is None:raise RuntimeError('owned launcher did not exit cooperatively; no force kill attempted, PID='+str(process.pid))
 # Close only the Edge application PID recorded inside this private fixture,
 # if a WebView fallback occurred; no process termination is used.
 edge_file=root/'edge-app.pid'
 if edge_file.is_file():
  edge_pid=int(edge_file.read_text().strip())
  for hwnd,cls in windows(edge_pid):u.PostMessageW(hwnd,0x10,0,0)
 log=safe_text(root/'launcher.log');child=(root/'fixture-child.log').read_text() if (root/'fixture-child.log').is_file() else ''
 row=dict(case=case,launcher_exit=process.returncode,pending_phase=phase(root),fault_injected=injected,old_target_restored=(root/'rtaweb.exe').is_file() and digest(root/'rtaweb.exe')==old_sha,new_target_preserved=(root/'rtaweb.exe').is_file() and digest(root/'rtaweb.exe')==new_sha,backup_retained=backup.is_file(),fake_settings_preserved=(root/'rlplayer-settings.conf').read_bytes()==settings,own_tray_exit=close_sent,own_error_dialogs=dialogs,child_crash_observed='mode=crash' in child,old_child_started='mode=serve' in child,cooperative_child_quit='cooperative=1' in child,seconds=time.monotonic()-started,launcher_log=str(root/'launcher.log'))
 if case in ['early-quit','normal-start']:ok=row['launcher_exit']==0 and row['old_child_started'] and row['cooperative_child_quit']
 elif case=='crash-restore-success':ok=row['launcher_exit']==0 and row['pending_phase']=='rolled_back' and row['old_target_restored'] and row['child_crash_observed'] and row['old_child_started'] and row['cooperative_child_quit']
 else:ok=row['launcher_exit']!=0 and row['pending_phase']=='rollback_failed' and row['fault_injected'] and row['new_target_preserved'] and not row['old_child_started']
 if case=='early-quit':ok=ok and 'Edge app 模式打开界面' not in log and '初始化返回后直接收尾' in log
 row['criterion_pass']=ok and row['fake_settings_preserved'];results.append(row)
 (a.evidence_dir/'progress.json').write_text(json.dumps(results,indent=2),encoding='utf-8')
(a.evidence_dir/'results.json').write_text(json.dumps(results,indent=2),encoding='utf-8');print(json.dumps(results,indent=2));raise SystemExit(0 if all(r['criterion_pass'] for r in results) else 1)
