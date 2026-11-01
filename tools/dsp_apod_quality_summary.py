from pathlib import Path
import sys
import csv,json,math
root=Path(sys.argv[1]) if len(sys.argv)>1 else Path(__file__).resolve().parents[2]/'evidence'/'apod'
single=list(csv.DictReader((root/'coefficients-dd-L32.csv').open()))
time=json.loads((root/'coefficients-analysis.json').read_text())
rows=[]
for rate,L in [(44100,16),(44100,32),(48000,16),(48000,32),(96000,4)]:
 for new in single:
  if int(new['L'])!=L or new['mode']!='quality':continue
  old=next(r for r in single if r['preset']==new['preset'] and r['L']==new['L'] and r['mode']=='old')
  t=next(r for r in time if r['file']==new['file'])
  rows.append({'preset':new['preset'],'inRate':rate,'outRate':rate*L,'L':L,
    'oldT':old['T'],'newT':new['T'],'oldF1db':old['passDb'],'newWorstPassDb':new['passDb'],
    'newWorstStopDb':new['stopDb'],'stopPeakHz':float(new['stopHz'])*rate/44100,
    'pre60_ms':t['pre60_ms']*44100/rate,'groupDelay_ms':t['delay_ms']*44100/rate,
    'coefficientBytes':int(new['T'])*L*8,'oldStrictFail':int(abs(float(old['passDb']))>.01),
    'newStrictFail':int(new['status']!='DENSE_SAMPLED_TARGET_MET'),
    'scope':'DD>=16N grid + endpoints + 8x17 local refinements; not interval certified'})
with(root/'production-40-before-after.csv').open('w',newline='',encoding='utf-8-sig')as f:
 w=csv.DictWriter(f,fieldnames=rows[0].keys());w.writeheader();w.writerows(rows)
cross=json.loads((root/'DD-mpmath-crosscheck.json').read_text())
summary={'production_cases':len(rows),'old_strict_fails':sum(r['oldStrictFail'] for r in rows),
 'new_strict_fails':sum(r['newStrictFail'] for r in rows),
 'new_worst_pass_db':min(float(r['newWorstPassDb']) for r in rows),
 'max_DD_mp90_delta_db':max(float(r['deltaDD_90']) for r in cross),
 'max_60_90_delta_db':max(float(r['delta60_90']) for r in cross),
 'mp90_crosschecks':len(cross),'performance':'Owned by root; no real-time claim in this subtask',
 'continuous_band':'All bands scanned densely; continuous stop-band interval proof remains unestablished'}
(root/'summary.json').write_text(json.dumps(summary,indent=2),encoding='utf8')
print(json.dumps(summary,indent=2))
