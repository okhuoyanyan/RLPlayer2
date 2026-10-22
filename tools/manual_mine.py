import re, io, sys
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding="utf-8", errors="replace")
t = open(r"D:\RTA-hqplayer\research\hqplayer6-manual.txt", encoding="utf-8").read()
print("LEN", len(t))
print("=== first 2500 ===")
print(t[:2500])
print("=== numbered headings ===")
seen=set()
for m in re.finditer(r"(?m)^\s*(\d+(?:\.\d+)*\.?)\s+(\S[^\n]{2,70})$", t):
    k=(m.group(1), m.group(2))
    if k in seen: continue
    seen.add(k); print(m.group(1), m.group(2))
