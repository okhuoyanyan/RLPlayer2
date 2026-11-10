"""Offline HIP PE/code-object gate. Never loads a DLL or opens a GPU.

Validate bundle descriptors in PE .hip_fat sections, actual ELF target flags,
non-empty executable kernels and both convolution/resampling per requested arch.
This proves embedded artifacts, not execution on any GPU family.
"""
from __future__ import annotations
import argparse
import hashlib
import json
import re
import struct
import subprocess
import tempfile
from pathlib import Path

MAGIC = b"__CLANG_OFFLOAD_BUNDLE__"
DEFAULT_ARCHS = "gfx803 gfx900 gfx906 gfx908 gfx90a gfx90c gfx942 gfx1010 gfx1011 gfx1012 gfx1030 gfx1031 gfx1032 gfx1033 gfx1034 gfx1035 gfx1036 gfx1100 gfx1101 gfx1102 gfx1103 gfx1150 gfx1151 gfx1200 gfx1201".split()


def require(condition, message):
    if not condition:
        raise ValueError(message)


def pe_sections(data):
    require(len(data) >= 64 and data[:2] == b"MZ", "not a PE image")
    pe = struct.unpack_from("<I", data, 60)[0]
    require(pe + 24 <= len(data) and data[pe:pe + 4] == b"PE\0\0", "invalid PE header")
    machine, count = struct.unpack_from("<HH", data, pe + 4)
    require(machine == 0x8664 and 0 < count < 128, "expected x64 PE with sections")
    opt_size = struct.unpack_from("<H", data, pe + 20)[0]
    table = pe + 24 + opt_size
    require(table + count * 40 <= len(data), "truncated PE section table")
    result = []
    for i in range(count):
        row = table + i * 40
        name = data[row:row + 8].split(b"\0", 1)[0].decode("ascii")
        size, offset = struct.unpack_from("<II", data, row + 16)
        require(offset + size <= len(data), "PE section extends outside file")
        result.append((name, offset, offset + size))
    return result


def bundle_objects(data):
    result = []
    for section, low, high in pe_sections(data):
        if section not in (".hip_fat", ".hip_fatbin"):
            continue
        cursor = low
        while True:
            base = data.find(MAGIC, cursor, high)
            if base < 0:
                break
            cursor = base + len(MAGIC)
            require(cursor + 8 <= high, "truncated bundle count")
            count = struct.unpack_from("<Q", data, cursor)[0]
            cursor += 8
            require(0 < count <= 128, "invalid bundle entry count")
            entries = []
            for _ in range(count):
                require(cursor + 24 <= high, "truncated bundle descriptor")
                descriptor = cursor
                offset, size, length = struct.unpack_from("<QQQ", data, cursor)
                cursor += 24
                require(0 < length <= 512 and cursor + length <= high, "invalid bundle target name")
                target = data[cursor:cursor + length].decode("ascii")
                name_offset = cursor
                cursor += length
                entries.append((target, base + offset, size, descriptor, name_offset))
            payload_end = cursor
            intervals = []
            for target, offset, size, descriptor, name_offset in entries:
                if target.startswith("host-") and size == 0:
                    continue
                require(size >= 64 and offset >= cursor and offset + size <= high, "invalid/truncated bundle payload")
                require(not any(offset < end and offset + size > begin for begin, end in intervals), "overlapping bundle payloads")
                intervals.append((offset, offset + size))
                require(target.startswith("hipv4-amdgcn-amd-amdhsa--"), "unexpected device bundle target")
                match = re.fullmatch(r"hipv4-amdgcn-amd-amdhsa--(gfx[0-9a-f]+)(?::[^\s]+)?", target)
                require(match is not None, "invalid AMD bundle target")
                result.append(dict(arch=match[1], offset=offset, size=size, bundle=base,
                                   descriptor=descriptor, name_offset=name_offset))
                payload_end = max(payload_end, offset + size)
            cursor = payload_end
    require(result, "no real HIP bundles in PE HIP section (strings/overlay are insufficient)")
    return result


def inspect(path, readobj, archs):
    data = Path(path).read_bytes()
    require(archs and len(set(archs)) == len(archs), "expected architecture list must be non-empty and unique")
    entries = bundle_objects(data)
    exports = subprocess.run([str(readobj), "--coff-exports", str(path)], capture_output=True,
                             text=True, encoding="utf-8", errors="replace", timeout=30)
    require(exports.returncode == 0, f"llvm-readobj exports failed: exit={exports.returncode}")
    host_exports = set(re.findall(r"(?m)^  Name: (\S+)\s*$", exports.stdout))
    required_exports = {"rta_gpu_available", "rta_gpu_convolve", "rta_gpu_resample", "rta_gpu_resample_invalidate"}
    require(required_exports <= host_exports, f"missing host exports: {sorted(required_exports - host_exports)}")
    coverage = {arch: set() for arch in archs}
    records = []
    with tempfile.TemporaryDirectory(prefix="rlplayer-hip-artifact-") as temp:
        for index, entry in enumerate(entries):
            code = data[entry["offset"]:entry["offset"] + entry["size"]]
            require(code[:7] == b"\x7fELF\x02\x01\x01" and code[7] == 64, "payload is not ELF64 AMD HSA")
            require(struct.unpack_from("<H", code, 18)[0] == 224, "payload machine is not EM_AMDGPU")
            obj = Path(temp) / f"{index}.co"
            obj.write_bytes(code)
            proc = subprocess.run([str(readobj), "--file-headers", "--symbols", str(obj)],
                                  capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=30)
            require(proc.returncode == 0, f"llvm-readobj failed: exit={proc.returncode}: {proc.stderr}")
            text = proc.stdout
            flag = re.search(r"EF_AMDGPU_MACH_AMDGCN_(GFX[0-9A-F]+)\b", text)
            require(flag and flag[1].lower() == entry["arch"], "bundle target differs from ELF machine flags")
            kernels = set()
            for symbol in re.findall(r"Symbol \{(.*?)\n  \}", text, re.S):
                name = re.search(r"Name: (\S+)", symbol)
                if name and "Type: Function" in symbol and "Section: .text" in symbol:
                    size = re.search(r"Size: (0x[0-9A-Fa-f]+|[0-9]+)", symbol)
                    if size and int(size[1], 0) > 0:
                        kernels.add(name[1])
            require(kernels, "code object contains no executable kernel functions")
            roles = set()
            if any("mac_kernel" in name for name in kernels):
                roles.add("convolution")
            if any("resample_kernel" in name for name in kernels):
                roles.add("resample")
            if entry["arch"] in coverage:
                coverage[entry["arch"]].update(roles)
            records.append(dict(arch=entry["arch"], offset=entry["offset"], size=entry["size"],
                                roles=sorted(roles), kernels=sorted(kernels), sha256=hashlib.sha256(code).hexdigest()))
    missing = {arch: sorted({"convolution", "resample"} - roles) for arch, roles in coverage.items() if roles != {"convolution", "resample"}}
    require(not missing, f"missing real kernel code objects: {missing}")
    return dict(status="PASS_OFFLINE_ARTIFACT_ONLY", hardware_status="NOT_TESTED", path=str(Path(path).resolve()),
                sha256=hashlib.sha256(data).hexdigest(), requested_archs=archs, host_exports=sorted(host_exports), objects=records,
                limitation="Does not prove kernel execution or correctness on any GPU. Separate staging runtime smoke required.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("dll", type=Path)
    parser.add_argument("--readobj", type=Path, default=Path(r"C:\Program Files\AMD\ROCm\7.2\bin\llvm-readobj.exe"))
    parser.add_argument("--arch", action="append", dest="archs")
    parser.add_argument("--report", type=Path)
    args = parser.parse_args()
    try:
        result = inspect(args.dll, args.readobj, args.archs or DEFAULT_ARCHS)
    except (OSError, ValueError, subprocess.SubprocessError, struct.error) as error:
        result = dict(status="FAIL", error=str(error), hardware_status="NOT_TESTED")
    if args.report:
        args.report.write_text(json.dumps(result, indent=2), encoding="utf-8")
    print(json.dumps({key: value for key, value in result.items() if key != "objects"}))
    return 0 if result["status"] == "PASS_OFFLINE_ARTIFACT_ONLY" else 1


if __name__ == "__main__":
    raise SystemExit(main())
