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
 r=pathlib.Path(tmp);s=r/'source';kit=r/'cuda'
 for d in ['src/gpu','src/dsp/detail','src/net','tools']:(s/d).mkdir(parents=True)
 for d in ['bin','nvvm','include','lib/x64']:(kit/d).mkdir(parents=True)
 # Keep the source graph realistic: the build names both CUDA translation units,
 # which reach a shared .cuh, a src-relative header and its relative child header.
 units=['src/gpu/gpu_conv_cuda.cu','src/gpu/gpu_resample_cuda.cu']
 (s/'tools/build_gpu_cuda.ps1').write_text('$src = @(\n'+',\n'.join(" (Join-Path $root '"+name.replace('/',chr(92))+"')" for name in units)+'\n)\n')
 cu_text='#include "gpu_common.cuh"\nint fixture = 1;\n'
 for name in units:(s/name).write_text(cu_text)
 (s/'src/gpu/gpu_common.cuh').write_text('#include "dsp/fft.h"\n')
 (s/'src/dsp/fft.h').write_text('#include "detail/twiddle.h"\n')
 transitive=s/'src/dsp/detail/twiddle.h';transitive.write_text('#define TWIDDLE_FIXTURE 1\n')
 ui=s/'src/net/ui_script.h';ui.write_text('unrelated UI version 1\n')
 cu=s/units[0];nvcc=kit/'bin/nvcc.exe';nvcc.write_bytes(b'never-run');(kit/'bin/cuobjdump.exe').write_bytes(b'never-run');(kit/'include/cuda.h').write_text('fixture');host=r/'host';host.mkdir();cl=host/'cl.exe';cl.write_bytes(b'never-run');(host/'dumpbin.exe').write_bytes(b'never-run');dll=r/'test.dll';dll.write_bytes(b'original-artifact');snap=r/'snapshot.json';record=r/'record.json'
 def run(mode,expected,label,error_contains=''):
  a=[sys.executable,str(here/'cuda_build_provenance.py'),mode,'--source',str(s),'--compiler',str(nvcc),'--host-compiler',str(cl),'--dll',str(dll),'--snapshot',str(snap),'--record',str(record)]
  result=subprocess.run(a,capture_output=True,text=True)
  ok=(result.returncode==0)==expected and (not error_contains or error_contains in result.stdout+result.stderr)
  if not ok:print(result.stdout,result.stderr)
  ck(ok,label)
 run('verify',False,'missing seal rejected','missing successful CUDA build provenance');run('snapshot',True,'content snapshot')
 inputs=json.loads(snap.read_text())['inputs']
 ck({k for k in inputs if k.startswith('src/')}==set(units+['src/gpu/gpu_common.cuh','src/dsp/fft.h','src/dsp/detail/twiddle.h']),'exact reachable include closure excludes UI')
 run('seal',True,'unchanged build sealed');run('verify',True,'matching source+toolchain+DLL accepted')
 ui.write_text('unrelated UI version 2\n');run('verify',True,'unrelated UI change does not invalidate GPU seal')
 old=cu.stat();cu.write_text(cu_text.replace('fixture = 1','fixture = 2'));os.utime(cu,ns=(old.st_atime_ns,old.st_mtime_ns))
 run('verify',False,'.cu edit with old mtime rejected',"stale CUDA source/toolchain: ['src/gpu/gpu_conv_cuda.cu']")
 run('seal',False,'source changed during build rejected',"stale CUDA source/toolchain: ['src/gpu/gpu_conv_cuda.cu']")
 cu.write_text(cu_text);run('verify',True,'restored source accepted')
 transitive.write_text('#define TWIDDLE_FIXTURE 2\n');run('verify',False,'transitive include edit rejected',"stale CUDA source/toolchain: ['src/dsp/detail/twiddle.h']")
 transitive.write_text('#define TWIDDLE_FIXTURE 1\n')
 unit=s/units[1];unit.unlink();run('verify',False,'missing listed translation unit rejected',"CUDA translation unit missing: ['src/gpu/gpu_resample_cuda.cu']")
 unit.write_text(cu_text);run('verify',True,'restored include closure accepted')
 dll.write_bytes(b'replaced DLL');run('verify',False,'replaced DLL rejected','CUDA DLL content differs')
 dll.write_bytes(b'original-artifact');run('verify',True,'restored DLL accepted')
 nvcc.write_bytes(b'changed-tool');run('verify',False,'changed nvcc rejected',"stale CUDA source/toolchain: ['cuda:bin/nvcc.exe']")
 nvcc.write_bytes(b'never-run');(kit/'include/cuda.h').write_text('changed-header');run('verify',False,'changed toolkit header rejected',"stale CUDA source/toolchain: ['cuda:include/cuda.h']")
print('PASS CUDA release controls='+str(checks)+' no GPU/no compilation')
