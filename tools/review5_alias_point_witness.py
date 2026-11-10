"""Independent rigorous POINT witnesses for the coherent alias-path budget.
A point lower bound may prove failure. Points can NEVER establish continuous PASS.
"""
import argparse,hashlib,json,math,struct,sys,time
from fractions import Fraction as Q
from pathlib import Path

def witnesses(pipeline,points,flint_path,precision=320):
 sys.path.insert(0,str(flint_path));from flint import arb,acb,fmpq,ctx
 if not 192<=precision<=4096:raise ValueError('bounded arbitrary precision required')
 ctx.prec=precision;ctx.threads=1;file=Path(pipeline);plan=json.loads(file.read_text());out=int(plan['output_rate']);high=max(s['coefficient_rate'] for s in plan['stages']);M=high//out
 if M*out!=high or not 1<M<=4096:raise ValueError('unsupported common multirate alias clock')
 stages=[];identities=[]
 for s in plan['stages']:
  raw=(file.parent/s['file']).read_bytes()
  if not raw or len(raw)%8 or len(raw)//8>131072:raise ValueError('invalid coefficient size/budget')
  h=struct.unpack('<'+'d'*(len(raw)//8),raw)
  if not all(math.isfinite(x) for x in h):raise ValueError('non-finite coefficients')
  gain=Q(s['reference_gain']);coeff=[arb(fmpq(*x.as_integer_ratio()))/arb(fmpq(gain.numerator,gain.denominator)) for x in h]
  stride=Q(out*M,s['coefficient_rate'])
  if stride.denominator!=1:raise ValueError('factor rate does not divide the common interpolation grid')
  stages.append((coeff,int(s['coefficient_rate']),int(stride)));identities.append(hashlib.sha256(raw).hexdigest())
 rows=[];started=time.perf_counter();threshold=arb(10)**(-arb(str(plan['stop_target_db']))/20)
 for requested in points:
  f=Q(str(requested))
  if not 0<=f<=Q(str(plan['pass_hz'])):raise ValueError('point lies outside frozen output passband')
  product=[acb(1)]*M
  for coeff,rate,stride in stages:
   folded=[acb(0)]*M
   for n,c in enumerate(coeff):
    argument=-2*f*n/rate
    # Each root is computed independently. Recurrent rectangular complex
    # multiplication/Horner can exhibit exponential interval wrapping at large N.
    root=acb(fmpq(argument.numerator,argument.denominator)).exp_pi_i()
    folded[(stride*n)%M]+=c*root
   transformed=acb.dft(folded);product=[a*b for a,b in zip(product,transformed)]
  lower=sum(z.abs_lower() for z in product[1:]).lower();upper=sum(z.abs_upper() for z in product[1:]).upper()
  valid=lower.is_finite() and upper.is_finite() and lower>0 and upper/lower<arb('1.00000001')
  rows.append({'output_hz':str(f),'rigorous_enclosure_resolved':bool(valid),'alias_amplitude_lower_exact':str(lower.fmpq()) if valid else None,'alias_amplitude_upper_exact':str(upper.fmpq()) if valid else None,
   'db_lower':float(20*lower.log()/arb(10).log()) if valid else None,'db_upper':float(20*upper.log()/arb(10).log()) if valid else None,'budget_failure_proved_by_point_lower':bool(valid and lower>threshold.upper())})
 return {'schema':'rlplayer-alias-point-witness-v1','status':'CONFIRMED_POINT_BUDGET_FAILURE' if any(x['budget_failure_proved_by_point_lower'] for x in rows) else 'POINTS_ONLY_NOT_CONTINUOUS_PASS','pipeline_sha256':hashlib.sha256(file.read_bytes()).hexdigest(),'coefficient_sha256':identities,'coefficient_rate':high,'output_rate':out,'alias_branch_count':M-1,'method':'original exact binary64 coefficients; independent rational trigonometric rotation; reduction modulo alias root order; rigorous Arb DFT; no derivative or correlation intermediates','rows':rows,'elapsed_seconds':time.perf_counter()-started,'precision_bits':precision,'threads':1}

def main():
 p=argparse.ArgumentParser();p.add_argument('--pipeline',required=True,type=Path);p.add_argument('--points',required=True,nargs='+');p.add_argument('--flint-path',required=True,type=Path);p.add_argument('--output',required=True,type=Path);a=p.parse_args()
 try:
  result=witnesses(a.pipeline,a.points,a.flint_path);a.output.write_text(json.dumps(result,indent=2));print(json.dumps(result));return 1 if result['status']=='CONFIRMED_POINT_BUDGET_FAILURE' else 3
 except Exception as e:print(json.dumps({'status':'REJECTED','reason':str(e)}));return 2
if __name__=='__main__':sys.exit(main())
