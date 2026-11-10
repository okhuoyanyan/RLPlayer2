"""Bundle the exact local FFmpeg/zlib source archives and build recipe."""
import argparse
import hashlib
import json
import pathlib
import shutil
import subprocess


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--work-dir',type=pathlib.Path,required=True)
    ap.add_argument('--out-dir',type=pathlib.Path,required=True)
    args=ap.parse_args()
    work=args.work_dir.resolve();out=args.out_dir.resolve()
    tools=pathlib.Path(__file__).resolve().parent
    files={
        'ffmpeg-9.0.tar.xz':work/'ffmpeg-9.0.tar.xz',
        'zlib-1.3.2.tar.xz':work/'zlib-1.3.2.tar.xz',
        'FFmpeg-LICENSE.md':work/'src/ffmpeg-9.0/LICENSE.md',
        'COPYING.GPLv2':work/'src/ffmpeg-9.0/COPYING.GPLv2',
        'COPYING.LGPLv2.1':work/'src/ffmpeg-9.0/COPYING.LGPLv2.1',
        'zlib-LICENSE':work/'zlib-1.3.2/LICENSE',
        'configure.log':work/'configure_lite.log',
    }
    for name in ('build_ffmpeg_lite.ps1','verify_ffmpeg_lite.ps1','verify_ffmpeg_png.py',
                 'ffmpeg_lite_required.txt','write_ffmpeg_build_manifest.py'):
        files[name]=tools/name
    if digest(files['zlib-1.3.2.tar.xz']) != 'd7a0654783a4da529d1bb793b7ad9c3318020af77667bcae35f95d0e42a792f3':
        raise ValueError('Pinned zlib source archive does not match upstream SHA256')
    for path in (*files.values(),out/'ffmpeg.exe',out/'ffprobe.exe'):
        if not path.is_file() or path.stat().st_size==0:
            raise ValueError(f'Missing or empty FFmpeg redistribution input: {path}')
    destination=out/'redistribution';destination.mkdir(exist_ok=True)
    manifest={
        'schema':1,'distribution':'RLPlayer audio FFmpeg, statically linked zlib',
        'ffmpeg_version':'9.0','zlib_version':'1.3.2',
        'source_urls':['https://ffmpeg.org/releases/ffmpeg-9.0.tar.xz','https://zlib.net/zlib-1.3.2.tar.xz'],
        'source_changes':'Unmodified upstream archives; configure/build recipe included',
        'binaries':{name:{'sha256':digest(out/name),'bytes':(out/name).stat().st_size} for name in ('ffmpeg.exe','ffprobe.exe')},
        'files':{},
        'build_inputs':{name:digest(work/path) for name,path in {
            'gcc.exe':'mingw64/bin/gcc.exe','libz.a':'zlib-static/lib/libz.a',
            'configure-script':'src/ffmpeg-9.0/configure','nasm.exe':'nasm-2.16.03/nasm.exe'}.items()}}
    for name,source in files.items():
        expected=digest(source);shutil.copyfile(source,destination/name)
        if digest(destination/name)!=expected or digest(source)!=expected:
            raise RuntimeError('Redistribution input changed during copy: '+name)
        manifest['files'][name]={'sha256':expected,'bytes':source.stat().st_size}
    result=subprocess.run([str(out/'ffmpeg.exe'),'-hide_banner','-version'],capture_output=True,check=True)
    manifest['binary_version_output']=result.stdout.decode('utf-8','replace')
    (destination/'BUILD-MANIFEST.json').write_text(json.dumps(manifest,ensure_ascii=False,indent=2),encoding='utf-8')
    print('FFMPEG_SOURCE_BUNDLE_PASS',len(files),'files',sum(v['bytes'] for v in manifest['files'].values()),'bytes')


if __name__=='__main__':
    main()
