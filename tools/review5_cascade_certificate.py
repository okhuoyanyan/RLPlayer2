"""Continuous factorized two-stage multirate proof, without expanded coefficients.

Input certificates must have been independently numerically verified. This tool
also checks original coefficient/spec/certificate identities and exact full-band
coverage. CLI normally re-verifies both stages before any composition.
"""
import argparse,bisect,hashlib,json,math,sys,time
from fractions import Fraction as Q
from pathlib import Path

def sha(path):return hashlib.sha256(Path(path).read_bytes()).hexdigest()
class Bounds:
 def __init__(self,certificate):
  data=json.loads(Path(certificate).read_text());self.hash=data['coefficient_sha256'];self.certificate_sha=sha(certificate);self.rows=sorted((Q(x['lo']),Q(x['hi']),Q(x['lower_power']),Q(x['upper_power'])) for x in data['leaves'])
  cursor=Q(0)
  for left,right,lower,upper in self.rows:
   if left!=cursor or right<=left or lower<0 or upper<lower:raise ValueError('factor certificates must cover entire [0,1/2] without gaps/overlap')
   cursor=right
  if cursor!=Q(1,2):raise ValueError('factor full-band coverage incomplete')
  self.left=[x[0] for x in self.rows];self.right=[x[1] for x in self.rows];self.size=1
  while self.size<len(self.rows):self.size*=2
  self.upper=[Q(0)]*(2*self.size);self.lower=[None]*(2*self.size)
  for i,(_,_,lo,hi) in enumerate(self.rows):self.upper[self.size+i]=hi;self.lower[self.size+i]=lo
  for i in range(self.size-1,0,-1):
   self.upper[i]=max(self.upper[i*2],self.upper[i*2+1]);a,b=self.lower[i*2:i*2+2];self.lower[i]=b if a is None else a if b is None else min(a,b)
 def query(self,left,right):
  if not Q(0)<=left<=right<=Q(1,2):raise ValueError('normalized factor interval outside [0,1/2]')
  lo=bisect.bisect_left(self.right,left)+self.size;hi=bisect.bisect_right(self.left,right)+self.size
  minimum=None;maximum=Q(0)
  while lo<hi:
   if lo&1:
    maximum=max(maximum,self.upper[lo]);x=self.lower[lo];minimum=x if minimum is None else minimum if x is None else min(minimum,x);lo+=1
   if hi&1:
    hi-=1;maximum=max(maximum,self.upper[hi]);x=self.lower[hi];minimum=x if minimum is None else minimum if x is None else min(minimum,x)
   lo//=2;hi//=2
  return minimum,maximum

def folded_intervals(left,right,period):
 # Exact periodicity and real-FIR conjugate symmetry; split every wrap/fold.
 period=Q(period);pieces=[]
 for k in range(math.floor(left/period),math.floor(right/period)+1):
  a=max(left,k*period)-k*period;b=min(right,(k+1)*period)-k*period
  if a>b:continue
  if a<=period/2:pieces.append((a/period,min(b,period/2)/period))
  if b>=period/2:pieces.append(((period-b)/period,(period-max(a,period/2))/period))
 return pieces

def factor_range(bound,left,right,period):
 values=[bound.query(a,b) for a,b in folded_intervals(left,right,period)]
 return min(x[0] for x in values),max(x[1] for x in values)

def compose(pipeline,first,second,flint_path,bins=1024,claimed=None):
 sys.path.insert(0,str(flint_path));from flint import arb,fmpq,ctx
 if type(bins) is not int or not 1<=bins<=65536:raise ValueError('positive bounded output interval count required')
 ctx.prec=320;ctx.threads=1;plan=json.loads(Path(pipeline).read_text());stages=plan['stages']
 if len(stages)!=2:raise ValueError('currently exactly two actual cascade stages required')
 a,b=stages;inrate=int(plan['input_rate']);out=int(plan['output_rate']);mid=int(a['output_rate']);high=int(b['coefficient_rate'])
 if a['input_rate']!=inrate or b['output_rate']!=out or a['coefficient_rate']!=mid:raise ValueError('stage frequency-domain identity mismatch')
 if a['M']!=1 or b['input_rate']!=mid or high!=mid*b['L'] or inrate*a['L']!=mid:raise ValueError('unsupported or inconsistent actual multirate topology')
 if high!=out*b['M'] or math.gcd(a['L']*b['L'],b['M'])!=1:raise ValueError('non-coprime or inconsistent final rational stage')
 path_ids=list(range(1,b['M']))
 if claimed is not None:
  if claimed['pipeline_sha256']!=sha(pipeline) or claimed['factor_coefficient_sha256']!=[first.hash,second.hash] or claimed['alias_path_ids']!=path_ids:raise ValueError('omitted alias path or changed Plan/coefficient identity')
  if claimed['output_frequency_intervals']!=bins:raise ValueError('changed output interval coverage')
  if claimed['factor_certificate_sha256']!=[first.certificate_sha,second.certificate_sha]:raise ValueError('changed factor certificate')
 fp=Q(str(plan['pass_hz']));stop=Q(str(plan['stop_hz']));nyquist=Q(high,2)
 if not 0<fp<stop<nyquist or Q(str(plan['stop_target_db']))<=0:raise ValueError('invalid trusted pass/stop domain')
 av=lambda q:arb(fmpq(q.numerator,q.denominator))
 started=time.perf_counter();joint_stop=Q(0)
 # Every second-factor closed cell tiles the full high-rate Nyquist domain.
 # First-factor range queries cover its periodic folded image over the whole
 # cell, so this is an interval product, never a frequency sample product.
 for lo,hi,l2,u2 in second.rows:
  left=max(stop,lo*high);right=min(nyquist,hi*high)
  if left>right:continue
  _,u1=factor_range(first,left,right,mid);joint_stop=max(joint_stop,u1*u2)
 worst_alias=arb(0);worst_desired_lower=arb(1);worst_desired_upper=arb(0);worst_alias_bin=0
 for q in range(bins):
  left=fp*q/bins;right=fp*(q+1)/bins
  l1,u1=factor_range(first,left,right,mid);l2,u2=factor_range(second,left,right,high)
  desired_lo=av(l1*l2).sqrt().lower();desired_hi=av(u1*u2).sqrt().upper()
  total=arb(0)
  # Reverse downsampling formula has EVERY k=0..M-1 branch. k=0 is desired;
  # all M-1 unwanted terms are summed in LINEAR amplitude (coherent worst case).
  for k in path_ids:
   f0=left+k*out;f1=right+k*out
   _,p1=factor_range(first,f0,f1,mid);_,p2=factor_range(second,f0,f1,high)
   total+=av(p1*p2).sqrt().upper()
  total=total.upper()
  if worst_alias<total:worst_alias=total;worst_alias_bin=q
  lower=(desired_lo-total).lower();upper=(desired_hi+total).upper()
  if lower<worst_desired_lower:worst_desired_lower=lower
  if upper>worst_desired_upper:worst_desired_upper=upper
 threshold=arb(10)**(-arb(str(plan['stop_target_db']))/20);pass_hi=arb(10)**(arb('0.01')/20);pass_lo=1/pass_hi
 stop_ok=av(joint_stop)<threshold*threshold
 alias_ok=worst_alias<threshold.lower();pass_ok=worst_desired_lower>pass_lo.upper() and worst_desired_upper<pass_hi.lower()
 status='PASS' if stop_ok and alias_ok and pass_ok else 'INDETERMINATE'
 if claimed is not None:
  if Q(claimed['joint_stop_power_upper_exact'])<joint_stop or Q(claimed['coherent_alias_amplitude_upper_exact'])<Q(str(worst_alias.fmpq())):raise ValueError('forged joint/alias upper envelope')
  if Q(claimed['pass_amplitude_lower_exact'])>Q(str(worst_desired_lower.fmpq())) or Q(claimed['pass_amplitude_upper_exact'])<Q(str(worst_desired_upper.fmpq())):raise ValueError('forged joint pass envelope')
  if claimed['status']=='PASS' and status!='PASS':raise ValueError('unproved joint PASS')
 result={'schema':'rlplayer-continuous-two-stage-v1','status':status,'pipeline_sha256':sha(pipeline),'factor_coefficient_sha256':[first.hash,second.hash],
  'factor_certificate_sha256':[first.certificate_sha,second.certificate_sha],'alias_path_ids':path_ids,'alias_branch_count':len(path_ids),'output_frequency_intervals':bins,
  'all_intervals_cover':'[0, pass_hz] with exact rational endpoints','joint_stop_continuous_pass':bool(stop_ok),'coherent_alias_budget_pass':bool(alias_ok),'joint_pass_ripple_pass':bool(pass_ok),
  'joint_stop_power_upper_exact':str(joint_stop),'coherent_alias_amplitude_upper_exact':str(worst_alias.fmpq()),
  'joint_stop_db_upper':float(10*av(joint_stop).log()/arb(10).log()),'coherent_alias_exact_zero':bool(worst_alias==0),'coherent_alias_db_upper':float(20*worst_alias.log()/arb(10).log()) if worst_alias>0 else '-inf','worst_alias_interval':[str(fp*worst_alias_bin/bins),str(fp*(worst_alias_bin+1)/bins)],
  'pass_amplitude_lower_exact':str(worst_desired_lower.fmpq()),'pass_amplitude_upper_exact':str(worst_desired_upper.fmpq()),'elapsed_seconds':time.perf_counter()-started,
  'scope':'continuous ideal multirate FIR transfer of the exact finite coefficients, all final-decimator branches; excludes runtime floating dot-product accumulation/output quantization'}
 return result

def main():
 p=argparse.ArgumentParser();p.add_argument('--pipeline',required=True,type=Path);p.add_argument('--stage1-spec',required=True,type=Path);p.add_argument('--stage1-certificate',required=True,type=Path);p.add_argument('--stage2-spec',required=True,type=Path);p.add_argument('--stage2-certificate',required=True,type=Path);p.add_argument('--flint-path',required=True,type=Path);p.add_argument('--output',required=True,type=Path);p.add_argument('--check-proof',type=Path);p.add_argument('--bins',type=int,default=1024);a=p.parse_args()
 try:
  from review5_stream_verify import verify,process_memory
  plan=json.loads(a.pipeline.read_text());base=a.pipeline.parent
  for s,sp,cp in zip(plan['stages'],[a.stage1_spec,a.stage2_spec],[a.stage1_certificate,a.stage2_certificate]):
   spec=json.loads(sp.read_text());c=json.loads(cp.read_text())
   if spec.get('normalize_dc',False):raise ValueError('actual multirate factors require reference L, not DC renormalization')
   if Fraction(str(spec.get('reference_gain','1')))!=s['L']:raise ValueError('factor reference gain must equal its actual interpolation L exactly')
   r=verify(base/s['file'],spec,c,flint_path=a.flint_path)
   if r['status']!='PASS':raise ValueError('factor proof did not independently validate')
  result=compose(a.pipeline,Bounds(a.stage1_certificate),Bounds(a.stage2_certificate),a.flint_path,a.bins,json.loads(a.check_proof.read_text()) if a.check_proof else None)
  result['process_memory']=process_memory();a.output.write_text(json.dumps(result,indent=2));print(json.dumps(result));return 0 if result['status']=='PASS' else 3
 except Exception as e:print(json.dumps({'status':'REJECTED','reason':str(e)}));return 2
# Fraction alias is kept explicit at the external-spec boundary.
Fraction=Q
if __name__=='__main__':sys.exit(main())
