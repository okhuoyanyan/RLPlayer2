"""Registration and real cache-path counterexamples; no test target is executed."""
import copy
from contextlib import redirect_stdout
import importlib.util
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
from unittest import mock
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
    total = len(cases)

    # Run the production main/key/cache-decision path against real files. Only
    # discovery and the final strict-gate process are mocked: neither synthetic
    # command is ever executed, and no production source is modified.
    spec = importlib.util.spec_from_file_location("safe_runner_cache_controls", runner)
    cache_runner = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(cache_runner)
    source = root / "cache-source"
    build = root / "cache-build"
    source.mkdir()
    build.mkdir()
    (source / "audited.py").write_text("# stable declared source\n", encoding="utf-8")
    fixture = source / "tests" / "fixtures" / "sample.bin"
    header = source / "src" / "dsp" / "filter.h"
    for path in (fixture, header):
        path.parent.mkdir(parents=True)
        path.write_bytes(b"original-content\n")
    binary_name = "cache_existing_command"
    runtime_name = "cache_runtime_command"
    all_names = sorted([binary_name, runtime_name])
    discovery = [dict(name=binary_name, command=[sys.executable, "--never-executed"]),
                 dict(name=runtime_name, command=["python", "--never-executed"])]
    cache_manifest = dict(schema=1, reviewed_registration_count=2, tests=[
        dict(name=name, category="offline", sources=["audited.py"], reason="mocked cache control")
        for name in all_names])
    manifest_path = root / "cache-manifest.json"
    manifest_path.write_text(json.dumps(cache_manifest), encoding="utf-8")
    cache_path = build / "safe-gate-cache.json"
    sequence = 0

    def check(name, ok, detail=""):
        global passed, total
        total += 1
        passed += bool(ok)
        print(("PASS" if ok else "FAIL"), name, detail)

    def invoke():
        global sequence
        sequence += 1
        run_root = root / ("cache-run-" + str(sequence))
        launches = []

        def discover(command, **kwargs):
            expected = ["ctest", "--test-dir", str(build.resolve()), "--show-only=json-v1"]
            if command != expected:
                raise AssertionError("Unexpected subprocess: " + repr(command))
            return subprocess.CompletedProcess(command, 0, json.dumps(dict(tests=discovery)), "")

        class MockStrictGate:
            def __init__(self, command, **kwargs):
                expected = ["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
                            str(runner.with_name("check_ctest.ps1"))]
                if command[:6] != expected:
                    raise AssertionError("Unexpected gate: " + repr(command))
                launches.append(command)
                self.stdout = iter(["Mock strict gate: test target executions=0\n"])

            def wait(self):
                return 0

        argv = [str(runner), "--build", str(build), "--source", str(source),
                "--run-root", str(run_root), "--manifest", str(manifest_path)]
        output = io.StringIO()
        with mock.patch.object(sys, "argv", argv), \
             mock.patch.object(cache_runner.subprocess, "run", side_effect=discover), \
             mock.patch.object(cache_runner.subprocess, "Popen", MockStrictGate), \
             redirect_stdout(output):
            rc = cache_runner.main()
        if rc != 0:
            raise AssertionError("Cache runner failed: " + output.getvalue())
        saved = json.loads(cache_path.read_text(encoding="utf-8"))
        audit = json.loads((run_root / "audit.json").read_text(encoding="utf-8"))
        if len(launches) != bool(audit["executed"]):
            raise AssertionError("Recorded execution did not match strict-gate decision")
        return {name: saved["entries"][name]["key"] for name in all_names}, audit, saved

    def verify_decision(label, audit, expected):
        check(label + " execution decision",
              audit["executed"] == sorted(expected) and
              audit["cached"] == sorted(set(all_names) - set(expected)),
              "executed=" + repr(audit["executed"]) + " cached=" + repr(audit["cached"]))

    def same_size_backdated(path):
        before = path.stat()
        path.write_bytes(b"modified-content\n")
        os.utime(path, ns=(before.st_atime_ns, before.st_mtime_ns - 10_000_000_000))
        after = path.stat()
        check(path.parent.name + " same-size backdated fixture",
              before.st_size == after.st_size and after.st_mtime_ns < before.st_mtime_ns)

    check("src tree production branch is active",
          Path(discovery[0]["command"][0]).is_file() and
          not Path(discovery[1]["command"][0]).is_file() and
          not (source / "sample.bin").exists() and not (source / "dsp" / "filter.h").exists())
    keys, audit, saved = invoke()
    check("cache schema rejects version 1", saved["schema"] == 2)
    verify_decision("first run", audit, all_names)
    unchanged, audit, _ = invoke()
    check("unchanged inputs stable", unchanged == keys)
    verify_decision("unchanged inputs", audit, [])

    same_size_backdated(fixture)
    changed, audit, _ = invoke()
    check("fixture content changes every dependent key", all(changed[n] != keys[n] for n in all_names))
    verify_decision("fixture content", audit, all_names)
    keys = changed
    extra = fixture.with_name("added.bin")
    extra.write_bytes(fixture.read_bytes())
    changed, audit, _ = invoke()
    check("fixture addition changes keys", all(changed[n] != keys[n] for n in all_names))
    verify_decision("fixture addition", audit, all_names)
    extra.unlink()
    restored, audit, _ = invoke()
    check("fixture deletion restores exact keys", restored == keys)
    verify_decision("fixture deletion", audit, all_names)
    renamed = fixture.with_name("renamed.bin")
    fixture.rename(renamed)
    changed, audit, _ = invoke()
    check("fixture rename changes keys", all(changed[n] != keys[n] for n in all_names))
    verify_decision("fixture rename", audit, all_names)

    keys = changed
    same_size_backdated(header)
    changed, audit, _ = invoke()
    check("src content changes production runtime key", changed[runtime_name] != keys[runtime_name])
    check("src content leaves existing-command key stable", changed[binary_name] == keys[binary_name])
    verify_decision("src content", audit, [runtime_name])
    keys = changed
    extra = header.with_name("added.h")
    extra.write_bytes(header.read_bytes())
    changed, audit, _ = invoke()
    check("src addition changes runtime key only",
          changed[runtime_name] != keys[runtime_name] and changed[binary_name] == keys[binary_name])
    verify_decision("src addition", audit, [runtime_name])
    extra.unlink()
    restored, audit, _ = invoke()
    check("src deletion restores exact keys", restored == keys)
    verify_decision("src deletion", audit, [runtime_name])
    header.rename(header.with_name("renamed.h"))
    changed, audit, _ = invoke()
    check("src rename changes runtime key only",
          changed[runtime_name] != keys[runtime_name] and changed[binary_name] == keys[binary_name])
    verify_decision("src rename", audit, [runtime_name])
    stable, audit, saved = invoke()
    check("changed inputs settle to stable keys", stable == changed)
    verify_decision("changed inputs unchanged", audit, [])
    # Keep the *current* keys but label the record schema 1. This isolates the
    # migration guard from the separate content-key fix: it must still rerun.
    saved["schema"] = 1
    cache_path.write_text(json.dumps(saved), encoding="utf-8")
    migrated, audit, saved = invoke()
    check("old-schema current keys are not trusted", migrated == stable and saved["schema"] == 2)
    verify_decision("old-schema cache", audit, all_names)
    print("manifest controls", passed, "/", total, "actual test executions=0", "cache-path runs=" + str(sequence))
    sys.exit(0 if passed == total else 1)
