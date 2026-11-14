"""Serial actual 2ch24 PCM manifest. Finite points are never continuous PASS."""
import argparse,json,hashlib,subprocess,time,math
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--exe',type=Path,required=True);p.add_argument('--output',type=Path,required=True);p.add_argument('--all-presets',action='store_true');p.add_argument('--max-tool-bytes',type=int,default=1536*1024**2);a=p.parse_args();a.output.mkdir(parents=True,exist_ok=True)
root=Path('D:/RLPlayer-Develop-20261103');source=root/'source';matrix=json.loads((root/'evidence/dsp/acceptance_matrix.json').read_text(encoding='utf-8-sig'));pairs=[x for x in matrix['frozen_dimensions_proposed']['pcm_pairs'] if x[0]!=x[1]];assert len(pairs)==17
def sha(p):
 h=hashlib.sha256()
 with p.open('rb') as f:
  while b:=f.read(1048576):h.update(b)
 return h.hexdigest()
catalog=a.output/'catalog.json';subprocess.run([str(a.exe),'--catalog',str(catalog)],check=True);names=json.loads(catalog.read_text());assert len(names)==79
if not a.all_presets:names=['poly-sinc-short-lp','poly-sinc-short-mp','poly-sinc-ext2-hires-ip','poly-sinc-gauss-xl','poly-sinc-long-mp']
identity={'exe_sha256':sha(a.exe),'tool_admission_bytes':a.max_tool_bytes,'concurrency':1,'pairs_literal_hz':pairs,'original_matrix_sha256':sha(root/'evidence/dsp/acceptance_matrix.json'),'source_hashes':{str(f.relative_to(source)):sha(f) for f in [source/'tools/review5_pcm_manifest.cpp',source/'src/engine/pipeline.h',source/'src/dsp/resampler.h',source/'src/dsp/filter_families.h',source/'src/dsp/fixed_conv.h',source/'src/dsp/response_precision.h']}}
(a.output/'identity.json').write_text(json.dumps(identity,indent=2));rows=[];start=time.perf_counter();total=len(names)*len(pairs)
for index,name in enumerate(names):
 for pair_index,(ir,orr) in enumerate(pairs):
  folder=a.output/f'preset-{index:03d}-pair-{pair_index:02d}';folder.mkdir(exist_ok=True);cmd=[str(a.exe),name,str(ir),str(orr),str(folder),str(a.max_tool_bytes)];t=time.perf_counter();r=subprocess.run(cmd,capture_output=True,text=True,errors='replace');file=folder/'row.json'
  try:row=json.loads(file.read_text())
  except Exception as e:row={'preset':name,'input_rate':ir,'output_rate':orr,'status':'TOOL_ERROR','reason':str(e),'partial_json':file.read_text() if file.exists() else None}
  row.update(exit_code=r.returncode,wall_seconds=time.perf_counter()-t,stderr=r.stderr,command=cmd,folder=str(folder),pair_index=pair_index,preset_index=index)
  for s in row.get('stages',[]):
   s['double_sha256']=sha(folder/s['double_file'])
   if s.get('fixed_file'):s['fixed_sha256']=sha(folder/s['fixed_file'])
  if 'points' in row:
   assert all(math.isfinite(x['hz']) and math.isfinite(x['desired_response_db']) for x in row['points'])
   row['pass_point_max_abs_db']=max(abs(x['desired_response_db']) for x in row['points'] if x['kind']=='pass');row['mirror_stop_point_max_db']=max(x['desired_response_db'] for x in row['points'] if x['kind']=='mirror_stop')
   pm=row['pass_point_max_abs_db']-.01;sm=row['mirror_stop_point_max_db']+row['declared_stop_target_db'];row['point_quality_status']='POINT_THRESHOLD_FAILURE' if max(pm,sm)>1e-6 else 'INDETERMINATE_NEAR_POINT_THRESHOLD' if max(pm,sm)>-1e-6 else 'POINTS_NO_FAILURE_FOUND_NOT_CONTINUOUS'
  else:row['point_quality_status']='NOT_MEASURED_NO_NUMERIC_PASS'
  file.write_text(json.dumps(row,indent=2));rows.append(row);summary={'schema':1,'status':'COMPLETE_FINITE_POINT_MANIFEST' if len(rows)==total else 'IN_PROGRESS','expected_rows':total,'completed_rows':len(rows),'presets':names,'pairs_literal_hz':pairs,'elapsed_seconds':time.perf_counter()-start,'scope':'actual production 2ch integer24 CPU Pipeline with automatic taps/cascade and real fixed eligibility; finite desired-factor points and per-stage phase statistics, NOT continuous bands or all-alias proof; tool admission != product unsupported','tool_admission_bytes':a.max_tool_bytes,'identity':'identity.json','rows':rows};(a.output/'manifest.json').write_text(json.dumps(summary,indent=2));print(f"{len(rows)}/{total} {name} {ir}->{orr} {row['status']} {row['point_quality_status']} wall={row['wall_seconds']:.3f}s WS={row.get('peak_working_set_bytes')}",flush=True)
  if row.get('peak_commit_bytes',0)>a.max_tool_bytes:raise RuntimeError('observed tool commit exceeds admission limit; stop further serial cases without killing process')
print(json.dumps({'completed':len(rows),'elapsed_seconds':summary['elapsed_seconds'],'native_statuses':{s:sum(x['status']==s for x in rows) for s in sorted({x['status'] for x in rows})},'point_statuses':{s:sum(x['point_quality_status']==s for x in rows) for s in sorted({x['point_quality_status'] for x in rows})}}))
