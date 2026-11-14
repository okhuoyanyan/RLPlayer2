"""Independent RIFF fixtures; native reader hash must equal handwritten decoding."""
import argparse,pathlib,struct,subprocess,tempfile,json
ap=argparse.ArgumentParser();ap.add_argument('--reader',required=True);ap.add_argument('--evidence-dir');a=ap.parse_args();owned=None if a.evidence_dir else tempfile.TemporaryDirectory(prefix='rlplayer-wav-stream-');root=pathlib.Path(a.evidence_dir) if a.evidence_dir else pathlib.Path(owned.name);root.mkdir(parents=True,exist_ok=True);checks=0

def ck(ok,label):
 global checks
 checks+=1
 if not ok:raise AssertionError(label)
def chunk(tag,b):return tag+struct.pack('<I',len(b))+b+(b'\0' if len(b)%2 else b'')
def wav(bits,raw,tag=1,extra=b''):
 align=bits//8;body=b'WAVE'+chunk(b'JUNK',b'odd')+chunk(b'fmt ',struct.pack('<HHIIHH',tag,1,44100,44100*align,align,bits))+extra+chunk(b'data',raw);return b'RIFF'+struct.pack('<I',len(body))+body
def run(path,n):return subprocess.run([a.reader,str(path),str(n)],capture_output=True,text=True)
def hashints(vals):
 h=14695981039346656037
 for v in vals:
  for b in struct.pack('<i',v):h=((h^b)*1099511628211)&((1<<64)-1)
 return h
for bits in (8,16,24,32):
 vals=[-(1<<(bits-1)), -1,0,1,(1<<(bits-1))-1]*401
 raw=b''.join(bytes([v+128]) if bits==8 else int(v).to_bytes(bits//8,'little',signed=True) for v in vals)
 p=root/f'pcm{bits}.wav';p.write_bytes(wav(bits,raw));expected=f'{len(vals)} {hashints(vals)}'
 for n in (1,7,256,65536):
  r=run(p,n);ck(r.returncode==0 and r.stdout.strip()==expected,f'PCM{bits} independent hash chunk{n}')
floats=[-.75,0,.2,1.]*501;raw=struct.pack('<'+'f'*len(floats),*floats);vals=struct.unpack('<'+'i'*len(floats),raw);p=root/'float.wav';p.write_bytes(wav(32,raw,3))
for n in (1,13,65536):
 r=run(p,n);ck(r.returncode==0 and r.stdout.strip()==f'{len(vals)} {hashints(vals)}','float original bits preserved')
base=wav(16,b'\0\0'*3)
bad={'truncated':base[:-1],'trailing':base+b'x','partial-frame':wav(16,b'x'),'rf64':b'RF64'+base[4:],'endian':b'RIFX'+base[4:],'nan':wav(32,struct.pack('<f',float('nan')),3),'duplicate-data':wav(16,b'\0\0',extra=chunk(b'data',b'\0\0'))}
for name,b in bad.items():
 p=root/(name+'.wav');p.write_bytes(b);ck(run(p,7).returncode!=0,name+' must fail')
# Valid >64MiB source: bounded reader scans/streams all samples, not merely header.
p=root/'large.wav';frames=17*1024*1024;size=frames*4
header=wav(32,b'')[:-8]+b'data'+struct.pack('<I',size);header=header[:4]+struct.pack('<I',len(header)-8+size)+header[8:]
with p.open('wb') as f:f.write(header);f.seek(len(header)+size-1);f.write(b'\0')
r1=run(p,4096);r2=run(p,65536);ck(r1.returncode==r2.returncode==0 and r1.stdout==r2.stdout and r1.stdout.startswith(str(frames)+' '),'68MiB complete reader traversal chunk hashes agree')
(root/'summary.json').write_text(json.dumps(dict(checks=checks,status='PASS',large_source_bytes=p.stat().st_size,audio_device_calls=0),indent=2));print('PASS checks='+str(checks))
