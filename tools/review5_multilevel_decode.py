"""Independent hand-decoder for .rlmultilevel v1. No writer helpers imported.
Not DSD/DoP or a DAC transport. Code indices map to y=2*i/(levels-1)-1.
"""
import argparse,collections,json,mmap,os,struct
from pathlib import Path
FNV_START=14695981039346656037
FNV_PRIME=1099511628211

def strict_object(pairs):
    result={}
    for key,value in pairs:
        if key in result:raise ValueError('duplicate metadata key')
        result[key]=value
    return result

def decode(path,materialize=True):
    # Mapping + bounded checksum chunks allow large exported files to be
    # validated without reading them or decoded Python floats into RAM at once.
    with Path(path).open('rb') as stream:
        if os.fstat(stream.fileno()).st_size<160:raise ValueError('truncated header/footer')
        with mmap.mmap(stream.fileno(),0,access=mmap.ACCESS_READ) as raw:
            return _decode_mapped(raw,materialize)

def _decode_mapped(raw,materialize):
    if len(raw)<160:raise ValueError('truncated header/footer')
    if raw[:8]!=b'RLMULTI\0':raise ValueError('bad magic')
    version,size,endian=struct.unpack_from('<HHI',raw,8)
    if version!=1:raise ValueError('unsupported version')
    if size!=128 or endian!=0x01020304:raise ValueError('unsupported header/endian tag')
    rate,channels=struct.unpack_from('<QI',raw,16)
    bits,requested_order,effective_order,requested_levels,levels,flags=struct.unpack_from('<6H',raw,28)
    source_rate,metadata_size=struct.unpack_from('<II',raw,40)
    source_frames,valid_frames,tail_frames,frames,payload_bytes=struct.unpack_from('<5Q',raw,48)
    if any(raw[88:128]):raise ValueError('reserved header bytes must be zero')
    if not 0<rate<=0xffffffff or not 0<source_rate<=0xffffffff or not 1<=channels<=32:raise ValueError('invalid rate/channels')
    if bits not in [6,8] or requested_levels!=(1<<bits) or levels!=requested_levels:raise ValueError('not genuine supported multilevel data')
    if not 1<=effective_order<=requested_order<=8:raise ValueError('invalid requested/effective order')
    if flags!=1:raise ValueError('incomplete/unsealed stream')
    if not 1<=metadata_size<=65536 or not source_frames or not valid_frames:raise ValueError('missing source ledger/metadata')
    if valid_frames+tail_frames!=frames or frames*channels!=payload_bytes:raise ValueError('frame ledger mismatch')
    end=128+metadata_size+payload_bytes
    if len(raw)!=end+32:raise ValueError('truncated payload or trailing data')
    if raw[end:end+8]!=b'RLENDv1\0':raise ValueError('missing complete EOF footer')
    footer_frames,footer_bytes,wanted=struct.unpack_from('<3Q',raw,end+8)
    if footer_frames!=frames or footer_bytes!=payload_bytes:raise ValueError('EOF ledger mismatch')
    value=FNV_START
    for start in range(0,end,65536):
        for byte in raw[start:min(end,start+65536)]:value=((value^byte)*FNV_PRIME)&0xffffffffffffffff
    if value!=wanted:raise ValueError('checksum mismatch')
    metadata=json.loads(raw[128:128+metadata_size].decode('utf-8'),object_pairs_hook=strict_object,parse_constant=lambda x:(_ for _ in ()).throw(ValueError('nonfinite JSON metadata')))
    if not isinstance(metadata,dict):raise ValueError('metadata must be an object')
    first=128+metadata_size
    counts=collections.Counter()
    for start in range(first,end,65536):counts.update(raw[start:min(end,start+65536)])
    if any(index>=levels for index in counts):raise ValueError('index outside declared levels')
    total=sum(counts.values())
    result=dict(version=version,rate=rate,channels=channels,bits=bits,requested_order=requested_order,effective_order=effective_order,requested_levels=requested_levels,effective_levels=levels,source_rate=source_rate,source_frames=source_frames,source_valid_output_frames=valid_frames,pipeline_tail_frames=tail_frames,frames=frames,metadata=metadata,payload_bytes=payload_bytes,distinct_code_count=len(counts),mean=sum(count*(2*index/(levels-1)-1) for index,count in counts.items())/total if total else 0,minimum=2*min(counts)/(levels-1)-1 if counts else 0,maximum=2*max(counts)/(levels-1)-1 if counts else 0)
    if materialize:
        if payload_bytes>8*1024*1024:raise ValueError('materializing decode exceeds 8 MiB; use CLI bounded-memory validation or materialize=False')
        payload=raw[first:end]
        result.update(payload=payload,samples=[2*index/(levels-1)-1 for index in payload])
    return result

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('input',type=Path);p.add_argument('--report',type=Path);a=p.parse_args()
    try:
        result=decode(a.input,materialize=False);result.update(status='PASS_COMPLETE_FILE',memory_mode='bounded checksum chunks and code histogram; no decoded sample array')
        text=json.dumps(result,ensure_ascii=False,indent=2);print(text)
        if a.report:a.report.write_text(text,encoding='utf-8')
        return 0
    except Exception as e:print('INVALID_MULTILEVEL',str(e));return 1
if __name__=='__main__':raise SystemExit(main())
