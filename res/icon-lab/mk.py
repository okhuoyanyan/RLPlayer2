
import numpy as np, os
from PIL import Image, ImageDraw, ImageFilter, ImageFont

S = 1024
FONT = r"C:\Windows\Fonts\ariblk.ttf"

def lerp_grad(size, stops):
    """stops: list of (pos0..1, (r,g,b))"""
    w, h = size
    t = np.linspace(0, 1, h)
    out = np.zeros((h, 3), dtype=np.float64)
    for i in range(len(stops) - 1):
        p0, c0 = stops[i]; p1, c1 = stops[i + 1]
        m = (t >= p0) & (t <= p1)
        if not m.any(): continue
        k = ((t[m] - p0) / max(1e-6, (p1 - p0)))[:, None]
        out[m] = np.array(c0) * (1 - k) + np.array(c1) * k
    out[t < stops[0][0]] = stops[0][1]
    out[t > stops[-1][0]] = stops[-1][1]
    arr = np.repeat(out[:, None, :], w, axis=1).astype(np.uint8)
    return Image.fromarray(arr, "RGB")

def radial_glow(size, center, radius, color, strength=1.0):
    w, h = size
    yy, xx = np.mgrid[0:h, 0:w]
    d = np.sqrt((xx - center[0]) ** 2 + (yy - center[1]) ** 2) / radius
    a = np.clip(1 - d, 0, 1) ** 2 * strength
    arr = np.zeros((h, w, 4), dtype=np.uint8)
    arr[..., :3] = color
    arr[..., 3] = (a * 255).astype(np.uint8)
    return Image.fromarray(arr, "RGBA")

def blob(size, box, color, blur, alpha):
    lay = Image.new("RGBA", size, (0, 0, 0, 0))
    d = ImageDraw.Draw(lay)
    d.ellipse(box, fill=tuple(color) + (alpha,))
    return lay.filter(ImageFilter.GaussianBlur(blur))

def rounded_mask(size, radius):
    m = Image.new("L", size, 0)
    ImageDraw.Draw(m).rounded_rectangle([0, 0, size[0] - 1, size[1] - 1], radius=radius, fill=255)
    return m

def shadow_of(mask, offset, blur, color, alpha):
    lay = Image.new("RGBA", mask.size, (0, 0, 0, 0))
    solid = Image.new("RGBA", mask.size, tuple(color) + (alpha,))
    lay.paste(solid, (0, 0), mask)
    lay = lay.filter(ImageFilter.GaussianBlur(blur))
    out = Image.new("RGBA", mask.size, (0, 0, 0, 0))
    out.paste(lay, offset, lay)
    return out

def make(variant="A", size=S):
    W = size
    base = Image.new("RGBA", (W, W), (0, 0, 0, 0))

    # ---- background: navy, painterly
    bg = lerp_grad((W, W), [(0, (18, 44, 96)), (0.45, (11, 27, 64)), (1, (7, 16, 40))])
    base = bg.convert("RGBA")

    # painterly colour field
    for box, col, blur, al in [
        ((-120, -180, 620, 480), (46, 126, 214), 120, 150),   # blue upper-left
        ((520, -220, 1180, 420), (58, 150, 232), 130, 120),   # blue upper-right
        ((560, 260, 1120, 760), (244, 96, 132), 150, 70),     # pink right
        ((-160, 620, 420, 1120), (255, 120, 92), 130, 110),   # coral bottom-left
        ((300, 760, 980, 1180), (18, 40, 92), 110, 120),      # deep navy bottom
    ]:
        base = Image.alpha_composite(base, blob((W, W), box, col, blur, al))

    # ---- grooves (vinyl identity)
    grooves = Image.new("RGBA", (W, W), (0, 0, 0, 0))
    gd = ImageDraw.Draw(grooves)
    cx = cy = W / 2
    r = W * 0.455
    i = 0
    while r > W * 0.20:
        a = 46 if i % 5 == 0 else 22
        gd.ellipse([cx - r, cy - r, cx + r, cy + r], outline=(190, 225, 255, a), width=2)
        r -= W * 0.0165
        i += 1
    base = Image.alpha_composite(base, grooves)

    # ---- warm accents: coral arc upper-right, cyan splash lower-left
    acc = Image.new("RGBA", (W, W), (0, 0, 0, 0))
    ad = ImageDraw.Draw(acc)
    ad.arc([W * 0.10, W * 0.10, W * 0.90, W * 0.90], start=-78, end=-8, fill=(255, 156, 96, 210), width=int(W * 0.028))
    ad.arc([W * 0.10, W * 0.10, W * 0.90, W * 0.90], start=196, end=246, fill=(96, 214, 236, 190), width=int(W * 0.022))
    acc = acc.filter(ImageFilter.GaussianBlur(W * 0.004))
    base = Image.alpha_composite(base, acc)

    # ---- monogram
    txt = "RL"
    fs = int(W * 0.62)
    font = ImageFont.truetype(FONT, fs)
    # tighten tracking
    tmp = Image.new("L", (W * 2, W * 2), 0)
    td = ImageDraw.Draw(tmp)
    tracking = -int(W * 0.045)
    widths = [td.textbbox((0, 0), ch, font=font)[2] for ch in txt]
    total = sum(widths) + tracking * (len(txt) - 1)
    x = (W - total) / 2
    for ch, wch in zip(txt, widths):
        td.text((x, W * 2 / 2), ch, font=font, fill=255, anchor="lm")
        x += wch + tracking
    mask = tmp.crop((W * 2 // 2 - W // 2, W * 2 // 2 - W // 2, W * 2 // 2 - W // 2 + W, W * 2 // 2 - W // 2 + W))
    bb = mask.getbbox()
    dx = int((W - (bb[2] - bb[0])) / 2 - bb[0])
    dy = int((W - (bb[3] - bb[1])) / 2 - bb[1] - W * 0.01)
    mask = Image.fromarray(np.roll(np.roll(np.array(mask), dy, axis=0), dx, axis=1))

    # warm halo behind letters
    halo = shadow_of(mask, (0, 0), W * 0.06, (255, 150, 90), 170)
    base = Image.alpha_composite(base, halo)

    # letter fill: cream -> peach gradient
    fillgrad = lerp_grad((W, W), [(0, (255, 251, 240)), (0.55, (255, 236, 205)), (1, (255, 197, 132))]).convert("RGBA")
    letters = Image.new("RGBA", (W, W), (0, 0, 0, 0))
    letters.paste(fillgrad, (0, 0), mask)

    # subtle top rim light
    rim = Image.new("RGBA", (W, W), (255, 255, 255, 0))
    rim.paste(Image.new("RGBA", (W, W), (255, 255, 255, 120)), (0, 0), mask)
    rim = rim.filter(ImageFilter.GaussianBlur(W * 0.004))
    letters = Image.alpha_composite(letters, rim)

    base = Image.alpha_composite(shadow_of(mask, (0, int(W * 0.018)), W * 0.012, (4, 9, 24), 150), base)
    base = Image.alpha_composite(base, letters)

    # ---- vignette + squircle mask
    vig = radial_glow((W, W), (W / 2, W / 2), W * 0.78, (0, 0, 0), 0.55)
    vig = Image.fromarray(np.dstack([np.full((W, W), 3, np.uint8), np.full((W, W), 7, np.uint8), np.full((W, W), 20, np.uint8), (np.array(vig)[..., 3] * 0.9).astype(np.uint8)]), "RGBA")
    base = Image.alpha_composite(base, vig)

    base.putalpha(rounded_mask((W, W), int(W * 0.225)))
    return base

out = r"D:\RTA-hqplayer\res\icon-lab"
os.makedirs(out, exist_ok=True)
img = make("A")
img.save(os.path.join(out, "v1_1024.png"))
# preview sheet
sheet = Image.new("RGB", (1200, 420), "#EEEAE2")
s224 = img.resize((256, 256), Image.LANCZOS)
sheet.paste(s224, (20, 80), s224)
x_ = 310
for sz, lbl in [(128, "128"), (64, "64"), (48, "48"), (32, "32"), (16, "16")]:
    im = img.resize((sz, sz), Image.LANCZOS)
    sheet.paste(im, (x_, 80), im)
    x_ += sz + 26
d = ImageDraw.Draw(sheet)
d.text((x_ + 10, 120), "small sizes", fill="#333")
sheet.save(os.path.join(out, "v1_sheet.png"))
print("done")
