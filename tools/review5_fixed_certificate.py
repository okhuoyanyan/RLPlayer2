"""Exact Q62 coefficient-perturbation extension of an existing continuous proof.

For normalized stage transfer H_j, A_j=||h_j||1/L_j and
d_j=||q_j/2^62-h_j||1/L_j, so for every real frequency
 |product(H_j+E_j)-product(H_j)| <= product(A_j+d_j)-product(A_j)=D.
All final decimator paths are counted: alias increases <=(M-1)D;
desired-plus-alias pass bounds enlarge by M D. No frequency sampling.
The existing independent continuous certificate remains a stated premise,
bound by its SHA and the exact original coefficient identities. This tool
independently recomputes every perturbation from ACTUAL private Q62 tables.
Runtime int128-to-double/output rounding is expressly outside this theorem.
"""
import argparse,hashlib,json,math,struct,sys
from fractions import Fraction as F
from pathlib import Path
def sha(p):return hashlib.sha256(Path(p).read_bytes()).hexdigest()
def half_away(v):
 a=abs(v);n=(2*a.numerator+a.denominator)//(2*a.denominator)
 return -n if v<0 else n
def prove(actual_path,base_path,expected_base_sha,flint_path):
 actual_path=Path(actual_path);base_path=Path(base_path);a=json.loads(actual_path.read_text());p=json.loads(base_path.read_text());folder=actual_path.parent
 if sha(base_path)!=expected_base_sha:raise ValueError('base continuous proof SHA changed')
 if p['status']!='PASS' or not p['coherent_alias_budget_pass'] or not p['joint_pass_ripple_pass'] or not p.get('joint_stop_continuous_pass',p.get('continuous_stop_pass',False)):raise ValueError('base proof must be an independently verified continuous PASS')
 if a.get('stats_usedFixed') is not True or a.get('channels')!=2 or a.get('bits')!=24 or a.get('fixed_coefficient_bits')!=62:raise ValueError('actual eligible 2ch24 Q62 path identity absent')
 stages=a['stages'];hashes=[s['double_sha256'] for s in stages]
 if hashes!=p.get('factor_coefficient_sha256',[p.get('coefficient_sha256')]):raise ValueError('actual production coefficients differ from continuous proof')
 M=stages[-1]['M'];source=a['input_rate'];out=a['output_rate']
 if p['alias_path_ids']!=list(range(1,M)) or M*out!=stages[-1]['coefficient_rate']:raise ValueError('all final decimator branches/physical clocks required')
 if stages[0]['input_rate']!=source or stages[-1]['output_rate']!=out:raise ValueError('actual endpoints inconsistent')
 A=F(1);AQ=F(1);results=[];nonzero=0
 for index,s in enumerate(stages):
  if s['coefficient_rate']!=s['input_rate']*s['L'] or s['coefficient_rate']!=s['output_rate']*s['M'] or s['reference_gain']!=s['L']:raise ValueError('stage clocks/reference gain inconsistent')
  if index and s['input_rate']!=stages[index-1]['output_rate']:raise ValueError('stage topology discontinuous')
  raw=(folder/s['double_file']).read_bytes()
  if sha(folder/s['double_file'])!=s['double_sha256'] or len(raw)!=s['count']*8:raise ValueError('original binary64 identity/length changed')
  h=struct.unpack('<'+'d'*s['count'],raw)
  if not all(math.isfinite(x) for x in h):raise ValueError('nonfinite coefficient')
  hf=[F.from_float(x) for x in h];gain=F(s['L']);norm=sum(map(abs,hf),F())/gain;errs=[F()]*len(hf)
  if s['useFixed']:
   nonzero+=1
   if s['actual_fixed_table_phases']!=s['L'] or s['actual_fixed_history_channels']!=2:raise ValueError('actual fixed table/kernel execution not observed')
   qr=(folder/s['fixed_file']).read_bytes()
   if sha(folder/s['fixed_file'])!=s['fixed_sha256'] or len(qr)!=s['count']*8:raise ValueError('actual Q62 identity/length changed')
   q=struct.unpack('<'+'q'*s['count'],qr)
   for i,(x,v) in enumerate(zip(hf,q)):
    if half_away(x*(1<<62))!=v:raise ValueError('actual Q62 differs from exact production llround quantizer')
    errs[i]=abs(F(v,1<<62)-x)
  elif s.get('fixed_file') is not None or s['actual_fixed_table_phases']!=0:raise ValueError('unexpected Q62 object for double stage')
  phase=[sum(errs[k::s['L']],F()) for k in range(s['L'])];raw_l1=sum(phase,F());delta=raw_l1/gain;A*=norm;AQ*=norm+delta
  results.append({'index':index+1,'useFixed':s['useFixed'],'L':s['L'],'T':s['T'],'coefficient_count':s['count'],'double_sha256':s['double_sha256'],'Q62_sha256':s.get('fixed_sha256'),'nonzero_coefficient_errors':sum(bool(e) for e in errs),'raw_L1_error_exact':str(raw_l1),'normalized_L1_error_exact':str(delta),'per_phase_raw_L1_errors_exact':[str(v) for v in phase],'max_phase_raw_L1_error_exact':str(max(phase)),'normalized_original_L1_exact':str(norm)})
 if nonzero!=1 or not stages[0]['useFixed']:raise ValueError('this production audit requires fixed first/single stage; no invented stage2 fixed')
 D=AQ-A;alias=F(p['coherent_alias_amplitude_upper_exact'])+(M-1)*D
 power=F(p.get('joint_stop_power_upper_exact',p.get('stop_power_upper_exact')));limit=F(1,10**14)
 stop_ok=D<limit and power<(limit-D)**2;alias_ok=alias<limit
 lo=F(p['pass_amplitude_lower_exact'])-M*D;hi=F(p['pass_amplitude_upper_exact'])+M*D
 sys.path.insert(0,str(flint_path));from flint import arb,fmpq,ctx
 ctx.prec=320;ctx.threads=1
 exact=lambda x:arb(fmpq(x.numerator,x.denominator))
 ripple=arb(10)**(arb('0.01')/20);pass_ok=bool(exact(lo)>(1/ripple).upper() and exact(hi)<ripple.lower())
 stop=(exact(power).sqrt()+exact(D)).upper();alias_arb=exact(alias)
 result={'schema':'rlplayer-actual-Q62-continuous-extension-v1','status':'PASS' if stop_ok and alias_ok and pass_ok else 'UNRESOLVED','preset':a['preset'],'input_rate':source,'output_rate':out,'channels':2,'source_bits':24,'actual_stats_usedFixed':True,'actual_sha256':sha(actual_path),'base_continuous_proof_path':str(base_path),'base_continuous_proof_sha256':expected_base_sha,'all_alias_path_ids':list(range(1,M)),'alias_branch_count':M-1,'target_db':280,'stage_errors':results,'uniform_normalized_cascade_error_exact':str(D),'alias_increase_bound_exact':str((M-1)*D),'total_pass_enlargement_exact':str(M*D),'new_coherent_alias_amplitude_upper_exact':str(alias),'new_pass_lower_exact':str(lo),'new_pass_upper_exact':str(hi),'continuous_stop_pass':bool(stop_ok),'all_alias_sum_pass':bool(alias_ok),'total_pass_ripple_pass':pass_ok,'stop_db_upper':float(20*stop.log()/arb(10).log()),'all_alias_sum_db_upper':float(20*alias_arb.log()/arb(10).log()),'scope':'CONTINUOUS ideal transfer of actual Q62 first/single stage plus actual binary64 stage2; exact L1 perturbation extension; excludes runtime output rounding, hardware, other presets/rates','independent_method':'exact Fraction per original binary64 and actual signed-int64 table; exact phase/full L1, gain and all-path propagation; no sampled extrema'}
 return result
def main():
 p=argparse.ArgumentParser();p.add_argument('--actual',required=True,type=Path);p.add_argument('--base-proof',required=True,type=Path);p.add_argument('--base-proof-sha256',required=True);p.add_argument('--flint-path',required=True,type=Path);p.add_argument('--output',required=True,type=Path);a=p.parse_args()
 try:r=prove(a.actual,a.base_proof,a.base_proof_sha256,a.flint_path);a.output.write_text(json.dumps(r,indent=2));print(json.dumps({k:v for k,v in r.items() if k not in ['stage_errors','all_alias_path_ids']}));return 0 if r['status']=='PASS' else 3
 except Exception as e:print(json.dumps({'status':'REJECTED','reason':str(e)}));return 2
if __name__=='__main__':sys.exit(main())
