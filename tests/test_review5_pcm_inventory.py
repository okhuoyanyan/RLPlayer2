"""Structural/identity validation of1343 actual routes with explicit supplements.
This validates the finite manifest, never upgrades points into continuous bands.
"""
import argparse,copy,json,hashlib,math,struct,collections
from pathlib import Path
from fractions import Fraction as F
p=argparse.ArgumentParser();p.add_argument('--manifest',type=Path,required=True);p.add_argument('--supplement',type=Path,required=True);p.add_argument('--output',type=Path,required=True);a=p.parse_args();base=json.loads(a.manifest.read_text());supp=json.loads(a.supplement.read_text());sha=lambda p:hashlib.sha256(Path(p).read_bytes()).hexdigest()
def key(x):return x['preset'],x['input_rate'],x['output_rate']
replace={key(x):x for x in supp['rows']};effective=[replace.get(key(x),x) for x in base['rows']];file_checks=0;finite_values=0
def validate_rows(rows,hash_files=False):
 global file_checks,finite_values
 assert len(rows)==1343 and len({key(x) for x in rows})==1343
 assert len({x['preset'] for x in rows})==79 and {(x['input_rate'],x['output_rate']) for x in rows}=={tuple(x) for x in base['pairs_literal_hz']}
 for r in rows:
  assert r['status']=='ACTUAL_PIPELINE_FINITE_POINT_SCREEN' and r['exit_code']==0
  assert (r['channels'],r['input_bits'],r['source_bits'],r['output_bits'],r['isFloat'])==(2,24,24,24,False)
  assert r['cpu_threads_last_block']<=2 and r['phase_mix']==.5 and r['automatic_taps'] and r['automatic_cascade']
  stages=r['stages'];assert sum(s['count'] for s in stages)==r['planned_coefficient_count'] and len(stages)==r['planned_stage_count']
  ratio=F(1);last=r['input_rate']
  for i,s in enumerate(stages):
   assert s['count']==s['L']*s['T'] and s['reference_gain']==s['L'] and s['input_rate']==last
   assert s['coefficient_rate']==s['input_rate']*s['L']==s['output_rate']*s['M'];ratio*=F(s['L'],s['M']);last=s['output_rate']
   if i:assert not s['useFixed']
   assert 0<=s['pre_peak_energy_fraction']<=1.000000000001 and 0<=s['post_peak_energy_fraction']<=1.000000000001
   if hash_files:
    for f,h in [('double_file','double_sha256'),('fixed_file','fixed_sha256')]:
     if not s.get(f):continue
     path=Path(r['folder'])/s[f];assert path.stat().st_size==s['count']*8 and sha(path)==s[h];file_checks+=1
     if f=='double_file':
      with path.open('rb') as stream:
       while raw:=stream.read(8192*8):
        vals=struct.unpack('<'+'d'*(len(raw)//8),raw);assert all(math.isfinite(v) for v in vals);finite_values+=len(vals)
  assert last==r['output_rate'] and ratio==F(r['output_rate'],r['input_rate'])
  expected=stages[0]['T']*stages[0]['output_rate']*2<=250000000
  assert stages[0]['useFixed']==expected==r['actual_usedFixed']
  # Original window-specific engineering thresholds; never redefine a failure
  # as pass by loosening the numeric target or replacing F1 with0.9*F1.
  target=100 if stages[0]['window']=='gauss' else 280;assert r['declared_stop_target_db']==target
  assert any(q['hz']==r['pass_hz'] and q['kind']=='pass' for q in r['points']) and any(q['hz']==r['mirror_stop_hz'] and q['kind']=='mirror_stop' for q in r['points'])
  assert all(q['kind']==('pass' if q['hz']<=r['pass_hz'] else 'mirror_stop' if q['hz']>=r['mirror_stop_hz'] else 'transition') for q in r['points'])
  assert r['pass_point_max_abs_db']==max(abs(q['desired_response_db']) for q in r['points'] if q['kind']=='pass')
  assert r['mirror_stop_point_max_db']==max(q['desired_response_db'] for q in r['points'] if q['kind']=='mirror_stop')
  margin=max(r['pass_point_max_abs_db']-.01,r['mirror_stop_point_max_db']+target)
  expected_status='POINT_THRESHOLD_FAILURE' if margin>1e-6 else 'INDETERMINATE_NEAR_POINT_THRESHOLD' if margin>-1e-6 else 'POINTS_NO_FAILURE_FOUND_NOT_CONTINUOUS'
  assert r['point_quality_status']==expected_status,'point failure/uncertainty cannot be relabelled as PASS'
  if all(s['phase']=='lp' for s in stages):
   expected_delay=sum((s['count']-1)/2/s['coefficient_rate'] for s in stages);dc=next(q for q in r['points'] if q['hz']==0);assert abs(dc['group_delay_seconds']-expected_delay)<1e-9
validate_rows(effective,True);negative=[]
def fake_pass(rows):
 r=rows[0];point=next(q for q in r['points'] if q['kind']=='pass');point['desired_response_db']=1.;r['pass_point_max_abs_db']=1.
for name,mutate in [('missing-route',lambda x:x.pop()),('duplicate-route',lambda x:x.__setitem__(-1,copy.deepcopy(x[0]))),('wrong-clock',lambda x:x[0]['stages'][0].update(coefficient_rate=1)),('fake-usedFixed',lambda x:x[0].update(actual_usedFixed=not x[0]['actual_usedFixed'])),('weakened-threshold',lambda x:x[0].update(declared_stop_target_db=100)),('F1-replaced-by-point-nine-F1',lambda x:x[0].update(pass_hz=x[0]['pass_hz']*.9)),('bad-point-cannot-retain-PASS-label',fake_pass)]:
 rows=copy.deepcopy(effective);mutate(rows)
 try:validate_rows(rows)
 except AssertionError:negative.append({'case':name,'rejected':True})
 else:raise AssertionError('bad manifest accepted '+name)
index=[]
for original,r in zip(base['rows'],effective):index.append({'preset':r['preset'],'input_rate':r['input_rate'],'output_rate':r['output_rate'],'original_status':original['status'],'supplement_used':key(r) in replace,'effective_status':r['status'],'point_quality_status':r['point_quality_status'],'row_path':str(Path(r['folder'])/'row.json'),'row_sha256':sha(Path(r['folder'])/'row.json')})
summary={'status':'PASS_FINITE_MANIFEST_IDENTITY_NOT_CONTINUOUS','actual_routes':len(effective),'presets':79,'rate_pairs':17,'phase_mix':.5,'original_admission_rows_preserved':len(replace),'artifact_checks':file_checks,'finite_binary64_values_checked':finite_values,'negative_controls':negative,'actual_fixed_routes':sum(r['actual_usedFixed'] for r in effective),'actual_double_routes':sum(not r['actual_usedFixed'] for r in effective),'actual_cascade_routes':sum(r['actual_cascade'] for r in effective),'point_status_counts':dict(collections.Counter(r['point_quality_status'] for r in effective)),'max_peak_WS_bytes':max(r['peak_working_set_bytes'] for r in effective),'max_peak_commit_bytes':max(r['peak_commit_bytes'] for r in effective),'slowest_cold_prepare_seconds':max(r['cold_prepare_seconds'] for r in effective),'original_manifest_sha256':sha(a.manifest),'supplement_sha256':sha(a.supplement),'scope':'1343 actual configurations at default phase_mix=.5 and14/15 fixed physical frequencies; no16N dense grid, no all-rate continuous or full alias proof; per-stage phase statistics not full pipeline impulse of all rows','effective_row_index':index};a.output.write_text(json.dumps(summary,indent=2));print(json.dumps({k:v for k,v in summary.items() if k!='effective_row_index'}))
