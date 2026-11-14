"""Independent Arb golden for a real exported DD raw FIR object."""
import argparse,json,math,struct,hashlib,sys,time
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--pipeline',required=True,type=Path);p.add_argument('--flint-path',required=True,type=Path);p.add_argument('--output',required=True,type=Path);a=p.parse_args();sys.path.insert(0,str(a.flint_path));from flint import arb,fmpq,ctx
ctx.prec=320;ctx.threads=1;plan=json.loads(a.pipeline.read_text());s=plan['stages'][0]
if len(plan['stages'])!=1 or s['phase']!='lp' or s['window'] not in ['kaiser','bharris']:raise RuntimeError('requires single linear high-attenuation Kaiser-equivalent stage')
n=s['count'];L=s['L'];M=s['M'];cutoff=min(1./L,1./M)*(1.-s['transition']*.5);beta=.1102*(s['attenuation']-8.7)
fc=arb(cutoff);b=arb(beta);den=b.bessel_i(0);kernel=[arb(0)]*n;start=time.perf_counter()
for i in range(n//2+n%2):
 x=arb(2*i-(n-1))/2;r=arb(fmpq(2*i-(n-1),n-1));w=arb(0) if i==0 else (b*(1-r*r).sqrt()).bessel_i(0)/den
 sinc=fc if x==0 else (fc*x).sin_pi()/(arb.pi()*x);kernel[i]=kernel[n-1-i]=sinc*w
total=sum(kernel);out=[]
for v in kernel:
 raw=v*L/total;lo,hi=float(raw.lower()),float(raw.upper())
 if lo!=hi:raise RuntimeError('320-bit interval cannot settle final binary64 rounding')
 out.append(lo)
golden=struct.pack('<'+'d'*n,*out);actual=(a.pipeline.parent/s['file']).read_bytes();different=sum(actual[i:i+8]!=golden[i:i+8] for i in range(0,len(golden),8)) if len(actual)==len(golden) else -1
result={'status':'PASS' if different==0 else 'FAIL','object':'independent Arb320 final-raw-gain golden for actual exported finite coefficients','coefficient_count':n,'coefficient_sha256':hashlib.sha256(actual).hexdigest(),'golden_sha256':hashlib.sha256(golden).hexdigest(),'different':different,'cutoff_binary64_hex':cutoff.hex(),'beta_binary64_hex':beta.hex(),'elapsed_seconds':time.perf_counter()-start};a.output.write_text(json.dumps(result,indent=2));print(json.dumps(result));sys.exit(different!=0)
