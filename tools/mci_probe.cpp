// mci_probe.cpp —— 只用 MCI 探测 CD（不开 SPTI、不发原生 SCSI 命令）
#include <cstdio>
#include <cstring>
#include <windows.h>
#include <mmsystem.h>

static bool mci(const char* cmd, char* out, DWORD outLen) {
    const MCIERROR e = mciSendStringA(cmd, out, outLen, nullptr);
    if (e != 0) {
        char buf[256] = {0};
        mciGetErrorStringA(e, buf, sizeof(buf));
        std::printf("  MCI[%s] 失败: %s\n", cmd, buf);
        return false;
    }
    return true;
}

int main(int argc, char** argv) {
    const char* letter = (argc > 1) ? argv[1] : "H";
    char buf[512] = {0};

    // 1) 开 cdaudio 设备（MCI 自己跟驱动打交道，不经过我们的 SPTI）
    char cmd[128];
    std::snprintf(cmd, sizeof(cmd), "open %s: type cdaudio alias cd1", letter);
    if (!mci(cmd, buf, sizeof(buf))) {
        std::printf("打不开 CD 音频设备（可能无碟或设备不支持 MCI cdaudio）\n");
        return 1;
    }
    std::printf("1) open cdaudio 成功\n");

    // 2) 轨道数 / 总长
    if (mci("status cd1 number of tracks", buf, sizeof(buf))) std::printf("2) 轨道数 = %s\n", buf);
    if (mci("status cd1 length", buf, sizeof(buf)))              std::printf("3) 总长   = %s ms\n", buf);
    if (mci("status cd1 mode", buf, sizeof(buf)))                std::printf("4) 状态   = %s\n", buf);
    // 3) 第 1 轨长度
    if (mci("status cd1 length track 1", buf, sizeof(buf)))      std::printf("5) 第1轨长= %s ms\n", buf);

    // 4) 只做 seek 测试（不实际出声，只验证定位能力）
    if (mci("seek cd1 to start", buf, sizeof(buf)))              std::printf("6) seek to start 成功\n");
    if (mci("status cd1 position", buf, sizeof(buf)))            std::printf("7) 位置   = %s\n", buf);

    mci("close cd1", buf, sizeof(buf));
    std::printf("已 close（未播放任何声音）\n");
    return 0;
}
