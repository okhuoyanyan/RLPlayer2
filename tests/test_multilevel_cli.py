"""Actual offline exporter + independent decoder: DC, 48 full-band cells, failures."""
import argparse,ctypes,importlib.util,json,math,os,struct,subprocess,tempfile,wave
from pathlib import Path
import numpy as np
p=argparse.ArgumentParser();p.add_argument('--exe',required=True,type=Path);p.add_argument('--evidence-dir',type=Path);a=p.parse_args()
reader=Path(__file__).resolve().parents[1]/'tools/review5_multilevel_decode.py';spec=importlib.util.spec_from_file_location('rlmultilevel_independent',reader);decoder=importlib.util.module_from_spec(spec);spec.loader.exec_module(decoder)
owned=None
if a.evidence_dir:root=a.evidence_dir;root.mkdir(parents=True,exist_ok=False)
else:owned=tempfile.TemporaryDirectory(prefix='rlplayer-multilevel-cli-');root=Path(owned.name)
checks=0;quality_rows=[];commands=[]
def ck(ok,label):
 global checks
 checks+=1
 if not ok:raise RuntimeError(label)
def float_wav(path,rate,values):
 values=np.asarray(values,dtype='<f4');channels=1 if values.ndim==1 else values.shape[1];payload=values.tobytes();fmt=struct.pack('<HHIIHH',3,channels,rate,rate*channels*4,channels*4,32)
 path.write_bytes(b'RIFF'+struct.pack('<I',36+len(payload))+b'WAVEfmt '+struct.pack('<I',16)+fmt+b'data'+struct.pack('<I',len(payload))+payload)
def execute(name,input,bits,extra=(),expected=0):
 out=root/(name+'.rlmultilevel');report=root/(name+'.json');args=[str(a.exe),str(input),'--out',str(out),'--bits',str(bits),'--report',str(report),*map(str,extra)]
 r=subprocess.run(args,cwd=root,capture_output=True,text=True,encoding='utf-8',errors='replace',timeout=120);(root/(name+'.log')).write_text(r.stdout+r.stderr,encoding='utf-8');commands.append(dict(name=name,exit=r.returncode,expected=expected))
 ck(r.returncode==expected,f'{name}: exit {r.returncode}, expected {expected}: {r.stderr}')
 return out,json.loads(report.read_text(encoding='utf-8')) if expected==0 else None

def analyze(samples,rate,frequency,expected_rms):
 n=len(samples);t=np.arange(n,dtype=np.float64)/rate;window=.5-.5*np.cos(2*np.pi*np.arange(n)/n)
 rms=math.sqrt(2)*abs(np.sum(samples*window*np.exp(-2j*np.pi*frequency*t)))/window.sum()
 gain=20*math.log10(rms/expected_rms) if rms else -999.
 spectrum=np.fft.rfft(samples*window);frequencies=np.fft.rfftfreq(n,1/rate);mask=frequencies<=20000;center=int(round(frequency*n/rate));mask[max(0,center-1):center+2]=False
 power=2*np.abs(spectrum)**2/(n*n*.375);power[0]*=.5;noise=math.sqrt(float(np.sum(power[mask])));residual=20*math.log10(noise/expected_rms) if noise else -999.
 return dict(coherent_rms=rms,gain_db=gain,audio_20k_residual_db_relative=residual)
def acceptable(m):return abs(m['gain_db'])<=.02 and m['audio_20k_residual_db_relative']<=-60
# Effective audio predicate counterexamples, separate from binary validity.
rate=2822400;n=int(rate*.04);t=np.arange(n)/rate;pure=.01*np.sin(2*np.pi*10000*t);reference=.01/math.sqrt(2)
ck(acceptable(analyze(pure,rate,10000,reference)),'known-good tone must pass audio criterion')
ck(not acceptable(analyze(.5*pure,rate,10000,reference)),'half-gain negative control must fail')
ck(not acceptable(analyze(pure+.001*np.sin(2*np.pi*19000*t),rate,10000,reference)),'added audio spur negative control must fail')
ck(not acceptable(analyze(np.ones(n),rate,10000,reference)),'locked DC negative control must fail')
ck(not acceptable(analyze(np.where(pure>=0,1.,-1.),rate,10000,reference)),'sign-collapse negative control must fail')
# Real multilevel +/-0.2 DC in independent channels, not just legal code widths.
for source_rate in [44100,48000]:
 source=root/f'dc-{source_rate}.wav';count=int(source_rate*.03)
 with wave.open(str(source),'wb') as f:f.setnchannels(2);f.setsampwidth(2);f.setframerate(source_rate);f.writeframes(struct.pack('<hh',6554,-6554)*count)
 for bits in [6,8]:
  name=f'dc-{source_rate}-{bits}';path,report=execute(name,source,bits);decoded=decoder.decode(path);samples=np.asarray(decoded['samples']).reshape(-1,2);valid=decoded['source_valid_output_frames'];middle=samples[valid//4:3*valid//4];means=middle.mean(axis=0)
  ck(abs(means[0]-6554/32768)<.0003 and abs(means[1]+6554/32768)<.0003,'DC means cannot collapse to +/-1')
  ck(decoded['effective_levels']==1<<bits and not decoded['metadata']['one_bit_output'],'genuine requested multilevel configuration')
  ck(decoded['frames']==valid+decoded['pipeline_tail_frames'] and decoded['pipeline_tail_frames']>0,'actual full causal tail ledger')
  ck(report['source_frames']==count and report['source_duration_seconds']==count/source_rate,'source duration is the unique realtime numerator')
  ck(abs(report['source_realtime_ratio']-report['source_duration_seconds']/report['whole_complete_seconds'])<1e-12,'realtime ratio excludes generated tail duration')
  ck(report['cold_prepare_ms']>=0 and report['whole_complete_seconds']>0 and report['nonfinite']==0,'actual preparation/whole completion/counter report')
  ck(len(set(decoded['payload']))>2,'true 6/8-bit code grid, not two endpoint codes')
# Full-band AHM export quality at actual Pipeline output: both base rates,
# both bit depths, requested 5/7, .01/.62, and 1/10/19 kHz.
for source_rate in [44100,48000]:
 for frequency in [1000,10000,19000]:
  for amplitude in [.01,.62]:
   count=int(source_rate*.05);source=root/f'tone-{source_rate}-{frequency}-{amplitude}.wav';signal=(amplitude*np.sin(2*np.pi*frequency*np.arange(count)/source_rate)).astype('<f4');float_wav(source,source_rate,signal)
   for bits in [6,8]:
    for order in [5,7]:
     name=f'tone-{source_rate}-{frequency}-{amplitude}-{bits}-{order}';path,report=execute(name,source,bits,['--order',order]);decoded=decoder.decode(path);samples=np.asarray(decoded['samples']);rate=decoded['rate'];valid=decoded['source_valid_output_frames'];margin=int(round(rate*.005));length=int(round(rate*.04))
     ck(decoded['channels']==1 and decoded['metadata']['source_float'],'float WAV precision survives actual Pipeline doubleOut path')
     ck(decoded['pipeline_tail_frames']/rate<.005,'fixture middle region lies beyond full FIR transient; never shrink quality window silently')
     ck(margin+length<=valid,'complete steady source region exists')
     # Source and output windows have exactly 40/400/760 coherent cycles.
     source_middle=signal[int(source_rate*.005):int(source_rate*.005)+int(source_rate*.04)].astype(np.float64)
     ref=analyze(source_middle,source_rate,frequency,amplitude/math.sqrt(2))['coherent_rms']
     measured=analyze(samples[margin:margin+length],rate,frequency,ref)
     row=dict(name=name,source_rate=source_rate,rate=rate,frequency=frequency,amplitude=amplitude,bits=bits,requested_order=order,effective_order=decoded['effective_order'],effective_levels=decoded['effective_levels'],cutoff_hz=decoded['metadata']['cutoff_hz'],**measured);quality_rows.append(row)
     (root/'quality-progress.json').write_text(json.dumps(quality_rows,indent=2),encoding='utf-8')
     ck(acceptable(measured),f'audio quality failed without threshold relaxation: {row}')
     ck(report['overload']==0 and report['clamped']==0 and report['nonfinite']==0,'no hidden repair in ordinary audio export')
     print('PASS',name,'gain_db='+str(measured['gain_db']),'residual_db='+str(measured['audio_20k_residual_db_relative']),flush=True)
# Real high-rate functional cells; long realtime benchmarks are explicitly separate.
for source_rate in [44100,48000]:
 source=root/f'high-{source_rate}.wav';count=int(source_rate*.003);float_wav(source,source_rate,np.full((count,2),[.2,-.2],dtype='<f4'))
 for bits in [6,8]:
  path,report=execute(f'high-{source_rate}-{bits}',source,bits,['--rate',source_rate*1024]);decoded=decoder.decode(path)
  ck(decoded['rate']==source_rate*1024 and decoded['effective_levels']==1<<bits and decoded['effective_order']==7,'high-rate export retains true multilevel seventh order')
  ck(report['sections']==4 and report['source_frames']==count,'actual high-rate sections and source frame count are reported')
# Cancelled/failed work cannot reuse an existing completed-looking target.
source=root/'dc-44100.wav';old=b'old-destination-bytes'
for name,extra in [('cancelled',['--cancel-after-source-frames','1']),('invalid-bits',['--bits','7']),('invalid-cutoff',['--cutoff','nan'])]:
 target=root/(name+'.rlmultilevel');target.write_bytes(old);execute(name,source,6,extra,130 if name=='cancelled' else 2);ck(target.read_bytes()==old,'failure preserves old target every byte')
bad=root/'bad.wav';bad.write_bytes(b'RIFF broken input');target=root/'bad-input.rlmultilevel';target.write_bytes(old);execute('bad-input',bad,6,expected=2);ck(target.read_bytes()==old,'bad WAV does not publish a fake complete file')
nonfinite=root/'nonfinite.wav';float_wav(nonfinite,44100,np.array([float('nan')],dtype='<f4'));execute('nonfinite-input',nonfinite,6,expected=2);ck(not (root/'nonfinite-input.rlmultilevel').exists(),'nonfinite source refused before publication')
late=root/'late-nonfinite.wav';v=np.zeros(2048,dtype='<f4');v[-1]=np.nan;float_wav(late,44100,v)
late_out=root/'late-nonfinite.rlmultilevel';late_out.write_bytes(old);execute('late-nonfinite',late,6,expected=2);ck(late_out.read_bytes()==old,'late bad sample after actual streamed DSP preserves old target')
# Sharing denial proves the CLI uses atomic replacement after actual DSP work.
held=root/'sharing-denial.rlmultilevel';held.write_bytes(old);kernel=ctypes.WinDLL('kernel32',use_last_error=True);kernel.CreateFileW.argtypes=[ctypes.c_wchar_p,ctypes.c_ulong,ctypes.c_ulong,ctypes.c_void_p,ctypes.c_ulong,ctypes.c_ulong,ctypes.c_void_p];kernel.CreateFileW.restype=ctypes.c_void_p;kernel.CloseHandle.argtypes=[ctypes.c_void_p];h=kernel.CreateFileW(str(held),0x80000000,3,None,3,0x80,None)
ck(h not in [None,ctypes.c_void_p(-1).value],'sharing-denial fixture handle valid')
try:execute('sharing-denial',source,6,expected=2)
finally:kernel.CloseHandle(h)
ck(held.read_bytes()==old,'failed atomic CLI replacement preserves old complete target')
# Native wide Windows arguments, including Chinese, spaces and a non-ACP emoji.
unicode_dir=root/'含中文 空格😊';unicode_dir.mkdir();unicode_source=unicode_dir/'源文件 立体声.wav';float_wav(unicode_source,44100,np.full((1323,2),[.2,-.2],dtype='<f4'))
path,report=execute('含中文 空格😊/真实 导出',unicode_source,8);decoded=decoder.decode(path);ck(decoded['source_frames']==1323 and decoded['channels']==2,'native UTF-16 argv is correctly converted to UTF-8 paths')
ck(report['riff_max_file_bytes']==4294967303 and report['source_file_bytes']==unicode_source.stat().st_size,'native RIFF boundary is reported')
# Streaming source chunk boundaries must not alter any emitted sample index.
p1,r1=execute('stream-chunk-1',unicode_source,8,['--chunk','1'])
p2,r2=execute('stream-chunk-4096',unicode_source,8,['--chunk','4096'])
d1=decoder.decode(p1);d2=decoder.decode(p2)
ck(d1['payload']==d2['payload'] and d1['frames']==d2['frames'],'source block size preserves every multilevel code index and tail ledger')
# Source/rate/size extremes fail before unbounded allocations or publication.
huge=root/'over-memory-budget.wav'
with huge.open('wb') as f:f.seek(64*1024*1024);f.write(b'x')
execute('memory-budget',huge,8,expected=2);ck(not (root/'memory-budget.rlmultilevel').exists(),'large invalid RIFF refused without whole-file read')
low=root/'too-low-rate.wav';float_wav(low,1000,np.zeros(4,dtype='<f4'));execute('source-rate-boundary',low,8,expected=2)
execute('output-rate-boundary',source,8,['--rate','4294967295'],2)
execute('zero-chunk',source,8,['--chunk','0'],2)
# Deterministic cancellation is cooperative: the design scope, work loop and
# final checksum/atomic publication all observe the same cancellation token.
timed=root/'timed-cancel.rlmultilevel';timed.write_bytes(old);execute('timed-cancel',source,6,['--cancel-after-ms','0'],130);ck(timed.read_bytes()==old,'cooperative timed cancellation preserves old destination')

summary=dict(checks=checks,quality_cells=len(quality_rows),quality=quality_rows,commands=commands,scope='short generated WAV functional and full-band numeric regression only; 30-second x3 performance is UNVERIFIED_PENDING_ROOT_SERIAL_WINDOW',audio_device_calls=0)
(root/'summary.json').write_text(json.dumps(summary,indent=2),encoding='utf-8');print('PASS actual multilevel CLI checks='+str(checks)+' quality_cells='+str(len(quality_rows))+' audio_device_calls=0')
