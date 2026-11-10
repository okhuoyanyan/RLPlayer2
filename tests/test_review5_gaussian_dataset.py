"""Dataset integrity plus an independent Arb calibration of finite DD points.
This is deliberately not a continuous Gaussian quality certificate.
"""
import argparse,hashlib,json,math,struct,sys,time
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--dataset',type=Path,required=True);p.add_argument('--flint-path',required=True);a=p.parse_args();sys.path.insert(0,a.flint_path)
from flint import arb,acb,ctx
ctx.prec=256;ctx.threads=1
d=json.loads(a.dataset.read_text());rows=d['rows'];assert len(rows)==90
checks=0
for x in rows:
 assert x['source_support_T']>0;checks+=1
 if x['status']=='ACTUAL_PIPELINE_POINT_SCREEN':
  assert x['exit_code']==0 and len(x['points']) in (13,14) and x['stages'][0]['T']==x['source_support_T'];checks+=1
  for s in x['stages']:
   f=Path(x['folder'])/s['file'];raw=f.read_bytes();assert hashlib.sha256(raw).hexdigest()==s['sha256'] and len(raw)==s['count']*8;assert all(math.isfinite(v) for v in struct.unpack('<'+'d'*s['count'],raw));checks+=1
  q=x['actual_impulse'];assert q['max_cpu_parallel_threads']<=2 and q['output_frames']>q['peak_index'] and 0<=q['pre_peak_energy_fraction']<=1 and 0<=q['post_peak_energy_fraction']<=1;checks+=1
 else:assert 'reason' in x and 'points' not in x;checks+=1
cal=[];start=time.perf_counter()
for version,ir,orr in [('baseline',96000,384000),('candidate',44100,384000)]:
 row=next(x for x in rows if x['version']==version and x['preset']=='poly-sinc-gauss-short' and x['input_rate']==ir and x['output_rate']==orr)
 for f in [0,row['pass_hz'],row['mirror_stop_hz']]:
  value=acb(1)
  for s in row['stages']:
   raw=(Path(row['folder'])/s['file']).read_bytes();coeff=struct.unpack('<'+'d'*s['count'],raw);z=acb(0);freq=arb(f)/s['coefficient_rate']
   # Independently recompute each trig value. No DD recurrence or its roots.
   for n,h in enumerate(coeff):
    angle=freq*(-2*n);z+=acb(angle.cos_pi(),angle.sin_pi())*arb(h)
   value*=z/s['reference_gain']
  db=20*abs(value).log()/arb(10).log();actual=next(x['desired_component_db'] for x in row['points'] if x['hz']==f);error=abs(db-arb(actual));bound=float(error.upper());assert bound<1e-8,(version,f,bound)
  cal.append({'version':version,'hz':f,'dd_db':actual,'arb_db_enclosure':str(db),'absolute_db_error_upper':bound});checks+=1
report={'status':'PASS','checks':checks,'calibration_points':cal,'calibration_seconds':time.perf_counter()-start,'scope':'90-row identity/resource/impulse integrity plus six independent Arb point calibrations; NOT continuous band proof'}
a.dataset.with_name('validation.json').write_text(json.dumps(report,indent=2));print(json.dumps(report))
