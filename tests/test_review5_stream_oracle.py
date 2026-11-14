"""Independent exact upsample/convolve/decimate oracle for exported real streams.
Does not import the production phase/index/history or numerical helpers.
Every binary64 input/coefficient is an exact Fraction. A standard gamma_(2T)
roundoff bound (separate multiply/add) is propagated through both stages.
"""
import argparse,json,struct,hashlib,time
from pathlib import Path
from fractions import Fraction as F
p=argparse.ArgumentParser();p.add_argument('--evidence',type=Path,required=True);a=p.parse_args();b=a.evidence;rows=json.loads((b/'cases.json').read_text());cache={};checked=0;maxerr=F();maxbound=F();outcount=0;start=time.perf_counter();phase_coverage={};negative=[]
def doubles(file):
 raw=(b/file).read_bytes();return raw,list(struct.unpack('<'+'d'*(len(raw)//8),raw))
def direct(x,e,h,L,M):
 # Literal zero insertion followed by ordinary linear convolution. Select
 # indices 0,M,2M,... only after convolution; no production polyphase indexing.
 up=[F()]*(len(x)*L);ue=[F()]*(len(x)*L)
 for i,v in enumerate(x):up[i*L]=v;ue[i*L]=e[i]
 conv=[F()]*(len(up)+len(h)-1);magnitude=[F()] *len(conv);incoming=[F()]*len(conv)
 nz_h=[(j,v) for j,v in enumerate(h) if v]
 for i,v in enumerate(up):
  if v:
   for j,w in nz_h:conv[i+j]+=v*w;magnitude[i+j]+=abs(v*w)
 for i,v in enumerate(ue):
  if v:
   for j,w in nz_h:incoming[i+j]+=abs(w)*v
 T=len(h)//L;u=F(1,1<<53);gamma=2*T*u/(1-2*T*u);n=(len(x)*L+M-1)//M
 return [conv[k*M] for k in range(n)],[incoming[k*M]+gamma*(magnitude[k*M]+incoming[k*M]) for k in range(n)]
def accepted(actual,ideal,bounds):return len(actual)==len(ideal) and all(abs(F.from_float(v)-q)<=e for v,q,e in zip(actual,ideal,bounds))
for r in rows:
 assert r['max_threads']<=2
 h=[];ids=[]
 for s in r['stages']:
  raw,coeff=doubles(s['file']);assert len(coeff)==s['L']*s['T'];h.append([F.from_float(v) for v in coeff]);ids.append(hashlib.sha256(raw).hexdigest())
 topology=tuple((s['L'],s['M']) for s in r['stages']);coverage=phase_coverage.setdefault(str(topology),{'source_M_residues':set(),'reached_stage_phase_pairs':set(),'chunks':set(),'channels':set()});coverage['source_M_residues'].add(r['source_phase_mod_M']);coverage['chunks'].add(r['chunk']);coverage['channels'].add(r['channels'])
 if len(topology)==2:
  L1,M1=topology[0];L2,M2=topology[1];assert M1==1
  coverage['reached_stage_phase_pairs'].update(((k*M2//L2)%L1,(k*M2)%L2) for k in range(r['output_frames']))
 for channel,(xi,yi) in enumerate(zip(r['input'],r['output'])):
  raw,x=doubles(xi);_,actual=doubles(yi);key=(hashlib.sha256(raw).hexdigest(),tuple(ids),topology)
  if key not in cache:
   ideal=[F.from_float(v) for v in x];bound=[F()]*len(x)
   for s,coeff in zip(r['stages'],h):ideal,bound=direct(ideal,bound,coeff,s['L'],s['M'])
   cache[key]=(ideal,bound)
  ideal,bound=cache[key];assert r['output_frames']==len(ideal) and accepted(actual,ideal,bound),(r['id'],channel)
  for v,q,e in zip(actual,ideal,bound):maxerr=max(maxerr,abs(F.from_float(v)-q));maxbound=max(maxbound,e)
  checked+=1;outcount+=len(actual)
  # One nonzero fixture for each topology: bad result controls are checked by
  # the same exact-oracle predicate, not by an unrelated assertion.
  marker=str(topology)
  if channel==0 and not any(z['topology']==marker for z in negative):
   tests={};tests['dropped_frame_rejected']=not accepted(actual[:-1],ideal,bound)
   tests['all_zero_output_rejected']=not accepted([0.]*len(actual),ideal,bound)
   modified=list(actual);idx=max(range(len(actual)),key=lambda i:abs(actual[i]));modified[idx]+=.125;tests['bad_sample_rejected']=not accepted(modified,ideal,bound)
   # Deliberately interpret raw interpolation gain L as unity: wrong gain must fail.
   tests['wrong_reference_gain_rejected']=not accepted([v/topology[0][0] for v in actual],ideal,bound)
   # Simulate block-local clock/history restart, including its count drift.
   buggy=[]
   for pos in range(0,len(x),7):
    zz=[F.from_float(v) for v in x[pos:pos+7]];ee=[F()]*len(zz)
    for s,coeff in zip(r['stages'],h):zz,ee=direct(zz,ee,coeff,s['L'],s['M'])
    buggy.extend(float(v) for v in zz)
   tests['block_phase_history_restart_rejected']=not accepted(buggy,ideal,bound)
   assert all(tests.values()),(marker,tests);negative.append({'topology':marker,'checks':tests})
for topology,c in phase_coverage.items():
 import ast,math
 ss=ast.literal_eval(topology);M=math.prod(x[1] for x in ss);assert c['source_M_residues']==set(range(M))
 if len(ss)==2:assert len(c['reached_stage_phase_pairs'])==ss[0][0]*ss[1][0]
 for k in c:c[k]=sorted(c[k])
report={'status':'PASS','actual_cases':len(rows),'channel_stream_checks':checked,'compared_output_samples':outcount,'unique_exact_oracle_inputs':len(cache),'max_absolute_error_exact':str(maxerr),'max_absolute_error':float(maxerr),'max_propagated_roundoff_bound':float(maxbound),'negative_controls':negative,'phase_coverage':phase_coverage,'elapsed_seconds':time.perf_counter()-start,'scope':'six small exact ratios, single-stage and actual M1=1 rational two-stage; 1/2/6ch; every source n mod M; reachable stage phase pairs; whole/1/7/13 blocks plus empty; no preset-wide spectral/quality claim','method':'independent literal zero insertion, exact linear convolution, integer decimation; binary64 inputs/coeff exact Fraction; propagated gamma_(2T) with u=2^-53','input_manifest_sha256':hashlib.sha256((b/'cases.json').read_bytes()).hexdigest()}
(b/'oracle.json').write_text(json.dumps(report,indent=2));print(json.dumps(report))
