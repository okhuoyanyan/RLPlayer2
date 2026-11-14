"""Explicitly authorized two-case4GiB supplement. Serial, no process killing."""
import argparse,json,hashlib,ctypes,subprocess,time
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--exe',type=Path,required=True);p.add_argument('--output',type=Path,required=True);a=p.parse_args();a.output.mkdir(parents=True,exist_ok=True)
class Memory(ctypes.Structure):_fields_=[('length',ctypes.c_ulong),('load',ctypes.c_ulong),('total',ctypes.c_ulonglong),('available',ctypes.c_ulonglong),('page_total',ctypes.c_ulonglong),('page_available',ctypes.c_ulonglong),('virtual_total',ctypes.c_ulonglong),('virtual_available',ctypes.c_ulonglong),('extended',ctypes.c_ulonglong)]
def available():
 x=Memory();x.length=ctypes.sizeof(x);assert ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(x));return x.available
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
rows=[];start=time.perf_counter()
for name in ['poly-sinc-gauss-hires-mp','poly-sinc-gauss-hires-ip']:
 mem=available();assert mem>=12*1024**3,('less than12GiB available; do not launch',mem);d=a.output/name;d.mkdir(exist_ok=True);cmd=[str(a.exe),name,'96000','44100',str(d),str(4*1024**3)];t=time.perf_counter();print(name,'available',mem,'start',flush=True)
 with (d/'stdout.log').open('w') as out,(d/'stderr.log').open('w') as err:
  process=subprocess.Popen(cmd,stdout=out,stderr=err)
  while process.poll() is None:
   if time.perf_counter()-t>600:
    # No force kill: record noncompletion and stop launching more work.
    (a.output/'timeout.json').write_text(json.dumps({'status':'TIMEOUT_RUNNING_NOT_KILLED','pid':process.pid,'seconds':time.perf_counter()-t,'command':cmd},indent=2));raise RuntimeError('600s exceeded; child not killed, no PASS, notify root')
   time.sleep(.1)
  code=process.returncode
 file=d/'row.json';r=json.loads(file.read_text());r.update(exit_code=code,wall_seconds=time.perf_counter()-t,command=cmd,folder=str(d),available_physical_before_bytes=mem,available_physical_after_bytes=available(),supplemental_authorization='two specified admission rows only;4GiB tool budget, serial,>=12GiB available before launch,600s monitoring; original product maxPrototype unchanged',exe_sha256=sha(a.exe))
 for s in r.get('stages',[]):
  s['double_sha256']=sha(d/s['double_file'])
  if s.get('fixed_file'):s['fixed_sha256']=sha(d/s['fixed_file'])
 if 'points' in r:
  r['pass_point_max_abs_db']=max(abs(x['desired_response_db']) for x in r['points'] if x['kind']=='pass');r['mirror_stop_point_max_db']=max(x['desired_response_db'] for x in r['points'] if x['kind']=='mirror_stop');v=max(r['pass_point_max_abs_db']-.01,r['mirror_stop_point_max_db']+r['declared_stop_target_db']);r['point_quality_status']='POINT_THRESHOLD_FAILURE' if v>1e-6 else 'INDETERMINATE_NEAR_POINT_THRESHOLD' if v>-1e-6 else 'POINTS_NO_FAILURE_FOUND_NOT_CONTINUOUS'
 else:r['point_quality_status']='NOT_MEASURED_NO_NUMERIC_PASS'
 assert code in [0,3],('tool error not accepted',code);file.write_text(json.dumps(r,indent=2));rows.append(r);(a.output/'supplement.json').write_text(json.dumps({'status':'COMPLETE' if len(rows)==2 else 'IN_PROGRESS','original_manifest':'D:/RLPlayer-Develop-20261103/evidence/dsp/pcm1343-manifest/manifest.json','original_rows_unchanged':True,'elapsed_seconds':time.perf_counter()-start,'rows':rows},indent=2));print(name,r['status'],r['point_quality_status'],r['wall_seconds'],r.get('peak_working_set_bytes'),flush=True)
