"""Narrow ISA exceptions: match whole known CRT functions, relocations and guards.
Unknown application EVEX never inherits a library-wide exemption. No PE is run.
"""
import argparse,hashlib,json,os,re,struct,subprocess,tempfile
from pathlib import Path

def sha(b):return hashlib.sha256(b).hexdigest()
def run(args):return subprocess.check_output(list(map(str,args)),text=True,encoding="utf-8",errors="replace")
def coff(data):
    machine,n,_,symptr,nsym,opt,_=struct.unpack_from('<HHLLLHH',data)
    if machine!=0x8664 or opt:raise ValueError('Expected AMD64 COFF object')
    strings=data[symptr+18*nsym:]
    def string(offset):return strings[offset:].split(b'\0',1)[0].decode('utf-8',errors='replace')
    syms={};i=0
    while i<nsym:
        name=data[symptr+18*i:symptr+18*i+8]
        text=string(struct.unpack_from('<L',name,4)[0]) if name[:4]==b'\0'*4 else name.split(b'\0',1)[0].decode(errors='replace')
        value,section,typ,storage,aux=struct.unpack_from('<LhHBB',data,symptr+18*i+8)
        syms[i]=dict(name=text,value=value,section=section,type=typ)
        i+=1+aux
    sections=[]
    for index in range(n):
        off=20+40*index
        name=data[off:off+8].split(b'\0',1)[0].decode(errors='replace')
        size,pointer,relptr=struct.unpack_from('<LLL',data,off+16)
        nr=struct.unpack_from('<H',data,off+32)[0]
        rel=[]
        for k in range(nr):
            address,symbol,kind=struct.unpack_from('<LLH',data,relptr+10*k)
            rel.append(dict(offset=address,symbol=syms[symbol]['name'],kind=kind,symbol_value=syms[symbol]['value'],symbol_section=syms[symbol]['section']))
        funcs=[s['name'] for s in syms.values() if s['section']==index+1 and s['value']==0 and s['type']==0x20]
        sections.append(dict(name=name,index=index+1,bytes=data[pointer:pointer+size],relocations=rel,functions=funcs))
    return sections

def pe(data):
    pos=struct.unpack_from('<L',data,0x3c)[0]
    if data[pos:pos+4]!=b'PE\0\0':raise ValueError('not PE')
    n=struct.unpack_from('<H',data,pos+6)[0];opt=struct.unpack_from('<H',data,pos+20)[0]
    base=struct.unpack_from('<Q',data,pos+24+24)[0]
    sections=[]
    for k in range(n):
        p=pos+24+opt+k*40;name=data[p:p+8].split(b'\0',1)[0]
        virtual,rva,size,raw=struct.unpack_from('<LLLL',data,p+8)
        if name==b'.text':sections.append(dict(rva=rva,bytes=data[raw:raw+size]))
    if not sections:raise ValueError('No executable .text')
    return base,sections

instruction=re.compile(r'^\s*([0-9a-f]+):\s+((?:[0-9a-f]{2}\s+)+)\s*(\S+)(.*)$')
def instructions(text):
    result=[]
    for line in text.splitlines():
        m=instruction.match(line)
        if m:result.append(dict(offset=int(m[1],16),bytes=bytes.fromhex(m[2]),mnemonic=m[3],operands=m[4].strip()))
    return result

def evex(ins):return ins['bytes'][:1]==b'\x62' and ins['mnemonic'] not in ['<unknown>','.byte']
def symbols_disasm(text):
    groups={};current=None
    for line in text.splitlines():
        m=re.match(r'^[0-9a-f]+ <(.+)>:$',line)
        if m:current=m[1];groups[current]=[]
        elif current:
            found=instructions(line)
            if found:groups[current]+=found
    return groups

def matches(section,images,allow_addr32nb=False):
    code=section['bytes'];mask=bytearray(b'\1'*len(code))
    for rel in section['relocations']:
        if not (4<=rel['kind']<=9 or (allow_addr32nb and rel['kind']==3)):return []
        at=rel['offset'];mask[at:at+4]=b'\0'*4
    chunks=[];start=None
    for i,v in enumerate(mask+b'\0'):
        if v and start is None:start=i
        if not v and start is not None:chunks.append((start,i));start=None
    if not chunks:return []
    lo,hi=max(chunks,key=lambda q:q[1]-q[0])
    if hi-lo<16:return []
    found=[]
    for image in images:
        data=image['bytes'];at=data.find(code[lo:hi])
        while at>=0:
            begin=at-lo
            if begin>=0 and begin+len(code)<=len(data) and all(data[begin+a:begin+b]==code[a:b] for a,b in chunks):
                found.append((image['rva']+begin,data[begin:begin+len(code)]))
            at=data.find(code[lo:hi],at+1)
    return found

def targets(section,rva,actual,symbol):
    return {rva+r['offset']+4+(r['kind']-4)+struct.unpack_from('<i',actual,r['offset'])[0] for r in section['relocations'] if r['symbol']==symbol}

def guarded(section,ins):
    # Only __isa_available >= 6 (documented CRT AVX512 level) can admit EVEX.
    offsets={x['offset']:x for x in ins};order=list(offsets);pred={x:set() for x in order};guards=[]
    for i,x in enumerate(ins):
        following=ins[i+1]['offset'] if i+1<len(ins) else None
        branch=re.match(r'0x([0-9a-f]+)',x['operands']);dest=int(branch[1],16) if branch else None
        if x['mnemonic'].startswith('j'):
            if dest in pred:pred[dest].add(x['offset'])
            if x['mnemonic']!='jmp' and following is not None:pred[following].add(x['offset'])
        elif not x['mnemonic'].startswith('ret') and following is not None:pred[following].add(x['offset'])
        if x['mnemonic']=='cmpl' and x['operands'].startswith('$0x6,') and i+2<len(ins):
            refs=[r for r in section['relocations'] if x['offset']<=r['offset']<x['offset']+len(x['bytes']) and r['symbol']=='__isa_available']
            conditional=ins[i+1];good=ins[i+2]['offset']
            if refs and conditional['mnemonic']=='jl':guards.append((x['offset'],conditional['offset'],good))
    if not order:return []
    reachable={order[0]};changed=True
    while changed:
        changed=False
        for node in order:
            if node not in reachable and pred[node]&reachable:reachable.add(node);changed=True
    dom={x:({x} if x==order[0] else set(reachable)) for x in reachable};changed=True
    while changed:
        changed=False
        for node in order[1:]:
            if node not in reachable:continue
            pp=pred[node]&reachable
            value={node}|set.intersection(*(dom[x] for x in pp)) if pp else {node}
            if dom[node]!=value:dom[node]=value;changed=True
    valid=[(cmp,branch,good) for cmp,branch,good in guards if pred[branch]=={cmp} and pred[good]=={branch}]
    if all(any(good in dom.get(x['offset'],set()) for _,_,good in valid) for x in ins if evex(x)):
        return [dict(compare=cmp,conditional=branch,admitted=good) for cmp,branch,good in valid]
    return []

def trusted_jump_table_data(data,base,images,actual,hazards,llvm):
    """Only exact SDK strpbrk object contents and every relocation admit its tables."""
    libraries=sorted(Path('C:/Program Files (x86)/Windows Kits/10/Lib').glob('*/ucrt/x64/libucrt.lib'))
    if not libraries:return []
    library=libraries[-1]
    member=next((x for x in run([llvm/'llvm-ar.exe','t',library]).splitlines() if x.endswith('\\strpbrk.obj')),None)
    if not member:return []
    raw=subprocess.check_output([str(llvm/'llvm-ar.exe'),'p',str(library),member])
    sections=coff(raw);owner=next(x for x in sections if 'strpbrk' in x['functions'])
    fallback=next(x for x in sections if 'fallbackMethod' in x['functions'])
    fallback_starts={rva for rva,_ in matches(fallback,images)}
    offsets={r['offset'] for r in owner['relocations'] if r['kind']==3}
    tail=[];cursor=len(owner['bytes'])-4
    while cursor in offsets:tail.insert(0,cursor);cursor-=4
    if not tail:return []
    table_start=min(tail);table_end=len(owner['bytes'])
    if tail!=list(range(table_start,table_end,4)):return []
    # The table must follow a real ret and padding, not merely lack unwind info.
    if owner['bytes'][table_start-3:table_start]!=b'\xc3\x66\x90':return []
    peoff=struct.unpack_from('<L',data,0x3c)[0];opt=peoff+24
    exception_rva,exception_size=struct.unpack_from('<LL',data,opt+112+3*8)
    def at_rva(rva):
        n=struct.unpack_from('<H',data,peoff+6)[0];size=struct.unpack_from('<H',data,peoff+20)[0]
        for i in range(n):
            v,r,size_raw,ptr=struct.unpack_from('<LLLL',data,peoff+24+size+40*i+8)
            if r<=rva<r+size_raw:return ptr+rva-r
        raise ValueError('Unmapped PE RVA')
    entries=[struct.unpack_from('<LLL',data,at_rva(exception_rva)+i) for i in range(0,exception_size,12)]
    result=[]
    for rva,code in matches(owner,images,True):
        valid=True
        for rel in owner['relocations']:
            off=rel['offset'];kind=rel['kind']
            if kind==3:
                valid &= rel['symbol_section']==owner['index'] and struct.unpack_from('<L',code,off)[0]==rva+rel['symbol_value']+struct.unpack_from('<L',owner['bytes'],off)[0]
            elif rel['symbol']=='__ImageBase':
                valid &= rva+off+4+struct.unpack_from('<i',code,off)[0]==0
            elif rel['symbol']=='fallbackMethod':
                valid &= rva+off+4+struct.unpack_from('<i',code,off)[0] in fallback_starts
            else:valid=False
        lo,hi=base+rva+table_start,base+rva+table_end
        # Match the complete object extent in the Windows unwind function, and
        # never exempt a PE entry point, another runtime function or direct target.
        valid &= (rva,rva+table_end) in {(a,b) for a,b,_ in entries}
        entry=base+struct.unpack_from('<L',data,opt+16)[0]
        valid &= not lo<=entry<hi and not any(lo<=base+a<hi for a,_,_ in entries)
        for ins in actual:
            if lo<=ins['offset']<hi:continue
            if ins['mnemonic'].startswith(('j','call')):
                dest=re.match(r'0x([0-9a-f]+)',ins['operands'])
                if dest and lo<=int(dest[1],16)<hi:valid=False
        if not valid:continue
        excluded=[x for x in hazards if lo<=x['offset'] and x['offset']+len(x['bytes'])<=hi]
        if excluded:result.append(dict(function='strpbrk',start_rva=hex(rva),table_start_rva=hex(rva+table_start),table_end_rva=hex(rva+table_end),data_addresses=[hex(x['offset']) for x in excluded],object_sha256=sha(raw),archive_sha256=sha(library.read_bytes()),archive=str(library),relocations_verified=len(owner['relocations']),reason='Exact complete SDK object, all internal table RVAs and external relocations, ret/unwind boundary, no direct control-flow or entry target into data'))
    return result

def check(binary,llvm,crt):
    data=binary.read_bytes();base,images=pe(data)
    actual=instructions(run([llvm/'llvm-objdump.exe','-d','--section=.text',binary]));hazards=[x for x in actual if evex(x)]
    report=dict(file=str(binary),sha256=sha(data),decoded_evex=len(hazards),accepted=[],unmatched=[],scope='Known CRT function-content and feature-dispatch screen only; old-CPU execution is not claimed')
    if not hazards:return report
    report['proven_data']=trusted_jump_table_data(data,base,images,actual,hazards,llvm)
    data_addresses={int(x,16) for item in report['proven_data'] for x in item['data_addresses']}
    hazards=[x for x in hazards if x['offset'] not in data_addresses]
    if not hazards:return report
    objects={}
    with tempfile.TemporaryDirectory(prefix='rlplayer-crt-dispatch-') as tmp:
        for archive,suffix in [('libcpmt.lib','vector_algorithms.obj'),('libcmt.lib','cpu_disp.obj')]:
            library=crt/archive
            member=next(x for x in run([llvm/'llvm-ar.exe','t',library]).splitlines() if x.endswith('\\'+suffix))
            raw=subprocess.check_output([str(llvm/'llvm-ar.exe'),'p',str(library),member]);path=Path(tmp)/suffix;path.write_bytes(raw)
            objects[suffix]=(coff(raw),symbols_disasm(run([llvm/'llvm-objdump.exe','-dr',path])),sha(raw),sha(library.read_bytes()))
        initializer,_,init_sha,_=objects['cpu_disp.obj']
        init=next(x for x in initializer if '__isa_available_init' in x['functions'])
        # Pin the actual SDK CPU initializer, not an arbitrary global >=6.
        if b'\x0f\xa2' not in init['bytes'] or b'\x0f\x01\xd0' not in init['bytes']:raise ValueError('CRT initializer lacks CPUID/XGETBV')
        isa=set()
        init_matches=matches(init,images)
        for rva,code in init_matches:isa|=targets(init,rva,code,'__isa_available')
        if len(isa)!=1:raise ValueError('Cannot bind one matched CRT CPU-initializer __isa_available global')
        report['initializer_matches']=[dict(start_rva=hex(rva),size=len(code)) for rva,code in init_matches];report['isa_global_rva']=hex(next(iter(isa)));report['initializer_object_sha256']=init_sha
        sections,dis,object_sha,archive_sha=objects['vector_algorithms.obj']
        covered=set()
        for section in sections:
            for name in section['functions']:
                if not ('__std_minmax_disp' in name or '__std_minmax_impl' in name):continue
                ins=dis.get(name,[])
                if not any(evex(x) for x in ins):continue
                guards=guarded(section,ins)
                if not guards:continue
                for rva,code in matches(section,images):
                    if targets(section,rva,code,'__isa_available')!=isa:continue
                    addresses={base+rva+x['offset'] for x in ins if evex(x)}
                    present=addresses&{x['offset'] for x in hazards}
                    if not present:continue
                    covered|=present
                    report['accepted'].append(dict(function=name,start_rva=hex(rva),size=len(code),evex_addresses=[hex(x) for x in sorted(present)],guards=guards,object_sha256=object_sha,archive_sha256=archive_sha))
        report['unmatched']=[dict(address=hex(x['offset']),mnemonic=x['mnemonic']) for x in hazards if x['offset'] not in covered]
    return report

def default_crt():
    if os.environ.get('VCToolsInstallDir'):
        return Path(os.environ['VCToolsInstallDir'])/'lib/x64'
    candidates=sorted(Path('C:/Program Files (x86)/Microsoft Visual Studio/2022').glob('*/VC/Tools/MSVC/*/lib/x64'))
    if not candidates:raise ValueError('No trusted MSVC CRT archive directory; specify --crt-dir')
    return candidates[-1]

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('binary',type=Path);p.add_argument('--llvm-dir',type=Path,default=Path('C:/Program Files/AMD/ROCm/7.2/bin'));p.add_argument('--crt-dir',type=Path,default=default_crt());p.add_argument('--report',type=Path,required=True);a=p.parse_args()
    try:r=check(a.binary,a.llvm_dir,a.crt_dir);rc=1 if r['unmatched'] else 0
    except Exception as e:r=dict(file=str(a.binary),error=str(e));rc=1
    a.report.write_text(json.dumps(r,indent=2),encoding='utf-8');print('CPU_DISPATCH', 'FAIL' if rc else 'PASS',str(a.binary),'decoded_evex='+str(r.get('decoded_evex')),'known_functions='+str(len(r.get('accepted',[]))));return rc
if __name__=='__main__':raise SystemExit(main())
