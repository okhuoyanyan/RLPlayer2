import re, sys, os

def strings(path, minlen=5, maxbytes=None):
    with open(path, 'rb') as f:
        data = f.read() if maxbytes is None else f.read(maxbytes)
    # ASCII
    for m in re.finditer(rb'[\x20-\x7e]{%d,}' % minlen, data):
        yield m.start(), m.group().decode('ascii', 'replace')
    # UTF-16LE
    for m in re.finditer(rb'(?:[\x20-\x7e]\x00){%d,}' % minlen, data):
        yield m.start(), m.group().decode('utf-16-le', 'replace')

if __name__ == '__main__':
    path, out = sys.argv[1], sys.argv[2]
    pats = sys.argv[3].split(',') if len(sys.argv) > 3 else None
    n = 0
    with open(out, 'w', encoding='utf-8', errors='replace') as o:
        for off, s in strings(path):
            if pats and not any(p.lower() in s.lower() for p in pats):
                continue
            o.write(f"{off:#010x}\t{s}\n")
            n += 1
    print(f"{os.path.basename(path)}: {n} matches -> {out}")
