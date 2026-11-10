"""Memory-bounded continuous power-Taylor certificate generation.

Independent verification is implemented in review5_stream_verify.py. This generator
uses BALL autocorrelation and one-sided derivative DFTs, retained one order at a time.
"""
import argparse,hashlib,json,math,struct,sys,time
from fractions import Fraction
from pathlib import Path

def generate(coefficients,spec,flint_path,grid=None,degree=40,precision=256,max_grid=1048576):
 sys.path.insert(0,str(flint_path));from flint import arb,acb,arb_poly,fmpq,ctx
 if not 192<=precision<=4096 or not 8<=degree<=64:raise ValueError('invalid precision/degree')
 ctx.prec=precision;ctx.threads=1
 if Path(coefficients).stat().st_size>131072*8:raise ValueError('explicit coefficient input-size budget exhausted')
 raw=Path(coefficients).read_bytes()
 if not raw or len(raw)%8:raise ValueError('invalid binary64 file')
 if len(raw)//8>131072:raise ValueError('explicit coefficient-count budget exhausted')
 n=len(raw)//8
 if grid is None:
  grid=1
  while grid<4*n:grid*=2
 if grid<n or grid&(grid-1) or grid>max_grid:raise ValueError('explicit grid budget exhausted')
 if grid*(512+precision)>640*1024**2:raise ValueError('explicit <1GiB workspace budget exhausted')
 h=struct.unpack('<'+'d'*(len(raw)//8),raw)
 if not all(math.isfinite(v) for v in h):raise ValueError('non-finite coefficients')
 reference=sum(Fraction(*v.as_integer_ratio()) for v in h) if spec.get('normalize_dc',False) else Fraction(str(spec.get('reference_gain','1')))
 if not reference:raise ValueError('zero reference gain')
 coeff=[arb(fmpq(*v.as_integer_ratio()))/arb(fmpq(reference.numerator,reference.denominator)) for v in h]
 span=max(n-1,1)
 if not spec.get('bands'):raise ValueError('nonempty fixed bands required')
 started=time.perf_counter();regions=[];active=set();limits=[]
 for bid,b in enumerate(spec['bands']):
  lo,hi=Fraction(str(b['lo'])),Fraction(str(b['hi']))
  if not 0<=lo<hi<=Fraction(1,2):raise ValueError('invalid band')
  if b['kind']=='pass':
   maximum=arb(10)**(arb(str(b['ripple_db']))/10);minimum=1/maximum
  elif b['kind']=='stop':maximum=arb(10)**(arb(str(b['db']))/10);minimum=arb(0)
  elif b['kind']=='envelope':maximum=arb(str(b['max_amplitude']))**2;minimum=arb(0)
  else:raise ValueError('invalid band kind')
  limits.append((minimum,maximum))
  for k in range(max(0,math.ceil(lo*grid-Fraction(1,2))),min(grid//2,math.floor(hi*grid+Fraction(1,2)))+1):
   left=max(lo,Fraction(2*k-1,2*grid));right=min(hi,Fraction(2*k+1,2*grid))
   if left<right:regions.append((bid,k,left,right));active.add(k)
 # Approximate ball polynomial product, deliberately not the verifier's exact rational product.
 correlation=arb_poly(coeff)*arb_poly(coeff[::-1]);weights=[correlation[n-1+lag]*(1 if lag==0 else 2) for lag in range(n)]
 l1=sum(abs(v) for v in coeff);radius=arb(span)*arb.pi()/grid
 remainder=(l1*l1*radius**degree/math.factorial(degree)).upper()
 centers=sorted(active);variation={k:remainder for k in centers};points={}
 for order in range(degree):
  if order:weights=[(v*lag/span/order) if lag else arb(0) for lag,v in enumerate(weights)]
  transform=acb.dft(weights+[arb(0)]*(grid-n))
  # Each DFT derivative is enclosed on its entire closed cell by the same r.
  if not order:points={k:transform[k].real for k in centers}
  else:
   factor=(-acb(0,1))**order;scale=radius**order
   for k in centers:variation[k]+=(transform[k]*factor).real.abs_upper()*scale
  del transform
 leaves=[];unresolved=failed=0
 for bid,k,left,right in regions:
  minimum,maximum=limits[bid];lo=(points[k]-variation[k]).lower();hi=(points[k]+variation[k]).upper()
  if lo<0:lo=arb(0)
  low=(lo*(1-arb('1e-9'))-arb('1e-50')).lower();high=(hi*(1+arb('1e-9'))+arb('1e-50')).upper()
  if low<0:low=arb(0)
  good=high<maximum.lower() and (spec['bands'][bid]['kind']!='pass' or low>minimum.upper())
  bad=points[k].lower()>maximum.upper() or (spec['bands'][bid]['kind']=='pass' and points[k].upper()<minimum.lower())
  status='PASS' if good else 'FAIL' if bad else 'INDETERMINATE';unresolved+=status=='INDETERMINATE';failed+=status=='FAIL'
  leaves.append({'band':bid,'lo':str(left),'hi':str(right),'center':str(Fraction(k,grid)),'lower_power':str(low.fmpq()),'upper_power':str(high.fmpq()),'status':status})
 return {'schema':'rlplayer-continuous-fir-v1','status':'FAIL' if failed else 'INDETERMINATE' if unresolved else 'PASS','coefficient_sha256':hashlib.sha256(raw).hexdigest(),'coefficient_count':n,'spec_sha256':hashlib.sha256(json.dumps(spec,sort_keys=True,separators=(',',':')).encode()).hexdigest(),'precision_bits':precision,'degree':degree,'grid':grid,'method':'streamed ball autocorrelation, one-sided Arb DFT derivatives, full closed-cell Taylor remainder','normalization':{'mode':'exact declared gain','denominator':str(reference)},'leaves':leaves,'attempts':[{'grid':grid,'leaves':len(leaves),'violations':failed,'unresolved':unresolved}],'elapsed_seconds':time.perf_counter()-started,'runtime':{'flint':__import__('flint').__version__,'threads':1}}

def main():
 p=argparse.ArgumentParser();p.add_argument('--coefficients',required=True,type=Path);p.add_argument('--spec',required=True,type=Path);p.add_argument('--output',required=True,type=Path);p.add_argument('--flint-path',required=True,type=Path);p.add_argument('--grid',type=int);p.add_argument('--degree',type=int,default=40);p.add_argument('--precision',type=int,default=256);a=p.parse_args()
 try:
  result=generate(a.coefficients,json.loads(a.spec.read_text()),a.flint_path,a.grid,a.degree,a.precision)
  # Reuse only an OS counter helper; no mathematical implementation is shared.
  from review5_spectral_certificate import process_memory
  result['process_memory']=process_memory();a.output.write_text(json.dumps(result,separators=(',',':')))
  print(json.dumps({k:v for k,v in result.items() if k!='leaves'}));return {'PASS':0,'FAIL':1,'INDETERMINATE':3}[result['status']]
 except Exception as e:print(json.dumps({'status':'REJECTED','reason':str(e)}));return 2
if __name__=='__main__':sys.exit(main())
