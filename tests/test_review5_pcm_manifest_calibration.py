"""Independent exact Q62->DD and Arb finite-point calibration for manifest tool."""
import argparse,json,struct,hashlib,sys
from pathlib import Path
from fractions import Fraction as F
p=argparse.ArgumentParser();p.add_argument('--manifest',type=Path,required=True);p.add_argument('--q62-fixture',type=Path,required=True);p.add_argument('--flint-path',type=Path,required=True);p.add_argument('--output',type=Path,required=True);a=p.parse_args();fix=json.loads(a.q62_fixture.read_text())
assert all(F.from_float(x['hi'])+F.from_float(x['lo'])==F(x['q'],1<<62) for x in fix)
lost=sum(F.from_float(float(x['q'])/(1<<62))!=F(x['q'],1<<62) for x in fix);assert lost>0,'negative control must expose naive q->double low-bit loss'
sys.path.insert(0,str(a.flint_path));from flint import arb,acb,ctx,fmpq
ctx.prec=256;ctx.threads=1;rows=json.loads(a.manifest.read_text())['rows'];results=[];sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
for preset,ir,orr in [('poly-sinc-short-lp',96000,44100),('poly-sinc-short-mp',44100,384000),('poly-sinc-ext2-hires-ip',44100,384000)]:
 r=next(x for x in rows if x['preset']==preset and x['input_rate']==ir and x['output_rate']==orr)
 for f in [r['pass_hz'],r['mirror_stop_hz']]:
  total=acb(1)
  for s in r['stages']:
   file=Path(r['folder'])/(s.get('fixed_file') or s['double_file']);assert sha(file)==s['fixed_sha256' if s['useFixed'] else 'double_sha256'];raw=file.read_bytes();vals=struct.unpack('<'+('q' if s['useFixed'] else 'd')*s['count'],raw);z=acb(0);freq=arb(f)/s['coefficient_rate']
   for n,v in enumerate(vals):
    x=arb(fmpq(v,1<<62)) if s['useFixed'] else arb(v);angle=freq*(-2*n);z+=acb(angle.cos_pi(),angle.sin_pi())*x
   total*=z/s['L']
  db=20*abs(total).log()/arb(10).log();actual=next(x['desired_response_db'] for x in r['points'] if x['hz']==f);error=float(abs(db-arb(actual)).upper());assert error<1e-6,(preset,f,error)
  assert abs(db-arb(actual+1))>arb('0.000001'),'bad +1dB must be rejected by same calibrated threshold'
  results.append({'preset':preset,'input_rate':ir,'output_rate':orr,'hz':f,'dd_db':actual,'Arb256_db_enclosure':str(db),'absolute_error_upper_db':error})
report={'status':'PASS','exact_Q62_to_DD_cases':len(fix),'naive_double_cast_loses_bits_rejected_cases':lost,'actual_Arb_points':results,'bad_plus_one_db_points_rejected':len(results),'scope':'267 generic signed-int64 low-bit checks and6 actual finite response points; no continuous/all-manifest precision proof','manifest_sha256':sha(a.manifest),'q62_fixture_sha256':sha(a.q62_fixture)};a.output.write_text(json.dumps(report,indent=2));print(json.dumps(report))
