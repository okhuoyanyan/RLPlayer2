import sys
from pypdf import PdfReader
src, dst = sys.argv[1], sys.argv[2]
r = PdfReader(src)
print("pages:", len(r.pages))
with open(dst, "w", encoding="utf-8", errors="replace") as f:
    for i, p in enumerate(r.pages):
        try:
            t = p.extract_text() or ""
        except Exception as e:
            t = f"[PAGE {i+1} EXTRACT ERROR: {e}]"
        f.write(f"\n===== PAGE {i+1} =====\n{t}")
print("done ->", dst)
