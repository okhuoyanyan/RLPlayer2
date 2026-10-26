// cda_toc.cpp —— 用 IOCTL_CDROM_READ_TOC_EX 读光盘 TOC（只读，不出声）
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <windows.h>
#include <winioctl.h>
#include <ntddcdrm.h>

int main() {
    HANDLE h = CreateFileA("\\\\.\\H:", GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) { std::printf("open failed %lu\n", GetLastError()); return 1; }

    CDROM_TOC toc{};
    DWORD ret = 0;
    if (!DeviceIoControl(h, IOCTL_CDROM_READ_TOC, nullptr, 0, &toc, sizeof(toc), &ret, nullptr)) {
        std::printf("READ_TOC failed %lu\n", GetLastError()); CloseHandle(h); return 2;
    }
    const int first = toc.FirstTrack, last = toc.LastTrack;
    std::printf("TOC: 轨道 %d..%d, %lu 字节\n", first, last, (unsigned long)ret);
    struct Trk { int no; uint32_t lba; };
    std::vector<Trk> trks;
    for (int i = 0; i <= last - first + 1 && (3 + i) < 100; ++i) {
        const TRACK_DATA& t = toc.TrackData[i];
        const uint32_t lba = ((uint32_t)t.Address[1] << 8) | t.Address[2];
        const uint32_t msf = ((uint32_t)t.Address[1] << 16) | ((uint32_t)t.Address[2] << 8) | t.Address[3];
        std::printf("  Track %02d  ctrl/adr=0x%02X  LBA=%u (%u:%02u:%02u)\n",
                    t.TrackNumber, t.Control, lba,
                    msf >> 16, (msf >> 8) & 0xFF, msf & 0xFF);
        if (t.TrackNumber == 0xAA) break;
        trks.push_back({t.TrackNumber, lba});
    }
    // 读 .cda 看它的 LBA 字段在哪
    FILE* f = std::fopen("H:\\Track01.cda", "rb");
    if (f) {
        uint8_t hdr[44] = {0};
        std::fread(hdr, 1, 44, f);
        std::fclose(f);
        std::printf("\n.cda 44 字节（十六进制）:\n  ");
        for (int i = 0; i < 44; ++i) std::printf("%02X ", hdr[i]);
        std::printf("\n  0x16(trackNo)=%u  0x18=%u  0x1C(LBA)=%u\n",
                    (unsigned)hdr[0x16],
                    (unsigned)(hdr[0x18] | (hdr[0x19] << 8)),
                    (unsigned)((uint32_t)hdr[0x1C] | ((uint32_t)hdr[0x1D] << 8) |
                               ((uint32_t)hdr[0x1E] << 16) | ((uint32_t)hdr[0x1F] << 24)));
    }
    CloseHandle(h);
    return 0;
}
