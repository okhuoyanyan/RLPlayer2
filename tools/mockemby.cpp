// mockemby.cpp — 模拟 Emby 服务器，用于集成测试
// 用法: mockemby <port> <audiofile>
#include "net/webui.h"
#include <cstdio>
#include <string>
#include <fstream>

using namespace rta::net;

static std::string g_file;
static const char* kUser = "demo";
static const char* kPass = "demo";
static const char* kToken = "MOCKTOKEN";

static std::string read_all(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 3) { printf("用法: mockemby <port> <audiofile>\n"); return 2; }
    const uint16_t port = (uint16_t)atoi(argv[1]);
    g_file = argv[2];

    WebUi ui;
    if (!ui.start(port, [](const HttpRequest& rq, std::string& ct) -> std::string {
        ct = "application/json; charset=utf-8";
        const std::string& p = rq.path;
        if (p.rfind("/Users/AuthenticateByName", 0) == 0) {
            if (rq.body.find(std::string("\"") + kUser + "\"") != std::string::npos &&
                rq.body.find(std::string("\"") + kPass + "\"") != std::string::npos)
                return std::string("{\"AccessToken\":\"") + kToken + "\",\"User\":{\"Id\":\"u1\"}}";
            return "{\"error\":\"bad credentials\"}";
        }
        const bool authed = p.find(std::string("api_key=") + kToken) != std::string::npos;
        if (p.rfind("/Users/u1/Items", 0) == 0) {
            if (!authed) return "{\"error\":\"unauthorized\"}";
            if (p.find("ParentId=") == std::string::npos)
                return "{\"Items\":[{\"Id\":\"ar1\",\"Name\":\"Emby 歌手\",\"Type\":\"MusicArtist\"}]}";
            return "{\"Items\":[{\"Id\":\"t1\",\"Name\":\"Emby 远程曲\",\"Type\":\"Audio\","
                   "\"Album\":\"Emby 专辑\",\"Artists\":[\"Emby 歌手\"],\"Container\":\"wav\","
                   "\"RunTimeTicks\":30000000}]}";
        }
        if (p.rfind("/Items/t1/File", 0) == 0) {
            if (!authed) { ct = "text/plain"; return "UNAUTHORIZED"; }
            ct = "audio/wav";
            return read_all(g_file);          // ★ 原样返回
        }
        if (p.rfind("/Items/", 0) == 0 && p.find("/Images/Primary") != std::string::npos) {
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
    })) { printf("启动失败\n"); return 1; }

    printf("模拟 Emby 已启动: http://127.0.0.1:%u  (%s/%s)\n", (unsigned)ui.port(), kUser, kPass);
    for (;;) Sleep(1000);
    return 0;
}
