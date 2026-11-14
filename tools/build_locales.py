# -*- coding: utf-8 -*-
"""由中文页面 + website/i18n/<lang>.json 生成 /en /fr /ru /es /ar /ja 五个页面。

用法： python tools/build_locales.py [lang ...]
没有对应 json 的语言会被跳过。
"""
import io, os, re, json, sys, html

HERE = os.path.dirname(os.path.abspath(__file__))
WEB = os.path.join(os.path.dirname(HERE), "website")
I18N = os.path.join(WEB, "i18n")
SITE = "https://rlplayer.rainlain.com"

PAGES = ["index.html", "features.html", "manual.html", "measurements.html", "download.html"]

# 2026-11-02：多语种截图暂停使用（软件语言包待补），先统一挂中文图；
# 翻译/截图就绪后改回 True 并重跑即可。
USE_LOCALIZED_IMAGES = False
LANGS = ["en", "fr", "ru", "es", "ar", "ja"]
HTML_LANG = {"en": "en", "fr": "fr", "ru": "ru", "es": "es", "ar": "ar", "ja": "ja"}
RTL = {"ar"}

TEXT_RE = re.compile(r">([^<>]+)<")
ATTR_RE = re.compile(r'(alt|title|aria-label|placeholder|content|data-cap)="([^"]*)"')
SPLIT_RE = re.compile(r'(<script\b[^>]*>.*?</script>|<style\b[^>]*>.*?</style>)', re.S | re.I)

def esc_text(s):
    return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")

def esc_attr(s):
    return esc_text(s).replace('"', "&quot;")

def load_map(lang):
    src = json.load(io.open(os.path.join(I18N, "zh_source.json"), encoding="utf-8"))
    dstp = os.path.join(I18N, lang + ".json")
    if not os.path.exists(dstp):
        return None
    dst = json.load(io.open(dstp, encoding="utf-8"))
    zh, tr = src["strings"], dst["strings"]
    if len(tr) < len(zh):
        print("  ! %s: 译文 %d 条少于原文 %d 条，缺的保留中文" % (lang, len(tr), len(zh)))
    elif len(tr) > len(zh):
        print("  ! %s: 译文 %d 条多于原文 %d 条，多出的忽略" % (lang, len(tr), len(zh)))
    m = {}
    for a, b in zip(zh[:len(tr)], tr):
        if b and b.strip():
            m[a.strip()] = b.strip()
    return m

def translate_fragment(frag, table):
    def t_text(m):
        raw = m.group(1)
        key = raw.strip()
        if key in table:
            lead = raw[:len(raw) - len(raw.lstrip())]
            trail = raw[len(raw.rstrip()):]
            return ">" + lead + esc_text(table[key]) + trail + "<"
        return m.group(0)

    def t_attr(m):
        name, val = m.group(1), m.group(2)
        if val.strip() in table:
            return '%s="%s"' % (name, esc_attr(table[val.strip()]))
        return m.group(0)

    frag = TEXT_RE.sub(t_text, frag)
    frag = ATTR_RE.sub(t_attr, frag)
    return frag

def rewrite_links(s, lang, page):
    s = s.replace('href="assets/', 'href="../assets/').replace('src="assets/', 'src="../assets/')
    s = s.replace('<html lang="zh-CN">',
                  '<html lang="%s" dir="%s">' % (HTML_LANG.get(lang, lang), "rtl" if lang in RTL else "ltr"))
    canon = "%s/%s/%s" % (SITE, lang, page)
    s = re.sub(r'<link rel="canonical" href="[^"]*"', '<link rel="canonical" href="%s"' % canon, s)
    s = re.sub(r'<meta property="og:url" content="[^"]*"', '<meta property="og:url" content="%s"' % canon, s)
    s = s.replace('"url":"%s/"' % SITE, '"url":"%s/%s/"' % (SITE, lang))
    # hreflang：整块重写
    block = ['<link rel="alternate" hreflang="zh-CN" href="%s/%s">' % (SITE, page)]
    for code in LANGS:
        block.append('<link rel="alternate" hreflang="%s" href="%s/%s/%s">' % (code, SITE, code, page))
    block.append('<link rel="alternate" hreflang="x-default" href="%s/%s">' % (SITE, page))
    s = re.sub(r'(?:<link rel="alternate" hreflang="[^"]*" href="[^"]*">\s*)+', "\n".join(block) + "\n", s, count=1)
    return s




def localize_images(s, lang):
    """若 assets/img/<lang>/ 下有同名图（webp），就换成该语种的版本。"""
    base = os.path.join(WEB, "assets", "img", lang)

    def sub(m):
        stem = os.path.splitext(os.path.basename(m.group(2)))[0]
        cand = os.path.join(base, stem + ".webp")
        if os.path.exists(cand):
            return '%s="../assets/img/%s/%s.webp"' % (m.group(1), lang, stem)
        return m.group(0)

    return re.sub(r'(src|href)="\.\./assets/img/(?:hero/)?([^"/]+\.(?:webp|png))"', sub, s)


def strip_lang_detect(s):
    """语种子页不能再跑首页语言检测：它会 location.replace 回自己 -> 无限跳转。"""
    return re.sub(r'\s*<script[^>]*lang-detect\.js[^>]*>\s*</script>', '', s)


def build_page(lang, page, table):
    src = io.open(os.path.join(WEB, page), encoding="utf-8").read()
    parts = SPLIT_RE.split(src)
    out = []
    for i, p in enumerate(parts):
        if i % 2 == 1:
            # script / style 原样（JSON-LD 单独处理）
            if 'application/ld+json' in p:
                keys = sorted(table, key=len, reverse=True)
                for k in keys:
                    if k in p:
                        p = p.replace('"' + k + '"', '"' + table[k].replace('"', '\\"') + '"')
            out.append(p)
        else:
            out.append(translate_fragment(p, table))
    s = "".join(out)
    s = rewrite_links(s, lang, page)
    s = strip_lang_detect(s)
    if USE_LOCALIZED_IMAGES:
        s = localize_images(s, lang)
    dst_dir = os.path.join(WEB, lang)
    os.makedirs(dst_dir, exist_ok=True)
    io.open(os.path.join(dst_dir, page), "w", encoding="utf-8", newline="\n").write(s)
    return len(s)

def main():
    langs = sys.argv[1:] or LANGS
    for lang in langs:
        table = load_map(lang)
        if table is None:
            print("%-3s 跳过（缺 i18n/%s.json）" % (lang, lang))
            continue
        sizes = []
        for page in PAGES:
            sizes.append("%s:%d" % (page.replace(".html", ""), build_page(lang, page, table)))
        left = sum(1 for p in PAGES for _ in re.finditer(r"[\u4e00-\u9fff]", io.open(os.path.join(WEB, lang, p), encoding="utf-8").read()))
        print("%-3s 完成  %s  残留汉字 %d" % (lang, " ".join(sizes), left))

if __name__ == "__main__":
    main()
