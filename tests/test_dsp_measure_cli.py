"""Exercise the built production CLI, including deliberate coefficient faults."""
import argparse
from pathlib import Path
import subprocess
import tempfile
import sys
import json
import csv

p=argparse.ArgumentParser()
p.add_argument("--exe",required=True,type=Path)
a=p.parse_args()
validation_directory=tempfile.TemporaryDirectory(prefix="dsp-measure-arguments-")
def run(args, expected):
    r=subprocess.run([str(a.exe),*args],capture_output=True,text=True,cwd=validation_directory.name)
    assert r.returncode==expected,(args,r.returncode,r.stdout,r.stderr)
    print("PASS",expected," ".join(args),flush=True)

for args in [["--max-fails","100"],["--unknown"],["report.tsv"],["--preset"],
             ["--two-stage","invalid"],
             ["--preset","missing-preset"],["--in-rate","44100"],
             ["--in-rate","0","--out-rate","0"],
             ["--in-rate","-1","--out-rate","705600"],["--fft-factor","15"],
             ["--fft-factor","16garbage"],["--points","nan"],
             ["--limit","banana"],["--offset","999999"]]:
    run(args,2)
with tempfile.TemporaryDirectory(prefix="dsp-measure-") as d:
    out=str(Path(d)/"quality.tsv")
    base=["--preset","poly-sinc-short-lp","--in-rate","44100","--out-rate","705600","--output",out]
    run(base,0)
    assert "DENSE_SAMPLED_TARGET_MET" in Path(out).read_text()
    run(base+["--inject-regression"],1)
    assert "CONFIRMED_SAMPLED_FAIL" in Path(out).read_text()
    baseline=Path(d)/"baseline.txt"
    baseline.write_text("44100/705600/poly-sinc-short-lp/DENSE_SAMPLED_TARGET_MET\n")
    run(base+["--regression-baseline",str(baseline)],0)
    run(base+["--regression-baseline",str(baseline),"--inject-regression"],1)
    baseline.write_text("48000/768000/poly-sinc-short-lp/DENSE_SAMPLED_TARGET_MET\n")
    run(base+["--regression-baseline",str(baseline)],1)
    run(base+["--points","NaN"],2)
    run(base+["--points","352801"],2)
    run(base+["--points","20000,22394.53125"],0)
    assert "dd_db\texact_db" in Path(out).read_text()
    export=Path(d)/"coefficients"
    run(["--preset","poly-sinc-mqa/mp3-mp","--in-rate","44100","--out-rate","705600",
         "--points","20000,24000","--output",out,"--coefficients",str(export)],0)
    raw=(export/"44100-705600-poly-sinc-mqa/mp3-mp.f64").read_bytes()
    assert len(raw)>0 and len(raw)%8==0
    reference=Path(__file__).resolve().parents[1]/"tools/dsp_measure_reference.py"
    refout=Path(d)/"slash-reference.json"
    result=subprocess.run([sys.executable,str(reference),"--table",out,"--coefficients",str(export),"--output",str(refout)],capture_output=True,text=True)
    assert result.returncode==0,(result.stdout,result.stderr)
    values=json.loads(refout.read_text())
    assert len(values)==2 and max(abs(float(x["delta_dd_db"])) for x in values)<1e-8
    print("PASS slash-preset export and independent-90-digit reader",flush=True)
    # Exercise apod_check itself, not merely a similar test-only predicate.
    # Original declared F1 for apod-20k is 18.4k at 44.1k input.
    run(["--preset","apod-20k","--in-rate","44100","--out-rate","705600",
         "--points","18400","--output",out],0)
    with Path(out).open() as f:
        endpoint=next(csv.DictReader((s for s in f if not s.startswith("#")),delimiter="\t"))
    declared=float(endpoint["exact_db"])
    apod_exe=a.exe.with_name("apod_check.exe" if a.exe.suffix==".exe" else "apod_check")
    result=subprocess.run([str(apod_exe),"--preset","apod-20k","--in-rate","44100",
        "--out-rate","705600","--output",out],capture_output=True,text=True)
    assert result.returncode in (0,1),(result.stdout,result.stderr)
    with Path(out).open() as f:
        measured=next(csv.DictReader((s for s in f if not s.startswith("#")),delimiter="\t"))
    assert abs(float(measured["pass_db"])-declared)<1e-9,(measured,declared)
    assert abs(float(measured["pass_hz"])-18400)<1e-8,measured
    print("PASS actual-apod-CLI-strict-declared-F1",flush=True)
    manifest=Path(d)/"forced-on-manifest.txt"
    run(base+["--two-stage","on","--manifest",str(manifest)],0)
    with Path(out).open() as f:
        forced=next(csv.DictReader((s for s in f if not s.startswith("#")),delimiter="\t"))
    assert forced["topology"]=="cascade" and forced["requested_topology"]=="on"
    assert "/topology=on/" in manifest.read_text()
    run(base+["--two-stage","off"],0)
    with Path(out).open() as f:
        forced=next(csv.DictReader((s for s in f if not s.startswith("#")),delimiter="\t"))
    assert forced["topology"]=="single" and forced["requested_topology"]=="off"
