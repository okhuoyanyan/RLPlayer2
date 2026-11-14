"""Offline packaged PNG and attached-art contract; Python standard library only.

The embedded FLAC contains 441 original 16-bit samples at <= -60 dBFS. No
generator FFmpeg, PATH codec, network or audio device participates in this test.
"""
import argparse
import base64
import hashlib
import json
import os
import pathlib
import struct
import subprocess
import tempfile
import zlib

FLAC = ('ZkxhQ4AAACIBuQG5AACWAACWCsRA8AAAAbnWoMahnLBiFdbNh/RhKycw//h5CAABuFVMAAAABAAJAA0AEQAUABfnKtJCWjFHtR9bb9mdsMACSZ5PJz5zOTk5JmcmTkycmZnMycmTMzJmZycnknOTwmTOTzJnJkzk5yTMmczkyeSSZOT5knJOTJJyTJyZk5mTnMk55mZMzOZknM5mZOTnJ+ZknPJyTmZOeScmTySZPJ8yTkmeSTJnkyc+TJzk5C6r')


def png_pixels(data):
    """Small independent RGB8 PNG decoder for the exact two-by-two oracle."""
    if data[:8] != b'\x89PNG\r\n\x1a\n':
        raise ValueError('Not PNG')
    compressed = bytearray()
    offset = 8
    header = None
    while offset < len(data):
        size = int.from_bytes(data[offset:offset+4], 'big')
        kind = data[offset+4:offset+8]
        body = data[offset+8:offset+8+size]
        crc = data[offset+8+size:offset+12+size]
        if len(crc) != 4 or zlib.crc32(kind+body) != int.from_bytes(crc, 'big'):
            raise ValueError('PNG CRC mismatch')
        if kind == b'IHDR':
            header = struct.unpack('>IIBBBBB', body)
        if kind == b'IDAT':
            compressed.extend(body)
        offset += 12+size
        if kind == b'IEND':
            break
    if header != (2, 2, 8, 2, 0, 0, 0) or offset != len(data):
        raise ValueError('Unexpected PNG geometry/format/trailing data')
    raw = zlib.decompress(compressed)
    if len(raw) != 14:
        raise ValueError('Wrong PNG row data size')
    out = bytearray()
    prior = bytes(6)
    for y in range(2):
        f = raw[y*7]
        row = bytearray(raw[y*7+1:y*7+7])
        for x in range(6):
            left = row[x-3] if x >= 3 else 0
            above = prior[x]
            upper_left = prior[x-3] if x >= 3 else 0
            p = left+above-upper_left
            distances = (abs(p-left), abs(p-above), abs(p-upper_left))
            paeth = (left, above, upper_left)[distances.index(min(distances))]
            if f > 4:
                raise ValueError('Invalid PNG filter')
            row[x] = (row[x]+(0, left, above, (left+above)//2, paeth)[f]) & 255
        out.extend(row)
        prior = row
    return bytes(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--dir', type=pathlib.Path, required=True)
    args = ap.parse_args()
    directory = args.dir.resolve()
    ff, fp = directory/'ffmpeg.exe', directory/'ffprobe.exe'
    env = os.environ.copy()
    system_root = env.get('SYSTEMROOT', env.get('SystemRoot', 'C:\\Windows'))
    env['PATH'] = str(pathlib.Path(system_root)/'System32')
    checks = []

    def check(value, name):
        checks.append(dict(name=name, passed=bool(value)))
        if not value:
            raise RuntimeError(name)

    with tempfile.TemporaryDirectory(prefix='rlplayer-png-contract-') as tmp:
        root = pathlib.Path(tmp)

        def run(exe, *argv):
            result = subprocess.run([str(exe), *map(str, argv)], cwd=root,
                                    env=env, capture_output=True, timeout=45)
            if result.returncode:
                raise RuntimeError(f'{exe.name} exit {result.returncode}: '
                                   + result.stderr.decode('utf-8', 'replace')[-2000:])
            return result.stdout

        def chunk(kind, data):
            return struct.pack('>I', len(data))+kind+data+struct.pack('>I', zlib.crc32(kind+data))

        # A real 2 x 2 RGB image with four different pixels, not the product's
        # placeholder. Decode and encode paths both have exact pixel oracles.
        pixels = bytes([255, 0, 0, 0, 255, 0, 0, 0, 255, 17, 31, 47])
        png = (b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR', struct.pack('>IIBBBBB', 2, 2, 8, 2, 0, 0, 0))
               + chunk(b'IDAT', zlib.compress(b'\0'+pixels[:6]+b'\0'+pixels[6:]))
               + chunk(b'IEND', b''))
        (root/'cover.png').write_bytes(png)
        bmp_rows = b''.join(bytes([pixels[i+2], pixels[i+1], pixels[i]]) for i in range(0, 6, 3))+b'\0\0'
        bmp_rows += b''.join(bytes([pixels[i+2], pixels[i+1], pixels[i]]) for i in range(6, 12, 3))+b'\0\0'
        bmp = struct.pack('<2sIHHI', b'BM', 70, 0, 0, 54)+struct.pack('<IiiHHIIiiII', 40, 2, -2, 1, 24, 0, 16, 0, 0, 0, 0)+bmp_rows
        (root/'pixels.bmp').write_bytes(bmp)
        (root/'audio.flac').write_bytes(base64.b64decode(FLAC))
        check(png_pixels(png) == pixels, 'Independent PNG reader known-pixel control')
        decoded = run(ff, '-v', 'error', '-i', 'cover.png', '-frames:v', '1', '-f', 'image2pipe', '-c:v', 'png', '-pix_fmt', 'rgb24', '-')
        check(png_pixels(decoded) == pixels, 'PNG decode and reencode return all four exact RGB pixels')
        run(ff, '-v', 'error', '-y', '-i', 'pixels.bmp', '-frames:v', '1', '-c:v', 'png', '-pix_fmt', 'rgb24', 'encoded.png')
        check(png_pixels((root/'encoded.png').read_bytes()) == pixels, 'PNG encoder preserves exact independent BMP RGB pixels')
        before = run(ff, '-v', 'error', '-i', 'audio.flac', '-map', '0:a:0', '-f', 's16le', '-')
        check(len(before) == 882 and any(before), 'Independent audio fixture decodes to 441 nonzero 16-bit samples')
        run(ff, '-v', 'error', '-y', '-i', 'audio.flac', '-i', 'cover.png', '-map', '0:a:0', '-map', '1:v:0',
            '-c', 'copy', '-disposition:v', 'attached_pic', '-metadata', 'title=PNG contract', 'attached.flac')
        run(ff, '-v', 'error', '-y', '-i', 'attached.flac', '-map', '0', '-c', 'copy',
            '-metadata', 'title=PNG metadata rewrite', 'rewritten.flac')
        for name in ('attached.flac', 'rewritten.flac'):
            info = json.loads(run(fp, '-v', 'error', '-show_streams', '-show_format', '-of', 'json', name))
            images = [s for s in info['streams'] if s.get('codec_type') == 'video']
            check(len(images) == 1 and images[0].get('codec_name') == 'png'
                  and images[0].get('width') == 2 and images[0].get('height') == 2
                  and images[0].get('disposition', {}).get('attached_pic') == 1,
                  name+': attached PNG dimensions and disposition survive')
            check(run(ff, '-v', 'error', '-i', name, '-map', '0:a:0', '-f', 's16le', '-') == before,
                  name+': all decoded audio samples remain identical')
            check(png_pixels(run(ff, '-v', 'error', '-i', name, '-map', '0:v:0', '-frames:v', '1',
                      '-f', 'image2pipe', '-c:v', 'png', '-pix_fmt', 'rgb24', '-')) == pixels,
                  name+': attached cover exact pixel check')
        check(info['format'].get('tags', {}).get('title') == 'PNG metadata rewrite', 'Metadata rewrite commits its title')
    print(json.dumps(dict(status='PASS', checks=checks, check_count=len(checks), device_opened=False,
                          path_scope='System32 only',
                          binaries={p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in (ff, fp)}), indent=2))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
