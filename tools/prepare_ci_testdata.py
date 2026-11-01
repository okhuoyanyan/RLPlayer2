"""Create copyright-free encoded media for the unchanged native-tag A/B test.

Only ffmpeg lavfi silence is used; no music, network, or audio endpoint is read.
The test deliberately discovers build/testdata relative to its repository root.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument('--ffmpeg', default='ffmpeg')
    args = parser.parse_args()
    ffmpeg = shutil.which(args.ffmpeg)
    if not ffmpeg:
        parser.error('ffmpeg must be installed before preparing CI fixtures')
    folder = args.root.resolve() / 'build' / 'testdata' / 'ci-synthetic'
    folder.mkdir(parents=True, exist_ok=True)
    records = []
    codecs = {'wav': ['-c:a', 'pcm_s16le'], 'flac': ['-c:a', 'flac', '-sample_fmt', 's16'],
              'mp3': ['-c:a', 'libmp3lame', '-b:a', '128k', '-id3v2_version', '3'],
              'ogg': ['-c:a', 'libvorbis', '-q:a', '4']}
    for extension, codec in codecs.items():
        for index in range(1, 6):
            path = folder / f'ci-{index}.{extension}'
            command = [ffmpeg, '-nostdin', '-hide_banner', '-loglevel', 'error', '-y',
                       '-f', 'lavfi', '-i', 'anullsrc=r=44100:cl=stereo', '-t', '1',
                       *codec, '-metadata', f'title=CI Silent {index}', '-metadata',
                       'artist=RLPlayer CI', '-metadata', 'album=Synthetic fixtures', str(path)]
            subprocess.run(command, check=True)
            records.append({'path': str(path.relative_to(args.root.resolve())),
                            'bytes': path.stat().st_size,
                            'sha256': hashlib.sha256(path.read_bytes()).hexdigest()})
    manifest = {'source': 'ffmpeg lavfi anullsrc (synthetic digital silence)', 'files': records}
    (folder / 'manifest.json').write_text(json.dumps(manifest, indent=2), encoding='utf-8')
    print(json.dumps(manifest, indent=2))


if __name__ == '__main__':
    main()
