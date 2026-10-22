// test_zipstore.cpp — 最小 ZIP 打包的结构回归（不起外部解压器，纯内存反解）
//
// 为什么这条判据能证明"ZIP 是对的"（铁律 3）：
//   * 我们自己 parse() 回读，核对 名字/长度/CRC32/STORE 一致性 —— 能挡住
//     "签名写错、长度字段写漏、CRC 算错" 这类真错。
//   * 但 parse() 是我自己写的，跟 build() 共享同一个误解 ⟹ 还不够。
//     所以另加两条【独立】判据：
//       ① 结构常量检查：本地头 PK\3\4 / 中央目录 PK\1\2 / EOCD PK\5\6 的
//          魔数与字段偏移按 PKWARE 规范硬核对（不是"跟我自己一致"）；
//       ② 真实解压：分析文档里贴 Expand-Archive 的实测输出（外部判据）。
#include "net/zipstore.h"
#include <cstdio>
#include <string>
#include <vector>

#ifdef _WIN32
extern "C" __declspec(dllimport) int __stdcall SetConsoleOutputCP(unsigned int);
#endif
#ifndef CP_UTF8
#define CP_UTF8 65001
#endif

static int g_fail = 0;
static void ck(bool cond, const char* what, const std::string& detail = "") {
    if (cond) std::printf("  [PASS] %s\n", what);
    else { std::printf("  [FAIL] %s  %s\n", what, detail.c_str()); ++g_fail; }
}

static uint16_t rd16(const std::string& s, size_t p) {
    return (uint16_t)((unsigned char)s[p] | ((unsigned char)s[p + 1] << 8));
}
static uint32_t rd32(const std::string& s, size_t p) {
    return (uint32_t)(unsigned char)s[p] | ((uint32_t)(unsigned char)s[p + 1] << 8) |
           ((uint32_t)(unsigned char)s[p + 2] << 16) | ((uint32_t)(unsigned char)s[p + 3] << 24);
}

int main() {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    std::printf("== zipstore：最小 ZIP（STORE）结构 ==\n");

    // 参考值：CRC32("123456789") = 0xCBF43926（PKWARE / IEEE 标准测试向量）
    {
        const char* v = "123456789";
        const uint32_t got = rta::zip::crc32_update(0, v, 9);
        ck(got == 0xCBF43926u, "CRC32 标准测试向量 \"123456789\" == 0xCBF43926",
           "got=0x" + std::to_string((unsigned long long)got));
        ck(rta::zip::crc32_update(0, "", 0) == 0u, "空数据 CRC32 == 0");
    }

    // 中英文文件名 + 空文件 + 二进制内容都放进去
    std::vector<rta::zip::Entry> items;
    items.push_back({"rlplayer-diag.json", "{\"ok\":true,\"ver\":\"1.0.7\"}", 0, 0});
    items.push_back({"rlplayer-playback.log", "line1\nline2\n", 0, 0});
    items.push_back({"launcher.log", "", 0, 0});                     // 空文件
    items.push_back({"dumps/说明.txt", "中文内容 test", 0, 0});        // 子目录 + UTF-8

    const std::string zip = rta::zip::build(items);
    ck(zip.size() > 100, "生成了非空 ZIP", "size=" + std::to_string(zip.size()));

    // ---- ① 规范常量硬核对（不依赖自己的 parse）----
    {
        ck(rd32(zip, 0) == 0x04034B50u, "本地头签名 PK\\3\\4");
        const uint16_t method = rd16(zip, 8);
        ck(method == 0, "压缩方法 = 0 (STORE)", "got=" + std::to_string(method));
        const uint16_t flags = rd16(zip, 6);
        ck((flags & 0x0800) != 0, "标志位 bit11 = UTF-8 文件名");
        const uint32_t crc0 = rd32(zip, 14);
        ck(crc0 == rta::zip::crc32_update(0, items[0].data.data(), items[0].data.size()),
           "第一个条目的 CRC 与内容一致");
        const uint32_t csize = rd32(zip, 18), usize = rd32(zip, 22);
        ck(csize == usize && csize == items[0].data.size(), "STORE：压缩后大小 == 原大小");
        const uint16_t nlen = rd16(zip, 26);
        ck(nlen == items[0].name.size(), "文件名长度字段正确",
           "decl=" + std::to_string(nlen) + " real=" + std::to_string(items[0].name.size()));
        // EOCD 在最后 22 字节（无注释）
        const size_t eocd = zip.size() - 22;
        ck(rd32(zip, eocd) == 0x06054B50u, "EOCD 签名 PK\\5\\6 位于末尾");
        ck(rd16(zip, eocd + 10) == items.size(), "EOCD 条目数正确");
        const uint32_t cdStart = rd32(zip, eocd + 16);
        ck(rd32(zip, cdStart) == 0x02014B50u, "中央目录签名 PK\\1\\2 位于 EOCD 指示的偏移");
    }

    // ---- ② 自反解：名字/大小/CRC/内容逐条核对 ----
    {
        std::vector<rta::zip::Parsed> back;
        std::string err;
        const bool ok = rta::zip::parse(zip, back, err);
        ck(ok, "自反解成功", err);
        ck(back.size() == items.size(), "条目数一致",
           std::to_string(back.size()) + " vs " + std::to_string(items.size()));
        if (ok && back.size() == items.size()) {
            bool allName = true, allData = true, allCrc = true;
            for (size_t i = 0; i < items.size(); ++i) {
                if (back[i].name != items[i].name) allName = false;
                if (back[i].data != items[i].data) allData = false;
                if (back[i].crc != rta::zip::crc32_update(0, items[i].data.data(), items[i].data.size()))
                    allCrc = false;
            }
            ck(allName, "条目名逐条一致（含中文与子目录路径）");
            ck(allData, "条目内容逐条一致（含空文件）");
            ck(allCrc, "条目 CRC 逐条一致");
        }
    }

    // ---- ③ 破坏性判据：CRC 被改一个字节必须被检出 ----
    {
        std::string bad = zip;
        // 第一个条目数据区起始 = 30 + nlen + elen
        const uint16_t nlen = rd16(bad, 26);
        const size_t dataAt = 30 + nlen;
        bad[dataAt] = (char)(bad[dataAt] ^ 0xFF);
        std::vector<rta::zip::Parsed> back;
        std::string err;
        const bool ok = rta::zip::parse(bad, back, err);
        ck(!ok, "篡改数据后 CRC 校验必须失败（判据不是永远返回 true）", "err=" + err);
    }

    // ---- ④ 空包（0 条目）也是合法 ZIP ----
    {
        const std::string empty = rta::zip::build({});
        ck(empty.size() == 22, "空包 = 22 字节纯 EOCD", "size=" + std::to_string(empty.size()));
        std::vector<rta::zip::Parsed> back;
        std::string err;
        ck(rta::zip::parse(empty, back, err) && back.empty(), "空包可反解且 0 条目", err);
    }

    if (g_fail == 0) std::printf("== 全部通过 ==\n");
    else std::printf("== 失败 %d 项 ==\n", g_fail);
    return g_fail ? 1 : 0;
}
