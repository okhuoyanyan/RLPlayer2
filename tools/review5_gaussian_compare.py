"""Summarize factual finite-point differences; never assigns continuous PASS."""
import json,argparse
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('dataset',type=Path);a=p.parse_args();d=json.loads(a.dataset.read_text());rows=d['rows'];pairs=[]
for b in rows:
 if b['version']!='baseline':continue
 c=next(x for x in rows if x['version']=='candidate' and all(x[k]==b[k] for k in ('preset','input_rate','output_rate')))
 q={k:b[k] for k in ('preset','input_rate','output_rate')};q.update(baseline_status=b['status'],candidate_status=c['status'],source_support_preserved=b['source_support_T']==c['source_support_T'])
 if 'points' in b and 'points' in c:
  bp={x['hz']:x for x in b['points']};cp={x['hz']:x for x in c['points']};assert bp.keys()==cp.keys()
  passf=[f for f in bp if f<=b['pass_hz']];stopf=[f for f in bp if f>=b['mirror_stop_hz']]
  q.update(baseline_pass_max_abs_db=max(abs(bp[f]['desired_component_db']) for f in passf),candidate_pass_max_abs_db=max(abs(cp[f]['desired_component_db']) for f in passf),baseline_stop_max_db=max(bp[f]['desired_component_db'] for f in stopf),candidate_stop_max_db=max(cp[f]['desired_component_db'] for f in stopf),max_pass_point_change_db=max(abs(cp[f]['desired_component_db']-bp[f]['desired_component_db']) for f in passf),stop_point_max_worsening_db=max(cp[f]['desired_component_db']-bp[f]['desired_component_db'] for f in stopf),baseline_coefficient_count=b['coefficient_count'],candidate_coefficient_count=c['coefficient_count'],baseline_peak_ws=b['peak_working_set_bytes'],candidate_peak_ws=c['peak_working_set_bytes'],baseline_cold_seconds=b['cold_prepare_seconds'],candidate_cold_seconds=c['cold_prepare_seconds'])
  q['warnings']=[]
  if q['max_pass_point_change_db']>.001:q['warnings'].append('pass-point delta >0.001dB; investigate, not automatically quality failure')
  if q['candidate_stop_max_db']>q['baseline_stop_max_db']+.1:q['warnings'].append('maximum measured stop point worsens >0.1dB; no nonregression claim')
 else:q['reason']='baseline cannot be measured within explicit native/admission budget; no invented baseline quality'
 pairs.append(q)
out={'scope':'FINITE_POINTS_AND_ACTUAL_ONE_PHASE_IMPULSE_ONLY; shortest search and continuous Gaussian nonregression NOT PROVEN','rows':len(rows),'pairs':len(pairs),'all_source_support_preserved':all(x['source_support_preserved'] for x in pairs),'successful_candidate_preparations':sum(x['version']=='candidate' and x['status']=='ACTUAL_PIPELINE_POINT_SCREEN' for x in rows),'status_counts':{v:{s:sum(x['version']==v and x['status']==s for x in rows) for s in sorted({x['status'] for x in rows})} for v in ['baseline','candidate']},'comparisons':pairs}
a.dataset.with_name('comparison.json').write_text(json.dumps(out,indent=2));print(json.dumps({k:v for k,v in out.items() if k!='comparisons'}));print(json.dumps([x for x in pairs if x.get('warnings')],indent=2))
