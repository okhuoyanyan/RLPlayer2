"""Single-rate alias path count and wrong-clock/empty-coverage rejection."""
import argparse,copy,importlib.util,json,struct,sys
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--flint-path',required=True,type=Path);p.add_argument('--evidence',required=True,type=Path);a=p.parse_args();a.evidence.mkdir(parents=True,exist_ok=True)
root=Path(__file__).resolve().parents[1];sys.path.insert(0,str(root/'tools'))
from review5_single_rate_certificate import compose
from review5_cascade_certificate import Bounds
from review5_stream_certificate import generate
from review5_stream_verify import verify
h=a.evidence/'stage1.f64le';h.write_bytes(struct.pack('<d',.2));spec={'reference_gain':'2','bands':[{'kind':'envelope','lo':'0','hi':'1/2','max_amplitude':'1'}]};cert=generate(h,spec,a.flint_path,grid=64);cp=a.evidence/'stage1.certificate.json';cp.write_text(json.dumps(cert));assert verify(h,spec,cert,flint_path=a.flint_path)['status']=='PASS'
plan={'input_rate':3,'output_rate':2,'pass_hz':.25,'stop_hz':.5,'stop_target_db':16,'stages':[{'file':'stage1.f64le','input_rate':3,'output_rate':2,'L':2,'M':3,'coefficient_rate':6,'reference_gain':'2'}]};pp=a.evidence/'pipeline.json';pp.write_text(json.dumps(plan));b=Bounds(cp);r=compose(pp,b,a.flint_path,bins=8);checks=[]
def ck(ok,name):checks.append({'name':name,'pass':bool(ok)});print(('PASS ' if ok else 'FAIL ')+name)
ck(r['alias_path_ids']==[1,2],'M3_has_all_two_non_main_paths')
ck(r['continuous_stop_pass'] and not r['coherent_alias_budget_pass'] and r['status']!='PASS','per_path_minus20_does_not_prove_coherent_sum_minus16')
for name,mutate in [('omitted_path',lambda x:x['alias_path_ids'].pop()),('forged_alias',lambda x:x.update(coherent_alias_amplitude_upper_exact='0')),('wrong_output_clock_identity',lambda x:x.update(pipeline_sha256='0'*64))]:
 bad=copy.deepcopy(r);mutate(bad);rejected=False
 try:compose(pp,b,a.flint_path,bins=8,claimed=bad)
 except ValueError:rejected=True
 ck(rejected,name+'_rejected')
for bins in [0,-1]:
 rejected=False
 try:compose(pp,b,a.flint_path,bins=bins)
 except ValueError:rejected=True
 ck(rejected,'empty_or_negative_frequency_coverage_rejected')
wrong=copy.deepcopy(plan);wrong['stages'][0]['coefficient_rate']=wrong['input_rate'];wp=a.evidence/'wrong-clock.json';wp.write_text(json.dumps(wrong));rejected=False
try:compose(wp,b,a.flint_path,bins=8)
except ValueError:rejected=True
ck(rejected,'source_clock_cannot_replace_interpolation_or_output_clock')
no_alias=copy.deepcopy(plan);no_alias['output_rate']=6;no_alias['stages'][0]['output_rate']=6;no_alias['stages'][0]['M']=1;np=a.evidence/'no-decimation.json';np.write_text(json.dumps(no_alias));nr=compose(np,b,a.flint_path,bins=8)
ck(nr['alias_branch_count']==0 and nr['coherent_alias_exact_zero'] and nr['coherent_alias_db_upper']=='-inf','M1_exact_zero_alias_has_valid_JSON_not_NaN_or_Infinity')
summary={'checks':checks,'passed':sum(x['pass'] for x in checks),'failed':sum(not x['pass'] for x in checks)};(a.evidence/'controls.json').write_text(json.dumps(summary,indent=2));print(json.dumps(summary));sys.exit(bool(summary['failed']))
