"""Bind a successful HIP build to content hashes; mtimes are never evidence.

2026-11-16（用户：小改动出包要 1 小时，必须简化；但门禁不许放水）：
  旧实现把 **src/ 下所有 .cpp/.h** 都当成 rtagpu.dll 的输入，于是改一行 UI 文案
  （ui_script.h）或版本号（version.h）都会判 "HIP provenance FAIL: Stale ..."，
  逼着每次出包重编 25 个架构的 GPU 二进制 —— 而它们根本进不了这个 DLL。

  现在按**真实依赖**判定：从 build_gpu.ps1 里解析出实际参与编译的 translation units，
  再递归展开它们的 #include 闭包（项目内头文件），加上 build_gpu.ps1 与编译器本身。
  判据反而更精确：改到闭包里的头文件一定失效；改 UI/元数据/版本号不再误伤。

  旧记录（schema=1，broad）仍然可验证：只要**窄集合里每一项**在旧记录里存在且哈希一致，
  就证明这个 DLL 的真实输入没变（这比"所有 src 都没变"更贴近事实，而不是更松）。
"""
import argparse
import hashlib
import json
import re
from pathlib import Path
import sys

SCHEMA = 2
DEFAULT_TUS = ("src/gpu/gpu_conv_hip.cpp", "src/gpu/gpu_resample_hip.cpp")
CODE_SUFFIXES = {".cpp", ".h", ".hpp", ".cuh", ".inl", ".c", ".cc"}


def digest(p):
    with p.open("rb") as f:
        return hashlib.file_digest(f, "sha256").hexdigest()


def _rel(source: Path, p: Path) -> str:
    return str(p.relative_to(source)).replace("\\", "/")


def translation_units(source: Path):
    """build_gpu.ps1 里 $src = @(...) 列出的就是真正编译的文件。"""
    script = source / "tools/build_gpu.ps1"
    found = []
    if script.is_file():
        text = script.read_text(encoding="utf-8", errors="replace")
        for m in re.finditer(r"src\\gpu\\[\w.\-]+\.cpp", text):
            found.append("src/gpu/" + m.group(0).split("\\")[-1])
    tus = sorted(set(found)) or list(DEFAULT_TUS)
    missing = [t for t in tus if not (source / t).is_file()]
    if missing:
        raise ValueError("GPU translation unit missing: " + str(missing))
    return tus


def include_closure(source: Path, tus):
    """递归展开项目内 #include（相对 includer 目录 + src/）；系统头不算（编译器哈希已覆盖）。"""
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
            candidates = [current.parent / name, source / "src" / name, source / name]
            for cand in candidates:
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


def inputs(source: Path, compiler: Path):
    tus = translation_units(source)
    paths = [source / t for t in tus] + [source / p for p in include_closure(source, tus)]
    script = source / "tools/build_gpu.ps1"
    if script.is_file():
        paths.append(script)
    values = {}
    for p in sorted(set(paths)):
        values[_rel(source, p)] = digest(p)
    values["gate"] = "hip_build_provenance/v%d" % SCHEMA
    values["compiler:hipcc"] = digest(compiler)
    clang = compiler.parent / "clang++.exe"
    if not clang.is_file():
        raise ValueError("HIP clang compiler missing")
    values["compiler:clang++"] = digest(clang)
    return values


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("mode", choices=["snapshot", "seal", "verify"])
    p.add_argument("--source", required=True, type=Path)
    p.add_argument("--compiler", required=True, type=Path)
    p.add_argument("--dll", type=Path)
    p.add_argument("--snapshot", type=Path)
    p.add_argument("--record", type=Path)
    a = p.parse_args()
    current = inputs(a.source.resolve(), a.compiler)
    if a.mode == "snapshot":
        if not a.snapshot:
            raise ValueError("snapshot path required")
        a.snapshot.write_text(json.dumps(dict(schema=SCHEMA, inputs=current), indent=2), encoding="utf-8")
    else:
        if not a.dll or not a.record:
            raise ValueError("dll and record paths required")
        prior = a.snapshot if a.mode == "seal" else a.record
        if not prior or not prior.is_file():
            raise ValueError("Missing build provenance; rebuild HIP, do not accept timestamp-only DLL")
        record = json.loads(prior.read_text(encoding="utf-8"))
        recorded = record.get("inputs", {})
        if record.get("schema") == SCHEMA:
            if recorded != current:
                changed = [k for k in set(recorded) | set(current) if recorded.get(k) != current.get(k)]
                raise ValueError("Stale HIP source/toolchain content: " + str(sorted(changed)[:12]))
        else:
            # 旧记录（把整个 src/ 都算进去）。判据：窄集合里每一项都必须在旧记录里存在且一致。
            # 少一项 = 无法证明这个 DLL 与当前源码一致 ⟹ 必须重编（宁可严，不可猜）。
            # "gate" 是 v2 才有的标记键，旧记录里没有 —— 不参与"未知输入"判定。
            unknown = [k for k in current if k != "gate" and k not in recorded]
            changed = [k for k in current if k != "gate" and k in recorded and recorded[k] != current[k]]
            if unknown or changed:
                raise ValueError("Stale HIP source/toolchain content: " + str(sorted(unknown + changed)[:12]))
            ignored = len(set(recorded) - set(current))
            # 信息行必须走 stdout：make_dist 在 ErrorActionPreference=Stop 下把原生命令的
            # stderr 当成错误记录，会让整条出包命令以 exit 1 收尾（本轮实测踩到）。
            print("HIP provenance: legacy broad record; %d GPU-unrelated entries ignored (real inputs %d verified)"
                  % (ignored, len(current)))
        dll_hash = digest(a.dll)
        if a.mode == "verify" and record.get("dll_sha256") != dll_hash:
            raise ValueError("DLL hash differs from successful build record")
        if a.mode == "seal":
            a.record.write_text(json.dumps(dict(schema=SCHEMA, inputs=current, dll_sha256=dll_hash,
                                                scope="Content freshness of the real HIP inputs only; code-object gate and hardware smoke remain mandatory"),
                                             indent=2), encoding="utf-8")
    print("HIP provenance PASS", a.mode, "inputs=" + str(len(current)))
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as e:
        print("HIP provenance FAIL:", e, file=sys.stderr)
        sys.exit(1)
