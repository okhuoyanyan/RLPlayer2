"""Independent, finite-point numerical audit; no audio device, no installs.
Run exporter first, then python tools/run_filter_numerical_audit.py EVIDENCE_DIR.
The 280 dB target describes coefficient rejection, NOT process error/noise.
"""
import argparse,csv,json,math,struct,time,hashlib
from pathlib import Path
import mpmath as mp

def read(p):
    raw = p.read_bytes()
    if len(raw) % 8:
        raise ValueError(f"{p.name}: byte length is not divisible by eight")
    values = list(struct.unpack('<' + 'd' * (len(raw) // 8), raw))
    if not all(math.isfinite(value) for value in values):
        raise ValueError(f"{p.name}: non-finite binary64 value")
    return values


def validate_export(directory, cases, frequencies, execution, baseline_path=None):
    """Cheap structural checks must complete before any high-precision calculation."""
    marker = json.loads((directory / 'export-complete.json').read_text(encoding='utf8'))
    if marker.get('complete') is not True or marker.get('case_count') != len(cases):
        raise ValueError('missing or inconsistent successful-export completion marker')
    if not cases:
        raise ValueError('empty case manifest')
    by_id = {}
    expected_counts = {}
    for case in cases:
        identity = case['id']
        if not identity.isascii() or not identity.isdecimal() or identity in by_id:
            raise ValueError(f'duplicate or invalid case id: {identity!r}')
        by_id[identity] = case
        L, M, T = (int(case[field]) for field in ('L', 'M', 'T'))
        in_rate, out_rate = int(case['in']), int(case['out'])
        if min(L, M, T, in_rate, out_rate) <= 0 or math.gcd(L, M) != 1:
            raise ValueError(f'case {identity}: illegal L/M/T or rate')
        if out_rate * M != in_rate * L:
            raise ValueError(f'case {identity}: L/M does not match sample rates')
        h = read(directory / f'{identity}-h.bin')
        x = read(directory / f'{identity}-x.bin')
        if len(h) != T * L:
            raise ValueError(f'case {identity}: coefficient length must equal T*L')
        if not x or len(x) != int(case['input_count']):
            raise ValueError(f'case {identity}: input length does not match input_count')
        if any(abs(value) > 1 or not (value * 8388608).is_integer() for value in x):
            raise ValueError(f'case {identity}: input is not normalized exact 24-bit PCM')
        expected = (len(x) * L + M - 1) // M
        if int(case['output_count']) != expected:
            raise ValueError(f'case {identity}: output_count must equal ceil(input_count*L/M)')
        expected_counts[identity] = expected
        for mode in (0, 1):
            for amplitude in (0, 1, 2):
                y = read(directory / f'{identity}-m{mode}-y{amplitude}.bin')
                if len(y) != expected:
                    raise ValueError(f'case {identity} mode {mode} amplitude {amplitude}: output length mismatch')
    frequency_counts = {identity: 0 for identity in by_id}
    for row in frequencies:
        identity = row['id']
        if identity not in by_id:
            raise ValueError(f'frequency references unknown case {identity}')
        case = by_id[identity]
        values = [float(row[field]) for field in ('hz', 'double_db', 'dd_db')]
        if not all(math.isfinite(value) for value in values):
            raise ValueError(f'case {identity}: non-finite frequency metadata')
        if not 0 <= values[0] <= int(case['in']) * int(case['L']) / 2:
            raise ValueError(f'case {identity}: frequency outside prototype Nyquist')
        frequency_counts[identity] += 1
    if any(count != 6 for count in frequency_counts.values()):
        raise ValueError('each exported case must have all six frequency calibration rows')
    execution_keys = set()
    for row in execution:
        key = (row['id'], row['kernel'], int(row['amplitude_index']))
        if key in execution_keys or key[0] not in by_id or key[1] not in ('auto', 'scalar') or key[2] not in (0, 1, 2):
            raise ValueError(f'invalid or duplicate execution metadata: {key}')
        execution_keys.add(key)
        block_frames = int(row['block_frames'])
        simd_outputs, threads = int(row['simd_outputs']), int(row['max_threads'])
        if not 1 <= block_frames <= 1048576 or block_frames != marker.get('block_frames'):
            raise ValueError('invalid or inconsistent block_frames metadata')
        if not 0 <= simd_outputs <= expected_counts[key[0]] or not 0 <= threads <= 8:
            raise ValueError('invalid CPU execution counters')
        if key[1] == 'scalar' and simd_outputs:
            raise ValueError('scalar execution cannot report SIMD outputs')
    expected_keys = {(identity, kernel, amplitude) for identity in by_id
                     for kernel in ('auto', 'scalar') for amplitude in (0, 1, 2)}
    if execution_keys != expected_keys:
        raise ValueError('execution metadata does not cover every case/mode/amplitude')
    if baseline_path:
        baseline = json.loads(baseline_path.read_text(encoding='utf8'))
        previous = {}
        for entry in baseline['cases']:
            case = entry['configuration']
            identity = case['id']
            if identity in previous:
                raise ValueError(f'duplicate baseline case id: {identity}')
            previous[identity] = case
        if set(previous) != set(by_id):
            raise ValueError('baseline case id set does not match current manifest')
        for identity, case in by_id.items():
            if case != previous[identity]:
                raise ValueError(f'baseline configuration mismatch for case {identity}')
    return by_id

def db(v):return float(20*mp.log10(v)) if v else None
def stats(errors,refs):
 e2=mp.fsum(e*e for e in errors);r2=mp.fsum(r*r for r in refs)
 return dict(max_abs=float(max(map(abs,errors))),rms_abs=float(mp.sqrt(e2/len(errors))),relative_rms_db=db(mp.sqrt(e2/r2)),samples=len(errors))
def main():
 a=argparse.ArgumentParser();a.add_argument('directory',type=Path);a.add_argument('--baseline',type=Path);a.add_argument('--inject',choices=['coefficient','arithmetic']);a.add_argument('--fft-only',action='store_true');a.add_argument('--case',choices=['0','1','2']);a.add_argument('--validate-only',action='store_true');args=a.parse_args();d=args.directory;t=time.time()
 if args.fft_only:return fft_check(d)
 cases=list(csv.DictReader((d/'cases.tsv').open(),delimiter='\t'));freqs=list(csv.DictReader((d/'frequencies.tsv').open(),delimiter='\t'))
 execution=list(csv.DictReader((d/'execution.tsv').open(),delimiter='\t'))
 validated=validate_export(d,cases,freqs,execution,args.baseline)
 if args.case is not None and args.case not in validated:raise ValueError('selected case is absent')
 if args.validate_only:
  print(f'Validated {len(validated)} cases before high-precision calculation',flush=True)
  return 0
 result={'schema':1,'scope':'finite selected points; no continuous-band guarantee; CPU single-stage; process compared to exact exported binary64 input and coefficients','seed':2802026,'input':'24-bit signed uniform PCM / 2^23, exactly binary64; trailing zeros; block size and actual CPU SIMD counters in execution metadata','execution':execution,'reference_dps':[60,90],'cases':[],'gate_pass':True}
 for c in cases:
  if args.case is not None and c['id']!=args.case:continue
  id=c['id'];L=int(c['L']);M=int(c['M']);h=read(d/f'{id}-h.bin');x=read(d/f'{id}-x.bin');T=int(c['T']);entry={'configuration':c,'coefficient_sha256':hashlib.sha256((d/f'{id}-h.bin').read_bytes()).hexdigest(),'frequency':[],'process':[]}
  if args.inject=='coefficient':h[len(h)//2]+=1e-8
  # Exact integer accumulation over ALL outputs. Binary64 coefficients and
  # 24-bit inputs are dyadic rationals; common power-of-two denominator makes
  # every product/sum exact, independent of mpmath and production accumulation.
  ratios=[v.as_integer_ratio() for v in h];q=max(b.bit_length()-1 for _,b in ratios)
  hi=[a << (q-(b.bit_length()-1)) for a,b in ratios];xi=[int(v*8388608) for v in x]
  exact=[]
  for n in range(int(c['output_count'])):
   base,phase=divmod(n*M,L)
   exact.append(sum(hi[phase+u*L]*xi[base-u] for u in range(min(T,base+1)) if base-u<len(x)))
  # Match exported evaluator's normalized binary64 coefficients, not ideal design.
  hn=[v/L for v in h]
  for row in [r for r in freqs if r['id']==id]:
   vals=[]
   for prec in (60,90):
    mp.mp.dps=prec;w=mp.exp(-2j*mp.pi*mp.mpf(float(row['hz']))/(int(c['in'])*L));z=mp.mpc(1);s=mp.mpc(0)
    for v in hn:s+=mp.mpf(v)*z;z*=w
    vals.append(abs(s))
   delta=abs(vals[0]-vals[1]);refdb=db(vals[1]);dd_delta=abs(float(row['dd_db'])-refdb) if refdb is not None else 0
   # Deliberate center-coefficient perturbation, independent of evaluator.
   bad=s+mp.mpf('1e-8')*w**(len(h)//2);detected=abs(bad-s)>mp.mpf('1e-12')
   entry['frequency'].append(dict(hz=float(row['hz']),double_db=float(row['double_db']),dd_db=float(row['dd_db']),mp90_db=refdb,reference_60_90_abs_delta=float(delta),dd_db_error=dd_delta,coefficient_negative_control_detected=bool(detected)))
   result['gate_pass'] &= delta<mp.mpf('1e-45') and dd_delta<1e-5 and detected
  for mode,amp in ((m,a) for m in (0,1) for a in (0,1,2)):
   y=read(d/f'{id}-m{mode}-y{amp}.bin');factor=mp.mpf(2)**((0,-4,-20)[amp])
   if args.inject=='arithmetic':y[len(y)//2]+=1e-8*float(factor)
   # Explicit boundaries, full steady region and tail, plus fixed random indices.
   import random
   rng=random.Random(2802026);idx=set([0,1,L-1,L, len(y)-2,len(y)-1]);idx.update(rng.sample(range(len(y)),min(96,len(y))))
   errors=[];refs=[];refs60=[]
   for prec in (60,90):
    mp.mp.dps=prec;ref=[]
    for n in sorted(idx):
     base,phase=divmod(n*M,L);v=mp.fsum(mp.mpf(h[phase+u*L])*mp.mpf(x[base-u])*factor for u in range(min(T,base+1)) if base-u<len(x));ref.append(v)
    if prec==60:refs60=ref
    else:refs=ref
   errors=[mp.mpf(y[n])-v for n,v in zip(sorted(idx),refs)];st=stats(errors,refs)
   shift=(0,4,20)[amp];den=q+23+shift;all_errors=[];all_refs=[]
   for yy,rr in zip(y,exact):
    ya,yb=yy.as_integer_ratio();yd=yb.bit_length()-1;common=max(den,yd)
    diff=(ya << (common-yd))-(rr << (common-den))
    all_errors.append(math.ldexp(float(diff),-common));all_refs.append(math.ldexp(float(rr),-den))
   st['full_output_exact_dyadic']={'samples':len(y),'max_abs':max(map(abs,all_errors)),'rms_abs':math.sqrt(math.fsum(e*e for e in all_errors)/len(y)),'relative_rms_db':10*math.log10(math.fsum(e*e for e in all_errors)/math.fsum(v*v for v in all_refs))}
   st['exact_vs_mp90_max_delta']=float(max(abs(mp.mpf(exact[n])*mp.power(2,-den)-v) for n,v in zip(sorted(idx),refs)))
   st['amplitude']=float(factor);st['kernel']='scalar' if mode else 'auto';st['execution']=next((r for r in execution if r['id']==id and r['kernel']==st['kernel'] and int(r['amplitude_index'])==amp),None);st['indices']=sorted(idx);st['reference_60_90_max_delta']=float(max(abs(v-w) for v,w in zip(refs,refs60)))
   limit=2e-14*float(factor);st['max_abs_gate']=limit;st['arithmetic_negative_control_detected']=float(max(abs(e+mp.mpf('1e-8')*factor) for e in errors))>limit
   st['pass']=st['full_output_exact_dyadic']['max_abs']<limit and st['arithmetic_negative_control_detected'] and st['reference_60_90_max_delta']<1e-45 and st['exact_vs_mp90_max_delta']<1e-75
   result['gate_pass'] &= st['pass'];entry['process'].append(st)
  result['cases'].append(entry);print(id,c['preset'],'T',T,'done',round(time.time()-t,1),flush=True)
 if args.baseline:
  baseline=json.loads(args.baseline.read_text(encoding='utf8'));checks=[]
  for current in result['cases']:
   old=next((c for c in baseline['cases'] if c['configuration']['id']==current['configuration']['id']),None)
   same=old is not None and current['configuration']==old['configuration'] and current['coefficient_sha256']==old['coefficient_sha256']
   checks.append({'id':current['configuration']['id'],'coefficient_bits_unchanged':same})
   result['gate_pass'] &= same
  result['baseline_checks']=checks
 result['elapsed_seconds']=time.time()-t
 result['limits']='2e-14*input amplitude is a regression ceiling, not 280dB claim; full-output max/RMS exact dyadic reference, mpmath convergence sampled; no DAC/analogue audibility claim. long double digits metadata shows whether native wider precision exists.'
 (d/('numerical-audit-'+args.inject+'.json' if args.inject else 'numerical-audit.json')).write_text(json.dumps(result,indent=2),encoding='utf8');print('gate_pass',result['gate_pass']);return 0 if result['gate_pass'] else 1
def fft_check(d):
 results=[];passed=True
 for n in (1024,4096,16384,65536):
  x=read(d/f'fft-{n}-x.bin');old=read(d/f'fft-{n}-old.bin');new=read(d/f'fft-{n}-new.bin')
  for k in (0,1,n//7,n//2,n-1):
   vals=[]
   for prec in (60,90):
    mp.mp.dps=prec;w=mp.exp(-2j*mp.pi*k/n);z=mp.mpc(1);v=mp.mpc(0)
    for i in range(n):v+=mp.mpc(x[2*i],x[2*i+1])*z;z*=w
    vals.append(v)
   eo=abs(mp.mpc(old[2*k],old[2*k+1])-v);en=abs(mp.mpc(new[2*k],new[2*k+1])-v)
   ok=en<mp.mpf('2e-11') and abs(vals[0]-vals[1])<mp.mpf('1e-45');passed &= ok
   results.append(dict(n=n,bin=k,old_abs_error=float(eo),new_abs_error=float(en),reference_60_90_abs_delta=float(abs(vals[0]-vals[1])),passed=bool(ok)))
  print('FFT',n,'done',flush=True)
 (d/'fft-numerical-audit.json').write_text(json.dumps({'pass':bool(passed),'finite_selected_bins':True,'results':results},indent=2),encoding='utf8')
 return 0 if passed else 1
if __name__=='__main__':
 try:
  raise SystemExit(main())
 except (OSError, ValueError, KeyError, TypeError, OverflowError) as error:
  print(f'Audit validation/error: {error}', file=__import__('sys').stderr)
  raise SystemExit(2)
