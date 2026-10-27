// test_artpage.cpp — 网页取图 / URL 安全策略 / 图片闸 的离线单测（2026-10-27，261026-04 #7/#9）
//
// 为什么这些判据必须离线测（铁律 3）：真实网络每次结果都不同，"今天命中 4/6"证明不了
// "抽取逻辑对"——它可能只是搜索恰好给了个带 og:image 的页。所以把**页面解析**与**安全策略**
// 全部抽成 artwork.h 里的纯函数，用固定 HTML 夹具断言，再由 rtaweb 的实测给命中率数字。
//
// 覆盖：
//   1) 安全策略：内网/回环/IP/userinfo/怪端口/非 http ⟹ 拒；陌生 CDN 域名 ⟹ 放（白名单已删）
//   2) 六路抽取：og:image / og:image:secure_url / twitter:image(:src) / JSON-LD(image,thumbnailUrl)
//      / link rel=image_src / itemprop=image / JS 变量 / <img> 启发式
//   3) 三个反例（用户判据 #4）：内网地址、非图片内容、1×1 像素占位图 —— 必须全被拒
//   4) 名字验收：简繁折叠命中（天門/天门）、去括号命中、**不**误配（徐瞑蕾 ≠ 徐娇）
//   5) 扩展源（bangumi/AniList/Apple）用 HTTP 钩子做假响应：只接受同名，不许"第一个结果就用"
#include "meta/artwork.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace rta::meta;

static int g_fail = 0;
static int g_pass = 0;

static void check(bool cond, const std::string& what) {
    if (cond) { ++g_pass; return; }
    ++g_fail;
    std::printf("  FAIL %s\n", what.c_str());
}

static void checkEq(const std::string& got, const std::string& want, const std::string& what) {
    if (got == want) { ++g_pass; return; }
    ++g_fail;
    std::printf("  FAIL %s  got=[%s] want=[%s]\n", what.c_str(), got.c_str(), want.c_str());
}

// ---- 造一个最小 PNG 头（只要 IHDR 够 art_image_dims 读） ----
static std::string makePng(int w, int h, size_t totalBytes) {
    std::string b;
    const unsigned char sig[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    b.append((const char*)sig, 8);
    unsigned char ihdr[25] = {0};
    ihdr[0] = 0; ihdr[1] = 0; ihdr[2] = 0; ihdr[3] = 13;      // 长度 13
    ihdr[4] = 'I'; ihdr[5] = 'H'; ihdr[6] = 'D'; ihdr[7] = 'R';
    ihdr[8]  = (unsigned char)((w >> 24) & 0xFF);
    ihdr[9]  = (unsigned char)((w >> 16) & 0xFF);
    ihdr[10] = (unsigned char)((w >> 8) & 0xFF);
    ihdr[11] = (unsigned char)(w & 0xFF);
    ihdr[12] = (unsigned char)((h >> 24) & 0xFF);
    ihdr[13] = (unsigned char)((h >> 16) & 0xFF);
    ihdr[14] = (unsigned char)((h >> 8) & 0xFF);
    ihdr[15] = (unsigned char)(h & 0xFF);
    ihdr[16] = 8; ihdr[17] = 6; ihdr[18] = 0; ihdr[19] = 0; ihdr[20] = 0;
    b.append((const char*)ihdr, 21);
    while (b.size() < totalBytes) b += '\0';
    return b;
}

static std::string makeGif(int w, int h, size_t totalBytes) {
    std::string b = "GIF89a";
    b += (char)(w & 0xFF); b += (char)((w >> 8) & 0xFF);
    b += (char)(h & 0xFF); b += (char)((h >> 8) & 0xFF);
    b += (char)0x00; b += (char)0x3B; b += (char)0x00; b += (char)0x00;
    while (b.size() < totalBytes) b += '\0';
    return b;
}

// ===========================================================================
// 1) 安全策略（判据 #4 的反例 1：内网地址）
// ===========================================================================
static void testUrlPolicy() {
    std::printf("[1] URL 安全策略\n");
    std::string why;
    check(!art_url_public_ok("http://127.0.0.1:8080/a.jpg", &why), "回环地址必须拒");
    check(!art_url_public_ok("http://localhost/a.jpg", &why), "localhost 必须拒");
    check(!art_url_public_ok("http://192.168.1.10/x.png", &why), "192.168 必须拒");
    check(!art_url_public_ok("http://10.1.2.3/x.png", &why), "10.x 必须拒");
    check(!art_url_public_ok("http://172.20.0.5/x.png", &why), "172.16-31 必须拒");
    check(!art_url_public_ok("http://169.254.169.254/latest/meta-data/", &why), "云元数据地址必须拒");
    check(!art_url_public_ok("http://1.2.3.4/x.jpg", &why), "裸 IP 必须拒");
    check(!art_url_public_ok("file:///C:/a.jpg", &why), "非 http(s) 必须拒");
    check(!art_url_public_ok("http://user:pass@evil.com/a.jpg", &why), "userinfo 必须拒");
    check(!art_url_public_ok("http://example.com:8080/a.jpg", &why), "非 80/443 端口必须拒");
    check(!art_url_public_ok("javascript:alert(1)", &why), "javascript: 必须拒");
    check(!art_url_public_ok("", &why), "空串必须拒");
    // 白名单删除后：陌生域名必须放行（这是上一轮 0/6 的根因之一）
    check(art_url_public_ok("https://p1.music.126.net/abc/109951169164936450.jpg?param=300y300", &why),
          "网易图床（带 ?param=）必须放");
    check(art_url_public_ok("https://lain.bgm.tv/pic/crt/l/87/b1/10868_prsn_LoxnW.jpg", &why),
          "bangumi 图床必须放");
    check(art_url_public_ok("https://s4.anilist.co/file/anilistcdn/staff/large/n106297.jpg", &why),
          "anilist 图床必须放");
    check(art_url_public_ok("https://never-heard-of-this-cdn.example.com/a/b/c.webp", &why),
          "陌生 CDN 域名必须放（白名单已删）");
    check(art_url_public_ok("https://cdn.dizzylab.net/media/avatars/x.jpg!cover", &why),
          "dizzylab 的 !cover 形态必须放");
}

// ===========================================================================
// 2) 六路抽取
// ===========================================================================
static void testExtract() {
    std::printf("[2] 六路取图\n");
    const std::string base = "https://example.com/person/123";

    {   // og:image + og:image:secure_url（取 secure 优先）
        const std::string html =
            "<html><head>"
            "<meta property=\"og:image\" content=\"https://img.example.com/a.jpg\">"
            "<meta property=\"og:image:secure_url\" content=\"https://img.example.com/secure.jpg\">"
            "</head><body></body></html>";
        const auto c = art_page_image_candidates(html, base, "测试");
        check(!c.empty(), "og:image 应抽到候选");
        checkEq(c.front().url, "https://img.example.com/secure.jpg", "secure_url 应排第一");
        checkEq(c.front().kind, "og:secure", "kind 应为 og:secure");
    }
    {   // twitter:image:src
        const std::string html =
            "<meta name=\"twitter:image:src\" content=\"https://img.example.com/tw.png\">";
        const auto c = art_page_image_candidates(html, base);
        check(!c.empty() && c.front().url == "https://img.example.com/tw.png", "twitter:image:src 应抽到");
    }
    {   // JSON-LD：image 是对象数组
        const std::string html =
            "<script type=\"application/ld+json\">{\"@type\":\"Person\",\"name\":\"X\","
            "\"image\":[{\"@type\":\"ImageObject\",\"url\":\"https://img.example.com/ld.jpg\"}]}</script>";
        const auto c = art_page_image_candidates(html, base);
        check(!c.empty() && c.front().url == "https://img.example.com/ld.jpg", "JSON-LD image 应抽到");
        check(!c.empty() && c.front().kind == "json-ld", "JSON-LD kind");
    }
    {   // JSON-LD：thumbnailUrl + 相对路径
        const std::string html =
            "<script type=\"application/ld+json\">{\"thumbnailUrl\":\"/pic/t1.jpg\"}</script>";
        const auto c = art_page_image_candidates(html, base);
        check(!c.empty() && c.front().url == "https://example.com/pic/t1.jpg", "JSON-LD 相对路径应补成绝对");
    }
    {   // link rel=image_src + itemprop=image
        const std::string html =
            "<link rel=\"image_src\" href=\"//cdn.example.com/is.jpg\">"
            "<div itemprop=\"image\" content=\"https://cdn.example.com/ip.jpg\"></div>";
        const auto c = art_page_image_candidates(html, base);
        bool hasIs = false, hasIp = false;
        for (const auto& x : c) { if (x.url == "https://cdn.example.com/is.jpg") hasIs = true;
                                  if (x.url == "https://cdn.example.com/ip.jpg") hasIp = true; }
        check(hasIs, "link rel=image_src 应抽到（含 // 协议相对）");
        check(hasIp, "itemprop=image 应抽到");
    }
    {   // JS 变量兜底："pic":"..."（网易云那类接口回包内联在页面里）
        const std::string html =
            "<script>window.__DATA__={\"singer\":{\"pic\":\"https:\\/\\/p2.music.126.net\\/x.jpg\"}};</script>";
        const auto c = art_page_image_candidates(html, base);
        check(!c.empty(), "JS 变量 pic 应抽到");
        if (!c.empty()) checkEq(c.front().url, "https://p2.music.126.net/x.jpg", "JSON 转义的 \\/ 应还原");
    }
    {   // <img> 启发式：头像类 URL 得分应高于 logo/图标（后者被跳过）
        const std::string html =
            "<img src=\"/static/logo.png\" width=\"32\" height=\"32\">"
            "<img src=\"/upload/avatar/7788.jpg\" class=\"artist-avatar\" width=\"300\" height=\"300\">";
        const auto c = art_page_image_candidates(html, base, "某人");
        check(!c.empty(), "<img> 应抽到候选");
        if (!c.empty()) {
            checkEq(c.front().url, "https://example.com/upload/avatar/7788.jpg", "头像图应排第一");
            check(c.front().url.find("logo") == std::string::npos, "logo 必须被跳过");
        }
    }
    {   // SPA 页面（dizzylab 的实测形态）：没有 og:image ⟹ 抽不到就该是 0，而不是瞎猜
        const std::string html =
            "<html><body><div id=\"app\"></div><script src=\"/js/app.4f2c.js\"></script></body></html>";
        const auto c = art_page_image_candidates(html, base);
        check(c.empty(), "SPA 壳页应当抽不到候选（宁缺毋错）");
    }
    {   // 反例 2：非图片内容页面（纯文本）—— 抽不到任何图
        const std::string html = "<html><body><p>404 Not Found</p></body></html>";
        const auto c = art_page_image_candidates(html, base);
        check(c.empty(), "纯文本页不应抽到候选");
    }
}

// ===========================================================================
// 3) 图片闸（判据 #4 的反例 3：1×1 像素占位图）
// ===========================================================================
static void testImageGate() {
    std::printf("[3] 图片闸 / 尺寸\n");
    int w = 0, h = 0;
    const std::string png1 = makePng(1, 1, 120);
    check(art_image_dims(png1, &w, &h) && w == 1 && h == 1, "1x1 PNG 尺寸应解析出 1x1");
    std::string why;
    check(!art_image_ok(png1, 64, 64, &why), "1x1 PNG 必须拒（字节闸 + 尺寸闸）");
    check(why.find("too_small") == 0, "拒绝原因应是 too_small*");

    // 1x1 GIF 灌到 4 KB：字节闸拦不住，**只能靠尺寸闸**（这就是加尺寸校验的理由）
    const std::string gif1 = makeGif(1, 1, 4096);
    check(art_image_dims(gif1, &w, &h) && w == 1 && h == 1, "1x1 GIF 尺寸应解析出 1x1");
    check(!art_image_ok(gif1, 2048, 64, &why), "灌到 4KB 的 1x1 GIF 也必须拒");
    checkEq(why, "too_small_dims 1x1", "拒绝原因应是 too_small_dims");

    const std::string png600 = makePng(600, 600, 5000);
    check(art_image_ok(png600, 2048, 64, &why), "600x600 PNG 应通过");
    check(why.empty(), "通过时 why 应为空");

    // 非图片字节（HTML 错误页 / JSON 回包）
    const std::string notImg = "<html><body><h1>403 Forbidden</h1><p>hotlink denied by CDN policy, "
                               "please visit the site.</p></body></html>";
    check(!art_image_ok(notImg, 64, 64, &why), "非图片内容必须拒");
    checkEq(why, "not_image", "拒绝原因应是 not_image");

    // 长宽比闸：64x316 的"唱片条"装饰图必须拒（实测被当成头像落过盘）
    const std::string strip = makePng(64, 316, 4096);
    check(!art_image_ok(strip, 2048, 64, &why), "64x316 横幅/装饰条必须拒");
    checkEq(why, "bad_aspect 64x316", "拒绝原因应是 bad_aspect");
    check(art_image_ok(makePng(640, 420, 5000), 2048, 64, &why), "640x420 应通过（比例 1.52）");
    check(art_image_ok(makePng(1200, 630, 5000), 2048, 64, &why), "1200x630 应通过（比例 1.90）");
    check(!art_image_ok(makePng(867, 225, 5000), 2048, 64, &why), "867x225 站招横幅必须拒");
    // 太小（< 字节下限）
    check(!art_image_ok(makePng(300, 300, 500), 2048, 64, &why), "小于字节下限必须拒");
    check(why.find("too_small") == 0, "拒绝原因应是 too_small*");
}

// ===========================================================================
// 4) 名字验收（简繁折叠 / 去括号 / 不误配）
// ===========================================================================
static void testNames() {
    std::printf("[4] 名字验收\n");
    check(art_name_same_loose("天門", "天门"), "天門 应折叠命中 天门");
    check(art_name_same_loose("天门", "天門"), "反向也应命中");
    check(art_name_same_loose("水瀬いのり", "水瀬いのり (水濑祈)"), "去括号后应命中");
    check(art_name_same_loose("手嶌葵", "手嶌 葵"), "去空格后应命中");
    check(art_name_same_loose("Inori Minase", "inori minase"), "大小写无关");
    check(!art_name_same_loose("徐娇", "徐瞑蕾"), "徐娇 绝不能命中 徐瞑蕾");
    check(!art_name_same_loose("天野正道", "天门"), "天野正道 绝不能命中 天门");
    check(!art_name_same_loose("水瀬マユ", "水瀬いのり (水濑祈)"), "水瀬マユ 绝不能命中 水瀬いのり");
    check(!art_name_same_loose("", "天门"), "空名不命中");
    // 变体生成：括号版应同时给出简写
    const auto vs = art_name_variants("水瀬いのり (水濑祈)");
    bool hasShort = false;
    for (const auto& v : vs) if (v == "水瀬いのり") hasShort = true;
    check(hasShort, "变体应包含去括号后的名字");
}

// ===========================================================================
// 5) 扩展源（HTTP 钩子；只验"同名才收"这条纪律）
// ===========================================================================
static void testExtSources() {
    std::printf("[5] 扩展源（假响应）\n");
    const std::string png = makePng(400, 400, 4096);
    ArtworkOpts o;
    o.useCache = false;
    o.perSourceMs = 3000;
    o.downloadMs = 3000;
    o.totalBudgetMs = 9000;
    o.minBytes = 1024;

    // bangumi：第一个结果不同名，第二个同名 ⟹ 必须用第二个
    {
        set_artwork_http_for_test([&](const ArtHttpRequest& q) {
            rta::net::HttpResponse r;
            r.status = 200;
            if (q.url.find("api.bgm.tv/v0/search/persons") != std::string::npos) {
                const std::string js =
                    "{\"data\":[{\"id\":1,\"name\":\"天野喜孝\",\"images\":{\"large\":\"https://lain.bgm.tv/a.jpg\"}},"
                    "{\"id\":2,\"name\":\"天門\",\"images\":{\"large\":\"https://lain.bgm.tv/b.jpg\"}}]}";
                r.body.assign(js.begin(), js.end());
                r.ok = true;
                return r;
            }
            if (q.url.find("lain.bgm.tv") != std::string::npos) {
                r.body.assign(png.begin(), png.end());
                r.ok = true;
                return r;
            }
            r.status = 404;
            return r;
        });
        const ArtWebFill r = art_ext_artist_image("天门", o);
        check(r.ok, "bangumi 同名命中应成功");
        checkEq(r.url, "https://lain.bgm.tv/b.jpg", "必须用同名那条（不是第一个结果）");
        check(r.source.rfind("bgm", 0) == 0, "命中源应是 bgm");
    }
    // 三家都不同名 ⟹ 必须留空（宁缺毋错）
    {
        set_artwork_http_for_test([&](const ArtHttpRequest& q) {
            rta::net::HttpResponse r;
            r.status = 200;
            if (q.url.find("api.bgm.tv") != std::string::npos) {
                const std::string js = "{\"data\":[{\"id\":9,\"name\":\"徐娇\",\"images\":{\"large\":\"https://lain.bgm.tv/x.jpg\"}}]}";
                r.body.assign(js.begin(), js.end());
                r.ok = true;
                return r;
            }
            if (q.url.find("graphql.anilist.co") != std::string::npos) {
                const std::string js = "{\"data\":{\"Page\":{\"staff\":[{\"name\":{\"full\":\"Someone\",\"native\":\"某\"},\"image\":{\"large\":\"https://s4.anilist.co/x.jpg\"}}]}}}";
                r.body.assign(js.begin(), js.end());
                r.ok = true;
                return r;
            }
            if (q.url.find("itunes.apple.com") != std::string::npos) {
                const std::string js = "{\"resultCount\":1,\"results\":[{\"artistName\":\"Nobody\",\"artistId\":42}]}";
                r.body.assign(js.begin(), js.end());
                r.ok = true;
                return r;
            }
            r.status = 404;
            return r;
        });
        const ArtWebFill r = art_ext_artist_image("徐瞑蕾", o);
        check(!r.ok, "没有同名候选时必须留空（不许张冠李戴）");
        check(!r.tried.empty(), "失败也要留逐源原因");
    }
    // AniList：native 同名命中 + 默认头像要跳过
    {
        set_artwork_http_for_test([&](const ArtHttpRequest& q) {
            rta::net::HttpResponse r;
            r.status = 200;
            if (q.url.find("graphql.anilist.co") != std::string::npos) {
                const std::string js =
                    "{\"data\":{\"Page\":{\"staff\":["
                    "{\"name\":{\"full\":\"A\",\"native\":\"水瀬いのり\"},\"image\":{\"large\":\"https://s4.anilist.co/a.jpg\"}}]}}}";
                r.body.assign(js.begin(), js.end());
                r.ok = true;
                return r;
            }
            if (q.url.find("s4.anilist.co") != std::string::npos) {
                r.body.assign(png.begin(), png.end());
                r.ok = true;
                return r;
            }
            if (q.url.find("api.bgm.tv") != std::string::npos) {   // bangumi 先跑：让它 404
                r.status = 404;
                return r;
            }
            r.status = 404;
            return r;
        });
        const ArtWebFill r = art_ext_artist_image("水瀬いのり", o);
        check(r.ok, "AniList native 同名应命中");
        checkEq(r.url, "https://s4.anilist.co/a.jpg", "应用 AniList 给的那张");
    }
    set_artwork_http_for_test(nullptr);
}


// ===========================================================================
// 6) 精度（2026-10-27 实测事故回归）：面板页/家具图/无关页 一律不许"命中"
// ===========================================================================
static void testPrecision() {
    std::printf("[6] 精度（错图比空白更糟）\n");
    // ① 页面里没提到这个名字 ⟹ 即便有漂亮的 og:image 也不许用（百度百科《守候》事故）
    const std::string otherPage =
        "<html><head><meta property=\"og:image\" content=\"https://bkimg.cdn.bcebos.com/pic/x.jpg\">"
        "<title>守候（歌曲）</title></head><body>守候是一首歌…</body></html>";
    check(art_html_has_name(otherPage, "徐瞑蕾") == false, "无关页不得被判为提到名字");
    check(art_html_has_name(otherPage, "守候") == true, "同名页应被判为提到名字");
    const std::string personPage =
        "<html><body><h1>Mugai无害P</h1><p>同人音乐社团</p></body></html>";
    check(art_html_has_name(personPage, "Mugai无害P"), "提到名字的页应通过相关性闸");
    // 简写也算（"水瀬いのり (水濑祈)" 的页面通常只写前半）
    check(art_html_has_name("<html><body>水瀬いのり 歌手</body></html>", "水瀬いのり (水濑祈)"),
          "去括号简写应通过");
    // ② 站点家具（dizzylab 的 /static/dl-n1.jpg）不许当头像（实测落错过一次）
    const std::string furniture =
        "<html><body><h1>Mugai无害P</h1>"
        "<img src=\"https://cdn.dizzylab.net/static/dl-n1.jpg\" width=\"867\" height=\"225\">"
        "<img src=\"/img/logo.png\" width=\"200\" height=\"200\">"
        "</body></html>";
    const auto c = art_page_image_candidates(furniture, "https://www.dizzylab.net/d/MGAI-01/", "Mugai无害P");
    bool pickedFurniture = false;
    for (const auto& x : c)
        if (x.url.find("/static/") != std::string::npos || x.url.find("logo") != std::string::npos)
            pickedFurniture = true;
    check(!pickedFurniture, "站点家具/横幅/logo 必须被跳过");
    // ③ 真正带 avatar 暗示的图仍然要收（别把精度修成"什么都不收"）
    const std::string good =
        "<html><body><h1>Mugai无害P</h1>"
        "<img src=\"/media/avatars/16157_dizzz.jpg!cover\" class=\"user-avatar\" width=\"300\" height=\"300\">"
        "</body></html>";
    const auto c2 = art_page_image_candidates(good, "https://www.dizzylab.net/d/MGAI-01/", "Mugai无害P");
    check(!c2.empty() && c2.front().url.find("/media/avatars/") != std::string::npos,
          "带 avatar 暗示的图应收");
}


// ===========================================================================
// 7) 标题闸（把「人物条目」和「作品条目」分开）
// ===========================================================================
static void testTitleGate() {
    std::printf("[7] 标题闸\n");
    const std::string songPage =
        "<html><head><title>守候（歌曲）_百度百科</title>"
        "<meta property=\"og:image\" content=\"https://bkimg.cdn.bcebos.com/pic/x.jpg\"></head>"
        "<body>演唱者为徐瞑蕾（亦写作徐瞑蕾）</body></html>";
    check(art_html_title(songPage).find("守候") != std::string::npos, "<title> 应抽到标题");
    check(!art_title_has_name(art_html_title(songPage), "徐瞑蕾"), "作品条目的标题不含人名 ⟹ 必须拒");
    check(art_html_has_name(songPage, "徐瞑蕾"), "正文闸会放它 —— 正因为如此才需要标题闸");

    const std::string personPage =
        "<html><head><title>徐瞑蕾 - 网易云音乐</title></head><body>徐瞑蕾</body></html>";
    check(art_title_has_name(art_html_title(personPage), "徐瞑蕾"), "人物条目应通过标题闸");

    const std::string enPage =
        "<html><head><title>Mugai - Anihk</title></head><body>Mugai Mushibugyo</body></html>";
    check(art_title_has_name(art_html_title(enPage), "Mugai无害P"),
          "纯英文条目页应靠 ASCII 词（Mugai）通过");

    const std::string noise =
        "<html><head><title>音乐推荐 - 首页</title></head><body>Mugai无害P</body></html>";
    check(!art_title_has_name(art_html_title(noise), "Mugai无害P"), "标题不含名字的推荐页必须拒");
    // og:title 也算标题
    const std::string ogOnly =
        "<html><head><meta property=\"og:title\" content=\"手嶌葵 | 歌手\"></head><body></body></html>";
    check(art_title_has_name(art_html_title(ogOnly), "手嶌 葵"), "og:title 应被当作标题");
}

int main() {
    testUrlPolicy();
    testExtract();
    testImageGate();
    testNames();
    testExtSources();
    testPrecision();
    testTitleGate();
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
