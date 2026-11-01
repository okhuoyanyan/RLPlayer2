"""Dense FFT screening, time envelope and mpmath-90 verification of real f64 exports.
FFT results below its calibrated floor are screening values, never certification.
"""
from pathlib import Path
import argparse,csv,json,math,hashlib
import numpy as np
from scipy.fft import rfft,next_fast_len
from scipy.signal import find_peaks
import mpmath as mp

pa=argparse.ArgumentParser();pa.add_argument('directory',type=Path)
pa.add_argument('--max-l',type=int,default=1024);pa.add_argument('--precision',action='store_true')
pa.add_argument('--only-quality',action='store_true');pa.add_argument('--only-l',type=int)
args=pa.parse_args();root=args.directory
rows=list(csv.DictReader((root/'coefficients.csv').open()));results=[]
for row in rows:
    L=int(row['L']);T=int(row['T']);f1=float(row['F1']);f2=float(row['F2'])
    if L>args.max_l:continue
    if args.only_quality and row['mode']!='quality':continue
    if args.only_l and L!=args.only_l:continue
    raw=(root/row['file']).read_bytes();h=np.frombuffer(raw,dtype='<f8');N=len(h)
    nfft=next_fast_len(N*16);H=rfft(h,nfft);v=np.abs(H)
    db=lambda x:20*math.log10(max(1e-300,float(x)))
    i1=int(math.floor((f1/L)*nfft/2));i2=int(math.ceil(((2-f1)/L)*nfft/2))
    pass_min_idx=int(np.argmin(v[:i1+1]));pass_max_idx=int(np.argmax(v[:i1+1]))
    stop_idx=i2+int(np.argmax(v[i2:]));peak=int(np.argmax(np.abs(h)))
    # Outermost sample above -60 dB relative to the actual impulse peak.
    above=np.flatnonzero(abs(h)>=abs(h[peak])*0.001)
    center=float(row.get('latencySamples',peak))
    delay=center/(44100*L)*1000
    reflection=int(round(2*center));indices=np.arange(N);mirror=reflection-indices
    paired=(mirror>=0)&(mirror<N)
    symmetry=float(np.max(abs(h[indices[paired]]-h[mirror[paired]]))/abs(h[peak]))
    x=np.arange(N)-center
    pre=float(np.sum(h[x<0]**2));post=float(np.sum(h[x>0]**2))
    centered=H[:i1+1]*np.exp(2j*np.pi*np.arange(i1+1)*center/nfft)
    # Chord interpolation remainder for centered complex DTFT:
    # <= step^2/8 * sum |h[n]|*(n-peak)^2. This is a conservative
    # analytic budget, not an outward-rounded interval arithmetic proof.
    interpolation_budget=(2*np.pi/nfft)**2/8*float(np.sum(abs(h)*x*x))
    entry={**row,'sha256':hashlib.sha256(raw).hexdigest(),'fft':nfft,
      'dc_db':db(math.fsum(h)), 'pass_min_fft_db':db(v[pass_min_idx]),'pass_max_fft_db':db(v[pass_max_idx]),
      'pass_min_hz':pass_min_idx/nfft*44100*L,'stop_fft_db':db(v[stop_idx]),'stop_peak_hz':stop_idx/nfft*44100*L,
      'delay_ms':delay,'pre60_ms':(center-int(above[0]))/(44100*L)*1000,
      'post60_ms':(int(above[-1])-center)/(44100*L)*1000,'pre_post_energy':pre/post,
      'symmetry_relative':symmetry,'centroid_minus_center_samples':float(np.sum(h*h*x)/np.sum(h*h)),
      'pass_phase_residual_rad':float(np.max(abs(np.angle(centered)))),
      'pass_interpolation_amplitude_budget':interpolation_budget,
      'energy_outside_3_5ms':float(np.sum(h[abs(x)>3.5/1000*44100*L]**2)/np.sum(h*h))}
    if args.precision:
      mp.mp.dps=90
      # Binary64 coefficients and binary64 declared F1 are converted exactly.
      # Anchor mathematical Hz from the original rational preset declaration too.
      points={'F1':mp.mpf(f1)/L,'F2':mp.mpf(f2)/L,'mid':mp.mpf(f1+f2)/2/L,
              'image':mp.mpf(2-f1)/L,'fft_stop_peak':mp.mpf(2*stop_idx)/nfft,
              'pass_min':mp.mpf(2*pass_min_idx)/nfft,'pass_max':mp.mpf(2*pass_max_idx)/nfft}
      precise={}
      for name,freq in points.items():
        vals=[]
        for precision in [60,90]:
          mp.mp.dps=precision;step=mp.exp(-1j*mp.pi*freq);z=mp.mpc(1);s=mp.mpc(0)
          for a in h:s+=mp.mpf(float(a))*z;z*=step
          vals.append(20*mp.log10(abs(s)))
        precise[name]={'db90':str(vals[1]),'delta60_90':str(abs(vals[0]-vals[1]))}
      entry['precise']=precise
    results.append(entry)
    (root.parent/(root.name+('-analysis-precise.json' if args.precision else '-analysis.json'))).write_text(json.dumps(results,indent=2),encoding='utf8')
    print(row['file'],'pass',entry.get('precise',{}).get('F1',{}).get('db90',entry['pass_min_fft_db']),
          'stopFFT',entry['stop_fft_db'],'pre60',entry['pre60_ms'],flush=True)
