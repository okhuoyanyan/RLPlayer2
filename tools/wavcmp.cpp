// wavcmp.cpp — 逐位比对两个 WAV 文件
// 用途：证明"比特完美"主张（源文件 vs 经过完整链路后的输出）
// 用法：wavcmp <a.wav> <b.wav>
// 退出码：0 = 逐位相同；1 = 有差异；2 = 无法读取
#include "audio/wav.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace rta::audio;

static std::vector<uint8_t> raw_bytes(const AudioBuffer& a) {
    const int bytesPer = a.bits / 8;
    std::vector<uint8_t> o(a.raw.size() * (size_t)bytesPer);
    for (size_t i = 0; i < a.raw.size(); ++i) {
        const uint32_t v = (uint32_t)a.raw[i];
        uint8_t* p = o.data() + i * (size_t)bytesPer;
        if (bytesPer == 2) { p[0] = (uint8_t)(v & 0xFF); p[1] = (uint8_t)((v >> 8) & 0xFF); }
        else if (bytesPer == 3) { p[0] = (uint8_t)(v & 0xFF); p[1] = (uint8_t)((v >> 8) & 0xFF); p[2] = (uint8_t)((v >> 16) & 0xFF); }
        else { p[0] = (uint8_t)(v & 0xFF); p[1] = (uint8_t)((v >> 8) & 0xFF); p[2] = (uint8_t)((v >> 16) & 0xFF); p[3] = (uint8_t)((v >> 24) & 0xFF); }
    }
    return o;
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    SetConsoleOutputCP(CP_UTF8);
    // --prefix：只要求 B 的**前缀**与 A 逐位相同，允许 B 更长。
    // 用于 DoP 直通：播放器在末尾最多补一个周期的合法静音帧，长度会比源多一点点。
    bool prefix = false;
    int argi = 1;
    for (; argi + 2 < argc + 0 && argi < argc; ++argi)
        if (std::strcmp(argv[argi], "--prefix") == 0) { prefix = true; break; }
    std::vector<const char*> pos;
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "--prefix") != 0) pos.push_back(argv[i]);
    if (pos.size() < 2) { printf("用法: wavcmp <a.wav> <b.wav> [--prefix]\n"); return 2; }

    AudioBuffer a, b;
    try { a = read_wav(pos[0]); } catch (const std::exception& e) { printf("读取 %s 失败: %s\n", pos[0], e.what()); return 2; }
    try { b = read_wav(pos[1]); } catch (const std::exception& e) { printf("读取 %s 失败: %s\n", pos[1], e.what()); return 2; }

    printf("A: %u Hz %d ch %d bit %zu 帧\n", a.rate, a.channels, a.bits, a.frames());
    printf("B: %u Hz %d ch %d bit %zu 帧\n", b.rate, b.channels, b.bits, b.frames());

    if (a.rate != b.rate || a.channels != b.channels || a.bits != b.bits) {
        printf("格式不同 —— 无法逐位比对\n");
        return 2;
    }
    auto ba = raw_bytes(a), bb = raw_bytes(b);
    const size_t n = std::min(ba.size(), bb.size());
    size_t diff = 0; size_t firstAt = (size_t)-1;
    for (size_t i = 0; i < n; ++i) {
        if (ba[i] != bb[i]) { ++diff; if (firstAt == (size_t)-1) firstAt = i; }
    }
    const size_t tail = (ba.size() > bb.size() ? ba.size() - bb.size() : bb.size() - ba.size());

    printf("字节数: A=%zu B=%zu (长度差 %zu)\n", ba.size(), bb.size(), tail);
    if (prefix) {
        if (bb.size() < ba.size()) {
            printf("\n❌ B 比 A 短，无法做前缀比对\n");
            return 1;
        }
        if (diff == 0) {
            printf("\n✅ 前缀逐位相同（A 的 %zu 字节全部一致；B 多出 %zu 字节）\n",
                   ba.size(), bb.size() - ba.size());
            return 0;
        }
        printf("\n❌ 前缀不一致: %zu 字节不同\n", diff);
        return 1;
    }
    if (diff == 0 && tail == 0) {
        printf("\n✅ 逐位完全相同 (%zu 字节, %zu 样本)\n", ba.size(), a.raw.size());
        return 0;
    }
    printf("\n❌ 不一致: %zu 字节不同", diff);
    if (firstAt != (size_t)-1) printf(", 首个差异在第 %zu 字节 (样本 %zu)", firstAt, firstAt / (size_t)(a.bits / 8));
    printf("\n");
    return 1;
}
