import sys, os
src = r"C:\Program Files\Signalyst\HQPlayer 6 Desktop\hqplayer6desktop-manual.pdf"
out = r"D:\RTA-hqplayer\research\hqplayer6-manual.txt"
text = None
try:
    import pypdf
    r = pypdf.PdfReader(src)
    pages = [p.extract_text() or "" for p in r.pages]
    text = "\n\n=== PAGE BREAK ===\n\n".join(pages)
    print("pypdf pages:", len(pages))
except Exception as e:
    print("pypdf failed:", e)
if text is None:
    try:
        import fitz
        d = fitz.open(src)
        text = "\n\n=== PAGE BREAK ===\n\n".join(p.get_text() for p in d)
        print("pymupdf pages:", d.page_count)
    except Exception as e:
        print("pymupdf failed:", e)
if text:
    open(out, "w", encoding="utf-8").write(text)
    print("wrote", out, len(text))
else:
    print("NO EXTRACTION")
