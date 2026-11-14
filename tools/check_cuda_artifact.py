"""Offline CUDA12 FULL artifact gate. No NVIDIA device opened."""
import argparse,json,re,subprocess,sys
from pathlib import Path
ARCHS=[50,52,53,60,61,62,70,72,75,80,86,87,89,90,100,101,103,120,121]
EXPORTS=['rta_gpu_available','rta_gpu_convolve','rta_gpu_resample','rta_gpu_resample_invalidate']
def validate(elf,exports,archs):
 # Only actual numbered cuobjdump ELF entries count, not arbitrary strings/PTX.
 rows=re.findall(r'^\s*ELF file\s+\d+\s*:\s*(.+)$',elf,re.M)
 counts={sm:sum(bool(re.search(r'\.sm_'+str(sm)+r'\.cubin(?:\s|$)',x)) for x in rows) for sm in archs}
 missing=[sm for sm,n in counts.items() if n<2]
 if missing:raise ValueError('missing per-translation-unit cubins (need conv+resample): '+str(missing))
 missing=[name for name in EXPORTS if not re.search(r'^\s*\d+\s+[0-9A-Fa-f]+\s+[0-9A-Fa-f]+\s+'+re.escape(name)+r'\s*$',exports,re.M)]
 if missing:raise ValueError('missing exact host exports: '+str(missing))
 return counts
def main():
 p=argparse.ArgumentParser();p.add_argument('dll',type=Path);p.add_argument('--cuobjdump',type=Path,required=True);p.add_argument('--dumpbin',type=Path,required=True);p.add_argument('--archs',default=','.join(map(str,ARCHS)));p.add_argument('--report',type=Path);a=p.parse_args()
 if not a.dll.is_file() or a.dll.stat().st_size<64:raise ValueError('missing/empty CUDA DLL')
 results=[]
 for cmd in [[str(a.cuobjdump),'-lelf',str(a.dll)],[str(a.dumpbin),'/exports',str(a.dll)]]:
  r=subprocess.run(cmd,capture_output=True,text=True,errors='replace')
  if r.returncode:raise ValueError('artifact tool failed exit '+str(r.returncode)+': '+r.stdout+r.stderr)
  results.append(r.stdout)
 counts=validate(*results,[int(x) for x in a.archs.split(',')]);report=dict(status='PASS_OFFLINE_ARTIFACT_ONLY',arch_cubin_counts=counts,exports=EXPORTS,nvidia_hardware='UNVERIFIED_REQUIRES_MATCHING_DEVICE',tool_output=results)
 if a.report:a.report.write_text(json.dumps(report,indent=2),encoding='utf-8')
 print('CUDA OFFLINE ARTIFACT PASS arches='+str(len(counts))+'; real NVIDIA kernel execution remains UNVERIFIED');return 0
if __name__=='__main__':
 try:sys.exit(main())
 except Exception as e:print('CUDA artifact FAIL:',e,file=sys.stderr);sys.exit(1)
