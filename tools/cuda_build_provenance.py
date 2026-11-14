"""CUDA build content freshness; independent artifact and real hardware gates remain required."""
import argparse,hashlib,json,sys
from pathlib import Path
def digest(p):
 with p.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()
def inputs(source,compiler,host):
 paths=sorted(p for p in (source/'src').rglob('*') if p.is_file() and p.suffix.lower() in {'.cpp','.h','.hpp','.cu','.cuh','.inl','.c','.cc'})
 paths += [source/'tools'/n for n in ['build_gpu_cuda.ps1','cuda_build_provenance.py','check_cuda_artifact.py']]
 result={str(p.relative_to(source)).replace('\\','/'):digest(p) for p in paths}
 kit=compiler.parent.parent
 # Hash all CUDA driver executables/DLLs, compiler NVVM/codegen, headers and
 # static runtime inputs: nvcc alone is only a frontend orchestrator.
 for base in [kit/'bin',kit/'nvvm',kit/'include',kit/'lib/x64']:
  if not base.is_dir():raise ValueError('CUDA toolchain directory missing: '+str(base))
  for p in sorted(base.rglob('*')):
   if p.is_file():result['cuda:'+p.relative_to(kit).as_posix()]=digest(p)
 for name in ['nvcc.exe','cuobjdump.exe']:
  if not (compiler.parent/name).is_file():raise ValueError('required CUDA tool missing: '+name)
 if not host.is_file():raise ValueError('host compiler missing')
 for p in sorted(host.parent.iterdir()):
  if p.is_file() and p.suffix.lower() in {'.exe','.dll'}:result['host:'+p.name]=digest(p)
 if not (host.parent/'dumpbin.exe').is_file():raise ValueError('host dumpbin missing')
 return result
def main():
 p=argparse.ArgumentParser();p.add_argument('mode',choices=['snapshot','seal','verify']);p.add_argument('--source',type=Path,required=True);p.add_argument('--compiler',type=Path,required=True);p.add_argument('--host-compiler',type=Path,required=True);p.add_argument('--snapshot',type=Path);p.add_argument('--record',type=Path);p.add_argument('--dll',type=Path);a=p.parse_args();current=inputs(a.source.resolve(),a.compiler,a.host_compiler)
 if a.mode=='snapshot':
  if not a.snapshot:raise ValueError('snapshot required')
  a.snapshot.write_text(json.dumps(dict(inputs=current),indent=2),encoding='utf-8')
 else:
  prior=a.snapshot if a.mode=='seal' else a.record
  if not prior or not prior.is_file():raise ValueError('missing successful CUDA build provenance; rebuild')
  old=json.loads(prior.read_text(encoding='utf-8'))
  if old.get('inputs')!=current:raise ValueError('stale CUDA source/toolchain: '+str([k for k in set(old.get('inputs',{}))|set(current) if old.get('inputs',{}).get(k)!=current.get(k)][:12]))
  if not a.dll or not a.record:raise ValueError('DLL and record required')
  value=digest(a.dll)
  if a.mode=='verify' and old.get('dll_sha256')!=value:raise ValueError('CUDA DLL content differs from sealed build')
  if a.mode=='seal':a.record.write_text(json.dumps(dict(schema=1,backend='cuda',inputs=current,dll_sha256=value,scope='content freshness only; cubin/exports and real NVIDIA hardware execution separate'),indent=2),encoding='utf-8')
 print('CUDA provenance PASS',a.mode,'inputs='+str(len(current)));return 0
if __name__=='__main__':
 try:sys.exit(main())
 except Exception as e:print('CUDA provenance FAIL:',e,file=sys.stderr);sys.exit(1)
