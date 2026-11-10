#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""官网中文页生成器（RLPlayer 主页 · 3.0.10）
把 dist 里的《技术白皮书》《使用说明书》接进 website/：
  features.html  技术细节 = 白皮书
  manual.html    使用说明 = 说明书
  assets/doc.css 文档页样式（白皮书 CSS + 说明书补充）
不改 index.html / download.html（它们由手工维护，只共享 assets/nav.css）。
"""
import io, os, re, shutil, zipfile, json
from docx import Document
from docx.table import Table
from docx.text.paragraph import Paragraph
from docx.oxml.ns import qn

ROOT = r"D:\RTA-hqplayer"
WEB = os.path.join(ROOT, "website")
DIST = os.path.join(ROOT, "dist")
WP_DIR = os.path.join(DIST, "RLPlayer-3.0.10-官网白皮书")
APP_DIR = os.path.join(DIST, "RLPlayer-3.0.11-官网白皮书")
VER = "3.0.10"
SITE = "https://rlplayer.rainlain.com"

WP_PDF = "RLPlayer-3.0.10-技术白皮书.pdf"
WP_DOCX = "RLPlayer-3.0.10-技术白皮书.docx"
UG_PDF = "RLPlayer-3.0.10-使用说明书.pdf"
UG_DOCX = "RLPlayer-3.0.10-使用说明书.docx"

NAV = """<a class="skip" href="#paper">跳到正文</a>
<header class="nav">
  <div class="nav-inner">
    <a class="brand" href="index.html"><span class="dot"></span>RLPlayer</a>
    <button class="nav-toggle" type="button" aria-expanded="false" aria-controls="navmenu" aria-label="打开导航菜单">
      <span></span><span></span><span></span>
    </button>
    <nav class="nav-links" id="navmenu" aria-label="主导航">
      <a href="index.html"{a_home}>主页</a>
      <a href="features.html"{a_feat}>技术细节</a>
      <a href="manual.html"{a_man}>使用说明</a>
      <a href="measurements.html"{a_meas}>实测数据</a>
      <a href="download.html"{a_dl}>下载</a>
    </nav>
  </div>
</header>"""


def nav(active):
    return NAV.format(
        ver=VER,
        a_home=' class="on"' if active == "home" else "",
        a_feat=' class="on"' if active == "features" else "",
        a_man=' class="on"' if active == "manual" else "",
        a_meas=' class="on"' if active == "measurements" else "",
        a_dl=' class="on"' if active == "download" else "",
    )


def esc(t):
    return (t.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;"))


def head(title, desc, canonical, css="assets/doc.css", extra="", active="x"):
    return """<!doctype html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>{title}</title>
<meta name="description" content="{desc}">
<meta name="keywords" content="{kw}">
<meta name="author" content="RainLain">
<meta name="robots" content="index,follow,max-image-preview:large">
<meta name="theme-color" content="#08090d">
<link rel="canonical" href="{site}{canonical}">
<meta property="og:type" content="article">
<meta property="og:site_name" content="RLPlayer">
<meta property="og:locale" content="zh_CN">
<meta property="og:title" content="{title}">
<meta property="og:description" content="{desc}">
<meta property="og:url" content="{site}{canonical}">
<meta property="og:image" content="{site}/assets/img/albums.png">
<meta name="twitter:card" content="summary_large_image">
<meta name="twitter:title" content="{title}">
<meta name="twitter:description" content="{desc}">
<meta name="twitter:image" content="{site}/assets/img/albums.png">
<link rel="icon" href="assets/img/favicon.svg" type="image/svg+xml">
<link rel="stylesheet" href="{css}?v=20261101d">
<link rel="stylesheet" href="assets/nav.css?v=20261101d">
{extra}</head>
<body>
{nav}
""".format(title=esc(title), desc=esc(desc), kw="HiFi播放器,技术白皮书,使用说明书,DSD,bit-perfect,RLPlayer",
           site=SITE, canonical=canonical, css=css, extra=extra, nav=nav(active))


def site_scripts():
    return """<script src="assets/doc.js?v=20261101d" defer></script>
<script src="assets/nav.js?v=20261101d" defer></script>
<script src="assets/i18n.js?v=20261101d" defer></script>
</body>
</html>
"""


# ---------------------------------------------------------------- features
def build_features():
    src = io.open(os.path.join(WP_DIR, "whitepaper.html"), encoding="utf-8").read()

    # 1) head
    src = re.sub(r"<title>.*?</title>", "<title>技术细节 — RLPlayer 技术白皮书｜PCM · DSD · 数值证据</title>", src, count=1)
    src = re.sub(r'<meta name="description" content="[^"]*">',
                 '<meta name="description" content="RLPlayer {v} 技术白皮书：PCM 重采样与采样率族、两级滤波器、280 dB 的真实含义、DSD 直通与调制稳定性、CPU/GPU 逐样本一致、bit-perfect 的适用条件。">'.format(v=VER), src, count=1)
    src = re.sub(r'<link rel="canonical" href="[^"]*">',
                 '<link rel="canonical" href="%s/features.html">' % SITE, src, count=1)
    src = re.sub(r'<meta name="author" content="[^"]*">', '<meta name="author" content="RainLain">', src, count=1)

    # 2) css
    src = src.replace('<link rel="stylesheet" href="assets/whitepaper.css">',
                      '<link rel="stylesheet" href="assets/doc.css?v=20261101d">\n'
                      '<link rel="stylesheet" href="assets/nav.css?v=20261101d">')

    # 3) nav
    src = re.sub(r'<a class="skip"[^>]*>.*?</a>\s*<header class="nav">.*?</header>',
                 nav("features"), src, count=1, flags=re.S)

    # 4) 下载链接指向 assets/docs
    src = src.replace('href="%s"' % WP_PDF, 'href="assets/docs/%s" download' % WP_PDF)
    src = src.replace('href="%s"' % WP_DOCX, 'href="assets/docs/%s" download' % WP_DOCX)

    # 5) 脚本 + 图标
    src = src.replace('<script src="assets/whitepaper.js" defer></script>',
                      '<script src="assets/doc.js?v=20261101d" defer></script>\n'
                      '<script src="assets/nav.js?v=20261101d" defer></script>\n'
                      '<script src="assets/i18n.js?v=20261101d" defer></script>')
    src = src.replace('<meta name="theme-color" content="#08090d">',
                      '<meta name="theme-color" content="#08090d">\n<link rel="icon" href="assets/img/favicon.svg" type="image/svg+xml">', 1)
    # 6) hreflang
    src = src.replace('</head>', hreflang("/features.html") + "</head>", 1)
    entry = ('<div class="note" style="margin-top:34px"><strong>附录 A · 79 个滤波器预设实测频响</strong>'
             '：每个预设的实测幅频、群延迟、通带偏差与阻带抑制，单独成页 → '
             '<a href="measurements.html">查看实测数据</a></div>')
    src = src.replace('</main>', entry + '</main>', 1)
    io.open(os.path.join(WEB, "features.html"), "w", encoding="utf-8", newline="\n").write(src)
    return len(src)


def hreflang(path):
    out = []
    for code in ["zh-CN", "en", "fr", "ru", "es", "ar", "ja"]:
        url = SITE + path if code == "zh-CN" else SITE + "/" + code.split("-")[0] + path
        out.append('<link rel="alternate" hreflang="%s" href="%s">' % (code, url))
    out.append('<link rel="alternate" hreflang="x-default" href="%s">' % (SITE + path))
    return "\n".join(out) + "\n"


# ---------------------------------------------------------------- manual
def iter_blocks(doc):
    body = doc.element.body
    for child in body.iterchildren():
        if child.tag == qn("w:p"):
            yield ("p", Paragraph(child, doc))
        elif child.tag == qn("w:tbl"):
            yield ("tbl", Table(child, doc))


def runs_html(par):
    out = []
    for r in par.runs:
        t = esc(r.text)
        if not t:
            continue
        if r.bold:
            t = "<strong>%s</strong>" % t
        if r.italic:
            t = "<em>%s</em>" % t
        out.append(t)
    return "".join(out) or esc(par.text)


def table_html(tb):
    rows = []
    for ri, row in enumerate(tb.rows):
        cells = "".join("<%s>%s</%s>" % ("th" if ri == 0 else "td", esc(c.text.strip().replace("\n", " ")), "th" if ri == 0 else "td") for c in row.cells)
        rows.append("<tr>%s</tr>" % cells)
    head = rows[0] if rows else ""
    body = "".join(rows[1:])
    return ('<div class="table-wrap" tabindex="0" role="region" aria-label="表格">'
            '<table><thead>%s</thead><tbody>%s</tbody></table></div>' % (head, body))


def build_manual():
    doc = Document(os.path.join(DIST, UG_DOCX))
    blocks = []
    for kind, obj in iter_blocks(doc):
        if kind == "tbl":
            blocks.append(("table", table_html(obj)))
            continue
        st = obj.style.name
        txt = obj.text.strip()
        has_img = bool(obj._p.findall(".//" + qn("a:blip")))
        if has_img:
            blocks.append(("image", obj))
        if not txt:
            continue
        if st in ("Title", "Subtitle", "toc 1"):
            continue
        if st == "Heading 1":
            blocks.append(("h1", txt))
        elif st == "Heading 2":
            blocks.append(("h2", txt))
        elif st == "List Bullet":
            blocks.append(("li", runs_html(obj)))
        elif st == "Caption":
            blocks.append(("caption", txt))
        else:
            blocks.append(("p", runs_html(obj)))
    return blocks


def slug_for(h1, idx):
    if h1.startswith("第"):
        m = re.match(r"第\s*(\d+)\s*章", h1)
        if m:
            return "ch-%02d" % int(m.group(1))
    if "附录 A" in h1:
        return "app-a"
    if "附录 B" in h1:
        return "app-b"
    return "sec-%02d" % idx


def render_manual(blocks):
    chapters = []       # (id, no, title, body_html)
    intro = []          # 「阅读导航」正文（前言），不合入正文章节
    collect_intro = False
    cur = None
    buf = []
    counter = 0
    step_open = False
    list_open = False

    def close_lists():
        nonlocal step_open, list_open
        if step_open:
            buf.append("</ol>"); step_open = False
        if list_open:
            buf.append("</ul>"); list_open = False

    def flush():
        nonlocal cur, buf
        if cur:
            cur["body"] = "\n".join(buf)
            chapters.append(cur)
        buf = []

    for kind, val in blocks:
        if kind == "h1":
            close_lists()
            flush()
            collect_intro = (val == "阅读导航")
            if collect_intro:
                cur = None
                continue
            counter += 1
            cur = {"id": slug_for(val, counter), "title": val, "no": ""}
            buf = []
            m = re.match(r"第\s*(\d+)\s*章\s*(.*)$", val)
            if m:
                cur["no"] = "%02d" % int(m.group(1)); cur["title"] = m.group(2).strip()
            elif "附录 A" in val:
                cur["no"] = "A"; cur["title"] = val.replace("附录 A", "").strip()
            elif "附录 B" in val:
                cur["no"] = "B"; cur["title"] = val.replace("附录 B", "").strip()
            elif val == "常用音频词语":
                cur["no"] = "00"
            else:
                cur["no"] = "%02d" % counter
            continue
        if cur is None:
            if collect_intro and kind == "p":
                intro.append(re.sub(r"<[^>]+>", "", val))
            continue
        if kind == "h2":
            close_lists(); buf.append("<h3>%s</h3>" % esc(val))
        elif kind == "p":
            m = re.match(r"^\s*(\d+)[\u3000]\s*(.+)$", val, re.S)
            if m:
                if list_open:
                    buf.append("</ul>"); list_open = False
                if not step_open:
                    buf.append('<ol class="steps">'); step_open = True
                buf.append("<li>%s</li>" % m.group(2))
            else:
                close_lists()
                cls = ' class="practice"' if val.startswith("小练习") else ""
                buf.append("<p%s>%s</p>" % (cls, val))
        elif kind == "li":
            if step_open:
                buf.append("</ol>"); step_open = False
            if not list_open:
                buf.append("<ul>"); list_open = True
            buf.append("<li>%s</li>" % val)
        elif kind == "table":
            close_lists(); buf.append(val)
        elif kind == "caption":
            close_lists(); buf.append('<p class="figcap">%s</p>' % esc(val))
        elif kind == "image":
            close_lists()
            buf.append('<figure class="shot"><img src="assets/img/manual/fig-pcm.png" alt="RLPlayer 3.0.9 设置 → PCM：源采样率与同族优先" width="1340" height="880" loading="lazy" decoding="async"></figure>')
    close_lists()
    flush()
    return chapters, "".join(intro)

def build_manual_page(chapters, intro=""):
    toc = "\n".join(
        '<li><a href="#%s"><span>%s</span>%s</a></li>' % (c["id"], c["no"], esc(c["title"]))
        for c in chapters
    )
    toc_mobile = '<details class="mobile-toc"><summary>目录 · %d 章</summary><ol>%s</ol></details>' % (len(chapters), toc)

    body = []
    for c in chapters:
        body.append('<section class="chapter" id="%s">' % c["id"])
        body.append('<div class="chapter-head"><span class="chapter-no">%s</span><h2>%s</h2></div>' % (c["no"], esc(c["title"])))
        body.append(c["body"])
        body.append("</section>")
    article = "\n".join(body)

    quick = """<div class="table-wrap quickstart" tabindex="0" role="region" aria-label="快捷入口">
<table><thead><tr><th>你想做什么</th><th>先看哪里</th></tr></thead><tbody>
<tr><td>尽快开始听歌</td><td><a href="#ch-01">第 1 章　安装与首次播放</a></td></tr>
<tr><td>不改动原始样本</td><td><a href="#ch-05">第 5 章　理解逐位直通</a></td></tr>
<tr><td>选择升频与采样率</td><td><a href="#ch-06">第 6–8 章　PCM 与滤波</a></td></tr>
<tr><td>播放或输出 DSD</td><td><a href="#ch-09">第 9–10 章　SDM 与 DSD 源</a></td></tr>
<tr><td>调耳机或房间的声音</td><td><a href="#ch-12">第 12–13 章　EQ 与声音校准</a></td></tr>
<tr><td>找音乐、管曲库</td><td><a href="#ch-03">第 3 章　查找音乐与管理曲库</a></td></tr>
<tr><td>没有声音 / 断续</td><td><a href="#ch-18">第 18 章　没有声音与断续播放</a></td></tr>
</tbody></table></div>"""

    hero = """<div class="hero">
  <div class="wrap hero-grid">
    <div>
      <span class="eyebrow">USER GUIDE / {v}</span>
      <h1>RLPlayer<br><em>使用说明书</em></h1>
      <p class="lede">从第一次打开，到选择设备、管理曲库和调整声音。这本手册帮助你看懂播放器正在做什么，并能在遇到问题时找到具体原因。先完成首次播放，再按需要阅读 PCM、DSD、EQ 和房间校正章节。</p>
      <div class="actions">
        <a class="btn" href="assets/docs/{ug_pdf}" download>下载 PDF ↓</a>
        <a class="btn ghost" href="assets/docs/{ug_docx}" download>可编辑 Word ↓</a>
        <button class="btn ghost" type="button" onclick="window.print()">打印</button>
      </div>
      <div class="meta">适用版本 {v}　·　编写日期 2026 年 11 月 1 日　·　WINDOWS 正式发布版</div>
    </div>
    <div class="cover-path" aria-label="手册覆盖的四个层次">
      <div class="route-label">FROM FIRST LAUNCH TO DAILY USE</div>
      <div class="route-step"><span class="route-num">01</span><div><div class="route-title">装好、放出声</div><div class="route-detail">解压 · 选设备 · 首次播放</div></div></div>
      <div class="route-step"><span class="route-num">02</span><div><div class="route-title">看懂信号链</div><div class="route-detail">采样率 · 滤波 · 抖动</div></div></div>
      <div class="route-step"><span class="route-num">03</span><div><div class="route-title">DSD 与输出</div><div class="route-detail">SDM · DoP · ASIO</div></div></div>
      <div class="route-step"><span class="route-num">04</span><div><div class="route-title">调声与排错</div><div class="route-detail">EQ · 校准 · 日志</div></div></div>
    </div>
  </div>
</div>
<div class="reading-info"><div class="wrap">
  <span><strong>适用版本</strong>　{v}　·　Windows 正式发布版</span>
  <span><strong>章节</strong>　22 章 + 2 个附录</span>
  <span><strong>阅读方式</strong>　左侧目录，或手机端「目录」展开</span>
</div></div>""".format(v=VER, ug_pdf=UG_PDF, ug_docx=UG_DOCX)

    html = head("使用说明 — RLPlayer 官方手册｜安装 · PCM · DSD · EQ",
                "RLPlayer %s 使用说明书：安装与首次播放、PCM 采样率策略、滤波与抖动、SDM/DSD 输出、EQ 与房间校正、音频分析、AI 资料整理、故障排查与一键反馈。" % VER,
                "/manual.html")
    html += hero + """
<div class="wrap">
  <div class="layout">
    <aside class="toc" aria-label="手册目录">
      <div class="toc-title">CONTENTS / 目录</div>
      <ol>%s</ol>
      <div class="read-progress" data-progress></div>
    </aside>
    <main id="paper">
      %s
      %s
      %s
      <div class="note" style="margin-top:10px">%s 本手册对应 RLPlayer %s。技术原理与数值证据见 <a href="features.html">技术细节（白皮书）</a>。</div>
    </main>
  </div>
</div>
<footer class="paper-footer"><div class="wrap">
  <span>© 2026 RLPlayer　·　RainLain、猫与小行星</span>
  <span><a href="%s">rlplayer.rainlain.com</a>　·　版本 %s</span>
</div></footer>
""" % (toc, toc_mobile, quick, article, intro.strip(), VER, SITE, VER)
    html += site_scripts()
    io.open(os.path.join(WEB, "manual.html"), "w", encoding="utf-8", newline="\n").write(html)
    return len(html)


# ---------------------------------------------------------------- assets
MANUAL_CSS = """

/* ============ 使用说明书补充（build_homepage.py 生成） ============ */
#paper .quickstart{margin:0 0 38px}
#paper .chapter .steps{margin:18px 0 24px;padding:0;list-style:none;counter-reset:s}
#paper .chapter .steps li{position:relative;margin:12px 0;padding:0 0 0 42px;color:#b4bed2;line-height:1.85}
#paper .chapter .steps li:before{counter-increment:s;content:counter(s);position:absolute;left:0;top:2px;width:26px;height:26px;border-radius:8px;background:#121722;border:1px solid #26324b;color:#93a7ff;font:12px/26px Consolas,monospace;text-align:center}
#paper .chapter ul{margin:16px 0 22px;padding-left:22px}
#paper .chapter ul li{margin:9px 0;color:#b4bed2;line-height:1.85}
#paper .chapter .practice{border-left:3px solid #a87bff;background:linear-gradient(100deg,#a87bff12,#6b8afd06);padding:14px 18px;border-radius:0 10px 10px 0;font-size:14px;color:#c6d1e7}
#paper figure.shot{margin:26px 0 6px}
#paper figure.shot img{display:block;width:100%;height:auto;border:1px solid var(--line);border-radius:12px;background:#0c0e14}
#paper .figcap{font-size:12px;color:var(--text-mute);margin:10px 0 30px;letter-spacing:.3px}
#paper .table-wrap td a{color:#afc2ff}
@media(max-width:760px){
  #paper .chapter .steps li{padding-left:36px}
  #paper .chapter .steps li:before{width:23px;height:23px;font-size:11px;line-height:23px}
  /* 手机上表格改为自适应换行，不再左右裁切 */
  .table-wrap table{min-width:0;width:100%;font-size:12.5px;table-layout:auto}
  .table-wrap th,.table-wrap td{padding:10px 12px;word-break:break-word;overflow-wrap:anywhere}
  .table-wrap td:first-child{min-width:0}
  /* 阅读信息条：手机上竖排 */
  .reading-info .wrap{display:flex;flex-direction:column;align-items:flex-start;gap:9px}
  .reading-info span{display:block}
}
@media print{
  #paper .chapter .steps li:before{background:white;border:1px solid #9aa4b8;color:#333}
  #paper figure.shot img{border:1px solid #c2cbd9}
}
"""


def build_css():
    src = io.open(os.path.join(WP_DIR, "assets", "whitepaper.css"), encoding="utf-8").read()
    out = src + MANUAL_CSS
    io.open(os.path.join(WEB, "assets", "doc.css"), "w", encoding="utf-8", newline="\n").write(out)
    return len(out)


def build_docs_and_assets():
    docs = os.path.join(WEB, "assets", "docs")
    os.makedirs(docs, exist_ok=True)
    for name in (WP_PDF, WP_DOCX, UG_PDF, UG_DOCX):
        shutil.copyfile(os.path.join(DIST, name), os.path.join(docs, name))
    shutil.copyfile(os.path.join(WP_DIR, "assets", "whitepaper.js"), os.path.join(WEB, "assets", "doc.js"))
    imgdir = os.path.join(WEB, "assets", "img", "manual")
    os.makedirs(imgdir, exist_ok=True)
    with zipfile.ZipFile(os.path.join(DIST, UG_DOCX)) as z:
        data = z.read("word/media/image1.png")
    io.open(os.path.join(imgdir, "fig-pcm.png"), "wb").write(data)
    # favicon（SVG，站点色调）
    fav = ('<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 64 64">'
           '<defs><linearGradient id="g" x1="0" y1="0" x2="1" y2="1">'
           '<stop offset="0" stop-color="#6b8afd"/><stop offset="1" stop-color="#a87bff"/></linearGradient></defs>'
           '<rect width="64" height="64" rx="14" fill="#08090d"/>'
           '<circle cx="32" cy="32" r="13" fill="none" stroke="url(#g)" stroke-width="4"/>'
           '<circle cx="32" cy="32" r="4" fill="url(#g)"/></svg>')
    io.open(os.path.join(WEB, "assets", "img", "favicon.svg"), "w", encoding="utf-8", newline="\n").write(fav)



# ---------------------------------------------------------------- 实测数据（附录 A）
def build_measurements():
    src = io.open(os.path.join(APP_DIR, "whitepaper.html"), encoding="utf-8").read()
    m = re.search(r'<section class="chapter" id="appendix[^"]*"[^>]*>.*?</section>', src, re.S)
    if not m:
        raise SystemExit("appendix section not found in 3.0.11 whitepaper")
    body = m.group(0)

    # 图片改站内 WebP 路径
    body = re.sub(r'assets/figs/(freq-[0-9]+-[^.]+)\.png', r'assets/img/freq/\1.webp', body)

    # h3 加锚点，同时生成目录
    toc_items = []
    counter = {"n": 0}

    def h3(m2):
        counter["n"] += 1
        aid = "a%d" % counter["n"]
        title = re.sub(r"<[^>]+>", "", m2.group(1)).strip()
        toc_items.append((aid, title))
        return '<h3 id="%s">%s</h3>' % (aid, m2.group(1))

    body = re.sub(r"<h3[^>]*>(.*?)</h3>", h3, body, flags=re.S)
    # 页面自己有 hero，去掉章节头；统一 id
    body = re.sub(r'<div class="chapter-head">.*?</div>', '', body, count=1, flags=re.S)
    body = re.sub(r'<section class="chapter" id="appendix[^"]*"[^>]*>', '<section class="chapter" id="appendix">', body, count=1)

    def _clean(t, aid):
        return esc(re.sub(r"^A\.[0-9]+\s*", "", t))
    toc = "\n".join('        <li><a href="#%s"><span>%s</span>%s</a></li>' % (aid, aid.upper(), _clean(t, aid)) for aid, t in toc_items)
    toc_mobile = '<details class="mobile-toc"><summary>目录 · %d 节</summary><ol>%s</ol></details>' % (len(toc_items), toc)

    hero = """<div class="hero">
  <div class="wrap hero-grid">
    <div>
      <span class="eyebrow">MEASURED DATA</span>
      <h1>实测数据<br><em>79 个预设的真实频响</em></h1>
      <p class="lede">每个滤波器预设的实测幅频、群延迟、通带偏差与阻带抑制，以及测量方法、置信区间和失败组合的如实登记。这些曲线由随包命令行工具在离线环境跑出，可逐条复核。</p>
      <div class="actions">
        <a class="btn" href="assets/docs/RLPlayer-3.0.11-附录A-79预设频响.csv" download>下载原始数据 CSV ↓</a>
        <a class="btn ghost" href="assets/docs/RLPlayer-3.0.11-技术白皮书.pdf" download>完整白皮书 PDF ↓</a>
        <button class="btn ghost" type="button" onclick="window.print()">打印</button>
      </div>
      <div class="meta">79 组曲线　·　14 张表　·　数据与白皮书中附录 A 一致</div>
    </div>
    <div class="cover-path" aria-label="附录 A 的结构">
      <div class="route-label">HOW TO READ</div>
      <div class="route-step"><span class="route-num">A.1</span><div><div class="route-title">判定口径</div><div class="route-detail">先定标准，再看图</div></div></div>
      <div class="route-step"><span class="route-num">A.2</span><div><div class="route-title">测量与置信</div><div class="route-detail">条件、重复性、误差</div></div></div>
      <div class="route-step"><span class="route-num">A.3</span><div><div class="route-title">关键测量</div><div class="route-detail">44.1 kHz → 384 / 705.6 kHz</div></div></div>
      <div class="route-step"><span class="route-num">A.5</span><div><div class="route-title">79 张曲线</div><div class="route-detail">逐个预设可查</div></div></div>
    </div>
  </div>
</div>
<div class="reading-info"><div class="wrap">
  <span><strong>数据规模</strong>　79 个预设 × 2 个目标采样率</span>
  <span><strong>阅读方式</strong>　左侧目录，或手机端「目录」展开</span>
</div></div>"""

    html = head("实测数据 — 79 个滤波器预设的实测频响｜RLPlayer",
                "RLPlayer 79 个滤波器预设的实测幅频、群延迟、通带偏差与阻带抑制，含测量口径、置信区间、失败组合登记与原始 CSV 数据。",
                "/measurements.html", active="measurements",
                extra="<style>@media(max-width:760px){.hero h1{font-size:30px;line-height:1.26}}</style>")
    html += hero + """
<div class="wrap">
  <div class="layout">
    <aside class="toc" aria-label="附录目录">
      <div class="toc-title">CONTENTS / 目录</div>
      <ol>%s</ol>
    </aside>
    <main id="paper">
      %s
      %s
      <div class="note" style="margin-top:6px">本节数据与《RLPlayer 技术白皮书》附录 A 一致。最高输出规格仍以设备、驱动与所选处理路径为准；测量结论只描述被测信号本身，不等同于听感评价。</div>
    </main>
  </div>
</div>
<footer class="paper-footer"><div class="wrap">
  <span>© 2026 RLPlayer　·　RainLain、猫与小行星</span>
  <span><a href="%s">rlplayer.rainlain.com</a></span>
</div></footer>
""" % (toc, toc_mobile, body, SITE)
    html += site_scripts()
    io.open(os.path.join(WEB, "measurements.html"), "w", encoding="utf-8", newline="\n").write(html)
    return len(html), len(toc_items)

if __name__ == "__main__":
    css_len = build_css()
    feat_len = build_features()
    chapters, intro = render_manual(build_manual())
    man_len = build_manual_page(chapters, intro)
    meas_len, meas_secs = build_measurements()
    build_docs_and_assets()
    print("doc.css   %7d bytes" % css_len)
    print("features  %7d bytes" % feat_len)
    print("manual    %7d bytes, %d chapters" % (man_len, len(chapters)))
    print("measured  %7d bytes, %d sections" % (meas_len, meas_secs))
    for c in chapters:
        print("   %-5s %s" % (c["no"], c["title"]))