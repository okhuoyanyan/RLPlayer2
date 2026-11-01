
# -*- coding: utf-8 -*-
import os, struct, io
from PIL import Image, ImageDraw, ImageFont
R=r"D:\RTA-hqplayer\res\icon-rl"
data=open(os.path.join(R,"rlplayer-rl.ico"),"rb").read()
n=struct.unpack("<HHH", data[:6])[2]
frames=[]
for i in range(n):
    off=6+16*i
    w,h,cc,rr,pl,bpp,sz,doff=struct.unpack("<BBBBHHII", data[off:off+16])
    w=256 if w==0 else w
    im=Image.open(io.BytesIO(data[doff:doff+sz])).convert("RGBA")
    frames.append((w,im))
    print("frame", w, im.size)

# contact sheet straight from the .ico payloads (proof the file is readable)
H=300
sheet=Image.new("RGB",(1180,H),"#F2EFE9")
d=ImageDraw.Draw(sheet)
x=16
for w,im in frames:
    big=im.resize((w*2,w*2),Image.NEAREST) if w<=32 else im
    sheet.paste(big,(x,60),big)
    d.text((x, 270), str(w), font=ImageFont.truetype(r"C:\Windows\Fonts\segoeuib.ttf",13), fill="#444")
    x += big.size[0]+16
sheet.save(os.path.join(R,"preview-ico-frames.png"))
print("saved")
