
# -*- coding: utf-8 -*-
"""RLPlayer 应用图标生成器 (RL monogram)

风格来源：用户上传的水彩黑胶插画
  · 深蓝唱片底 + 手绘同心槽纹
  · 湖蓝 / 珊瑚 / 蜜桃水彩晕染
  · 珊瑚色笔触弧（右上）、薄荷色笔触弧（左下）
  · 字体 = 极端粗黑体，奶油 → 蜜桃渐变，带极浅的立体挤出

用法：
    python make_icon_rl.py            # 生成全部 PNG + ICO + 预览图

输出（同目录）：
    rlplayer-rl-1024.png   母版
    rlplayer-rl-512/256/128/64/48/32/24/16.png
    rlplayer-rl.ico        16..256 共 10 帧（PNG 负载，Vista+）
    rlplayer-rl-small-src-256.png   16~32px 专用的强化小尺寸母版
    preview-sizes.png / preview-ico-frames.png / preview-context.png

渲染在 4 倍尺寸下完成后 LANCZOS 降采样，16px 才不至于糊成一团。
"""
import io
import math
import os
import struct

import numpy as np
from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
FONT = r"C:\Windows\Fonts\ariblk.ttf"   # Arial Black：字怀大、笔画厚，缩到 16px 仍可辨
SS = 4                                   # 超采样倍数

NAVY_DEEP = (8, 18, 48)
AZURE = (48, 132, 226)
SKY = (104, 196, 248)
CORAL = (244, 92, 102)
PEACH = (255, 176, 124)
AMBER = (255, 208, 138)
MINT = (122, 228, 226)


# ---------------------------------------------------------------- 基础绘图件
def vgrad(size, stops):
    w, h = size
    t = np.linspace(0, 1, h)
    out = np.zeros((h, 3))
    for i in range(len(stops) - 1):
        p0, c0 = stops[i]
        p1, c1 = stops[i + 1]
        m = (t >= p0) & (t <= p1)
        if m.any():
            k = ((t[m] - p0) / max(1e-6, p1 - p0))[:, None]
            out[m] = np.array(c0, float) * (1 - k) + np.array(c1, float) * k
    out[t < stops[0][0]] = stops[0][1]
    out[t > stops[-1][0]] = stops[-1][1]
    arr = np.repeat(out[:, None, :], w, axis=1).astype(np.uint8)
    return Image.fromarray(arr, "RGB").convert("RGBA")


def dgrad(size, stops, angle=-42.0, box=None):
    w, h = size
    yy, xx = np.mgrid[0:h, 0:w].astype(np.float64)
    a = math.radians(angle)
    p = xx * math.cos(a) + yy * math.sin(a)
    lo, hi = float(p.min()), float(p.max())
    if box is not None:
        x0, y0, x1, y1 = box
        q0 = x0 * math.cos(a) + y0 * math.sin(a)
        q1 = x1 * math.cos(a) + y1 * math.sin(a)
        lo, hi = min(q0, q1), max(q0, q1)
    p = np.clip((p - lo) / max(1e-6, hi - lo), 0, 1)
    out = np.zeros((h, w, 3))
    for i in range(len(stops) - 1):
        p0, c0 = stops[i]
        p1, c1 = stops[i + 1]
        m = (p >= p0) & (p <= p1)
        if m.any():
            k = ((p[m] - p0) / max(1e-6, p1 - p0))[:, None]
            out[m] = np.array(c0, float) * (1 - k) + np.array(c1, float) * k
    out[p < stops[0][0]] = stops[0][1]
    out[p > stops[-1][0]] = stops[-1][1]
    return Image.fromarray(np.clip(out, 0, 255).astype(np.uint8), "RGB").convert("RGBA")


def blob(size, box, color, blur, alpha):
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


def grooves(size, cx, cy, r_out, r_in, step, color, amaj, amin, width):
    lay = Image.new("RGBA", size, (0, 0, 0, 0))
    d = ImageDraw.Draw(lay)
    r, i = r_out, 0
    while r > r_in:
        d.ellipse([cx - r, cy - r, cx + r, cy + r],
                  outline=tuple(color) + ((amaj if i % 6 == 0 else amin),), width=width)
        r -= step
        i += 1
    return lay


def monogram(size, txt, font_path, target_w, tracking_ratio, y_bias=0.0):
    """把字母串渲染成遮罩，按"墨迹包围盒"在方形里居中。"""
    W = size
    big = W * 2
    d = ImageDraw.Draw(Image.new("L", (8, 8)))
    f = ImageFont.truetype(font_path, int(big * 0.4))
    ws = [d.textbbox((0, 0), c, font=f)[2] - d.textbbox((0, 0), c, font=f)[0] for c in txt]
    total = sum(ws) + int(big * 0.4 * tracking_ratio) * (len(txt) - 1)
    fs = int(big * 0.4 * target_w * W / total)
    f = ImageFont.truetype(font_path, fs)
    tr = int(fs * tracking_ratio)
    ws = [d.textbbox((0, 0), c, font=f)[2] - d.textbbox((0, 0), c, font=f)[0] for c in txt]
    total = sum(ws) + tr * (len(txt) - 1)
    asc, desc = f.getmetrics()
    tmp = Image.new("L", (big, big), 0)
    td = ImageDraw.Draw(tmp)
    x = (big - total) / 2.0
    base_y = big / 2.0 + (asc - (asc + desc) / 2.0)
    for c, wc in zip(txt, ws):
        td.text((x, base_y), c, font=f, fill=255, anchor="ls")
        x += wc + tr
    bb = tmp.getbbox()
    ox = int(W / 2 - (bb[0] + (bb[2] - bb[0]) / 2))
    oy = int(W / 2 - (bb[1] + (bb[3] - bb[1]) / 2) + y_bias * W)
    out = Image.new("L", (W, W), 0)
    out.paste(tmp, (ox, oy))
    return out


def taper_arc(size, box, start, end, color, w_max, w_min, alpha,
              taper=0.55, jitter=0.05, seed=0, blur=0.0016, specks=0):
    """沿椭圆弧的锥形笔触：中段最粗、两端收细，可选飞溅小点。"""
    W = size[0] if isinstance(size, (tuple, list)) else size
    x0, y0, x1, y1 = box
    cx, cy = (x0 + x1) / 2, (y0 + y1) / 2
    rx, ry = (x1 - x0) / 2, (y1 - y0) / 2
    n = 460
    rng = np.random.RandomState(seed)
    pts = []
    for i in range(n + 1):
        k = i / n
        e = math.sin(math.pi * k) ** taper
        w = w_min + (w_max - w_min) * e
        if jitter:
            w *= max(0.30, 1.0 + jitter * rng.randn())
        t = math.radians(start + (end - start) * k)
        px, py = cx + rx * math.cos(t), cy + ry * math.sin(t)
        nx, ny = math.cos(t) / max(rx, 1e-6), math.sin(t) / max(ry, 1e-6)
        L = math.hypot(nx, ny)
        nx, ny = nx / L, ny / L
        pts.append((px, py, w, nx, ny))
    poly = [(px + nx * w / 2, py + ny * w / 2) for (px, py, w, nx, ny) in pts]
    poly += [(px - nx * w / 2, py - ny * w / 2) for (px, py, w, nx, ny) in reversed(pts)]
    lay = Image.new("RGBA", (W, W), (0, 0, 0, 0))
    d = ImageDraw.Draw(lay)
    d.polygon(poly, fill=tuple(color) + (int(alpha),))
    for idx in (0, -1):
        px, py, w, nx, ny = pts[idx]
        d.ellipse([px - w / 2, py - w / 2, px + w / 2, py + w / 2], fill=tuple(color) + (int(alpha),))
    for idx in (0, -1):
        px, py, w, nx, ny = pts[idx]
        for _ in range(specks):
            ang = math.atan2(ny, nx) + rng.uniform(-1.1, 1.1)
            dist = rng.uniform(w * 0.9, w * 2.8)
            r = rng.uniform(w * 0.06, w * 0.24)
            ex, ey = px + math.cos(ang) * dist, py + math.sin(ang) * dist
            d.ellipse([ex - r, ey - r, ex + r, ey + r],
                      fill=tuple(color) + (int(alpha * rng.uniform(0.5, 0.9)),))
    return lay.filter(ImageFilter.GaussianBlur(W * blur))


def paint(size, mask, top, bottom, angle=-42, warm_shade=(150, 74, 44)):
    """按遮罩上渐变，并加左上高光 / 右下暖色阴影，做出浅浅的立体感。"""
    W = size
    bb = mask.getbbox()
    fill = dgrad((W, W), [(0.0, top), (1.0, bottom)], angle=angle, box=bb)
    lay = Image.new("RGBA", (W, W), (0, 0, 0, 0))
    lay.paste(fill, (0, 0), mask)
    hi = ImageChops.subtract(ImageChops.offset(mask, -max(1, int(W * 0.004)),
                                               -max(1, int(W * 0.004))), mask)
    hi = hi.filter(ImageFilter.GaussianBlur(W * 0.0035)).point(lambda v: int(v * 0.70))
    lo = ImageChops.subtract(ImageChops.offset(mask, max(1, int(W * 0.005)),
                                               max(1, int(W * 0.005))), mask)
    lo = lo.filter(ImageFilter.GaussianBlur(W * 0.004)).point(lambda v: int(v * 0.45))
    hlay = Image.new("RGBA", (W, W), (255, 255, 255, 0))
    hlay.putalpha(hi)
    llay = Image.new("RGBA", (W, W), tuple(warm_shade) + (0,))
    llay.putalpha(lo)
    return Image.alpha_composite(lay, Image.alpha_composite(llay, hlay))


# ---------------------------------------------------------------- 主体渲染
def render(small=False, size=1024):
    """small=True 时改用"小尺寸专用"版本：对比更强、细节更少、字母更大。"""
    W = size * SS
    img = vgrad((W, W), [(0.0, (33, 80, 158)), (0.26, (20, 48, 112)),
                         (0.62, (13, 30, 74)), (1.0, (7, 16, 42))])
    # 水彩晕染：左上湖蓝、右上天空、右侧珊瑚、左下珊瑚+蜜桃、右下深压
    img = Image.alpha_composite(img, blob((W, W), (-0.22 * W, -0.32 * W, 0.66 * W, 0.38 * W), AZURE, 0.09 * W, 195))
    img = Image.alpha_composite(img, blob((W, W), (0.36 * W, -0.38 * W, 1.24 * W, 0.34 * W), SKY, 0.10 * W, 165))
    img = Image.alpha_composite(img, blob((W, W), (0.60 * W, 0.24 * W, 1.30 * W, 0.90 * W), CORAL, 0.12 * W, 112))
    img = Image.alpha_composite(img, blob((W, W), (-0.28 * W, 0.54 * W, 0.38 * W, 1.24 * W), CORAL, 0.11 * W, 100))
    img = Image.alpha_composite(img, blob((W, W), (-0.14 * W, 0.82 * W, 0.50 * W, 1.34 * W), AMBER, 0.10 * W, 85))
    img = Image.alpha_composite(img, blob((W, W), (0.14 * W, 0.62 * W, 1.14 * W, 1.32 * W), NAVY_DEEP, 0.10 * W, 180))

    if small:
        img = Image.alpha_composite(img, grooves((W, W), W / 2, W / 2, W * 0.470, W * 0.20,
                                                 W * 0.030, (206, 234, 255), 30, 12, max(1, int(W / 512))))
        img = Image.alpha_composite(img, taper_arc((W, W), [W * 0.05, W * 0.05, W * 0.95, W * 0.95],
                                                   -84, -2, CORAL, W * 0.055, W * 0.030, 240,
                                                   0.35, 0.0, 5, 0.0016, 0))
        m = monogram(W, "RL", FONT, 0.760, -0.075, -0.005)
        img = Image.alpha_composite(img, drop_shadow(m, (0, int(W * 0.026)), W * 0.020, (3, 9, 24), 190))
        img = Image.alpha_composite(img, paint(W, m, (255, 255, 252), (255, 214, 180), -42, (150, 74, 44)))
    else:
        img = Image.alpha_composite(img, grooves((W, W), W / 2, W / 2, W * 0.466, W * 0.182,
                                                 W * 0.0166, (206, 234, 255), 40, 15, max(1, int(W / 512))))
        img = Image.alpha_composite(img, taper_arc((W, W), [W * 0.058, W * 0.058, W * 0.942, W * 0.942],
                                                   -84, -2, CORAL, W * 0.044, W * 0.012, 236,
                                                   0.50, 0.05, 5, 0.0016, 5))
        img = Image.alpha_composite(img, taper_arc((W, W), [W * 0.112, W * 0.112, W * 0.888, W * 0.888],
                                                   182, 258, MINT, W * 0.026, W * 0.007, 195,
                                                   0.58, 0.04, 9, 0.0016, 4))
        img = Image.alpha_composite(img, radial((W, W), (W * 0.5, W * 0.5), W * 0.44, (255, 176, 124), 0.14, 1.9))
        m = monogram(W, "RL", FONT, 0.650, -0.058, -0.004)
        img = Image.alpha_composite(img, drop_shadow(m, (0, int(W * 0.022)), W * 0.018, (3, 9, 24), 175))
        img = Image.alpha_composite(img, paint(W, m, (255, 254, 249), (255, 206, 168)))

    img = Image.alpha_composite(img, radial((W, W), (W * 0.5, W * 0.5), W * 0.86, (3, 8, 22), 0.46, 2.6))
    rng = np.random.RandomState(7)
    a = np.asarray(img).astype(np.float32)
    a[..., :3] = np.clip(a[..., :3] + rng.randn(W, W, 1) * (2.5 if small else 3.5), 0, 255)
    img = Image.fromarray(a.astype(np.uint8), "RGBA")
    img.putalpha(squircle_mask(W, int(W * 0.2225)))
    return img.resize((size, size), Image.LANCZOS)


def pack_ico(frames, out_path):
    """frames: [(size, Image)] → 多帧 ICO（PNG 负载，Vista 起支持）。"""
    blobs = []
    for size, im in frames:
        buf = io.BytesIO()
        im.save(buf, format="PNG", optimize=True)
        blobs.append((size, buf.getvalue()))
    n = len(blobs)
    offset = 6 + 16 * n
    dirs, body = b"", b""
    for size, data in blobs:
        b = 0 if size >= 256 else size
        dirs += struct.pack("<BBBBHHII", b, b, 0, 0, 1, 32, len(data), offset)
        offset += len(data)
        body += data
    with open(out_path, "wb") as f:
        f.write(struct.pack("<HHH", 0, 1, n) + dirs + body)
    return n, 6 + 16 * n + len(body)


SIZES = (16, 20, 24, 32, 40, 48, 64, 96, 128, 256)


def main():
    master = render(False, 1024)
    small = render(True, 256)

    master.save(os.path.join(HERE, "rlplayer-rl-1024.png"), optimize=True)
    for s in (512, 256, 128, 64, 48):
        master.resize((s, s), Image.LANCZOS).save(
            os.path.join(HERE, "rlplayer-rl-%d.png" % s), optimize=True)
    for s in (32, 24, 16):
        small.resize((s, s), Image.LANCZOS).save(
            os.path.join(HERE, "rlplayer-rl-%d.png" % s), optimize=True)
    small.resize((256, 256), Image.LANCZOS).save(
        os.path.join(HERE, "rlplayer-rl-small-src-256.png"), optimize=True)

    frames = [(s, (small if s <= 32 else master).resize((s, s), Image.LANCZOS)) for s in SIZES]
    n, total = pack_ico(frames, os.path.join(HERE, "rlplayer-rl.ico"))
    print("rlplayer-rl.ico: %d 帧, %d 字节" % (n, total))


if __name__ == "__main__":
    main()
