
# -*- coding: utf-8 -*-
import os, sys, importlib.util, struct
sys.path.insert(0, r"D:\RTA-hqplayer\res\icon-lab")
from ico_pack import pack_ico
from PIL import Image

R = r"D:\RTA-hqplayer\res\icon-rl"
master = Image.open(os.path.join(R, "rlplayer-rl-1024.png")).convert("RGBA")
small  = Image.open(os.path.join(R, "rlplayer-rl-small-src-256.png")).convert("RGBA")

SIZES = [16, 20, 24, 32, 40, 48, 64, 96, 128, 256]
frames = []
for s in SIZES:
    src = small if s <= 32 else master
    frames.append((s, src.resize((s, s), Image.LANCZOS)))

n, total = pack_ico(frames, os.path.join(R, "rlplayer-rl.ico"))
print("ico frames=%d bytes=%d" % (n, total))

data = open(os.path.join(R, "rlplayer-rl.ico"), "rb").read()
res, typ, cnt = struct.unpack("<HHH", data[:6])
print("verify: reserved=%d type=%d count=%d" % (res, typ, cnt))
for i in range(cnt):
    off = 6 + 16 * i
    w, h, cc, rr, planes, bpp, sz, doff = struct.unpack("<BBBBHHII", data[off:off+16])
    w = 256 if w == 0 else w
    # confirm each payload starts with the PNG signature
    sig = data[doff:doff+8] == b"\x89PNG\r\n\x1a\n"
    print("  %3dx%-3d %2dbpp %6d bytes  png=%s" % (w, h, bpp, sz, sig))
