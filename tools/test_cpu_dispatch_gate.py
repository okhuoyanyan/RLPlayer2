"""Reject application EVEX and damaged CRT guards using private copies only."""
import argparse,json,struct,subprocess,sys,tempfile
from pathlib import Path
sys.stdout.reconfigure(encoding="utf-8",errors="replace")
p=argparse.ArgumentParser();p.add_argument('--valid',required=True,type=Path);p.add_argument('--data-valid',type=Path);p.add_argument('--evidence-dir',type=Path);a=p.parse_args()
owned=None
if a.evidence_dir:root=a.evidence_dir;root.mkdir(parents=True,exist_ok=False)
else:owned=tempfile.TemporaryDirectory(prefix='rlplayer-crt-controls-');root=Path(owned.name)
checker=Path(__file__).with_name('check_cpu_dispatch.py');results=[]
def run(name,path,expected):
 report=root/(name+'.json');r=subprocess.run([sys.executable,str(checker),str(path),'--report',str(report)],capture_output=True,text=True,encoding='utf-8',errors='replace')
 ok=(r.returncode==0)==expected;results.append(dict(case=name,exit=r.returncode,correct=ok));print('PASS' if ok else 'FAIL',name,'exit='+str(r.returncode));return json.loads(report.read_text(encoding='utf-8'))
base=a.valid.read_bytes()
def raw_offset(data,rva):
 pe=struct.unpack_from('<L',data,0x3c)[0];n=struct.unpack_from('<H',data,pe+6)[0];opt=struct.unpack_from('<H',data,pe+20)[0]
 for i in range(n):
  off=pe+24+opt+40*i;_,start,size,pointer=struct.unpack_from('<LLLL',data,off+8)
  if start<=rva<start+size:return pointer+rva-start
 raise ValueError('RVA not in raw section')
valid=run('valid-exact-crt',a.valid,True)
if not valid.get('accepted') or not valid.get('initializer_matches'):raise RuntimeError('Positive control must contain actual matched CRT EVEX')
first=valid['accepted'][0];data=bytearray(base);at=raw_offset(data,int(first['start_rva'],16)+first['guards'][0]['compare']+6)
if data[at]!=6:raise RuntimeError('Expected actual >=6 feature guard immediate')
data[at]=0;path=root/'guard-damaged.exe';path.write_bytes(data);run('damaged-feature-guard',path,False)
data=bytearray(base);init=valid['initializer_matches'][0];start=raw_offset(data,int(init['start_rva'],16));at=data.find(b'\x0f\x01\xd0',start,start+init['size'])
if at<0:raise RuntimeError('No actual xgetbv in matched initializer')
data[at:at+3]=b'\x90'*3;path=root/'initializer-damaged.exe';path.write_bytes(data);run('damaged-cpu-initializer',path,False)
data=bytearray(base);pe=struct.unpack_from('<L',data,0x3c)[0];entry=struct.unpack_from('<L',data,pe+24+16)[0];at=raw_offset(data,entry)
# An application/startup function gets a real EVEX instruction. Keep the known
# CRT functions intact: their presence may not exempt this additional code.
data[at:at+6]=bytes.fromhex('62 f2 f5 28 40 d0');path=root/'unguarded-application.exe';path.write_bytes(data);bad=run('unguarded-app-with-valid-crt',path,False)
if not bad.get('unmatched'):raise RuntimeError('Application EVEX was not identified independently')
if a.data_valid:
 original=a.data_valid.read_bytes()
 proof=run('valid-exact-sdk-jump-data',a.data_valid,True)
 if not proof.get('proven_data'):raise RuntimeError('Expected proven SDK table data')
 owner=proof['proven_data'][0];start=int(owner['start_rva'],16);table=int(owner['table_start_rva'],16)
 def mutate(name,rva,code):
  blob=bytearray(original);at=raw_offset(blob,rva);blob[at:at+len(code)]=code
  path=root/(name+'.dll');path.write_bytes(blob);report=run(name,path,False)
  if not report.get('unmatched'):raise RuntimeError('Mutated table/code must retain unaccepted EVEX evidence')
 peoff=struct.unpack_from('<L',original,0x3c)[0];entry=struct.unpack_from('<L',original,peoff+24+16)[0]
 mutate('sdk-table-plus-real-application-evex',entry,bytes.fromhex('62 f2 f5 28 40 d0'))
 mutate('sdk-table-noncase-target',table,struct.pack('<L',start+1))
 mutate('sdk-table-data-target',table,struct.pack('<L',table))
 # Exact dispatcher bound is 14; changing its admissible range cannot inherit the table exception.
 mutate('sdk-dispatch-bound-damaged',start+0x35,b'\x0f')
 mutate('sdk-direct-branch-into-data',entry,b'\xe9'+struct.pack('<i',table-(entry+5)))
 mutate('sdk-dispatch-table-reference-damaged',start+0x3b,struct.pack('<L',table+4))
(root/'regression.json').write_text(json.dumps(results,indent=2),encoding='utf-8');print('CRT dispatch controls',sum(x['correct'] for x in results),'/',len(results),'PE executions=0')
sys.exit(0 if all(x['correct'] for x in results) else 1)
