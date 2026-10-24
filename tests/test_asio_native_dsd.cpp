// test_asio_native_dsd.cpp — 原生 DSD（ASIO）纯函数门禁：全离线，不打开任何音频设备
//
// 为什么这些用例值得存在（铁律 3：判据本身要先被验证）：
//   1) 采样类型数值曾在本仓库里写错成 21/22/23，而 ASIO SDK 2.3 的 asio.h 是
//      ASIOSTDSDInt8LSB1 = 32 / MSB1 = 33 / NER8 = 40。数值错了，驱动永远不会报那个类型，
//      于是"我们支持原生 DSD"永远不成立，但代码看起来完全正常。
//   2) DSD 的 bufferSize 单位是【样本】(1 bit)，每回调能写的字节 = bufferSize/8。
//      本机 AudioByte 实测报 32768 而双缓冲只有 4096 字节；按 32768 写会直接访问冲突
//      （StackOverflow 52774883 的 "DSD heap corruption" 就是同一个坑）。
//   3) LSB1/MSB1 的位序搞反，声音会变成噪声而不是报错 —— 只能靠位级用例钉死。
//   4) DoP 回落的标记相位必须跨缓冲连续（A,B,A,B…），断了接收端就判非法。
//
// 注意：本测试【不验证】真机出声。真机验证见 tools/asio_probe.cpp 与 rtaplay --native-dsd。
#include "audio/asio_out.h"
#include <cstdio>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>

using namespace rta::audio;

static int g_fail = 0, g_pass = 0;
static void check(bool ok, const char* what, const std::string& detail = std::string()) {
    if (ok) { ++g_pass; printf("  [PASS] %s\n", what); }
    else { ++g_fail; printf("  [FAIL] %s %s\n", what, detail.c_str()); }
}

// 造一个 MSB-first 平面：第 n 个 1-bit 样本放在字节 n>>3 的 bit(7-(n&7))
static std::vector<uint8_t> make_plane(const std::vector<int>& bits) {
    std::vector<uint8_t> b((bits.size() + 7) / 8, 0);
    for (size_t n = 0; n < bits.size(); ++n)
        if (bits[n]) b[n >> 3] = (uint8_t)(b[n >> 3] | (1u << (7 - (n & 7))));
    return b;
}
static int bit_of(const std::vector<uint8_t>& b, size_t n) {
    return (b[n >> 3] >> (7 - (n & 7))) & 1;
}

int main() {
    SetConsoleOutputCP(CP_UTF8);
    printf("=== 原生 DSD（ASIO）纯函数门禁 ===\n");

    // ---------- 1. 采样类型数值必须与 ASIO SDK 一致 ----------
    printf("\n[1] ASIOSampleType 数值（ASIO SDK 2.3 asio.h：LSB1=32 / MSB1=33 / NER8=40）\n");
    check(ASIOSTDSDInt8LSB1 == 32, "ASIOSTDSDInt8LSB1 == 32",
          "实际=" + std::to_string((int)ASIOSTDSDInt8LSB1));
    check(ASIOSTDSDInt8MSB1 == 33, "ASIOSTDSDInt8MSB1 == 33",
          "实际=" + std::to_string((int)ASIOSTDSDInt8MSB1));
    check(ASIOSTDSDInt8NER8 == 40, "ASIOSTDSDInt8NER8 == 40",
          "实际=" + std::to_string((int)ASIOSTDSDInt8NER8));
    // 反向验证：仓库旧版写的 21/22/23 不是 DSD 类型（否则本用例会成为"永远通过"的假门禁）
    check(!asio_is_dsd_1bit(21) && !asio_is_dsd_1bit(22) && !asio_is_dsd_1bit(23),
          "旧版错误数值 21/22/23 不被当作 DSD 类型");
    check(asio_is_dsd_1bit(ASIOSTDSDInt8LSB1) && asio_is_dsd_1bit(ASIOSTDSDInt8MSB1),
          "32/33 被识别为 1-bit DSD");
    check(!asio_is_dsd_1bit(ASIOSTDSDInt8NER8), "NER8(40) 不算 1-bit 打包（1 样本 1 字节，规则不同）");

    // ---------- 2. 每回调字节数 = bufferSize / 8 ----------
    printf("\n[2] bufferSize(单位=1-bit 样本) -> 每回调字节数\n");
    check(asio_dsd_bytes_per_buffer(32768) == 4096, "32768 -> 4096（本机 AudioByte 实测值）",
          "实际=" + std::to_string(asio_dsd_bytes_per_buffer(32768)));
    check(asio_dsd_bytes_per_buffer(1024) == 128, "1024 -> 128");
    check(asio_dsd_bytes_per_buffer(7) == 1, "7 -> 1（向下取整，但至少 1 字节）");
    check(asio_dsd_bytes_per_buffer(0) == 1, "0 -> 1（不返回 0，避免每回调写 0 字节的死循环）");
    // 反向验证：若按"bufferSize 就是字节数"实现（旧直觉），32768 会写成 32768 字节 —— 这正是 AV 的原因
    check(asio_dsd_bytes_per_buffer(32768) != 32768, "不会把 32768 当字节数（那是 AV 的写法）");

    // ---------- 3. 位序：MSB1 直拷 / LSB1 逐字节反序 ----------
    printf("\n[3] 位序转换（LSB1 = 最早样本在 bit0）\n");
    check(asio_reverse_bits8(0x00) == 0x00 && asio_reverse_bits8(0xFF) == 0xFF, "0x00/0xFF 反序不变");
    check(asio_reverse_bits8(0x80) == 0x01, "0x80 -> 0x01（原 bit7 -> bit0）");
    check(asio_reverse_bits8(0x01) == 0x80, "0x01 -> 0x80");
    check(asio_reverse_bits8(0x69) == 0x96, "0x69 -> 0x96");
    bool invol = true;
    for (int v = 0; v < 256; ++v)
        if (asio_reverse_bits8(asio_reverse_bits8((uint8_t)v)) != (uint8_t)v) invol = false;
    check(invol, "全 256 值二次反序还原（对合性）");

    // 关键：给定相同的 1-bit 序列，MSB1 原样写、LSB1 反序写，两种情况下"第 n 个样本"都要落在正确位置
    {
        std::vector<int> seq(16);
        for (int n = 0; n < 16; ++n) seq[(size_t)n] = (n * 5 + 1) & 1;   // 任意非平凡序列
        const std::vector<uint8_t> plane = make_plane(seq);
        bool msbOk = true, lsbOk = true;
        for (int n = 0; n < 16; ++n) {
            const uint8_t msbByte = plane[(size_t)n >> 3];        // MSB1：原样
            const uint8_t lsbByte = asio_reverse_bits8(plane[(size_t)n >> 3]);  // LSB1：反序
            if ((int)((msbByte >> (7 - (n & 7))) & 1) != seq[(size_t)n]) msbOk = false;
            if ((int)((lsbByte >> (n & 7)) & 1) != seq[(size_t)n]) lsbOk = false;
        }
        check(msbOk, "MSB1：样本 n 在 bit(7-(n&7))，原样写出正确");
        check(lsbOk, "LSB1：样本 n 在 bit(n)，逐字节反序后正确");
    }

    // ---------- 4. DoP 回落打包 ----------
    printf("\n[4] DoP 回落打包（标记 A/B 交替 + 16 bit/样本 + 相位跨块连续）\n");
    {
        std::vector<int> bits(32);
        for (int n = 0; n < 32; ++n) bits[(size_t)n] = (n % 3 == 0) ? 1 : 0;
        const std::vector<uint8_t> p0 = make_plane(bits);
        const std::vector<uint8_t> p1 = make_plane(bits);
        std::vector<const uint8_t*> planes = { p0.data(), p1.data() };
        std::vector<int32_t> out;
        std::vector<char> phase;
        asio_dop_pack_planes(planes, 0, 32, out, phase);
        check(out.size() == 4, "32 bit / 16 = 2 承载样本 x 2 声道 = 4");
        const uint32_t m0 = ((uint32_t)out[0] >> 16) & 0xFF;
        const uint32_t m1 = ((uint32_t)out[2] >> 16) & 0xFF;   // 下一帧（同声道）
        check(m0 == 0x05 && m1 == 0xFA, "同声道标记 A(0x05)/B(0xFA) 交替",
              "m0=" + std::to_string(m0) + " m1=" + std::to_string(m1));
        bool payloadOk = true;
        for (int k = 0; k < 2; ++k) {
            const uint32_t payload = (uint32_t)out[(size_t)k * 2] & 0xFFFFu;
            for (int b = 0; b < 16; ++b)
                if ((int)((payload >> (15 - b)) & 1) != bits[(size_t)k * 16 + (size_t)b]) payloadOk = false;
        }
        check(payloadOk, "低 16 位 = 原始 1-bit 序列（MSB-first）");
        // 跨块相位：一块 = 16 bit（1 个承载样本），第二块必须从 B 继续。
        // 注意：上一段用 32 bit（2 个样本）刚好把相位转回 A —— 那是偶数个样本，测不出连续性，
        //       所以这里单独用"奇数个样本"的块。这正是"判据要能真的证明你要证明的事"。
        std::vector<char> ph2;
        std::vector<int32_t> blk1, blk2;
        asio_dop_pack_planes(planes, 0, 16, blk1, ph2);
        asio_dop_pack_planes(planes, 16, 16, blk2, ph2);
        const uint32_t mb1 = ((uint32_t)blk1[0] >> 16) & 0xFF;
        const uint32_t mb2 = ((uint32_t)blk2[0] >> 16) & 0xFF;
        check(mb1 == 0x05 && mb2 == 0xFA, "16 bit 分块：第一块 A(0x05)、第二块 B(0xFA)（相位跨缓冲连续）",
              "mb1=" + std::to_string(mb1) + " mb2=" + std::to_string(mb2));
        // 反向验证：相位若被重置，第二块会以 A 开头 —— 用新相位容器跑一次，确认状态确实是外部维持的
        std::vector<char> fresh;
        std::vector<int32_t> out3;
        asio_dop_pack_planes(planes, 32, 32, out3, fresh);
        check((((uint32_t)out3[0] >> 16) & 0xFF) == 0x05, "全新相位容器从 A 开始（证明上面测的是跨块状态）");
    }

    // ---------- 5. 电平估计（铁律 1 的判据） ----------
    printf("\n[5] DSD 电平估计（去直流短窗峰值，偏保守=安全方向）\n");
    {
        std::vector<uint8_t> zeros(4096, 0x00);
        std::vector<const uint8_t*> zp = { zeros.data(), zeros.data() };
        const AsioDsdLevel z = asio_dsd_level_planes(zp, zeros.size());
        check(z.allZero, "全零平面识别为 allZero");
        check(z.peakDbfs < -200.0, "全零 -> 极低电平（不会被误判成 0 dBFS）",
              "peak=" + std::to_string(z.peakDbfs));

        std::vector<uint8_t> dc(4096, 0xFF);
        std::vector<const uint8_t*> dp = { dc.data(), dc.data() };
        const AsioDsdLevel d = asio_dsd_level_planes(dp, dc.size());
        check(!d.allZero && d.peakDbfs < -200.0, "恒定电平（纯直流）不算音频电平",
              "peak=" + std::to_string(d.peakDbfs));
        check(d.dcDbfs > -1.0, "但直流分量本身被如实报出（dc≈0 dBFS）",
              "dc=" + std::to_string(d.dcDbfs));

        // 满幅方波要在【音频带内】：用 512 字节（≈1.45 ms @ DSD64 ≈ 344 Hz）为半周期的块状交替。
        // 反例（写进注释，避免下一个人再踩）：逐字节交替 0xAA/0x55 是 352 kHz 的方波，
        //   短窗平均后密度恒等于 0.5 —— 那不是"估计器坏了"，那是它正确地忽略了带外信号。
        std::vector<uint8_t> alt(4096);
        for (size_t i = 0; i < alt.size(); ++i) alt[i] = ((i / 512) & 1) ? 0xFF : 0x00;
        std::vector<const uint8_t*> ap = { alt.data(), alt.data() };
        const AsioDsdLevel a = asio_dsd_level_planes(ap, alt.size());
        check(a.peakDbfs > -3.0, "带内满幅方波 -> 接近 0 dBFS（判据能看见可闻素材）",
              "peak=" + std::to_string(a.peakDbfs));

        // 反向对照：逐字节交替（352 kHz，带外）应被判为低电平 —— 证明估计器不是"见交替就报满幅"
        std::vector<uint8_t> oob(4096);
        for (size_t i = 0; i < oob.size(); ++i) oob[i] = (i & 1) ? 0x55 : 0xAA;
        std::vector<const uint8_t*> op = { oob.data(), oob.data() };
        const AsioDsdLevel o = asio_dsd_level_planes(op, oob.size());
        check(o.peakDbfs < -20.0, "逐字节交替（352 kHz 带外）不被算作可闻电平",
              "peak=" + std::to_string(o.peakDbfs));
    }

    printf("\n=== 结果：%d 通过 / %d 失败 ===\n", g_pass, g_fail);
    if (g_fail) {
        printf("⚠️ 本测试是纯函数门禁：它只能证明打包规则/数值/相位是对的，\n"
               "   证明不了真机出声（真机见 asio_probe --dsd-create 与 rtaplay --native-dsd）。\n");
        return 1;
    }
    return 0;
}
