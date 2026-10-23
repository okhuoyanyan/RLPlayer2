// test_metaproviders.cpp — 四源 provider(src/meta/provider.h):全部离线,不发任何网络请求
//
// 判据说明(铁律 3):
//   * 只测「构造」与「解析」两组纯函数链路,用 2026-10-21 node 探活(build/synchk/probe_providers.js)
//     看到的真实响应结构写死报文;search_qq/search_netease/search_itunes/lyric_* 网络函数的
//     真实行为由该次探活背书,本测试不做联网验证。
//   * QQ 歌词端到端已验证(2026-10-21 晚,build/synchk/probe_qq_lyric.js:歌曲级 mid
//     匿名取到完整 base64 LRC);这里仍覆盖 parse_qq_lyric 的解析逻辑(base64 → LRC),
//     网络函数本体不做联网测试(网络行为由探活背书)。
//   * body/URL 断言用整串精确匹配——编码回归(大小写 hex / 字段顺序 / 编码集)也要被抓到。
//   * 双重编码守卫:http_client 的 percent_encode_path 只编 <=0x20 / >=0x7F 与少数特殊字符、
//     不再编 '%',因此 percent_enc_utf8 先编码后拼串不会二次编码——该性质有专测,
//     这是 spec 点名的坑(「已编码的 % 会被再编成 %25」的规避依据)。
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "meta/provider.h"
#include "meta/enrich.h"         // EnrichOpts(map 形态)的注册表配套断言(P1.5)
#include <cstdio>
#include <string>
#include <vector>

using namespace rta::meta;

static int g_fail = 0;
static void ck(bool cond, const char* what, const std::string& detail = "") {
    if (cond) std::printf("  [PASS] %s\n", what);
    else { std::printf("  [FAIL] %s  %s\n", what, detail.c_str()); ++g_fail; }
}

int main() {
    SetConsoleOutputCP(CP_UTF8);
    std::printf("== meta/provider.h 四源 provider(离线)==\n");

    // ---------------- 1) percent 编码 / JSON 转义 / 年份 / base64 ----------------
    std::printf("-- percent_enc_utf8 --\n");
    {
        // 期望值与 2026-10-21 node 探活的 encodeURIComponent 输出逐字节一致:
        //   encodeURIComponent('周杰伦 晴天') = %E5%91%A8%E6%9D%B0%E4%BC%A6%20%E6%99%B4%E5%A4%A9
        ck(percent_enc_utf8("周杰伦 晴天") ==
               "%E5%91%A8%E6%9D%B0%E4%BC%A6%20%E6%99%B4%E5%A4%A9",
           "中文+空格编码正确(与 node encodeURIComponent 一致)",
           percent_enc_utf8("周杰伦 晴天"));
        ck(percent_enc_utf8("叶惠美") == "%E5%8F%B6%E6%83%A0%E7%BE%8E",
           "「叶惠美」编码正确", percent_enc_utf8("叶惠美"));
        ck(percent_enc_utf8("") == "", "空串 → 空串");
        ck(percent_enc_utf8("A-z_09.~") == "A-z_09.~", "unreserved 字符原样保留",
           percent_enc_utf8("A-z_09.~"));
        ck(percent_enc_utf8("&=?#+%") == "%26%3D%3F%23%2B%25",
           "保留字(& = ? # + %)全量编码", percent_enc_utf8("&=?#+%"));

        // 双重编码守卫:已编码串再过 http_client 的 percent_encode_path 必须不变
        const std::string enc = percent_enc_utf8("周杰伦 晴天?&=#中 文");
        ck(rta::net::percent_encode_path(enc) == enc,
           "已编码串过 percent_encode_path 不变(无双重编码)", enc);

        // 构造出的查询 URL 是 ASCII 安全的(http_client 可直接用)
        const std::string u = itunes_build_search_url("周杰伦 晴天");
        bool asciiSafe = true;
        for (unsigned char c : u) if (c <= 0x20 || c >= 0x7F) asciiSafe = false;
        ck(asciiSafe, "itunes_build_search_url 输出 ASCII 安全", u);
    }

    std::printf("-- pv_jesc / pv_year4 --\n");
    {
        ck(pv_jesc("a\"b\\c\nd\te\x01中") == "a\\\"b\\\\c\\nd\\te\\u0001中",
           "pv_jesc 引号/反斜杠/换行/控制字符/中文原样", pv_jesc("a\"b\\c\nd\te\x01中"));
        const rta::net::Json j = rta::net::json_parse("{\"v\":\"" + pv_jesc("回\"环\\两\n字") + "\"}");
        ck(j.type == rta::net::Json::Obj && j["v"].str == "回\"环\\两\n字",
           "pv_jesc 输出可被 net/json.h 无损还原", j["v"].str);

        ck(pv_year4("2003-07-31") == "2003", "QQ time_public 形态", pv_year4("2003-07-31"));
        ck(pv_year4("2003-07-31T07:00:00Z") == "2003", "iTunes releaseDate ISO 形态",
           pv_year4("2003-07-31T07:00:00Z"));
        ck(pv_year4("我 1998 年") == "1998", "混排文本取第一段 ≥4 位数字", pv_year4("我 1998 年"));
        ck(pv_year4("") == "" && pv_year4("abc") == "" && pv_year4("12-34") == "",
           "无 4 位数字段 → 空串", pv_year4("12-34"));
        // 合理窗口 [1000,2100](2026-10-22 QQ "0000" 写进 DATE 的事故守卫,provider.h pv_year4)
        ck(pv_year4("0000") == "", "窗口守卫:\"0000\" → 空串(QQ 实测事故形态)",
           pv_year4("0000"));
        ck(pv_year4("0999") == "", "窗口守卫:\"0999\"(<1000)→ 空串", pv_year4("0999"));
        ck(pv_year4("2023") == "2023", "窗口内:\"2023\" 原样返回", pv_year4("2023"));
        ck(pv_year4("2101") == "", "窗口守卫:\"2101\"(>2100)→ 空串", pv_year4("2101"));
        ck(pv_year4("1000") == "1000" && pv_year4("2100") == "2100",
           "窗口边界 1000/2100 含端点", pv_year4("1000") + "|" + pv_year4("2100"));
        ck(pv_year4("未出版") == "", "无数字文本 → 空串", pv_year4("未出版"));
        ck(pv_year4("0000-00-00") == "", "QQ \"0000-00-00\" 全形态 → 空串", pv_year4("0000-00-00"));

        // E-14 边界覆盖
        ck(pv_year4("1999") == "1999", "E-14 覆盖：'1999' → '1999'", pv_year4("1999"));
        ck(pv_year4("1999-05-01") == "1999", "E-14 覆盖：'1999-05-01' → '1999'", pv_year4("1999-05-01"));
        ck(pv_year4("0") == "", "E-14 脏值：'0' → 空串", pv_year4("0"));
        ck(pv_year4("9999") == "", "E-14 脏值：'9999' → 空串", pv_year4("9999"));
        ck(pv_year4("-1") == "", "E-14 脏值：'-1' → 空串", pv_year4("-1"));
    }

    std::printf("-- pv_b64_decode --\n");
    {
        // RFC 4648 标准向量 + padding/非法字符边界
        struct BV { const char* in; const char* want; };
        const BV vs[] = {
            {"", ""}, {"Zg==", "f"}, {"Zm8=", "fo"}, {"Zm9v", "foo"},
            {"Zm9vYg==", "foob"}, {"Zm9vYmE=", "fooba"}, {"Zm9vYmFy", "foobar"},
        };
        for (const BV& v : vs) {
            const std::string got = pv_b64_decode(v.in);
            ck(got == v.want, (std::string("b64(\"") + v.in + "\") = \"" + v.want + "\"").c_str(), got);
        }
        ck(pv_b64_decode("====") == "", "只有 padding → 空串", pv_b64_decode("===="));
        ck(pv_b64_decode("Zg") == "f", "无 padding 宽松解码", pv_b64_decode("Zg"));
        ck(pv_b64_decode("Zm9v\r\nYg==") == "foob", "载荷内换行(非法字符)跳过",
           pv_b64_decode("Zm9v\r\nYg=="));
        ck(pv_b64_decode("Zm!9v") == "foo", "非法字符宽松跳过", pv_b64_decode("Zm!9v"));
        // 探活实测向量:QQ 歌词同款「base64 的 UTF-8 LRC」
        ck(pv_b64_decode("WzAwOjEyLjM0XeaZtOWkqSAtIOWRqOadsOS8pg==") == "[00:12.34]晴天 - 周杰伦",
           "QQ 歌词同款中文 base64 向量",
           pv_b64_decode("WzAwOjEyLjM0XeaZtOWkqSAtIOWRqOadsOS8pg=="));
    }

    // ---------------- 2) 搜索 body / URL 构造(整串精确匹配) ----------------
    std::printf("-- 信封/body 构造精确匹配 --\n");
    {
        const std::string qq = qq_build_search_body("晴天");
        ck(qq == "{\"req_1\":{\"method\":\"DoSearchForQQMusicDesktop\","
                 "\"module\":\"music.search.SearchCgiService\","
                 "\"param\":{\"search_type\":0,\"query\":\"晴天\","
                 "\"page_num\":1,\"num_per_page\":8}}}",
           "qq_build_search_body 精确匹配(无顶层 comm)", qq);

        const std::string qqEsc = qq_build_search_body("a\"b\\c");
        ck(qqEsc.find("\"query\":\"a\\\"b\\\\c\"") != std::string::npos,
           "query 内引号/反斜杠经 pv_jesc 转义", qqEsc);

        const std::string ne = netease_build_search_body("周杰伦 晴天");
        ck(ne == "s=%E5%91%A8%E6%9D%B0%E4%BC%A6%20%E6%99%B4%E5%A4%A9&type=1&limit=8&offset=0",
           "netease_build_search_body 精确匹配", ne);

        const std::string it = itunes_build_search_url("周杰伦 晴天");
        ck(it == "https://itunes.apple.com/search?term=%E5%91%A8%E6%9D%B0%E4%BC%A6%20%E6%99%B4%E5%A4%A9"
                 "&media=music&entity=song&country=TW&limit=8",
           "itunes_build_search_url 精确匹配", it);

        const std::string lg = lrclib_build_get_url("晴天", "周杰伦", "叶惠美", 269);
        ck(lg == "https://lrclib.net/api/get?track_name=%E6%99%B4%E5%A4%A9"
                 "&artist_name=%E5%91%A8%E6%9D%B0%E4%BC%A6&album_name=%E5%8F%B6%E6%83%A0%E7%BE%8E"
                 "&duration=269",
           "lrclib_build_get_url 精确匹配", lg);
        const std::string ls = lrclib_build_search_url("晴天", "周杰伦");
        ck(ls == "https://lrclib.net/api/search?track_name=%E6%99%B4%E5%A4%A9"
                 "&artist_name=%E5%91%A8%E6%9D%B0%E4%BC%A6",
           "lrclib_build_search_url 精确匹配", ls);
        ck(lrclib_build_get_url("a", "b", "", -3) ==
               "https://lrclib.net/api/get?track_name=a&artist_name=b&album_name=&duration=0",
           "lrclib 空专辑/负时长边界(钳到 0)", lrclib_build_get_url("a", "b", "", -3));

        ck(netease_build_lyric_url(186016) ==
               "https://music.163.com/api/song/lyric?id=186016&lv=-1&kv=-1&tv=-1",
           "netease_build_lyric_url 精确匹配", netease_build_lyric_url(186016));

        const std::string ql = qq_build_lyric_body("0039MnYb0qxYhV");
        ck(ql == "{\"req_1\":{\"method\":\"GetPlayLyricInfo\","
                 "\"module\":\"music.musichallSong.PlayLyricInfo\","
                 "\"param\":{\"songMID\":\"0039MnYb0qxYhV\"}}}",
           "qq_build_lyric_body 精确匹配(param songMID;✅ 端到端已验证 2026-10-21,须传歌曲级 mid)", ql);
    }

    // ---------------- 3) QQ 搜索解析 ----------------
    std::printf("-- parse_qq_search --\n");
    {
        // 报文按 2026-10-21 探活的 req_1.data.body.song.list[] 真实结构写死:
        // 条目 A 全字段(歌曲级 time_public "2003-07-31");条目 B 双歌手 + 歌曲级
        // time_public 缺失回落专辑级。
        const std::string resp =
            "{\"code\":0,\"req_1\":{\"code\":0,\"data\":{\"body\":{\"song\":{\"list\":["
            "{\"title\":\"晴天\","
            "\"singer\":[{\"id\":4558,\"mid\":\"0025NhlN2yWrP4\",\"name\":\"周杰伦\",\"type\":0}],"
            "\"album\":{\"id\":28324,\"mid\":\"000MkMni19ClKG\",\"name\":\"叶惠美\",\"time_public\":\"\"},"
            "\"file\":{\"media_mid\":\"003Qui1q2u1Zho\",\"size_128mp3\":4001234},"
            "\"id\":97773,\"mid\":\"0039MnYb0qxYhV\",\"interval\":269,"
            "\"time_public\":\"2003-07-31\"},"
            "{\"title\":\"晴天 (Live)\","
            "\"singer\":[{\"name\":\"周杰伦\"},{\"name\":\"费玉清\"}],"
            "\"album\":{\"id\":9001,\"mid\":\"004fXSyj3bWTMN\",\"name\":\"浮声旧梦(演唱会)\","
            "\"time_public\":\"2005-06-24\"},"
            "\"file\":{\"media_mid\":\"00abcDEFxyz\"},"
            "\"interval\":214}"
            "]}}}}}";
        std::string err;
        const std::vector<Candidate> out = parse_qq_search(resp, err);
        ck(err.empty() && out.size() == 2, "2 条命中、err 为空",
           "err=" + err + " n=" + std::to_string(out.size()));
        if (out.size() == 2) {
            const Candidate& a = out[0];
            ck(a.source == "qq" && a.title == "晴天" && a.artist == "周杰伦" && a.album == "叶惠美",
               "A: source/title/artist/album 映射", a.title + "|" + a.artist + "|" + a.album);
            ck(a.songId == "0039MnYb0qxYhV" && a.albumId == "000MkMni19ClKG",
               "A: songId=歌曲级 mid(歌词接口只认它,任务 A 修正), albumId=album.mid",
               a.songId + "|" + a.albumId);
            ck(a.year == "2003" && a.durationMs == 269000,
               "A: 年份取歌曲级 time_public 前 4 位;interval 秒→毫秒",
               a.year + "|" + std::to_string(a.durationMs));
            ck(a.coverUrl == "https://y.gtimg.cn/music/photo_new/T002R800x800M000000MkMni19ClKG.jpg",
               "A: 封面 URL 按 T002R800x800M000+album.mid+.jpg 拼接", a.coverUrl);
            ck(a.genre.empty(), "A: genre 未探活确认不盲映射 → 空", a.genre);

            const Candidate& b = out[1];
            ck(b.artist == "周杰伦 / 费玉清", "B: 多歌手 \" / \" 连接", b.artist);
            ck(b.year == "2005", "B: 歌曲级 time_public 缺失回落专辑级", b.year);
            ck(b.songId == "00abcDEFxyz", "B: 无歌曲级 mid → 退 file.media_mid(任务 A 的回退路径)",
               b.songId);
            ck(b.durationMs == 214000 && b.coverUrl ==
                   "https://y.gtimg.cn/music/photo_new/T002R800x800M000004fXSyj3bWTMN.jpg",
               "B: 时长与封面拼接", b.coverUrl);
        }

        std::string e2;
        ck(parse_qq_search("<html>500</html>", e2).empty() && e2.find("JSON") != std::string::npos,
           "非法 JSON → err", e2);
        std::string e3;
        ck(parse_qq_search("{}", e3).empty() && e3.find("list") != std::string::npos,
           "缺 list → err", e3);
        std::string e4;
        ck(parse_qq_search("{\"req_1\":{\"data\":{\"body\":{\"song\":{\"list\":[]}}}}}", e4).empty() &&
           e4.empty(), "list 空 = 合法空结果(err 为空)", e4);

        // 上限 8 条:10 条进 → 8 条出(任务「各返回 ≤8 条」)
        std::string list10 = "{\"req_1\":{\"data\":{\"body\":{\"song\":{\"list\":[";
        for (int i = 0; i < 10; ++i) {
            if (i) list10 += ",";
            list10 += "{\"title\":\"t" + std::to_string(i) + "\",\"singer\":[],\"album\":{},\"file\":{}}";
        }
        list10 += "]}}}}}";
        std::string e5;
        ck(parse_qq_search(list10, e5).size() == 8, "超过 8 条截断到 8", e5);

        // 垃圾年份守卫(2026-10-22 实测事故):QQ 对该曲返回 time_public="0000-00-00"
        // (歌曲级与专辑级双垃圾),candidate.year 必须为空,而不是把 "0000" 交给上层写回。
        const std::string respZero =
            "{\"code\":0,\"req_1\":{\"code\":0,\"data\":{\"body\":{\"song\":{\"list\":["
            "{\"title\":\"无年份曲\",\"singer\":[{\"name\":\"某人\"}],"
            "\"album\":{\"id\":1,\"mid\":\"00albumMidZERO\",\"name\":\"某专辑\","
            "\"time_public\":\"0000-00-00\"},"
            "\"file\":{\"media_mid\":\"00mediaMidZERO\"},"
            "\"id\":2,\"mid\":\"00songMidZERO00\",\"interval\":200,"
            "\"time_public\":\"0000-00-00\"}"
            "]}}}}}";
        std::string errZero;
        const std::vector<Candidate> outZero = parse_qq_search(respZero, errZero);
        ck(errZero.empty() && outZero.size() == 1 && outZero[0].year.empty(),
           "time_public=\"0000-00-00\" → candidate.year 为空(2026-10-22 QQ 事故守卫)",
           "err=" + errZero +
               " year=" + (outZero.size() == 1 ? outZero[0].year : "(条目缺失)"));
    }

    // ---------------- 4) 网易搜索解析 ----------------
    std::printf("-- parse_netease_search --\n");
    {
        // 条目 A 按探活实测:album 没有 picUrl 字段(实证形状);条目 B 双歌手 + picUrl。
        const std::string resp =
            "{\"code\":200,\"result\":{\"songCount\":250,\"songs\":["
            "{\"name\":\"晴天 (原唱 周杰伦)\",\"id\":2668397359,\"duration\":270738,"
            "\"artists\":[{\"id\":1,\"name\":\"RyaVocal\"}],"
            "\"album\":{\"id\":260499608,\"name\":\"晴天\"}},"
            "{\"name\":\"晴天\",\"id\":186016,\"duration\":269133,"
            "\"artists\":[{\"name\":\"周杰伦\"},{\"name\":\"温岚\"}],"
            "\"album\":{\"id\":18875,\"name\":\"叶惠美\","
            "\"picUrl\":\"https://p1.music.126.net/abcdef/18875.jpg\"}}"
            "]}}";
        std::string err;
        const std::vector<Candidate> out = parse_netease_search(resp, err);
        ck(err.empty() && out.size() == 2, "2 条命中、err 为空",
           "err=" + err + " n=" + std::to_string(out.size()));
        if (out.size() == 2) {
            const Candidate& a = out[0];
            ck(a.source == "netease" && a.title == "晴天 (原唱 周杰伦)" &&
               a.artist == "RyaVocal" && a.album == "晴天",
               "A: source/title/artist/album 映射", a.title + "|" + a.artist);
            ck(a.songId == "2668397359" && a.albumId == "260499608",
               "A: 数值 id → 十进制字符串", a.songId + "|" + a.albumId);
            ck(a.durationMs == 270738, "A: duration 已是毫秒直取", std::to_string(a.durationMs));
            ck(a.coverUrl.empty() && a.genre.empty() && a.year.empty(),
               "A: 探活实证 album 无 picUrl → coverUrl 空;genre/year 该接口没有 → 空",
               a.coverUrl + "|" + a.genre + "|" + a.year);

            const Candidate& b = out[1];
            ck(b.artist == "周杰伦 / 温岚", "B: 多歌手 \" / \" 连接", b.artist);
            ck(b.coverUrl == "https://p1.music.126.net/abcdef/18875.jpg",
               "B: album.picUrl 直填 coverUrl", b.coverUrl);
        }

        std::string e2;
        ck(parse_netease_search("not json", e2).empty() && e2.find("JSON") != std::string::npos,
           "非法 JSON → err", e2);
        std::string e3;
        ck(parse_netease_search("{\"result\":{\"songCount\":0}}", e3).empty() &&
           e3.find("songs") != std::string::npos, "缺 result.songs → err", e3);
        std::string e4;
        ck(parse_netease_search("{\"result\":{\"songs\":[]}}", e4).empty() && e4.empty(),
           "songs 空 = 合法空结果", e4);
    }

    // ---------------- 5) iTunes 解析 ----------------
    std::printf("-- parse_itunes_search --\n");
    {
        // 条目 A 按探活实测结果写死(含 artworkUrl100);条目 B 缺 genre/artwork。
        const std::string resp =
            "{\"resultCount\":2,\"results\":["
            "{\"trackName\":\"晴天\",\"artistName\":\"周杰倫\",\"collectionName\":\"葉惠美\","
            "\"primaryGenreName\":\"華語流行樂\",\"releaseDate\":\"2003-07-31T07:00:00Z\","
            "\"trackId\":535824738,\"collectionId\":535824731,\"trackTimeMillis\":269747,"
            "\"artworkUrl100\":\"https://is1-ssl.mzstatic.com/image/thumb/Music115/v4/45/8a/e4/"
            "458ae484-dc8b-5683-ce04-8d2948346462/JAY.jpg/100x100bb.jpg\","
            "\"kind\":\"song\",\"wrapperType\":\"track\"},"
            "{\"trackName\":\"聽媽媽的話\",\"artistName\":\"周杰倫\",\"collectionName\":\"依然范特西\","
            "\"releaseDate\":\"2006-09-05T07:00:00Z\","
            "\"trackId\":536499821,\"collectionId\":536499812,\"trackTimeMillis\":240000}"
            "]}";
        std::string err;
        const std::vector<Candidate> out = parse_itunes_search(resp, err);
        ck(err.empty() && out.size() == 2, "2 条命中、err 为空",
           "err=" + err + " n=" + std::to_string(out.size()));
        if (out.size() == 2) {
            const Candidate& a = out[0];
            ck(a.source == "itunes" && a.title == "晴天" && a.artist == "周杰倫" &&
               a.album == "葉惠美", "A: source/title/artist/album 映射",
               a.title + "|" + a.artist + "|" + a.album);
            ck(a.genre == "華語流行樂" && a.year == "2003",
               "A: primaryGenreName→genre;releaseDate 前 4 位→year", a.genre + "|" + a.year);
            ck(a.songId == "535824738" && a.albumId == "535824731",
               "A: trackId/collectionId → 字符串句柄", a.songId + "|" + a.albumId);
            ck(a.durationMs == 269747, "A: trackTimeMillis → durationMs",
               std::to_string(a.durationMs));
            ck(a.coverUrl ==
                   "https://is1-ssl.mzstatic.com/image/thumb/Music115/v4/45/8a/e4/"
                   "458ae484-dc8b-5683-ce04-8d2948346462/JAY.jpg/1200x1200bb.jpg",
               "A: artworkUrl100 的 100x100bb → 1200x1200bb(smoke:1200 是可用上限)", a.coverUrl);

            const Candidate& b = out[1];
            ck(b.genre.empty() && b.coverUrl.empty(),
               "B: 缺 primaryGenreName/artworkUrl100 → 空", b.genre + "|" + b.coverUrl);
            ck(b.year == "2006", "B: 年份仍可提取", b.year);
        }

        // artwork 无标记 → 原样(不猜其它尺寸)
        const std::string resp2 =
            "{\"results\":[{\"trackName\":\"x\",\"artistName\":\"y\",\"collectionName\":\"z\","
            "\"trackId\":1,\"collectionId\":2,\"trackTimeMillis\":1000,"
            "\"artworkUrl100\":\"https://example.com/img.jpg\"}]}";
        std::string e5;
        const std::vector<Candidate> out2 = parse_itunes_search(resp2, e5);
        ck(out2.size() == 1 && out2[0].coverUrl == "https://example.com/img.jpg",
           "artworkUrl100 无 100x100bb 标记 → 原样保留",
           out2.empty() ? "(no out)" : out2[0].coverUrl);

        std::string e2;
        ck(parse_itunes_search("<html/>", e2).empty() && e2.find("JSON") != std::string::npos,
           "非法 JSON → err", e2);
        std::string e3;
        ck(parse_itunes_search("{\"resultCount\":0}", e3).empty() && e3.find("results") != std::string::npos,
           "缺 results → err", e3);
        std::string e4;
        ck(parse_itunes_search("{\"results\":[]}", e4).empty() && e4.empty(),
           "results 空 = 合法空结果", e4);
    }

    // ---------------- 6) 歌词解析 ----------------
    std::printf("-- parse_netease_lyric --\n");
    {
        // 形状按 2026-10-21 探活 [4]:lrc.lyric 为 LRC 文本,tlyric.lyric 可为空
        const std::string resp =
            "{\"sgc\":false,\"sfy\":false,\"qfy\":false,\"code\":200,"
            "\"lrc\":{\"version\":6,\"lyric\":\"[00:00.000] 作词 : 周杰伦\\n[00:02.250]晴天 - 周杰伦\\n\"},"
            "\"klyric\":{\"version\":0,\"lyric\":\"\"},"
            "\"tlyric\":{\"version\":1,\"lyric\":\"[00:00.000] Lyricist: Jay Chou\\n\"}}";
        const LyricResult r = parse_netease_lyric(resp);
        ck(r.ok && r.source == "netease", "netease 歌词 ok", r.err);
        ck(r.lrc == "[00:00.000] 作词 : 周杰伦\n[00:02.250]晴天 - 周杰伦\n",
           "lrc.lyric 原样提取(\\n 还原)", r.lrc);
        ck(r.translation == "[00:00.000] Lyricist: Jay Chou\n",
           "tlyric.lyric → translation", r.translation);

        // 探活同款:该曲目 tlyric 存在但 lyric 为空串 → translation 空、主体仍 ok
        const LyricResult r2 = parse_netease_lyric(
            "{\"code\":200,\"lrc\":{\"lyric\":\"[00:01]A\"},\"tlyric\":{\"lyric\":\"\"}}");
        ck(r2.ok && r2.lrc == "[00:01]A" && r2.translation.empty(),
           "tlyric 空 → translation 空、ok 不变", r2.translation + "|" + r2.err);
        // 完全没有 tlyric 键
        const LyricResult r3 = parse_netease_lyric("{\"code\":200,\"lrc\":{\"lyric\":\"[00:01]A\"}}");
        ck(r3.ok && r3.translation.empty(), "缺 tlyric 键 → translation 空", r3.err);

        const LyricResult re = parse_netease_lyric("{\"code\":200}");
        ck(!re.ok && re.err.find("lrc.lyric") != std::string::npos, "缺 lrc.lyric → err", re.err);
        ck(!parse_netease_lyric("<html>").ok, "非法 JSON → err");
    }

    std::printf("-- lrclib synced 优先 / plain 回退 --\n");
    {
        // 形状按探活 [6]/[7]:键 id/trackName/.../plainLyrics/syncedLyrics
        const std::string synced = "[00:29.36] 故事的小黃花\n[00:32.77] 從出生那年就飄著\n";
        const LyricResult r = parse_lrclib_get(
            "{\"id\":19682758,\"trackName\":\"晴天\",\"artistName\":\"周杰伦\","
            "\"albumName\":\"葉惠美\",\"duration\":269,\"instrumental\":false,"
            "\"plainLyrics\":\"故事的小黃花\\n\",\"syncedLyrics\":\"" + pv_jesc(synced) + "\"}");
        ck(r.ok && r.source == "lrclib" && r.lrc == synced,
           "get:syncedLyrics 优先", r.lrc);
        ck(r.translation.empty(), "lrclib 无翻译字段 → translation 空", r.translation);

        const LyricResult rp = parse_lrclib_get(
            "{\"duration\":200,\"instrumental\":false,\"plainLyrics\":\"第一行\\n第二行\","
            "\"syncedLyrics\":\"\"}");
        ck(rp.ok && rp.lrc == "第一行\n第二行", "get:synced 空 → plainLyrics 回退", rp.lrc);

        const LyricResult rn = parse_lrclib_get(
            "{\"duration\":200,\"plainLyrics\":\"\",\"syncedLyrics\":\"\"}");
        ck(!rn.ok && !rn.err.empty(), "get:两者皆空 → ok=false 带 err", rn.err);
        ck(!parse_lrclib_get("gateway timeout").ok, "get:非法 JSON → err");

        // search 取第一条:第一条只有 plain、第二条有 synced —— 必须取第一条的 plain
        const LyricResult rs = parse_lrclib_search_first(
            "[{\"albumName\":\"A\",\"syncedLyrics\":\"\",\"plainLyrics\":\"第一条无synced\"},"
            "{\"albumName\":\"B\",\"syncedLyrics\":\"[00:01]第二条\"}]");
        ck(rs.ok && rs.lrc == "第一条无synced", "search:取第一条(synced 空 → plain 回退)", rs.lrc);

        const LyricResult rs2 = parse_lrclib_search_first(
            "[{\"syncedLyrics\":\"[00:01]x\",\"plainLyrics\":\"y\"}]");
        ck(rs2.ok && rs2.lrc == "[00:01]x", "search:第一条有 synced 直接用", rs2.lrc);

        const LyricResult re1 = parse_lrclib_search_first("[]");
        ck(!re1.ok && re1.err.find("无结果") != std::string::npos, "search:空数组 → err", re1.err);
        const LyricResult re2 = parse_lrclib_search_first("{\"a\":1}");
        ck(!re2.ok && re2.err.find("数组") != std::string::npos, "search:非数组 → err", re2.err);
    }

    std::printf("-- parse_qq_lyric(base64 LRC) --\n");
    {
        // ⚠️ QQ 歌词端到端未验证(见 provider.h 头注);以下为解析逻辑测试,报文按探活
        // 看到的 req_1.data 键骨架 + 任务 spec(data.lyric = base64 的 LRC)人工构造。
        const std::string resp =
            "{\"code\":0,\"req_1\":{\"code\":0,\"data\":{\"songID\":97773,\"songName\":\"晴天\","
            "\"lyric\":\"WzAwOjEyLjM0XeaZtOWkqSAtIOWRqOadsOS8pg==\","
            "\"trans\":\"WzAwOjEyLjM0XSBTdW5ueSBEYXk=\",\"qrc\":\"\",\"crypt\":0}}}";
        const LyricResult r = parse_qq_lyric(resp);
        ck(r.ok && r.source == "qq", "qq 歌词 ok", r.err);
        ck(r.lrc == "[00:12.34]晴天 - 周杰伦", "data.lyric base64 → UTF-8 LRC", r.lrc);
        ck(r.translation == "[00:12.34] Sunny Day", "data.trans base64 → 翻译", r.translation);

        const LyricResult r2 = parse_qq_lyric(
            "{\"req_1\":{\"data\":{\"songID\":1,\"lyric\":\"Zm9v\"}}}");
        ck(r2.ok && r2.lrc == "foo" && r2.translation.empty(),
           "缺 trans 键 → translation 空", r2.translation);

        const LyricResult re1 = parse_qq_lyric("{\"req_1\":{\"data\":{\"lyric\":\"\"}}}");
        ck(!re1.ok && re1.err.find("req_1.data.lyric") != std::string::npos,
           "lyric 空 → err 指名字段", re1.err);
        const LyricResult re2 = parse_qq_lyric("{\"req_1\":{\"data\":{\"lyric\":\"!!!\"}}}");
        ck(!re2.ok && re2.err.find("base64") != std::string::npos,
           "全非法字符解码为空 → err", re2.err);
        ck(!parse_qq_lyric("oops").ok, "非法 JSON → err");
    }

    // ---------------- 7) 注册表(P1.5:ProviderDesc / provider_registry / 歌词统一入口) ----------------
    std::printf("-- provider_registry --\n");
    {
        // ⚠️ 2026-10-22:注册表从 4 项扩到 5 项(dizzylab 插到最前,同人曲目第一优先)。
        // dizzylab 的全部用例在 tests/test_dizzylab.cpp;这里只守住「注册表契约」本身:
        // 顺序、can* 标志与函数指针的一致性 —— 加源最容易漏的就是这条。
        const std::vector<ProviderDesc>& reg = provider_registry();
        ck(reg.size() == 5, "注册表 5 项(四源 + dizzylab)", std::to_string(reg.size()));
        if (reg.size() == 5) {
            ck(std::string(reg[0].id) == "dizzylab" && std::string(reg[1].id) == "qq" &&
               std::string(reg[2].id) == "netease" && std::string(reg[3].id) == "itunes" &&
               std::string(reg[4].id) == "lrclib",
               "顺序 = dizzylab/qq/netease/itunes/lrclib(dizzylab 同人第一优先)",
               std::string(reg[0].id) + "/" + reg[1].id + "/" + reg[2].id + "/" +
                   reg[3].id + "/" + reg[4].id);
            ck(std::string(reg[0].name) == "dizzylab" && std::string(reg[1].name) == "QQ音乐" &&
               std::string(reg[2].name) == "网易云" && std::string(reg[3].name) == "iTunes" &&
               std::string(reg[4].name) == "lrclib 歌词",
               "显示名(设置页用)", std::string(reg[0].name) + "/" + reg[1].name + "/" +
                                   reg[2].name + "/" + reg[3].name + "/" + reg[4].name);
            ck(reg[0].canSearch && !reg[0].canLyric, "dizzylab: 仅搜索(无歌词接口,如实)");
            ck(reg[1].canSearch && reg[1].canLyric, "qq: 搜索+歌词");
            ck(reg[2].canSearch && reg[2].canLyric, "netease: 搜索+歌词");
            ck(reg[3].canSearch && !reg[3].canLyric, "itunes: 仅搜索");
            ck(!reg[4].canSearch && reg[4].canLyric, "lrclib: 仅歌词");
            ck(reg[0].search != nullptr && reg[1].search != nullptr &&
               reg[2].search != nullptr && reg[3].search != nullptr,
               "canSearch 源 search 指针非空");
            ck(reg[4].search == nullptr, "lrclib 无 search(canSearch=false → 空)");
            ck(reg[0].lyric == nullptr, "dizzylab 无 lyric(canLyric=false → 空)");
            ck(reg[1].lyric != nullptr && reg[2].lyric != nullptr && reg[4].lyric != nullptr,
               "canLyric 源 lyric 指针非空");
            ck(reg[3].lyric == nullptr, "itunes 无 lyric(canLyric=false → 空)");
        }

        // 歌词链顺序 = 注册表逆序 + canLyric 过滤 → lrclib → netease → qq(P1 固定顺序不变)
        std::vector<std::string> lyrOrder;
        std::string lyrJoin;
        for (auto it = reg.rbegin(); it != reg.rend(); ++it)
            if (it->canLyric) { lyrOrder.push_back(it->id); lyrJoin += it->id; }
        ck(lyrOrder == std::vector<std::string>{"lrclib", "netease", "qq"},
           "歌词链 = 注册表逆序过滤 = lrclib→netease→qq", lyrJoin);

        // 句柄挑选纯函数:第一个「同源且句柄非空」的候选(数组已按分数从高到低)
        std::vector<Candidate> cs(3);
        cs[0].source = "netease"; cs[0].songId = "186016";
        cs[1].source = "qq"; cs[1].songId = "";                    // 同源但无句柄 → 跳过
        cs[2].source = "qq"; cs[2].songId = "0039MnYb0qxYhV";
        ck(pv_lyric_song_id(cs, "qq") == "0039MnYb0qxYhV",
           "pv_lyric_song_id 跳过空句柄取下一个", pv_lyric_song_id(cs, "qq"));
        ck(pv_lyric_song_id(cs, "netease") == "186016",
           "pv_lyric_song_id 命中该源第一个", pv_lyric_song_id(cs, "netease"));
        ck(pv_lyric_song_id(cs, "itunes").empty(), "无该源候选 → 空串");
        ck(pv_lyric_song_id({}, "qq").empty(), "空候选表 → 空串");

        // 歌词统一入口的「无句柄 = 静默不参与」分支(不发网络请求):
        // cands 里不放 qq 候选 → lyric_qq_entry 直接 ok=false、err 空。
        LyricRequest lrq0;
        std::string lerr = "sentinel";
        const LyricResult noqq = lyric_qq_entry(lrq0, lerr);
        ck(!noqq.ok && lerr.empty() && noqq.source == "qq",
           "qq 入口:无句柄 → ok=false、err 空、不发起请求", lerr);
        const LyricResult none = lyric_netease_entry(lrq0, lerr);
        ck(!none.ok && lerr.empty() && none.source == "netease",
           "netease 入口:无句柄 → ok=false、err 空、不发起请求", lerr);
    }

    // ---------------- 8) EnrichOpts(map 形态,P1.5) ----------------
    std::printf("-- EnrichOpts map --\n");
    {
        EnrichOpts def;
        ck(def.enabled("qq") && def.enabled("netease") && def.enabled("itunes") &&
           def.enabled("lrclib") && def.llm,
           "缺省构造:所有源开、llm 开");
        ck(def.enabled("不存在的源"), "map 未列出的源(将来新增)= 缺省开");

        EnrichOpts half = make_enrich_opts(false, true, false, true, false);
        ck(!half.enabled("qq") && half.enabled("netease") && !half.enabled("itunes") &&
           half.enabled("lrclib") && !half.llm,
           "make_enrich_opts(旧五布尔)逐位映射");
        ck(half.srcOn.size() == 4, "make_enrich_opts 显式写满四源", std::to_string(half.srcOn.size()));

        EnrichOpts one;
        one.srcOn["qq"] = false;
        one.llm = false;
        ck(!one.enabled("qq") && one.enabled("netease"), "逐键写法:只关 qq 不影响其它源");
    }

    // ---------------- 9) 可选代理(P1.5:http_client.h 的纯函数部分) ----------------
    std::printf("-- http_proxy_normalize --\n");
    {
        ck(rta::net::http_proxy_normalize("") == "", "空 = 直连(现状)");
        ck(rta::net::http_proxy_normalize("127.0.0.1:7897") == "127.0.0.1:7897",
           "host:port 原样", rta::net::http_proxy_normalize("127.0.0.1:7897"));
        ck(rta::net::http_proxy_normalize("http://127.0.0.1:7897") == "127.0.0.1:7897",
           "剥 http:// scheme", rta::net::http_proxy_normalize("http://127.0.0.1:7897"));
        ck(rta::net::http_proxy_normalize("http://127.0.0.1:7897/") == "127.0.0.1:7897",
           "剥尾斜杠", rta::net::http_proxy_normalize("http://127.0.0.1:7897/"));
    }

    // ---------------- 收尾 ----------------
    std::printf("\n%s\n", g_fail == 0 ? "全部通过" : (std::to_string(g_fail) + " 项失败").c_str());
    return g_fail == 0 ? 0 : 1;
}
