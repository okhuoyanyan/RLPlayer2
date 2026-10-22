// mocksub.cpp — 模拟 Navidrome（Subsonic API），用于集成测试
// 用法: mocksub <port> <要提供的音频文件路径>
#include "net/webui.h"
#include "net/md5.h"
#include <cstdio>
#include <string>
#include <fstream>
#include <vector>

using namespace rta::net;

static std::string g_file;
static const char* kUser = "demo";
static const char* kPass = "demo";

static std::string read_all(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 3) { printf("用法: mocksub <port> <audiofile>\n"); return 2; }
    const uint16_t port = (uint16_t)atoi(argv[1]);
    g_file = argv[2];

    WebUi ui;
    if (!ui.start(port, [](const HttpRequest& rq, std::string& ct) -> std::string {
        auto param = [&](const char* k) -> std::string {
            const std::string key = std::string(k) + "=";
            size_t p = rq.path.find(key);
            if (p == std::string::npos) return "";
            p += key.size();
            size_t e = rq.path.find('&', p);
            return rq.path.substr(p, (e == std::string::npos) ? std::string::npos : e - p);
        };
        const bool auth = (param("u") == kUser) && (param("t") == md5_hex(std::string(kPass) + param("s")));
        ct = "application/json; charset=utf-8";

        if (rq.path.rfind("/rest/ping.view", 0) == 0)
            return auth ? std::string("{\"subsonic-response\":{\"status\":\"ok\"}}")
                        : std::string("{\"subsonic-response\":{\"status\":\"failed\",\"error\":{\"message\":\"bad credentials\"}}}");

        if (rq.path.rfind("/rest/getIndexes.view", 0) == 0) {
            if (!auth) return "{\"subsonic-response\":{\"status\":\"failed\"}}";
            return "{\"subsonic-response\":{\"status\":\"ok\",\"indexes\":{\"index\":["
                   "{\"name\":\"T\",\"artist\":[{\"id\":\"ar1\",\"name\":\"测试艺术家\",\"isDir\":true}]}]}}";
        }
        if (rq.path.rfind("/rest/getMusicDirectory.view", 0) == 0) {
            if (!auth) return "{\"subsonic-response\":{\"status\":\"failed\"}}";
            return "{\"subsonic-response\":{\"status\":\"ok\",\"directory\":{\"child\":["
                   "{\"id\":\"s1\",\"title\":\"远程测试曲\",\"artist\":\"测试艺术家\",\"album\":\"远程专辑\","
                   "\"suffix\":\"wav\",\"duration\":3,\"isDir\":false,\"coverArt\":\"al1\"}]}}";
        }
        if (rq.path.rfind("/rest/getCoverArt.view", 0) == 0) {
            if (!auth) { ct = "text/plain"; return ""; }
            static const unsigned char px[] = {
                0x89,0x50,0x4E,0x47,0x0D,0x0A,0x1A,0x0A,0,0,0,0x0D,0x49,0x48,0x44,0x52,
                0,0,0,1,0,0,0,1,8,6,0,0,0,0x1F,0x15,0xC4,0x89,0,0,0,0x0A,0x49,0x44,0x41,
                0x54,0x78,0x9C,0x63,0,1,0,0,5,0,1,0x0D,0x0A,0x2D,0xB4,0,0,0,0,0x49,
                0x45,0x4E,0x44,0xAE,0x42,0x60,0x82 };
            ct = "image/png";
            return std::string((const char*)px, sizeof(px));
        }
        if (rq.path.rfind("/rest/download.view", 0) == 0) {
            if (!auth) { ct = "text/plain"; return "AUTH_FAIL"; }
            ct = "audio/wav";
            return read_all(g_file);          // ★ 原样返回原始文件
        }
        ct = "text/plain";
        return "not found";
    })) { printf("启动失败\n"); return 1; }

    printf("模拟 Navidrome 已启动: http://127.0.0.1:%u\n", (unsigned)ui.port());
    printf("  用户 %s / 口令 %s\n", kUser, kPass);
    printf("  提供文件: %s\n", g_file.c_str());
    for (;;) Sleep(1000);
    return 0;
}
