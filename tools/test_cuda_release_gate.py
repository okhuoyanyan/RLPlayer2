"""Small CUDA release false-green controls, no compiler or device execution."""
import sys,pathlib,tempfile,subprocess,os,importlib.util,json
sys.stdout.reconfigure(encoding='utf-8',errors='replace');here=pathlib.Path(__file__).parent
spec=importlib.util.spec_from_file_location('cudaartifact',here/'check_cuda_artifact.py');g=importlib.util.module_from_spec(spec);spec.loader.exec_module(g)
checks=0

def ck(ok,label):
 global checks
 checks+=1
 if not ok:raise AssertionError(label)
 print('PASS',label)
elf='\n'.join(f'ELF file {i*2+j+1}: rtagpu_cuda.{i*2+j+1}.sm_{sm}.cubin' for i,sm in enumerate(g.ARCHS) for j in range(2));exports='\n'.join(f' {i+1} {i:X} 00001000 {name}' for i,name in enumerate(g.EXPORTS))
ck(len(g.validate(elf,exports,g.ARCHS))==19,'all 19 real ELF cubin entries and four exact exports')
for label,e,x in [('missing-cubin','\n'.join(elf.splitlines()[1:]),exports),('PTX-only','compute_50 sm_50 PTX',exports),('strings-only',elf.replace('ELF file','string'),exports),('missing-export',elf,'\n'.join(exports.splitlines()[1:])),('export-prefix',elf,exports.replace('rta_gpu_convolve','rta_gpu_convolve_fake'))]:
 try:g.validate(e,x,g.ARCHS);ok=False
 except ValueError:ok=True
 ck(ok,label+' rejected')
with tempfile.TemporaryDirectory(prefix='rlplayer-cuda-seal-') as tmp:
 r=pathlib.Path(tmp);s=r/'source';(s/'src/gpu').mkdir(parents=True);(s/'tools').mkdir();kit=r/'cuda'
 for d in ['bin','nvvm','include','lib/x64']:(kit/d).mkdir(parents=True)
 for n in ['build_gpu_cuda.ps1','cuda_build_provenance.py','check_cuda_artifact.py']:(s/'tools'/n).write_text('fixture')
 cu=s/'src/gpu/a.cu';cu.write_text('original');nvcc=kit/'bin/nvcc.exe';nvcc.write_bytes(b'never-run');(kit/'bin/cuobjdump.exe').write_bytes(b'never-run');(kit/'include/cuda.h').write_text('fixture');host=r/'host';host.mkdir();cl=host/'cl.exe';cl.write_bytes(b'never-run');(host/'dumpbin.exe').write_bytes(b'never-run');dll=r/'test.dll';dll.write_bytes(b'original-artifact');snap=r/'snapshot.json';record=r/'record.json'
 def run(mode,expected,label):
  a=[sys.executable,str(here/'cuda_build_provenance.py'),mode,'--source',str(s),'--compiler',str(nvcc),'--host-compiler',str(cl),'--dll',str(dll),'--snapshot',str(snap),'--record',str(record)]
  result=subprocess.run(a,capture_output=True);ck((result.returncode==0)==expected,label)
 run('verify',False,'missing seal rejected');run('snapshot',True,'content snapshot');run('seal',True,'unchanged build sealed');run('verify',True,'matching source+toolchain+DLL accepted')
 old=cu.stat();cu.write_text('changed');os.utime(cu,ns=(old.st_atime_ns,old.st_mtime_ns));run('verify',False,'.cu edit with old mtime rejected');run('seal',False,'source changed during build rejected');cu.write_text('original');dll.write_bytes(b'replaced DLL');run('verify',False,'replaced DLL rejected');dll.write_bytes(b'original-artifact');nvcc.write_bytes(b'changed-tool');run('verify',False,'changed nvcc rejected');nvcc.write_bytes(b'never-run');(kit/'include/cuda.h').write_text('changed-header');run('verify',False,'changed toolkit header rejected')
print('PASS CUDA release controls='+str(checks)+' no GPU/no compilation')
