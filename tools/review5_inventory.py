"""Serial actual-Pipeline inventory with explicit preallocation resource admission.
No timeout/kill is used. Every preset gets a row, including errors/resource skips.
"""
import argparse,hashlib,json,subprocess,time
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--exe',required=True,type=Path);p.add_argument('--output',required=True,type=Path);p.add_argument('--max-prepare-bytes',type=int,default=768*1024**2);a=p.parse_args();a.output.mkdir(parents=True,exist_ok=True)
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest();catalog_path=a.output/'catalog.json'
subprocess.run([str(a.exe),'--catalog',str(catalog_path)],check=True);catalog=json.loads(catalog_path.read_text());rows=[];start=time.perf_counter()
for i,definition in enumerate(catalog):
 folder=a.output/f'preset-{i:03d}';folder.mkdir(exist_ok=True);command=[str(a.exe),'--one',definition['preset'],str(folder),str(a.max_prepare_bytes)]
 r=subprocess.run(command,capture_output=True,text=True,encoding='utf-8',errors='replace')
 path=folder/'inventory.json';row=json.loads(path.read_text()) if path.exists() else dict(definition,status='ERROR',actual_pipeline_prepared=False)
 row.update(catalog_index=i,command=command,exit_code=r.returncode,stdout=r.stdout,stderr=r.stderr)
 for stage in row.get('stages',[]):stage['sha256']=sha(folder/stage['file'])
 path.write_text(json.dumps(row,indent=2));rows.append(row)
 summary={'binary_sha256':sha(a.exe),'catalog_count':len(catalog),'completed_rows':len(rows),'actual_pipeline_exports':sum(x['status']=='ACTUAL_PIPELINE_EXPORTED' for x in rows),'resource_rejections':sum(x['status']=='RESOURCE_ADMISSION_REJECTED_BEFORE_DESIGN' for x in rows),'errors':sum(x['status']=='ERROR' for x in rows),'all_apod_contracts_explicit':all('apod_contract_F2_hz' in x and 'mirror_stop_hz' in x for x in rows),'max_prepare_admission_bytes':a.max_prepare_bytes,'elapsed_seconds':time.perf_counter()-start,'rows':rows,'status':'IN_PROGRESS' if len(rows)!=len(catalog) else 'INVENTORIED_NOT_SPECTRALLY_CERTIFIED'}
 (a.output/'inventory.json').write_text(json.dumps(summary,indent=2));print(f"{i+1}/{len(catalog)} {definition['preset']} {row['status']} coeff={row.get('coefficient_count')} elapsed={row.get('elapsed_seconds')} peak={row.get('peak_working_set_bytes')}",flush=True)
print(json.dumps({k:v for k,v in summary.items() if k!='rows'}))
