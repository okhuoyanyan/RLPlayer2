
# -*- coding: utf-8 -*-
import numpy as np, os, math, random
from PIL import Image, ImageDraw, ImageFilter, ImageFont, ImageChops

OUT = r"D:\RTA-hqplayer\res\icon-lab"
FONT = r"C:\Windows\Fonts\ariblk.ttf"

NAVY_DEEP=(8,18,48); AZURE=(48,132,226); SKY=(104,196,248)
CORAL=(246,98,104); PEACH=(255,170,118); AMBER=(255,205,130)
CREAM=(255,250,242); MINT=(122,228,226)

def vgrad(size, stops):
    w,h=size; t=np.linspace(0,1,h); out=np.zeros((h,3))
    for i in range(len(stops)-1):
        p0,c0=stops[i]; p1,c1=stops[i+1]; m=(t>=p0)&(t<=p1)
        if m.any():
            k=((t[m]-p0)/max(1e-6,p1-p0))[:,None]
            out[m]=np.array(c0,float)*(1-k)+np.array(c1,float)*k
    out[t<stops[0][0]]=stops[0][1]; out[t>stops[-1][0]]=stops[-1][1]
    return Image.fromarray(np.repeat(out[:,None,:],w,axis=1).astype(np.uint8),"RGB").convert("RGBA")

def dgrad(size, stops, angle=-40.0, box=None):
    w,h=size
    yy,xx=np.mgrid[0:h,0:w].astype(np.float64)
    a=math.radians(angle); p=xx*math.cos(a)+yy*math.sin(a)
    if box is not None:
        x0,y0,x1,y1=box
        p=np.clip(p, x0*math.cos(a)+y0*math.sin(a), x1*math.cos(a)+y1*math.sin(a))
    p=(p-p.min())/(p.max()-p.min())
    out=np.zeros((h,w,3))
    for i in range(len(stops)-1):
        p0,c0=stops[i]; p1,c1=stops[i+1]
        m=(p>=p0)&(p<=p1)
        if m.any():
            k=((p[m]-p0)/max(1e-6,p1-p0))[:,None]
            out[m]=np.array(c0,float)*(1-k)+np.array(c1,float)*k
    out[p<stops[0][0]]=stops[0][1]; out[p>stops[-1][0]]=stops[-1][1]
    return Image.fromarray(out.astype(np.uint8),"RGB").convert("RGBA")

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

def shadow(mask,offset,blur,color,alpha):
    solid=Image.new("RGBA",mask.size,tuple(color)+(int(alpha),))
    lay=Image.new("RGBA",mask.size,(0,0,0,0)); lay.paste(solid,(0,0),mask)
    lay=lay.filter(ImageFilter.GaussianBlur(blur))
    out=Image.new("RGBA",mask.size,(0,0,0,0)); out.paste(lay,offset,lay)
    return out

def grooves(size,cx,cy,r_out,r_in,step,color,amaj,amin,width):
    lay=Image.new("RGBA",size,(0,0,0,0)); d=ImageDraw.Draw(lay); r=r_out; i=0
    while r>r_in:
        d.ellipse([cx-r,cy-r,cx+r,cy+r], outline=tuple(color)+((amaj if i%6==0 else amin),), width=width)
        r-=step; i+=1
    return lay

def mono_mask(size,txt,font_path,target_w,tracking,y_bias=0.0):
    W=size; big=W*3
    d=ImageDraw.Draw(Image.new("L",(8,8)))
    f=ImageFont.truetype(font_path,int(big*0.5))
    ws=[d.textbbox((0,0),c,font=f)[2]-d.textbbox((0,0),c,font=f)[0] for c in txt]
    total=sum(ws)+int(big*0.5*tracking)*(len(txt)-1)
    fs=int(big*0.5*target_w*W/total)
    f=ImageFont.truetype(font_path,fs); tr=int(fs*tracking)
    ws=[d.textbbox((0,0),c,font=f)[2] for c in txt]
    ws=[d.textbbox((0,0),c,font=f)[2]-d.textbbox((0,0),c,font=f)[0] for c in txt]
    total=sum(ws)+tr*(len(txt)-1)
    asc,desc=f.getmetrics()
    tmp=Image.new("L",(big,big),0); td=ImageDraw.Draw(tmp)
    x=(big-total)/2.0; base_y=big/2.0+(asc-(asc+desc)/2.0)
    for c,wc in zip(txt,ws):
        td.text((x,base_y),c,font=f,fill=255,anchor="ls"); x+=wc+tr
    bb=tmp.getbbox()
    ox=int(W/2-(bb[0]+(bb[2]-bb[0])/2)); oy=int(W/2-(bb[1]+(bb[3]-bb[1])/2)+y_bias*W)
    o=Image.new("L",(W,W),0); o.paste(tmp,(ox,oy)); return o

def paint_arc(size, box, start, end, color, w_max, w_min, alpha=235, seed=0, beads=True):
    """tapered brush stroke along an arc"""
    W=size
    lay=Image.new("RGBA",(W,W),(0,0,0,0)); d=ImageDraw.Draw(lay)
    x0,y0,x1,y1=box
    cx,cy=(x0+x1)/2,(y0+y1)/2; rx,ry=(x1-x0)/2,(y1-y0)/2
    n=140
    rnd=random.Random(seed)
    for i in range(n):
        t0=start+(end-start)*i/n; t1=start+(end-start)*(i+1)/n
        k=i/(n-1)
        # taper: thin at both ends, fat in the middle-ish
        e=math.sin(math.pi*min(1,max(0,k)))**0.55
        w=w_min+(w_max-w_min)*e
        pts=[(cx+rx*math.cos(math.radians(t)), cy+ry*math.sin(math.radians(t))) for t in (t0,t1)]
        d.line(pts, fill=tuple(color)+(alpha,), width=max(1,int(w/2)), joint="curve")
        for p in pts:
            r=w/2.0
            d.ellipse([p[0]-r,p[1]-r,p[0]+r,p[1]+r], fill=tuple(color)+(alpha,))
    if beads:
        for _ in range(7):
            t=rnd.uniform(start-14, end+10)
            rr=(cx+rx*math.cos(math.radians(t)), cy+ry*math.sin(math.radians(t)))
            rad=rnd.uniform(w_min*0.25, w_min*0.85)
            off=(rnd.uniform(-W*0.02,W*0.02), rnd.uniform(-W*0.02,W*0.02))
            d.ellipse([rr[0]+off[0]-rad, rr[1]+off[1]-rad, rr[0]+off[0]+rad, rr[1]+off[1]+rad],
                      fill=tuple(color)+(int(alpha*0.75),))
    return lay.filter(ImageFilter.GaussianBlur(W*0.0035))

def splatter(size, region, color, count, rmin, rmax, seed, alpha=200, streak=True):
    W=size; x0,y0,x1,y1=region
    rnd=random.Random(seed)
    lay=Image.new("RGBA",(W,W),(0,0,0,0)); d=ImageDraw.Draw(lay)
    for _ in range(count):
        cx=rnd.uniform(x0,x1); cy=rnd.uniform(y0,y1); r=rnd.uniform(rmin,rmax)
        a=int(alpha*rnd.uniform(0.45,1.0))
        d.ellipse([cx-r,cy-r,cx+r,cy+r], fill=tuple(color)+(a,))
    if streak:
        for _ in range(max(2,count//6)):
            cx=rnd.uniform(x0,x1); cy=rnd.uniform(y0,y1)
            ln=rnd.uniform(rmax*3,rmax*9); ang=rnd.uniform(-2.6,-0.5)
            ex,ey=cx+ln*math.cos(ang), cy+ln*math.sin(ang)
            wd=rnd.uniform(rmin*0.6,rmax*0.9)
            d.line([(cx,cy),(ex,ey)], fill=tuple(color)+(int(alpha*0.8),), width=max(1,int(wd)))
            d.ellipse([ex-wd/2,ey-wd/2,ex+wd/2,ey+wd/2], fill=tuple(color)+(int(alpha*0.8),))
    return lay.filter(ImageFilter.GaussianBlur(W*0.006))

def letters(size, mask, top, bottom, angle=-42):
    W=size
    bb=mask.getbbox()
    fill=dgrad((W,W),[(0.0,top),(1.0,bottom)],angle=angle,box=bb)
    lay=Image.new("RGBA",(W,W),(0,0,0,0)); lay.paste(fill,(0,0),mask)

    # inner bevel: highlight offset up-left, shade down-right
    hi=ImageChops.subtract(ImageChops.offset(mask,-int(W*0.004),-int(W*0.004)), mask)
    hi=hi.filter(ImageFilter.GaussianBlur(W*0.0035)).point(lambda v:int(v*0.9))
    lo=ImageChops.subtract(ImageChops.offset(mask,int(W*0.005),int(W*0.005)), mask)
    lo=lo.filter(ImageFilter.GaussianBlur(W*0.004)).point(lambda v:int(v*0.55))

    hlay=Image.new("RGBA",(W,W),(255,255,255,0)); hlay.putalpha(hi)
    llay=Image.new("RGBA",(W,W),(120,50,30,0)); llay.putalpha(lo)
    lay=Image.alpha_composite(lay,Image.alpha_composite(llay,hlay))

    # overall top-left sheen
    sheen=dgrad((W,W),[(0.0,(255,255,255)),(0.55,(255,255,255))],angle=-45,box=bb)
    sl=Image.new("RGBA",(W,W),(0,0,0,0)); sl.paste(sheen,(0,0),mask)
    from PIL import ImageEnhance
    lay=Image.blend(lay,Image.alpha_composite(lay,sl.point(lambda v:int(v*0.35))),0.35) if False else lay
    return lay

def build(size=1024, variant="A"):
    W=size
    img=vgrad((W,W),[(0.0,(32,76,152)),(0.28,(19,46,108)),(0.64,(13,30,74)),(1.0,(8,17,44))])
    # painterly washes
    img=Image.alpha_composite(img, blob((W,W),(-0.20*W,-0.30*W,0.68*W,0.40*W), AZURE, 0.09*W, 180))
    img=Image.alpha_composite(img, blob((W,W),(0.38*W,-0.36*W,1.22*W,0.36*W), SKY, 0.10*W, 150))
    img=Image.alpha_composite(img, blob((W,W),(0.58*W,0.26*W,1.28*W,0.92*W), CORAL, 0.12*W, 95))
    img=Image.alpha_composite(img, blob((W,W),(-0.26*W,0.56*W,0.40*W,1.22*W), CORAL, 0.11*W, 85))
    img=Image.alpha_composite(img, blob((W,W),(-0.12*W,0.80*W,0.52*W,1.32*W), AMBER, 0.10*W, 70))
    img=Image.alpha_composite(img, blob((W,W),(0.16*W,0.64*W,1.12*W,1.30*W), NAVY_DEEP, 0.10*W, 165))

    # splatter (the reference's watercolour specks)
    img=Image.alpha_composite(img, splatter(W,(0.52*W,0.02*W,1.02*W,0.30*W), PEACH, 16, W*0.004, W*0.016, 11, 120))
    img=Image.alpha_composite(img, splatter(W,(0.00*W,0.62*W,0.36*W,1.00*W), CORAL, 18, W*0.004, W*0.017, 22, 130))
    img=Image.alpha_composite(img, splatter(W,(0.60*W,0.66*W,1.00*W,0.98*W), SKY, 12, W*0.003, W*0.012, 33, 95))

    # vinyl grooves
    img=Image.alpha_composite(img, grooves((W,W),W/2,W/2,W*0.468,W*0.175,W*0.0168,(202,232,255),58,24,max(1,int(W/512))))

    # accent brush strokes
    img=Image.alpha_composite(img, paint_arc(W,[W*0.09,W*0.09,W*0.91,W*0.91],-78,-4,CORAL,W*0.040,W*0.010,235,5))
    img=Image.alpha_composite(img, paint_arc(W,[W*0.13,W*0.13,W*0.87,W*0.87],186,254,MINT,W*0.026,W*0.008,205,9,beads=False))

    # warm label glow
    img=Image.alpha_composite(img, radial((W,W),(W*0.50,W*0.50),W*0.42,(255,168,112),0.30,1.8))

    mask=mono_mask(W,"RL",FONT,0.645,-0.055,-0.004)
    img=Image.alpha_composite(img, shadow(mask,(0,int(W*0.020)),W*0.016,(3,9,24),160))
    img=Image.alpha_composite(img, letters(W,mask,(255,253,246),(255,186,140),-42))

    # vignette + squircle
    img=Image.alpha_composite(img, radial((W,W),(W*0.5,W*0.5),W*0.84,(3,8,22),0.55,2.6))
    img.putalpha(squircle_mask(W,int(W*0.2225)))
    return img

os.makedirs(OUT,exist_ok=True)
v=build(1024)
v.save(os.path.join(OUT,"v4_1024.png"))
sheet=Image.new("RGB",(1180,430),"#EFEBE3")
big=v.resize((300,300),Image.LANCZOS); sheet.paste(big,(24,66),big)
x=360
for sz in (128,64,48,32,24,16):
    im=v.resize((sz,sz),Image.LANCZOS); sheet.paste(im,(x,66+(300-sz)//2),im); x+=sz+28
sheet.save(os.path.join(OUT,"v4_sheet.png"))
print("ok")
