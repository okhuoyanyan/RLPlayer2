// cda_spti.cpp —— 用 SCSI Pass-Through 读 CDDA 原始扇区（标准抓轨方式）
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <windows.h>
#include <winioctl.h>
#include <ntddcdrm.h>
#include <ntddscsi.h>

static uint32_t msfToLba(const UCHAR* a) {
    return ((uint32_t)a[1] * 60 + a[2]) * 75 + a[3];
}

// 用 READ CD (0xBE) 读 [lba, lba+sectors) 的原始音频（2352 B/扇区）
static bool readCdda(HANDLE h, uint32_t lba, uint16_t sectors, std::vector<uint8_t>& out) {
    out.assign((size_t)sectors * 2352, 0);
    std::vector<uint8_t> cdb(12, 0);
    cdb[0] = 0xBE;                       // READ CD
    cdb[1] = 0x00;                       // 期望扇区类型 = 全部
    cdb[2] = (uint8_t)(lba >> 24);
    cdb[3] = (uint8_t)(lba >> 16);
    cdb[4] = (uint8_t)(lba >> 8);
    cdb[5] = (uint8_t)(lba);
    cdb[6] = (uint8_t)(sectors >> 16);
    cdb[7] = (uint8_t)(sectors >> 8);
    cdb[8] = (uint8_t)(sectors);
    cdb[9] = 0xF8;                       // 含 sync/header/subheader + 2352 B
    cdb[10] = 0x00;
    cdb[11] = 0x00;

    const size_t dataLen = out.size();
    std::vector<uint8_t> buf(sizeof(SCSI_PASS_THROUGH_DIRECT) + dataLen);
    auto* spt = reinterpret_cast<SCSI_PASS_THROUGH_DIRECT*>(buf.data());
    std::memset(spt, 0, sizeof(SCSI_PASS_THROUGH_DIRECT));
    spt->Length             = sizeof(SCSI_PASS_THROUGH_DIRECT);
    spt->CdbLength          = 12;
    spt->DataTransferLength = (ULONG)dataLen;
    spt->DataBuffer         = out.data();
    spt->TimeOutValue       = 10;
    std::memcpy(spt->Cdb, cdb.data(), 12);

    DWORD ret = 0;
    const BOOL dio = DeviceIoControl(h, IOCTL_SCSI_PASS_THROUGH_DIRECT, spt, (DWORD)buf.size(),
                                     spt, (DWORD)buf.size(), &ret, nullptr);
    if (!dio) {
        std::printf("    DeviceIoControl 失败 err=%lu\n", GetLastError());
        return false;
    }
    if (spt->ScsiStatus != 0) {
        std::printf("    SCSI 状态=0x%02X  transferred=%lu\n",
                    spt->ScsiStatus, (unsigned long)spt->DataTransferLength);
        return false;
    }
    if (spt->DataTransferLength != (ULONG)dataLen)
        std::printf("    注意：只传回 %lu / %zu 字节\n", (unsigned long)spt->DataTransferLength, dataLen);
    return true;
}

int main(int argc, char** argv) {
    const char* letter = (argc > 1) ? argv[1] : "H";
    const std::string dev = std::string("\\\\.\\") + letter + ":";
    HANDLE h = CreateFileA(dev.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        const DWORD e = GetLastError();
        std::printf("open [%s] failed %lu\n", dev.c_str(), e);
        // 退一步：只读 + 全部共享标志再试一次
        h = CreateFileA(dev.c_str(), 0,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                        OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) { std::printf("  重试(access=0) 仍失败 %lu\n", GetLastError()); return 1; }
        std::printf("  重试(access=0) 成功\n");
    }

    CDROM_TOC toc{};
    DWORD ret = 0;
    DeviceIoControl(h, IOCTL_CDROM_READ_TOC, nullptr, 0, &toc, sizeof(toc), &ret, nullptr);
    std::vector<uint32_t> b;
    for (int i = 0; i < 100; ++i) {
        const TRACK_DATA& t = toc.TrackData[i];
        if (!t.TrackNumber) break;
        b.push_back(msfToLba(t.Address));
        if (t.TrackNumber == 0xAA) break;
    }
    std::printf("轨道边界: ");
    for (auto v : b) std::printf("%u ", v);
    std::printf("\n");

    // 读 Track01 前 8 个扇区
    std::vector<uint8_t> pcm;
    const bool ok = readCdda(h, b[0], 8, pcm);
    std::printf("READ CD 8 扇区 -> %s\n", ok ? "成功" : "失败");
    if (ok) {
        long long nz = 0; int peak = 0;
        for (size_t i = 0; i + 1 < pcm.size(); i += 2) {
            const int16_t s = (int16_t)((uint16_t)pcm[i] | ((uint16_t)pcm[i + 1] << 8));
            if (s) ++nz;
            const int a = s < 0 ? -s : s;
            if (a > peak) peak = a;
        }
        std::printf("字节 %zu，非零 %lld，峰值 %d (%.1f dBFS)\n", pcm.size(), nz, peak,
                    peak ? 20.0 * std::log10(peak / 32768.0) : -999.0);
        std::printf("前 8 样点: ");
        for (int i = 0; i < 16; i += 2)
            std::printf("%d ", (int)(int16_t)((uint16_t)pcm[i] | ((uint16_t)pcm[i+1] << 8)));
        std::printf("\n");
        // 轨道时长估算
        if (b.size() >= 2) {
            const double secs = (double)(b[1] - b[0]) / 75.0;
            std::printf("Track01 时长约 %.1f 秒\n", secs);
        }
    }
    CloseHandle(h);
    return 0;
}
