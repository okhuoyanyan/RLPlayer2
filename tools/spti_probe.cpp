// spti_probe.cpp —— 用最简单的 SCSI 命令逐级定位（INQUIRY / READ CAPACITY / READ TOC via SPTI / READ CD）
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <windows.h>
#include <winioctl.h>
#include <ntddscsi.h>
#include <ntddcdrm.h>

static bool spti(HANDLE h, std::vector<uint8_t> cdb, void* data, size_t dataLen,
                 std::string& why, UCHAR& status) {
    std::vector<uint8_t> buf(sizeof(SCSI_PASS_THROUGH_DIRECT) + dataLen);
    auto* spt = reinterpret_cast<SCSI_PASS_THROUGH_DIRECT*>(buf.data());
    std::memset(spt, 0, sizeof(SCSI_PASS_THROUGH_DIRECT));
    spt->Length = sizeof(SCSI_PASS_THROUGH_DIRECT);
    spt->CdbLength = (UCHAR)cdb.size();
    spt->DataTransferLength = (ULONG)dataLen;
    spt->DataBuffer = data;
    spt->TimeOutValue = 15;
    std::memcpy(spt->Cdb, cdb.data(), cdb.size());
    DWORD ret = 0;
    if (!DeviceIoControl(h, IOCTL_SCSI_PASS_THROUGH_DIRECT, spt, (DWORD)buf.size(),
                         spt, (DWORD)buf.size(), &ret, nullptr)) {
        why = "DeviceIoControl err=" + std::to_string(GetLastError());
        return false;
    }
    status = spt->ScsiStatus;
    if (spt->ScsiStatus != 0) {
        why = "SCSI status=0x" + std::to_string(spt->ScsiStatus) +
              " transferred=" + std::to_string(spt->DataTransferLength);
        return false;
    }
    why = "ok transferred=" + std::to_string(spt->DataTransferLength);
    return true;
}

int main(int argc, char** argv) {
    const char* letter = (argc > 1) ? argv[1] : "H";
    const std::string dev = std::string("\\\\.\\") + letter + ":";
    HANDLE h = CreateFileA(dev.c_str(), GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) { std::printf("open 失败 %lu\n", GetLastError()); return 1; }

    std::string why; UCHAR st = 0;

    // 1) INQUIRY (0x12) —— 最基础，能答就说明 SPTI 通路正常
    {
        std::vector<uint8_t> cdb(6, 0), data(96, 0);
        cdb[0] = 0x12; cdb[4] = (uint8_t)data.size();
        const bool ok = spti(h, cdb, data.data(), data.size(), why, st);
        std::printf("1) INQUIRY       -> %s  [%s]\n", ok ? "成功" : "失败", why.c_str());
        if (ok) {
            char vendor[9] = {0}, product[17] = {0};
            std::memcpy(vendor, data.data() + 8, 8);
            std::memcpy(product, data.data() + 16, 16);
            std::printf("     vendor=[%s] product=[%s]\n", vendor, product);
        }
    }
    // 2) READ TOC/PMA/ATIP via SPTI (0x43) —— 走 SPTI 的 TOC
    {
        std::vector<uint8_t> cdb(10, 0), data(804, 0);
        cdb[0] = 0x43; cdb[1] = 0x00; cdb[7] = 0x03; cdb[8] = (uint8_t)(data.size() >> 8);
        cdb[9] = (uint8_t)(data.size() & 0xFF);
        const bool ok = spti(h, cdb, data.data(), data.size(), why, st);
        std::printf("2) READ TOC(S)   -> %s  [%s]\n", ok ? "成功" : "失败", why.c_str());
        if (ok) std::printf("     首轨=%u 末轨=%u\n", data[2], data[3]);
    }
    // 3) READ CD (0xBE) 读 1 个扇区
    {
        std::vector<uint8_t> cdb(12, 0), data(2352, 0);
        cdb[0] = 0xBE; cdb[1] = 0x00;
        cdb[2] = 0; cdb[3] = 0; cdb[4] = 0; cdb[5] = 150;   // LBA 150
        cdb[6] = 0; cdb[7] = 0; cdb[8] = 1;                 // 1 扇区
        cdb[9] = 0xF8;
        const bool ok = spti(h, cdb, data.data(), data.size(), why, st);
        std::printf("3) READ CD x1    -> %s  [%s]\n", ok ? "成功" : "失败", why.c_str());
        if (ok) {
            int peak = 0; long long nz = 0;
            for (size_t i = 0; i + 1 < data.size(); i += 2) {
                const int16_t s = (int16_t)((uint16_t)data[i] | ((uint16_t)data[i+1] << 8));
                if (s) ++nz;
                const int a = s < 0 ? -s : s;
                if (a > peak) peak = a;
            }
            std::printf("     非零 %lld / %zu  峰值 %d (%.1f dBFS)\n", nz, data.size()/2, peak,
                        peak ? 20.0 * std::log10(peak / 32768.0) : -999.0);
        }
    }
    // 4) 再用 IOCTL_CDROM_READ_TOC 确认设备仍在线
    {
        CDROM_TOC toc{}; DWORD ret = 0;
        const BOOL ok = DeviceIoControl(h, IOCTL_CDROM_READ_TOC, nullptr, 0, &toc, sizeof(toc), &ret, nullptr);
        std::printf("4) IOCTL TOC     -> %s  err=%lu\n", ok ? "成功" : "失败", ok ? 0UL : GetLastError());
    }
    CloseHandle(h);
    return 0;
}
