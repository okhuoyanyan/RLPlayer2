// mci_state2.cpp —— 验证按 MSF 定位播放 + position 是否实时推进
#include <cstdio>
#include <cstring>
#include <windows.h>
#include <mmsystem.h>

static bool q(const char* cmd, char* out = nullptr, DWORD n = 0) {
    char buf[256] = {0};
    const MCIERROR e = mciSendStringA(cmd, buf, sizeof(buf), nullptr);
    if (e) {
        char why[256] = {0};
        mciGetErrorStringA(e, why, sizeof(why));
        std::printf("  %-42s -> 错误: %s\n", cmd, why);
        return false;
    }
    std::printf("  %-42s -> [%s]\n", cmd, buf);
    if (out && n) std::snprintf(out, n, "%s", buf);
    return true;
}

int main() {
    char buf[64] = {0};
    if (mciSendStringA("open H: type cdaudio alias cd", buf, sizeof(buf), nullptr)) return 1;
    char start[64] = {0}, end[64] = {0};
    q("status cd position track 5", start, sizeof(start));
    q("status cd position track 6", end, sizeof(end));
    std::printf("--- 用 MSF 播放第 5 轨（%s ~ %s）---\n", start, end);
    char cmd[128];
    std::snprintf(cmd, sizeof(cmd), "play cd from %s to %s", start, end);
    q(cmd);
    for (int i = 1; i <= 5; ++i) {
        Sleep(1000);
        char pos[64] = {0};
        q("status cd position", pos, sizeof(pos));
        q("status cd mode");
    }
    q("stop cd");
    q("close cd");
    return 0;
}
