
# -*- coding: utf-8 -*-
"""RLPlayer application icon.

Style anchor: the uploaded watercolour vinyl artwork - navy disc + painted
grooves, azure/coral/peach washes, cream type.  The mark itself is the
"RL" monogram (R = Rip / Read, L = Lossless / Listen), set in a heavy
grotesque and carrying the same cream-to-peach paint as the artwork.

Rendered at 4x and downsampled so the 16px frame stays clean.
"""
import numpy as np, os, math
from PIL import Image, ImageDraw, ImageFilter, ImageFont, ImageChops

OUT  = r"D:\RTA-hqplayer\res\icon-lab"
FONT = r"C:\Windows\Fonts\ariblk.ttf"
SS   = 4

NAVY_DEEP=(8,18,48); AZURE=(48,132,226); SKY=(104,196,248)
CORAL=(246,98,104); PEACH=(255,170,118); AMBER=(255,205,130)
CREAM=(255,250,242); MINT=(122,228,226); INK=(11,20,46)

# ------------------------------------------------------------------ helpers
def vgrad(size, stops):
    w,h=size; t=np.linspace(0,1,h); out=np.zeros((h,3))
    for i in range(len(stops)-1):
        p0,c0=stops[i]; p1,c1=stops[i+1]; m=(t>=p0)&(t<=p1)
        if m.any():
            k=((t[m]-p0)/max(1e-6,p1-p0))[:,None]
            out[m]=np.array(c0,float)*(1-k)+np.array(c1,float)*k
    out[t<stops[0][0]]=stops[0][1]; out[t>stops[-1][0]]=stops[-1][1]
    return Image.fromarray(np.repeat(out[:,None,:],w,axis=1).astype(np.uint8),"RGB").convert("RGBA")

def dgrad(size, stops, angle=-42.0, box=None):
    w,h=size
    yy,xx=np.mgrid[0:h,0:w].astype(np.float64)
    a=math.radians(angle); p=xx*math.cos(a)+yy*math.sin(a)
    lo,hi=float(p.min()),float(p.max())
    if box is not None:
        x0,y0,x1,y1=box
        q0=x0*math.cos(a)+y0*math.sin(a); q1=x1*math.cos(a)+y1*math.sin(a)
        lo,hi=min(q0,q1),max(q0,q1)
    p=np.clip((p-lo)/max(1e-6,hi-lo),0,1)
    out=np.zeros((h,w,3))
    for i in range(len(stops)-1):
        p0,c0=stops[i]; p1,c1=stops[i+1]; m=(p>=p0)&(p<=p1)
        if m.any():
            k=((p[m]-p0)/max(1e-6,p1-p0))[:,None]
            out[m]=np.array(c0,float)*(1-k)+np.array(c1,float)*k
    out[p<stops[0][0]]=stops[0][1]; out[p>stops[-1][0]]=stops[-1][1]
    return Image.fromarray(np.clip(out,0,255).astype(np.uint8),"RGB").convert("RGBA")

def blob(size, box, color, blur, alpha):
    lay=Image.new("RGBA",size,(0,0,0,0))
    ImageDraw.Draw(lay).ellipse(box, fill=tuple(color)+(int(alpha),))
    return lay.filter(ImageFilter.GaussianBlur(blur))

def radial(size, center, radius, color, alpha=1.0, power=2.0):
    w,h=size; yy,xx=np.mgrid[0:h,0:w]
    d=np.sqrt((xx-center[0])**2+(yy-center[1])**2)/radius
    a=np.clip(1-d,0,1)**power*alpha
    arr=np.zeros((h,w,4),np.uint8)
    arr[...,0],arr[...,1],arr[...,2]=color
    arr[...,3]=(a*255).astype(np.uint8)
    return Image.fromarray(arr,"RGBA")

def squircle_mask(size,radius):
    m=Image.new("L",(size,size),0)
    ImageDraw.Draw(m).rounded_rectangle([0,0,size-1,size-1],radius=radius,fill=255)
    return m

def drop_shadow(mask,offset,blur,color,alpha):
    solid=Image.new("RGBA",mask.size,tuple(color)+(int(alpha),))
    lay=Image.new("RGBA",mask.size,(0,0,0,0)); lay.paste(solid,(0,0),mask)
    lay=lay.filter(ImageFilter.GaussianBlur(blur))
    out=Image.new("RGBA",mask.size,(0,0,0,0)); out.paste(lay,offset,lay)
    return out

def grooves(size,cx,cy,r_out,r_in,step,color,amaj,amin,width):
    lay=Image.new("RGBA",size,(0,0,0,0)); d=ImageDraw.Draw(lay); r=r_out; i=0
    while r>r_in:
        d.ellipse([cx-r,cy-r,cx+r,cy+r],
                  outline=tuple(color)+((amaj if i%6==0 else amin),), width=width)
        r-=step; i+=1
    return lay

def glyphs(size, txt, font_path, target_w, tracking_ratio, y_bias=0.0):
    """Return (whole mask, [(char, mask, x0, x1), ...]) so a glyph can be
    painted independently while keeping the original spacing."""
    W=size; big=W*2
    d=ImageDraw.Draw(Image.new("L",(8,8)))
    f=ImageFont.truetype(font_path,int(big*0.4))
    ws=[d.textbbox((0,0),c,font=f)[2]-d.textbbox((0,0),c,font=f)[0] for c in txt]
    total=sum(ws)+int(big*0.4*tracking_ratio)*(len(txt)-1)
    fs=int(big*0.4*target_w*W/total)
    f=ImageFont.truetype(font_path,fs); tr=int(fs*tracking_ratio)
    ws=[d.textbbox((0,0),c,font=f)[2]-d.textbbox((0,0),c,font=f)[0] for c in txt]
    total=sum(ws)+tr*(len(txt)-1)
    asc,desc=f.getmetrics()
    tmp=Image.new("L",(big,big),0); td=ImageDraw.Draw(tmp)
    x=(big-total)/2.0; base_y=big/2.0+(asc-(asc+desc)/2.0)
    spans=[]
    for c,wc in zip(txt,ws):
        td.text((x,base_y),c,font=f,fill=255,anchor="ls")
        spans.append((c,x,x+wc)); x+=wc+tr
    bb=tmp.getbbox()
    ox=int(W/2-(bb[0]+(bb[2]-bb[0])/2)); oy=int(W/2-(bb[1]+(bb[3]-bb[1])/2)+y_bias*W)
    whole=Image.new("L",(W,W),0); whole.paste(tmp,(ox,oy))
    parts=[]
    for (c,x0,x1) in spans:
        pm=Image.new("L",(W,W),0); pm.paste(tmp.crop((int(x0)+ox,0,int(x1)+ox+1,big)),(int(x0)+ox,oy))
        parts.append((c,pm,int(x0)+ox,int(x1)+ox))
    return whole, parts

def taper_arc(size, box, start, end, color, w_max, w_min, alpha, taper=0.62, jitter=0.0, seed=0):
    W=size[0] if isinstance(size,(tuple,list)) else size
    x0,y0,x1,y1=box
    cx,cy=(x0+x1)/2,(y0+y1)/2; rx,ry=(x1-x0)/2,(y1-y0)/2
    n=420; rng=np.random.RandomState(seed); pts=[]
    for i in range(n+1):
        k=i/n; e=math.sin(math.pi*k)**taper
        w=w_min+(w_max-w_min)*e
        if jitter: w*=max(0.35,1.0+jitter*rng.randn())
        t=math.radians(start+(end-start)*k)
        px,py=cx+rx*math.cos(t), cy+ry*math.sin(t)
        nx,ny=math.cos(t)/max(rx,1e-6), math.sin(t)/max(ry,1e-6)
        L=math.hypot(nx,ny); nx,ny=nx/L,ny/L
        pts.append((px,py,w,nx,ny))
    poly=[(px+nx*w/2,py+ny*w/2) for (px,py,w,nx,ny) in pts]
    poly+= [(px-nx*w/2,py-ny*w/2) for (px,py,w,nx,ny) in reversed(pts)]
    lay=Image.new("RGBA",(int(W),int(W)),(0,0,0,0))
    ImageDraw.Draw(lay).polygon([(float(a),float(b)) for a,b in poly], fill=tuple(color)+(int(alpha),))
    return lay.filter(ImageFilter.GaussianBlur(W*0.0030))

def paint(size, mask, top, bottom, angle=-42, warm_shade=(150,70,40)):
    W=size; bb=mask.getbbox()
    fill=dgrad((W,W),[(0.0,top),(1.0,bottom)],angle=angle,box=bb)
    lay=Image.new("RGBA",(W,W),(0,0,0,0)); lay.paste(fill,(0,0),mask)
    hi=ImageChops.subtract(ImageChops.offset(mask,-max(1,int(W*0.004)),-max(1,int(W*0.004))), mask)
    hi=hi.filter(ImageFilter.GaussianBlur(W*0.0035)).point(lambda v:int(v*0.70))
    lo=ImageChops.subtract(ImageChops.offset(mask,max(1,int(W*0.005)),max(1,int(W*0.005))), mask)
    lo=lo.filter(ImageFilter.GaussianBlur(W*0.004)).point(lambda v:int(v*0.45))
    hlay=Image.new("RGBA",(W,W),(255,255,255,0)); hlay.putalpha(hi)
    llay=Image.new("RGBA",(W,W),tuple(warm_shade)+(0,)); llay.putalpha(lo)
    return Image.alpha_composite(lay, Image.alpha_composite(llay, hlay))

# ------------------------------------------------------------------ scenes
def disc_bg(W):
    img=vgrad((W,W),[(0.0,(30,74,150)),(0.26,(19,46,108)),(0.62,(13,30,74)),(1.0,(7,16,42))])
    img=Image.alpha_composite(img, blob((W,W),(-0.22*W,-0.32*W,0.66*W,0.38*W), AZURE, 0.09*W, 190))
    img=Image.alpha_composite(img, blob((W,W),(0.36*W,-0.38*W,1.24*W,0.34*W), SKY, 0.10*W, 160))
    img=Image.alpha_composite(img, blob((W,W),(0.60*W,0.24*W,1.30*W,0.90*W), CORAL, 0.12*W, 105))
    img=Image.alpha_composite(img, blob((W,W),(-0.28*W,0.54*W,0.38*W,1.24*W), CORAL, 0.11*W, 95))
    img=Image.alpha_composite(img, blob((W,W),(-0.14*W,0.82*W,0.50*W,1.34*W), AMBER, 0.10*W, 80))
    img=Image.alpha_composite(img, blob((W,W),(0.14*W,0.62*W,1.14*W,1.32*W), NAVY_DEEP, 0.10*W, 175))
    img=Image.alpha_composite(img, grooves((W,W),W/2,W/2,W*0.468,W*0.180,W*0.0168,(202,232,255),56,22,max(1,int(W/512))))
    img=Image.alpha_composite(img, taper_arc((W,W),[W*0.070,W*0.070,W*0.930,W*0.930],-80,-4,CORAL,W*0.036,W*0.007,232,0.62,0.06,5))
    img=Image.alpha_composite(img, taper_arc((W,W),[W*0.120,W*0.120,W*0.880,W*0.880],186,258,MINT,W*0.021,W*0.005,195,0.70,0.05,9))
    return img

def cream_bg(W):
    img=vgrad((W,W),[(0.0,(255,253,247)),(0.55,(250,243,231)),(1.0,(236,226,210))])
    img=Image.alpha_composite(img, blob((W,W),(-0.12*W,-0.26*W,0.72*W,0.28*W), CORAL, 0.10*W, 46))
    img=Image.alpha_composite(img, blob((W,W),(0.42*W,0.60*W,1.18*W,1.32*W), MINT, 0.11*W, 44))
    img=Image.alpha_composite(img, blob((W,W),(0.50*W,-0.22*W,1.22*W,0.40*W), AMBER, 0.11*W, 58))
    img=Image.alpha_composite(img, grooves((W,W),W/2,W/2,W*0.470,W*0.180,W*0.0170,INK,24,12,max(1,int(W/440))))
    img=Image.alpha_composite(img, taper_arc((W,W),[W*0.070,W*0.070,W*0.930,W*0.930],-80,-4,CORAL,W*0.034,W*0.007,225,0.62,0.05,5))
    img=Image.alpha_composite(img, taper_arc((W,W),[W*0.120,W*0.120,W*0.880,W*0.880],186,258,AZURE,W*0.020,W*0.005,185,0.70,0.05,9))
    return img

def finish(img,W,grain=4.5):
    if grain:
        rng=np.random.RandomState(7)
        n=rng.randn(W,W,1)*grain
        a=np.asarray(img).astype(np.float32)
        a[...,:3]=np.clip(a[...,:3]+n,0,255)
        img=Image.fromarray(a.astype(np.uint8),"RGBA")
    img.putalpha(squircle_mask(W,int(W*0.2225)))
    return img.resize((W//SS,W//SS), Image.LANCZOS)

# ------------------------------------------------------------------ variants
def variant(kind, size=1024):
    W=size*SS
    if kind=="A":
        img=disc_bg(W)
        img=Image.alpha_composite(img, radial((W,W),(W*0.5,W*0.5),W*0.42,(255,170,116),0.15,1.9))
        m,_=glyphs(W,"RL",FONT,0.640,-0.058,-0.004)
        img=Image.alpha_composite(img, drop_shadow(m,(0,int(W*0.021)),W*0.017,(3,9,24),170))
        img=Image.alpha_composite(img, paint(W,m,(255,254,248),(255,188,142),-42))
        img=Image.alpha_composite(img, radial((W,W),(W*0.5,W*0.5),W*0.85,(3,8,22),0.50,2.6))
        return finish(img,W)

    if kind=="B":
        img=disc_bg(W)
        R=W*0.315
        dm=Image.new("L",(W,W),0); ImageDraw.Draw(dm).ellipse([W/2-R,W/2-R,W/2+R,W/2+R],fill=255)
        img=Image.alpha_composite(img, drop_shadow(dm,(0,int(W*0.013)),W*0.022,(3,9,24),155))
        disc=Image.new("RGBA",(W,W),(0,0,0,0))
        disc.paste(dgrad((W,W),[(0.0,(255,234,184)),(0.40,(255,188,130)),(0.76,(250,138,128)),(1.0,(230,98,116))],
                         -38, box=(W/2-R,W/2-R,W/2+R,W/2+R)),(0,0),dm)
        img=Image.alpha_composite(img, disc)
        m,_=glyphs(W,"RL",FONT,0.520,-0.055,-0.003)
        img=Image.alpha_composite(img, drop_shadow(m,(0,int(W*0.008)),W*0.012,(70,20,24),130))
        img=Image.alpha_composite(img, paint(W,m,(32,46,96),(9,16,40),-40))
        img=Image.alpha_composite(img, radial((W,W),(W*0.5,W*0.5),W*0.88,(3,8,22),0.40,2.6))
        return finish(img,W,4.0)

    if kind=="C":     # two-tone monogram: R coral/peach, L cream
        img=disc_bg(W)
        img=Image.alpha_composite(img, radial((W,W),(W*0.5,W*0.5),W*0.42,(255,170,116),0.14,1.9))
        m,parts=glyphs(W,"RL",FONT,0.640,-0.058,-0.004)
        img=Image.alpha_composite(img, drop_shadow(m,(0,int(W*0.021)),W*0.017,(3,9,24),170))
        for i,(c,pm,x0,x1) in enumerate(parts):
            if c=="R":
                img=Image.alpha_composite(img, paint(W,pm,(255,226,182),(248,116,112),-42))
            else:
                img=Image.alpha_composite(img, paint(W,pm,(255,254,248),(190,232,246),-42))
        img=Image.alpha_composite(img, radial((W,W),(W*0.5,W*0.5),W*0.85,(3,8,22),0.50,2.6))
        return finish(img,W)

    if kind=="D":     # cream plate (light / taskbar-friendly inverse)
        img=cream_bg(W)
        img=Image.alpha_composite(img, radial((W,W),(W*0.5,W*0.5),W*0.60,(255,255,255),0.35,1.6))
        m,_=glyphs(W,"RL",FONT,0.640,-0.058,-0.004)
        img=Image.alpha_composite(img, drop_shadow(m,(0,int(W*0.018)),W*0.016,(140,110,90),120))
        img=Image.alpha_composite(img, paint(W,m,(44,74,140),(10,20,50),-42,warm_shade=(20,30,60)))
        img=Image.alpha_composite(img, radial((W,W),(W*0.5,W*0.5),W*0.95,(200,180,150),0.30,3.0))
        return finish(img,W,3.0)

    if kind=="E":     # monogram only, no plate (transparent) for tray/in-app
        img=disc_bg(W)
        m,_=glyphs(W,"RL",FONT,0.640,-0.058,-0.004)
        img=Image.alpha_composite(img, drop_shadow(m,(0,int(W*0.021)),W*0.017,(3,9,24),170))
        img=Image.alpha_composite(img, paint(W,m,(255,254,248),(255,188,142),-42))
        return img

    raise ValueError(kind)

if __name__=="__main__":
    os.makedirs(OUT,exist_ok=True)
    made={}
    for k in "ABCD":
        v=variant(k,1024); made[k]=v; v.save(os.path.join(OUT,f"v6{k}_1024.png"))

    sheet=Image.new("RGB",(1500,880),"#EDE9E1"); d=ImageDraw.Draw(sheet)
    f=ImageFont.truetype(r"C:\Windows\Fonts\segoeuib.ttf",26)
    labels={"A":"A · cream RL / navy disc","B":"B · navy RL / warm label","C":"C · two-tone R+L","D":"D · light plate / navy RL"}
    for i,k in enumerate("ABCD"):
        y=26+i*212
        d.text((26,y), labels[k], font=f, fill="#222")
        big=made[k].resize((176,176),Image.LANCZOS); sheet.paste(big,(26,y+34),big)
        x=230
        for sz in (128,96,64,48,32,24,16):
            im=made[k].resize((sz,sz),Image.LANCZOS); sheet.paste(im,(x,y+34+(176-sz)//2),im); x+=sz+28
    sheet.save(os.path.join(OUT,"v6_sheet.png"))
    print("ok")
