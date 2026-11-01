
# -*- coding: utf-8 -*-
import os, struct
from PIL import Image, ImageDraw, ImageFont
R=r"D:\RTA-hqplayer\res\icon-rl"
p=os.path.join(R,"rlplayer-rl.ico")
data=open(p,"rb").read()
res,typ,n=struct.unpack("<HHH", data[:6])
print("ico header: reserved=%d type=%d count=%d size=%d" % (res,typ,n,len(data)))
entries=[]
for i in range(n):
    off=6+16*i
    w,h,cc,rr,planes,bpp,sz,dataoff=struct.unpack("<BBBBHHII", data[off:off+16])
    entries.append((w if w else 256, h if h else 256, bpp, sz))
for e in entries: print("  frame %dx%d %dbpp %d bytes" % e)
print("total frames ok:", len(entries)==n)

for f in sorted(os.listdir(R)):
    print("%-28s %8d bytes" % (f, os.path.getsize(os.path.join(R,f))))
