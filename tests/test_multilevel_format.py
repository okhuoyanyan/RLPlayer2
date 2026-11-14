"""Golden codewords + corruptions decoded by independent Python implementation."""
import argparse,importlib.util,json,struct,subprocess,sys,tempfile
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--writer',required=True,type=Path);a=p.parse_args()
reader=Path(__file__).resolve().parents[1]/'tools/review5_multilevel_decode.py';spec=importlib.util.spec_from_file_location('independent_multilevel',reader);module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
checks=0
with tempfile.TemporaryDirectory(prefix='rlplayer-multilevel-format-') as tmp:
 root=Path(tmp);result=subprocess.run([str(a.writer),str(root)],capture_output=True,text=True,encoding='utf-8',errors='replace');print(result.stdout,result.stderr)
 if result.returncode:raise RuntimeError('writer controls failed')
 for bits in [6,8]:
  path=root/f'golden-{bits}.rlmultilevel';decoded=module.decode(path);levels=1<<bits
  if decoded['payload']!=bytes(range(levels)):raise RuntimeError('golden codeword bytes changed')
  if decoded['samples'][0]!=-1 or decoded['samples'][-1]!=1 or abs(sum(decoded['samples']))>1e-12:raise RuntimeError('code-index mapping is wrong')
  if decoded['channels']!=2 or decoded['frames']!=levels//2:raise RuntimeError('interleaved golden frame ledger wrong')
  checks+=levels;print('PASS all',levels,'codeword bytes and independent mapping')
 valid=(root/'golden-6.rlmultilevel').read_bytes()
 cases={}
 data=bytearray(valid);struct.pack_into('<H',data,8,2);cases['unsupported-version']=(data,'version')
 data=bytearray(valid);data[12:16]=bytes.fromhex('01 02 03 04');cases['wrong-endian']=(data,'endian')
 cases['truncated-header']=(valid[:100],'truncated')
 cases['truncated-payload']=(valid[:-33],'truncated')
 cases['truncated-footer']=(valid[:-1],'truncated')
 cases['trailing-garbage']=(valid+b'x','trailing')
 data=bytearray(valid);data[-1]^=1;cases['bad-checksum']=(data,'checksum')
 data=bytearray(valid);struct.pack_into('<H',data,38,0);cases['unsealed']=(data,'incomplete')
 data=bytearray(valid);data[88]=1;cases['reserved-field']=(data,'reserved')
 data=bytearray(valid);meta=struct.unpack_from('<I',data,44)[0];data[128+meta]=64
 h=14695981039346656037
 for b in data[:-32]:h=((h^b)*1099511628211)&0xffffffffffffffff
 struct.pack_into('<Q',data,len(data)-8,h);cases['illegal-index-valid-checksum']=(data,'index')
 for name,(data,want) in cases.items():
  path=root/(name+'.rlmultilevel');path.write_bytes(data)
  try:module.decode(path)
  except ValueError as e:
   if want not in str(e):raise RuntimeError(f'{name}: wrong rejection reason: {e}')
   checks+=1;print('PASS reject',name,str(e))
  else:raise RuntimeError('false green accepted '+name)
 print('PASS independent multilevel controls checks='+str(checks)+' audio_device_calls=0')
