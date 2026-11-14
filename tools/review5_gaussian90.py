"""Serial 15 Gaussian presets x 3 rates x 2 actual Pipeline builds.
Rejected preparations remain factual rows, never fabricated frequency responses.
"""
import argparse,hashlib,json,subprocess,time,difflib
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--baseline',type=Path,required=True);p.add_argument('--candidate',type=Path,required=True);p.add_argument('--output',type=Path,required=True);a=p.parse_args();a.output.mkdir(parents=True,exist_ok=True)
def sha(p):
 h=hashlib.sha256()
 with p.open('rb') as f:
  while b:=f.read(1048576):h.update(b)
 return h.hexdigest()
catalog=a.output/'catalog.json';subprocess.run([str(a.candidate),'--catalog',str(catalog)],check=True);names=json.loads(catalog.read_text());assert len(names)==15,names
roots={'baseline':Path('D:/RLPlayer-Develop-20261103/build-cache/gaussian-baseline-src/src'),'candidate':Path('D:/RLPlayer-Develop-20261103/source/src')}
identity={}
for version,root in roots.items():
 identity[version]={'binary_sha256':sha(getattr(a,version)),'source_files':{str(f.relative_to(root)):sha(f) for f in sorted(root.rglob('*.h'))},'compile_flags':'clang++ -std=c++20 -O2 -ffp-contract=off; CPU scalar measurement; no device'}
old=Path('D:/RLPlayer-Develop-20261103/baseline/src/engine/pipeline.h');copy=roots['baseline']/'engine/pipeline.h';diff=''.join(difflib.unified_diff(old.read_text(encoding='utf-8-sig').splitlines(True),copy.read_text(encoding='utf-8-sig').splitlines(True),fromfile='immutable baseline',tofile='instrumented baseline'))
(a.output/'baseline-instrumentation.diff').write_text(diff);identity['baseline']['instrumentation_diff']=diff;identity['baseline']['original_pipeline_sha256']=sha(old)
(a.output/'identity.json').write_text(json.dumps(identity,indent=2));rows=[];start=time.perf_counter()
for name in names:
 for ir,orr in [(44100,96000),(44100,384000),(96000,384000)]:
  for version in ('baseline','candidate'):
   folder=a.output/version/f'{name}-{ir}-{orr}';folder.mkdir(parents=True,exist_ok=True);command=[str(getattr(a,version)),name,str(ir),str(orr),str(folder)]
   t=time.perf_counter();r=subprocess.run(command,capture_output=True,text=True,errors='replace');path=folder/'row.json'
   try:row=json.loads(path.read_text())
   except Exception as e:row={'preset':name,'input_rate':ir,'output_rate':orr,'status':'TOOL_ERROR','parse_error':str(e),'partial_json':path.read_text() if path.exists() else ''}
   row.update(version=version,exit_code=r.returncode,stderr=r.stderr,wall_seconds=time.perf_counter()-t,binary_sha256=identity[version]['binary_sha256'],folder=str(folder))
   row['quality_status']='POINTS_ONLY_NOT_CONTINUOUS_PASS' if 'points' in row else 'RESOURCE_REJECTED_NO_RESPONSE'
   row['resource_scope']='process peak includes preparation, coefficient export, finite-point measurement and impulse; not isolated playback working set'
   for s in row.get('stages',[]):
    s['sha256']=sha(folder/s['file']);s['coefficient_bytes']=s['count']*8
    s['alpha_note']='declared_gaussian_alpha is top-level parameter; phase factors split attenuation; Kaiser has no Gaussian alpha'
    s['phase_factor_gaussian_alphas']=[]
    if s['window']=='gauss':
     n,A=s['count'],s['attenuation'];count=1 if s['phase']=='lp' or n<5 or (n<2048 and A<250) else 2;As=[A/count]*count
     if count==2 and s['phase']=='ip':
      order=2*((n-1)//2);lp=2*int(.5*order*.5+.5);lp=max(2,min(lp,order-2));As=[A*lp/order,A-A*lp/order]
     s['phase_factor_gaussian_alphas']=[3+.011*max(0,v-60) for v in As]
   path.write_text(json.dumps(row,indent=2));rows.append(row)
   result={'status':'COMPLETE_POINT_DATASET_NOT_CONTINUOUS' if len(rows)==90 else 'IN_PROGRESS','expected_rows':90,'completed_rows':len(rows),'elapsed_seconds':time.perf_counter()-start,'identity_file':'identity.json','rows':rows}
   (a.output/'gaussian90.json').write_text(json.dumps(result,indent=2));print(f"{len(rows)}/90 {version} {name} {ir}->{orr} {row['status']} {row['wall_seconds']:.2f}s peak={row.get('peak_working_set_bytes')}",flush=True)
print(json.dumps({'completed':len(rows),'status_counts':{s:sum(x['status']==s for x in rows) for s in sorted({x['status'] for x in rows})}}))
