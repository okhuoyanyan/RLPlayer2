// cda_read.cpp —— TOC 拿轨道位置 + 原始扇区读取验证
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <windows.h>
#include <winioctl.h>
#include <ntddcdrm.h>

static uint32_t msfToLba(const UCHAR* a) {
    const uint32_t m = a[1], s = a[2], f = a[3];
    return (m * 60 + s) * 75 + f;      // 绝对 LBA（含 150 扇区前导）
}

int main(int argc, char** argv) {
    const char* letter = (argc > 1) ? argv[1] : "H";
    const std::string dev = std::string("\\\\.\\") + letter + ":";
    HANDLE h = CreateFileA(dev.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) { std::printf("open %s failed %lu\n", dev.c_str(), GetLastError()); return 1; }

    CDROM_TOC toc{};
    DWORD ret = 0;
    if (!DeviceIoControl(h, IOCTL_CDROM_READ_TOC, nullptr, 0, &toc, sizeof(toc), &ret, nullptr)) {
        std::printf("READ_TOC failed %lu\n", GetLastError()); CloseHandle(h); return 2;
    }
    std::vector<uint32_t> lba;
    for (int i = 0; i < 100; ++i) {
        const TRACK_DATA& t = toc.TrackData[i];
        if (t.TrackNumber == 0) break;
        lba.push_back(msfToLba(t.Address));
        if (t.TrackNumber == 0xAA) break;
    }
    std::printf("轨道边界 LBA：");
    for (size_t i = 0; i < lba.size(); ++i) std::printf("%u ", lba[i]);
    std::printf("\n共 %zu 个边界（含 lead-out）\n", lba.size());

    // 读 Track01 开头 4 个扇区（2352 字节/扇区）
    const uint32_t start = lba[0];
    LARGE_INTEGER off; off.QuadPart = (LONGLONG)start * 2048;
    SetFilePointerEx(h, off, nullptr, FILE_BEGIN);
    std::vector<uint8_t> buf(2352 * 4);
    DWORD got = 0;
    const BOOL okRead = ReadFile(h, buf.data(), (DWORD)buf.size(), &got, nullptr);
    std::printf("\nReadFile(LBA=%u, off=%lld) -> ok=%d err=%lu got=%lu\n",
                start, (long long)off.QuadPart, (int)okRead, okRead ? 0UL : GetLastError(), got);
    if (okRead && got > 0) {
        long long nz = 0; int peak = 0;
        for (DWORD i = 0; i + 1 < got; i += 2) {
            const int16_t s = (int16_t)((uint16_t)buf[i] | ((uint16_t)buf[i+1] << 8));
            if (s) ++nz;
            const int a = s < 0 ? -s : s;
            if (a > peak) peak = a;
        }
        std::printf("非零 %lld/%lu 峰值 %d (%.1f dBFS)\n", nz, (unsigned long)(got/2), peak,
                    peak ? 20.0 * std::log10(peak / 32768.0) : -999.0);
        std::printf("前 8 样点: ");
        for (int i = 0; i < 16; i += 2)
            std::printf("%d ", (int)(int16_t)((uint16_t)buf[i] | ((uint16_t)buf[i+1] << 8)));
        std::printf("\n");
    }
    CloseHandle(h);
    return 0;
}
