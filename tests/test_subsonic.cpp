// test_subsonic.cpp — Subsonic 客户端端到端测试
//
// 用内置的 WebUi 起一个模拟 Navidrome，然后用真实客户端去调它：
//   * 校验 token = md5(password + salt)
//   * 校验 ping / browse / search 的解析
//   * 校验 download 拿回的是【逐字节相同】的原始文件
#include "net/webui.h"
#include "net/subsonic.h"
#include "net/md5.h"
#include <cstdio>
#include <string>
#include <vector>
#include <thread>
#include <chrono>

using namespace rta::net;

static int fails = 0;
static void ck(bool ok, const std::string& what, const std::string& detail = "") {
    printf("  [%s] %s %s\n", ok ? "PASS" : "FAIL", what.c_str(), detail.c_str());
    if (!ok) ++fails;
}

static const char* kUser = "tester";
static const char* kPass = "s3cret";

// 模拟服务端
static std::string mock_handler(const HttpRequest& rq, std::string& ct) {
    auto param = [&](const char* k) -> std::string {
        const std::string key = std::string(k) + "=";
        size_t p = rq.path.find(key);
        if (p == std::string::npos) return "";
        p += key.size();
        size_t e = rq.path.find('&', p);
        return rq.path.substr(p, (e == std::string::npos) ? std::string::npos : e - p);
    };
    const std::string u = param("u"), t = param("t"), s = param("s");

    // 鉴权：一律校验
    const bool authOk = (u == kUser) && (t == md5_hex(std::string(kPass) + s)) && !s.empty();

    ct = "application/json; charset=utf-8";
    if (rq.path.rfind("/rest/ping.view", 0) == 0) {
        if (!authOk) return "{\"subsonic-response\":{\"status\":\"failed\",\"error\":{\"message\":\"bad auth\"}}}";
        return "{\"subsonic-response\":{\"status\":\"ok\",\"version\":\"1.16.1\"}}";
    }
    if (rq.path.rfind("/rest/getMusicDirectory.view", 0) == 0) {
        if (!authOk) return "{\"subsonic-response\":{\"status\":\"failed\"}}";
        return "{\"subsonic-response\":{\"status\":\"ok\",\"directory\":{\"id\":\"1\",\"child\":["
               "{\"id\":\"s1\",\"title\":\"第一首\",\"artist\":\"测试艺术家\",\"album\":\"测试专辑\","
               "\"suffix\":\"flac\",\"duration\":215,\"size\":34567890,\"isDir\":false,\"coverArt\":\"al-1\"},"
               "{\"id\":\"d2\",\"title\":\"子目录\",\"isDir\":true}]}}";
    }
    if (rq.path.rfind("/rest/search3.view", 0) == 0) {
        if (!authOk) return "{\"subsonic-response\":{\"status\":\"failed\"}}";
        return "{\"subsonic-response\":{\"status\":\"ok\",\"searchResult3\":{\"song\":["
               "{\"id\":\"s9\",\"title\":\"搜到的歌\",\"artist\":\"A\",\"album\":\"B\"}]}}";
    }
    if (rq.path.rfind("/rest/download.view", 0) == 0) {
        if (!authOk) { ct = "text/plain"; return "AUTH_FAIL"; }
        // 返回一段可识别的"原始文件"内容
        std::string body;
        for (int i = 0; i < 4096; ++i) body += (char)(i & 0xFF);
        body += "RTA-ORIGINAL-FILE-MARKER";
        ct = "audio/flac";
        return body;
    }
    ct = "text/plain";
    return "not found";
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    SetConsoleOutputCP(CP_UTF8);

    WebUi ui;
    if (!ui.start(0, mock_handler)) { printf("模拟服务器启动失败\n"); return 2; }
    const std::string base = "http://127.0.0.1:" + std::to_string(ui.port());
    printf("模拟 Navidrome 起在 %s\n\n", base.c_str());

    SubConfig bad{ base, kUser, "wrong-pass" };
    SubConfig good{ base, kUser, kPass };

    printf("=== 1. 鉴权 ===\n");
    {
        // 注意：不能把 ping() 调用和读取 err 写在同一个表达式里 ——
        // 函数实参求值顺序未定义，err 可能在 ping 执行前就被读走，打印出过期值。
        Subsonic sBad(bad);
        std::string errBad;
        const bool badRes = sBad.ping(errBad);
        ck(!badRes, "错误口令应失败", "(err=" + errBad + ")");

        Subsonic sGood(good);
        std::string errGood;
        const bool goodRes = sGood.ping(errGood);
        ck(goodRes, "正确口令应通过", "(err=" + errGood + ")");
    }

    printf("\n=== 2. 浏览目录 ===\n");
    {
        Subsonic s(good);
        std::string err;
        auto v = s.browse("1", err);
        ck(v.size() == 2, "返回 2 个条目", "(size=" + std::to_string(v.size()) + ")");
        if (v.size() >= 2) {
            ck(v[0].title == "第一首", "中文标题解析正确", "(" + v[0].title + ")");
            ck(v[0].artist == "测试艺术家", "artist 解析正确");
            ck(v[0].suffix == "flac", "suffix 解析正确");
            ck(!v[0].isDir && v[1].isDir, "isDir 区分正确");
            ck(v[0].size == 34567890LL, "size 解析正确", "(" + std::to_string(v[0].size) + ")");
        }
    }

    printf("\n=== 3. 搜索 ===\n");
    {
        Subsonic s(good);
        std::string err;
        auto v = s.search("搜到的", err);
        ck(v.size() == 1 && v[0].title == "搜到的歌", "搜索结果解析", "(size=" + std::to_string(v.size()) + ")");
    }

    printf("\n=== 4. ★ 原始文件下载必须逐字节相同 ===\n");
    {
        Subsonic s(good);
        auto r = s.download("s1");
        std::string expected;
        for (int i = 0; i < 4096; ++i) expected += (char)(i & 0xFF);
        expected += "RTA-ORIGINAL-FILE-MARKER";
        const std::string got(r.body.begin(), r.body.end());
        ck(r.ok, "下载成功", "(" + std::to_string(r.body.size()) + " 字节)");
        ck(got == expected, "内容逐字节一致", "(FNV " + md5_hex(got).substr(0, 8) + ")");
        ck(r.contentType == "audio/flac", "Content-Type 正确", "(" + r.contentType + ")");
    }

    printf("\n=== 5. 认证失败时不得返回数据 ===\n");
    {
        Subsonic s(bad);
        auto r = s.download("s1");
        const std::string got(r.body.begin(), r.body.end());
        ck(got.find("RTA-ORIGINAL-FILE-MARKER") == std::string::npos, "错误口令拿不到原始文件");
    }

    ui.stop();
    printf("\n%s (失败 %d)\n", fails == 0 ? "全部通过" : "存在失败", fails);
    return fails == 0 ? 0 : 1;
}
