
# -*- coding: utf-8 -*-
import os
from PIL import Image, ImageDraw, ImageFont
R=r"D:\RTA-hqplayer\res\icon-rl"
m=Image.open(os.path.join(R,"rlplayer-rl-1024.png")).convert("RGBA")
# preview 1: white sheet with size ladder
sheet=Image.new("RGB",(1180,420),"#F6F4EF"); d=ImageDraw.Draw(sheet)
big=m.resize((300,300),Image.LANCZOS); sheet.paste(big,(20,60),big)
x=350
for s in (128,64,48,32,24,16):
    t=m.resize((s,s),Image.LANCZOS); sheet.paste(t,(x,60+(300-s)//2),t); x+=s+26
sheet.save(os.path.join(R,"preview-sizes.png"))
# preview 2: real contexts - taskbar / explorer / desktop icon on dark
ctx=Image.new("RGB",(900,340),"#202124")
bar=Image.new("RGB",(900,48),"#1A1A1C"); ctx.paste(bar,(0,20))
xb=430
for s in (24,24,24,32,48):
    t=m.resize((s,s),Image.LANCZOS); ctx.paste(t,(xb,20+(48-s)//2),t); xb+=56
d=ImageDraw.Draw(ctx)
f=ImageFont.truetype(r"C:\Windows\Fonts\segoeui.ttf",16)
d.text((20,30),"taskbar 24px", font=f, fill="#BBB")
d.text((20,100),"Explorer / large icons 256px", font=f, fill="#CCC")
for i,s in enumerate((256,96,64,48,32,16)):
    t=m.resize((s,s),Image.LANCZOS); ctx.paste(t,(20+i*230 if False else 20,0) if False else (20+sum([256,96,64,48,32,16][:i])+18*i, 130),t)
ctx.save(os.path.join(R,"preview-context.png"))
print("ok")
