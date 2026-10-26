
import os
from PIL import Image

src = r"D:\RTA-hqplayer\09-origami-ribbon.png"
res = r"D:\RTA-hqplayer\res"
img = Image.open(src).convert("RGBA")
print("source:", img.size, img.mode)

# 官方 ICO：Windows 标准多尺寸（16/24/32/48/64/128/256）
sizes = [(16,16),(24,24),(32,32),(48,48),(64,64),(128,128),(256,256)]
ico_path = os.path.join(res, "rlplayer.ico")
img.save(ico_path, format="ICO", sizes=sizes)
print("ico:", ico_path, os.path.getsize(ico_path), "bytes")

# 同步 256 / 64 PNG（仓库里既有的两个规格，保持一致）
for n in (256, 64):
    out = os.path.join(res, f"rlplayer-{n}.png")
    img.resize((n, n), Image.LANCZOS).save(out, format="PNG", optimize=True)
    print(f"png{n}:", out, os.path.getsize(out), "bytes")

# 把原始资产也留一份到 res（作为图标源，便于以后重生成）
import shutil
shutil.copy2(src, os.path.join(res, "official-icon-source.png"))
print("source copied to res/official-icon-source.png",
      os.path.getsize(os.path.join(res, "official-icon-source.png")))
