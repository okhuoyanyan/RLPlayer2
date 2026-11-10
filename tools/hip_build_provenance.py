"""Bind a successful HIP build to content hashes; mtimes are never evidence."""
import argparse
import hashlib
import json
from pathlib import Path
import sys

def digest(p):
    with p.open("rb") as f:
        return hashlib.file_digest(f,"sha256").hexdigest()

def inputs(source, compiler):
    paths = sorted(p for p in (source / "src").rglob("*") if p.is_file() and p.suffix.lower() in {".cpp",".h",".hpp",".cuh",".inl",".c",".cc"})
    paths += [source / "tools/build_gpu.ps1", source / "tools/hip_build_provenance.py"]
    values = {str(p.relative_to(source)).replace("\\","/"):digest(p) for p in paths}
    values["compiler:hipcc"] = digest(compiler)
    clang = compiler.parent / "clang++.exe"
    if not clang.is_file(): raise ValueError("HIP clang compiler missing")
    values["compiler:clang++"] = digest(clang)
    return values

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument("mode",choices=["snapshot","seal","verify"])
    p.add_argument("--source",required=True,type=Path)
    p.add_argument("--compiler",required=True,type=Path)
    p.add_argument("--dll",type=Path)
    p.add_argument("--snapshot",type=Path)
    p.add_argument("--record",type=Path)
    a=p.parse_args()
    current=inputs(a.source.resolve(),a.compiler)
    if a.mode=="snapshot":
        if not a.snapshot:raise ValueError("snapshot path required")
        a.snapshot.write_text(json.dumps(dict(inputs=current),indent=2),encoding="utf-8")
    else:
        if not a.dll or not a.record:raise ValueError("dll and record paths required")
        prior=a.snapshot if a.mode=="seal" else a.record
        if not prior or not prior.is_file():raise ValueError("Missing build provenance; rebuild HIP, do not accept timestamp-only DLL")
        record=json.loads(prior.read_text(encoding="utf-8"))
        if record.get("inputs")!=current:
            old=record.get("inputs",{})
            changed=[k for k in set(old)|set(current) if old.get(k)!=current.get(k)]
            raise ValueError("Stale HIP source/toolchain content: "+str(sorted(changed)[:12]))
        dll_hash=digest(a.dll)
        if a.mode=="verify" and record.get("dll_sha256")!=dll_hash:raise ValueError("DLL hash differs from successful build record")
        if a.mode=="seal":
            a.record.write_text(json.dumps(dict(schema=1,inputs=current,dll_sha256=dll_hash,scope="Content freshness only; code-object gate and hardware smoke remain mandatory"),indent=2),encoding="utf-8")
    print("HIP provenance PASS",a.mode,"inputs="+str(len(current)))
    return 0
if __name__=="__main__":
    try:sys.exit(main())
    except Exception as e:print("HIP provenance FAIL:",e,file=sys.stderr);sys.exit(1)
