
# -*- coding: utf-8 -*-
"""Pack a multi-size Windows .ico from PNG frames (PNG-payload, Vista+)."""
import os, struct, io

def pack_ico(frames, out_path):
    """frames: list of (size, PIL.Image). Writes ICO with PNG-compressed payloads."""
    blobs = []
    for size, im in frames:
        buf = io.BytesIO()
        im.save(buf, format="PNG", optimize=True)
        blobs.append((size, buf.getvalue()))

    n = len(blobs)
    header = struct.pack("<HHH", 0, 1, n)
    offset = 6 + 16 * n
    dirs = b""
    body = b""
    for size, data in blobs:
        b = 0 if size >= 256 else size
        dirs += struct.pack("<BBBBHHII", b, b, 0, 0, 1, 32, len(data), offset)
        offset += len(data)
        body += data
    with open(out_path, "wb") as f:
        f.write(header + dirs + body)
    return n, len(header + dirs + body)
