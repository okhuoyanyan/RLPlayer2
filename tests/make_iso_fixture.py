"""Generate lawful audio-disc fixtures from numeric sample formulas only."""
import pathlib,struct,subprocess,sys,wave
out=pathlib.Path(sys.argv[1]);out.mkdir(parents=True,exist_ok=True)
ff=sys.argv[2]
def pcm(n,ch):return ((n*43+ch*991)%20001)-10000
mlpraw=out/'mlp-source.wav'
with wave.open(str(mlpraw),'wb') as f:
 f.setparams((2,2,48000,1200,'NONE','not compressed'));f.writeframes(b''.join(struct.pack('<h',pcm(i,c)) for i in range(1200) for c in range(2)))
subprocess.run([ff,'-v','error','-y','-i',str(mlpraw),'-strict','-2','-c:a','mlp','-f','mlp',str(out/'synthetic.mlp')],check=True)
def pts(n):return bytes([0x21|((n>>29)&14),(n>>22)&255,((n>>14)&254)|1,(n>>7)&255,((n<<1)&254)|1])
def sector(payload,timestamp=0):
 pack=bytes.fromhex('000001ba4400040004010189c3f8');pes=bytes.fromhex('000001bd')+struct.pack('>H',len(payload)+8)+bytes([0x80,0x80,5])+pts(timestamp)+payload
 b=pack+pes;left=2048-len(b)
 if left>=6:b+=bytes.fromhex('000001be')+struct.pack('>H',left-6)+bytes(left-6)
 else:b+=bytes(left)
 assert len(b)==2048
 return b
def lpcm(bits,rate,track):
 frames=480 if rate==48000 else 320;payload=bytearray()
 for i in range(0,frames,2):
  values=[pcm(track*frames+i+n,c)<<(bits-16) for n in range(2) for c in range(2)]
  for v in values:payload+=struct.pack('>H',(v>>(bits-16))&65535)
  if bits==24:payload+=bytes(v&255 for v in values)
 private=bytes([0xa0,0,0,8,0,6,0x10,((bits-16)//4)<<4|15,(0 if rate==48000 else 1)<<4|15,0,1,0x80])+payload
 return sector(private,round(track*frames/rate*90000)),frames
rawmlp=(out/'synthetic.mlp').read_bytes();mlppacks=b''.join(sector(bytes([0xa1,0,0,6])+bytes(6)+rawmlp[i:i+1980]) for i in range(0,len(rawmlp),1980))
img=bytearray(200*2048)
def put32(o,n):img[o:o+4]=struct.pack('>I',n)
def record(name,lba,n,directory=False):
 name=name.encode() if isinstance(name,str) else bytes([name]);length=33+len(name)+(len(name)%2==0);r=bytearray(length);r[0]=length;r[2:10]=struct.pack('<I',lba)+struct.pack('>I',lba);r[10:18]=struct.pack('<I',n)+struct.pack('>I',n);r[18:25]=bytes([126,11,10,0,0,0,0]);r[25]=2 if directory else 0;r[28:32]=bytes([1,0,0,1]);r[32]=len(name);r[33:33+len(name)]=name;return r
pvd=16*2048;img[pvd]=1;img[pvd+1:pvd+6]=b'CD001';img[pvd+6]=1;img[pvd+156:pvd+190]=record(0,20,2048,True)
# ECMA-119 PVD and both endian path tables, independently readable by 7-Zip.
img[pvd+8:pvd+40]=b'RLPLAYER NUMERIC FIXTURE'.ljust(32,b' ')
img[pvd+40:pvd+72]=b'SYNTHETIC_DVDA'.ljust(32,b' ')
img[pvd+80:pvd+88]=struct.pack('<I',len(img)//2048)+struct.pack('>I',len(img)//2048)
for offset,value in ((120,1),(124,1),(128,2048)):
 img[pvd+offset:pvd+offset+4]=struct.pack('<H',value)+struct.pack('>H',value)
def path_record(name,lba,parent,endian):
 return bytes([len(name),0])+struct.pack(endian+'IH',lba,parent)+name+(b'\0' if len(name)%2 else b'')
little=path_record(b'\0',20,1,'<')+path_record(b'AUDIO_TS',21,1,'<')
big=path_record(b'\0',20,1,'>')+path_record(b'AUDIO_TS',21,1,'>')
img[18*2048:18*2048+len(little)]=little;img[19*2048:19*2048+len(big)]=big
img[pvd+132:pvd+140]=struct.pack('<I',len(little))+struct.pack('>I',len(little))
struct.pack_into('<I',img,pvd+140,18);struct.pack_into('>I',img,pvd+148,19)
for a,z in ((190,318),(318,446),(446,574),(574,702),(702,739),(739,776),(776,813)):
 img[pvd+a:pvd+z]=b' '*(z-a)
img[pvd+574:pvd+702]=b'RLPLAYER SYNTHETIC NUMERIC AUDIO'.ljust(128,b' ')
for offset in (813,830,864):img[pvd+offset:pvd+offset+17]=b'2026111000000000\0'
img[pvd+847:pvd+864]=b'0000000000000000\0';img[pvd+881]=1

img[17*2048]=255;img[17*2048+1:17*2048+6]=b'CD001';img[17*2048+6]=1
root=record(0,20,2048,True)+record(1,20,2048,True)+record('AUDIO_TS',21,2048,True);img[20*2048:20*2048+len(root)]=root
files=[]
def file(name,lba,data):img[lba*2048:lba*2048+len(data)]=data;files.append(record(name+';1',lba,len(data)))
file('AUDIO_TS.IFO',22,b'DVDAUDIO-AMG'+bytes(2036))
def ifo(group,bits,rate,aobs,frames,mlp=False):
 b=bytearray(4096);b[:12]=b'DVDAUDIO-ATS';b[204:208]=struct.pack('>I',1);b[256]=int(mlp);base=2048;b[base:base+2]=struct.pack('>H',1);b[base+4:base+8]=struct.pack('>I',2047);b[base+12:base+16]=struct.pack('>I',16);o=base+16;nt=len(frames);b[o+2:o+4]=bytes([nt,nt]);st=18+nt*20;b[o+14:o+16]=struct.pack('>H',st);first=0;tick=0
 for t,(n,sects) in enumerate(zip(frames,aobs)):
  ts=o+18+t*20;b[ts+4]=t+1;b[ts+6:ts+10]=struct.pack('>I',tick);dur=round(n/rate*90000);b[ts+10:ts+14]=struct.pack('>I',dur);tick+=dur;sp=o+st+t*12;b[sp+4:sp+8]=struct.pack('>I',first);b[sp+8:sp+12]=struct.pack('>I',first+sects-1);first+=sects
 return b
packs=[];frames=[]
for t in range(2):pack,n=lpcm(16,48000,t);packs.append(pack);frames.append(n)
file('ATS_01_0.IFO',23,ifo(1,16,48000,[1,1],frames));file('ATS_01_1.AOB',30,b''.join(packs))
packs24=[];frames24=[]
for t in range(2):pack,n=lpcm(24,96000,t);packs24.append(pack);frames24.append(n)
file('ATS_02_0.IFO',25,ifo(2,24,96000,[1,1],frames24));file('ATS_02_1.AOB',32,b''.join(packs24))
file('ATS_03_0.IFO',27,ifo(3,16,48000,[len(mlppacks)//2048],[1200],True));file('ATS_03_1.AOB',40,mlppacks)
audio=record(0,21,2048,True)+record(1,20,2048,True)+b''.join(files);img[21*2048:21*2048+len(audio)]=audio
(out/'synthetic-dvda.iso').write_bytes(img)
print(out/'synthetic-dvda.iso',len(img),'MLP',len(rawmlp),'sectors',len(mlppacks)//2048)

# UDF-only copies of exactly the same numeric audio, no CD001 bridge.
# File entries use multiple non-contiguous recorded allocation extents for AOBs.
import binascii
for revision,label in ((0x0102,'udf102'),(0x0150,'udf150')):
 udf=bytearray(600*2048);partition_start=300
 def tag(block,ident,location,used=512):
  struct.pack_into('<HH',block,0,ident,2);struct.pack_into('<H',block,6,1)
  struct.pack_into('<HHI',block,8,binascii.crc_hqx(block[16:used],0),used-16,location)
  block[4]=sum(block[:4]+block[5:16])&255
  return block
 def block(lba,data):udf[lba*2048:lba*2048+len(data)]=data
 for lba,ident in ((16,b'BEA01'),(17,b'NSR02'),(18,b'TEA01')):
  v=bytearray(2048);v[1:6]=ident;v[6]=1;block(lba,v)
 anchor=bytearray(2048);struct.pack_into('<IIII',anchor,16,3*2048,260,3*2048,260);block(256,tag(anchor,2,256))
 pd=bytearray(2048);struct.pack_into('<HH',pd,20,1,0);struct.pack_into('<II',pd,188,partition_start,200);block(260,tag(pd,5,260))
 lv=bytearray(2048);struct.pack_into('<I',lv,212,2048);lv[217:240]=b'*OSTA UDF Compliant'.ljust(23,b'\0');struct.pack_into('<H',lv,240,revision);struct.pack_into('<IIH',lv,248,2048,0,0);struct.pack_into('<II',lv,264,6,1);lv[440:446]=bytes([1,6,1,0,0,0]);block(261,tag(lv,6,261,446));block(262,tag(bytearray(2048),8,262))
 fsd=bytearray(2048);struct.pack_into('<IIH',fsd,400,2048,1,0);block(partition_start,tag(fsd,256,0))
 def fid(name,entry,isdir,logical):
  name=bytes([8])+name.encode('ascii');n=(38+len(name)+3)&~3;b=bytearray(n);struct.pack_into('<H',b,16,1);b[18]=2 if isdir else 0;b[19]=len(name);struct.pack_into('<IIH',b,20,2048,entry,0);b[38:38+len(name)]=name;return tag(b,257,logical,n)
 def fe(logical,payload_extents,size,isdir=False,embedded=None):
  b=bytearray(2048);b[27]=4 if isdir else 5;struct.pack_into('<H',b,34,3 if embedded is not None else 0);struct.pack_into('<Q',b,56,size)
  ads=bytes(embedded) if embedded is not None else b''.join(struct.pack('<II',n,lba) for lba,n in payload_extents)
  struct.pack_into('<II',b,168,0,len(ads));b[176:176+len(ads)]=ads;block(partition_start+logical,tag(b,261,logical,176+len(ads)))
 rootdir=fid('AUDIO_TS',3,True,2);block(partition_start+2,rootdir);fe(1,[(2,len(rootdir))],len(rootdir),True)
 audio=bytearray();cursor=30
 for file_index,record_bytes in enumerate(files):
  name=record_bytes[33:33+record_bytes[32]].decode().split(';')[0];source_lba,=struct.unpack_from('<I',record_bytes,2);size,=struct.unpack_from('<I',record_bytes,10);payload=img[source_lba*2048:source_lba*2048+size];entry=6+file_index
  extents=[]
  for offset in range(0,len(payload),2048):
   chunk=payload[offset:offset+2048];block(partition_start+cursor,chunk);extents.append((cursor,len(chunk)));cursor+=2
  fe(entry,extents,len(payload));audio+=fid(name,entry,False,4)
 block(partition_start+4,audio);fe(3,[(4,len(audio))],len(audio),True)
 last=bytearray(anchor);block(599,tag(last,2,599))
 (out/f'synthetic-dvda-{label}.iso').write_bytes(udf)
 print(out/f'synthetic-dvda-{label}.iso',len(udf),'fragmented UDF revision',hex(revision))
