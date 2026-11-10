"""Small content-freshness counterexamples. No HIP compiler or DLL is executed."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
sys.stdout.reconfigure(encoding="utf-8",errors="replace")
with tempfile.TemporaryDirectory(prefix="rlplayer-hip-provenance-") as tmp:
    root=Path(tmp);source=root/"source";(source/"src/dsp").mkdir(parents=True);(source/"tools").mkdir();(root/"compiler").mkdir()
    header=source/"src/dsp/fft.h";header.write_text("original")
    for name in ["build_gpu.ps1","hip_build_provenance.py"]:(source/"tools"/name).write_text("mock build input")
    compiler=root/"compiler/hipcc.bat";compiler.write_text("never executed");(compiler.parent/"clang++.exe").write_bytes(b"never executed")
    dll=root/"rtagpu.dll";dll.write_bytes(b"mock artifact, never loaded");snapshot=root/"snapshot.json";record=root/"record.json"
    results=[]
    def run(name,mode,expected):
        r=subprocess.run([sys.executable,str(Path(__file__).with_name("hip_build_provenance.py")),mode,"--source",str(source),"--compiler",str(compiler),"--dll",str(dll),"--snapshot",str(snapshot),"--record",str(record)],capture_output=True,text=True)
        ok=(r.returncode==0)==expected;results.append(ok);print("PASS" if ok else "FAIL",name,"exit="+str(r.returncode))
        if not ok:print(r.stdout,r.stderr)
    run("missing-record","verify",False);run("snapshot","snapshot",True);run("seal","seal",True);run("valid","verify",True)
    prior=header.stat();header.write_text("changed");os.utime(header,ns=(prior.st_atime_ns,prior.st_mtime_ns))
    run("changed-header-with-backdated-mtime","verify",False);run("inputs-changed-during-build","seal",False)
    header.write_text("original");dll.write_bytes(b"copied older artifact");run("replaced-dll","verify",False)
    print("provenance controls",sum(results),"/",len(results));sys.exit(0 if all(results) else 1)
