#!/usr/bin/env python3
"""Deterministic allowlisted RLPlayer source snapshots (Python standard library only).

Usage: python tools/make_source_archive.py --source-root D:/RLPlayer-Next-20261109
       --output D:/RLPlayer-Next-20261109/dist/RLPlayer-3.3.0-source.zip

The ZIP includes the working files, including uncommitted changes. It does not
claim to contain upstream FFmpeg/SDK sources that are not in the allowed roots.
"""
from __future__ import annotations
import argparse
import base64
import binascii
from dataclasses import dataclass
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import sys
import tempfile
import zipfile

ROOTS = ('src', 'tests', 'tools', 'res', 'data', 'third_party/webview2',
         'third_party/sacd-reference')
ROOT_FILES = ('CMakeLists.txt', 'LICENSE', 'README.md', 'README.txt',
              'THIRD-PARTY-NOTICES.txt', 'OPRA-NOTICE.txt', 'verify.ps1',
              'verify-stream.ps1', '.gitattributes', '.gitignore',
              'server/rlplayer_service.py', 'server/rlplayer_publish.py',
              'server/admin_ui.py', 'server/geoip.py')
WEBVIEW_FILES = ('third_party/webview2/include/WebView2.h',
                 'third_party/webview2/include/WebView2EnvironmentOptions.h',
                 'third_party/webview2/x64/WebView2Loader.dll')
SACD_FILES = tuple('third_party/sacd-reference/' + name for name in (
    'COPYING.LGPLv2.1', 'COPYING.SACD', 'NOTICE.txt', 'PROVENANCE.json',
    'dstdec-reference.c', 'dvd_udf-reference.c', 'pcm-dvda-reference.c',
    'scarletbook.h', 'scarletbook_read.c'))
DATA_FILES = ('data/opra_index.json', 'data/webview2-runtime-pin.json') + tuple('data/i18n/' + lang + '.json'
                                             for lang in ('ar','en','es','fr','ja','ru'))
TEST_FIXTURE_FILES = ('tests/fixtures/waveform336.flac',)
REQUIRED = ROOT_FILES[:8] + ('src/version.h', 'res/rlplayer.ico', 'res/rlplayer.rc.in') + \
           WEBVIEW_FILES + SACD_FILES + DATA_FILES + TEST_FIXTURE_FILES
CODE_EXTENSIONS = {'.h','.hpp','.hh','.hxx','.c','.cc','.cpp','.cxx','.cu','.cuh',
                   '.inc','.inl','.ipp','.tcc','.in','.cmake','.def','.rc','.manifest',
                   '.ps1','.py','.js','.mjs','.cjs','.ts','.sh','.bat','.cmd',
                   '.json','.txt','.md','.yml','.yaml','.css','.html','.svg'}
SKIP_DIRS = {'.git','.svn','.hg','.vs','.vscode','.idea','.venv','venv',
             '__pycache__','.pytest_cache','node_modules','build','dist','out',
             'cache','caches','tmp','temp','downloads','profiles','rlplayer-meta',
             'music','recordings','testdata','.ssh','.aws','secrets','secret'}
SECRET_BASENAMES = {'key','key.txt','credentials','credentials.json','secrets.json',
                    'id_rsa','id_ed25519','id_dsa','id_ecdsa'}
PRIVATE_PEM = re.compile(rb'(?m)^[\t \xef\xbb\xbf\xff\xfe]*-----BEGIN (?P<label>[^\r\n-]*PRIVATE KEY(?: BLOCK)?)-----[\t ]*\r?$')
CREDENTIAL_PATTERNS = (
    ('API token', re.compile(rb'(?<![A-Za-z0-9])sk-(?:proj-|svcacct-)?[A-Za-z0-9_-]{20,}(?![A-Za-z0-9])')),
    ('GitHub token', re.compile(rb'(?<![A-Za-z0-9])(?:gh[pousr]_[A-Za-z0-9]{30,}|github_pat_[A-Za-z0-9_]{30,})')),
    ('cloud access key', re.compile(rb'(?<![A-Za-z0-9])(?:AKIA|ASIA)[A-Z0-9]{16}(?![A-Za-z0-9])')),
)
# Two nonfunctional mock credentials verified in the test transport/fixture
# code. Exact source-path plus token SHA only; no generic "test key" exemption.
MOCK_CREDENTIALS = {
    ('tests/test_metallm.cpp', 'c04e5fee6a04723cd5a7598934f023b85790d4008994aeff26d1adb283891961'),
    ('tests/test_quality_ai.cpp', 'f2ede4328a46bf52821f7ad503462c19948d320fbdd287bb1ae7a4eb03a92c5a'),
}
FIXED_DATE = (1980, 1, 1, 0, 0, 0)

class ArchiveError(Exception):
    pass

@dataclass(frozen=True)
class SourceFile:
    path: str
    source: Path
    signature: tuple[int, int, int]

def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()

def json_bytes(value: object) -> bytes:
    return (json.dumps(value, ensure_ascii=False, sort_keys=True, indent=2) + '\n').encode('utf-8')

def checked_name(path: str) -> None:
    if any(ord(c) < 32 for c in path) or '\\' in path or ':' in path:
        raise ArchiveError('Unsafe archive pathname (control character or drive syntax)')
    if any(p in ('', '.', '..') for p in path.split('/')):
        raise ArchiveError('Unsafe archive pathname component')

def no_reparse(path: Path) -> None:
    info = path.lstat()
    if path.is_symlink() or getattr(info, 'st_file_attributes', 0) & 0x400:
        raise ArchiveError('Refusing symbolic-link/reparse source path: ' + path.name)

def signature(path: Path) -> tuple[int, int, int]:
    s = path.stat()
    return (s.st_size, s.st_mtime_ns, getattr(s, 'st_ino', 0))

def excluded_reason(rel: str) -> str | None:
    p = Path(rel)
    parts = tuple(x.lower() for x in p.parts)
    name = parts[-1]
    if name == '.env' or name.startswith('.env.') or name in SECRET_BASENAMES or \
       name.endswith('.key') or (name.startswith('private') and name.endswith('.pem')):
        return 'secret-name'
    if any(x in SKIP_DIRS or x.startswith('build-') or x.startswith('build_') for x in parts[:-1]):
        return 'state/cache/build/secret-directory'
    if '.bak' in name or name.endswith(('~','.orig','.rej','.tmp','.temp')):
        return 'backup/temporary-file'
    if name.startswith(('rlplayer-', 'rta-')) and p.suffix.lower() in {
        '.conf','.ini','.log','.tsv','.db','.sqlite','.json'}:
        return 'runtime-state'
    if rel.startswith('res/'):
        if len(parts) != 2:
            return 'unreferenced-icon-experiments'
        return None if p.suffix.lower() in {'.ico','.png','.rc','.in','.manifest'} else 'unreviewed-resource'
    if rel.startswith('data/'):
        return None if rel in DATA_FILES else 'unreviewed-data/runtime-state'
    if rel.startswith('third_party/webview2/'):
        return None if rel in WEBVIEW_FILES else 'unreviewed-vendor-resource'
    if rel.startswith('third_party/sacd-reference/'):
        return None if rel in SACD_FILES else 'unreviewed-vendor-resource'
    if rel in ROOT_FILES or rel in TEST_FIXTURE_FILES:
        return None
    return None if p.suffix.lower() in CODE_EXTENSIONS else 'non-source/media/binary-file'

def inventory(root: Path) -> tuple[list[SourceFile], list[dict[str, str]]]:
    files: list[SourceFile] = []
    excluded: list[dict[str, str]] = []
    seen: set[str] = set()
    def inspect(p: Path) -> None:
        no_reparse(p)
        rel = p.relative_to(root).as_posix()
        checked_name(rel)
        reason = excluded_reason(rel)
        if reason:
            excluded.append({'path':rel, 'reason':reason})
            return
        if not p.is_file():
            raise ArchiveError('Non-regular source file: ' + rel)
        key = rel.casefold()
        if key in seen:
            raise ArchiveError('Case-insensitive archive pathname collision: ' + rel)
        seen.add(key)
        files.append(SourceFile(rel, p, signature(p)))
    for name in ROOT_FILES:
        p = root/name
        if p.exists():
            inspect(p)
    for name in ROOTS:
        base = root/name
        if not base.is_dir():
            raise ArchiveError('Required source directory is missing: ' + name)
        no_reparse(base)
        # Enumerate every candidate path for the exclusion report, but never
        # follow directory links. Only files selected by the fixed policy ship.
        for current, dirs, names in os.walk(base, followlinks=False):
            for d in dirs:
                no_reparse(Path(current)/d)
            for name in names:
                inspect(Path(current)/name)
    files.sort(key=lambda f: f.path)
    excluded.sort(key=lambda e: e['path'])
    present = {f.path for f in files}
    # The source archive must reproduce every reviewed test registration.
    # Only four explicit server modules are admitted; never ship live server
    # state, deployment configuration, credentials or IP lookup databases.
    registration = root/'tools/safe_test_manifest.json'
    if registration.is_file():
        audited = json.loads(registration.read_text(encoding='utf-8-sig'))
        dependencies = {name for row in audited['tests'] for name in row.get('sources', [])}
        absent = sorted(dependencies-present)
        if absent:
            raise ArchiveError('Reviewed test source missing from archive: '+', '.join(absent))
    missing = [name for name in REQUIRED if name not in present]
    if missing:
        raise ArchiveError('Required build/license resources missing: ' + ', '.join(missing))
    return files, excluded

def has_private_pem(data: bytes) -> bool:
    # A quoted/regex detection pattern is source, not a key. Require an actual
    # line-delimited BEGIN/END block with a base64 payload. Legacy encrypted
    # PEM and PGP armor may have metadata/checksum lines around the payload.
    views = [data]
    if b'\x00' in data:
        views.append(data.replace(b'\x00', b''))
    for view in views:
        for start in PRIVATE_PEM.finditer(view):
            end_marker = b'-----END '+start.group('label')+b'-----'
            end = view.find(end_marker, start.end())
            if end < 0:
                continue
            lines = view[start.end():end].strip().splitlines()
            body = b''.join(line.strip() for line in lines
                            if line.strip() and b':' not in line and not line.startswith(b'='))
            if len(body) >= 32:
                try:
                    if len(base64.b64decode(body, validate=True)) >= 16:
                        return True
                except (binascii.Error, ValueError):
                    pass
    return False

def source_readme(version: str) -> bytes:
    return ('''RLPlayer ''' + version + ''' corresponding-source snapshot

This is a snapshot of the actual working source, including uncommitted changes.
The immutable SHA256 list identifies every delivered file. Existing LICENSE and
third-party notices/copyright text are preserved without relicensing vendors.

Scope: player source, tests, tools, production resources/data, WebView2 build
headers/loader, and the retained SACD/DST/DVD-Audio reference sources/notices.
Build outputs, caches, recordings, local runtime configuration, secret filenames,
source backups and unused icon experiments are intentionally excluded.

This is NOT the complete source of every third-party binary in a player package.
Full upstream FFmpeg/zlib source archives and recipes are provided separately
by the full release's third-party payload; this player-source ZIP complements
those payloads. Proprietary AMD/NVIDIA SDK/runtime and Microsoft runtime sources
are not included here. The build scripts identify external dependencies and
their separate licenses still apply.

CPU application build prerequisites: Windows 11, MSVC/Windows SDK, Python,
CMake, Ninja and the documented Clang C++23 compiler. Run these from this folder
in PowerShell after installing those tools (no audio device is opened by building):

python -m pip install --target .source-audit-deps -r tools/requirements-audit.txt -i https://pypi.tuna.tsinghua.edu.cn/simple
$deps = (Resolve-Path -LiteralPath .source-audit-deps).Path
cmake -G Ninja -S . -B build-source -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER="C:/Program Files/AMD/ROCm/7.2/bin/clang++.exe" -DCMAKE_RC_COMPILER="C:/Program Files/AMD/ROCm/7.2/bin/llvm-rc.exe" "-DRTA_CERTIFICATE_PYTHON_PATH=$deps"
cmake --build build-source --parallel 2

GPU kernels and a self-contained distribution additionally require the vendor
SDKs and external dependencies documented in tools/build_gpu*.ps1,
tools/build_ffmpeg_lite.ps1 and tools/make_dist.ps1. Full package generation also
requires the release documentation and runtime payloads prepared by the release
workflow; those generated/non-source payloads are not claimed as part of this ZIP.

Reproducibility: lexical entry order, fixed 1980 timestamps, fixed file modes and
ZIP_STORED (no compressor-version variability). No absolute paths or wall-clock
metadata enter the ZIP. SOURCE_SHA256SUMS.txt excludes itself to avoid recursion.
''').encode('utf-8')

def create_archive(source_root: Path, output: Path, prefix: str | None = None,
                   overwrite: bool = False) -> dict[str, object]:
    root = Path(os.path.abspath(source_root))
    for ancestor in (root, *root.parents):
        no_reparse(ancestor)
    if not root.is_dir():
        raise ArchiveError('Source root is not a directory')
    output = Path(os.path.abspath(output))
    if output.suffix.lower() != '.zip':
        raise ArchiveError('Output must have a .zip extension')
    try:
        rel_output = output.relative_to(root).as_posix()
    except ValueError:
        rel_output = ''
    if any(rel_output == name or rel_output.startswith(name+'/') for name in ROOTS):
        raise ArchiveError('Refusing to write output inside selected source directories')
    sidecars = [output, Path(str(output)+'.manifest.json'),
                Path(str(output)+'.source-sha256.txt'), Path(str(output)+'.sha256')]
    for p in sidecars:
        if os.path.lexists(p) and not overwrite:
            raise ArchiveError('Output exists; use --overwrite explicitly: ' + p.name)
    files, excluded = inventory(root)
    payload: dict[str, bytes] = {}
    for f in files:
        data = f.source.read_bytes()
        if signature(f.source) != f.signature:
            raise ArchiveError('Source changed during snapshot: ' + f.path)
        # Plaintext UTF-8 and UTF-16 disguised as ordinary source are both checked.
        # Never include or echo the offending content in an error/report.
        if has_private_pem(data):
            raise ArchiveError('Refusing private-key content in source path: ' + f.path)
        for label, pattern in CREDENTIAL_PATTERNS:
            for token in pattern.finditer(data):
                if (f.path, digest(token.group())) not in MOCK_CREDENTIALS:
                    raise ArchiveError('Refusing credential content ('+label+') in source path: '+f.path)
        payload[f.path] = data
    if any(signature(f.source) != f.signature for f in files):
        raise ArchiveError('Source tree changed while taking the snapshot; retry when stable')
    versions = re.findall(rb'^\s*#define\s+RLPLAYER_VERSION\s+"([0-9]+\.[0-9]+\.[0-9]+)"',
                          payload['src/version.h'], re.M)
    if len(versions) != 1:
        raise ArchiveError('Required src/version.h must define one semantic RLPLAYER_VERSION')
    version = versions[0].decode('ascii')
    prefix = prefix or ('RLPlayer-'+version+'-source')
    checked_name(prefix)
    if '/' in prefix:
        raise ArchiveError('Archive prefix must be one directory name')
    payload['SOURCE_ARCHIVE_README.txt'] = source_readme(version)
    entries = [{'path':p,'bytes':len(b),'sha256':digest(b)} for p,b in sorted(payload.items())]
    manifest = {'format':'rlplayer-source-v1', 'version':version,
                'source_file_count':len(files), 'payload_file_count':len(entries),
                'source_bytes':sum(len(payload[f.path]) for f in files),
                'files':entries, 'allowed_roots':list(ROOTS),
                'private_key_guard':'Actual PEM private-key blocks reject the archive; quoted detection patterns are legal',
                'credential_guard':'API/GitHub/cloud key patterns rejected; only exact reviewed mock test tokens exempt',
                'reviewed_mock_credentials':[{'path':p,'sha256':h} for p,h in sorted(MOCK_CREDENTIALS)],
                'external_dependency_source_not_claimed':['full FFmpeg/zlib upstream archives',
                     'proprietary GPU SDK/runtime sources','Microsoft runtime sources']}
    manifest_data = json_bytes(manifest)
    # Excluded local/cache filenames are audit evidence only, never ZIP input:
    # adding a cache must not change a source-identical archive or leak its name.
    audit_manifest_data = json_bytes({**manifest, 'excluded':excluded})
    payload['SOURCE_MANIFEST.json'] = manifest_data
    sums = ''.join(digest(b)+'  '+p+'\n' for p,b in sorted(payload.items())).encode('utf-8')
    payload['SOURCE_SHA256SUMS.txt'] = sums
    for ancestor in (output.parent, *output.parent.parents):
        if os.path.lexists(ancestor):
            no_reparse(ancestor)
    output.parent.mkdir(parents=True, exist_ok=True)
    for ancestor in (output.parent, *output.parent.parents):
        no_reparse(ancestor)
    staged: list[Path] = []
    try:
        handle, temp_name = tempfile.mkstemp(prefix='.rlplayer-source-',suffix='.zip',dir=output.parent)
        os.close(handle)
        temporary = Path(temp_name)
        staged.append(temporary)
        with zipfile.ZipFile(temporary,'w',compression=zipfile.ZIP_STORED,allowZip64=True) as z:
            z.comment=b'RLPlayer deterministic source snapshot v1'
            for name,data in sorted(payload.items()):
                info=zipfile.ZipInfo(prefix+'/'+name,FIXED_DATE)
                info.create_system=3
                info.external_attr=(0o100755 if name.endswith('.sh') else 0o100644)<<16
                info.compress_type=zipfile.ZIP_STORED
                z.writestr(info,data)
        zip_hash=digest(temporary.read_bytes())
        extras=[audit_manifest_data,sums,(zip_hash+'  '+output.name+'\n').encode('utf-8')]
        for final,data in zip(sidecars[1:],extras):
            fd,name=tempfile.mkstemp(prefix='.rlplayer-source-',dir=output.parent)
            with os.fdopen(fd,'wb') as f:
                f.write(data)
            staged.append(Path(name))
        for temp,final in zip(staged,sidecars):
            if os.path.lexists(final) and not overwrite:
                raise ArchiveError('Output appeared during creation; refusing replacement: '+final.name)
            os.replace(temp,final)
        return {'archive':str(output),'sha256':zip_hash,'archive_bytes':output.stat().st_size,
                'version':version,'source_files':len(files),'archive_entries':len(payload),
                'source_bytes':manifest['source_bytes'],'excluded_files':len(excluded),
                'manifest':str(sidecars[1]),'source_checksums':str(sidecars[2]),
                'archive_checksum':str(sidecars[3])}
    finally:
        for p in staged:
            if p.exists():
                p.unlink()

def main() -> int:
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-root',type=Path,default=Path(__file__).resolve().parents[1])
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--prefix',help='Single top-level directory; default RLPlayer-<version>-source')
    parser.add_argument('--overwrite',action='store_true',help='Explicitly replace this archive and its sidecars')
    args=parser.parse_args()
    try:
        result=create_archive(args.source_root,args.output,args.prefix,args.overwrite)
    except (ArchiveError,OSError,ValueError) as e:
        print('SOURCE_ARCHIVE_REJECTED: '+str(e),file=sys.stderr)
        return 1
    print(json.dumps(result,ensure_ascii=False,sort_keys=True))
    return 0
if __name__=='__main__':
    raise SystemExit(main())
