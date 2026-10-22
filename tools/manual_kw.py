import io, sys, re
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding="utf-8", errors="replace")
t = open(r"D:\RTA-hqplayer\research\hqplayer6-manual.txt", encoding="utf-8").read()
kws = sys.argv[1:]
for kw in kws:
    print("########", kw)
    for m in re.finditer(kw, t, re.I):
        s = t[max(0,m.start()-350):m.start()+450].replace("\n"," ")
        print(" ...", s)
        print()
