
# -*- coding: utf-8 -*-
import os
from PIL import Image, ImageDraw, ImageFont
LAB=r"D:\RTA-hqplayer\res\icon-lab\final"
old=r"D:\RTA-hqplayer\res\rlplayer-256.png"
items=[("A  cream RL / navy disc",os.path.join(LAB,"rlplayer-icon-A-1024.png")),
       ("B  navy RL / warm label",os.path.join(LAB,"rlplayer-icon-B-1024.png")),
       ("C  two-tone R + L",os.path.join(LAB,"rlplayer-icon-C-1024.png")),
       ("current icon",old)]
SIZES=[256,128,64,48,32,24,16]
W=1500; ROW=230
sheet=Image.new("RGB",(W,ROW*len(items)+40),"#FFFFFF")
d=ImageDraw.Draw(sheet)
f=ImageFont.truetype(r"C:\Windows\Fonts\segoeuib.ttf",22)
for i,(lbl,p) in enumerate(items):
    im=Image.open(p).convert("RGBA")
    y=20+i*ROW
    d.text((16,y), lbl, font=f, fill="#111")
    big=im.resize((176,176),Image.LANCZOS)
    sheet.paste(big,(16,y+40),big)
    x=230
    for s in SIZES:
        t=im.resize((s,s),Image.LANCZOS)
        sheet.paste(t,(x,y+40+(176-s)//2),t)
        d.text((x,y+40+176+2), str(s), font=ImageFont.truetype(r"C:\Windows\Fonts\segoeuib.ttf",14), fill="#666")
        x+=s+26
sheet.save(os.path.join(LAB,"compare-white.png"))

# dark background strip for 16/24/32 legibility
dark=Image.new("RGB",(760,150),"#1B1B1D")
d2=ImageDraw.Draw(dark)
x=20
for i,(lbl,p) in enumerate(items):
    im=Image.open(p).convert("RGBA")
    for s in (32,24,16):
        t=im.resize((s,s),Image.LANCZOS)
        dark.paste(t,(x,20+(32-s)//2),t)
        dark.paste(t,(x,64+(32-s)//2),t)
        x+=s+8
    x+=20
dark.save(os.path.join(LAB,"compare-dark.png"))
print("ok")
