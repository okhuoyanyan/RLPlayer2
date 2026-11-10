"""Continuous actual single-stage rational resampler + all M decimator paths."""
import argparse,hashlib,json,math,sys,time,gc
from fractions import Fraction as Q
from pathlib import Path
from review5_cascade_certificate import Bounds,factor_range

def compose(pipeline,bounds,flint_path,bins=1024,claimed=None):
 if type(bins) is not int or not 1<=bins<=65536:raise ValueError('positive bounded output interval count required')
 sys.path.insert(0,str(flint_path));from flint import arb,fmpq,ctx
 ctx.prec=320;ctx.threads=1;path=Path(pipeline);plan=json.loads(path.read_text());stages=plan['stages']
 if len(stages)!=1:raise ValueError('single actual stage required')
 stage=stages[0];high=int(stage['coefficient_rate']);out=int(plan['output_rate']);source=int(plan['input_rate']);L=int(stage['L']);M=int(stage['M'])
 if high!=source*L or high!=out*M or math.gcd(L,M)!=1 or stage['input_rate']!=source or stage['output_rate']!=out:raise ValueError('actual physical sample clocks/L/M inconsistent')
 ids=list(range(1,M));pipeline_sha=hashlib.sha256(path.read_bytes()).hexdigest()
 if claimed is not None:
  if claimed['alias_path_ids']!=ids or claimed['pipeline_sha256']!=pipeline_sha or claimed['coefficient_sha256']!=bounds.hash or claimed['certificate_sha256']!=bounds.certificate_sha or claimed['output_frequency_intervals']!=bins:raise ValueError('omitted alias path or changed Plan/factor identity')
 pass_hz=Q(str(plan['pass_hz']));stop_hz=Q(str(plan['stop_hz']))
 if not 0<pass_hz<stop_hz<Q(high,2):raise ValueError('invalid frozen physical bands')
 exact=lambda x:arb(fmpq(x.numerator,x.denominator));started=time.perf_counter();stop_power=Q(0)
 for left,right,lower,upper in bounds.rows:
  if right*high>=stop_hz:stop_power=max(stop_power,upper)
 alias=arb(0);pass_lo=arb(1);pass_hi=arb(0);worst=0
 for i in range(bins):
  left=pass_hz*i/bins;right=pass_hz*(i+1)/bins
  lo,hi=factor_range(bounds,left,right,high);lower=exact(lo).sqrt().lower();upper=exact(hi).sqrt().upper();total=arb(0)
  for k in ids:
   _,p=factor_range(bounds,left+k*out,right+k*out,high)
   total+=exact(p).sqrt().upper()
  total=total.upper()
  if alias<total:alias=total;worst=i
  if lower-total<pass_lo:pass_lo=(lower-total).lower()
  if upper+total>pass_hi:pass_hi=(upper+total).upper()
 threshold=arb(10)**(-arb(str(plan['stop_target_db']))/20);ripple=arb(10)**(arb('0.01')/20)
 stop_ok=exact(stop_power)<(threshold*threshold).lower();alias_ok=alias<threshold.lower();pass_ok=pass_lo>(1/ripple).upper() and pass_hi<ripple.lower()
 status='PASS' if stop_ok and alias_ok and pass_ok else 'INDETERMINATE'
 if claimed is not None:
  if Q(claimed['stop_power_upper_exact'])<stop_power or Q(claimed['coherent_alias_amplitude_upper_exact'])<Q(str(alias.fmpq())):raise ValueError('forged stop or alias envelope')
  if claimed['status']=='PASS' and status!='PASS':raise ValueError('unproved PASS')
 return {'schema':'rlplayer-continuous-single-rate-v1','status':status,'pipeline_sha256':pipeline_sha,'coefficient_sha256':bounds.hash,'certificate_sha256':bounds.certificate_sha,
  'L':L,'M':M,'coefficient_rate':high,'output_rate':out,'alias_path_ids':ids,'alias_branch_count':len(ids),'output_frequency_intervals':bins,
  'continuous_stop_pass':bool(stop_ok),'coherent_alias_budget_pass':bool(alias_ok),'joint_pass_ripple_pass':bool(pass_ok),'stop_db_upper':float(10*exact(stop_power).log()/arb(10).log()),
  'coherent_alias_exact_zero':bool(alias==0),'coherent_alias_db_upper':float(20*alias.log()/arb(10).log()) if alias>0 else '-inf','stop_power_upper_exact':str(stop_power),'coherent_alias_amplitude_upper_exact':str(alias.fmpq()),
  'pass_amplitude_lower_exact':str(pass_lo.fmpq()),'pass_amplitude_upper_exact':str(pass_hi.fmpq()),'worst_alias_interval_hz':[str(pass_hz*worst/bins),str(pass_hz*(worst+1)/bins)],
  'elapsed_seconds':time.perf_counter()-started,'scope':'entire frozen FIR bands plus all final-decimator alias paths on [0,pass_hz]; exact coefficient transfer, not runtime accumulation'}

def main():
 p=argparse.ArgumentParser();p.add_argument('--pipeline',required=True,type=Path);p.add_argument('--spec',required=True,type=Path);p.add_argument('--certificate',required=True,type=Path);p.add_argument('--flint-path',required=True,type=Path);p.add_argument('--output',required=True,type=Path);p.add_argument('--check-proof',type=Path);p.add_argument('--factor-output',type=Path);p.add_argument('--bins',type=int,default=1024);a=p.parse_args()
 try:
  from review5_stream_verify import verify,process_memory
  plan=json.loads(a.pipeline.read_text());stage=plan['stages'][0];spec=json.loads(a.spec.read_text())
  if len(plan['stages'])!=1 or spec.get('normalize_dc',False) or Q(str(spec.get('reference_gain','1')))!=stage['L']:raise ValueError('actual gain must be exact L for this single stage')
  if a.certificate.stat().st_size>128*1024**2:raise ValueError('explicit certificate input-size budget exhausted')
  c=json.loads(a.certificate.read_text());result=verify(a.pipeline.parent/stage['file'],spec,c,flint_path=a.flint_path)
  if result['status']!='PASS':raise ValueError('factor independently failed')
  result['process_memory']=process_memory();result['coefficient_sha256']=c['coefficient_sha256'];result['certificate_sha256']=hashlib.sha256(a.certificate.read_bytes()).hexdigest()
  if a.factor_output:a.factor_output.write_text(json.dumps(result,indent=2))
  del c;gc.collect()
  result=compose(a.pipeline,Bounds(a.certificate),a.flint_path,a.bins,json.loads(a.check_proof.read_text()) if a.check_proof else None)
  result['process_memory']=process_memory();a.output.write_text(json.dumps(result,indent=2));print(json.dumps(result));return 0 if result['status']=='PASS' else 3
 except Exception as e:print(json.dumps({'status':'REJECTED','reason':str(e)}));return 2
if __name__=='__main__':sys.exit(main())
