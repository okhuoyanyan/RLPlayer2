
import os
from PIL import Image, ImageDraw, ImageFont

fonts = ["ariblk.ttf","bahnschrift.ttf","GOTHICB.TTF","impact.ttf","FRAMDCN.TTF","segoeuib.ttf","HarmonyOS_Sans_SC_Bold.ttf","FRABK.TTF","calibrib.ttf","trebucbd.ttf","verdanab.ttf"]
canvas = Image.new("RGB",(1600,1100),"#12194a")
d = ImageDraw.Draw(canvas)
y_ = 20
for f in fonts:
    p = os.path.join(r"C:\Windows\Fonts", f)
    if not os.path.exists(p): continue
    try:
        ft = ImageFont.truetype(p, 150)
    except Exception as e:
        print("fail",f,e); continue
    d.text((30,y_), "RL", font=ft, fill="#FFF7EC")
    d.text((500,y_), "rl av", font=ft, fill="#9fd8ff")
    d.text((1000,y_), f, font=ImageFont.truetype(p, 40), fill="#ffc15e")
    y_ += 180
canvas.save(r"D:\RTA-hqplayer\res\icon-lab\fonttest.png")
print("ok")
