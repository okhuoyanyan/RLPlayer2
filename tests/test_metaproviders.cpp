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
#include "meta/artwork.h"        // 能力矩阵派生 + 头像/封面链(2026-10-27)
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using namespace rta::meta;

static int g_fail = 0;
static void ck(bool cond, const char* what, const std::string& detail = "") {
    if (cond) std::printf("  [PASS] %s\n", what);
    else { std::printf("  [FAIL] %s  %s\n", what, detail.c_str()); ++g_fail; }
}
// 字符串相等断言(顺序/派生名单这类「整串对齐」的判据用它，失败时两边都打出来)
static void ck_eq(const std::string& got, const std::string& want, const char* what) {
    ck(got == want, what, "got=[" + got + "] want=[" + want + "]");
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
        // 2026-10-26（261026-07）：Discogs 已注册为**纯开关型**数据源（canSearch/canLyric 都为 false），
    // 让它在设置页可见、/api/providers 可返回、用户可关闭 —— 之前它在 enrich 里干活但完全不可见。
    ck(reg.size() == 6, "注册表 6 项(四源 + dizzylab + discogs)", std::to_string(reg.size()));
    {
        const ProviderDesc* dg = nullptr;
        for (const auto& d : reg) if (std::string(d.id) == "discogs") { dg = &d; break; }
        ck(dg != nullptr, "Discogs 已在注册表内");
        if (dg) ck(!dg->canSearch && !dg->canLyric, "Discogs 为纯开关型（不参与搜索/歌词遍历）");
    }
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
            ck(reg[0].searchQuery != nullptr, "dizzylab searchQuery 非空(E-17 支持扩展上下文)");
            ck(reg[1].searchQuery == nullptr && reg[2].searchQuery == nullptr &&
               reg[3].searchQuery == nullptr && reg[4].searchQuery == nullptr,
               "其它源 searchQuery 为空(默认忽略新参数)");
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

    // ---------------- 10) 能力矩阵 + 全信源候选派生(2026-10-27) ----------------
    // 判据(铁律 3)：这一节要证明的不是「函数能跑」，而是
    //   (a) caps 是唯一事实源 —— canSearch/canLyric 必须严格等于 caps&指针；
    //   (b) 头像/封面/歌词的候选列表**完全由矩阵派生**(没有第二份手写名单)；
    //   (c) 精确名匹配能挡住实测中真实存在的错位(粉丝号/同名他人/拼写变体)；
    //   (d) 负缓存命中时**零网络**(用注入计数器的 mock transport 数请求次数)。
    // Fixture 全部来自 2026-10-27 真实响应切片(网易 type=100 / QQ musicu / dizzylab /search /
    // Discogs 匿名)，不是拟合数据。
    std::printf("-- 能力矩阵 / 全信源候选派生 --\n");
    {
        const auto& reg = provider_registry();
        auto has = [&](const char* id, ArtCap c) {
            for (const auto& d : reg)
                if (std::string(d.id) == id) return pv_has_cap(d.caps, c);
            return false;
        };

        // (a) 派生位 ⇔ 矩阵 ⇔ 指针
        bool consistent = true;
        std::string bad;
        for (const auto& d : reg) {
            if (d.canSearch != (pv_has_cap(d.caps, ArtCap::Search) && d.search != nullptr)) {
                consistent = false; bad += std::string(d.id) + "(canSearch);";
            }
            if (d.canLyric != (pv_has_cap(d.caps, ArtCap::Lyrics) && d.lyric != nullptr)) {
                consistent = false; bad += std::string(d.id) + "(canLyric);";
            }
        }
        ck(consistent, "canSearch/canLyric 严格由 caps+指针派生(caps 是唯一事实源)", bad);

        // (b) 五个内容类的能力位(依据写在 provider.h 注册表条目注释里)
        ck(has("dizzylab", ArtCap::ArtistImage) && has("qq", ArtCap::ArtistImage) &&
           has("netease", ArtCap::ArtistImage) && has("discogs", ArtCap::ArtistImage),
           "头像能力: dizzylab/qq/netease/discogs = true(全信源,不是只有网易云)");
        ck(!has("itunes", ArtCap::ArtistImage),
           "iTunes 头像 = false(实测 entity=musicArtist 响应里没有 artwork 字段)");
        ck(!has("lrclib", ArtCap::Cover) && !has("lrclib", ArtCap::ArtistImage) &&
           !has("lrclib", ArtCap::ArtistBio),
           "lrclib 只有歌词(封面/头像/简介 = false)");
        ck(has("dizzylab", ArtCap::ArtistBio) && has("discogs", ArtCap::ArtistBio) &&
           !has("netease", ArtCap::ArtistBio) && !has("qq", ArtCap::ArtistBio) &&
           !has("itunes", ArtCap::ArtistBio),
           "简介能力: dizzylab/discogs = true;网易 briefDesc 实测为空、QQ/iTunes 无接口 ⇒ false");
        ck(has("qq", ArtCap::Lyrics) && has("netease", ArtCap::Lyrics) &&
           has("lrclib", ArtCap::Lyrics) && !has("dizzylab", ArtCap::Lyrics) &&
           !has("itunes", ArtCap::Lyrics) && !has("discogs", ArtCap::Lyrics),
           "歌词能力: qq/netease/lrclib = true(其余 false)");

        // (c) 候选列表 = 矩阵派生(顺序与改造前逐项一致)
        std::map<std::string, bool> on;
        auto ids = [](const std::vector<const ProviderDesc*>& v) {
            std::string s;
            for (const auto* p : v) { if (!s.empty()) s += ","; s += p->id; }
            return s;
        };
        ck_eq(ids(art_cover_sources(on)), "dizzylab,qq,netease,itunes,discogs",
              "封面候选源 = 矩阵 Cover 位 + 注册表正序");
        ck_eq(ids(art_artist_sources(on)), "dizzylab,qq,netease,discogs",
              "头像候选源 = 矩阵 ArtistImage 位 + 注册表正序");
        ck_eq(ids(art_lyric_sources(on)), "lrclib,netease,qq",
              "歌词链 = 矩阵 Lyrics 位 + 逆序(与改造前逐项相同)");

        // 源开关仍然生效(数据驱动不等于忽略用户设置)
        std::map<std::string, bool> off;
        off["dizzylab"] = false;
        ArtworkOpts offOpts;
        offOpts.srcOn = off;
        std::string planOff;
        for (const ArtArtistSource* s : art_artist_image_plan(offOpts)) {
            if (!planOff.empty()) planOff += ",";
            planOff += s->id;
        }
        ck_eq(planOff, "qq,netease,discogs", "关掉 dizzylab 后头像候选自动去掉它");

        // 抓取实现表 ⇔ 矩阵(两边不一致 = 有人声明了能力却没实现，或实现了却没声明)
        size_t capCount = 0;
        for (const auto& d : reg) if (pv_has_cap(d.caps, ArtCap::ArtistImage)) ++capCount;
        ck(capCount == art_artist_fetchers().size(),
           "声明 ArtistImage 的源数 == 有抓取实现的源数",
           std::to_string(capCount) + " vs " + std::to_string(art_artist_fetchers().size()));
        bool fetchersDeclared = true;
        std::string undeclared;
        for (const auto& f : art_artist_fetchers())
            if (!has(f.id, ArtCap::ArtistImage)) { fetchersDeclared = false; undeclared += f.id; }
        ck(fetchersDeclared, "每个抓取实现都在矩阵里声明了 ArtistImage", undeclared);
    }

    // ---------------- 11) 艺人名归一化 + 精确匹配(防错位的唯一闸门) ----------------
    std::printf("-- 艺人名归一化 / 精确匹配 --\n");
    {
        ck(art_norm_name("  Khrisma  ") == "khrisma", "归一化:去首尾空白 + 小写");
        ck(art_norm_name("ＫＨＲＩＳＭＡ") == "khrisma", "归一化:全角 ASCII 折半角");
        ck(art_norm_name("The   Beatles") == "the beatles", "归一化:内部连续空白折叠");
        ck(art_norm_name("周杰伦.") != art_norm_name("周杰伦"),
           "归一化**不丢标点**:网易云粉丝号「周杰伦.」不等于「周杰伦」");
        ck(art_name_equal("Orquesta de Camara Promusica de Malaga",
                          "Orquesta de Cámara Promúsica de Málaga"),
           "拉丁重音折叠:「Malaga」==「Málaga」(实测 QQ 返回的就是带音标的名字)");
        ck(!art_name_equal("Motley Crue", "Motley Crue II"), "折叠不越过「名字不同」这条线");
        ck(art_name_equal("洛天依", " 洛天依 "), "精确匹配:空白无关");
        ck(!art_name_equal("超绝可爱洛天依", "洛天依"), "精确匹配:拒绝粉丝号包子名");
        ck(art_name_fuzzy("超绝可爱洛天依", "洛天依"),
           "宽松匹配(仅 allowFuzzy=true)会误判粉丝号 —— 这正是它默认关闭的理由");

        // 网易云 type=100 真实响应切片:第一条是正主,第二条是粉丝号(带 accountId)
        const char* neJson =
            "{\"result\":{\"artistCount\":83,\"artists\":["
            "{\"id\":6452,\"name\":\"周杰伦\",\"picUrl\":\"https://p1.music.126.net/NWv6/109951169164936450.jpg\","
            "\"alias\":[\"Jay Chou\"]},"
            "{\"id\":51128649,\"name\":\"周杰伦.\",\"picUrl\":\"https://p1.music.126.net/IovR/109951168883440426.jpg\","
            "\"accountId\":6347778659}]}}";
        const std::vector<ArtArtistCandidate> ne = art_parse_netease_artists(neJson);
        ck(ne.size() == 2, "netease 解析 2 条艺人", std::to_string(ne.size()));
        ck(ne.size() == 2 && ne[0].id == "6452" && ne[0].pageUrl.find("6452") != std::string::npos,
           "netease 解析 id/外链");
        ck(ne.size() == 2 && ne[1].fanAccount, "netease 把带 accountId 的识别为粉丝号");
        const ArtArtistCandidate* pickNe = art_pick_artist(ne, "周杰伦", false);
        ck(pickNe && pickNe->id == "6452", "netease 精确匹配挑正主,不挑粉丝号",
           pickNe ? pickNe->id : std::string("(null)"));

        // QQ musicu 真实响应切片:Khrisma 只搜到 Kharisma(拼写不同)
        const char* qqJson =
            "{\"code\":0,\"req\":{\"code\":0,\"data\":{\"body\":{\"singer\":{\"list\":["
            "{\"singerID\":1173855,\"singerMID\":\"002G3B064RcufT\",\"singerName\":\"Kharisma\","
            "\"singerPic\":\"http://y.gtimg.cn/music/photo_new/T001R150x150M000002G3B064RcufT_2.jpg\"},"
            "{\"singerID\":8098174,\"singerMID\":\"003dYMST40KSmO\",\"singerName\":\"炒饭\","
            "\"singerPic\":\"http://y.gtimg.cn/music/photo_new/T001R150x150M000003dYMST40KSmO_2.jpg\"}"
            "]}}}}}";
        const std::vector<ArtArtistCandidate> qq = art_parse_qq_singers(qqJson);
        ck(qq.size() == 2, "QQ 解析 2 条歌手", std::to_string(qq.size()));
        ck(qq.size() == 2 && qq[1].imageUrl.find("T001R150x150") != std::string::npos,
           "QQ 解析 singerPic(150x150 原样保留)");
        ck(art_pick_artist(qq, "Khrisma", false) == nullptr,
           "QQ 精确匹配:拒绝 Kharisma(拼写不同,宁可留空)");
        const ArtArtistCandidate* fuzzyQq = art_pick_artist(qq, "Khrisma", true);
        ck(fuzzyQq == nullptr,
           "QQ 即使 allowFuzzy=true 也不接受 Kharisma(不是包含关系;宁缺毋错)");
        const ArtArtistCandidate* pickFc = art_pick_artist(qq, "炒饭", false);
        ck(pickFc && pickFc->id == "003dYMST40KSmO", "QQ 精确匹配:炒饭 命中");

        // dizzylab 真实搜索页切片(「作品」段 + 「用户」段;用户段里有同名他人)
        const char* dzHtml =
            "<h4 class=\"text-warning\">作品</h4>\n"
            "<a href=\"/d/GKCD-013\">\n<div class=\"card\">\n<div class=\"card-body\">\n"
            "<img src=\"https://cdn.dizzylab.net/media/cover/GKCD-013.jpg!cover\" />\n"
            "<h1>ORDINARY MIRACLES</h1>\n<h3>月華社</h3>\n</div>\n</div>\n</a>\n"
            "<hr class=\"border-1 border-dark\" />\n"
            "<h4 class=\"text-warning\">用户</h4>\n"
            "<a href=\"/u/1307\">\n<div class=\"card\">\n<div class=\"card-body\">\n"
            "<img src=\"https://cdn.dizzylab.net/media/avatars/1307_dizzz.jpg!cover\" width=\"100\" />\n"
            "<h1>铁棍山药炒饭</h1>\n<p>一个饭。</p>\n</div>\n</div>\n</a>\n"
            "<a href=\"/u/68242\">\n<div class=\"card\">\n<div class=\"card-body\">\n"
            "<img src=\"https://cdn.dizzylab.net/media/avatars/7B6C6FC7B3-E054-4736-BD95-B64DC1BBCE467D.png!cover\" width=\"100\" />\n"
            "<h1>Khrisma</h1>\n<p style='margin-top:12px'>月華社所属。东方/编曲/音游/公共交通/F1。</p>\n"
            "</div>\n</div>\n</a>\n";
        const std::vector<ArtArtistCandidate> dz = art_parse_dizzylab_users(dzHtml);
        ck(dz.size() == 2, "dizzylab 只解析「用户」段(作品段的卡片不算用户)",
           std::to_string(dz.size()));
        ck(dz.size() == 2 && dz[1].id == "68242" && dz[1].name == "Khrisma",
           "dizzylab 解析用户 id/名字");
        ck(dz.size() == 2 && dz[1].imageUrl ==
               "https://cdn.dizzylab.net/media/avatars/7B6C6FC7B3-E054-4736-BD95-B64DC1BBCE467D.png!cover",
           "dizzylab 解析头像 URL(avatars/…!cover)", dz.size() == 2 ? dz[1].imageUrl : "");
        ck(dz.size() == 2 && dz[1].bio.find("月華社所属") == 0,
           "dizzylab 解析简介(用户卡片的 <p>)", dz.size() == 2 ? dz[1].bio : "");
        ck(dz.size() == 2 && dz[1].pageUrl == "https://dizzylab.net/u/68242",
           "dizzylab 解析用户外链");
        ck(art_pick_artist(dz, "炒饭", false) == nullptr,
           "dizzylab 精确匹配:拒绝「铁棍山药炒饭」(实测最容易张冠李戴的一条)");
        const ArtArtistCandidate* dzHit = art_pick_artist(dz, "Khrisma", false);
        ck(dzHit && dzHit->id == "68242", "dizzylab 精确匹配:Khrisma 命中本人");

        // 没有「用户」段 = 该名字在 dizzylab 没有用户命中(实测 蔡琴/Various Artists 就是这种)
        ck(art_parse_dizzylab_users("<h4 class=\"text-warning\">作品</h4><a href=\"/d/X\">x</a>").empty(),
           "无「用户」段 → 0 条(不猜)");

        // Discogs 匿名真实响应切片
        const char* dcSearch =
            "{\"pagination\":{\"items\":1},\"results\":["
            "{\"id\":5296077,\"type\":\"artist\",\"title\":\"蔡琴\",\"uri\":\"/artist/5296077-蔡琴\","
            "\"resource_url\":\"https://api.discogs.com/artists/5296077\"}]}";
        const std::vector<ArtArtistCandidate> dc = art_parse_discogs_search(dcSearch);
        ck(dc.size() == 1 && dc[0].id == "5296077" && dc[0].name == "蔡琴",
           "Discogs 搜索解析(type=artist 过滤 + id/name)");
        const char* dcArtist =
            "{\"name\":\"蔡琴\",\"id\":5296077,\"uri\":\"https://www.discogs.com/artist/5296077-蔡琴\","
            "\"profile\":\"Tsai Chin is a pop and folk singer from Taiwan.\","
            "\"images\":[{\"type\":\"secondary\",\"uri\":\"https://i.discogs.com/sec.jpeg\"},"
            "{\"type\":\"primary\",\"uri\":\"https://i.discogs.com/pri.jpeg\"}]}";
        const ArtArtistCandidate dca = art_parse_discogs_artist(dcArtist);
        ck(dca.imageUrl == "https://i.discogs.com/pri.jpeg",
           "Discogs 详情:优先取 images[type=primary]", dca.imageUrl);
        ck(dca.bio.find("Tsai Chin") == 0, "Discogs 详情:profile → 简介");
        ck(dca.pageUrl.find("discogs.com/artist") != std::string::npos, "Discogs 详情:外链");

        // 图片魔数嗅探(不看 Content-Type)
        std::string jpg(3000, (char)0x41);
        jpg[0] = (char)0xFF; jpg[1] = (char)0xD8;
        std::string png(3000, (char)0x41);
        png[0] = (char)0x89; png[1] = 'P'; png[2] = 'N'; png[3] = 'G';
        ck(art_image_magic(jpg) && art_image_magic(png), "魔数嗅探:JPEG/PNG 通过");
        ck(!art_image_magic("<html>not an image</html>"), "魔数嗅探:HTML 报错页被拒");
    }

    // ---------------- 11b) 主艺人回退(多艺人串) ----------------
    // 真实库 245 位里有 106 位查不到头像，绝大多数是「A, B feat. C」这种多艺人串 ——
    // 任何源都不会有「A, B feat. C」这个艺人。取第一段(主艺人)再走同一条精确匹配链。
    std::printf("-- 主艺人回退(多艺人串) --\n");
    {
        ck_eq(art_primary_artist("陈彼得/游戏科学/8082Audio"), "陈彼得", "取 '/' 前的主艺人(实测库里就有这种串)");
        ck_eq(art_primary_artist("Colorpen & Loyde Vovo"), "Colorpen", "取 ' & ' 前的主艺人");
        ck_eq(art_primary_artist("SinghP, Haru.jpg, 之初"), "SinghP", "取 ',' 前的主艺人");
        ck_eq(art_primary_artist("洛天依 feat. 言和"), "洛天依", "取 ' feat. ' 前的主艺人");
        ck(art_primary_artist("AC/DC").empty(),
           "「AC/DC」不切(切出 AC 会贴错人) —— '/' 切分要求 ≥3 字");
        ck(art_primary_artist("Peak( かりんと, あさぬこ").empty(),
           "括号不配平的截断串放弃");
        ck(art_primary_artist("蔡琴").empty(), "单艺人名不产生主艺人");
        ck_eq(art_primary_artist("  甲 & 乙 "), "甲", "主艺人也会 trim");
    }

    // ---------------- 12) 封面候选 URL 链(修「只问第一首」) ----------------
    std::printf("-- 封面候选 URL 链 --\n");
    {
        std::vector<Candidate> cs(4);
        cs[0].source = "dizzylab"; cs[0].coverUrl = "https://a/1.jpg";
        cs[1].source = "qq";       cs[1].coverUrl = "https://b/2.jpg";
        cs[2].source = "netease";  cs[2].coverUrl = "https://a/1.jpg";   // 与 cs[0] 重复
        cs[3].source = "itunes";   cs[3].coverUrl = "";                  // 没有封面
        std::vector<Scored> sorted(4);
        for (size_t i = 0; i < 4; ++i) { sorted[i].idx = i; sorted[i].score = 1.0 - 0.1 * (double)i; }
        const std::vector<std::string> chain = art_cover_urls_by_score(sorted, cs);
        ck(chain.size() == 2 && chain[0] == "https://a/1.jpg" && chain[1] == "https://b/2.jpg",
           "封面链:按分数排序 + 去重 + 跳过空 URL", std::to_string(chain.size()));

        // 逐个 URL 试:第一个 404、第二个是图片 → 成功,且把失败原因留下
        std::vector<std::string> calls;
        std::string jpg(3000, (char)0x41);
        jpg[0] = (char)0xFF; jpg[1] = (char)0xD8;
        set_artwork_http_for_test([&](const ArtHttpRequest& q) {
            calls.push_back(q.url);
            rta::net::HttpResponse r;
            if (q.url == "https://b/2.jpg") {
                r.ok = true; r.status = 200; r.body.assign(jpg.begin(), jpg.end());
            } else {
                r.status = 404;
            }
            return r;
        });
        ArtworkOpts covOpts;
        covOpts.minBytes = 2049;          // 与 enrich 侧旧判据 >2048 等价
        const CoverFetchResult cf = art_fetch_cover_urls({"https://a/1.jpg", "https://b/2.jpg"}, covOpts);
        ck(cf.ok && cf.url == "https://b/2.jpg", "封面链:第一张 404 后自动换第二张", cf.err);
        ck(cf.tried.size() == 1 && cf.tried[0].find("HTTP 404") != std::string::npos,
           "封面链:失败原因逐条留痕", cf.tried.empty() ? std::string() : cf.tried[0]);
        ck(cf.bytes.size() == 3000, "封面链:返回图片字节");

        // 小于阈值的图不收(旧判据 >2KB)
        set_artwork_http_for_test([&](const ArtHttpRequest&) {
            rta::net::HttpResponse r;
            r.ok = true; r.status = 200;
            std::string small(100, (char)0x41);
            small[0] = (char)0xFF; small[1] = (char)0xD8;
            r.body.assign(small.begin(), small.end());
            return r;
        });
        const CoverFetchResult cf2 = art_fetch_cover_urls({"https://tiny.jpg"}, covOpts);
        ck(!cf2.ok, "封面链:<2KB 的图不采纳(与旧判据一致)");
        set_artwork_http_for_test(nullptr);
    }

    // ---------------- 13) 头像:正缓存/负缓存/瞬态失败(零网络验证) ----------------
    std::printf("-- 头像缓存(注入 transport 数请求次数)--\n");
    {
        namespace fs = std::filesystem;
        const char* dzUserHtml =
            "<h4 class=\"text-warning\">用户</h4>\n"
            "<a href=\"/u/99999\">\n<div class=\"card\">\n<div class=\"card-body\">\n"
            "<img src=\"https://cdn.dizzylab.net/media/avatars/t.png!cover\" width=\"100\" />\n"
            "<h1>测试艺人-缓存</h1>\n<p>简介一段。</p>\n</div>\n</div>\n</a>\n";
        const char* dzEmptyHtml = "<h4 class=\"text-warning\">作品</h4><p>无用户段</p>";
        const char* emptyJson = "{\"result\":{\"artistCount\":0,\"artists\":[]}}";
        const char* emptyQq = "{\"req\":{\"data\":{\"body\":{\"singer\":{\"list\":[]}}}}}";
        const char* emptyDc = "{\"results\":[]}";

        std::string jpg(3000, (char)0x41);
        jpg[0] = (char)0xFF; jpg[1] = (char)0xD8;

        std::vector<std::string> calls;
        int mode = 0;   // 0=命中 1=全 miss 2=瞬态失败
        auto mock = [&](const ArtHttpRequest& q) {
            calls.push_back(q.url);
            rta::net::HttpResponse r;
            r.ok = true; r.status = 200;
            if (q.url.find("cdn.dizzylab.net/media/avatars/") != std::string::npos) {
                r.body.assign(jpg.begin(), jpg.end());
                return r;
            }
            if (q.url.find("dizzylab.net/search") != std::string::npos) {
                if (mode == 2) { r.ok = false; r.status = 0; r.error = "connect failed"; return r; }
                if (mode == 3) {
                    // 「甲 & 乙」全名(编码后含 %26)查不到；「甲」能查到 —— 专门验证主艺人回退
                    if (q.url.find("%26") != std::string::npos) {
                        r.body.assign(dzEmptyHtml, dzEmptyHtml + std::strlen(dzEmptyHtml));
                        return r;
                    }
                    const char* jia =
                        "<h4 class=\"text-warning\">用户</h4>\n"
                        "<a href=\"/u/77777\">\n<div class=\"card\">\n<div class=\"card-body\">\n"
                        "<img src=\"https://cdn.dizzylab.net/media/avatars/jia.png!cover\" width=\"100\" />\n"
                        "<h1>甲</h1>\n<p>主艺人。</p>\n</div>\n</div>\n</a>\n";
                    r.body.assign(jia, jia + std::strlen(jia));
                    return r;
                }
                const std::string h = (mode == 0) ? dzUserHtml : dzEmptyHtml;
                r.body.assign(h.begin(), h.end());
                return r;
            }
            if (q.url.find("musicu.fcg") != std::string::npos) {
                r.body.assign(emptyQq, emptyQq + std::strlen(emptyQq)); return r;
            }
            if (q.url.find("/api/search/get") != std::string::npos) {
                r.body.assign(emptyJson, emptyJson + std::strlen(emptyJson)); return r;
            }
            if (q.url.find("api.discogs.com") != std::string::npos) {
                r.body.assign(emptyDc, emptyDc + std::strlen(emptyDc)); return r;
            }
            r.status = 404; r.ok = false;
            return r;
        };
        set_artwork_http_for_test(mock);
        auto mockNow = std::chrono::steady_clock::now();
        set_dizzylab_clock_for_test([&]() { return mockNow; });
        dl_gate_reset_for_test();

        const fs::path dir = fs::temp_directory_path() / "rlplayer_artwork_test";
        std::error_code ec;
        fs::remove_all(dir, ec);
        ArtworkOpts o;
        o.cacheDir = dir.string();
        o.totalBudgetMs = 8000;

        // ① 正缓存:第 1 次真抓(搜索 1 + 头像下载 1),第 2 次零网络
        mode = 0;
        calls.clear();
        const ArtistImageResult r1 = artwork_fetch_artist_image("测试艺人-缓存", o);
        const size_t n1 = calls.size();
        ck(r1.ok && r1.source == "dizzylab" && r1.bytes.size() == 3000,
           "头像命中:来源=dizzylab,字节数正确", r1.summary());
        ck(n1 == 2, "第 1 次:搜索 1 次 + 下载 1 次 = 2 个请求", std::to_string(n1));
        calls.clear();
        mockNow += std::chrono::seconds(5);
        const ArtistImageResult r2 = artwork_fetch_artist_image("测试艺人-缓存", o);
        ck(r2.ok && r2.fromCache, "第 2 次:命中磁盘正缓存", r2.summary());
        ck(calls.empty(), "第 2 次:零网络", std::to_string(calls.size()));
        ck(r2.source == "dizzylab" && r2.url.find("avatars/t.png") != std::string::npos,
           "第 2 次:溯源(source/url)从 .src 读回", r2.source + " " + r2.url);

        // ② 负缓存:第 1 次 4 源全 miss,第 2/3 次必须零网络
        mode = 1;
        calls.clear();
        mockNow += std::chrono::seconds(5);
        const ArtistImageResult m1 = artwork_fetch_artist_image("无图艺人-负缓存", o);
        ck(!m1.ok, "负缓存用例:第 1 次没有头像", m1.summary());
        ck(m1.tried.size() == 4, "负缓存用例:四个源都试过(全信源,不是只问网易云)",
           std::to_string(m1.tried.size()));
        const size_t nMiss1 = calls.size();
        ck(nMiss1 >= 4, "负缓存用例:第 1 次确实打了网络", std::to_string(nMiss1));
        calls.clear();
        mockNow += std::chrono::seconds(5);
        const ArtistImageResult m2 = artwork_fetch_artist_image("无图艺人-负缓存", o);
        ck(!m2.ok && m2.fromNegCache, "负缓存用例:第 2 次走负缓存", m2.summary());
        ck(calls.empty(), "负缓存用例:第 2 次零网络", std::to_string(calls.size()));
        calls.clear();
        mockNow += std::chrono::seconds(5);
        const ArtistImageResult m3 = artwork_fetch_artist_image("无图艺人-负缓存", o);
        ck(!m3.ok && calls.empty(), "负缓存用例:第 3 次仍零网络", std::to_string(calls.size()));
        std::string negText;
        {
            std::ifstream f(fs::u8path(art_cache_paths(o.cacheDir, "无图艺人-负缓存").none), std::ios::binary);
            std::ostringstream ss; ss << f.rdbuf(); negText = ss.str();
        }
        ck(negText.find("src=dizzylab") != std::string::npos &&
           negText.find("src=qq") != std::string::npos &&
           negText.find("src=netease") != std::string::npos &&
           negText.find("src=discogs") != std::string::npos,
           "负缓存文件逐源记录(src=dizzylab/qq/netease/discogs)", negText.substr(0, 120));
        ck(negText.find("kind=miss") != std::string::npos && negText.find("reason=") != std::string::npos,
           "负缓存文件带 kind=miss 与 reason=");

        // ③ 瞬态失败不钉死:kind=fail 的源下次仍重试(只记原因)
        mode = 2;
        calls.clear();
        mockNow += std::chrono::seconds(5);
        const ArtistImageResult t1 = artwork_fetch_artist_image("抖动艺人-瞬态", o);
        ck(!t1.ok, "瞬态用例:第 1 次没有头像");
        calls.clear();
        mockNow += std::chrono::seconds(5);
        const ArtistImageResult t2 = artwork_fetch_artist_image("抖动艺人-瞬态", o);
        const bool dizzylabRetried = std::any_of(calls.begin(), calls.end(), [](const std::string& u) {
            return u.find("dizzylab.net/search") != std::string::npos;
        });
        ck(dizzylabRetried, "瞬态失败(断网)下次仍重试 dizzylab(不写死成「没有头像」)",
           std::to_string(calls.size()) + " 个请求");
        (void)t2;

        // ④ 主艺人回退:「甲 & 乙」四源都没有,但「甲」在 dizzylab 有 -> 命中主艺人
        mode = 3;
        calls.clear();
        mockNow += std::chrono::seconds(5);
        const ArtistImageResult p1 = artwork_fetch_artist_image("甲 & 乙", o);
        ck(p1.ok && p1.bytes.size() == 3000, "主艺人回退:全名落空后靠「甲」命中", p1.summary());
        ck(p1.source.find("dizzylab") == 0 && p1.source.find("主艺人") != std::string::npos,
           "主艺人回退:溯源里写明「<-主艺人 甲」", p1.source);
        {
            std::string meta;
            std::ifstream f(fs::u8path(art_cache_paths(o.cacheDir, "甲 & 乙").src), std::ios::binary);
            std::ostringstream ss; ss << f.rdbuf(); meta = ss.str();
            ck(meta.find("primary=甲") != std::string::npos,
               "主艺人回退:.src 里记 primary=(可核对这张脸是谁的)", meta.substr(0, 80));
        }
        // 关掉这条策略就不该命中(证明它是显式开关，不是隐式行为)
        ArtworkOpts o2 = o;
        o2.splitPrimary = false;
        o2.cacheDir.clear();            // 不吃上面那份缓存
        const ArtistImageResult p2 = artwork_fetch_artist_image("甲 & 乙", o2);
        ck(!p2.ok, "splitPrimary=false 时不做主艺人回退");

        set_artwork_http_for_test(nullptr);
        set_dizzylab_clock_for_test(nullptr);
        dl_gate_reset_for_test();
        fs::remove_all(dir, ec);
    }

    // ---------------- 收尾 ----------------
    std::printf("\n%s\n", g_fail == 0 ? "全部通过" : (std::to_string(g_fail) + " 项失败").c_str());
    return g_fail == 0 ? 0 : 1;
}
