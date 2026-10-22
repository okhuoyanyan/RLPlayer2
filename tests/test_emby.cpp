// test_emby.cpp — Emby 客户端端到端测试（内置模拟 Emby 服务器）
#include "net/webui.h"
#include "net/emby.h"
#include <cstdio>
#include <string>

using namespace rta::net;

static int fails = 0;
static void ck(bool ok, const std::string& what, const std::string& detail = "") {
    printf("  [%s] %s %s\n", ok ? "PASS" : "FAIL", what.c_str(), detail.c_str());
    if (!ok) ++fails;
}

static const char* kUser = "embyuser";
static const char* kPass = "embypass";
static const char* kToken = "TOKEN123456";

static std::string mock(const HttpRequest& rq, std::string& ct) {
    ct = "application/json; charset=utf-8";
    const std::string& p = rq.path;

    if (p.rfind("/Users/AuthenticateByName", 0) == 0) {
        // 必须带 X-Emby-Authorization 头，且请求体里用户名口令正确
        // （WebUi 目前不暴露请求头给 handler，这里用请求体判断）
        if (rq.body.find(std::string("\"") + kUser + "\"") != std::string::npos &&
            rq.body.find(std::string("\"") + kPass + "\"") != std::string::npos) {
            return std::string("{\"AccessToken\":\"") + kToken + "\",\"User\":{\"Id\":\"u1\"}}";
        }
        ct = "application/json";
        return "{\"error\":\"bad credentials\"}";
    }
    // 其余接口都要 api_key
    const bool authed = rq.path.find(std::string("api_key=") + kToken) != std::string::npos;

    if (p.rfind("/Users/u1/Items", 0) == 0) {
        if (!authed) return "{\"error\":\"unauthorized\"}";
        if (p.find("ParentId=") == std::string::npos) {
            return "{\"Items\":[{\"Id\":\"ar1\",\"Name\":\"测试歌手\",\"Type\":\"MusicArtist\"}]}";
        }
        return "{\"Items\":[{\"Id\":\"t1\",\"Name\":\"Emby 测试曲\",\"Type\":\"Audio\","
               "\"Album\":\"Emby 专辑\",\"Artists\":[\"测试歌手\"],\"Container\":\"flac\","
               "\"RunTimeTicks\":1850000000}]}";
    }
    if (p.rfind("/Items/t1/File", 0) == 0) {
        if (!authed) { ct = "text/plain"; return "UNAUTHORIZED"; }
        std::string body;
        for (int i = 0; i < 2048; ++i) body += (char)((i * 7) & 0xFF);
        body += "EMBY-ORIGINAL-MARKER";
        ct = "audio/flac";
        return body;
    }
    if (p.rfind("/Items/t1/Images/Primary", 0) == 0) {
        static const unsigned char px[] = {
            0x89,0x50,0x4E,0x47,0x0D,0x0A,0x1A,0x0A,0,0,0,0x0D,0x49,0x48,0x44,0x52,
            0,0,0,1,0,0,0,1,8,6,0,0,0,0x1F,0x15,0xC4,0x89,0,0,0,0x0A,0x49,0x44,0x41,
            0x54,0x78,0x9C,0x63,0,1,0,0,5,0,1,0x0D,0x0A,0x2D,0xB4,0,0,0,0,0x49,
            0x45,0x4E,0x44,0xAE,0x42,0x60,0x82 };
        ct = "image/png";
        return std::string((const char*)px, sizeof(px));
    }
    ct = "text/plain";
    return "not found";
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    SetConsoleOutputCP(CP_UTF8);

    WebUi ui;
    if (!ui.start(0, mock)) { printf("模拟 Emby 启动失败\n"); return 2; }
    const std::string base = "http://127.0.0.1:" + std::to_string(ui.port());
    printf("模拟 Emby 起在 %s\n\n", base.c_str());

    printf("=== 1. 登录 ===\n");
    {
        Emby bad(EmbyConfig{ base, kUser, "wrong" });
        std::string e1; const bool r1 = bad.login(e1);
        ck(!r1, "错误口令应失败", "(err=" + e1 + ")");

        Emby good(EmbyConfig{ base, kUser, kPass });
        std::string e2; const bool r2 = good.login(e2);
        ck(r2, "正确口令应通过", "(err=" + e2 + ")");

        printf("\n=== 2. 浏览 ===\n");
        std::string e3;
        auto root = good.browse("", e3);
        ck(root.size() == 1 && root[0].title == "测试歌手" && root[0].isDir,
           "根目录返回艺术家", "(size=" + std::to_string(root.size()) + ")");

        std::string e4;
        auto alb = good.browse("ar1", e4);
        ck(alb.size() == 1, "艺术家目录返回曲目", "(size=" + std::to_string(alb.size()) + ")");
        if (alb.size() == 1) {
            ck(alb[0].title == "Emby 测试曲", "标题解析", "(" + alb[0].title + ")");
            ck(alb[0].artist == "测试歌手", "Artists 数组解析", "(" + alb[0].artist + ")");
            ck(alb[0].suffix == "flac", "Container 解析");
            ck(std::abs(alb[0].duration - 185.0) < 0.5, "RunTimeTicks 换算成秒",
               "(" + std::to_string(alb[0].duration) + " 秒)");
            ck(!alb[0].isDir, "曲目 isDir=false");
        }

        printf("\n=== 3. ★ 原始文件必须逐字节相同 ===\n");
        auto r = good.download("t1");
        std::string exp;
        for (int i = 0; i < 2048; ++i) exp += (char)((i * 7) & 0xFF);
        exp += "EMBY-ORIGINAL-MARKER";
        const std::string got(r.body.begin(), r.body.end());
        ck(r.ok, "下载成功", "(" + std::to_string(r.body.size()) + " 字节)");
        ck(got == exp, "内容逐字节一致");
        ck(r.contentType == "audio/flac", "Content-Type 正确", "(" + r.contentType + ")");

        printf("\n=== 4. 封面 ===\n");
        auto cv = good.cover("t1");
        ck(cv.ok && cv.body.size() > 50, "封面可取", "(" + std::to_string(cv.body.size()) + " 字节)");
    }

    ui.stop();
    printf("\n%s (失败 %d)\n", fails == 0 ? "全部通过" : "存在失败", fails);
    return fails == 0 ? 0 : 1;
}
