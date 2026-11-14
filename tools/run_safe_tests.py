"""Run reviewed CTest entries in per-test disposable install/cwd/profile directories.
No test is discovered-and-run implicitly: every registered name must occur once
in safe_test_manifest.json. Unknown/missing names fail before any execution.
"""
import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import uuid
from pathlib import Path

sys.stdout.reconfigure(encoding='utf-8', errors='replace')


def bracket(value):
    value = str(value)
    level = "="
    while "]" + level + "]" in value:
        level += "="
    return "[" + level + "[" + value + "]" + level + "]"


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--build", type=Path, required=True)
    p.add_argument("--source", type=Path, required=True)
    p.add_argument("--run-root", type=Path, required=True)
    p.add_argument("--manifest", type=Path, default=Path(__file__).with_name("safe_test_manifest.json"))
    p.add_argument("--ffmpeg-dir", type=Path)
    p.add_argument("--lite-dir", type=Path)
    p.add_argument("--group", action="append")
    p.add_argument("--include", default=".*")
    p.add_argument("--prepare-only", action="store_true")
    p.add_argument("--audit-only", action="store_true")
    a = p.parse_args()
    # CTest changes directory before evaluating each test's WORKING_DIRECTORY.
    # Anchor CLI paths here so a relative private run root does not become nested
    # beneath itself, and compiler/source arguments keep their original meaning.
    for key in ('build', 'source', 'run_root', 'manifest', 'ffmpeg_dir', 'lite_dir'):
        value = getattr(a, key)
        if value is not None:
            setattr(a, key, value.resolve())
    manifest = json.loads(a.manifest.read_text(encoding="utf-8-sig"))
    entries = manifest["tests"]
    allowed_categories = {"offline", "loopback", "mock-gpu", "numeric-quality", "hardware"}
    if manifest.get("reviewed_registration_count") != len(entries):
        raise ValueError("Manifest registration count is stale")
    for row in entries:
        if row.get("category") not in allowed_categories:
            raise ValueError("Invalid test classification: " + row["name"])
        missing = [f for f in row.get("sources", []) if not (a.source / f).is_file()]
        if missing:
            raise ValueError("Missing audited source: " + row["name"] + str(missing))
    names = [row["name"] for row in entries]
    if len(names) != len(set(names)):
        raise ValueError("Duplicate manifest entries")
    discovery = subprocess.run(["ctest", "--test-dir", str(a.build), "--show-only=json-v1"],
                               capture_output=True, text=True, encoding="utf-8", errors="replace", check=True)
    discovered = json.loads(discovery.stdout)["tests"]
    found = {row["name"] for row in discovered}
    if found != set(names):
        raise ValueError(f"Manifest mismatch: unclassified={sorted(found-set(names))}, unregistered={sorted(set(names)-found)}")
    if a.audit_only:
        print(f"MANIFEST PASS classified={len(found)} registered={len(found)}")
        return 0
    audit_dependencies = ""
    cache = a.build / "CMakeCache.txt"
    if cache.is_file():
        match = re.search(r"^RTA_CERTIFICATE_PYTHON_PATH:[^=]+=(.+)$", cache.read_text(encoding="utf-8", errors="replace"), re.MULTILINE)
        if match:
            audit_dependencies = match[1].strip()
            if audit_dependencies and not Path(audit_dependencies).is_dir():
                raise ValueError("Configured audit dependencies are missing: " + audit_dependencies)
    by_name = {row["name"]: row for row in entries}
    selected = []
    rejected = []
    for test in discovered:
        rule = by_name[test["name"]]
        if not rule.get("reason") or not rule.get("sources"):
            raise ValueError("Missing call-path audit evidence: " + rule["name"])
        if not re.fullmatch(a.include, test["name"]):
            continue
        if rule["category"] not in (a.group or ["offline", "loopback", "mock-gpu", "numeric-quality"]):
            rejected.append(dict(name=test["name"], reason=rule["category"]))
            continue
        guards = rule.get("required_source_guards", [])
        source_text = "\n".join((a.source / f).read_text(encoding="utf-8-sig") for f in rule["sources"] if (a.source / f).is_file())
        if any(guard not in source_text for guard in guards):
            rejected.append(dict(name=test["name"], reason="immutable baseline predates required opt-in guard"))
            continue
        selected.append(test)
    if not selected:
        raise ValueError("No reviewed executable tests selected")
    # Fresh directory only, never cleanup or overwrite an earlier run.
    a.run_root.mkdir(parents=True, exist_ok=False)
    shared_tools = a.run_root / "_tools"
    shared_tools.mkdir()
    if a.ffmpeg_dir:
        for tool in ("ffmpeg.exe", "ffprobe.exe"):
            shutil.copy2((a.ffmpeg_dir / tool).resolve(), shared_tools / tool)
    if a.lite_dir:
        lite = shared_tools / "ffmpeg-lite"
        lite.mkdir()
        for tool in ("ffmpeg.exe", "ffprobe.exe"):
            shutil.copy2((a.lite_dir / tool).resolve(), lite / tool)
    lines = []
    staged = []
    for test in selected:
        name = test["name"]
        rule = by_name[name]
        private = a.run_root / name
        private.mkdir()
        temp = private / "temp"
        temp.mkdir()
        profile = private / "profile"
        profile.mkdir()
        if not test.get("command"):
            raise FileNotFoundError("Registered executable has not been built: " + name)
        cmd = list(test["command"])
        copies = {}
        def stage_exe(path):
            src = Path(path)
            dst = private / src.name
            if not src.is_file():
                raise FileNotFoundError("Required executable missing: " + str(src))
            if src.name not in copies:
                shutil.copy2(src, dst)
                copies[src.name] = str(src)
            return str(dst)
        if Path(cmd[0]).suffix.lower() == ".exe" and Path(cmd[0]).stem.lower() not in ("python", "python3", "node", "powershell", "pwsh"):
            cmd[0] = stage_exe(cmd[0])
        for i in range(1, len(cmd)):
            if Path(cmd[i]).suffix.lower() == ".exe" and str(Path(cmd[i])).lower().startswith(str(a.build).lower()):
                cmd[i] = stage_exe(cmd[i])
            elif str(cmd[i]).lower().startswith(str(a.build).lower()) and Path(cmd[i]).suffix in (".tsv", ".json", ".csv"):
                cmd[i] = str(private / Path(cmd[i]).name)
        for i in range(1, len(cmd)):
            if cmd[i-1] in ("--evidence", "--evidence-dir"):
                cmd[i] = str(private / "evidence")
        for helper in rule.get("helpers", []):
            stage_exe(a.build / helper)
        if name == "wav_japanese_metadata":
            if not a.lite_dir:
                raise ValueError("Japanese WAV regression requires the packaged ffprobe")
            os.link(shared_tools / "ffmpeg-lite" / "ffprobe.exe", private / "ffprobe.exe")
            copies["ffprobe.exe"] = str((a.lite_dir / "ffprobe.exe").resolve())
        elif a.ffmpeg_dir:
            for tool in ("ffmpeg.exe", "ffprobe.exe"):
                os.link(shared_tools / tool, private / tool)
                copies[tool] = str((a.ffmpeg_dir / tool).resolve())
        if a.lite_dir and name in ("ffmpeg_matrix", "app_writeback_cover_real", "track_cover_resolution", "cover_local_resolution", "cover_source_controller", "iso_end_to_end", "iso_upnp"):
            lite = private / "ffmpeg-lite"
            lite.mkdir()
            for tool in ("ffmpeg.exe", "ffprobe.exe"):
                os.link(shared_tools / "ffmpeg-lite" / tool, lite / tool)
                copies["ffmpeg-lite/" + tool] = str((a.lite_dir / tool).resolve())
        if rule["category"] == "mock-gpu":
            # Exact original fixture directory; never copy a production backend.
            original = Path(test["command"][0]).parent
            if not any(part.startswith(("gpu-observation", "gpu-compact")) for part in original.parts):
                raise ValueError("Unrecognized fake-backend directory: " + str(original))
            dll = original / "rtagpu.dll"
            if not dll.is_file():
                raise FileNotFoundError("Required fake DLL missing: " + str(dll))
            shutil.copy2(dll, private / dll.name)
        env = {
            "RTA_NO_DEVICE": "1", "RTA_NET_TESTS": "0", "RTA_REAL_LIBRARY_TESTS": "0",
            "RLPLAYER_TEST_SOURCE_ROOT": str(a.source),
            # Tests launch Windows PowerShell 5.1. An inherited PowerShell 7
            # module path can select its incompatible Utility module and lose
            # Get-FileHash. Use only the matching, system-owned 5.1 modules.
            "PSModulePath": str(Path(os.environ["SystemRoot"]) / "System32/WindowsPowerShell/v1.0/Modules"),
            "PYTHONPATH": audit_dependencies, "RTA_TEST_LIBRARY_DIR": "", "RTA_TEST_DSF_FILE": "", "PYTHONOPTIMIZE": "",
            "TEMP": str(temp), "TMP": str(temp), "APPDATA": str(profile), "LOCALAPPDATA": str(profile),
            "RLPLAYER_LIBRARY_DIR": str(profile), "RLPLAYER_UPDATE_DIR": str(profile / "update"),
            "RLPLAYER_TEST_FLAC": "", "RTA_DURATION_FLAC": "", "RL_TEST_TAGS_SCALE": "1000",
            "PATH": str(private) + ";" + str(Path(os.environ["SystemRoot"]) / "System32") + ";" + str(Path(os.environ["SystemRoot"]) / "System32" / "WindowsPowerShell" / "v1.0"),
        }
        if name == "app_playback_error_contract":
            node = shutil.which("node")
            if not node:
                raise FileNotFoundError("Node is required by the actual planner-to-UI contract")
            shutil.copy2(node, private / "node.exe")
        if name == "rlplayer_publish":
            openssl = shutil.which("openssl")
            if not openssl:
                raise FileNotFoundError("OpenSSL is required by the real PS256 publisher gate")
            openssl = Path(openssl).resolve()
            env["PATH"] = str(openssl.parent) + ";" + env["PATH"]
            copies["external-openssl.exe"] = str(openssl)
        if name in ("release_gate_regression", "safe_manifest_controls"):
            ctest_exe = Path(shutil.which("ctest"))
            # Python CMake wheels expose a launcher; use its actual standalone CTest.
            wheel_ctest = Path(sys.prefix) / "Lib/site-packages/cmake/data/bin/ctest.exe"
            if wheel_ctest.is_file():
                ctest_exe = wheel_ctest
            shutil.copy2(ctest_exe, private / "ctest.exe")
        if name == "release_gate_regression":
            # Keep the isolated PATH. CMake locates share/modules relative to its
            # real install, so neither its Python launcher nor a bare EXE copy is suitable.
            cmake_found = shutil.which("cmake")
            if not cmake_found:
                raise FileNotFoundError("Native argv regression requires CMake")
            cmake_exe = Path(cmake_found).resolve()
            wheel_cmake = Path(sys.prefix) / "Lib/site-packages/cmake/data/bin/cmake.exe"
            if wheel_cmake.is_file():
                cmake_exe = wheel_cmake.resolve()
            if not list((cmake_exe.parent.parent / "share").glob("cmake-*/Modules/CMake.cmake")):
                raise FileNotFoundError("CMake must retain its complete share/modules installation: " + str(cmake_exe))
            env["RTA_TEST_CMAKE_EXE"] = str(cmake_exe)
            copies["external-native-cmake.exe"] = str(cmake_exe)
        if name in ("health_robust", "release_gate_regression"):
            # Keep health's deliberate 200-character basename and the nested
            # redistribution fixtures below Windows PowerShell/.NET MAX_PATH.
            temp = Path(a.run_root.resolve().anchor) / ("RLGatesTmp-" + uuid.uuid4().hex[:8])
            temp.mkdir(exist_ok=False)
            env["TEMP"] = env["TMP"] = str(temp)
        if name == "opra_eq":
            data = private / "data"
            data.mkdir()
            shutil.copy2(a.source / "data" / "opra_index.json", data / "opra_index.json")
        if name == "tags_native":
            music = private / "MUSIC"
            music.mkdir()
            codecs = {"flac": "flac", "wav": "pcm_s16le", "mp3": "libmp3lame", "ogg": "libvorbis"}
            for ext, codec in codecs.items():
                sample = music / ("fixture-0." + ext)
                generate = subprocess.run([str(private / "ffmpeg.exe"), "-v", "error", "-f", "lavfi", "-i", "anullsrc=r=44100:cl=stereo", "-t", "0.1", "-c:a", codec, "-metadata", "title=Gate Fixture", "-metadata", "artist=Gate Artist", "-metadata", "album=Gate Album", str(sample)], cwd=private, capture_output=True, timeout=60)
                if generate.returncode or not sample.is_file():
                    raise ValueError("Tag fixture generation failed: " + ext)
                for index in range(1, 5):
                    shutil.copy2(sample, music / (f"fixture-{index}." + ext))
            cmd.append(str(private))
            print("UNVERIFIED_REAL tags_native uses generated media only", flush=True)
        if name == "filter_input_validation":
            env["RTA_VALIDATION_OVERFLOW_CHECK"] = "1"
        if name == "ffmpeg_matrix":
            if not a.lite_dir:
                raise ValueError("ffmpeg_matrix requires explicit --lite-dir; missing tools must not SKIP")
            env["RTA_FFMPEG_LITE_DIR"] = str(private / "ffmpeg-lite")
        if name == "dsd_stream_decode":
            helper = stage_exe(a.build / "make_dsd.exe")
            fixture = private / "generated-dsd64.dsf"
            result = subprocess.run([helper, "--out", str(fixture), "--seconds", "0.05", "--amp", "-60"],
                                    cwd=private, env={**os.environ, **env}, capture_output=True, timeout=60)
            (private / "fixture.log").write_bytes(result.stdout + result.stderr)
            if result.returncode != 0 or not fixture.is_file():
                raise ValueError("DSD fixture generation failed instead of accepting zero assertions")
            cmd.append(str(fixture))
        if name == "media_duration":
            if not a.ffmpeg_dir:
                raise ValueError("media_duration requires ffmpeg fixture generator")
            fixture = private / "generated-duration.flac"
            result = subprocess.run([str(private / "ffmpeg.exe"), "-v", "error", "-f", "lavfi", "-i", "anullsrc=r=44100:cl=stereo", "-t", "171.692290", "-c:a", "flac", "-metadata", "title=Gate Fixture", "-metadata", "artist=Gate Artist", "-metadata", "album=Gate Album", str(fixture)],
                                    cwd=private, env={**os.environ, **env}, capture_output=True, timeout=60)
            if result.returncode != 0 or not fixture.is_file():
                raise ValueError("FLAC fixture generation failed")
            env["RTA_DURATION_FLAC"] = str(fixture)
        lines.append("add_test(" + bracket(name) + " " + " ".join(bracket(x) for x in cmd) + ")")
        props = {prop["name"]: prop["value"] for prop in test.get("properties", [])}
        props.pop("WORKING_DIRECTORY", None)
        prior_env = props.pop("ENVIRONMENT", [])
        for value in prior_env:
            key, _, val = value.partition("=")
            if key not in env:
                env[key] = val
        props["WORKING_DIRECTORY"] = str(private)
        props["ENVIRONMENT"] = [key + "=" + value.replace(";", "\\;") for key, value in env.items()]
        props.setdefault("TIMEOUT", rule.get("timeout_seconds", 600))
        # 2026-11-14（3.3.11 门禁提速）：loopback 类测试会起本地服务/绑端口，必须彼此互斥；
        # 其余（offline / numeric-quality / mock-gpu）互相独立，可以并行。
        # CTest 的 RESOURCE_LOCK 让同锁测试串行、不同锁并行 —— 这样整体 --parallel 才是安全的。
        if rule["category"] == "loopback":
            props.setdefault("RESOURCE_LOCK", "rlp_loopback")
        for key, value in props.items():
            if isinstance(value, list):
                value = ";".join(str(v) for v in value)
            if isinstance(value, bool):
                value = "TRUE" if value else "FALSE"
            lines.append("set_tests_properties(" + bracket(name) + " PROPERTIES " + key + " " + bracket(value) + ")")
        staged.append(dict(name=name, category=rule["category"], command=cmd, private=str(private),
                           environment=env, source_hashes={f: hashlib.sha256((a.source/f).read_bytes()).hexdigest() for f in rule["sources"] if (a.source/f).is_file()}, executable_inputs=copies))
    (a.run_root / "CTestTestfile.cmake").write_text("\n".join(lines) + "\n", encoding="utf-8")
    (a.run_root / "audit.json").write_text(json.dumps(dict(registered=len(found), certificate_python_path=audit_dependencies, selected=staged, excluded=rejected), indent=2), encoding="utf-8")
    print(f"MANIFEST registered={len(found)} selected={len(selected)} excluded={len(rejected)} fresh_root={a.run_root}", flush=True)
    if a.prepare_only:
        return 0
    report = a.run_root / "results.xml"
    gate = Path(__file__).with_name("check_ctest.ps1")
    # 2026-11-14（3.3.11 门禁提速）：默认并行度 = min(4, CPU-2)，RTA_GATE_PARALLEL=1 强制串行。
    # loopback 测试已由 RESOURCE_LOCK 互斥，其余并行；串行回归仍可用 RTA_GATE_PARALLEL=1 复现。
    try:
        parallel = int(os.environ.get("RTA_GATE_PARALLEL", "0"))
    except ValueError:
        parallel = 0
    if parallel <= 0 and os.environ.get("RTA_GATE_PARALLEL") != "1":
        parallel = max(1, min(4, (os.cpu_count() or 4) - 2))
    command = ["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(gate), "-BuildDir", str(a.run_root), "-Report", str(report)]
    if parallel > 1:
        # 2026-11-14（3.3.11）：负载敏感的用例单独串行跑。
        # 判据不是手写名单 —— 手写名单必然漏（第一版就漏了 6 个 UI 测试，它们在并行段同时拉起
        # 无头 Edge，把后面 controls336 / workbench_profiles336 的 CDP 全部拖超时）。
        # 现在按清单自动判定两类：
        #   ① category == loopback（起本地服务/绑端口）
        #   ② sources 里有 .js（这些测试本身就是「起 Edge + 走 CDP 驱动真实页面」）
        # 其余（纯数值/离线）才进并行段。
        sensitive_names = sorted(
            rule["name"] for rule in entries
            if rule["category"] == "loopback" or any(str(f).endswith(".js") for f in rule.get("sources", []))
        )
        sensitive = "^(" + "|".join(re.escape(n) for n in sensitive_names) + ")$"
        command += ["-Parallel", str(parallel), "-SerialTests", sensitive]
        print(f"GATE parallel={parallel}; serial set = {len(sensitive_names)} load/UI-sensitive tests", flush=True)
    # The nested strict CTest gate records actual skips and failures.
    with (a.run_root / "ctest.log").open("w", encoding="utf-8") as log:
        proc = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, encoding="utf-8", errors="replace")
        for line in proc.stdout:
            log.write(line); log.flush(); print(line, end="", flush=True)
        return proc.wait()


if __name__ == "__main__":
    raise SystemExit(main())
