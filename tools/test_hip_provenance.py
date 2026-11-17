"""Real include-closure counterexamples. No HIP compiler or DLL is executed."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
sys.stdout.reconfigure(encoding="utf-8",errors="replace")
with tempfile.TemporaryDirectory(prefix="rlplayer-hip-provenance-") as tmp:
    root=Path(tmp);source=root/"source"
    for directory in ["src/gpu","src/dsp/detail","src/net","tools"]:(source/directory).mkdir(parents=True)
    (root/"compiler").mkdir()
    # The build lists two real translation units; both reach fft.h through a
    # shared header, and fft.h itself reaches a relative transitive include.
    units=["src/gpu/gpu_conv_hip.cpp","src/gpu/gpu_resample_hip.cpp"]
    (source/"tools/build_gpu.ps1").write_text("$src = @(\n"+",\n".join(" (Join-Path $root '"+name.replace('/',chr(92))+"')" for name in units)+"\n)\n")
    for name in units:(source/name).write_text('#include "gpu_common.h"\nint fixture = 1;\n')
    (source/"src/gpu/gpu_common.h").write_text('#include "dsp/fft.h"\n')
    header=source/"src/dsp/fft.h";header_text='#include "detail/twiddle.h"\n#define FFT_FIXTURE 1\n';header.write_text(header_text)
    transitive=source/"src/dsp/detail/twiddle.h";transitive.write_text('#define TWIDDLE_FIXTURE 1\n')
    ui=source/"src/net/ui_script.h";ui.write_text('unrelated UI version 1\n')
    compiler=root/"compiler/hipcc.bat";compiler.write_text("never executed");(compiler.parent/"clang++.exe").write_bytes(b"never executed")
    dll=root/"rtagpu.dll";dll.write_bytes(b"mock artifact, never loaded");snapshot=root/"snapshot.json";record=root/"record.json"
    results=[]
    def run(name,mode,expected,error_contains=""):
        r=subprocess.run([sys.executable,str(Path(__file__).with_name("hip_build_provenance.py")),mode,"--source",str(source),"--compiler",str(compiler),"--dll",str(dll),"--snapshot",str(snapshot),"--record",str(record)],capture_output=True,text=True)
        ok=(r.returncode==0)==expected and (not error_contains or error_contains in r.stdout+r.stderr)
        results.append(ok);print("PASS" if ok else "FAIL",name,"exit="+str(r.returncode))
        if not ok:print(r.stdout,r.stderr);raise AssertionError(name)
    run("missing-record","verify",False,"Missing build provenance")
    run("snapshot","snapshot",True)
    inputs=json.loads(snapshot.read_text())["inputs"]
    expected=set(units+["src/gpu/gpu_common.h","src/dsp/fft.h","src/dsp/detail/twiddle.h"])
    closure_ok={key for key in inputs if key.startswith("src/") }==expected
    results.append(closure_ok);print("PASS" if closure_ok else "FAIL","exact reachable include closure excludes UI")
    if not closure_ok:raise AssertionError(inputs)
    run("seal","seal",True);run("valid","verify",True)
    ui.write_text('unrelated UI version 2\n');run("unrelated-ui-change-keeps-seal","verify",True)
    prior=header.stat();header.write_text(header_text.replace("FIXTURE 1","FIXTURE 2"));os.utime(header,ns=(prior.st_atime_ns,prior.st_mtime_ns))
    run("changed-header-with-backdated-mtime","verify",False,"Stale HIP source/toolchain content: ['src/dsp/fft.h']")
    run("inputs-changed-during-build","seal",False,"Stale HIP source/toolchain content: ['src/dsp/fft.h']")
    header.write_text(header_text);run("restored-header-valid","verify",True)
    transitive.write_text('#define TWIDDLE_FIXTURE 2\n')
    run("transitive-header-edit","verify",False,"Stale HIP source/toolchain content: ['src/dsp/detail/twiddle.h']")
    transitive.write_text('#define TWIDDLE_FIXTURE 1\n')
    unit=source/units[1];saved=unit.read_text();unit.unlink()
    run("missing-listed-translation-unit","verify",False,"GPU translation unit missing: ['src/gpu/gpu_resample_hip.cpp']")
    unit.write_text(saved);run("restored-closure-valid","verify",True)
    dll.write_bytes(b"copied older artifact");run("replaced-dll","verify",False,"DLL hash differs")
    dll.write_bytes(b"mock artifact, never loaded");run("restored-dll-valid","verify",True)
    compiler.write_text("changed compiler");run("changed-compiler","verify",False,"Stale HIP source/toolchain content: ['compiler:hipcc']")
    print("provenance controls",sum(results),"/",len(results));sys.exit(0 if all(results) else 1)
