
# -*- coding: utf-8 -*-
"""RLPlayer icon - refinements.

Two changes over v6:
  * brush arcs get round caps and a softer edge (they read as paint, not laser)
  * the warm label disc (variant B) no longer collides with the monogram
  * variant C paints R and L separately with correct glyph masks
"""
import numpy as np, os, math
from PIL import Image, ImageDraw, ImageFilter, ImageFont, ImageChops

OUT  = r"D:\RTA-hqplayer\res\icon-lab"
FONT = r"C:\Windows\Fonts\ariblk.ttf"
SS   = 4

NAVY_DEEP=(8,18,48); AZURE=(48,132,226); SKY=(104,196,248)
CORAL=(246,98,104); PEACH=(255,176,124); AMBER=(255,208,138)
CREAM=(255,250,242); MINT=(122,228,226); INK=(11,20,46)

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

def grooves(size,cx,cy,r_out,r_in,step,color,amaj,amin,width,label_ring=None):
    lay=Image.new("RGBA",size,(0,0,0,0)); d=ImageDraw.Draw(lay); r=r_out; i=0
    while r>r_in:
        d.ellipse([cx-r,cy-r,cx+r,cy+r],
                  outline=tuple(color)+((amaj if i%6==0 else amin),), width=width)
        r-=step; i+=1
    if label_ring:
        d.ellipse([cx-label_ring,cy-label_ring,cx+label_ring,cy+label_ring],
                  outline=tuple(color)+(amaj+18,), width=int(width*1.6))
    return lay

def glyphs(size, txt, font_path, target_w, tracking_ratio, y_bias=0.0):
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
        spans.append((c,int(x),int(x+wc))); x+=wc+tr
    bb=tmp.getbbox()
    ox=int(W/2-(bb[0]+(bb[2]-bb[0])/2)); oy=int(W/2-(bb[1]+(bb[3]-bb[1])/2)+y_bias*W)
    whole=Image.new("L",(W,W),0); whole.paste(tmp,(ox,oy))
    # per-glyph masks: cut the *shifted* canvas by absolute x-range
    parts=[]
    for (c,x0,x1) in spans:
        cut=Image.new("L",(W,W),0)
        xa,xb=max(0,x0+ox),min(W,x1+ox+1)
        if xb>xa: cut.paste(whole.crop((xa,0,xb,W)),(xa,0))
        parts.append((c,cut))
    return whole, parts

def taper_arc(size, box, start, end, color, w_max, w_min, alpha, taper=0.62, jitter=0.0, seed=0, blur=0.0030):
    W=size[0] if isinstance(size,(tuple,list)) else size
    x0,y0,x1,y1=box
    cx,cy=(x0+x1)/2,(y0+y1)/2; rx,ry=(x1-x0)/2,(y1-y0)/2
    n=460; rng=np.random.RandomState(seed); pts=[]
    for i in range(n+1):
        k=i/n; e=math.sin(math.pi*k)**taper
        w=w_min+(w_max-w_min)*e
        if jitter: w*=max(0.45,1.0+jitter*rng.randn())
        t=math.radians(start+(end-start)*k)
        px,py=cx+rx*math.cos(t), cy+ry*math.sin(t)
        nx,ny=math.cos(t)/max(rx,1e-6), math.sin(t)/max(ry,1e-6)
        L=math.hypot(nx,ny); nx,ny=nx/L,ny/L
        pts.append((px,py,w,nx,ny))
    poly=[(px+nx*w/2,py+ny*w/2) for (px,py,w,nx,ny) in pts]
    poly+=[(px-nx*w/2,py-ny*w/2) for (px,py,w,nx,ny) in reversed(pts)]
    lay=Image.new("RGBA",(W,W),(0,0,0,0)); d=ImageDraw.Draw(lay)
    d.polygon(poly, fill=tuple(color)+(int(alpha),))
    # round caps
    for idx in (0,-1):
        px,py,w,nx,ny=pts[idx]
        d.ellipse([px-w/2,py-w/2,px+w/2,py+w/2], fill=tuple(color)+(int(alpha),))
    return lay.filter(ImageFilter.GaussianBlur(W*blur))

def paint(size, mask, top, bottom, angle=-42, warm_shade=(160,80,50)):
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

def disc_bg(W):
    img=vgrad((W,W),[(0.0,(32,78,156)),(0.26,(20,48,112)),(0.62,(13,30,74)),(1.0,(7,16,42))])
    img=Image.alpha_composite(img, blob((W,W),(-0.22*W,-0.32*W,0.66*W,0.38*W), AZURE, 0.09*W, 195))
    img=Image.alpha_composite(img, blob((W,W),(0.36*W,-0.38*W,1.24*W,0.34*W), SKY, 0.10*W, 165))
    img=Image.alpha_composite(img, blob((W,W),(0.60*W,0.24*W,1.30*W,0.90*W), CORAL, 0.12*W, 110))
    img=Image.alpha_composite(img, blob((W,W),(-0.28*W,0.54*W,0.38*W,1.24*W), CORAL, 0.11*W, 100))
    img=Image.alpha_composite(img, blob((W,W),(-0.14*W,0.82*W,0.50*W,1.34*W), AMBER, 0.10*W, 85))
    img=Image.alpha_composite(img, blob((W,W),(0.14*W,0.62*W,1.14*W,1.32*W), NAVY_DEEP, 0.10*W, 180))
    img=Image.alpha_composite(img, grooves((W,W),W/2,W/2,W*0.466,W*0.182,W*0.0166,(206,234,255),42,16,max(1,int(W/512))))
    img=Image.alpha_composite(img, taper_arc((W,W),[W*0.062,W*0.062,W*0.938,W*0.938],-82,-2,CORAL,W*0.042,W*0.013,232,0.55,0.05,5,0.0042))
    img=Image.alpha_composite(img, taper_arc((W,W),[W*0.115,W*0.115,W*0.885,W*0.885],184,258,MINT,W*0.024,W*0.008,190,0.62,0.05,9,0.0038))
    return img

def cream_bg(W):
    img=vgrad((W,W),[(0.0,(255,253,247)),(0.55,(250,242,229)),(1.0,(238,228,211))])
    img=Image.alpha_composite(img, blob((W,W),(-0.14*W,-0.28*W,0.68*W,0.26*W), CORAL, 0.11*W, 40))
    img=Image.alpha_composite(img, blob((W,W),(0.44*W,0.62*W,1.20*W,1.34*W), MINT, 0.12*W, 38))
    img=Image.alpha_composite(img, blob((W,W),(0.52*W,-0.24*W,1.24*W,0.38*W), AMBER, 0.12*W, 52))
    img=Image.alpha_composite(img, grooves((W,W),W/2,W/2,W*0.470,W*0.182,W*0.0170,INK,20,10,max(1,int(W/440))))
    img=Image.alpha_composite(img, taper_arc((W,W),[W*0.062,W*0.062,W*0.938,W*0.938],-82,-2,CORAL,W*0.040,W*0.012,220,0.55,0.05,5,0.0042))
    img=Image.alpha_composite(img, taper_arc((W,W),[W*0.115,W*0.115,W*0.885,W*0.885],184,258,AZURE,W*0.023,W*0.007,180,0.62,0.05,9,0.0038))
    return img

def finish(img,W,grain=4.0):
    if grain:
        rng=np.random.RandomState(7)
        n=rng.randn(W,W,1)*grain
        a=np.asarray(img).astype(np.float32)
        a[...,:3]=np.clip(a[...,:3]+n,0,255)
        img=Image.fromarray(a.astype(np.uint8),"RGBA")
    img.putalpha(squircle_mask(W,int(W*0.2225)))
    return img.resize((W//SS,W//SS), Image.LANCZOS)

def variant(kind, size=1024):
    W=size*SS
    if kind=="A":
        img=disc_bg(W)
        img=Image.alpha_composite(img, radial((W,W),(W*0.5,W*0.5),W*0.44,(255,176,124),0.14,1.9))
        m,_=glyphs(W,"RL",FONT,0.648,-0.058,-0.004)
        img=Image.alpha_composite(img, drop_shadow(m,(0,int(W*0.022)),W*0.018,(3,9,24),175))
        img=Image.alpha_composite(img, paint(W,m,(255,254,249),(255,206,168),-42,(150,74,44)))
        img=Image.alpha_composite(img, radial((W,W),(W*0.5,W*0.5),W*0.86,(3,8,22),0.48,2.6))
        return finish(img,W)

    if kind=="B":
        img=disc_bg(W)
        R=W*0.330
        dm=Image.new("L",(W,W),0); ImageDraw.Draw(dm).ellipse([W/2-R,W/2-R,W/2+R,W/2+R],fill=255)
        img=Image.alpha_composite(img, drop_shadow(dm,(0,int(W*0.013)),W*0.024,(3,9,24),160))
        disc=Image.new("RGBA",(W,W),(0,0,0,0))
        disc.paste(dgrad((W,W),[(0.0,(255,238,192)),(0.38,(255,196,140)),(0.74,(250,144,132)),(1.0,(228,96,116))],
                         -38, box=(W/2-R,W/2-R,W/2+R,W/2+R)),(0,0),dm)
        img=Image.alpha_composite(img, disc)
        # centre hole (the vinyl spindle) - a quiet nod to the reference
        hr=W*0.022
        hole=Image.new("L",(W,W),0); ImageDraw.Draw(hole).ellipse([W/2-hr,W/2-hr,W/2+hr,W/2+hr],fill=255)
        m,_=glyphs(W,"RL",FONT,0.500,-0.055,-0.003)
        img=Image.alpha_composite(img, drop_shadow(m,(0,int(W*0.009)),W*0.013,(70,20,24),135))
        img=Image.alpha_composite(img, paint(W,m,(36,52,104),(10,18,44),-40,(20,30,60)))
        img=Image.alpha_composite(img, radial((W,W),(W*0.5,W*0.5),W*0.90,(3,8,22),0.38,2.6))
        return finish(img,W,3.6)

    if kind=="C":
        img=disc_bg(W)
        img=Image.alpha_composite(img, radial((W,W),(W*0.5,W*0.5),W*0.44,(255,176,124),0.12,1.9))
        m,parts=glyphs(W,"RL",FONT,0.648,-0.058,-0.004)
        img=Image.alpha_composite(img, drop_shadow(m,(0,int(W*0.022)),W*0.018,(3,9,24),175))
        for c,pm in parts:
            if c=="R":
                img=Image.alpha_composite(img, paint(W,pm,(255,222,168),(246,110,108),-42,(150,60,50)))
            else:
                img=Image.alpha_composite(img, paint(W,pm,(255,255,250),(196,236,250),-42,(60,90,120)))
        img=Image.alpha_composite(img, radial((W,W),(W*0.5,W*0.5),W*0.86,(3,8,22),0.48,2.6))
        return finish(img,W)

    if kind=="D":
        img=cream_bg(W)
        img=Image.alpha_composite(img, radial((W,W),(W*0.5,W*0.5),W*0.62,(255,255,255),0.32,1.6))
        m,_=glyphs(W,"RL",FONT,0.648,-0.058,-0.004)
        img=Image.alpha_composite(img, drop_shadow(m,(0,int(W*0.018)),W*0.016,(150,120,96),110))
        img=Image.alpha_composite(img, paint(W,m,(48,80,150),(10,20,50),-42,(20,30,60)))
        img=Image.alpha_composite(img, radial((W,W),(W*0.5,W*0.5),W*0.98,(198,178,148),0.34,3.0))
        return finish(img,W,3.0)

    raise ValueError(kind)

if __name__=="__main__":
    os.makedirs(OUT,exist_ok=True)
    made={}
    for k in "ABCD":
        v=variant(k,1024); made[k]=v; v.save(os.path.join(OUT,f"v7{k}_1024.png"))
    sheet=Image.new("RGB",(1500,900),"#EDE9E1"); d=ImageDraw.Draw(sheet)
    f=ImageFont.truetype(r"C:\Windows\Fonts\segoeuib.ttf",26)
    labels={"A":"A · cream RL / navy disc","B":"B · navy RL / warm label","C":"C · two-tone R+L","D":"D · light plate / navy RL"}
    for i,k in enumerate("ABCD"):
        y=24+i*216
        d.text((26,y), labels[k], font=f, fill="#222")
        big=made[k].resize((180,180),Image.LANCZOS); sheet.paste(big,(26,y+34),big)
        x=234
        for sz in (128,96,64,48,32,24,16):
            im=made[k].resize((sz,sz),Image.LANCZOS); sheet.paste(im,(x,y+34+(180-sz)//2),im); x+=sz+28
    sheet.save(os.path.join(OUT,"v7_sheet.png"))
    print("ok")
