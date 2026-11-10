"""Independent ECMA-167 fixtures; numeric bytes, no mounted filesystem."""
from pathlib import Path
import struct,binascii
import sys
out=Path(sys.argv[1]) if len(sys.argv)>1 else Path.cwd()/"udf-fixtures"
out.mkdir(parents=True,exist_ok=True)
B=2048; part=400
p16=lambda b,o,x:struct.pack_into('<H',b,o,x)
p32=lambda b,o,x:struct.pack_into('<I',b,o,x)
p64=lambda b,o,x:struct.pack_into('<Q',b,o,x)
def tag(b,ident,loc,used=None):
 if used is None:used=len(b)
 p16(b,0,ident);p16(b,2,2);p16(b,6,1);p16(b,8,binascii.crc_hqx(b[16:used],0));p16(b,10,used-16);p32(b,12,loc);b[4]=0;b[4]=sum(b[:4]+b[5:16])&255;return b
def ad(n,lbn,kind=0):return struct.pack('<II',n|(kind<<30),lbn)
def fid(name,lbn,flags=0):
 n=bytes([8])+name.encode() if name else b'';b=bytearray((38+len(n)+3)&~3);p16(b,16,1);b[18]=flags;b[19]=len(n);p32(b,20,B);p32(b,24,lbn);p16(b,28,0);b[38:38+len(n)]=n;return tag(b,257,2)
def make(label,ads,size,mode=0,aed=None):
 image=bytearray(1000*B)
 def put(lbn,data):image[lbn*B:lbn*B+len(data)]=data
 for lbn,ident in [(16,b'BEA01'),(17,b'NSR02'),(18,b'TEA01')]:
  b=bytearray(B);b[1:6]=ident;b[6]=1;put(lbn,b)
 for seq in (32,48):
  b=bytearray(B);p32(b,16,1);put(seq,tag(b,1,seq,512))
  b=bytearray(B);p32(b,16,2);p16(b,20,1);p16(b,22,0);b[25:31]=b'+NSR02';p32(b,184,1);p32(b,188,part);p32(b,192,400);put(seq+1,tag(b,5,seq+1,512))
  b=bytearray(B);p32(b,16,3);p32(b,212,B);b[217:240]=b'*OSTA UDF Compliant'.ljust(23,b'\0');p16(b,240,0x102);p32(b,248,B);p32(b,252,0);p16(b,256,0);p32(b,264,6);p32(b,268,1);b[440:446]=bytes([1,6,1,0,0,0]);put(seq+2,tag(b,6,seq+2,446));put(seq+3,tag(bytearray(B),8,seq+3,512))
 for lbn in (256,999):
  b=bytearray(B);struct.pack_into('<IIII',b,16,4*B,32,4*B,48);put(lbn,tag(b,2,lbn,512))
 b=bytearray(B);p32(b,400,B);p32(b,404,1);put(part,tag(b,256,0,512))
 directory=fid('',1,8)+fid('DATA.BIN',3);put(part+2,directory)
 def fe(lbn,alloc,size,mode,filetype):
  b=bytearray(B);p16(b,20,4);b[27]=filetype;p16(b,34,mode);p16(b,48,1);p64(b,56,size);p64(b,64,(size+B-1)//B);p32(b,168,0);p32(b,172,len(alloc));b[176:176+len(alloc)]=alloc;put(part+lbn,tag(b,261,lbn,176+len(alloc)))
 fe(1,ad(len(directory),2),len(directory),0,4);fe(3,ads,size,mode,5)
 if aed:
  b=bytearray(B);p32(b,20,len(aed));b[24:24+len(aed)]=aed;put(part+5,tag(b,258,5,24+len(aed)))
 for lbn,seed in ((4,17),(10,91)):
  put(part+lbn,bytes((i*37+seed)&255 for i in range(B)))
 (out/f'{label}.iso').write_bytes(image)
make('udf-aed-fragmented',ad(B,4)+ad(B,5,3),4096,aed=ad(B,10))
make('udf-zero-terminator',ad(B,4)+ad(0,0)+ad(B,10),4096)
make('udf-after-continuation',ad(B,4)+ad(B,5,3)+ad(B,4),6144,aed=ad(B,10))
make('udf-sparse-large',ad(900000,0,2),900000)
# Extended AD represents a 2048-byte allocation with only its first 1024 information bytes.
ext=lambda n,rec,info,lbn:struct.pack('<IIIIHH',n,rec,info,lbn,0,0)
make('udf-extended-info',ext(B,1024,1024,4)+ext(B,1024,1024,10),2048,mode=2)
print('generated 5 independent UDF navigation boundary fixtures')

make('udf-long-ad',struct.pack('<IIH6x',B,4,0)+struct.pack('<IIH6x',B,10,0),4096,mode=1)
make('udf-cycle',ad(B,5,3),4096,aed=ad(B,5,3))
make('udf-outside',ad(B,400),B)
make('udf-extended-zero-tail',ext(B,1024,B,4),B,mode=2)
for label in ('udf-fid-wrong-location','udf-bad-crc'):
 b=bytearray((out/'udf-aed-fragmented.iso').read_bytes())
 if label=='udf-fid-wrong-location':
  offset=(part+2)*B;record=bytearray(b[offset:offset+40]);record=tag(record,257,99,len(record));b[offset:offset+len(record)]=record
 else:b[(part+3)*B+176]^=1
 (out/f'{label}.iso').write_bytes(b)
# Embedded file content lives in the ICB, not a physical extent.
b=bytearray((out/'udf-aed-fragmented.iso').read_bytes());offset=(part+3)*B;data=bytes((i*37+17)&255 for i in range(800));entry=bytearray(B);entry[27]=5;p16(entry,34,3);p64(entry,56,len(data));p32(entry,172,len(data));entry[176:176+len(data)]=data;b[offset:offset+B]=tag(entry,261,3,176+len(data));(out/'udf-embedded.iso').write_bytes(b)
print('generated 12 numeric UDF navigation boundary fixtures (includes deliberate corruption)')

b=bytearray((out/'udf-aed-fragmented.iso').read_bytes());directory=b[(part+2)*B:(part+2)*B+88];parent=directory[:40];target=directory[40:88]
new=parent*51+target
b[(part+2)*B:(part+3)*B]=new[:B];b[(part+12)*B:(part+12)*B+len(new)-B]=new[B:]
entry=bytearray(b[(part+1)*B:(part+2)*B]);struct.pack_into('<Q',entry,56,len(new));struct.pack_into('<I',entry,172,16);entry[176:192]=struct.pack('<IIII',B,2,len(new)-B,12);b[(part+1)*B:(part+2)*B]=tag(entry,261,1,192)
(out/'udf-fid-cross-fragments.iso').write_bytes(b)
# First FID byte stays in logical block2 even when its CRC/body spans to logical block12.
wrong=bytearray(b);target=tag(bytearray(target),257,12);wrong[(part+2)*B+2040:(part+3)*B]=target[:8];wrong[(part+12)*B:(part+12)*B+40]=target[8:];(out/'udf-fid-cross-wronglocation.iso').write_bytes(wrong)
print('generated cross-fragment FID pair')

