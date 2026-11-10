"""Independent 90-digit pole design / 50-digit scalar 1-bit error-feedback oracle.
No production design/state implementation is imported. Dither is explicitly off.
"""
import argparse,decimal,json,math,time
from pathlib import Path
import mpmath as mp
p=argparse.ArgumentParser();p.add_argument('--report',required=True,type=Path);p.add_argument('--samples',type=int,default=65536);a=p.parse_args()
mp.mp.dps=90;decimal.getcontext().prec=50;D=decimal.Decimal
rows=[]
for rate in [45158400,49152000]:
 for cutoff in [20000,100000]:
  started=time.perf_counter();n=5;wc=mp.tan(mp.pi*cutoff/rate);pole=(1-wc)/(1+wc)
  coeff=[[-pole,mp.mpf(0),pole-1,mp.mpf(0)]]
  for k in range(n//2-1,-1,-1):
   theta=mp.pi*(2*k+n+1)/(2*n);analog=wc/mp.mpc(mp.cos(theta),mp.sin(theta));pole=(1+analog)/(1-analog);q=abs(pole)**2
   coeff.append([-2*pole.real,q,2*(pole.real-1),1-q])
  values=[[D(str(x)) for x in c] for c in coeff];states=[[D(0),D(0)] for _ in coeff];one=D(1);limit=D('1.5');overloads=0;ys=yc=0.;frequency=13*rate/65536
  for i in range(2*a.samples):
   phase=2*math.pi*frequency*i/rate;x=D(str(.62*math.sin(phase)));feedback=sum((z[0] for z in states),D(0));u=x+feedback
   if u>limit:u=limit;overloads+=1
   if u< -limit:u=-limit;overloads+=1
   y=one if u>=0 else -one;e=y-u
   for c,z in zip(values,states):
    out=e+z[0];next1=c[2]*e-c[0]*z[0]+z[1];next2=c[3]*e-c[1]*z[0];z[0]=next1;z[1]=next2;e=out
   if i>=a.samples:
    yy=float(y);ys+=yy*math.sin(phase);yc+=yy*math.cos(phase)
  row=dict(rate=rate,cutoff_hz=cutoff,order=5,arithmetic_digits=50,pole_design_digits=90,samples=a.samples,settle_samples=a.samples,dither_lsb=0,tone_hz=frequency,coherent_rms=math.hypot(ys,yc)*math.sqrt(2)/a.samples,expected=.62/math.sqrt(2),overloads=overloads,elapsed_seconds=time.perf_counter()-started,coefficients=[[str(x) for x in c] for c in values]);rows.append(row)
  print(json.dumps({k:v for k,v in row.items() if k!='coefficients'}),flush=True)
a.report.write_text(json.dumps(dict(schema=1,scope='Independent high-precision reference, not production code',cases=rows),indent=2),encoding='utf-8')
