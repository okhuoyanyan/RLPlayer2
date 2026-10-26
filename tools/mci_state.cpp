// mci_state.cpp —— 逐条打印 MCI 状态，定位 play/status 语义
#include <cstdio>
#include <cstring>
#include <windows.h>
#include <mmsystem.h>

static void q(const char* cmd) {
    char buf[256] = {0};
    const MCIERROR e = mciSendStringA(cmd, buf, sizeof(buf), nullptr);
    if (e) {
        char why[256] = {0};
        mciGetErrorStringA(e, why, sizeof(why));
        std::printf("  %-38s -> 错误: %s\n", cmd, why);
    } else {
        std::printf("  %-38s -> [%s]\n", cmd, buf);
    }
}

int main() {
    char buf[256] = {0};
    if (mciSendStringA("open H: type cdaudio alias cd", buf, sizeof(buf), nullptr)) {
        std::printf("open 失败\n"); return 1;
    }
    std::printf("--- open 之后 ---\n");
    q("status cd number of tracks");
    q("status cd length");
    q("status cd mode");
    q("status cd ready");
    q("status cd position");
    std::printf("--- 第 5 轨信息 ---\n");
    q("status cd length track 5");
    q("status cd position track 5");
    std::printf("--- seek 到第 5 轨起点再 play ---\n");
    q("seek cd to 5:00:00");
    q("status cd position");
    q("play cd");
    std::printf("--- play 后 1 秒 ---\n");
    Sleep(1000);
    q("status cd mode");
    q("status cd position");
    q("status cd length");
    std::printf("--- 停 ---\n");
    q("stop cd");
    q("close cd");
    return 0;
}
