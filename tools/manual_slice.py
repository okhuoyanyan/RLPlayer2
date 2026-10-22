import io, sys
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding="utf-8", errors="replace")
t = open(r"D:\RTA-hqplayer\research\hqplayer6-manual.txt", encoding="utf-8").read()
pages = t.split("=== PAGE BREAK ===")
start = int(sys.argv[1]); end = int(sys.argv[2])
for i in range(start-1, min(end, len(pages))):
    print(f"----- PAGE {i+1} -----")
    print(pages[i].strip())
