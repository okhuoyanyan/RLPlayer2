"""Executable positive/negative controls for actual-Q62 continuous extensions."""
import argparse,copy,hashlib,json,struct,sys
from fractions import Fraction as F
from pathlib import Path
from review5_fixed_certificate import prove,half_away,sha
p=argparse.ArgumentParser();p.add_argument('--evidence',type=Path,required=True);p.add_argument('--flint-path',type=Path,required=True);a=p.parse_args();a.evidence.mkdir(parents=True,exist_ok=True);checks=[]
def check(name,yes):
 checks.append({'name':name,'pass':bool(yes)})
 if not yes:raise AssertionError(name)
for x,y in [(F(1,2),1),(F(-1,2),-1),(F(3,2),2),(F(-3,2),-2),(F(1,3),0),(F(-1,3),0),(F(0),0)]:check('independent exact llround tie/sign '+str(x),half_away(x)==y)
root=a.evidence.parent;actual=root/'poly-sinc-short-lp-96000-44100/actual.json';original=json.loads(actual.read_text());proof_path=Path(json.loads((actual.parent/'continuous-extension.json').read_text())['base_continuous_proof_path']);proof=json.loads(proof_path.read_text());expected=sha(proof_path)
for folder in sorted(root.iterdir()):
 if not folder.is_dir() or not (folder/'continuous-extension.json').exists():continue
 r=json.loads((folder/'continuous-extension.json').read_text());re=prove(folder/'actual.json',Path(r['base_continuous_proof_path']),r['base_continuous_proof_sha256'],a.flint_path)
 check('actual production continuous recheck '+folder.name,re['status']=='PASS' and re['new_coherent_alias_amplitude_upper_exact']==r['new_coherent_alias_amplitude_upper_exact'])
def reject(name,mutate,base_mutate=None):
 folder=a.evidence/name;folder.mkdir(exist_ok=True);obj=copy.deepcopy(original)
 for s in obj['stages']:
  for k in ['double_file','fixed_file']:
   if s.get(k):s[k]=str(actual.parent/s[k])
 base=proof_path;pin=expected
 if base_mutate:
  q=copy.deepcopy(proof);base_mutate(q);base=folder/'base.json';base.write_text(json.dumps(q));pin=sha(base)
 mutate(obj,folder);ap=folder/'actual.json';ap.write_text(json.dumps(obj))
 try:prove(ap,base,pin,a.flint_path)
 except (ValueError,KeyError) as e:checks.append({'name':name,'pass':True,'rejection':str(e)});return
 raise AssertionError('bad data accepted '+name)
reject('usedFixed-false',lambda x,d:x.update(stats_usedFixed=False))
reject('wrong-reference-gain',lambda x,d:x['stages'][0].update(reference_gain=148))
reject('missing-alias-branch',lambda x,d:None,lambda q:q['alias_path_ids'].pop())
reject('wrong-output-clock',lambda x,d:x.update(output_rate=48000))
reject('missing-actual-integer-history',lambda x,d:x['stages'][0].update(actual_fixed_history_channels=0))
reject('forged-double-identity',lambda x,d:x['stages'][0].update(double_sha256='0'*64))
def mutate_fixed(x,d):
 s=x['stages'][0];raw=bytearray(Path(s['fixed_file']).read_bytes());v=struct.unpack_from('<q',raw)[0];struct.pack_into('<q',raw,0,v+1);f=d/'tampered.q62le';f.write_bytes(raw);s.update(fixed_file=str(f),fixed_sha256=sha(f))
reject('actual-Q62-not-llround',mutate_fixed)
def nan(x,d):
 s=x['stages'][0];raw=bytearray(Path(s['double_file']).read_bytes());struct.pack_into('<d',raw,0,float('nan'));f=d/'nan.f64le';f.write_bytes(raw);s.update(double_file=str(f),double_sha256=sha(f))
 # Keep the old proof identity: either SHA binding or nonfinite guard must reject.
reject('nonfinite-raw-coefficient',nan)
try:prove(actual,proof_path,'0'*64,a.flint_path)
except ValueError as e:checks.append({'name':'pinned-base-proof-sha-mismatch','pass':True,'rejection':str(e)})
else:raise AssertionError('unpinned proof accepted')
# Analytic calibration: exact dyadic two-stage products at z=1,-1,i,-i.
# The theorem itself is the triangle inequality for ALL |z|=1; these controls
# catch missing gain/product cross terms without pretending to prove all bands.
h=[[F(1,2),F(1,4),F(1,8)],[F(3,4),F(-1,8)]];e=[[F(1,1<<63),-F(1,1<<64),F(0)],[F(1,1<<65),F(1,1<<66)]];g=[F(4),F(3)]
norm=[sum(map(abs,t),F())/v for t,v in zip(h,g)];de=[sum(map(abs,t),F())/v for t,v in zip(e,g)];D=(norm[0]+de[0])*(norm[1]+de[1])-norm[0]*norm[1]
for z in [complex(1),complex(-1),complex(0,1),complex(0,-1)]:
 def evaluate(v):
  # Roots are exactly integral Gaussian rationals; evaluate as Fraction pairs.
  zr,zi=F(int(z.real)),F(int(z.imag));r=i=F();pr=F(1);pi=F()
  for q in v:r+=q*pr;i+=q*pi;pr,pi=pr*zr-pi*zi,pr*zi+pi*zr
  return r,i
 def product(v):
  aa=evaluate(v[0]);bb=evaluate(v[1]);return ((aa[0]*bb[0]-aa[1]*bb[1])/(g[0]*g[1]),(aa[0]*bb[1]+aa[1]*bb[0])/(g[0]*g[1]))
 old=product(h);new=product([[x+y for x,y in zip(v,w)] for v,w in zip(h,e)]);dr=new[0]-old[0];di=new[1]-old[1];check('analytic two-stage exact product bound '+str(z),dr*dr+di*di<=D*D)
report={'status':'PASS','passed':sum(c['pass'] for c in checks),'failed':0,'checks':checks,'scope':'actual Q62 exact-bound rechecks plus analytic roots and independent mutation negatives; not hardware/output-rounding proof'};(a.evidence/'controls.json').write_text(json.dumps(report,indent=2));print(json.dumps(report))
