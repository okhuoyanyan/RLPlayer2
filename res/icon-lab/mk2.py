
# -*- coding: utf-8 -*-
import numpy as np, os, math
from PIL import Image, ImageDraw, ImageFilter, ImageFont

OUT = r"D:\RTA-hqplayer\res\icon-lab"
FONT = r"C:\Windows\Fonts\ariblk.ttf"

# ---------------- palette (sampled from the reference artwork) ----------------
NAVY_DEEP  = (10, 22, 58)
NAVY_MID   = (23, 48, 108)
AZURE      = (58, 143, 232)
SKY        = (96, 190, 245)
CORAL      = (247, 106, 106)
PEACH      = (255, 176, 120)
AMBER      = (255, 205, 150)
CREAM      = (255, 246, 233)
MINT       = (120, 226, 226)

def vgrad(size, stops):
    w, h = size
    t = np.linspace(0, 1, h)
    out = np.zeros((h, 3))
    for i in range(len(stops) - 1):
        p0, c0 = stops[i]; p1, c1 = stops[i + 1]
        m = (t >= p0) & (t <= p1)
        if m.any():
            k = ((t[m] - p0) / max(1e-6, p1 - p0))[:, None]
            out[m] = np.array(c0, float) * (1 - k) + np.array(c1, float) * k
    out[t < stops[0][0]] = stops[0][1]
    out[t > stops[-1][0]] = stops[-1][1]
    arr = np.repeat(out[:, None, :], w, axis=1).astype(np.uint8)
    return Image.fromarray(arr, "RGB").convert("RGBA")

def dgrad(size, stops, angle=-35.0):
    """diagonal linear gradient"""
    w, h = size
    yy, xx = np.mgrid[0:h, 0:w].astype(np.float64)
    a = math.radians(angle)
    p = (xx * math.cos(a) + yy * math.sin(a))
    p = (p - p.min()) / (p.max() - p.min())
    out = np.zeros((h, w, 3))
    for i in range(len(stops) - 1):
        p0, c0 = stops[i]; p1, c1 = stops[i + 1]
        m = (p >= p0) & (p <= p1)
        if m.any():
            k = ((p[m] - p0) / max(1e-6, p1 - p0))[:, None]
            out[m] = np.array(c0, float) * (1 - k) + np.array(c1, float) * k
    out[p < stops[0][0]] = stops[0][1]
    out[p > stops[-1][0]] = stops[-1][1]
    return Image.fromarray(out.astype(np.uint8), "RGB").convert("RGBA")

def soft_blob(size, box, color, blur, alpha):
    lay = Image.new("RGBA", size, (0, 0, 0, 0))
    ImageDraw.Draw(lay).ellipse(box, fill=tuple(color) + (int(alpha),))
    return lay.filter(ImageFilter.GaussianBlur(blur))

def radial(size, center, radius, color, alpha=1.0, power=2.0):
    w, h = size
    yy, xx = np.mgrid[0:h, 0:w]
    d = np.sqrt((xx - center[0]) ** 2 + (yy - center[1]) ** 2) / radius
    a = np.clip(1 - d, 0, 1) ** power * alpha
    arr = np.zeros((h, w, 4), np.uint8)
    arr[..., 0], arr[..., 1], arr[..., 2] = color
    arr[..., 3] = (a * 255).astype(np.uint8)
    return Image.fromarray(arr, "RGBA")

def squircle_mask(size, radius):
    m = Image.new("L", (size, size), 0)
    ImageDraw.Draw(m).rounded_rectangle([0, 0, size - 1, size - 1], radius=radius, fill=255)
    return m

def drop_shadow(mask, offset, blur, color, alpha):
    solid = Image.new("RGBA", mask.size, tuple(color) + (int(alpha),))
    lay = Image.new("RGBA", mask.size, (0, 0, 0, 0))
    lay.paste(solid, (0, 0), mask)
    lay = lay.filter(ImageFilter.GaussianBlur(blur))
    out = Image.new("RGBA", mask.size, (0, 0, 0, 0))
    out.paste(lay, offset, lay)
    return out

def text_mask(size, txt, font_path, target_w, tracking_ratio, y_bias=0.0, bold_ratio=0.0):
    """Render txt centered, scaled so the ink width == target_w*size."""
    W = size
    big = W * 3
    f = ImageFont.truetype(font_path, int(big * 0.5))
    # measure at a big size then rescale
    tmp = Image.new("L", (big, big), 0)
    d = ImageDraw.Draw(tmp)
    track = int(big * 0.5 * tracking_ratio)
    boxes = [d.textbbox((0, 0), c, font=f) for c in txt]
    ws = [b[2] - b[0] for b in boxes]
    total = sum(ws) + track * (len(txt) - 1)
    scale = (target_w * W) / total
    fs = int(big * 0.5 * scale)
    f = ImageFont.truetype(font_path, fs)
    track = int(fs * tracking_ratio)
    boxes = [d.textbbox((0, 0), c, font=f) for c in txt]
    ws = [b[2] - b[0] for b in boxes]
    total = sum(ws) + track * (len(txt) - 1)
    asc, desc = f.getmetrics()
    x = (big - total) / 2.0
    base_y = big / 2.0 + (asc - (asc + desc) / 2.0)
    for c, wch in zip(txt, ws):
        d.text((x, base_y), c, font=f, fill=255, anchor="ls")
        x += wch + track
    bb = tmp.getbbox()
    ink_w, ink_h = bb[2] - bb[0], bb[3] - bb[1]
    # place ink centre at canvas centre
    ox = int(W / 2 - (bb[0] + ink_w / 2))
    oy = int(W / 2 - (bb[1] + ink_h / 2) + y_bias * W)
    out = Image.new("L", (W, W), 0)
    out.paste(tmp, (ox, oy))
    return out

def groove_rings(size, cx, cy, r_out, r_in, step, color, a_major, a_minor, width):
    lay = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    d = ImageDraw.Draw(lay)
    r = r_out; i = 0
    while r > r_in:
        a = a_major if i % 6 == 0 else a_minor
        d.ellipse([cx - r, cy - r, cx + r, cy + r], outline=tuple(color) + (int(a),), width=width)
        r -= step; i += 1
    return lay

def build(size=1024, variant="A", detail=1.0):
    W = size
    img = vgrad((W, W), [(0.0, (28, 62, 128)), (0.38, (18, 40, 92)), (0.72, (14, 31, 76)), (1.0, (9, 20, 50))])

    # painterly colour fields (the reference's watercolour bleed)
    img = Image.alpha_composite(img, soft_blob((W, W), (-0.15*W, -0.25*W, 0.72*W, 0.45*W), AZURE, 0.10*W, 165))
    img = Image.alpha_composite(img, soft_blob((W, W), (0.45*W, -0.30*W, 1.18*W, 0.40*W), SKY, 0.11*W, 130))
    img = Image.alpha_composite(img, soft_blob((W, W), (0.52*W, 0.35*W, 1.20*W, 0.95*W), CORAL, 0.13*W, 95))
    img = Image.alpha_composite(img, soft_blob((W, W), (-0.22*W, 0.60*W, 0.42*W, 1.22*W), PEACH, 0.11*W, 120))
    img = Image.alpha_composite(img, soft_blob((W, W), (0.22*W, 0.70*W, 1.05*W, 1.25*W), NAVY_DEEP, 0.10*W, 150))

    # groove rings (vinyl), centred slightly low so the letters sit in the "label"
    img = Image.alpha_composite(img, groove_rings(W, W*0.5, W*0.5, W*0.470, W*0.175, W*0.0165,
                                                 (198, 228, 255), 60*detail, 26*detail, max(1, int(W/512))))

    # accent arcs: coral (upper right), mint (lower left)
    acc = Image.new("RGBA", (W, W), (0, 0, 0, 0))
    d = ImageDraw.Draw(acc)
    d.arc([W*0.115, W*0.115, W*0.885, W*0.885], start=-74, end=-12, fill=CORAL + (225,), width=int(W*0.030))
    d.arc([W*0.155, W*0.155, W*0.845, W*0.845], start=192, end=250, fill=MINT + (200,), width=int(W*0.020))
    acc = acc.filter(ImageFilter.GaussianBlur(W*0.0045))
    img = Image.alpha_composite(img, acc)

    # warm "label" glow behind the monogram
    img = Image.alpha_composite(img, radial((W, W), (W*0.5, W*0.5), W*0.40, (255, 168, 120), 0.30, 1.7))

    # ---- monogram
    mask = text_mask(W, "RL", FONT, target_w=0.655, tracking_ratio=-0.055, y_bias=-0.005)
    img = Image.alpha_composite(img, drop_shadow(mask, (0, int(W*0.016)), W*0.014, (6, 12, 30), 135))

    fill = dgrad((W, W), [(0.0, (255, 252, 243)), (0.34, (255, 240, 213)), (0.70, (255, 198, 140)), (1.0, (250, 146, 118))], angle=-40)
    letters = Image.new("RGBA", (W, W), (0, 0, 0, 0))
    letters.paste(fill, (0, 0), mask)
    # top rim light
    rim = Image.new("RGBA", (W, W), (255, 255, 255, 0))
    rim.paste(Image.new("RGBA", (W, W), (255, 255, 255, 110)), (0, 0), mask)
    rim = rim.filter(ImageFilter.GaussianBlur(W*0.006))
    interior = mask.filter(ImageFilter.MinFilter(9)) if W >= 512 else mask
    rim = rim.paste if False else rim
    letters = Image.alpha_composite(letters, rim.point(lambda v: v) if False else rim)
    img = Image.alpha_composite(img, letters)

    # vignette
    img = Image.alpha_composite(img, radial((W, W), (W*0.5, W*0.5), W*0.80, (3, 8, 22), 0.50, 2.6))

    img.putalpha(squircle_mask(W, int(W*0.2225)))
    return img

os.makedirs(OUT, exist_ok=True)
big = build(1024, "A")
big.save(os.path.join(OUT, "v2_1024.png"))

sheet = Image.new("RGB", (1300, 460), "#EFEBE3")
sh = big.resize((300, 300), Image.LANCZOS); sheet.paste(sh, (25, 80), sh)
x = 360
for sz in (128, 64, 48, 32, 24, 16):
    im = big.resize((sz, sz), Image.LANCZOS)
    sheet.paste(im, (x, 80 + (300-sz)//2), im)
    x += sz + 30
sheet.save(os.path.join(OUT, "v2_sheet.png"))
print("ok")
