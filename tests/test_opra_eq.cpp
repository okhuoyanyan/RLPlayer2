// test_opra_eq.cpp — OPRA 耳机 EQ 适配离线单测（入 ctest）
//
// 判据体系（铁律 3、铁律 7）：
//   1. 3 款真实 OPRA 样本（HD600 / AirPods Pro 2 / WH-1000XM5）：
//      解析 -> 转 AutoEq 文本 -> 再解析 -> 逐频点比较解析幅频响应，误差 < 0.15 dB
//   2. 滤波器类型映射（PK/LS/HS）与 Q 语义正确（LS/HS 的 Q 版本 vs S=0.7 版本必须给出不同曲线）
//   3. Bypass vs Active 频响差值符合预期；0 dBFS 输入经 preamp 衰减后输出峰值 <= 0 dBFS（铁律 7 防削波）
//   4. 索引解析、URL 构造、令牌桶限频纯函数、缓存落盘/回读、畸变 JSON 容错
//   5. 网络用例 opt-in（RTA_NET_TESTS=1 时真抓 1 个预设校验）
#include "dsp/matrix.h"
#include "dsp/eq_text.h"
#include "dsp/opra.h"
#include "engine/pipeline.h"

#include <cstdio>
#include <string>
#include <vector>
#include <cmath>
#include <chrono>
#include <cstdlib>
#include <algorithm>

using namespace rta::dsp;

static int fails = 0;
static void check(bool ok, const std::string& what, const std::string& detail = "") {
    std::printf("  [%s] %s %s\n", ok ? "PASS" : "FAIL", what.c_str(), detail.c_str());
    if (!ok) ++fails;
}

// ---------------------------------------------------------------------------
// 真实 OPRA 样本 JSON
// ---------------------------------------------------------------------------

static const char* kHd600Json = R"({
  "id": "sennheiser:hd_600::oratory1990_harman_target",
  "productId": "sennheiser::hd_600",
  "author": "oratory1990",
  "details": "Harman Target",
  "type": "parametric_eq",
  "parameters": {
    "gainDb": -9.3,
    "bands": [
      { "frequency": 20, "gainDb": 4, "q": 1.1, "type": "peak_dip" },
      { "frequency": 97, "gainDb": -2.5, "q": 0.7, "type": "peak_dip" },
      { "frequency": 105, "gainDb": 5.5, "q": 0.71, "type": "low_shelf" },
      { "frequency": 215, "gainDb": -1.7, "q": 1.1, "type": "peak_dip" },
      { "frequency": 1400, "gainDb": -2.1, "q": 1.5, "type": "peak_dip" },
      { "frequency": 2000, "gainDb": 3, "q": 0.71, "type": "high_shelf" },
      { "frequency": 2700, "gainDb": -1.3, "q": 3, "type": "peak_dip" },
      { "frequency": 3370, "gainDb": -3.9, "q": 2.5, "type": "peak_dip" },
      { "frequency": 5350, "gainDb": -3.5, "q": 3, "type": "peak_dip" },
      { "frequency": 11000, "gainDb": -4, "q": 0.71, "type": "high_shelf" }
    ]
  }
})";

static const char* kAirPodsPro2Json = R"({
  "id": "apple:airpods_pro_2::autoeq_filk",
  "productId": "apple::airpods_pro_2",
  "author": "AutoEQ",
  "details": "Measured by Filk",
  "type": "parametric_eq",
  "parameters": {
    "gainDb": -3.6,
    "bands": [
      { "frequency": 105, "gainDb": 2.6, "q": 0.7, "type": "low_shelf" },
      { "frequency": 364, "gainDb": -1.9, "q": 1.62, "type": "peak_dip" },
      { "frequency": 4461, "gainDb": 3.3, "q": 2.37, "type": "peak_dip" },
      { "frequency": 77, "gainDb": 1.6, "q": 1.67, "type": "peak_dip" },
      { "frequency": 2828, "gainDb": -2.4, "q": 2.51, "type": "peak_dip" },
      { "frequency": 10000, "gainDb": -0.4, "q": 0.7, "type": "high_shelf" },
      { "frequency": 1257, "gainDb": 1.7, "q": 2.59, "type": "peak_dip" },
      { "frequency": 9292, "gainDb": 1.1, "q": 2.33, "type": "peak_dip" },
      { "frequency": 1734, "gainDb": -1, "q": 2.26, "type": "peak_dip" },
      { "frequency": 1320, "gainDb": 0.4, "q": 2.26, "type": "peak_dip" }
    ]
  }
})";

static const char* kWh1000Xm5Json = R"({
  "id": "sony:wh_1000xm5::oratory1990_harman_target",
  "productId": "sony::wh_1000xm5",
  "author": "oratory1990",
  "details": "Harman Target",
  "type": "parametric_eq",
  "link": "https://www.dropbox.com/s/ozv8rsyvq73zavf/Sony%20WH1000XM5.pdf?dl=0",
  "parameters": {
    "gainDb": -3.6,
    "bands": [
      { "frequency": 105, "gainDb": 5.5, "q": 0.71, "type": "low_shelf" },
      { "frequency": 270, "gainDb": -9.5, "q": 0.71, "type": "low_shelf" },
      { "frequency": 880, "gainDb": -1.4, "q": 3, "type": "peak_dip" },
      { "frequency": 1170, "gainDb": 1.9, "q": 2, "type": "peak_dip" },
      { "frequency": 1800, "gainDb": -1.9, "q": 3, "type": "peak_dip" },
      { "frequency": 2230, "gainDb": 6, "q": 1.4, "type": "peak_dip" },
      { "frequency": 3200, "gainDb": -5.3, "q": 1.8, "type": "peak_dip" },
      { "frequency": 6200, "gainDb": -3.8, "q": 3.3, "type": "peak_dip" },
      { "frequency": 8900, "gainDb": -3, "q": 6, "type": "peak_dip" },
      { "frequency": 9000, "gainDb": 3, "q": 0.71, "type": "high_shelf" }
    ]
  }
})";

// ---------------------------------------------------------------------------
// 辅助验证函数：逐频点比较两条滤波链响应误差
// ---------------------------------------------------------------------------

static double max_response_error(const std::vector<Biquad>& c1,
                                 const std::vector<Biquad>& c2,
                                 double fs = 48000.0) {
    double maxErr = 0.0;
    // 对数扫频 20 Hz ~ 20,000 Hz，覆盖 80+ 个特征频点
    for (double f = 20.0; f <= 20000.0; f *= 1.1) {
        double r1 = eq_chain_response_db(c1, f / (fs * 0.5));
        double r2 = eq_chain_response_db(c2, f / (fs * 0.5));
        double err = std::abs(r1 - r2);
        if (err > maxErr) maxErr = err;
    }
    return maxErr;
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("=== test_opra_eq: OPRA 耳机 EQ 离线门禁单测 ===\n");

    // -----------------------------------------------------------------------
    // §1. 3 款真实 OPRA 样本解析与 AutoEq 往返幅频响应对比 (< 0.15 dB)
    // -----------------------------------------------------------------------
    std::printf("\n[§1] 3 款真实 OPRA 样本解析与 AutoEq 往返幅频验证\n");
    struct SampleTest {
        const char* name;
        const char* json;
        double expPreamp;
        size_t expBands;
    };
    SampleTest samples[] = {
        { "Sennheiser HD 600", kHd600Json, -9.3, 10 },
        { "Apple AirPods Pro 2", kAirPodsPro2Json, -3.6, 10 },
        { "Sony WH-1000XM5", kWh1000Xm5Json, -3.6, 10 }
    };

    const double fs = 48000.0;
    for (const auto& sm : samples) {
        OpraPreset preset;
        std::string err;
        bool parseOk = OpraPreset::parse_json(sm.json, preset, &err);
        check(parseOk, std::string(sm.name) + " JSON 解析", err);
        check(preset.bands.size() == sm.expBands, std::string(sm.name) + " 频段数",
              std::to_string(preset.bands.size()) + "/" + std::to_string(sm.expBands));
        check(std::abs(preset.gainDb - sm.expPreamp) < 1e-6, std::string(sm.name) + " preampDb",
              std::to_string(preset.gainDb));

        // 直接由 preset 构建滤波链
        EqSpec specDirect = preset.to_eq_spec();
        auto chainDirect = build_eq_filters(specDirect, fs);

        // 导出为 AutoEq 文本并重新解析
        std::string autoEqText = preset.to_autoeq_text();
        EqSpec specFromText = parse_eq_text(autoEqText);
        auto chainFromText = build_eq_filters(specFromText, fs);

        check(specFromText.bands.size() == sm.expBands,
              std::string(sm.name) + " AutoEq 文本往返频段数一致",
              std::to_string(specFromText.bands.size()));
        check(std::abs(specFromText.preampDb - sm.expPreamp) < 0.05,
              std::string(sm.name) + " AutoEq Preamp 一致",
              std::to_string(specFromText.preampDb));

        // 逐频点比较解析幅频响应误差
        double maxErr = max_response_error(chainDirect, chainFromText, fs);
        char buf[64];
        std::snprintf(buf, sizeof(buf), "maxErr=%.6f dB (门槛 < 0.15 dB)", maxErr);
        check(maxErr < 0.15, std::string(sm.name) + " 逐频点幅频响应一致性", buf);
    }

    // -----------------------------------------------------------------------
    // §2. 滤波器类型映射与 Q 语义正确性（LS/HS Q 版 vs S=0.7 产生不同曲线）
    // -----------------------------------------------------------------------
    std::printf("\n[§2] 滤波器类型映射与 Q 语义正确性验证\n");
    {
        // 映射验证
        check(opra_type_to_eq_type("peak_dip") == "PK", "type 映射 peak_dip -> PK");
        check(opra_type_to_eq_type("low_shelf") == "LS", "type 映射 low_shelf -> LS");
        check(opra_type_to_eq_type("high_shelf") == "HS", "type 映射 high_shelf -> HS");
        check(opra_type_to_eq_type("low_pass") == "LP", "type 映射 low_pass -> LP");
        check(opra_type_to_eq_type("high_pass") == "HP", "type 映射 high_pass -> HP");
        check(opra_type_to_eq_type("band_pass") == "BP", "type 映射 band_pass -> BP");
        check(opra_type_to_eq_type("notch") == "NO", "type 映射 notch -> NO");
        check(opra_type_to_eq_type("all_pass") == "AP", "type 映射 all_pass -> AP");

        // LS Q 语义验证：架式滤波器在中心频率 f0 处增益恒为 gain/2，在过渡带（如 80 Hz / 150 Hz）
        // lowShelfQ(Q=1.5) 与 lowShelf(S=0.7) 响应产生显著差异（> 2 dB），证明 Q 公式生效
        Biquad q1, q2;
        biquad_design::lowShelfQ(q1, 48000.0, 105.0, 1.5, 6.0);
        biquad_design::lowShelf(q2, 48000.0, 105.0, 0.7, 6.0);
        double diff80 = std::abs(q1.response_db(80.0 / 24000.0) - q2.response_db(80.0 / 24000.0));
        char diffBuf[64];
        std::snprintf(diffBuf, sizeof(diffBuf), "80Hz 处差值=%.3f dB", diff80);
        check(diff80 > 1.0, "lowShelfQ(Q=1.5) vs lowShelf(S=0.7) 过渡带曲线显著不同，新函数生效", diffBuf);

        // HS Q 语义验证：highShelfQ(Q=1.5) vs highShelf(S=0.7) 在过渡带 8000 Hz 处差异显著
        biquad_design::highShelfQ(q1, 48000.0, 5000.0, 1.5, 6.0);
        biquad_design::highShelf(q2, 48000.0, 5000.0, 0.7, 6.0);
        double diff8k = std::abs(q1.response_db(8000.0 / 24000.0) - q2.response_db(8000.0 / 24000.0));
        std::snprintf(diffBuf, sizeof(diffBuf), "8000Hz 处差值=%.3f dB", diff8k);
        check(diff8k > 1.0, "highShelfQ(Q=1.5) vs highShelf(S=0.7) 过渡带曲线显著不同，新函数生效", diffBuf);
    }

    // -----------------------------------------------------------------------
    // §3. Bypass vs Active 频响差值符合预期 & 0 dBFS 输入防削波（铁律 7）
    // -----------------------------------------------------------------------
    std::printf("\n[§3] Bypass/Active 差值与 0 dBFS 防削波验证（铁律 7）\n");
    {
        // 1. Bypass 差值：无 EQ 时平直 0 dB
        std::vector<Biquad> emptyChain;
        double bypassResp = eq_chain_response_db(emptyChain, 1000.0 / 24000.0);
        check(bypassResp == 0.0, "Bypass 频响严格 0.0 dB");

        // 2. HD600 在 105 Hz 处有 +5.5 dB 提升，若无 Preamp，输入 0 dBFS 信号必然过载
        OpraPreset hd600;
        OpraPreset::parse_json(kHd600Json, hd600, nullptr);
        auto chain = build_eq_filters(hd600.to_eq_spec(), 48000.0);

        // 生成 1 秒 0 dBFS 的 105 Hz 正弦波
        const size_t N = 48000;
        std::vector<double> signal(N);
        for (size_t i = 0; i < N; ++i) {
            signal[i] = std::sin(2.0 * kPi * 105.0 * (double)i / 48000.0); // 峰值恰好 1.0 = 0 dBFS
        }

        // 不加 Preamp 时直接滤波：峰值必然 > 1.0
        std::vector<double> noPreamp = signal;
        for (auto& q : chain) {
            for (auto& s : noPreamp) s = q.process(s);
        }
        double maxNoPreamp = 0.0;
        for (double s : noPreamp) if (std::abs(s) > maxNoPreamp) maxNoPreamp = std::abs(s);
        char peakBuf[64];
        std::snprintf(peakBuf, sizeof(peakBuf), "无 preamp 峰值=%.2f (+%.2f dBFS, 必然削波)",
                      maxNoPreamp, 20.0 * std::log10(maxNoPreamp));
        check(maxNoPreamp > 1.2, "未引入 Preamp 时 105Hz 满幅信号严重过载", peakBuf);

        // 引入 OPRA Preamp (-9.3 dB) 施加于 ChannelProcess::gainDb
        // 增益因子 g = 10^(-9.3/20) ≈ 0.3427
        double preampGain = std::pow(10.0, hd600.gainDb / 20.0);
        std::vector<double> withPreamp = signal;
        // 先施加每声道增益（对齐 pipeline 阶段 3）
        for (auto& s : withPreamp) s *= preampGain;
        // 重置滤波状态再次处理
        auto chain2 = build_eq_filters(hd600.to_eq_spec(), 48000.0);
        for (auto& q : chain2) {
            for (auto& s : withPreamp) s = q.process(s);
        }
        double maxWithPreamp = 0.0;
        for (double s : withPreamp) if (std::abs(s) > maxWithPreamp) maxWithPreamp = std::abs(s);
        std::snprintf(peakBuf, sizeof(peakBuf), "施加 preamp 峰值=%.4f (%.2f dBFS <= 0 dBFS)",
                      maxWithPreamp, 20.0 * std::log10(maxWithPreamp));
        check(maxWithPreamp <= 1.0, "引入 Preamp 负增益保护后满幅输入峰值严格 <= 0 dBFS（防削波）", peakBuf);
        check(maxWithPreamp > 0.4, "输出信号未被过度衰减且非全零", peakBuf);
    }

    // -----------------------------------------------------------------------
    // §4. 索引解析、URL 构造、令牌桶限频、缓存往返与错误 JSON 容错
    // -----------------------------------------------------------------------
    std::printf("\n[§4] 纯函数组件单测（索引/URL/限频/缓存/容错）\n");
    {
        // 索引解析
        std::string sampleIndexJson = R"([
            ["sennheiser::hd_600", "Sennheiser", "HD 600"],
            ["apple::airpods_pro_2", "Apple", "AirPods Pro 2"],
            ["sony::wh_1000xm5", "Sony", "WH-1000XM5"]
        ])";
        OpraIndex idx;
        check(idx.load_from_json(sampleIndexJson), "索引 JSON 解析成功");
        check(idx.size() == 3, "索引条目数正确 (3)");
        auto hits = idx.filter("hd 600");
        check(hits.size() == 1 && hits[0].id == "sennheiser::hd_600", "模糊过滤搜索 HD 600");
        auto emptyHits = idx.filter("nonexistent_headphone_xyz");
        check(emptyHits.empty(), "搜索不存在产品返回空列表");

        // URL 构造
        std::string u1 = opra_url_products("sennheiser");
        check(u1 == "https://opra.roon.app/opra/products?vendorId=sennheiser", "products URL 构造", u1);
        std::string u2 = opra_url_eqs("sennheiser::hd_600");
        check(u2 == "https://opra.roon.app/opra/eqs?productId=sennheiser::hd_600", "eqs URL 构造", u2);
        std::string u3 = opra_url_eq("sennheiser:hd_600::oratory1990_harman_target");
        check(u3 == "https://opra.roon.app/opra/eqs/sennheiser:hd_600::oratory1990_harman_target", "single eq URL 构造", u3);

        // 令牌桶限频纯函数测试
        TokenBucket tb(10.0, 1.0); // 容量 10，每秒 1 个令牌
        auto t0 = std::chrono::steady_clock::now();
        check(tb.try_consume(10.0, t0), "初次消耗全部 10 个令牌成功");
        check(!tb.try_consume(1.0, t0), "无时间推移时立即再消耗被拒绝 (限频拦截生效)");
        auto t1 = t0 + std::chrono::milliseconds(2500); // 过去 2.5 秒，注入 2.5 令牌
        check(tb.try_consume(2.0, t1), "过去 2.5 秒后成功消耗 2 个令牌");
        check(!tb.try_consume(2.0, t1), "剩余令牌不足消耗 2 个再次被拒绝");

        // 缓存文件落盘与回读
        std::string tmpProfilePath = "build/test_opra_cache_sample.txt";
        std::string sampleText = "Preamp: -6.0 dB\nFilter 1: ON PK Fc 1000 Hz Gain 3.0 dB Q 1.00\n";
        std::string err;
        bool saveOk = save_autoeq_profile(tmpProfilePath, sampleText, &err);
        check(saveOk, "预设文件原子落盘", err);
        std::string readText;
        bool readOk = load_cached_profile(tmpProfilePath, readText);
        check(readOk && readText == sampleText, "已落盘预设文件完整回读一致");
        std::filesystem::remove(tmpProfilePath);

        // 错误与畸变 JSON 容错
        OpraPreset dummy;
        check(!OpraPreset::parse_json("{invalid_json_here", dummy, nullptr), "畸变 JSON 语法错误拒绝");
        check(!OpraPreset::parse_json("[]", dummy, nullptr), "空数组传入 parse_json 拒绝");
        check(!OpraPreset::parse_json("{}", dummy, nullptr), "缺少必要字段的空对象拒绝");
        std::vector<OpraPreset> dummies;
        check(!OpraPreset::parse_presets_json("{broken", dummies, nullptr), "畸变 JSON 传入 parse_presets_json 拒绝");

        // OpraService 初始化与 baseDir/cwd 索引解析
        bool initOk = OpraService::instance().init();
        if (!initOk) {
            initOk = OpraService::instance().init("", "..");
        }
        check(initOk, "OpraService 初始化并定位索引成功");
        check(OpraService::instance().index().size() > 5000, "OpraService 载入轻量索引条目数 > 5000");
    }

    // -----------------------------------------------------------------------
    // §5. 网络用例 opt-in (RTA_NET_TESTS=1)
    // -----------------------------------------------------------------------
    std::printf("\n[§5] 真实网络用例 (RTA_NET_TESTS=1 opt-in)\n");
    const char* netEnv = std::getenv("RTA_NET_TESTS");
    if (netEnv && std::string(netEnv) == "1") {
        std::printf("  [INFO] 检测到 RTA_NET_TESTS=1，开始请求真实 OPRA 服务端点...\n");
        std::vector<OpraPreset> presets;
        bool fromCache = false;
        std::string netErr;
        bool netOk = OpraService::instance().get_presets("sennheiser::hd_600", presets, fromCache, netErr, true);
        check(netOk && !presets.empty(), "网络真实抓取 HD 600 预设列表", netErr);
        if (!presets.empty()) {
            std::printf("  [INFO] 成功抓取 %zu 条预设，首条: %s (%s - %s)\n",
                        presets.size(), presets[0].id.c_str(), presets[0].author.c_str(), presets[0].details.c_str());
            check(!presets[0].id.empty() && !presets[0].bands.empty(), "首条预设参数结构完整");
        }
    } else {
        std::printf("  [SKIP] 默认跳过真实网络用例 (设置环境变量 RTA_NET_TESTS=1 可启用)\n");
    }

    std::printf("\n=== 总结: %d 项失败 ===\n", fails);
    return fails ? 1 : 0;
}
