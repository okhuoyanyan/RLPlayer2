"""The reference process must reject wrong results, not just print their error."""
import argparse
import csv
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

p=argparse.ArgumentParser()
p.add_argument('--exe',required=True,type=Path)
a=p.parse_args()
reference=Path(__file__).resolve().parents[1]/'tools/dsp_measure_reference.py'
env=dict(os.environ,RLPLAYER_FILTER_CACHE='0')
def run(args,expected):
    r=subprocess.run(list(map(str,args)),env=env,capture_output=True,text=True,encoding='utf-8',errors='replace')
    if r.returncode!=expected:
        raise RuntimeError(f'expected exit {expected}, got {r.returncode}: {args}\n{r.stdout}\n{r.stderr}')
    print('PASS exit',expected,Path(args[0]).name,flush=True)
    return r
with tempfile.TemporaryDirectory(prefix='rlplayer-review4-reference-') as tmp:
    d=Path(tmp)
    r=run([a.exe,d],0)
    print(r.stdout,flush=True)
    args=[sys.executable,reference,'--table',d/'calibration.tsv','--coefficients',d]
    for precision in [90,120]:
        r=run([*args,'--digits',precision,'--output',d/f'ref{precision}.json'],0)
        print(r.stdout.splitlines()[-1],flush=True)
    refs=[json.loads((d/f'ref{x}.json').read_text()) for x in [90,120]]
    if len(refs[0])!=14 or len(refs[1])!=14:
        raise RuntimeError('calibration identity count changed')
    for x,y in zip(*refs):
        if x['identity']!=y['identity'] or abs(float(x['db90'])-float(y['db90']))>1e-9:
            raise RuntimeError('reference does not converge at increased precision')
    with (d/'calibration.tsv').open() as f:
        rows=list(csv.DictReader(f,delimiter='\t'))
    rows[0]['dd_db']='-300' # DC of a unity impulse is exactly 0 dB.
    def write_rows(values):
        with (d/'broken.tsv').open('w',newline='') as f:
            w=csv.DictWriter(f,fieldnames=rows[0],delimiter='\t');w.writeheader();w.writerows(values)
    write_rows(rows[:1])
    broken=[sys.executable,reference,'--table',d/'broken.tsv','--coefficients',d,'--output',d/'bad.json']
    r=run(broken,1)
    if 'REFERENCE_MISMATCH' not in r.stdout:raise RuntimeError('wrong failure reason')
    rows[0]['dd_db']='nan';write_rows(rows[:1]);run(broken,1)
    write_rows([]);run(broken,2)
print('PASS independent-reference normal/300dB-error/NaN/empty-selection controls',flush=True)
