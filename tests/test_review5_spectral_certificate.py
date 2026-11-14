"""Executable red controls: finite-grid pass must not become continuous PASS."""
import argparse,copy,importlib.util,json,math,struct,sys,tempfile
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--flint-path',required=True,type=Path);p.add_argument('--evidence',required=True,type=Path);a=p.parse_args();a.evidence.mkdir(parents=True,exist_ok=True)
root=Path(__file__).resolve().parents[1]
def load(name,path):
 s=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(s);s.loader.exec_module(m);return m
generator=load('review5_generator',root/'tools/review5_spectral_certificate.py');checker=load('review5_checker',root/'tools/review5_spectral_verify.py')
streamgen=load('review5_streamgen',root/'tools/review5_stream_certificate.py');streamcheck=load('review5_streamcheck',root/'tools/review5_stream_verify.py')
sys.path.insert(0,str(a.flint_path));from flint import arb,acb,ctx,fmpq
checks=[]
def ck(ok,name):checks.append({'name':name,'pass':bool(ok)});print(('PASS' if ok else 'FAIL')+' '+name,flush=True)
def coef(name,h):
 path=a.evidence/(name+'.f64');path.write_bytes(struct.pack('<'+'d'*len(h),*h));return path
def run(name,h,spec,expect='PASS',**options):
 c=coef(name,h);cert=generator.certify(c,spec,flint_path=a.flint_path,**options);(a.evidence/(name+'.certificate.json')).write_text(json.dumps(cert,indent=2));
 result=checker.verify(c,spec,cert,flint_path=a.flint_path);(a.evidence/(name+'.verified.json')).write_text(json.dumps(result,indent=2));ck(cert['status']==expect and result['status']==expect,name+' generator_and_independent_verifier_'+expect);return c,spec,cert,result
unity={'normalize_dc':False,'bands':[{'kind':'pass','lo':'0','hi':'1/2','ripple_db':'0.01'}]}
base=run('exact_unity',[1.0],unity)
scaled=copy.deepcopy(unity);scaled['reference_gain']='3'
run('non_power_of_two_gain_exact_rational',[3.0],scaled)
normalized=copy.deepcopy(unity);normalized['normalize_dc']=True
run('exact_DC_normalization',[.1,.2,.7],{'normalize_dc':True,'bands':[{'kind':'pass','lo':'0','hi':'1/10000','ripple_db':'0.01'}]})
run('analytic_three_tap',[.25,.5,.25],{'bands':[{'kind':'stop','lo':'1/3','hi':'1/2','db':'-10'}]})
# Non-symmetric phase shapes must be accepted by the algorithm, without LP-only assumptions.
run('asymmetric_phase',[.75,.25],{'bands':[{'kind':'pass','lo':'0','hi':'1/1000','ripple_db':'0.01'}]})
# |H|=|sin(2*pi*f)|/2 has its exact maximum 1/2 at f=1/4.
# Coarse f=0,1/6,1/3,1/2 samples all below the -7 dB threshold.
h=[.25,0,-.25];coarse=max(abs(sum(x*complex(math.cos(-2*math.pi*k*n/6),math.sin(-2*math.pi*k*n/6)) for n,x in enumerate(h))) for k in range(4))
ck(coarse<10**(-7/20) and .5>10**(-7/20),'analytic_off_grid_peak_is_hidden_from_coarse_samples')
run('analytic_offgrid_peak',h,{'bands':[{'kind':'stop','lo':'0','hi':'1/2','db':'-7'}]},'FAIL')
# A 16*N grid also misses a deliberately near-threshold off-grid narrow lobe.
import numpy as np
n=64;g=16*n;freq=(g//4+.5)/g
h=np.cos(2*np.pi*freq*(np.arange(n)-(n-1)/2))*2/n
sampled=np.max(np.abs(np.fft.rfft(h,g)));point=abs(np.sum(h*np.exp(-2j*np.pi*freq*np.arange(n))))
scale=1e-14/((sampled+point)/2);h=(h*scale).tolist()
ck(sampled*scale<1e-14 and point*scale>1e-14,'16N_grid_passes_but_independent_off_grid_witness_exceeds_minus280')
_,_,_,r=run('narrow_16N_offgrid_peak',h,{'bands':[{'kind':'stop','lo':'0','hi':'1/2','db':'-280'}]},'FAIL')
run('fake_deep_stop',[1.0],{'bands':[{'kind':'stop','lo':'1/10','hi':'1/2','db':'-280'}]},'FAIL')
# Cross-calibrate the streamed algorithm against both independent evaluators.
for name,h,spec,expect in [
 ('stream_unity',[1.0],unity,'PASS'),
 ('stream_analytic',[.25,.5,.25],{'bands':[{'kind':'stop','lo':'1/3','hi':'1/2','db':'-10'}]},'PASS'),
 ('stream_analytic_offgrid',[.25,0,-.25],{'bands':[{'kind':'stop','lo':'0','hi':'1/2','db':'-7'}]},'FAIL')]:
 c=coef(name,h);cert=streamgen.generate(c,spec,a.flint_path,grid=256,degree=40)
 r1=checker.verify(c,spec,cert,flint_path=a.flint_path);r2=streamcheck.verify(c,spec,cert,flint_path=a.flint_path)
 ck(cert['status']==expect and r1['status']==expect and r2['status']==expect,name+'_Horner_and_symmetric_DFT_crosscheck')
# Reject non-finite binary64 even when an attacker recomputes the file hash.
for name,v in [('nan',float('nan')),('infinity',float('inf'))]:
 c=coef(name,[v]);rejected=False
 try:generator.certify(c,unity,flint_path=a.flint_path)
 except ValueError:rejected=True
 ck(rejected,name+'_rejected_before_numeric_work')
# The verifier reads the original external specification, never a self-approved certificate.
c,spec,cert,_=base
for name,mutate in [
 ('forged_overall_status',lambda x:x.update(status='FAIL')),
 ('forged_interval_status',lambda x:x['leaves'][0].update(status='FAIL')),
 ('wrong_coefficient_hash',lambda x:x.update(coefficient_sha256='0'*64)),
 ('deleted_frequency_cell',lambda x:x['leaves'].pop(0)),
 ('forged_rounding_bound',lambda x:x['leaves'][0].update(upper_power='0')),
 ('changed_fixed_band',lambda x:x.update(spec_sha256='1'*64)),
 ('invalid_remainder_degree',lambda x:x.update(degree=0)),
 ('changed_grid_identity',lambda x:x.update(grid=x['grid']*2)),
]:
 bad=copy.deepcopy(cert);mutate(bad);rejected=False
 try:checker.verify(c,spec,bad,flint_path=a.flint_path)
 except (ValueError,KeyError):rejected=True
 ck(rejected,name+'_independently_rejected')
# Bound exhaustion must remain INDETERMINATE, never sampled PASS.
c=coef('budget',[.25,.5,.25]);result=generator.certify(c,unity,flint_path=a.flint_path,max_grid=16)
ck(result['status']=='INDETERMINATE','explicit_budget_does_not_forge_a_certificate')
summary={'checks':checks,'pass_count':sum(x['pass'] for x in checks),'fail_count':sum(not x['pass'] for x in checks),'offgrid_16N':{'grid':g,'taps':n,'frequency':freq,'sampled_max':sampled*scale,'point_witness':point*scale,'threshold':1e-14}}
(a.evidence/'controls.json').write_text(json.dumps(summary,indent=2));print(json.dumps(summary));sys.exit(bool(summary['fail_count']))
