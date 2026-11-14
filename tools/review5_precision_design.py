"""High-precision coefficient diagnosis; ALL outputs explicitly experimental.
No product source is modified. The original parameter/shape/gain are kept fixed.
"""
import argparse,hashlib,json,math,struct,sys,time
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--directory',required=True,type=Path);p.add_argument('--flint-path',required=True,type=Path);p.add_argument('--actual-plan',required=True,type=Path);a=p.parse_args();sys.path.insert(0,str(a.flint_path));from flint import arb,fmpq,ctx
ctx.prec=256;ctx.threads=1;d=a.directory;params=json.loads((d/'parameters.json').read_text());n=params['n'];gain=params['reference_gain'];cutoff=float(params['cutoff_binary64']);beta=float(params['beta_binary64']);original=json.loads(a.actual_plan.read_text());start=time.perf_counter()
def read(name):raw=(d/name).read_bytes();return list(struct.unpack('<'+'d'*(len(raw)//8),raw))
def write(name,v):path=d/(name+'.f64le');path.write_bytes(struct.pack('<'+'d'*len(v),*v));return path
def rounded(x):
 lo=float(x.lower());hi=float(x.upper())
 if lo!=hi:raise ValueError('rounding not uniquely settled at256bits')
 return lo
sprod=read('production-sinc.f64le');wprod=read('production-window.f64le');productprod=read('production-product.f64le');rawprod=read('primitive-raw.f64le')
fc=arb(fmpq(*cutoff.as_integer_ratio()));beta_hp=arb(fmpq(*beta.as_integer_ratio()));den=beta_hp.bessel_i(0);pi=arb.pi();kernel=[arb(0)]*n;sinc=[arb(0)]*n;window=[arb(0)]*n;standard_kernel=[arb(0)]*n
for i in range((n+1)//2):
 x=arb(2*i-(n-1))/2;r=arb(fmpq(2*i-(n-1),n-1));aa=1-r*r
 w=arb(0) if i==0 else (beta_hp*aa.sqrt()).bessel_i(0)/den
 ws=arb(1)/den if i==0 else w
 s=fc if x==0 else (fc*x).sin_pi()/(pi*x)
 for j in {i,n-1-i}:sinc[j]=s;window[j]=w;kernel[j]=s*w;standard_kernel[j]=s*ws
normalizer=sum(kernel);normalizer_standard=sum(standard_kernel)
write('hp-window-rounded',[rounded(v) for v in window]);write('hp-sinc-rounded',[rounded(v) for v in sinc])
unit=[rounded(v/normalizer) for v in kernel]
def production_norm(values):
 total=0.0
 for v in values:total+=v
 return [(v/total)*gain for v in values]
variants={
 'hp-full-once-gain':[rounded(v*gain/normalizer) for v in kernel],
 'hp-unit-then-gain':[v*gain for v in unit],
 'hp-product-rounded-then-production-norm':production_norm([rounded(v) for v in kernel]),
 'hp-window-only':production_norm([s*rounded(w) for s,w in zip(sprod,window)]),
 'hp-sinc-only':production_norm([rounded(s)*w for s,w in zip(sinc,wprod)]),
 'hp-standard-endpoints-once':[rounded(v*gain/normalizer_standard) for v in standard_kernel]
}
# Construct exact original-product normalization once, not O(N^2).
prod_hp=[arb(fmpq(*x.as_integer_ratio())) for x in productprod];prod_sum=sum(prod_hp)
variants['production-product-hp-norm-once-gain']=[rounded(v*gain/prod_sum) for v in prod_hp]
reconstructed=production_norm(productprod)
if any(struct.pack('<d',x)!=struct.pack('<d',y) for x,y in zip(reconstructed,rawprod)):raise ValueError('Python staged baseline does not match actual C++ raw coefficients')
rows=[]
for name,v in variants.items():
 path=write(name,v);plan=json.loads(json.dumps(original));plan.update(object='EXPERIMENTAL_NOT_PIPELINE high precision coefficient diagnosis',experimental=True,variant=name);plan['stages'][0]['file']=path.name
 (d/(name+'.plan.json')).write_text(json.dumps(plan,indent=2));delta=[(x-y)/gain for x,y in zip(v,rawprod)]
 rows.append({'variant':name,'status':'EXPERIMENTAL_NOT_PIPELINE','coefficient_sha256':hashlib.sha256(path.read_bytes()).hexdigest(),'different_from_production':sum(struct.pack('<d',x)!=struct.pack('<d',y) for x,y in zip(v,rawprod)),'max_abs_normalized_coefficient_delta':max(map(abs,delta)),'l1_normalized_delta':math.fsum(map(abs,delta)),'rms_normalized_delta':math.sqrt(math.fsum(x*x for x in delta)/n),'symmetry_different':sum(struct.pack('<d',v[i])!=struct.pack('<d',v[n-1-i]) for i in range(n))})
result={'status':'EXPERIMENTAL_NOT_PIPELINE','parameters':params,'precision_bits':256,'rounding':'Arb lower and upper both convert to the same binary64 for every output; final output uses that unique rounding','cutoff_hex':cutoff.hex(),'beta_hex':beta.hex(),'original_cpp_primitive_exactly_matches_actual_pipeline':params['primitive_vs_actual_different']==0,'python_reconstruction_bit_exact':True,'normalizer_ball':str(normalizer),'elapsed_seconds':time.perf_counter()-start,'variants':rows}
(d/'hp-design-diagnosis.json').write_text(json.dumps(result,indent=2));print(json.dumps(result))
