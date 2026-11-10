#!/usr/bin/env python3
"""Build the self-contained Windows service package: dist/RLPlayer-<version>-server.zip.

Contents (all runtime files must be inside the zip; the server host has no pip):
  LICENSE, docs/国内可达-反馈与增量-规划.md
  server/README.md, server/deploy/*, server/*.py
  server/third_party/ip2region/*        (Apache-2.0 reader)
  server/data/ip2region_v4.xdb          (offline IPv4 database)
"""
from __future__ import annotations

import argparse
import hashlib
import io
from pathlib import Path
import re
import zipfile

ROOT = Path(__file__).resolve().parents[1]
SERVER = ROOT / "server"

FIXED_TIME = (1980, 1, 1, 0, 0, 0)


def version() -> str:
    text = (ROOT / "src" / "version.h").read_text(encoding="utf-8", errors="replace")
    match = re.search(r'#define\s+RLPLAYER_VERSION\s+"([^"]+)"', text)
    if not match:
        raise SystemExit("cannot read RLPLAYER_VERSION from src/version.h")
    return match.group(1)


def collect() -> list[tuple[str, Path]]:
    items: list[tuple[str, Path]] = [("LICENSE", ROOT / "LICENSE"),
                                     ("docs/国内可达-反馈与增量-规划.md", ROOT / "docs" / "国内可达-反馈与增量-规划.md")]
    for path in sorted(SERVER.rglob("*")):
        if not path.is_file():
            continue
        if "__pycache__" in path.parts or path.suffix in (".pyc", ".pyo"):
            continue
        relative = path.relative_to(ROOT).as_posix()
        items.append((relative, path))
    return items


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", default="")
    parser.add_argument("--version", default="")
    args = parser.parse_args()
    target = Path(args.output) if args.output else ROOT / "dist" / ("RLPlayer-%s-server.zip" % (args.version or version()))
    target.parent.mkdir(parents=True, exist_ok=True)
    items = collect()
    buffer = io.BytesIO()
    with zipfile.ZipFile(buffer, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for relative, path in items:
            info = zipfile.ZipInfo(relative, FIXED_TIME)
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o644 << 16
            archive.writestr(info, path.read_bytes())
    data = buffer.getvalue()
    target.write_bytes(data)
    print("zip     :", target)
    print("entries :", len(items))
    print("size    :", len(data), "B")
    print("sha256  :", hashlib.sha256(data).hexdigest())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
