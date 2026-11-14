"""Full alias enumeration/coherent +6.02 dB and tamper controls."""
import argparse,copy,importlib.util,json,math,struct,sys
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--flint-path',required=True,type=Path);p.add_argument('--evidence',required=True,type=Path);a=p.parse_args();a.evidence.mkdir(parents=True,exist_ok=True)
root=Path(__file__).resolve().parents[1]
def load(name,path):s=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(s);s.loader.exec_module(m);return m
g=load('streamgen',root/'tools/review5_stream_certificate.py');v=load('streamverify',root/'tools/review5_stream_verify.py');c=load('composition',root/'tools/review5_cascade_certificate.py')
checks=[]
def ck(ok,name):checks.append({'name':name,'pass':bool(ok)});print(('PASS ' if ok else 'FAIL ')+name)
# Two actual decimator alias branches, both constant positive 0.01 in gain,
# add coherently at the DC-input image. Per-path -40 dB is not a -36 dB sum.
spec={'reference_gain':'2','bands':[{'kind':'envelope','lo':'0','hi':'1/2','max_amplitude':'1'}]}
for k in [1,2]:
 h=a.evidence/f'stage{k}.f64le';h.write_bytes(struct.pack('<d',.2));cert=g.generate(h,spec,a.flint_path,grid=64);cp=a.evidence/f'stage{k}.certificate.json';cp.write_text(json.dumps(cert));verified=v.verify(h,spec,cert,flint_path=a.flint_path);ck(verified['status']=='PASS',f'independent_factor_{k}')
plan={'input_rate':3,'output_rate':4,'pass_hz':.5,'stop_hz':1,'stop_target_db':36,'stages':[{'file':'stage1.f64le','input_rate':3,'output_rate':6,'L':2,'M':1,'coefficient_rate':6,'reference_gain':'2'},{'file':'stage2.f64le','input_rate':6,'output_rate':4,'L':2,'M':3,'coefficient_rate':12,'reference_gain':'2'}]}
pipeline=a.evidence/'pipeline.json';pipeline.write_text(json.dumps(plan));first=c.Bounds(a.evidence/'stage1.certificate.json');second=c.Bounds(a.evidence/'stage2.certificate.json')
result=c.compose(pipeline,first,second,a.flint_path,bins=8);(a.evidence/'coherent-proof.json').write_text(json.dumps(result,indent=2))
ck(result['alias_path_ids']==[1,2] and result['alias_branch_count']==2,'every_decimator_alias_branch_enumerated')
ck(result['joint_stop_continuous_pass'] and not result['coherent_alias_budget_pass'] and result['status']!='PASS','per_path_minus40_dB_does_not_certify_coherent_sum_minus36_dB')
ck(abs(result['coherent_alias_db_upper']-20*math.log10(.02))<1e-6,'coherent_two_path_plus6_020599913_dB')
for name,mutation in [('forged_coherent_sum',lambda x:x.update(coherent_alias_amplitude_upper_exact='0')),('forged_joint_stop',lambda x:x.update(joint_stop_power_upper_exact='0')),('omitted_alias',lambda x:x['alias_path_ids'].pop()),('changed_plan',lambda x:x.update(pipeline_sha256='0'*64)),('changed_factor_sha',lambda x:x['factor_coefficient_sha256'].__setitem__(0,'0'*64)),('changed_factor_proof',lambda x:x['factor_certificate_sha256'].__setitem__(0,'0'*64))]:
 bad=copy.deepcopy(result);mutation(bad);rejected=False
 try:c.compose(pipeline,first,second,a.flint_path,bins=8,claimed=bad)
 except ValueError:rejected=True
 ck(rejected,name+'_rejected')
rejected=False
try:c.compose(pipeline,first,second,a.flint_path,bins=0)
except ValueError:rejected=True
ck(rejected,'zero_output_intervals_cannot_forge_empty_domain_PASS')
# Exercise wrap/fold boundaries: a cell spanning a period must not be sampled at
# one midpoint, and every folded piece must stay inside the real-FIR half circle.
folded=c.folded_intervals(c.Q(5),c.Q(13),c.Q(6));ck(len(folded)>=4 and all(0<=lo<=hi<=c.Q(1,2) for lo,hi in folded),'all_period_and_conjugate_folds_retained')
summary={'checks':checks,'passed':sum(x['pass'] for x in checks),'failed':sum(not x['pass'] for x in checks),'analytic_per_path_amplitude':.01,'analytic_coherent_sum':.02,'reported_upper_db':result['coherent_alias_db_upper']};(a.evidence/'controls.json').write_text(json.dumps(summary,indent=2));print(json.dumps(summary));sys.exit(bool(summary['failed']))
