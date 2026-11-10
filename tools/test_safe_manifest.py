"""Counterexamples for registration/classification gate, without executing a test."""
import copy
import json
from pathlib import Path
import subprocess
import sys
import tempfile
sys.stdout.reconfigure(encoding="utf-8", errors="replace")
runner = Path(__file__).with_name("run_safe_tests.py")
with tempfile.TemporaryDirectory(prefix="rlplayer-manifest-controls-") as tmp:
    root = Path(tmp)
    (root / "audited.cpp").write_text("// synthetic audited entry", encoding="utf-8")
    (root / "CTestTestfile.cmake").write_text('add_test(audit_fixture "must-never-be-executed.exe")\n', encoding="utf-8")
    valid = dict(schema=1, reviewed_registration_count=1, tests=[dict(name="audit_fixture",category="offline",sources=["audited.cpp"],reason="synthetic audit control")])
    cases = [("valid",valid,0)]
    for name in ["missing-registration", "duplicate", "invalid-category", "missing-source", "stale-count"]:
        m = copy.deepcopy(valid)
        if name == "missing-registration": m["tests"] = []; m["reviewed_registration_count"] = 0
        if name == "duplicate": m["tests"] *= 2; m["reviewed_registration_count"] = 2
        if name == "invalid-category": m["tests"][0]["category"] = "safe-trust-me"
        if name == "missing-source": m["tests"][0]["sources"] = ["does-not-exist.cpp"]
        if name == "stale-count": m["reviewed_registration_count"] = 9
        cases.append((name,m,1))
    passed = 0
    for name, manifest, fail in cases:
        path = root / (name + ".json")
        path.write_text(json.dumps(manifest), encoding="utf-8")
        r = subprocess.run([sys.executable,str(runner),"--build",str(root),"--source",str(root),"--run-root",str(root/"never-created"),"--manifest",str(path),"--audit-only"], capture_output=True,text=True,encoding="utf-8",errors="replace")
        ok = (r.returncode != 0) == bool(fail)
        print(("PASS" if ok else "FAIL"),name,"exit="+str(r.returncode))
        if not ok: print(r.stdout,r.stderr)
        passed += ok
    if (root / "never-created").exists(): raise RuntimeError("audit executed mutations")
    print("manifest controls",passed,"/",len(cases),"actual test executions=0")
    sys.exit(0 if passed == len(cases) else 1)
