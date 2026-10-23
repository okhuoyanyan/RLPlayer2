// test_dizzylab.cpp — dizzylab provider(src/meta/provider.h「dizzylab」节):离线为主
//
// 为什么单独一个文件(而不是追加到 test_metaproviders.cpp):
//   * test_metaproviders.cpp 已 549 行、按「四源 + 注册表」组织,注册表断言写死了 4 项与
//     qq/netease/itunes/lrclib 顺序 —— 新增源必然改它(本轮只做最小必要改动:数量 4→5、
//     顺序与 canLyric 断言),把 dizzylab 的全部用例塞进去会让那个文件失去主题;
//   * dizzylab 的 HTML 提取面自己就够大(实体反转义 / 卡片切分 / 曲目行切分 / 关键词阶梯 /
//     熔断限频),与 test_enrich.cpp、test_metamatch.cpp「一模块一文件」的既有惯例一致;
//   * 并行开发时(另一子代理在动 entities.h + CMakeLists)独立文件不会撞车。
//
// 判据(Fixture 来源,铁律 3「测对东西」):
//   * 下面的 HTML 片段全部是 2026-10-22 本轮**真实只读 GET** 抓下来的原文切片
//     (https://www.dizzylab.net/search/?s=… 与 /d/<catalog>),不是拟合出来的假数据。
//     抓取原始文件另存 build/dizzylab_probe/(未入库,仅供复核)。
//   * 断言不只「有输出」,而是逐字段对齐页面里肉眼可见的值:曲名/艺术家/时长/封面 URL/
//     社团名;时长还用 ffprobe 对同目录 MUSIC 文件实测过交叉验证:
//       GKSL-003 第 1 轨 02:11 = 131s ↔ 1 Pixel Library.flac 实测 131.6s
//       SSP1     第 1 轨 04:15 = 255s ↔ 1 Pick me!.flac     实测 255.0s
//     (⚠️ 但 enrich 侧 fileSec 恒 0,见 provider.h 头注已知边界 —— 时长信号端到端未生效。)
//   * 真网测试全部 opt-in(RTA_NET_TESTS=1),默认 ctest 在断网机器上必须全绿。
//
// ⚠️ 未解决项(铁律 4):真网用例的判据是「首页命中 + 字段非空」,不含 dizzylab 改版后的
//     自动适配;站点结构一变,离线用例会先红(这是有意的:结构变了就该有人看)。
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "meta/provider.h"
#include "meta/enrich.h"
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace rta::meta;

static int g_fail = 0;
static void ck(bool cond, const char* what, const std::string& detail = "") {
    if (cond) std::printf("  [PASS] %s\n", what);
    else { std::printf("  [FAIL] %s  %s\n", what, detail.c_str()); ++g_fail; }
}
static void ck_eq(const std::string& got, const std::string& want, const char* what) {
    ck(got == want, what, "got=[" + got + "] want=[" + want + "]");
}

// ---------------------------------------------------------------------------
// Fixture A:真实搜索页「作品」段切片(2026-10-22 GET /search/?s=月華社)
// 原文含两张卡片 + 卡片内 scoped <style> + 社团简介 h3(class=truncate-limit),
// 正是「从卡片里取第一个 h1/h3」要处理的形态:第一个 h3 是社团名,第二个是简介。
// ---------------------------------------------------------------------------
static const char* kSearchWorksSection =
    "<hr class=\"border-1 border-dark\" style=\"margin-block: 36px;\" />\n"
    "<h4 class=\"text-warning\">作品</h4>\n"
    "<a href=\"/d/GKCD-013\">\n"
    "<div class=\"card\" style=\"border:0px;margin-top:24px\">\n"
    "<div class=\"card-body\" style='padding:1px'>\n"
    "<img src=\"https://cdn.dizzylab.net/media/cover/GKCD-013.jpg!cover\" width=\"200\" "
    "height=\"200\" style=\"margin-bottom:1px;\" class=\"lazyload float-left\" />\n"
    "<h1>ORDINARY MIRACLES</h1>\n"
    "<h3>月華社</h3>\n"
    "<style type=\"text/css\" scoped>.truncate-limit{overflow:hidden;text-overflow:ellipsis;"
    "display:-webkit-box;-webkit-line-clamp:3;}</style>\n"
    "<h3 class=\"truncate-limit\">感谢您光临本店！</h3>\n"
    "</div>\n</div>\n</a>\n"
    "<a href=\"/d/GKCD-012\">\n"
    "<div class=\"card\" style=\"border:0px;margin-top:24px\">\n"
    "<div class=\"card-body\" style='padding:1px'>\n"
    "<img src=\"https://cdn.dizzylab.net/media/cover/GKCD-012.jpg!cover\" width=\"200\" "
    "height=\"200\" class=\"lazyload float-left\" />\n"
    "<h1>Routine LP</h1>\n"
    "<h3>月華社</h3>\n"
    "<style type=\"text/css\" scoped>.truncate-limit{overflow:hidden;}</style>\n"
    "<h3 class=\"truncate-limit\">循。</h3>\n"
    "</div>\n</div>\n</a>\n"
    "<h4 class=\"text-warning\">用户</h4>\n"
    "<a href=\"/u/someuser\"><h1>不该被取到的用户</h1><h3>用户段</h3></a>\n";

// ---------------------------------------------------------------------------
// Fixture B:真实专辑页曲目表切片(2026-10-22 GET /d/GKSL-003,2 轨)
// 曲目表包在 <li data-id data-audio> 里(data-audio 是串流地址 —— 本测试只读不取)。
// 页头是真实的作品名 h1 与社团 h1 > a[href^=/l/]。
// ---------------------------------------------------------------------------
static const char* kAlbumGksl003 =
    "<!DOCTYPE html>\n<html lang=\"zh-CN\">\n<head>\n"
    "<title>2 Pixels - dizzylab</title>\n"
    "<meta name=\"description\" content='来自月華社的2 Pixels:仅供购买了本社相关挂件的听众兑换。' />\n"
    "</head>\n<body>\n"
    "<h1 style='margin-top:18px'>2 Pixels</h1>\n"
    "<h1><span id='likes_indicator' class=\"badge badge-pill\"><i class=\"fa fa-heart\"></i></span></h1>\n"
    "<h1 class=\"p-1\" style=\"margin-top: 12px;\"><a href='/l/\xE6\x9C\x88\xE8\x8F\xAF\xE7\xA4\xBE'>"
    "\xE6\x9C\x88\xE8\x8F\xAF\xE7\xA4\xBE</a></h1>\n"
    "<ul class='playlist--list' style='margin-top:6px'>\n"
    "<li style=\"font-size:12px\" data-id=\"0\" data-audio=\"https://streaming.dizzylab.net/2026/GKSL-003/full/1.mp3\">\n"
    "<div class='rollbox'>\n"
    "<div class=\"row justify-content-between align-items-center w-100\">\n"
    "<div class=\"col\">\n"
    "<span class=\"t-title\">1. Pixel Library - Khrisma (02:11)</span>\n"
    "</div>\n</div>\n</div>\n</li>\n"
    "<li style=\"font-size:12px\" data-id=\"1\" data-audio=\"https://streaming.dizzylab.net/2026/GKSL-003/full/2.mp3\">\n"
    "<div class='rollbox'>\n"
    "<div class=\"row justify-content-between align-items-center w-100\">\n"
    "<div class=\"col\">\n"
    "<span class=\"t-title\">2. Retro Shoujo - Khrisma (02:21)</span>\n"
    "</div>\n</div>\n</div>\n</li>\n"
    "</ul>\n</body>\n</html>\n";

// ---------------------------------------------------------------------------
// Fixture C:NE-06 专辑页两轨(真实原文,含 &#39; 实体 —— 页面里 Alice&#39;s Adventures)
// 这条同时验证「HTML 实体反转义」在 TRUE 路径上真的跑了(反向验证见报告)。
// ---------------------------------------------------------------------------
static const char* kAlbumNe06 =
    "<html><head><title>\xE8\xA2\xAB\xE6\xB5\x93\xE9\x9B\xBE\xE9\x81\xAE\xE7\x9B\x96\xE7\x9A\x84\xE6\xA3\xAE\xE6\x9E\x97 - dizzylab</title></head>\n"
    "<body>\n"
    "<h1 style='margin-top:18px'>\xE8\xA2\xAB\xE6\xB5\x93\xE9\x9B\xBE\xE9\x81\xAE\xE7\x9B\x96\xE7\x9A\x84\xE6\xA3\xAE\xE6\x9E\x97</h1>\n"
    "<h1 class=\"p-1\" style=\"margin-top: 12px;\"><a href='/l/NEKOKOSTORE'>NEKOKOSTORE</a></h1>\n"
    "<h3 class=\"text-left\">\n"
    "\xE6\x97\xA7\xE7\x81\xAB<br />Composed &amp; Arranged\xEF\xBC\x9AHagali<br />"
    "Music &amp; Lyrics by Rockflos123<br />Alice&#39;s Adventures in Wonderland(1865)<br />\n"
    "</h3>\n"
    "<li style=\"font-size:12px\" data-id=\"0\" data-audio=\"https://streaming.dizzylab.net/x/NE-06/full/1.mp3\">\n"
    "<span class=\"t-title\">1. intro - Rockflos123 (01:32)</span>\n</li>\n"
    "<li style=\"font-size:12px\" data-id=\"1\" data-audio=\"https://streaming.dizzylab.net/x/NE-06/full/2.mp3\">\n"
    "<span class=\"t-title\">2.  \xE9\x9B\xA8 - Rockflos123 (04:28)</span>\n</li>\n"
    "</body></html>\n";

// ---------------------------------------------------------------------------
// Fixture D:SSP1 第 1 轨(真实原文,艺术家串很长且带 feat.,验证不按逗号/括号乱切)
// ---------------------------------------------------------------------------
static const char* kAlbumSsp1 =
    "<html><body>\n"
    "<h1 style='margin-top:18px'>SSParty!!</h1>\n"
    "<h1 class=\"p-1\"><a href='/l/Haru.jpg'>Haru.jpg</a></h1>\n"
    "<li data-id=\"0\" data-audio=\"https://streaming.dizzylab.net/x/SSP1/full/1.mp3\">\n"
    "<span class=\"t-title\">1. Pick me! - Peak( \xE3\x81\x8B\xE3\x82\x8A\xE3\x82\x93\xE3\x81\xA8, "
    "\xE3\x81\x82\xE3\x81\x95\xE3\x81\xAC\xE3\x81\x93, \xE6\xB0\xB7\xE9\xA3\x9F ) "
    "feat. IA, \xE9\x9F\xB3\xE8\xA1\x97\xE3\x82\xA6\xE3\x83\x8A (04:15)</span>\n</li>\n"
    "</body></html>\n";

// ===========================================================================
int main() {
    SetConsoleOutputCP(CP_UTF8);
    std::printf("== meta/provider.h · dizzylab(离线为主)==\n");

    // ---------------- 1) HTML 实体反转义 ----------------
    std::printf("-- pv_html_unescape --\n");
    {
        ck_eq(pv_html_unescape("Composed &amp; Arranged"), "Composed & Arranged",
              "&amp; → &(实测 MhT-TH21 曲目说明里的原文形态)");
        ck_eq(pv_html_unescape("Alice&#39;s Adventures"), "Alice's Adventures",
              "&#39; → '(实测 NE-06 页面里的原文形态)");
        ck_eq(pv_html_unescape("&lt;b&gt;"), "<b>", "&lt;/&gt;");
        ck_eq(pv_html_unescape("&quot;x&quot;"), "\"x\"", "&quot;");
        ck_eq(pv_html_unescape("a&nbsp;b"), "a b", "&nbsp; → 空格");
        ck_eq(pv_html_unescape("&apos;"), "'", "&apos;");
        // 数字实体:十进制 / 十六进制(大写 X 也认)
        ck_eq(pv_html_unescape("&#65;&#66;&#67;"), "ABC", "十进制数字实体");
        ck_eq(pv_html_unescape("&#x41;&#X42;&#x4a;"), "ABJ", "十六进制数字实体(含大写 X)");
        ck_eq(pv_html_unescape("&#x4E2D;&#x6587;"), "\xE4\xB8\xAD\xE6\x96\x87",
              "数字实体 → 中文(多字节码点正确编码)");
        ck_eq(pv_html_unescape("&#28450;&#23383;"), "\xE6\xBC\xA2\xE5\xAD\x97",
              "十进制 → 中文");
        // 边界:未知实体/非法数字/无分号一律原样保留(不猜、不丢)
        ck_eq(pv_html_unescape("&foo;"), "&foo;", "未知实体原样保留");
        ck_eq(pv_html_unescape("&#;"), "&#;", "空数字实体原样保留");
        ck_eq(pv_html_unescape("&#xZZ;"), "&#xZZ;", "非法十六进制原样保留");
        ck_eq(pv_html_unescape("a & b"), "a & b", "裸 & (无分号)原样保留");
        ck_eq(pv_html_unescape("&amp"), "&amp", "缺分号不反转义");
        ck_eq(pv_html_unescape(""), "", "空串");
        ck_eq(pv_html_unescape("&amp;amp;"), "&amp;", "只解一层(不递归,防二次解码)");
    }

    // ---------------- 2) HTML → 纯文本(丢 style/script/标签) ----------------
    std::printf("-- pv_html_text --\n");
    {
        ck_eq(pv_html_text("<h1>2 Pixels</h1>"), "2 Pixels", "标签剥离");
        ck_eq(pv_html_text("  <b> a </b>  c "), "a c", "标签当空白、折叠、trim");
        ck_eq(pv_html_text("<style scoped>.truncate-limit{overflow:hidden;}</style>标题"),
              "标题", "scoped <style> 整段丢弃(CSS 文本不污染标题)");
        ck_eq(pv_html_text("<script>var a=1;</script>x"), "x", "<script> 丢弃");
        ck_eq(pv_html_text("a&lt;b"), "a<b", "反转义在剥离之后做,不把实体当标签");
        ck_eq(pv_html_text("未闭合<b"), "未闭合", "标签未闭合:截断降级,不崩");
        ck_eq(pv_html_text("<style>未闭合"), "", "未闭合 style:整段丢弃");
    }

    // ---------------- 3) URL 构造(整串精确) ----------------
    std::printf("-- dizzylab URL 构造 --\n");
    {
        ck_eq(dizzylab_build_search_url("Pixel Library"),
              "https://www.dizzylab.net/search/?s=Pixel%20Library",
              "搜索 URL:参数名必须是 s、空格编码 %20");
        ck_eq(dizzylab_build_search_url("\xE6\x9C\x88\xE8\x8F\xAF\xE7\xA4\xBE"),
              "https://www.dizzylab.net/search/?s=%E6%9C%88%E8%8F%AF%E7%A4%BE",
              "中文关键词 percent 编码");
        ck_eq(dizzylab_build_album_url("MhT-TH21"), "https://www.dizzylab.net/d/MhT-TH21",
              "专辑 URL");
        ck_eq(dizzylab_cover_url("GKSL-003"),
              "https://cdn.dizzylab.net/media/cover/GKSL-003.jpg",
              "封面 URL 按编号拼(实测该 URL 200)");
        // 编码后过一遍 http_client 的 path 编码必须不变(与四源同一条双重编码守卫)
        const std::string su = dizzylab_build_search_url("\xE6\x9C\x88 \xE8\x8F\xAF");
        ck(rta::net::percent_encode_path(su) == su, "搜索 URL 无双重编码", su);
        // 编号白名单:非法编号不拼 URL(防把卡片里的怪字符带进请求路径)
        ck(dizzylab_catalog_ok("GKSL-003") && dizzylab_catalog_ok("MhT-TH21") &&
           dizzylab_catalog_ok("SSP1"),
           "合法编号通过白名单");
        ck(!dizzylab_catalog_ok("../../etc") && !dizzylab_catalog_ok("a b") &&
           !dizzylab_catalog_ok("") && !dizzylab_catalog_ok("a/b"),
           "非法编号(路径穿越/空格/空/斜杠)被拒");
    }

    // ---------------- 4) 搜索页解析(真实作品段切片) ----------------
    std::printf("-- parse_dizzylab_search --\n");
    {
        std::string err = "sentinel";
        const std::vector<Candidate> out = parse_dizzylab_search(kSearchWorksSection, err);
        ck(err.empty(), "真实作品段:err 空", err);
        ck(out.size() == 2, "只取「作品」段的 2 张卡片(用户段不取)",
           std::to_string(out.size()));
        if (out.size() == 2) {
            ck(out[0].source == "dizzylab", "source = dizzylab", out[0].source);
            ck_eq(out[0].album, "ORDINARY MIRACLES", "卡片1 作品名 = 第一个 h1");
            ck_eq(out[0].artist, "\xE6\x9C\x88\xE8\x8F\xAF\xE7\xA4\xBE",
                  "卡片1 社团名 = 第一个 h3(不是 class=truncate-limit 的简介)");
            ck_eq(out[0].title, "ORDINARY MIRACLES", "title 与 album 同为作品名");
            ck_eq(out[0].albumId, "GKCD-013", "albumId = /d/ 后的编号");
            ck_eq(out[0].coverUrl, "https://cdn.dizzylab.net/media/cover/GKCD-013.jpg",
                  "coverUrl 按编号拼");
            ck_eq(out[1].album, "Routine LP", "卡片2 作品名");
            ck_eq(out[1].albumId, "GKCD-012", "卡片2 编号");
            // 简介 h3 绝不能混进 artist(实测简介是「感谢您光临本店！」)
            ck(out[0].artist.find("\xE6\x84\x9F\xE8\xB0\xA2") == std::string::npos,
               "scoped style 与简介都没污染社团名", out[0].artist);
        }

        // 结构缺失:只有半个卡片(h1 在,</a> 缺)→ 优雅降级,不崩
        std::string e2;
        const std::string halfCard =
            "<h4 class=\"text-warning\">作品</h4><a href=\"/d/HALF-01\">"
            "<h1>半张卡片</h1>";
        const std::vector<Candidate> h = parse_dizzylab_search(halfCard, e2);
        ck(h.size() == 1 && h[0].album == "\xE5\x8D\x8A\xE5\xBC\xA0\xE5\x8D\xA1\xE7\x89\x87",
           "半个卡片(h1 有、</a> 缺)仍解析出条目(降级不崩)",
           h.empty() ? "(空)" : h[0].album);

        // 真实零结果页(2026-10-22 实测 GET /search/?s=zzzqqq不存在,11342 字节):
        // ⚠️ 该页面**一个 <h4 class="text-warning"> 段都没有**(社团/作品/用户三段全不渲染),
        // 只剩搜索表单 —— 所以「没有作品段」不能当作结构变化的证据。下面是该页的关键骨架切片。
        std::string e3 = "sentinel";
        const std::string zeroPage =
            "<!DOCTYPE html><html lang=\"zh-CN\"><head><title>搜索 - dizzylab</title></head>\n"
            "<body class='bg-primary p-0 m-0'>\n"
            "<form action=\"/albums/search\"><input type=\"search\" name=\"s\" "
            "id=\"search-field\" value=\"zzzqqq\" /></form>\n"
            "<hr class=\"border-1 border-dark\" style=\"margin-block: 36px;\" />\n"
            "<p class=\"text-center\"><a href=\"/tags\" class=\"text-info\">#查看所有标签</a></p>\n"
            "</body></html>";
        const std::vector<Candidate> z = parse_dizzylab_search(zeroPage, e3);
        ck(z.empty() && e3.empty(), "真实零结果页(无 h4 段):返回空且 err 为空(合法空结果)", e3);

        // 不是 HTML(网关错误页/空响应)→ 如实报错
        std::string e4;
        const std::vector<Candidate> g = parse_dizzylab_search("gateway timeout", e4);
        ck(g.empty() && !e4.empty(), "非 HTML 响应 → err", e4);

        // 卡片没有 h1 → 该卡片丢弃(宁缺:没作品名无从匹配)
        std::string e5;
        const std::vector<Candidate> n =
            parse_dizzylab_search("<h4 class=\"text-warning\">作品</h4><a href=\"/d/NOH1-01\">"
                                  "<h3>某社团</h3></a>", e5);
        ck(n.empty() && e5.empty(), "缺 h1 的卡片被丢弃(降级)", std::to_string(n.size()));

        // 编号非法 → 跳过该卡片(不拼 URL)
        std::string e6;
        const std::vector<Candidate> bad =
            parse_dizzylab_search("<h4 class=\"text-warning\">作品</h4><a href=\"/d/a b\">"
                                  "<h1>x</h1><h3>y</h3></a>", e6);
        ck(bad.empty(), "非法编号的卡片被跳过", std::to_string(bad.size()));
    }

    // ---------------- 5) 专辑页解析(真实 NE-06 / GKSL-003 / SSP1) ----------------
    std::printf("-- parse_dizzylab_album --\n");
    {
        std::string err;
        const std::vector<Candidate> g = parse_dizzylab_album(kAlbumGksl003, "GKSL-003", err);
        ck(err.empty(), "GKSL-003:err 空", err);
        ck(g.size() == 2, "GKSL-003:2 轨(实测该作品只有两首)", std::to_string(g.size()));
        if (g.size() == 2) {
            ck_eq(g[0].title, "Pixel Library", "轨1 曲名(剥掉序号与时长)");
            ck_eq(g[0].artist, "Khrisma", "轨1 艺术家(取自曲目行)");
            ck_eq(g[0].album, "2 Pixels", "专辑名 = 页面第一个 h1");
            ck_eq(g[0].albumId, "GKSL-003", "albumId = catalog");
            ck(g[0].durationMs == 131000, "轨1 时长 02:11 → 131000ms(ffprobe 实测 131.6s)",
               std::to_string(g[0].durationMs));
            ck_eq(g[0].coverUrl, "https://cdn.dizzylab.net/media/cover/GKSL-003.jpg",
                  "coverUrl");
            ck_eq(g[1].title, "Retro Shoujo", "轨2 曲名");
            ck(g[1].durationMs == 141000, "轨2 时长 02:21 → 141000ms",
               std::to_string(g[1].durationMs));
        }

        std::string e2;
        const std::vector<Candidate> n = parse_dizzylab_album(kAlbumNe06, "NE-06", e2);
        ck(e2.empty(), "NE-06:err 空", e2);
        ck(n.size() == 2, "NE-06 切片 2 轨", std::to_string(n.size()));
        if (n.size() == 2) {
            ck_eq(n[0].title, "intro", "NE-06 轨1 曲名");
            ck_eq(n[0].artist, "Rockflos123", "NE-06 轨1 艺术家");
            ck(n[0].durationMs == 92000, "NE-06 轨1 01:32 → 92000ms",
               std::to_string(n[0].durationMs));
            ck_eq(n[1].title, "\xE9\x9B\xA8", "NE-06 轨2 曲名「雨」(行内双空格也要吃掉)");
            ck(n[1].durationMs == 268000, "NE-06 轨2 04:28 → 268000ms",
               std::to_string(n[1].durationMs));
        }

        std::string e3;
        const std::vector<Candidate> s = parse_dizzylab_album(kAlbumSsp1, "SSP1", e3);
        ck(s.size() == 1, "SSP1 切片 1 轨", std::to_string(s.size()));
        if (s.size() == 1) {
            ck_eq(s[0].title, "Pick me!", "SSP1 曲名(艺术家串里的逗号/括号不干扰切分)");
            ck(s[0].artist.find("Peak(") == 0 && s[0].artist.find("feat. IA") != std::string::npos,
               "SSP1 艺术家串完整保留(feat. 不丢)", s[0].artist);
            ck(s[0].durationMs == 255000, "SSP1 04:15 → 255000ms(ffprobe 实测 255.0s)",
               std::to_string(s[0].durationMs));
        }

        // 社团名提取(album 页 /l/ 链接):search_dizzylab 用它在「曲目行没写艺术家」时兜底
        {
            std::string e7, circle;
            const std::vector<Candidate> cc =
                dl_parse_album_ex(kAlbumGksl003, "GKSL-003", e7, &circle);
            ck(cc.size() == 2 && circle == "\xE6\x9C\x88\xE8\x8F\xAF\xE7\xA4\xBE",
               "社团名 = h1 里 /l/ 链接的文本(月華社)", circle);
            // 没有 /l/ 链接时退到 <meta name=description content='来自<社团>的<作品>:...'>
            std::string e8, c8;
            dl_parse_album_ex("<html><head><meta name=\"description\" content='"
                              "\xE6\x9D\xA5\xE8\x87\xAA\xE6\x9F\x90\xE7\xA4\xBE"
                              "\xE7\x9A\x84\xE6\x9F\x90\xE4\xBD\x9C\xE5\x93\x81:x' />"
                              "</head><body><h1>X</h1>"
                              "<span class=\"t-title\">1. A - B (01:00)</span></body></html>",
                              "XX-01", e8, &c8);
            ck(c8 == "\xE6\x9F\x90\xE7\xA4\xBE", "社团名退路:meta description「来自<社团>的...」",
               c8);
        }

        // 结构变化:没有 t-title → 空 + err(如实,不当成成功)
        std::string e4;
        const std::vector<Candidate> none =
            parse_dizzylab_album("<html><h1>某作品</h1></html>", "XX-01", e4);
        ck(none.empty() && !e4.empty(), "无曲目表 → 空 + err", e4);

        // 编号非法 → 直接拒(不解析)
        std::string e5;
        ck(parse_dizzylab_album(kAlbumGksl003, "bad/id", e5).empty() && !e5.empty(),
           "非法编号 → 空 + err", e5);

        // 截断页(曲目表被切断在 </span> 之前)→ 已解析的轨保留、不崩
        std::string e6;
        const std::string trunc = std::string(kAlbumGksl003).substr(
            0, std::string(kAlbumGksl003).find("(02:11)"));
        const std::vector<Candidate> t = parse_dizzylab_album(trunc, "GKSL-003", e6);
        ck(t.empty(), "截断在轨名中途 → 该轨不产出(不产出半条脏数据)",
           std::to_string(t.size()));
    }

    // ---------------- 6) 单行曲目解析边界 ----------------
    std::printf("-- dl_parse_track_line --\n");
    {
        DlTrack t;
        ck(dl_parse_track_line("1. Pixel Library - Khrisma (02:11)", t) &&
           t.title == "Pixel Library" && t.artist == "Khrisma" && t.durationMs == 131000,
           "标准形态:序号/曲名/艺术家/时长", t.title + "|" + t.artist);
        // 标题自带 " - ":按**最后一个** " - " 切,标题里的破折保留
        ck(dl_parse_track_line("3. \xE6\xB3\xAA - \xE5\x9B\x9E - MhT (04:25)", t) &&
           t.title == "\xE6\xB3\xAA - \xE5\x9B\x9E" && t.artist == "MhT" &&
           t.durationMs == 265000,
           "标题内含 \" - \":按最后一个切", t.title + "|" + t.artist);
        // 没有时长
        ck(dl_parse_track_line("1. \xE6\x9F\x90\xE6\x9B\xB2 - \xE6\x9F\x90\xE8\x89\xBA\xE6\x9C\xAF", t) &&
           t.title == "\xE6\x9F\x90\xE6\x9B\xB2" && t.artist == "\xE6\x9F\x90\xE8\x89\xBA\xE6\x9C\xAF" &&
           t.durationMs == 0,
           "无时长 → durationMs 0(评分侧按中性)", std::to_string(t.durationMs));
        // 没有艺术家
        ck(dl_parse_track_line("5. \xE5\x8F\xAA\xE6\x9C\x89\xE6\x9B\xB2\xE5\x90\x8D (03:00)", t) &&
           t.title == "\xE5\x8F\xAA\xE6\x9C\x89\xE6\x9B\xB2\xE5\x90\x8D" && t.artist.empty() &&
           t.durationMs == 180000,
           "无艺术家 → artist 空", t.title + "|" + t.artist);
        // 小时级时长
        ck(dl_parse_track_line("1. Long - X (1:02:03)", t) && t.durationMs == 3723000,
           "H:MM:SS 也认", std::to_string(t.durationMs));
        // 括号里的东西不是时长(普通括号文本)→ 不当时长,也不吞掉标题
        ck(dl_parse_track_line("1. \xE6\x9F\x90\xE6\x9B\xB2 (Remix) - X (03:00)", t) &&
           t.title == "\xE6\x9F\x90\xE6\x9B\xB2 (Remix)" && t.durationMs == 180000,
           "标题里的括号不被误当时长", t.title);
        // 垃圾行
        ck(!dl_parse_track_line("", t), "空行 → false");
        ck(!dl_parse_track_line("   ", t), "纯空白 → false");
        // 实体在曲目行里也要反转义(同形态构造:站点把 & 写成 &amp;)
        ck(dl_parse_track_line("1. Rock &amp; Roll - A&amp;B (01:00)", t) &&
           t.title == "Rock & Roll" && t.artist == "A&B",
           "曲目行内实体反转义(→ MetaBase 不落脏数据)", t.title + "|" + t.artist);
    }

    // ---------------- 7) 关键词阶梯(后缀放宽) ----------------
    std::printf("-- pv_dl_query_ladder --\n");
    {
        const std::vector<std::string> l1 = pv_dl_query_ladder("Khrisma Pixel Library");
        ck(l1.size() == 3, "4 词查询 → 3 个变体(全串 + 末2 + 末1,末3 已含于全串去重)",
           std::to_string(l1.size()));
        ck(!l1.empty() && l1[0] == "Khrisma Pixel Library", "第一个变体 = 全串(原样)");
        ck(l1.size() >= 2 && l1[1] == "Pixel Library",
           "第二个变体 = 末 2 词(实测该关键词在 dizzylab 命中 2 Pixels)",
           l1.size() >= 2 ? l1[1] : "");
        ck(l1.size() >= 3 && l1[2] == "Library", "第三个变体 = 末 1 词",
           l1.size() >= 3 ? l1[2] : "");

        const std::vector<std::string> l2 = pv_dl_query_ladder("Pixel Library");
        ck(l2.size() == 2 && l2[0] == "Pixel Library" && l2[1] == "Library",
           "2 词查询:全串 + 末 1 词(实测全串即命中,第二个变体不会被发到)",
           std::to_string(l2.size()));

        ck(pv_dl_query_ladder("x").empty(), "过短查询(1 字节)不做请求");
        ck(pv_dl_query_ladder("   ").empty(), "纯空白 → 空阶梯");

        // 全角空格也当分隔(用户标签里出现过)
        const std::vector<std::string> l3 =
            pv_dl_query_ladder("Pick\xE3\x80\x80me!");
        ck(l3.size() >= 2 && l3[1] == "me!",
           "全角空格作分隔 → 末词 \"me!\"", l3.size() >= 2 ? l3[1] : "");

        // 去重:同一变体不重复发
        const std::vector<std::string> l4 = pv_dl_query_ladder("ABC");
        ck(l4.size() == 1, "单词查询变体唯一去重", std::to_string(l4.size()));
    }

    // ---------------- 8) 沾边筛选(轨名 vs 查询串) ----------------
    std::printf("-- 轨道相关性筛 --\n");
    {
        const std::string q = match_norm("Pixel Library");
        ck(dl_text_hit(q, "Pixel Library"), "规范化后完全相等 → 命中");
        ck(dl_text_hit(q, "Pixel Library (Extended)"), "候选含查询 → 命中");
        ck(dl_text_hit(match_norm("Pixel"), "Pixel Library"), "查询是候选子串 → 命中");
        ck(!dl_text_hit(q, "\xE5\xAE\x8C\xE5\x85\xA8\xE7\x84\xA1\xE5\x85\xB3"),
           "完全不沾 → 不命中(不往里灌噪声)");
        ck(!dl_text_hit("", "x") && !dl_text_hit(q, ""), "任一侧空 → 不命中");
        // 全半角/大小写/标点由 match_norm 折叠:同人曲名常见差异不该漏筛
        ck(dl_text_hit(match_norm("pick me!"), "Pick Me!"), "大小写/标点差异不影响命中");
        // 全角左括号 U+FF08 = EF BC 88,右 U+FF09 = EF BC 89
        ck(dl_text_hit(match_norm("\xEF\xBC\x88Live\xEF\xBC\x89x"), "(Live)x"),
           "全角括号折叠(match_norm 的 U+FF01..FF5E → ASCII)");
    }

    // ---------------- 9) 注册表 + canLyric=false + 采纳门槛不变 ----------------
    std::printf("-- 注册表 / 采纳门槛 --\n");
    {
        const std::vector<ProviderDesc>& reg = provider_registry();
        ck(reg.size() == 5, "注册表 5 项(新增 dizzylab)", std::to_string(reg.size()));
        ck(std::string(reg[0].id) == "dizzylab", "dizzylab 排第一(同人第一优先)",
           std::string(reg[0].id));
        ck(reg[0].canSearch && !reg[0].canLyric,
           "dizzylab:canSearch=true、canLyric=false(无歌词接口,如实不假装)");
        ck(reg[0].search != nullptr && reg[0].lyric == nullptr,
           "dizzylab:search 非空、lyric 为空(canLyric=false 的契约)");
        ck(std::string(reg[0].name) == "dizzylab", "显示名 = dizzylab", std::string(reg[0].name));

        // 歌词链顺序不受影响:逆序 + canLyric 过滤仍是 lrclib→netease→qq
        std::vector<std::string> lyrOrder;
        std::string join;
        for (auto it = reg.rbegin(); it != reg.rend(); ++it)
            if (it->canLyric) { lyrOrder.push_back(it->id); join += std::string(it->id) + " "; }
        ck(lyrOrder == std::vector<std::string>{ "lrclib", "netease", "qq" },
           "歌词链仍是 lrclib→netease→qq(dizzylab 无词,自动跳过)", join);

        // 采纳门槛原封不动(红线):dizzylab 排第一不许放宽门槛
        ck(!enrich_detail::enrich_adopt(false, -1, true, 0.4875),
           "门槛不变:非confident + LLM=-1 + 0.4875 → 不采纳(#1 实测分数形态)");
        ck(!enrich_detail::enrich_adopt(false, -1, true, 0.79),
           "门槛不变:0.79 < 0.80 → 不采纳");
        ck(enrich_detail::enrich_adopt(false, -1, true, 0.80),
           "门槛不变:0.80 → 采纳(与 kEnrichAdoptTop 同值)");
        ck(enrich_detail::enrich_adopt(false, 0, true, 0.10),
           "门槛不变:LLM 明确点名(pick=0)即采纳");
        ck(!enrich_detail::enrich_adopt(false, -9, false, 0.10),
           "门槛不变:LLM 未运行 + 低分 → 不采纳");
        ck(enrich_detail::kEnrichAdoptTop == 0.80, "kEnrichAdoptTop 仍是 0.80");
    }

    // ---------------- 10) 熔断/限频门(纯逻辑,不发请求) ----------------
    std::printf("-- dl_gate 熔断 --\n");
    {
        // 只在「冷却中」这条不发网络的分支上做断言:acquire 返回 false + err 非空。
        {
            std::lock_guard<std::mutex> lk(dl_gate_mtx());
            dl_gate().coolUntil = std::chrono::steady_clock::now() + std::chrono::seconds(60);
            dl_gate().fails = 0;
        }
        std::string err = "sentinel";
        ck(!dl_gate_acquire(err), "冷却期内 acquire 返回 false(不发请求)");
        ck(err.find("熔断") != std::string::npos, "err 写明熔断与剩余秒数", err);
        // 复原(避免影响后续真网 opt-in 用例)
        {
            std::lock_guard<std::mutex> lk(dl_gate_mtx());
            dl_gate().coolUntil = std::chrono::steady_clock::time_point{};
            dl_gate().fails = 0;
        }
        ck(kDizzylabMinIntervalMs >= 1000, "限频 ≥ 1 req/s(礼节下限)");
        ck(kDizzylabFailLimit >= 2 && kDizzylabCooldownSec >= 30,
           "熔断阈值/冷却窗合理(≥2 次失败、≥30s)");
    }

    // ---------------- 10b) E-18：连续失败熔断与恢复（可注入自动化测试） ----------------
    std::printf("-- dl_gate 连续失败熔断与恢复（可注入,E-18）--\n");
    {
        // 1. 设置模拟时钟与计数器
        auto mockNow = std::chrono::steady_clock::now();
        set_dizzylab_clock_for_test([&]() { return mockNow; });

        int transportCalls = 0;
        int statusToReturn = 500;
        std::string bodyToReturn = "";
        std::string lastRequestedUrl = "";

        set_dizzylab_transport_for_test([&](const std::string& url,
                                            const std::vector<std::string>&,
                                            int) -> rta::net::HttpResponse {
            ++transportCalls;
            lastRequestedUrl = url;
            rta::net::HttpResponse r;
            r.status = statusToReturn;
            if (statusToReturn == 0) {
                r.error = "Connection refused";
            } else if (statusToReturn == 200) {
                r.body.assign(bodyToReturn.begin(), bodyToReturn.end());
            }
            return r;
        });

        dl_gate_reset_for_test();
        ck(dl_gate().fails == 0, "初始失败计数为 0");
        ck(dl_gate().coolUntil.time_since_epoch().count() == 0, "初始不在冷却期");

        // 2. 连续第 1 次失败：不进入冷却
        {
            statusToReturn = 500;
            std::string err;
            const std::string res = dl_http_get("https://www.dizzylab.net/test1", err);
            ck(res.empty() && err == "HTTP 500", "第 1 次失败: dl_http_get 返回 HTTP 500");
            ck(transportCalls == 1, "第 1 次失败: 向 transport 发出了请求");
            ck(dl_gate().fails == 1, "第 1 次失败: fails 增加为 1");
            ck(dl_gate().coolUntil.time_since_epoch().count() == 0, "第 1 次失败: 未开冷却");
        }

        // 3. 连续第 2 次失败：仍不进入冷却
        {
            mockNow += std::chrono::milliseconds(1100);
            statusToReturn = 0; // 网络层报错
            std::string err;
            const std::string res = dl_http_get("https://www.dizzylab.net/test2", err);
            ck(res.empty() && err.find("请求失败") != std::string::npos, "第 2 次失败: dl_http_get 返回网络错误");
            ck(transportCalls == 2, "第 2 次失败: 向 transport 发出了请求");
            ck(dl_gate().fails == 2, "第 2 次失败: fails 增加为 2");
            ck(dl_gate().coolUntil.time_since_epoch().count() == 0, "第 2 次失败: 仍未开冷却");
            std::string acqErr;
            ck(dl_gate_acquire(acqErr), "连续 2 次失败后: dl_gate_acquire 仍可获得许可（不冷却）");
        }

        // 4. 连续第 3 次失败：立刻进入冷却，开启 120s 冷却窗
        {
            mockNow += std::chrono::milliseconds(1100);
            statusToReturn = 502; // 网关错误
            std::string err;
            const std::string res = dl_http_get("https://www.dizzylab.net/test3", err);
            ck(res.empty() && err == "HTTP 502", "第 3 次失败: dl_http_get 返回 HTTP 502");
            ck(transportCalls == 3, "第 3 次失败: 向 transport 发出了请求");
            ck(dl_gate().fails == 0, "第 3 次失败开冷却: fails 计数清零");
            const auto expectedCool = mockNow + std::chrono::seconds(kDizzylabCooldownSec);
            ck(dl_gate().coolUntil == expectedCool, "第 3 次失败开冷却: 冷却截止时间正确设定 (+120s)");
        }

        // 5. 冷却期间：严禁发起任何请求，直接返回熔断错误
        {
            const int callsBefore = transportCalls;
            mockNow += std::chrono::seconds(10); // 冷却才过去 10 秒，还剩 110 秒
            std::string err;
            const std::string res = dl_http_get("https://www.dizzylab.net/test4", err);
            ck(res.empty(), "冷却期内: dl_http_get 返回空");
            ck(err.find("连续失败熔断中") != std::string::npos && err.find("不发起请求") != std::string::npos,
               "冷却期内: err 明确提示熔断并给出剩余秒数", err);
            ck(transportCalls == callsBefore, "冷却期内: 严禁向 transport 发起请求（调用次数未增加）");

            // search_dizzylab 端到端同样被拦截
            std::string searchErr;
            const auto cands = search_dizzylab("Pixel Library", searchErr);
            ck(cands.empty(), "冷却期内: search_dizzylab 优雅返回空候选");
            ck(searchErr.find("连续失败熔断中") != std::string::npos, "冷却期内: search_dizzylab 给出熔断错误", searchErr);
            ck(transportCalls == callsBefore, "search_dizzylab 在冷却期内同样 0 请求");
        }

        // 6. 冷却时间到达（120s 过去）：自动恢复发请求
        {
            mockNow += std::chrono::seconds(115); // 总共过去 125s > 120s
            statusToReturn = 200;
            bodyToReturn = "<html><head><title>ok</title></head><body><h1>ok</h1></body></html>";
            const int callsBefore = transportCalls;
            std::string err;
            const std::string res = dl_http_get("https://www.dizzylab.net/test5", err);
            ck(!res.empty() && err.empty(), "冷却结束后: 请求恢复成功");
            ck(transportCalls == callsBefore + 1, "冷却结束后: 实际向 transport 发出了新请求");
            ck(dl_gate().fails == 0, "恢复请求成功后: fails 为 0");
        }

        // 7. 成功会重置失败计数：即使此前有 1 次或 2 次失败
        {
            mockNow += std::chrono::milliseconds(1100);
            statusToReturn = 500;
            std::string err;
            dl_http_get("https://www.dizzylab.net/test_fail1", err);
            ck(dl_gate().fails == 1, "失败记账: 1 次失败 → fails = 1");

            mockNow += std::chrono::milliseconds(1100);
            dl_http_get("https://www.dizzylab.net/test_fail2", err);
            ck(dl_gate().fails == 2, "连续第 2 次失败 → fails = 2");

            // 中途来一次成功
            mockNow += std::chrono::milliseconds(1100);
            statusToReturn = 200;
            bodyToReturn = "<html>ok</html>";
            const std::string okRes = dl_http_get("https://www.dizzylab.net/test_ok", err);
            ck(!okRes.empty(), "中途发生一次成功请求");
            ck(dl_gate().fails == 0, "成功重置失败计数: fails 重新归零");

            // 再次失败：从 1 开始记，而不是从 3 触发熔断
            mockNow += std::chrono::milliseconds(1100);
            statusToReturn = 500;
            dl_http_get("https://www.dizzylab.net/test_fail3", err);
            ck(dl_gate().fails == 1, "重置后再次失败: fails = 1（需要再连续 2 次才会熔断）");
            ck(dl_gate().coolUntil.time_since_epoch().count() == 0 || dl_gate().coolUntil <= mockNow,
               "重置后 1 次失败绝不触发熔断");
        }

        // 8. 清理钩子与重置状态，保证不影响后续测试与生产行为
        set_dizzylab_transport_for_test(nullptr);
        set_dizzylab_clock_for_test(nullptr);
        dl_gate_reset_for_test();
        ck(dl_transport_hook() == nullptr, "transport 钩子已还原为 nullptr（默认行为不变）");
        ck(dl_clock_hook() == nullptr, "clock 钩子已还原为 nullptr");
        ck(dl_gate().fails == 0, "gate 状态已完全重置");
    }

    // ---------------- 11) 真网 opt-in(RTA_NET_TESTS=1) ----------------
    // 默认不联网:ctest 必须在断网机器上全绿。联网时也只打 3 个请求(搜索 1 + 专辑 2),
    // 间隔由 dl_gate 保证 ≥1s —— 不并发、不轰炸。
    const char* netEnv = std::getenv("RTA_NET_TESTS");
    const bool netOn = netEnv && std::string(netEnv) == "1";
    std::printf("-- 真网(opt-in,RTA_NET_TESTS=1;当前%s)--\n", netOn ? "开启" : "关闭");
    if (!netOn) {
        std::printf("  [SKIP] 未设 RTA_NET_TESTS=1,跳过联网用例(默认离线)\n");
    } else {
        std::string err;
        // 实测命中样例:GKSL-003 的曲名 + 艺术家(整串在 dizzylab 零命中 → 阶梯必须放宽)
        const std::vector<Candidate> out = search_dizzylab("Khrisma Pixel Library", err);
        ck(err.empty(), "真网搜索:err 空", err);
        ck(!out.empty(), "真网搜索:Khrisma Pixel Library 经阶梯命中(实测期望 ≥1 条)",
           std::to_string(out.size()));
        if (!out.empty()) {
            ck_eq(out[0].title, "Pixel Library", "真网:首条曲名 = Pixel Library");
            ck_eq(out[0].artist, "Khrisma", "真网:首条艺术家 = Khrisma");
            ck_eq(out[0].album, "2 Pixels", "真网:专辑 = 2 Pixels");
            ck(out[0].durationMs > 0, "真网:时长非 0(match 的时长信号可用)",
               std::to_string(out[0].durationMs));
            ck(!out[0].coverUrl.empty(), "真网:coverUrl 非空", out[0].coverUrl);
            std::printf("      └ 首条: %s | %s | %s | %dms\n", out[0].title.c_str(),
                        out[0].artist.c_str(), out[0].album.c_str(), (int)out[0].durationMs);
        }
        // 零结果必须是「空 + err 空」,不是错误(上层要继续试其它源)
        std::string e2;
        const std::vector<Candidate> none = search_dizzylab("zzzzqqqq不存在的作品名", e2);
        ck(none.empty() && e2.empty(), "真网:零结果 = 空候选且无错误", e2);
    }

    std::printf("\n%s\n", g_fail == 0 ? "全部通过" : (std::to_string(g_fail) + " 项失败").c_str());
    return g_fail == 0 ? 0 : 1;
}
