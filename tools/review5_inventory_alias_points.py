"""Deduplicated strict point screening; never a continuous-band PASS table."""
import argparse,hashlib,json,sys,time
from pathlib import Path
from review5_alias_point_witness import witnesses
p=argparse.ArgumentParser();p.add_argument('--inventory',required=True,type=Path);p.add_argument('--flint-path',required=True,type=Path);p.add_argument('--stop-file',type=Path);a=p.parse_args();base=a.inventory.parent;data=json.loads(a.inventory.read_text());memo={};rows=[];started=time.perf_counter()
for row in data['rows']:
 if a.stop_file and a.stop_file.exists():break
 folder=base/f"preset-{row['catalog_index']:03d}"
 plan={'schema':1,'preset':row['preset'],'input_rate':row['input_rate'],'output_rate':row['output_rate'],'pass_hz':row['pass_hz'],'stop_hz':row['mirror_stop_hz'],'stop_target_db':row['declared_stop_target_db'],'stages':row['stages']}
 path=folder/'alias-point-plan.json';path.write_text(json.dumps(plan))
 key=(tuple(s['sha256'] for s in row['stages']),str(row['pass_hz']),str(row['declared_stop_target_db']))
 if key in memo:result=rows[memo[key]]['result'];reused=memo[key]
 else:result=witnesses(path,[0,row['pass_hz']],a.flint_path);memo[key]=len(rows);reused=None
 entry={'preset':row['preset'],'catalog_index':row['catalog_index'],'coefficient_sha256':[s['sha256'] for s in row['stages']],'reused_identical_coefficients_domains_target_from_row':reused,'result':result}
 rows.append(entry)
 summary={'status':'POINT_SCREEN_ONLY_NOT_CONTINUOUS_PASS','completed_rows':len(rows),'catalog_count':len(data['rows']),'unique_numeric_cases':len(memo),'confirmed_point_failures':sum(x['result']['status']=='CONFIRMED_POINT_BUDGET_FAILURE' for x in rows),'elapsed_seconds':time.perf_counter()-started,'rows':rows}
 (base/'alias-point-screen.json').write_text(json.dumps(summary,indent=2))
 print(len(rows),row['preset'],result['status'],[p['db_lower'] for p in result['rows']], 'reuse',reused,flush=True)
