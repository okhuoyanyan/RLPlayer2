"""Offline artwork profile migration gate; synthetic PNG only, no app/audio/network."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import tempfile
import zlib

def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--probe',type=Path,required=True)
    parser.add_argument('--evidence-dir',type=Path)
    args=parser.parse_args()
    root=(args.evidence_dir or Path(tempfile.mkdtemp(prefix='rlplayer-profile-artwork-'))).resolve()
    root.mkdir(parents=True,exist_ok=True)
    # Evidence roots supplied by the release runner can be deep; the fixture
    # itself contains version backups plus SHA-named blobs. Keep a unique short
    # private runtime so the gate does not depend on global LongPaths settings.
    runtime=Path(tempfile.mkdtemp(prefix='rlpam-',dir=Path(tempfile.gettempdir()).anchor))
    fixture=runtime/'fixture'
    (root/'runtime.json').write_text(json.dumps({'fixture':str(fixture)}),encoding='utf8')
    env={**os.environ,'RTA_NO_DEVICE':'1','RTA_NET_TESTS':'0'}
    result=subprocess.run([str(args.probe.resolve()),str(fixture)],env=env,capture_output=True,text=True,encoding='utf8',errors='replace',timeout=90)
    (root/'probe.log').write_text(result.stdout+result.stderr,encoding='utf8')
    match=re.search(r'RESULT (\d+) pass (\d+) fail',result.stdout)
    if result.returncode != 0 or match is None:
        report={'pass':False,'probe_returncode':result.returncode,'reason':'migration probe failed; inspect probe.log before image decoding','evidence_dir':str(root),'fixture':str(fixture)}
        (root/'results.json').write_text(json.dumps(report,indent=2),encoding='utf8')
        print(json.dumps(report))
        return 1
    # Independently validate the synthetic image's actual PNG chunks and decoded
    # raster. Counting copied files alone would not prove valid artwork bytes.
    image=next((fixture/'RLPlayer-3.3.1-win64/covercache').glob('1111111111111111.*.blob')).read_bytes()
    assert image[:8]==b'\x89PNG\r\n\x1a\n'
    offset=8
    compressed=bytearray()
    width=height=None
    ended=False
    while offset<len(image):
        n=struct.unpack('>I',image[offset:offset+4])[0]
        kind=image[offset+4:offset+8]
        data=image[offset+8:offset+8+n]
        crc=struct.unpack('>I',image[offset+8+n:offset+12+n])[0]
        assert zlib.crc32(kind+data)==crc
        if kind==b'IHDR':
            width,height,depth,color,_,_,_=struct.unpack('>IIBBBBB',data)
            assert depth==8 and color==2
        elif kind==b'IDAT':
            compressed.extend(data)
        elif kind==b'IEND':
            ended=True
        offset+=12+n
    raster=zlib.decompress(compressed)
    expected=b''.join(b'\0'+bytes((x*31+y*17+i*41)%256 for x in range(8) for i in range(3)) for y in range(8))
    decoded=ended and (width,height)==(8,8) and raster==expected
    passed=result.returncode==0 and match is not None and int(match[2])==0 and decoded
    report={'pass':passed,'probe_returncode':result.returncode,'checks_pass':int(match[1]) if match else 0,'checks_fail':int(match[2]) if match else None,'synthetic_png_exact_decoded_raster':decoded,'synthetic_png_bytes':len(image),'synthetic_png_sha256':hashlib.sha256(image).hexdigest(),'evidence_dir':str(root),'fixture':str(fixture)}
    (root/'results.json').write_text(json.dumps(report,indent=2),encoding='utf8')
    print(json.dumps(report))
    return 0 if passed else 1

if __name__=='__main__':
    raise SystemExit(main())

