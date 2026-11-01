
# -*- coding: utf-8 -*-
import os
from PIL import Image, ImageDraw, ImageFont

FIN = r"D:\RTA-hqplayer\res\icon-lab\final"
OUT = r"D:\RTA-hqplayer\res\icon-rl"
CJK = r"C:\Windows\Fonts\HarmonyOS_Sans_SC_Bold.ttf"
F  = ImageFont.truetype(CJK, 24)
FS = ImageFont.truetype(CJK, 15)

items = [
    ("A", "已交付 · 奶油 RL / 深蓝唱片", os.path.join(OUT, "rlplayer-rl-1024.png")),
    ("B", "备选 · 深蓝 RL / 暖色标签盘", os.path.join(FIN, "rlplayer-icon-B-1024.png")),
    ("C", "备选 · R 暖 L 冷 双色", os.path.join(FIN, "rlplayer-icon-C-1024.png")),
]
W, ROW = 1240, 250
sheet = Image.new("RGB", (W, ROW * len(items) + 30), "#F4F2ED")
d = ImageDraw.Draw(sheet)
for i, (tag, lbl, p) in enumerate(items):
    im = Image.open(p).convert("RGBA")
    y = 15 + i * ROW
    d.text((18, y + 6), tag, font=F, fill="#111")
    d.text((48, y + 11), lbl, font=FS, fill="#555")
    big = im.resize((190, 190), Image.LANCZOS)
    sheet.paste(big, (18, y + 40), big)
    x = 232
    for s in (128, 96, 64, 48, 32, 24, 16):
        t = im.resize((s, s), Image.LANCZOS)
        sheet.paste(t, (x, y + 40 + (190 - s) // 2), t)
        d.text((x, y + 40 + 190 + 2), str(s), font=FS, fill="#777")
        x += s + 24
sheet.save(os.path.join(OUT, "concepts.png"))
print("ok")
