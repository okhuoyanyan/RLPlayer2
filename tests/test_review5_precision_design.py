"""Independent Arb golden calibration of design DD arithmetic and final rounding."""
import argparse,json,struct,subprocess,sys,time,hashlib
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--exe',required=True,type=Path);p.add_argument('--flint-path',required=True,type=Path);p.add_argument('--evidence',required=True,type=Path);a=p.parse_args();a.evidence.mkdir(parents=True,exist_ok=True);sys.path.insert(0,str(a.flint_path));from flint import arb,fmpq,ctx
ctx.prec=320;ctx.threads=1;started=time.perf_counter();samples=a.evidence/'scalar-samples.json';guards=a.evidence/'guards.json'
for args in [['--samples',samples],['--guards',guards],['--dd',a.evidence]]:subprocess.run([str(a.exe),*map(str,args)],check=True)
rows=json.loads(samples.read_text());worst=arb(0);failed=[]
for number,s in enumerate(rows):
 actual=arb(s['hi'])+arb(s['lo']);beta=arb(s['beta'])
 if s['kind']=='i0':reference=beta.bessel_i(0)
 elif s['kind']=='sinc':
  cutoff=arb(s['cutoff']);x=arb(s['x']);reference=cutoff if s['x']==0 else (cutoff*x).sin_pi()/(arb.pi()*x)
 else:
  n,i=s['n'],s['i']
  if n==1:reference=arb(1)
  elif i in [0,n-1]:reference=arb(0)
  else:
   r=arb(fmpq(2*i-(n-1),n-1));reference=(beta*(1-r*r).sqrt()).bessel_i(0)/beta.bessel_i(0)
 error=(actual-reference).abs_upper()
 if reference.abs_lower()>0:relative=error/reference.abs_lower()
 else:relative=error
 if worst<relative:worst=relative
 if relative>arb('1e-28'):failed.append({'row':number,'kind':s['kind'],'relative_error':str(relative)})
# Full actual design fixture, exact original binary64 parameter values.
n=81732;gain=147;cutoff=arb(min(1./147.,1./320.)*(1.-.1*.5));beta=arb(.1102*(340.-8.7));den=beta.bessel_i(0);kernel=[arb(0)]*n
for i in range(n//2):
 x=arb(2*i-(n-1))/2;r=arb(fmpq(2*i-(n-1),n-1));w=arb(0) if i==0 else (beta*(1-r*r).sqrt()).bessel_i(0)/den
 value=(cutoff*x).sin_pi()/(arb.pi()*x)*w;kernel[i]=kernel[n-1-i]=value
total=sum(kernel)
def round_unique(x):
 lo,hi=float(x.lower()),float(x.upper())
 if lo!=hi:raise RuntimeError('oracle cannot settle final binary64 rounding')
 return lo
golden_raw=struct.pack('<'+'d'*n,*[round_unique(v*gain/total) for v in kernel]);golden_unit=struct.pack('<'+'d'*n,*[round_unique(v/total) for v in kernel]);actual_raw=(a.evidence/'dd-raw.f64le').read_bytes();actual_unit=(a.evidence/'dd-unit.f64le').read_bytes()
difference=lambda x,y:sum(x[i:i+8]!=y[i:i+8] for i in range(0,len(x),8))
raw_diff=difference(actual_raw,golden_raw);unit_diff=difference(actual_unit,golden_unit);sym_diff=sum(actual_raw[i*8:i*8+8]!=actual_raw[(n-1-i)*8:(n-i)*8] for i in range(n));guard=json.loads(guards.read_text())
result={'status':'PASS' if not failed and raw_diff==unit_diff==sym_diff==0 and guard['failed']==0 else 'FAIL','scalar_samples':len(rows),'scalar_failures':failed,'max_scalar_relative_error_upper':str(worst),'full_array_count':n,'raw_gain_final_round_different':raw_diff,'unit_final_round_different':unit_diff,'symmetry_different':sym_diff,'guard_checks':guard,'raw_sha256':hashlib.sha256(actual_raw).hexdigest(),'unit_sha256':hashlib.sha256(actual_unit).hexdigest(),'oracle_precision_bits':320,'elapsed_seconds':time.perf_counter()-started,'scope':'EXPERIMENTAL_NOT_PIPELINE numerical calibration; no spectral or playback pass is inferred'}
(a.evidence/'precision-golden.json').write_text(json.dumps(result,indent=2));print(json.dumps(result));sys.exit(result['status']!='PASS')
