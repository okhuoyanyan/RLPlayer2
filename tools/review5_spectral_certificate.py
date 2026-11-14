"""Continuous finite-FIR certificate generator (Arb ball DFT + Taylor enclosure).

A DFT is used to compute derivatives, NEVER as a continuous-maximum oracle.
For each complete closed frequency cell, a Taylor remainder covers ALL points.
The independent validator uses autocorrelation polynomials and Horner evaluation.
"""
from __future__ import annotations
import argparse, hashlib, json, math, struct, sys, time
from fractions import Fraction
from pathlib import Path

SCHEMA='rlplayer-continuous-fir-v1'
def digest(path):return hashlib.sha256(Path(path).read_bytes()).hexdigest()
def rational(value):return Fraction(str(value))
def rs(value):return str(value.numerator) if value.denominator==1 else str(value)
def load_flint(path=None):
    if path:sys.path.insert(0,str(Path(path).resolve()))
    from flint import arb,acb,fmpq,ctx
    return arb,acb,fmpq,ctx

def certify(coefficients,spec,*,flint_path=None,precision=256,degree=32,grid=None,max_grid=32768):
    arb,acb,fmpq,ctx=load_flint(flint_path);ctx.prec=precision;ctx.threads=1
    if not 192<=precision<=4096 or not 8<=degree<=64:raise ValueError('precision>=192, Taylor degree 8..64 required')
    raw=Path(coefficients).read_bytes()
    if not raw or len(raw)%8:raise ValueError('nonempty little-endian binary64 coefficient file required')
    floats=list(struct.unpack('<'+'d'*(len(raw)//8),raw))
    if not all(math.isfinite(x) for x in floats):raise ValueError('non-finite coefficient')
    exact=[Fraction(*x.as_integer_ratio()) for x in floats]
    reference_gain=sum(exact) if spec.get('normalize_dc',False) else rational(spec.get('reference_gain','1'))
    if not reference_gain:raise ValueError('zero reference gain/DC cannot normalize')
    exact=[x/reference_gain for x in exact]
    coeff=[arb(fmpq(x.numerator,x.denominator)) for x in exact]
    n=len(coeff);spread=max(n-1,1);half=arb(spread)/2
    l1=sum(abs(x) for x in coeff);dc=sum(coeff)
    if not spec.get('bands'):raise ValueError('nonempty trusted frequency bands required')
    if type(spec.get('normalize_dc',False)) is not bool:raise ValueError('normalize_dc must be boolean')
    bands=[]
    for b in spec['bands']:
        lo,hi=rational(b['lo']),rational(b['hi'])
        if not 0<=lo<hi<=Fraction(1,2):raise ValueError('bands must lie within normalized cycles [0,1/2]')
        if b['kind']=='stop':
            maximum=arb(10)**(arb(fmpq(rational(b['db']).numerator,rational(b['db']).denominator))/10)
            minimum=arb(0)
        elif b['kind']=='pass':
            db=rational(b['ripple_db']);maximum=arb(10)**(arb(fmpq(db.numerator,db.denominator))/10);minimum=1/maximum
        else:raise ValueError('unknown band kind')
        bands.append((b,lo,hi,minimum,maximum))
    g=grid or 1
    if not grid:
        while g<max(64,4*n):g*=2
    if g&(g-1) or g<n:raise ValueError('grid must be power of two and >= FIR support')
    started=time.perf_counter();attempts=[]
    while True:
        # Conservative Python ball-object memory ceiling before allocating moments.
        if g>max_grid or g*degree*(256+precision//4)>512*1024**2:
            return {'schema':SCHEMA,'status':'INDETERMINATE','reason':'explicit_grid_or_memory_budget','attempts':attempts}
        weights=coeff[:];moments=[]
        norm=[arb(2*i-(n-1))/spread for i in range(n)]
        for k in range(degree):
            if k:weights=[w*u/k for w,u in zip(weights,norm)]
            moments.append(acb.dft(weights+[arb(0)]*(g-n)))
        leaves=[];unresolved=0;violations=0;largest_power=arb(0)
        for band_id,(definition,lo,hi,minimum,maximum) in enumerate(bands):
            first=max(0,math.ceil(lo*g-Fraction(1,2)));last=min(g//2,math.floor(hi*g+Fraction(1,2)))
            for center in range(first,last+1):
                left=max(lo,Fraction(2*center-1,2*g));right=min(hi,Fraction(2*center+1,2*g))
                if left>=right:continue
                radius=max(abs(left-Fraction(center,g)),abs(right-Fraction(center,g)))
                t=half*(2*arb.pi())*arb(fmpq(radius.numerator,radius.denominator))
                q=[moments[k][center]*((-acb(0,1))**k) for k in range(degree)]
                power=[]
                for j in range(degree):
                    power.append(sum(q[k]*q[j-k].conjugate() for k in range(j+1)).real)
                remainder=(l1*l1*(2*t)**degree/math.factorial(degree)).upper()
                variation=remainder
                for j in range(1,degree):variation+=power[j].abs_upper()*t**j
                lower=(power[0]-variation).lower();upper=(power[0]+variation).upper()
                if lower<0:lower=arb(0)
                # Conservative serialized slack allows the independent evaluator
                # to use a different operation graph without trusting our rounding.
                claimed_lo=(lower*(1-arb('1e-9'))-arb('1e-50')).lower()
                if claimed_lo<0:claimed_lo=arb(0)
                claimed_hi=(upper*(1+arb('1e-9'))+arb('1e-50')).upper()
                good=claimed_hi<maximum.lower() and (definition['kind']!='pass' or claimed_lo>minimum.upper())
                pointlo=power[0].lower();pointhi=power[0].upper()
                bad=pointlo>maximum.upper() or (definition['kind']=='pass' and pointhi<minimum.lower())
                status='PASS' if good else 'FAIL' if bad else 'INDETERMINATE'
                unresolved+=status=='INDETERMINATE';violations+=status=='FAIL'
                if largest_power<claimed_hi:largest_power=claimed_hi
                leaves.append({'band':band_id,'lo':rs(left),'hi':rs(right),'center':rs(Fraction(center,g)),
                    'lower_power':str(claimed_lo.fmpq()),'upper_power':str(claimed_hi.fmpq()),'status':status})
        attempts.append({'grid':g,'leaves':len(leaves),'unresolved':unresolved,'violations':violations})
        status='FAIL' if violations else 'INDETERMINATE' if unresolved else 'PASS'
        result={'schema':SCHEMA,'status':status,'coefficient_sha256':hashlib.sha256(raw).hexdigest(),'coefficient_count':n,
            'spec_sha256':hashlib.sha256(json.dumps(spec,sort_keys=True,separators=(',',':')).encode()).hexdigest(),
            'precision_bits':precision,'degree':degree,'grid':g,'method':'Arb ball DFT moments; finite Taylor polynomial plus global Lagrange remainder for power',
            'normalization':{'mode':'exact DC rational' if spec.get('normalize_dc',False) else 'exact reference gain','denominator':rs(reference_gain)},
            'dc_power_ball':str(dc*dc),'runtime':{'python':sys.version.split()[0],'flint':__import__('flint').__version__,'threads':1},
            'attempts':attempts,'leaves':leaves,'elapsed_seconds':time.perf_counter()-started}
        if status!='INDETERMINATE' or g*2>max_grid:return result
        del moments;g*=2

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
    p=argparse.ArgumentParser();p.add_argument('--coefficients',required=True,type=Path);p.add_argument('--spec',required=True,type=Path);p.add_argument('--output',required=True,type=Path)
    p.add_argument('--flint-path',type=Path);p.add_argument('--precision',type=int,default=256);p.add_argument('--degree',type=int,default=32);p.add_argument('--grid',type=int);p.add_argument('--max-grid',type=int,default=32768)
    a=p.parse_args()
    try:
        spec=json.loads(a.spec.read_text(encoding='utf-8'));result=certify(a.coefficients,spec,flint_path=a.flint_path,precision=a.precision,degree=a.degree,grid=a.grid,max_grid=a.max_grid)
        result['process_memory']=process_memory()
        a.output.parent.mkdir(parents=True,exist_ok=True);a.output.write_text(json.dumps(result,indent=2),encoding='utf-8')
        print(json.dumps({k:v for k,v in result.items() if k!='leaves'}));return 0 if result['status']=='PASS' else 1 if result['status']=='FAIL' else 3
    except Exception as e:
        print(json.dumps({'status':'REJECTED','reason':str(e)}));return 2
if __name__=='__main__':sys.exit(main())
