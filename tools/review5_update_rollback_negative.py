"""Private real updater process: missing/locked backup must never report successful rollback."""
import argparse,ctypes,hashlib,json,os,pathlib,shutil,subprocess,sys
sys.stdout.reconfigure(encoding='utf-8',errors='replace')
p=argparse.ArgumentParser();p.add_argument('--probe',type=pathlib.Path,required=True);p.add_argument('--evidence-dir',type=pathlib.Path,required=True);a=p.parse_args();a.evidence_dir.mkdir(parents=True,exist_ok=False)
k=ctypes.WinDLL('kernel32',use_last_error=True);k.CreateFileW.argtypes=[ctypes.c_wchar_p,ctypes.c_ulong,ctypes.c_ulong,ctypes.c_void_p,ctypes.c_ulong,ctypes.c_ulong,ctypes.c_void_p];k.CreateFileW.restype=ctypes.c_void_p;k.CloseHandle.argtypes=[ctypes.c_void_p]
rows=[]
for kind in ['normal','missing-backup','locked-backup','locked-target','locked-ledger']:
 r=a.evidence_dir/kind;r.mkdir();shutil.copy2(a.probe,r/'update_probe.exe');backup=r/'updates/9.9.9/backup';backup.mkdir(parents=True);target=r/'rtaweb.exe';target.write_bytes(b'NEW-CURRENT-SENTINEL');old=b'OLD-BACKUP-SENTINEL'
 if kind!='missing-backup':(backup/'rtaweb.exe').write_bytes(old)
 protected=r/'rlplayer-settings.conf';protected.write_bytes(b'PRIVATE-FAKE-SETTINGS-NO-REAL-KEY')
 (r/'updates/pending.txt').write_text('phase=applied\nversion=9.9.9\nfrom=3.2.0\napplied=rtaweb.exe\nfile=rtaweb.exe\n',encoding='utf-8')
 handle=None
 if kind in ['locked-backup','locked-target','locked-ledger']:
  locked=backup/'rtaweb.exe' if kind=='locked-backup' else target if kind=='locked-target' else r/'updates/pending.txt'
  handle=k.CreateFileW(str(locked),0x80000000,0 if kind=='locked-backup' else 1,None,3,0x80,None)
  if handle in (None,ctypes.c_void_p(-1).value):raise RuntimeError('could not create real exclusive sharing-denial fixture')
 try:
  env={**os.environ,'RLPLAYER_UPDATE_DIR':str(r),'RTA_NO_DEVICE':'1','RTA_NET_TESTS':'0','APPDATA':str(r/'profile'),'LOCALAPPDATA':str(r/'profile')}
  result=subprocess.run([str(r/'update_probe.exe'),'--rollback','PRIVATE_PROCESS_FAILURE_INJECTION'],cwd=r,env=env,capture_output=True,timeout=15)
 finally:
  if handle is not None:k.CloseHandle(handle)
 output=(result.stdout+result.stderr).decode('utf-8',errors='replace');(r/'process.log').write_text(output,encoding='utf-8');ledger=(r/'updates/pending.txt').read_text(encoding='utf-8');phase=next((line.partition('=')[2] for line in ledger.splitlines() if line.startswith('phase=')),None)
 row=dict(case=kind,exit=result.returncode,phase=phase,target_exists=target.is_file(),target_is_old=target.is_file() and target.read_bytes()==old,target_new_preserved=target.is_file() and target.read_bytes()==b'NEW-CURRENT-SENTINEL',backup_preserved=(backup/'rtaweb.exe').is_file(),settings_preserved=protected.read_bytes()==b'PRIVATE-FAKE-SETTINGS-NO-REAL-KEY',output_log=str(r/'process.log'))
 row['criterion_pass']=(row['exit']==0 and row['phase']=='rolled_back' and row['target_is_old']) if kind=='normal' else (row['exit']!=0 and row['phase']!='rolled_back' and (row['target_new_preserved'] or row['target_is_old']) and (row['backup_preserved'] or kind=='missing-backup'))
 if kind=='locked-ledger':row['criterion_pass']=row['exit']!=0 and row['phase']=='applied' and row['target_is_old'] and row['backup_preserved'] and '状态写入失败' in output
 if kind in ['locked-backup','locked-target']:
  retry=subprocess.run([str(r/'update_probe.exe'),'--rollback','PRIVATE_RETRY_AFTER_UNLOCK'],cwd=r,env=env,capture_output=True,timeout=15);(r/'retry.log').write_bytes(retry.stdout+retry.stderr);row['retry_exit']=retry.returncode;row['retry_restored_old']=target.is_file() and target.read_bytes()==old;row['criterion_pass']=row['criterion_pass'] and row['retry_exit']==0 and row['retry_restored_old']
 rows.append(row)
(a.evidence_dir/'results.json').write_text(json.dumps(rows,indent=2),encoding='utf-8');print(json.dumps(rows,indent=2));raise SystemExit(0 if all(r['criterion_pass'] for r in rows) else 1)
