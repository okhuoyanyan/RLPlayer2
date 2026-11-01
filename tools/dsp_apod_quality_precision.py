"""60/90 decimal digit check of actual exported binary64 coefficients at DD peaks."""
from pathlib import Path
import sys
import csv,json,hashlib,struct
import mpmath as mp
root=Path(sys.argv[1]) if len(sys.argv)>1 else Path(__file__).resolve().parents[2]/'evidence'/'apod'
checks=[]
for directory,table,L,preset in [
 ('coefficients','coefficients-dd-L32.csv',16,None),
 ('cascade','cascade-dd-L32.csv',16,None),
 ('coefficients','coefficients-dd-L1024.csv',1024,'apod-20k'),
 ('cascade','cascade-dd-L1024.csv',1024,'apod-wide')]:
 for r in csv.DictReader((root/table).open()):
  if r['mode']!='quality' or int(r['L'])!=L or (preset and r['preset']!=preset):continue
  path=root/directory/r['file'];raw=path.read_bytes();h=struct.unpack('<'+'d'*(len(raw)//8),raw)
  vals=[]
  for precision in [60,90]:
   mp.mp.dps=precision;freq=mp.mpf(float(r['stopHz']))/(44100*L)
   step=mp.exp(-2j*mp.pi*freq);z=mp.mpc(1);v=mp.mpc(0)
   for a in h:v+=mp.mpf(a)*z;z*=step
   vals.append(20*mp.log10(abs(v)))
  entry={'directory':directory,'file':r['file'],'sha256':hashlib.sha256(raw).hexdigest(),
    'hz':r['stopHz'],'DD_db':r['stopDb'],'db60':str(vals[0]),'db90':str(vals[1]),
    'delta60_90':str(abs(vals[0]-vals[1])),'deltaDD_90':str(abs(mp.mpf(r['stopDb'])-vals[1]))}
  checks.append(entry);(root/'DD-mpmath-crosscheck.json').write_text(json.dumps(checks,indent=2),encoding='utf8')
  print(directory,r['file'],'DD',r['stopDb'],'mp90',mp.nstr(vals[1],20),'delta',mp.nstr(mp.mpf(r['stopDb'])-vals[1],8),flush=True)
