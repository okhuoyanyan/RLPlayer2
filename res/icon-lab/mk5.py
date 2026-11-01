
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
        p=(p-p.min())/(p.max()-p.min())
        q0=(x0*math.cos(a)+y0*math.sin(a)-p.min())/(p.max()-p.min())
        q1=(x1*math.cos(a)+y1*math.sin(a)-p.min())/(p.max()-p.min())
        p=(p-min(q0,q1))/max(1e-6,abs(q1-q0))
    else:
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

def brush_arc(size, box, start, end, color, w_max, w_min, alpha, seed, taper=0.55):
    W=size; x0,y0,x1,y1=box
    cx,cy=(x0+x1)/2,(y0+y1)/2; rx,ry=(x1-x0)/2,(y1-y0)/2
    lay=Image.new("RGBA",(W,W),(0,0,0,0)); d=ImageDraw.Draw(lay); n=180
    for i in range(n):
        t0=start+(end-start)*i/n; t1=start+(end-start)*(i+1)/n
        k=i/(n-1); e=math.sin(math.pi*min(1.0,max(0.0,k)))**taper
        w=w_min+(w_max-w_min)*e
        p0=(cx+rx*math.cos(math.radians(t0)), cy+ry*math.sin(math.radians(t0)))
        p1=(cx+rx*math.cos(math.radians(t1)), cy+ry*math.sin(math.radians(t1)))
        d.line([p0,p1], fill=tuple(color)+(int(alpha),), width=max(1,int(w)))
    return lay.filter(ImageFilter.GaussianBlur(W*0.0032))

def specks(size, pts, color, alpha, blur):
    W=size; lay=Image.new("RGBA",(W,W),(0,0,0,0)); d=ImageDraw.Draw(lay)
    for (x,y,r) in pts:
        d.ellipse([x-r,y-r,x+r,y+r], fill=tuple(color)+(int(alpha),))
    return lay.filter(ImageFilter.GaussianBlur(blur))

def letters(size, mask, top, bottom, angle=-42):
    W=size; bb=mask.getbbox()
    fill=dgrad((W,W),[(0.0,top),(1.0,bottom)],angle=angle,box=bb)
    lay=Image.new("RGBA",(W,W),(0,0,0,0)); lay.paste(fill,(0,0),mask)
    hi=ImageChops.subtract(ImageChops.offset(mask,-max(1,int(W*0.004)),-max(1,int(W*0.004))), mask)
    hi=hi.filter(ImageFilter.GaussianBlur(W*0.0035)).point(lambda v:int(v*0.75))
    lo=ImageChops.subtract(ImageChops.offset(mask,max(1,int(W*0.005)),max(1,int(W*0.005))), mask)
    lo=lo.filter(ImageFilter.GaussianBlur(W*0.004)).point(lambda v:int(v*0.5))
    hlay=Image.new("RGBA",(W,W),(255,255,255,0)); hlay.putalpha(hi)
    llay=Image.new("RGBA",(W,W),(150,70,40,0)); llay.putalpha(lo)
    return Image.alpha_composite(lay, Image.alpha_composite(llay, hlay))

def plate(size, vivid=1.0):
    W=size
    img=vgrad((W,W),[(0.0,(30,74,150)),(0.26,(19,46,108)),(0.62,(13,30,74)),(1.0,(7,16,42))])
    img=Image.alpha_composite(img, blob((W,W),(-0.22*W,-0.32*W,0.66*W,0.38*W), AZURE, 0.09*W, 185*vivid))
    img=Image.alpha_composite(img, blob((W,W),(0.36*W,-0.38*W,1.24*W,0.34*W), SKY, 0.10*W, 155*vivid))
    img=Image.alpha_composite(img, blob((W,W),(0.60*W,0.24*W,1.30*W,0.90*W), CORAL, 0.12*W, 100*vivid))
    img=Image.alpha_composite(img, blob((W,W),(-0.28*W,0.54*W,0.38*W,1.24*W), CORAL, 0.11*W, 90*vivid))
    img=Image.alpha_composite(img, blob((W,W),(-0.14*W,0.82*W,0.50*W,1.34*W), AMBER, 0.10*W, 75*vivid))
    img=Image.alpha_composite(img, blob((W,W),(0.14*W,0.62*W,1.14*W,1.32*W), NAVY_DEEP, 0.10*W, 170))
    return img

def build(size=1024, mode="A"):
    W=size
    img=plate(W)

    # watercolour specks placed with intent (not random noise)
    img=Image.alpha_composite(img, specks(W,[(0.80*W,0.10*W,W*0.010),(0.90*W,0.05*W,W*0.006),
                                              (0.72*W,0.04*W,W*0.005),(0.86*W,0.19*W,W*0.004)], PEACH, 150, W*0.004))
    img=Image.alpha_composite(img, specks(W,[(0.13*W,0.86*W,W*0.012),(0.05*W,0.74*W,W*0.007),
                                              (0.21*W,0.93*W,W*0.005)], CORAL, 165, W*0.004))
    img=Image.alpha_composite(img, specks(W,[(0.86*W,0.82*W,W*0.006),(0.93*W,0.72*W,W*0.004)], SKY, 120, W*0.004))

    img=Image.alpha_composite(img, grooves((W,W),W/2,W/2,W*0.468,W*0.180,W*0.0168,(202,232,255),56,22,max(1,int(W/512))))

    img=Image.alpha_composite(img, brush_arc(W,[W*0.075,W*0.075,W*0.925,W*0.925],-80,-2,CORAL,W*0.036,W*0.008,235,5))
    img=Image.alpha_composite(img, brush_arc(W,[W*0.125,W*0.125,W*0.875,W*0.875],184,256,MINT,W*0.022,W*0.006,200,9))

    if mode=="B":
        # warm centre label disc
        R=W*0.312
        dm=Image.new("L",(W,W),0); ImageDraw.Draw(dm).ellipse([W/2-R,W/2-R,W/2+R,W/2+R],fill=255)
        img=Image.alpha_composite(img, shadow(dm,(0,int(W*0.012)),W*0.020,(3,9,24),150))
        disc=Image.new("RGBA",(W,W),(0,0,0,0))
        disc.paste(dgrad((W,W),[(0.0,(255,226,168)),(0.42,(255,178,124)),(0.78,(250,130,126)),(1.0,(236,104,120))],-38,box=(W/2-R,W/2-R,W/2+R,W/2+R)),(0,0),dm)
        img=Image.alpha_composite(img, disc)
        mask=mono_mask(W,"RL",FONT,0.520,-0.055,-0.003)
        img=Image.alpha_composite(img, shadow(mask,(0,int(W*0.008)),W*0.012,(90,30,30),120))
        img=Image.alpha_composite(img, letters(W,mask,(26,38,80),(9,16,40),-40))
    else:
        img=Image.alpha_composite(img, radial((W,W),(W*0.5,W*0.5),W*0.40,(255,170,116),0.16,1.9))
        mask=mono_mask(W,"RL",FONT,0.640,-0.058,-0.004)
        img=Image.alpha_composite(img, shadow(mask,(0,int(W*0.021)),W*0.017,(3,9,24),170))
        img=Image.alpha_composite(img, letters(W,mask,(255,254,248),(255,190,144),-42))

    img=Image.alpha_composite(img, radial((W,W),(W*0.5,W*0.5),W*0.85,(3,8,22),0.52,2.6))
    img.putalpha(squircle_mask(W,int(W*0.2225)))
    return img

os.makedirs(OUT,exist_ok=True)
a=build(1024,"A"); b=build(1024,"B")
a.save(os.path.join(OUT,"v5A_1024.png")); b.save(os.path.join(OUT,"v5B_1024.png"))

sheet=Image.new("RGB",(1120,760),"#EFEBE3"); d=ImageDraw.Draw(sheet)
f=ImageFont.truetype(r"C:\Windows\Fonts\segoeuib.ttf",26)
for i,(lbl,v) in enumerate([("A  cream RL on disc",a),("B  navy RL on warm label",b)]):
    y=30+i*370
    d.text((30,y), lbl, font=f, fill="#222")
    big=v.resize((300,300),Image.LANCZOS); sheet.paste(big,(30,y+40),big)
    x=370
    for sz in (128,64,48,32,24,16):
        im=v.resize((sz,sz),Image.LANCZOS); sheet.paste(im,(x,y+40+(300-sz)//2),im); x+=sz+30
sheet.save(os.path.join(OUT,"v5_sheet.png"))
print("ok")
