"""Mutate only private copies of a real HIP DLL; no GPU or executable invocation."""
import argparse
import json
import struct
import subprocess
import sys
from pathlib import Path
sys.dont_write_bytecode = True
from check_hip_artifact import bundle_objects, MAGIC


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--dll", type=Path, required=True)
    p.add_argument("--evidence", type=Path, required=True)
    a = p.parse_args()
    a.evidence.mkdir(parents=True, exist_ok=True)
    raw = a.dll.read_bytes()
    objects = bundle_objects(raw)
    original = bytearray(raw)
    variants = {"valid": bytes(original)}
    # Old gate accepts this: every expected arch string remains in fake data.
    broken = bytearray(raw)
    for obj in objects:
        broken[obj["offset"]:obj["offset"] + 4] = b"FAKE"
    variants["strings-without-elf"] = bytes(broken)
    broken = bytearray(raw)
    obj = next(o for o in objects if o["arch"] == "gfx1201")
    label = b"hipv4-amdgcn-amd-amdhsa--gfx1201"
    broken[obj["name_offset"]:obj["name_offset"] + len(label)] = label.replace(b"gfx1201", b"gfx1200")
    variants["descriptor-elf-target-mismatch"] = bytes(broken)
    broken = bytearray(raw)
    struct.pack_into("<H", broken, obj["offset"] + 18, 62)  # x64 ELF instead of AMDGPU
    variants["wrong-elf-machine"] = bytes(broken)
    broken = bytearray(raw)
    for o in objects:
        if o["arch"] == "gfx1201":
            struct.pack_into("<Q", broken, o["descriptor"] + 8, 0)
    variants["zero-size-device-objects"] = bytes(broken)
    broken = bytearray(raw)
    # ELF header still matches target; structural corruption must fail the tool.
    struct.pack_into("<Q", broken, obj["offset"] + 40, 0xfffffffffffffff0)
    variants["invalid-elf-section-table"] = bytes(broken)
    broken = bytearray(raw)
    for o in objects:
        name = b"__CLANG_OFFLOAD_BUNDLE__"
        broken[o["bundle"]:o["bundle"] + len(name)] = b"X" * len(name)
    variants["arch-strings-only-no-bundle"] = bytes(broken)
    # Remove one mandatory API string from the export directory.
    variants["missing-host-export"] = raw.replace(b"rta_gpu_available\0", b"xxx_gpu_available\0")
    results = []
    checker = Path(__file__).with_name("check_hip_artifact.py")
    for name, data in variants.items():
        path = a.evidence / (name + ".dll")
        path.write_bytes(data)
        report = a.evidence / (name + ".json")
        command = [sys.executable, str(checker), str(path), "--arch", "gfx1201", "--report", str(report)]
        proc = subprocess.run(command, capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=60)
        expected = name == "valid"
        correct = (proc.returncode == 0) == expected
        results.append(dict(case=name, exit=proc.returncode, expected_success=expected, correct=correct,
                            report=json.loads(report.read_text()) if report.exists() else proc.stderr))
        print(name, proc.returncode, "correct=" + str(correct))
    (a.evidence / "regression.json").write_text(json.dumps(results, indent=2), encoding="utf-8")
    return 0 if all(r["correct"] for r in results) else 1


if __name__ == "__main__":
    raise SystemExit(main())
