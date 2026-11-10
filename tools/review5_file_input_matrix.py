"""Four real-file offline routes, 3 serial trials; primary clock is complete child process wall."""
import argparse,datetime,hashlib,importlib.util,json,math,os,pathlib,statistics,struct,subprocess,time,wave
import psutil
p=argparse.ArgumentParser();p.add_argument('--exe',type=pathlib.Path,required=True);p.add_argument('--baseline',type=pathlib.Path,required=True);p.add_argument('--dsf',type=pathlib.Path,required=True);p.add_argument('--output',type=pathlib.Path,required=True);a=p.parse_args();root=a.output.resolve();root.mkdir(parents=True,exist_ok=False)
spec=importlib.util.spec_from_file_location('existing_matrix_gate',pathlib.Path(__file__).with_name('review5_efficiency_matrix.py'));gate=importlib.util.module_from_spec(spec);spec.loader.exec_module(gate)
exe=a.exe.resolve();baseline=json.loads((a.baseline/'summary.json').read_text(encoding='utf-8'));assert gate.sha(exe)==baseline['exe_sha256'],'benchmark identity differs from existing matrix';dlls={f.name:gate.sha(f) for f in exe.parent.glob('*.dll')};assert dlls==baseline['dll_sha256'],'GPU stage identity differs from existing matrix'
ids=['pcm705_gpu','pcm_dsd1024_gpu','dsd64_native1024_ec7_cpu','dsd64_remod1024_gpu'];cases=[next(c for c in baseline['cases'] if c['id']==i) for i in ids];old={r['case']['id']:r for r in baseline['rows']}
fixture=a.dsf.resolve();assert fixture.is_file() and pathlib.Path(str(fixture)+'.fixturemeta').is_file(),'existing DSF+provenance required; never generate';dsf_hash=gate.sha(fixture)
# Match production generated PCM24 rounding, explicitly outside every timed run.
wav=root/'source-44100-stereo-pcm24-30s.wav';frames=44100*30;data=bytearray(frames*6);peak=0
for i in range(frames):
 x=.001*8388608.*math.sin(2*math.pi*1000.*i/44100);v=math.floor(x+.5) if x>=0 else math.ceil(x-.5);peak=max(peak,abs(v));b=int(v).to_bytes(3,'little',signed=True);data[i*6:i*6+3]=b;data[i*6+3:i*6+6]=b
with wave.open(str(wav),'wb') as w:w.setnchannels(2);w.setsampwidth(3);w.setframerate(44100);w.writeframes(data)
del data
source_sha=gate.sha(wav);receipt=dict(status='RUNNING',schema=1,seconds=30,trials=3,cases=cases,exe_sha256=gate.sha(exe),dll_sha256=dlls,driver_sha256=gate.sha(pathlib.Path(__file__)),source_wav=dict(path=str(wav),sha256=source_sha,frames=frames,rate=44100,channels=2,bits=24,nominal_dbfs=-60,quantized_peak_integer=peak,quantized_peak_dbfs=20*math.log10(peak/8388608)),source_dsf=dict(path=str(fixture),sha256=dsf_hash,reused=True),scope='OFFLINE Null output; no device, no GUI. Primary: source duration / child-process wall measured before Popen through subprocess return, including input open/read/close, fixture integrity validation, GPU probe, process startup/shutdown and all output hashing. Inner harness/prepare/first-block reported separately, not file-I/O-only differences.',created_utc=datetime.datetime.now(datetime.timezone.utc).isoformat());(root/'plan.json').write_text(json.dumps(receipt,indent=2),encoding='utf-8')
env=os.environ.copy();env['RLPLAYER_FILTER_CACHE']='1';env['RTA_NO_DEVICE']='1';env['RTA_NET_TESTS']='0';env['PATH']=str(pathlib.Path(os.environ['SystemRoot'])/'System32');rows=[]
for trial in range(1,4):
 for c in cases if trial%2 else list(reversed(cases)):
  out=root/(c['id']+'-'+str(trial));out.mkdir();env['RLPLAYER_FILTER_CACHE_DIR']=str(root/'cache'/c['id']);flags=['--route',c['route'],'--source-rate',str(c['source']),'--target-rate',str(c['target']),'--seconds','30','--gpu',str(c['gpu']),'--preset',c['preset'],'--family',c['family'],'--order',str(c['order']),'--block','4096','--topology','auto']
  if c['route'].startswith('pcm_'):flags+=['--input-wav',str(wav)]
  else:flags+=['--fixture',str(fixture),'--decode-mode','stream']
  if c['route'] in ['pcm_dsd','dsd_decode_remod']:flags+=['--overlap','1']
  idle=psutil.cpu_percent(interval=1);before=gate.cpu_snapshot();psutil.cpu_percent(interval=None);start=time.perf_counter();r=subprocess.run([str(exe),*flags],cwd=exe.parent,env=env,capture_output=True,timeout=300);wall=time.perf_counter()-start;busy=psutil.cpu_percent(interval=None);after=gate.cpu_snapshot();(out/'stdout.json').write_bytes(r.stdout);(out/'stderr.log').write_bytes(r.stderr)
  if r.returncode:raise RuntimeError(c['id']+' exit '+str(r.returncode))
  result=json.loads(r.stdout)
  for f in ['structural_ok','count_ok','full_output_checksum']:
   if result.get(f) is not True:raise RuntimeError(c['id']+' '+f)
  for f in ['marker_errors','finite_errors','integer_24bit_range_errors','sdm_nonfinite','sdm_overload','sdm_clamped']:
   if result.get(f)!=0:raise RuntimeError(c['id']+' '+f)
  execution=gate.verify_execution(result,c['gpu']);external=[dict(pid=pid,name=name,machine_percent=100*(cpu-before[pid][1])/wall/psutil.cpu_count()) for pid,(name,cpu) in after.items() if pid in before and pid!=os.getpid() and cpu>before[pid][1]];external.sort(key=lambda x:-x['machine_percent'])
  row=dict(case=c['id'],trial=trial,process_wall_seconds=wall,process_realtime_factor=30/wall,pre_run_machine_busy_percent=idle,machine_busy_percent_during=busy,external_cpu=external[:12],execution=execution,command=[str(exe),*flags],result=result,baseline_complete_hash_equal=result['full_output_sha256']==old[c['id']]['full_output_sha256']);rows.append(row);(out/'observation.json').write_text(json.dumps(row,indent=2),encoding='utf-8');(root/'trials.json').write_text(json.dumps(rows,indent=2),encoding='utf-8');print(c['id'],trial,'outer_RT',30/wall,'inner_RT',result['end_to_end_realtime_factor'],'baseline_fullhash',row['baseline_complete_hash_equal'],flush=True)
summary=[]
for c in cases:
 r=[x for x in rows if x['case']==c['id']];hashes={x['result']['full_output_sha256'] for x in r}
 if len(hashes)!=1:raise RuntimeError(c['id']+' complete outputs differ between trials')
 fields=['harness_wall_s','first_pipeline_prepare_ms','first_block_wall_s','steady_wall_s','eof_wall_s','end_to_end_realtime_factor','input_frames','input_bits','output_frames','expected_output_frames','peak_working_set_bytes']
 summary.append(dict(case=c['id'],trials=len(r),full_output_sha256=next(iter(hashes)),all_baseline_complete_hash_equal=all(x['baseline_complete_hash_equal'] for x in r),outer_wall_median=statistics.median(x['process_wall_seconds'] for x in r),outer_realtime_median=statistics.median(x['process_realtime_factor'] for x in r),outer_realtime_min=min(x['process_realtime_factor'] for x in r),outer_realtime_max=max(x['process_realtime_factor'] for x in r),inner_median={f:statistics.median(x['result'][f] for x in r) for f in fields},baseline_inner_median=old[c['id']]['median']))
assert gate.sha(wav)==source_sha and gate.sha(fixture)==dsf_hash,'source fixture modified';assert gate.sha(exe)==receipt['exe_sha256'] and {f.name:gate.sha(f) for f in exe.parent.glob('*.dll')}==dlls,'benchmark stage modified'
receipt.update(status='PASS',rows=summary,runs=len(rows),finished_utc=datetime.datetime.now(datetime.timezone.utc).isoformat());(root/'summary.json').write_text(json.dumps(receipt,indent=2),encoding='utf-8');print('PASS real-file offline matrix cases=4 runs=12 device_calls=0')
