"""Generate a deterministic filter-cache source/toolchain/options fingerprint.
Usage: python tools/review4_cache_fingerprint.py --source SOURCE --compiler CLANG --flags "EXACT OPTIONS" --output generated.h
For Windows static CRT add --auto-runtime-libs and repeat --runtime-probe-arg=OPTION
for the actual target/CRT driver options. -### runs neither compiler nor linker.
All archives in the discovered search directories are hashed conservatively,
including nested DEFAULTLIB dependencies; dynamic CRT or failed discovery is fatal.
Feed the result via -include generated.h. Re-run whenever any DSP header or actual
compile option/compiler identity changes. Hashing all DSP headers deliberately
invalidates more often than necessary; it avoids an incomplete manual dependency list.
"""
import argparse, hashlib, json, locale, os, re, shlex, subprocess
from pathlib import Path
p=argparse.ArgumentParser()
p.add_argument('--source',required=True,type=Path)
p.add_argument('--compiler',required=True,type=Path)
p.add_argument('--flags',required=True)
p.add_argument('--output',required=True,type=Path)
p.add_argument('--auto-runtime-libs',action='store_true',help='Require and hash the actual Windows Clang linker library search set discovered via -###')
p.add_argument('--runtime-probe-arg',action='append',default=[],help='Actual driver option for -###; use --runtime-probe-arg=-fms-runtime-lib=static; repeat as needed')
p.add_argument('--runtime-file',action='append',type=Path,default=[],help='Optional linked math/CRT artifact to bind exactly; repeat as needed')
a=p.parse_args()
def sha256_file(path):
    h=hashlib.sha256()
    with path.open('rb') as f:
        for block in iter(lambda:f.read(1024*1024),b''):
            h.update(block)
    return h.hexdigest()

def compiler_identity():
    is_cl=a.compiler.name.lower() in {'cl','cl.exe'}
    option='/Bv' if is_cl else '--version'
    result=subprocess.run([str(a.compiler),option],capture_output=True)
    raw=result.stdout+result.stderr
    text=None
    for encoding in ('utf-8','mbcs',locale.getpreferredencoding(False)):
        try:
            text=raw.decode(encoding)
            break
        except (UnicodeDecodeError,LookupError):
            pass
    if text is None:text=raw.decode('utf-8',errors='replace')
    if is_cl:
        # /Bv without a source intentionally emits its identity plus D8003.
        # Accept ONLY that diagnosed no-input failure, never arbitrary cl errors.
        identity=re.search(r'Microsoft\s+\(R\).*?C/C\+\+[^\r\n]*?\b\d+\.\d+\.\d+',text)
        errors=re.findall(r'\berror\s+([A-Z]\d{4})\b',text,re.IGNORECASE)
        expected_no_input=result.returncode in {1,2} and errors==['D8003']
        if not identity or not ((result.returncode==0 and not errors) or expected_no_input):
            raise RuntimeError('MSVC /Bv identity failed: returncode='+str(result.returncode)+
                               ', errors='+repr(errors)+'; refusing an unverified compiler identity')
    else:
        recognized=re.search(r'(?:clang version|gcc|g\+\+|Intel[^\r\n]*Compiler|icx|icpx)',text,re.IGNORECASE)
        if result.returncode!=0 or not recognized:
            raise RuntimeError('Compiler --version identity failed or was not a recognized compiler: returncode='+
                               str(result.returncode)+'; refusing an unverified compiler identity')
    return text,{'option':option,'returncode':result.returncode,
                 'expected_no_source':bool(is_cl and result.returncode!=0)}

def discover_runtime_libraries():
    # -### executes neither cc1 nor linker and creates no object/executable.
    command=[str(a.compiler),'-###',*a.runtime_probe_arg,'-x','c++','-','-o','NUL']
    result=subprocess.run(command,input='',capture_output=True,text=True,check=True)
    trace=result.stdout+result.stderr
    link_lines=[line for line in trace.splitlines() if '-libpath:' in line.lower() or '/libpath:' in line.lower()]
    if len(link_lines)!=1:
        raise RuntimeError('Cannot identify exactly one MSVC linker command from Clang -###; runtime remains unbound')
    cc1_lines=[line for line in trace.splitlines() if '"-cc1"' in line]
    cc1_tokens=[token for line in cc1_lines for token in shlex.split(line,posix=True)]
    if any(token in {'-D_DLL','_DLL','_DLL=1'} for token in cc1_tokens):
        raise RuntimeError('Dynamic CRT selected; hashing import archives does not bind runtime DLL bytes; runtime remains unbound')
    if not any(token in {'-D_MT','_MT','_MT=1'} for token in cc1_tokens):
        raise RuntimeError('Cannot verify static multithread CRT from driver cc1 command; runtime remains unbound')
    tokens=shlex.split(link_lines[0],posix=True)
    search_dirs=[]
    for token in tokens:
        if token.lower().startswith(('-libpath:','/libpath:')):
            search_dirs.append(Path(token.split(':',1)[1]).resolve())
    search_dirs.extend(Path(x).resolve() for x in os.environ.get('LIB','').split(';') if x)
    search_dirs=list(dict.fromkeys(search_dirs))
    resolved_dirs=[d for d in search_dirs if d.is_dir()]
    missing_dirs=[str(d) for d in search_dirs if not d.is_dir()]
    libraries=set()
    for directory in resolved_dirs:
        libraries.update(x.resolve() for x in directory.iterdir() if x.is_file() and x.suffix.lower()=='.lib')
    # Covers nested archive DEFAULTLIB dependencies without guessing their names:
    # bind the ENTIRE real search set, deliberately over-invalidating the cache.
    defaults=[]
    for token in tokens:
        if token.lower().startswith(('-defaultlib:','/defaultlib:')):
            name=token.split(':',1)[1]
            if not name.lower().endswith('.lib'):name+='.lib'
            found=next((directory/name for directory in resolved_dirs if (directory/name).is_file()),None)
            if found is None:
                raise RuntimeError('Unresolved linker DEFAULTLIB '+name+'; runtime remains unbound')
            defaults.append(str(found.resolve()))
        elif token.lower().endswith('.lib'):
            library=Path(token)
            if not library.is_absolute() or not library.is_file():
                raise RuntimeError('Unresolved explicit linker library '+token+'; runtime remains unbound')
            libraries.add(library.resolve())
    if not libraries or not defaults:
        raise RuntimeError('No runtime/default libraries discovered; runtime remains unbound')
    return sorted(libraries,key=lambda x:str(x).lower()),{
        'binding':'auto-discovered-complete-library-search-set',
        'probe_args':a.runtime_probe_arg,
        'linker':tokens[0],
        'library_search_dirs':[str(x) for x in resolved_dirs],
        'all_library_search_dirs':[str(x) for x in search_dirs],
        'missing_optional_search_dirs':missing_dirs,
        'resolved_driver_defaults':defaults,
        'library_count':len(libraries),
        'archive_bytes':sum(x.stat().st_size for x in libraries)}

runtime_files=list(a.runtime_file)
runtime_binding={'binding':'explicit-unverified' if runtime_files else 'unbound'}
if a.auto_runtime_libs:
    automatic,runtime_binding=discover_runtime_libraries()
    runtime_files.extend(automatic)
runtime_files=sorted(set(x.resolve() for x in runtime_files),key=lambda x:str(x).lower())
headers=sorted(x for x in (a.source/'src'/'dsp').rglob('*') if x.suffix in {'.h','.hpp','.inl'})
version,compiler_probe=compiler_identity()
files={x.relative_to(a.source).as_posix():sha256_file(x) for x in headers}
payload={'schema':1,'generator_sha256':hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),'compiler':version,'compiler_identity_probe':compiler_probe,'compiler_binary_sha256':sha256_file(a.compiler),'actual_flags':a.flags,'source_headers':files,'runtime_binding':runtime_binding,'runtime_library_directories':runtime_binding.get('all_library_search_dirs',[]),'linked_runtime_artifacts':{str(x):sha256_file(x) for x in runtime_files}}
blob=json.dumps(payload,ensure_ascii=False,sort_keys=True,separators=(',',':')).encode('utf-8')
digest=hashlib.sha256(blob).hexdigest()
a.output.parent.mkdir(parents=True,exist_ok=True)
def write_changed(path,text):
    if not path.exists() or path.read_text(encoding='utf-8') != text:
        path.write_text(text,encoding='utf-8')
write_changed(a.output,'#pragma once\n#define RTA_FILTER_CACHE_BUILD_FINGERPRINT "'+digest+'"\n#define RTA_FILTER_CACHE_RUNTIME_BOUND '+str(int(a.auto_runtime_libs))+'\n')
write_changed(a.output.with_suffix('.json'),json.dumps({'fingerprint':digest,**payload},ensure_ascii=False,indent=2))
print(json.dumps({'fingerprint':digest,'headers':len(headers),'output':str(a.output),'runtime_binding':runtime_binding}))
