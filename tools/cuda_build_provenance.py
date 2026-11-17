"""CUDA build content freshness; independent artifact and real hardware gates remain required.

2026-11-16（与 HIP 同一处修正）：旧实现把 **src/ 下所有 .cpp/.h** 都当 rtagpu_cuda.dll 的输入，
改一行 UI 文案/版本号就判 stale，逼着每次出包重编 19 个架构的 CUDA 二进制。
现在按**真实依赖**判定：build_gpu_cuda.ps1 里参与编译的 translation units + 它们的
#include 闭包 + 构建脚本 + 工具链（nvcc/CUDA 目录/MSVC host 仍全量哈希）。
旧记录（schema=1，broad）仍可验证：只要窄集合里每一项在旧记录里存在且一致。
"""
import argparse
import hashlib
import json
import re
import sys
from pathlib import Path

SCHEMA = 2
DEFAULT_TUS = ("src/gpu/gpu_conv_cuda.cu", "src/gpu/gpu_resample_cuda.cu")
CODE_SUFFIXES = {".cpp", ".h", ".hpp", ".cu", ".cuh", ".inl", ".c", ".cc"}


def digest(p):
    with p.open("rb") as f:
        return hashlib.file_digest(f, "sha256").hexdigest()


def _rel(source: Path, p: Path) -> str:
    return str(p.relative_to(source)).replace("\\", "/")


def translation_units(source: Path):
    script = source / "tools/build_gpu_cuda.ps1"
    found = []
    if script.is_file():
        text = script.read_text(encoding="utf-8", errors="replace")
        for m in re.finditer(r"src\\gpu\\[\w.\-]+\.(?:cu|cpp)", text):
            found.append("src/gpu/" + re.split(r"\\", m.group(0))[-1])
    tus = sorted(set(found)) or list(DEFAULT_TUS)
    missing = [t for t in tus if not (source / t).is_file()]
    if missing:
        raise ValueError("CUDA translation unit missing: " + str(missing))
    return tus


def include_closure(source: Path, tus):
    seen = set()
    queue = [source / t for t in tus]
    include_re = re.compile(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]', re.M)
    while queue:
        current = queue.pop()
        key = _rel(source, current)
        if key in seen:
            continue
        seen.add(key)
        try:
            text = current.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        for name in include_re.findall(text):
            for cand in (current.parent / name, source / "src" / name, source / name):
                try:
                    resolved = cand.resolve()
                    resolved.relative_to(source.resolve())
                except (OSError, ValueError):
                    continue
                if resolved.is_file() and resolved.suffix.lower() in CODE_SUFFIXES:
                    if _rel(source, resolved) not in seen:
                        queue.append(resolved)
                    break
    return sorted(seen)


def inputs(source, compiler, host):
    tus = translation_units(source)
    paths = [source / t for t in tus] + [source / p for p in include_closure(source, tus)]
    # 只把**构建脚本**算进来（它决定 archs/flags）。门禁脚本本身不是 DLL 的输入 ——
    # 把它算进来会让"改一句判据注释"也判 stale，正是本轮要消除的假失效。
    script = source / "tools" / "build_gpu_cuda.ps1"
    if script.is_file():
        paths.append(script)
    result = {}
    for p in sorted(set(paths)):
        result[_rel(source, p)] = digest(p)
    result["gate"] = "cuda_build_provenance/v%d" % SCHEMA
    kit = compiler.parent.parent
    # 工具链照旧全量哈希（nvcc 只是前端编排器，NVVM/codegen/头文件/静态运行库都要算）
    for base in [kit / "bin", kit / "nvvm", kit / "include", kit / "lib/x64"]:
        if not base.is_dir():
            raise ValueError("CUDA toolchain directory missing: " + str(base))
        for p in sorted(base.rglob("*")):
            if p.is_file():
                result["cuda:" + p.relative_to(kit).as_posix()] = digest(p)
    for name in ["nvcc.exe", "cuobjdump.exe"]:
        if not (compiler.parent / name).is_file():
            raise ValueError("required CUDA tool missing: " + name)
    if not host.is_file():
        raise ValueError("host compiler missing")
    for p in sorted(host.parent.iterdir()):
        if p.is_file() and p.suffix.lower() in {".exe", ".dll"}:
            result["host:" + p.name] = digest(p)
    if not (host.parent / "dumpbin.exe").is_file():
        raise ValueError("host dumpbin missing")
    return result


def main():
    p = argparse.ArgumentParser()
    p.add_argument("mode", choices=["snapshot", "seal", "verify"])
    p.add_argument("--source", type=Path, required=True)
    p.add_argument("--compiler", type=Path, required=True)
    p.add_argument("--host-compiler", type=Path, required=True)
    p.add_argument("--snapshot", type=Path)
    p.add_argument("--record", type=Path)
    p.add_argument("--dll", type=Path)
    a = p.parse_args()
    current = inputs(a.source.resolve(), a.compiler, a.host_compiler)
    if a.mode == "snapshot":
        if not a.snapshot:
            raise ValueError("snapshot required")
        a.snapshot.write_text(json.dumps(dict(schema=SCHEMA, inputs=current), indent=2), encoding="utf-8")
    else:
        prior = a.snapshot if a.mode == "seal" else a.record
        if not prior or not prior.is_file():
            raise ValueError("missing successful CUDA build provenance; rebuild")
        old = json.loads(prior.read_text(encoding="utf-8"))
        recorded = old.get("inputs", {})
        if old.get("schema") == SCHEMA:
            if recorded != current:
                changed = [k for k in set(recorded) | set(current) if recorded.get(k) != current.get(k)]
                raise ValueError("stale CUDA source/toolchain: " + str(sorted(changed)[:12]))
        else:
            unknown = [k for k in current if k != "gate" and k not in recorded]
            changed = [k for k in current if k != "gate" and k in recorded and recorded[k] != current[k]]
            if unknown or changed:
                raise ValueError("stale CUDA source/toolchain: " + str(sorted(unknown + changed)[:12]))
            print("CUDA provenance: legacy broad record; %d GPU-unrelated entries ignored (real inputs %d verified)"
                  % (len(set(recorded) - set(current)), len(current)))
        if not a.dll or not a.record:
            raise ValueError("DLL and record required")
        value = digest(a.dll)
        if a.mode == "verify" and old.get("dll_sha256") != value:
            raise ValueError("CUDA DLL content differs from sealed build")
        if a.mode == "seal":
            a.record.write_text(json.dumps(dict(schema=SCHEMA, backend="cuda", inputs=current, dll_sha256=value,
                                                scope="content freshness of the real CUDA inputs only; cubin/exports and real NVIDIA hardware execution separate"),
                                             indent=2), encoding="utf-8")
    print("CUDA provenance PASS", a.mode, "inputs=" + str(len(current)))
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as e:
        print("CUDA provenance FAIL:", e, file=sys.stderr)
        sys.exit(1)
