"""Offline paired production DSP harness. Default: build + selftest only.
Use --benchmark -- <shared benchmark flags> to run an explicitly selected case.
No device API or network; performance does not prove spectral quality.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys

FLAGS = ["-std=c++23", "-O3", "-DNDEBUG", "-march=x86-64-v2",
         "-ffast-math", "-fno-finite-math-only", "-fhonor-nans", "-fhonor-infinities",
         "-fno-math-errno", "-fno-strict-aliasing", "-ffp-contract=off",
         "-Xclang", "-flto-visibility-public-std", "-D_MT", "-Xclang",
         "--dependent-lib=libcmt", "-Wno-deprecated-declarations"]


def sha(path):
    with Path(path).open("rb") as f:
        return hashlib.file_digest(f, "sha256").hexdigest()


# Compare the tested signal/configuration and actual numerical route. Execution
# details such as overlap, SIMD dispatch or compact ABI masks may intentionally
# differ between implementations; their outputs must still match in full.
MATCH_FIELDS = (
    "route", "source_rate", "target_rate", "channels", "source_seconds", "block_frames",
    "input_wav", "input_wav_decode_in_dsp_timing", "preset_requested", "preset_applies",
    "family", "requested_order", "effective_order", "effective_levels", "step_down_reason",
    "sdm_osr_20k", "sdm_dither_lsb", "sdm_cutoff_hz", "sdm_oob_gain_db", "sdm_ntf_hinf",
    "decode_mode", "decode_rate", "headroom_db", "topology_requested", "cascade_active",
    "gpu_requested", "actual_gpu_any", "actual_gpu_all_blocks_all_stages",
    "actual_fixed_any", "actual_fixed_all", "T1", "T2", "logical_coeff_bytes",
    "input_frames", "input_bits", "raw_pipeline_frames", "drain_requested", "drained",
    "head_trimmed", "expected_output_frames", "output_frames", "output_native_bits",
    "native_independent_min_bits", "native_independent_support_max_bits",
    "marker_errors", "finite_errors", "integer_24bit_range_errors", "sdm_nonfinite",
    "sdm_overload", "sdm_clamped", "count_ok", "structural_ok", "full_output_checksum",
    "hash_encoding", "native_whole_target_retained", "spectral_quality_test",
)


def compare_results(baseline, current):
    mismatches = {}
    for key in MATCH_FIELDS:
        if key not in baseline or key not in current or baseline[key] != current[key]:
            mismatches[key] = {"baseline_present": key in baseline,
                               "current_present": key in current,
                               "baseline": baseline.get(key), "current": current.get(key)}
    errors = []
    for name, data in (("baseline", baseline), ("current", current)):
        for key in ("structural_ok", "count_ok", "full_output_checksum"):
            if data.get(key) is not True:
                errors.append(f"{name}: {key} must be true")
    def valid_hash(value):
        return isinstance(value, str) and len(value) == 64 and all(ch in "0123456789abcdefABCDEF" for ch in value)
    b_hash, c_hash = baseline.get("full_output_sha256"), current.get("full_output_sha256")
    equal = valid_hash(b_hash) and valid_hash(c_hash) and b_hash.lower() == c_hash.lower()
    if not equal:
        errors.append("Full output SHA256 differs or is missing/invalid")
    if mismatches:
        errors.append("Signal/configuration or actual GPU/order/count metadata differs")
    return {"full_output_sha256_equal": equal, "metadata_equal": not mismatches,
            "metadata_mismatches": mismatches, "comparison_errors": errors}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline-source", type=Path, default=Path(r"D:\RLPlayer-DSP-3.0.6-20261031\source"))
    parser.add_argument("--current-source", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--output", type=Path)
    parser.add_argument("--compiler", type=Path, default=Path(r"C:\Program Files\AMD\ROCm\7.2\bin\clang++.exe"))
    parser.add_argument("--gpu-dir", type=Path, default=Path(r"D:\RLPlayer-DSP-3.0.6-20261031\evidence\hqp-benchmark-20261101"),
                        help="Shared fallback GPU directory; per-build options override this")
    parser.add_argument("--baseline-gpu-dir", type=Path)
    parser.add_argument("--current-gpu-dir", type=Path)
    parser.add_argument("--baseline-overlap", type=int, choices=(0, 1), default=0)
    parser.add_argument("--current-overlap", type=int, choices=(0, 1), default=0)
    parser.add_argument("--skip-build", action="store_true")
    parser.add_argument("--benchmark", action="store_true")
    parser.add_argument("case", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    root = (args.output or args.current_source.parent / "evidence" / "acceptance-tools").resolve()
    root.mkdir(parents=True, exist_ok=True)
    receipt_path = root / "receipt.json"
    tool = args.current_source / "tools" / "dsp_efficiency_benchmark.cpp"
    receipt = {"schema": 2, "status": "RUNNING", "flags": FLAGS,
               "compiler": str(args.compiler), "spectral_quality_proven": False,
               "note": "Speed and matching SHA256 establish results only for the exact tested input/configuration, not spectral quality. Primary elapsed time is end-to-end harness wall including validation and every hash update; final fixed-size digest duplication/finalization is outside harness timing. Never subtract validation from overlapped runs.",
               "builds": {}}
    def save():
        receipt_path.write_text(json.dumps(receipt, indent=2), encoding="utf-8")
    try:
        save()
        receipt["tool_sha256"] = sha(tool)
        case = args.case[1:] if args.case[:1] == ["--"] else args.case
        if any(flag == "--overlap" or flag.startswith("--overlap=") for flag in case):
            raise RuntimeError("Use --baseline-overlap/--current-overlap; shared case --overlap is ambiguous")
        if args.benchmark and not case:
            raise RuntimeError("--benchmark requires explicit benchmark flags after --")
        for name, source, gpu_dir, overlap in (
            ("baseline", args.baseline_source, args.baseline_gpu_dir or args.gpu_dir, args.baseline_overlap),
            ("current", args.current_source, args.current_gpu_dir or args.gpu_dir, args.current_overlap),
        ):
            dest = root / name
            dest.mkdir(exist_ok=True)
            exe = dest / "dsp_efficiency_benchmark.exe"
            entry = {"headers_source": str(source.resolve()), "gpu_source": str(gpu_dir.resolve()),
                     "overlap_requested": overlap, "gpu_sha256": {}, "gpu_files_source": {}}
            receipt["builds"][name] = entry
            for dll in ("rtagpu.dll", "amdhip64_7.dll"):
                origin = gpu_dir / dll
                # A candidate build directory may contain only the changed GPU
                # backend. Reuse the common runtime unless explicitly supplied.
                if dll == "amdhip64_7.dll" and not origin.is_file():
                    origin = args.gpu_dir / dll
                if not origin.is_file():
                    raise RuntimeError(f"Required {name} GPU runtime absent: {origin}")
                if origin.resolve() != (dest / dll).resolve():
                    shutil.copy2(origin, dest / dll)
                entry["gpu_sha256"][dll] = sha(dest / dll)
                entry["gpu_files_source"][dll] = str(origin.resolve())
            command = [str(args.compiler), *FLAGS, "-I" + str(source / "src"), str(tool), "-o", str(exe), "-lpsapi", "-lbcrypt"]
            entry["command"] = command
            if not args.skip_build:
                build = subprocess.run(command, capture_output=True, text=True, errors="replace")
                (dest / "build.log").write_text(build.stdout + build.stderr, encoding="utf-8")
                entry["build_exit"] = build.returncode
                if build.returncode:
                    raise RuntimeError(f"{name} build failed; see {dest / 'build.log'}")
            entry["exe_sha256"] = sha(exe)
            test = subprocess.run([str(exe), "--selftest"], cwd=dest, capture_output=True, text=True, errors="replace")
            (dest / "selftest.log").write_text(test.stdout + test.stderr, encoding="utf-8")
            entry["selftest_exit"] = test.returncode
            entry["selftest"] = test.stdout.strip()
            if test.returncode:
                raise RuntimeError(f"{name} selftest failed; see {dest / 'selftest.log'}")
            if args.benchmark:
                run_flags = [*case, "--overlap", str(overlap)]
                entry["run_command"] = [str(exe), *run_flags]
                run = subprocess.run(entry["run_command"], cwd=dest, capture_output=True, text=True, errors="replace")
                (dest / "case.stdout.json").write_text(run.stdout, encoding="utf-8")
                (dest / "case.stderr.log").write_text(run.stderr, encoding="utf-8")
                entry["run_exit"] = run.returncode
                try:
                    entry["result"] = json.loads(run.stdout)
                except (json.JSONDecodeError, TypeError) as ex:
                    raise RuntimeError(f"{name} benchmark did not return valid JSON; see {dest / 'case.stdout.json'}") from ex
                if run.returncode:
                    raise RuntimeError(f"{name} benchmark structural checks failed")
                if entry["result"].get("overlap") != overlap:
                    raise RuntimeError(f"{name} benchmark overlap metadata does not match requested mode")
            save()
        receipt["same_gpu_dlls"] = receipt["builds"]["baseline"]["gpu_sha256"] == receipt["builds"]["current"]["gpu_sha256"]
        if args.benchmark:
            b, c = (receipt["builds"][name]["result"] for name in ("baseline", "current"))
            receipt.update(compare_results(b, c))
            receipt["actual_cpu_gpu_order_output_counts"] = {
                name: {key: data["result"].get(key) for key in (
                    "overlap", "gpu_requested", "actual_gpu_any", "actual_gpu_all_blocks_all_stages", "actual_fixed_any", "actual_fixed_all",
                    "requested_order", "effective_order", "output_frames", "output_native_bits", "count_ok", "structural_ok")}
                for name, data in receipt["builds"].items()}
            if receipt["comparison_errors"]:
                raise RuntimeError("; ".join(receipt["comparison_errors"]))
        receipt["status"] = "PASS"
        save()
        print(json.dumps({"receipt": str(receipt_path), "status": "PASS", "same_gpu_dlls": receipt["same_gpu_dlls"],
                          "selftests": "both PASS", "benchmark_run": args.benchmark}))
        return 0
    except Exception as ex:
        receipt["status"] = "FAIL"
        receipt["error"] = str(ex)
        save()
        print(json.dumps({"receipt": str(receipt_path), "status": "FAIL", "error": str(ex)}), file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
