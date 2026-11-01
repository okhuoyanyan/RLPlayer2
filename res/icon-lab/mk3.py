
# -*- coding: utf-8 -*-
import numpy as np, os, math
from PIL import Image, ImageDraw, ImageFilter, ImageFont

OUT = r"D:\RTA-hqplayer\res\icon-lab"
FONT = r"C:\Windows\Fonts\ariblk.ttf"

NAVY_DEEP = (8, 18, 48)
AZURE     = (48, 132, 226)
SKY       = (104, 196, 248)
CORAL     = (246, 98, 104)
PEACH     = (255, 170, 118)
AMBER     = (255, 200, 120)
CREAM     = (255, 248, 236)
MINT      = (122, 228, 226)

def vgrad(size, stops):
    w, h = size
    t = np.linspace(0, 1, h); out = np.zeros((h, 3))
    for i in range(len(stops)-1):
        p0,c0 = stops[i]; p1,c1 = stops[i+1]
        m = (t>=p0)&(t<=p1)
        if m.any():
            k = ((t[m]-p0)/max(1e-6,p1-p0))[:,None]
            out[m] = np.array(c0,float)*(1-k)+np.array(c1,float)*k
    out[t<stops[0][0]] = stops[0][1]; out[t>stops[-1][0]] = stops[-1][1]
    return Image.fromarray(np.repeat(out[:,None,:], w, axis=1).astype(np.uint8), "RGB").convert("RGBA")

def dgrad(size, stops, angle=-35.0):
    w,h = size
    yy,xx = np.mgrid[0:h,0:w].astype(np.float64)
    a = math.radians(angle)
    p = xx*math.cos(a)+yy*math.sin(a)
    p = (p-p.min())/(p.max()-p.min())
    out = np.zeros((h,w,3))
    for i in range(len(stops)-1):
        p0,c0 = stops[i]; p1,c1 = stops[i+1]
        m = (p>=p0)&(p<=p1)
        if m.any():
            k = ((p[m]-p0)/max(1e-6,p1-p0))[:,None]
            out[m] = np.array(c0,float)*(1-k)+np.array(c1,float)*k
    out[p<stops[0][0]] = stops[0][1]; out[p>stops[-1][0]] = stops[-1][1]
    return Image.fromarray(out.astype(np.uint8), "RGB").convert("RGBA")

def blob(size, box, color, blur, alpha):
    lay = Image.new("RGBA", size, (0,0,0,0))
    ImageDraw.Draw(lay).ellipse(box, fill=tuple(color)+(int(alpha),))
    return lay.filter(ImageFilter.GaussianBlur(blur))

def radial(size, center, radius, color, alpha=1.0, power=2.0):
    w,h = size
    yy,xx = np.mgrid[0:h,0:w]
    d = np.sqrt((xx-center[0])**2+(yy-center[1])**2)/radius
    a = np.clip(1-d,0,1)**power*alpha
    arr = np.zeros((h,w,4),np.uint8)
    arr[...,0],arr[...,1],arr[...,2] = color
    arr[...,3] = (a*255).astype(np.uint8)
    return Image.fromarray(arr,"RGBA")

def squircle(size, radius):
    m = Image.new("L",(size,size),0)
    ImageDraw.Draw(m).rounded_rectangle([0,0,size-1,size-1], radius=radius, fill=255)
    return m

def shadow(mask, offset, blur, color, alpha):
    solid = Image.new("RGBA", mask.size, tuple(color)+(int(alpha),))
    lay = Image.new("RGBA", mask.size, (0,0,0,0)); lay.paste(solid,(0,0),mask)
    lay = lay.filter(ImageFilter.GaussianBlur(blur))
    out = Image.new("RGBA", mask.size, (0,0,0,0)); out.paste(lay, offset, lay)
    return out

def ring(size, cx, cy, r, width, color, alpha, blur=0):
    lay = Image.new("RGBA",(size,size),(0,0,0,0))
    ImageDraw.Draw(lay).ellipse([cx-r,cy-r,cx+r,cy+r], outline=tuple(color)+(int(alpha),), width=int(width))
    return lay.filter(ImageFilter.GaussianBlur(blur)) if blur else lay

def grooves(size, cx, cy, r_out, r_in, step, color, amaj, amin, width):
    lay = Image.new("RGBA",(size,size),(0,0,0,0))
    d = ImageDraw.Draw(lay); r=r_out; i=0
    while r>r_in:
        d.ellipse([cx-r,cy-r,cx+r,cy+r], outline=tuple(color)+((amaj if i%6==0 else amin),), width=width)
        r-=step; i+=1
    return lay

def mono(size, txt, font_path, target_w, tracking, y_bias=0.0):
    W=size; big=W*3
    f = ImageFont.truetype(font_path, int(big*0.5))
    d = ImageDraw.Draw(Image.new("L",(8,8)))
    ws = [d.textbbox((0,0),c,font=f)[2]-d.textbbox((0,0),c,font=f)[0] for c in txt]
    total = sum(ws)+int(big*0.5*tracking)*(len(txt)-1)
    fs = int(big*0.5*target_w*W/total)
    f = ImageFont.truetype(font_path, fs)
    tr = int(fs*tracking)
    bb0 = [d.textbbox((0,0),c,font=f) for c in txt]
    ws = [b[2]-b[0] for b in bb0]
    total = sum(ws)+tr*(len(txt)-1)
    asc,desc = f.getmetrics()
    tmp = Image.new("L",(big,big),0); td = ImageDraw.Draw(tmp)
    x = (big-total)/2.0
    base_y = big/2.0 + (asc-(asc+desc)/2.0)
    for c,wc in zip(txt,ws):
        td.text((x, base_y), c, font=f, fill=255, anchor="ls")
        x += wc + tr
    bb = tmp.getbbox()
    ox = int(W/2-(bb[0]+(bb[2]-bb[0])/2)); oy = int(W/2-(bb[1]+(bb[3]-bb[1])/2)+y_bias*W)
    o = Image.new("L",(W,W),0); o.paste(tmp,(ox,oy))
    return o

def base_bg(W, vivid=1.0):
    img = vgrad((W,W), [(0.0,(30,72,148)),(0.30,(18,44,104)),(0.66,(13,30,74)),(1.0,(8,18,46))])
    img = Image.alpha_composite(img, blob((W,W), (-0.18*W,-0.28*W,0.70*W,0.42*W), AZURE, 0.10*W, 175*vivid))
    img = Image.alpha_composite(img, blob((W,W), (0.42*W,-0.34*W,1.20*W,0.38*W), SKY, 0.11*W, 135*vivid))
    img = Image.alpha_composite(img, blob((W,W), (0.55*W,0.30*W,1.25*W,0.95*W), CORAL, 0.13*W, 90*vivid))
    img = Image.alpha_composite(img, blob((W,W), (-0.24*W,0.58*W,0.44*W,1.24*W), CORAL, 0.12*W, 70*vivid))
    img = Image.alpha_composite(img, blob((W,W), (-0.10*W,0.78*W,0.55*W,1.30*W), AMBER, 0.10*W, 60*vivid))
    img = Image.alpha_composite(img, blob((W,W), (0.18*W,0.66*W,1.10*W,1.28*W), NAVY_DEEP, 0.10*W, 150))
    return img

def letters_layer(W, mask, stops, angle=-40):
    fill = dgrad((W,W), stops, angle=angle)
    lay = Image.new("RGBA",(W,W),(0,0,0,0))
    lay.paste(fill,(0,0),mask)
    # inner shading: darken bottom, brighten top
    shade = vgrad((W,W), [(0.0,(0,0,0)),(0.45,(0,0,0)),(1.0,(120,60,40))]).convert("RGBA")
    sh = Image.new("RGBA",(W,W),(0,0,0,0)); sh.paste(shade,(0,0),mask)
    lay = Image.alpha_composite(sh, lay)
    # rim light from upper-left
    lit = Image.new("RGBA",(W,W),(0,0,0,0))
    lit.paste(Image.new("RGBA",(W,W),(255,255,255,150)),(0,0),mask)
    lit = lit.filter(ImageFilter.GaussianBlur(W*0.006))
    inv = Image.eval(mask, lambda v: 255-v).filter(ImageFilter.GaussianBlur(W*0.004))
    band = Image.eval(inv, lambda v: v)
    lit.putalpha(Image.composite(lit.getchannel("A"), Image.new("L",(W,W),0), mask.point(lambda v:255 if v>0 else 0)))
    lit = Image.alpha_composite(lit, Image.new("RGBA",(W,W),(0,0,0,0)))
    return Image.alpha_composite(lay, Image.blend(Image.new("RGBA",(W,W),(0,0,0,0)), lit, 0.35))

def variant_A(size=1024):
    W=size
    img = base_bg(W, 1.0)
    img = Image.alpha_composite(img, grooves(W, W*0.5, W*0.5, W*0.470, W*0.180, W*0.0165, (200,230,255), 62, 26, max(1,int(W/512))))
    # accent arc coral top-right
    acc = Image.new("RGBA",(W,W),(0,0,0,0)); d=ImageDraw.Draw(acc)
    d.arc([W*0.105,W*0.105,W*0.895,W*0.895], start=-76, end=-6, fill=CORAL+(230,), width=int(W*0.029))
    d.arc([W*0.152,W*0.152,W*0.848,W*0.848], start=190, end=252, fill=MINT+(205,), width=int(W*0.019))
    acc = acc.filter(ImageFilter.GaussianBlur(W*0.005))
    img = Image.alpha_composite(img, acc)
    img = Image.alpha_composite(img, radial((W,W),(W*0.52,W*0.46),W*0.42,(255,165,110),0.28,1.8))
    mask = mono(W,"RL",FONT,0.640,-0.055,-0.004)
    img = Image.alpha_composite(img, shadow(mask,(0,int(W*0.018)),W*0.016,(4,10,26),150))
    img = Image.alpha_composite(img, letters_layer(W, mask, [(0.0,CREAM),(0.42,(255,232,196)),(0.78,(255,186,132)),(1.0,(247,140,116))], -42))
    img = Image.alpha_composite(img, radial((W,W),(W*0.5,W*0.5),W*0.82,(3,8,22),0.55,2.6))
    img.putalpha(squircle(W,int(W*0.2225)))
    return img

def variant_B(size=1024):
    """amber 'label' disc + navy RL"""
    W=size
    img = base_bg(W, 1.0)
    img = Image.alpha_composite(img, grooves(W, W*0.5, W*0.5, W*0.470, W*0.230, W*0.0160, (200,230,255), 62, 26, max(1,int(W/512))))
    acc = Image.new("RGBA",(W,W),(0,0,0,0)); d=ImageDraw.Draw(acc)
    d.arc([W*0.105,W*0.105,W*0.895,W*0.895], start=-76, end=-6, fill=CORAL+(230,), width=int(W*0.029))
    d.arc([W*0.152,W*0.152,W*0.848,W*0.848], start=190, end=252, fill=MINT+(205,), width=int(W*0.019))
    acc = acc.filter(ImageFilter.GaussianBlur(W*0.005))
    img = Image.alpha_composite(img, acc)
    # label disc
    R = W*0.300
    disc = Image.new("RGBA",(W,W),(0,0,0,0))
    dm = Image.new("L",(W,W),0); ImageDraw.Draw(dm).ellipse([W/2-R,W/2-R,W/2+R,W/2+R], fill=255)
    disc.paste(dgrad((W,W),[(0.0,(255,214,140)),(0.45,(255,170,118)),(1.0,(244,110,112))],-40),(0,0),dm)
    img = Image.alpha_composite(img, shadow(dm,(0,int(W*0.012)),W*0.022,(4,10,26),150))
    img = Image.alpha_composite(img, disc)
    img = Image.alpha_composite(img, ring(W,W/2,W/2,R,int(W*0.004),(255,255,255),70,W*0.002))
    mask = mono(W,"RL",FONT,0.500,-0.055,-0.004)
    img = Image.alpha_composite(img, shadow(mask,(0,int(W*0.008)),W*0.010,(60,20,20),110))
    img = Image.alpha_composite(img, letters_layer(W, mask, [(0.0,(24,34,74)),(1.0,(10,18,44))], -40))
    img = Image.alpha_composite(img, radial((W,W),(W*0.5,W*0.5),W*0.86,(3,8,22),0.42,2.6))
    img.putalpha(squircle(W,int(W*0.2225)))
    return img

def variant_C(size=1024):
    """big cream RL, single coral comet arc, cleaner"""
    W=size
    img = base_bg(W, 1.05)
    img = Image.alpha_composite(img, grooves(W, W*0.5, W*0.5, W*0.470, W*0.175, W*0.0170, (205,232,255), 55, 22, max(1,int(W/512))))
    acc = Image.new("RGBA",(W,W),(0,0,0,0)); d=ImageDraw.Draw(acc)
    d.arc([W*0.095,W*0.095,W*0.905,W*0.905], start=-72, end=4, fill=CORAL+(235,), width=int(W*0.034))
    acc = acc.filter(ImageFilter.GaussianBlur(W*0.006))
    img = Image.alpha_composite(img, acc)
    img = Image.alpha_composite(img, radial((W,W),(W*0.5,W*0.5),W*0.44,(255,170,115),0.30,1.7))
    mask = mono(W,"RL",FONT,0.665,-0.060,-0.004)
    img = Image.alpha_composite(img, shadow(mask,(0,int(W*0.020)),W*0.018,(3,9,24),160))
    img = Image.alpha_composite(img, letters_layer(W, mask, [(0.0,(255,252,244)),(0.50,(255,239,214)),(1.0,(255,196,150))], -42))
    img = Image.alpha_composite(img, radial((W,W),(W*0.5,W*0.5),W*0.82,(3,8,22),0.52,2.6))
    img.putalpha(squircle(W,int(W*0.2225)))
    return img

os.makedirs(OUT, exist_ok=True)
imgs = {"A": variant_A(1024), "B": variant_B(1024), "C": variant_C(1024)}
for k,v in imgs.items(): v.save(os.path.join(OUT, f"v3{k}_1024.png"))

sheet = Image.new("RGB",(1560,700),"#EFEBE3")
d = ImageDraw.Draw(sheet)
fsmall = ImageFont.truetype(r"C:\Windows\Fonts\segoeuib.ttf", 28)
x = 30
for k,v in imgs.items():
    big = v.resize((380,380), Image.LANCZOS); sheet.paste(big,(x,60),big)
    d.text((x+8,20), f"Variant {k}", font=fsmall, fill="#222")
    xx = x+20
    for sz in (96,48,32,24,16):
        im = v.resize((sz,sz), Image.LANCZOS)
        sheet.paste(im,(xx,470),im); xx += sz+22
    x += 500
sheet.save(os.path.join(OUT,"v3_sheet.png"))
print("ok")
