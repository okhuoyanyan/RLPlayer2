// cda_probe.cpp —— 验证能否从光驱原始读取 CDDA 音轨（.cda 只是 44 字节索引）
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

int main(int argc, char** argv) {
    const char* cdaPath = (argc > 1) ? argv[1] : "H:\\Track01.cda";
    // 1) 读 .cda 44 字节，取起始 LBA（偏移 0x0A，小端 32 位）
    FILE* f = std::fopen(cdaPath, "rb");
    if (!f) { std::printf("打不开 %s\n", cdaPath); return 1; }
    uint8_t hdr[44] = {0};
    const size_t got = std::fread(hdr, 1, 44, f);
    std::fclose(f);
    std::printf(".cda 读到 %zu 字节\n", got);
    std::printf("  magic=\"%.5s\"\n", (const char*)hdr);
    uint32_t lba = (uint32_t)hdr[0x0A] | ((uint32_t)hdr[0x0B] << 8) |
                   ((uint32_t)hdr[0x0C] << 16) | ((uint32_t)hdr[0x0D] << 24);
    std::printf("  起始 LBA = %u (%.2f 秒)\n", lba, (double)lba / 75.0);

#ifdef _WIN32
    // 2) 原始打开光驱
    const std::string dev = "\\\\.\\H:";
    HANDLE h = CreateFileA(dev.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        std::printf("CreateFile(%s) 失败: %lu\n", dev.c_str(), GetLastError());
        return 2;
    }
    std::printf("原始设备打开成功\n");
    // 3) CDDA 扇区 2352 字节；Win32 对 CDFS 的 SetFilePointer 以 2048 为单位计数，
    //    所以偏移要写 lba*2048，但每次读 2352（读满一个原始扇区）。
    LARGE_INTEGER off;
    off.QuadPart = (LONGLONG)lba * 2048;
    if (!SetFilePointerEx(h, off, nullptr, FILE_BEGIN)) {
        std::printf("SetFilePointerEx 失败: %lu\n", GetLastError());
        CloseHandle(h); return 3;
    }
    std::vector<uint8_t> buf(2352 * 8);
    DWORD readBytes = 0;
    if (!ReadFile(h, buf.data(), (DWORD)buf.size(), &readBytes, nullptr)) {
        std::printf("ReadFile 失败: %lu\n", GetLastError());
        CloseHandle(h); return 4;
    }
    std::printf("读到 %lu 字节（%u 个 CDDA 扇区）\n", readBytes, readBytes / 2352);
    // 4) 判断是不是真音频（不是全零/不是错误填充）
    long long nonzero = 0;
    int peak = 0;
    for (DWORD i = 0; i + 1 < readBytes; i += 2) {
        const int16_t s = (int16_t)((uint16_t)buf[i] | ((uint16_t)buf[i + 1] << 8));
        if (s != 0) ++nonzero;
        const int a = s < 0 ? -s : s;
        if (a > peak) peak = a;
    }
    std::printf("非零样点 %lld / %lu，峰值 %d (%.1f dBFS)\n",
                nonzero, (unsigned long)(readBytes / 2), peak,
                peak > 0 ? 20.0 * std::log10(peak / 32768.0) : -999.0);
    std::printf("前 8 个样点: ");
    for (int i = 0; i < 16; i += 2) {
        std::printf("%d ", (int)(int16_t)((uint16_t)buf[i] | ((uint16_t)buf[i+1] << 8)));
    }
    std::printf("\n");
    CloseHandle(h);
#endif
    return 0;
}
