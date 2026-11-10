"""Independent continuous FIR certificate verifier.

Does NOT import the generator or use its DFT values, moments, reported bounds,
status, or remainder. It constructs exact rational autocorrelation coefficients
and uses Arb polynomial evaluation and a separately evaluated Taylor enclosure.
"""
from __future__ import annotations
import argparse, hashlib, json, math, struct, sys, time
from fractions import Fraction
from pathlib import Path

def verify(coefficients,spec,certificate,*,flint_path=None):
    if flint_path:sys.path.insert(0,str(Path(flint_path).resolve()))
    from flint import arb,acb,acb_poly,fmpq,fmpq_poly,ctx
    if certificate.get('schema')!='rlplayer-continuous-fir-v1':raise ValueError('schema mismatch')
    raw=Path(coefficients).read_bytes()
    if not raw or len(raw)%8 or hashlib.sha256(raw).hexdigest()!=certificate['coefficient_sha256']:raise ValueError('coefficient SHA or length mismatch')
    if hashlib.sha256(json.dumps(spec,sort_keys=True,separators=(',',':')).encode()).hexdigest()!=certificate['spec_sha256']:raise ValueError('trusted external specification SHA mismatch')
    values=list(struct.unpack('<'+'d'*(len(raw)//8),raw))
    if not all(math.isfinite(x) for x in values):raise ValueError('nonfinite coefficients')
    n=len(values)
    if n!=certificate['coefficient_count'] or n>32768:raise ValueError('coefficient count/budget mismatch')
    degree=int(certificate['degree']);precision=int(certificate['precision_bits']);grid=int(certificate['grid'])
    if not 8<=degree<=64 or not 192<=precision<=4096 or grid<n or grid>32768 or grid&(grid-1):raise ValueError('invalid precision/degree/grid or explicit verifier budget')
    ctx.prec=max(precision+64,320);ctx.threads=1
    bands=spec['bands'];leaves=certificate['leaves']
    if not bands:raise ValueError('nonempty trusted frequency bands required')
    if type(spec.get('normalize_dc',False)) is not bool:raise ValueError('normalize_dc must be boolean')
    if len(leaves)>2*grid+len(bands):raise ValueError('too many intervals')
    definitions=[];groups={k:[] for k in range(len(bands))}
    for b in bands:
        lo,hi=Fraction(str(b['lo'])),Fraction(str(b['hi']))
        if not 0<=lo<hi<=Fraction(1,2):raise ValueError('invalid trusted frequency band')
        if b['kind']=='stop':
            db=Fraction(str(b['db']));maximum=arb(10)**(arb(fmpq(db.numerator,db.denominator))/10);minimum=arb(0)
        elif b['kind']=='pass':
            db=Fraction(str(b['ripple_db']));maximum=arb(10)**(arb(fmpq(db.numerator,db.denominator))/10);minimum=1/maximum
        else:raise ValueError('invalid band kind')
        definitions.append((lo,hi,minimum,maximum,b['kind']))
    for item in leaves:
        band=item['band']
        if type(band) is not int or band not in groups:raise ValueError('unknown/missing band identity')
        left,right,center=map(Fraction,(item['lo'],item['hi'],item['center']))
        if not left<right or not 0<=center<=Fraction(1,2) or (center*grid).denominator!=1:raise ValueError('invalid interval or DFT-center identity')
        if left<center-Fraction(1,2*grid) or right>center+Fraction(1,2*grid):raise ValueError('interval not contained in declared center cell')
        groups[band].append((left,right,center,item))
    for band,items in groups.items():
        lo,hi,*_=definitions[band];cursor=lo
        for left,right,center,item in sorted(items,key=lambda x:x[0]):
            if left!=cursor or right>hi:raise ValueError('frequency coverage gap/overlap or changed band edge')
            cursor=right
        if cursor!=hi:raise ValueError('frequency band incomplete')
    started=time.perf_counter()
    exact=[fmpq(*x.as_integer_ratio()) for x in values]
    reference_gain=sum(exact) if spec.get('normalize_dc',False) else fmpq(str(spec.get('reference_gain','1')))
    if not reference_gain:raise ValueError('zero reference gain/DC normalization')
    exact=[x/reference_gain for x in exact]
    # Exact multiplication of rational polynomials gives EVERY autocorrelation
    # lag; this does not share the generator's complex FIR derivative computation.
    correlation=fmpq_poly(exact)*fmpq_poly(exact[::-1])
    corr=[correlation[n-1+i] for i in range(n)]
    full_span=max(n-1,1);l1=sum(abs(x) for x in exact);l1_squared=arb(l1*l1)
    centers=sorted({center for items in groups.values() for _,_,center,_ in items})
    positions={x:i for i,x in enumerate(centers)}
    points=[acb(-2*fmpq(c.numerator,c.denominator)).exp_pi_i() for c in centers]
    derivatives=[]
    for j in range(degree):
        # d^j |H|^2 / d(full_span*w)^j / j!
        weights=[corr[0] if j==0 else fmpq(0)]
        for lag in range(1,n):weights.append(2*corr[lag]*fmpq(lag,full_span)**j/math.factorial(j))
        polynomial=acb_poly(weights)
        values_at_centers=polynomial.evaluate(points,algorithm='iter')
        factor=(-acb(0,1))**j
        derivatives.append([(v*factor).real for v in values_at_centers])
    failed=unresolved=verified=0;max_stop_power=arb(0);max_pass_deviation=arb(0)
    for band,items in groups.items():
        lo,hi,minimum,maximum,kind=definitions[band]
        for left,right,center,item in items:
            position=positions[center];radius=max(abs(left-center),abs(right-center))
            t=arb(full_span)*2*arb.pi()*arb(fmpq(radius.numerator,radius.denominator))
            remainder=(l1_squared*t**degree/math.factorial(degree)).upper()
            variation=remainder
            for j in range(1,degree):variation+=derivatives[j][position].abs_upper()*t**j
            actual_lower=(derivatives[0][position]-variation).lower();actual_upper=(derivatives[0][position]+variation).upper()
            if actual_lower<0:actual_lower=arb(0)
            claimed_lo=arb(fmpq(item['lower_power']));claimed_hi=arb(fmpq(item['upper_power']))
            if not claimed_lo.is_exact() or not claimed_hi.is_exact():raise ValueError('claimed bound must be exact dyadic at verifier precision')
            if claimed_lo>actual_lower or claimed_hi<actual_upper:raise ValueError('forged or insufficient rounding/remainder envelope')
            if claimed_lo<0 or claimed_hi<claimed_lo:raise ValueError('invalid power interval')
            good=claimed_hi<maximum.lower() and (kind!='pass' or claimed_lo>minimum.upper())
            witness_lo=derivatives[0][position].lower();witness_hi=derivatives[0][position].upper()
            bad=witness_lo>maximum.upper() or (kind=='pass' and witness_hi<minimum.lower())
            expected_status='FAIL' if bad else 'PASS' if good else 'INDETERMINATE'
            if item.get('status')!=expected_status:raise ValueError('forged interval status')
            if bad:failed+=1
            elif not good:unresolved+=1
            else:verified+=1
            if kind=='stop' and max_stop_power<claimed_hi:max_stop_power=claimed_hi
            if kind=='pass':
                d=max((claimed_hi-1).abs_upper(),(1-claimed_lo).abs_upper())
                if max_pass_deviation<d:max_pass_deviation=d
    status='FAIL' if failed else 'INDETERMINATE' if unresolved else 'PASS'
    if certificate['status']!=status:raise ValueError('certificate status disagrees with independent verification')
    return {'status':status,'verified_intervals':verified,'violating_point_witnesses':failed,'unresolved_intervals':unresolved,
        'method':'independent exact rational autocorrelation; Arb polynomial evaluation (no DFT); full Taylor cell enclosure',
        'max_stop_db_upper':float(10*max_stop_power.log()/arb(10).log()) if max_stop_power>0 else None,
        'max_pass_power_deviation_upper':float(max_pass_deviation),'precision_bits':ctx.prec,'elapsed_seconds':time.perf_counter()-started}

def process_memory():
    # Windows process counters are kept separate from the mathematical heap model.
    if sys.platform!='win32':return {'status':'unavailable_on_this_platform'}
    import ctypes
    class Counters(ctypes.Structure):
        _fields_=[('cb',ctypes.c_ulong),('faults',ctypes.c_ulong)]+[(x,ctypes.c_size_t) for x in ['peak_ws','ws','peak_paged','paged','peak_nonpaged','nonpaged','commit','peak_commit','private']]
    counters=Counters();counters.cb=ctypes.sizeof(counters)
    api=ctypes.WinDLL('psapi');api.GetProcessMemoryInfo.argtypes=[ctypes.c_void_p,ctypes.c_void_p,ctypes.c_ulong]
    if not api.GetProcessMemoryInfo(ctypes.c_void_p(-1),ctypes.byref(counters),counters.cb):return {'status':'counter_error'}
    return {'peak_working_set_bytes':counters.peak_ws,'working_set_bytes':counters.ws,'private_usage_bytes':counters.private,'process_commit_bytes':counters.commit,'peak_process_commit_bytes':counters.peak_commit}

def main():
    p=argparse.ArgumentParser();p.add_argument('--coefficients',required=True,type=Path);p.add_argument('--spec',required=True,type=Path);p.add_argument('--certificate',required=True,type=Path);p.add_argument('--flint-path',type=Path);p.add_argument('--output',type=Path);a=p.parse_args()
    try:
        result=verify(a.coefficients,json.loads(a.spec.read_text(encoding='utf-8')),json.loads(a.certificate.read_text(encoding='utf-8')),flint_path=a.flint_path)
        result['process_memory']=process_memory()
        code={'PASS':0,'FAIL':1,'INDETERMINATE':3}[result['status']]
    except Exception as e:result={'status':'REJECTED','reason':str(e)};code=2
    if a.output:a.output.write_text(json.dumps(result,indent=2),encoding='utf-8')
    print(json.dumps(result));return code
if __name__=='__main__':sys.exit(main())
