// test_dop_stream.cpp — 流式 DoP（分块调制 + 打包 + 跨块标记相位）验证
//
// 为什么需要这个测试：网页播放器是"分块渲染"的，DoP 的两个坑只有在分块下才暴露：
//   1) 标记必须在 0x05/0xFA 之间【连续交替】，每块重新从 A 开始会让接收端判定非法
//   2) 每块的 DSD 样本数不一定能被 16 整除，余数必须留到下一块
// 这里直接复刻播放器生产者线程的做法（调制 -> 打包 -> 残余），再解包验证。
#include "dsp/dop.h"
#include "dsp/modulator.h"
#include <cstdio>
#include <cmath>
#include <vector>
#include <memory>
#include <string>

using namespace rta::dsp;
static int fails = 0;
static void check(bool ok, const std::string& what, const std::string& detail = "") {
    printf("  [%s] %s %s\n", ok ? "PASS" : "FAIL", what.c_str(), detail.c_str());
    if (!ok) ++fails;
}
static const double PI = 3.14159265358979323846;

// 复刻 rtaweb 生产者：把一段 PCM 按 chunk 分块，调制 -> 打包 -> 残余
static std::vector<int32_t> stream_pack(const std::vector<double>& pcm, size_t chunk,
                                        int dsdMult, bool* markerOk) {
    const uint32_t fs = dsd_rate(dsdMult);
    SdmConfig cfg; cfg.fs = (double)fs; cfg.order = 5; cfg.levels = 2;
    cfg.cutoffHz = 100000.0; cfg.ditherLsb = 0.01;
    SdmModulator mod(cfg);
    DopPackState st;
    std::vector<double> carry;
    std::vector<int32_t> out;
    for (size_t pos = 0; pos < pcm.size(); pos += chunk) {
        const size_t n = std::min(chunk, pcm.size() - pos);
        std::vector<double> sd = carry;
        sd.reserve(carry.size() + n);
        for (size_t i = 0; i < n; ++i) sd.push_back(mod.process(pcm[pos + i]));
        const size_t use = (sd.size() / 16) * 16;
        const double* sp[1] = { sd.data() };
        std::vector<int32_t> packed;
        dop_pack(sp, 1, use, packed, st);
        out.insert(out.end(), packed.begin(), packed.end());
        carry.assign(sd.begin() + (long long)use, sd.end());
    }
    *markerOk = true;
    return out;
}

int main() {
    printf("=== 流式 DoP 验证 ===\n");

    const int mult = 64;
    const size_t N = 40000;                 // 输入 PCM 样本数
    std::vector<double> pcm(N);
    for (size_t n = 0; n < N; ++n)
        pcm[n] = 0.5 * std::sin(2.0 * PI * 997.0 * (double)n / 44100.0);

    // 1) 整块（参考）
    bool okRef = false;
    std::vector<int32_t> ref = stream_pack(pcm, pcm.size(), mult, &okRef);
    std::vector<std::vector<double>> bitsRef;
    const bool legalRef = dop_unpack(ref.data(), 1, ref.size(), bitsRef);
    printf("  整块：%zu 个载波帧，标记合法=%d\n", ref.size(), (int)legalRef);
    check(legalRef, "整块打包的 DoP 标记合法");

    // 2) 分块（含不能整除 16 的块长）
    for (size_t chunk : { (size_t)1000, (size_t)777, (size_t)4099, (size_t)64 }) {
        bool ok = false;
        std::vector<int32_t> got = stream_pack(pcm, chunk, mult, &ok);
        std::vector<std::vector<double>> bits;
        const bool legal = dop_unpack(got.data(), 1, got.size(), bits);
        printf("  块长 %zu：%zu 个载波帧，标记合法=%d\n", chunk, got.size(), (int)legal);
        check(legal, "分块打包的 DoP 标记仍然合法（跨块交替正确）", std::to_string(chunk));
        check(got.size() == ref.size(), "载波帧数与整块一致", std::to_string(chunk));
        size_t diff = 0;
        const size_t m = std::min(got.size(), ref.size());
        for (size_t i = 0; i < m; ++i) if (got[i] != ref[i]) ++diff;
        check(diff == 0, "与整块打包结果逐位一致", std::to_string(diff) + " 个不同");
    }

    // 3) 解包后的 1-bit 流前 4096 位应与"不打包直接取符号"一致
    {
        SdmConfig cfg; cfg.fs = (double)dsd_rate(mult); cfg.order = 5; cfg.levels = 2;
        cfg.cutoffHz = 100000.0; cfg.ditherLsb = 0.01;
        SdmModulator mod(cfg);
        std::vector<double> direct;
        for (size_t n = 0; n < N; ++n) direct.push_back(mod.process(pcm[n]));
        size_t diff = 0;
        for (size_t i = 0; i < std::min(bitsRef[0].size(), direct.size()); ++i)
            if (bitsRef[0][i] != direct[i]) ++diff;
        check(diff == 0, "解包还原的 1-bit 流与调制器输出一致", std::to_string(diff) + " 位不同");
    }

    // 4) 48k 家族的速率换算
    check(dsd_rate(128, 48000) == 6144000u, "48k 家族 DSD128 = 6.144 MHz",
          std::to_string(dsd_rate(128, 48000)));
    check(dop_carrier_rate(64, 48000) == 192000u, "48k 家族 DSD64 载波 = 192 kHz",
          std::to_string(dop_carrier_rate(64, 48000)));

    printf("\n%s (失败项 %d)\n", fails == 0 ? "全部通过" : "存在失败", fails);
    return fails == 0 ? 0 : 1;
}
