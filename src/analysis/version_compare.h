// version_compare.h — 音频体检 B3：同一首曲子的多版本音质对比（N 个文件）
//
// 用户目标：「对比同一首音乐，不同版本之间的音质高低」。注意这是两件事：
//   1) 对齐：不同版本的编码/母带往往前导静音不同、时长不同，不对齐就没法逐项比；
//   2) 排序：必须能说出「为什么 A 比 B 好」，而不是给个黑箱数字。
//
// ================== 对齐（不猜，用互相关） ==================
//  * 包络：每文件按 200 Hz 取单声道 RMS 包络（5 ms 一点），与版本无关、与编码无关；
//  * 粗对齐：FFT 互相关（不是 O(n²) 直接算 —— 5 分钟曲子 6 万个点直接算要 3.6e9 次乘加），
//    取归一化互相关峰的位置做包络级延迟；包络粒度 5 ms。
//  * 相似度门限：对齐后归一化包络相关 < 0.55 ⟹ 判为「不是同一首」，全部对比作废
//    （不同母带同一首歌实测 0.8~0.99；两首不同的曲子 0.1~0.3，余量很大）。
//  * 公共区段：把各版本按其延迟换算成时间轴，取所有版本都覆盖的那一段，
//    然后【只在这一段上重算全部指标】—— 否则一个版本多 10 秒前导静音就会污染
//    本底噪声、静音占比、动态重心等所有统计量。
//
// ================== 不可比项（必须显式标注，不许硬比） ==================
//  * 位深不同（如 16-bit vs 24-bit）⟹ 本底噪声 / SNR 不可比（受容器量化下限影响）；
//  * 采样率不同（|Δ| > 20%）⟹ 谱重心 / 95% 滚降点 / >10 kHz 占比不可比（奈奎斯特不同）；
//    ★ 2026-10-27 BUG-3 修：采样率不同时【非谐波残留（权重 0.08）】也标不可比 —— 该指标的 FFT 窗长
//      随 fs 变（44.1k 1.49 s / 96k 0.68 s），同一母带 96/24 vs 44.1/16 实测漂 13.9 dB（吃满该维 10 dB
//      满罚门限），曾让两版差 18.6 分。同采样率对比时它照常参与（不是一刀切关掉）。
//  * 声道数不同（单声道 vs 立体声）⟹ 声道相关 / 单声道损失 / side-mid 不可比。
//  不可比的行照常列出数值，但 comparable=false + 中文原因，且不参与综合评分
//  （评分权重在这些行上重新归一化，见 score_versions）。
//
// ================== 综合排序的权重与理由 ==================
//   有效带宽 cutoff    0.20  见下节「第 8 维：有效带宽 / 有损转码」（新，最大权重之一）
//                            另加 0..12 分的「编解码损伤」附加项（不在权重和里，见两笔扣分那节）
//   动态范围 DR        0.18  母带压得狠就不可逆损失微动态，是最核心的判据
//   削波 clipping      0.14  波形被削掉是不可逆破坏，比"听起来差一点"严重
//   信噪比 SNR         0.14  本底噪声抬升（有损转制/不良转录）同样不可逆
//   非谐波残留         0.08  16 峰之外的能量（编码噪声/失真上界），覆盖面比 SNR 宽
//   峰值因子 crest     0.07  与 DR 同向但口径不同（全局 vs 3 s 块），交叉验证
//   LRA                0.07  短时动态分布，同上
//   单声道兼容 mono    0.04  只有相位反转这种病态素材会爆表，平时差异极小，故最低
//  合计 0.92（评分按【可比维度权重和】归一化，见 score_versions）。
//  ★ 为什么把原 7 维的权重整体调小：第 8 维拿走了 0.20，若其余不动，相对份额会被悄悄稀释
//    （0.20/1.20 只有 16.7 分），达不到"带宽不足要显著扣分"的要求。现在 0.20 是明确标定的：
//    带宽维净扣 21.7 分（= 100×0.20/0.92 满罚），> 削波维最大摆幅 14 分、> DR 维 19.6 分，
//    任何"靠削波少/动态高"攒出来的 6~8 分优势都翻不过它。
//  ★ 第 8 维（2026-10-27 新增，修下面的排序缺陷）
//  缺陷（上一轮自己上报的未解决项）：只用前 7 维时，有损编码（MP3/AAC）把峰值压低、
//    削波样本变少 ⟹ 「削波段数少」反而让它占便宜。实测同一首 60 s 四版：
//    V3(128k MP3) 100.0 > V4(低通14k假高解析) 94.3 > V2(320k MP3) 93.8 > V1(FLAC) 93.4，
//    削波段数 V1 276 段 → V3 51 段：无损因「削波多」被扣分，结论完全反了。
//  修法：用【信号本身】测有效带宽（不看容器/码率等可伪造的元数据）——
//    复用 fake_lossless.h 的长期平均谱算法（≤12 个 65536 点 Hann 窗、参考段 1–5 kHz、
//    −40 dB 门限、8 bin 连跑、1/6 倍频程陡沿判据），得到 spectralCutoffHz 与 lossySuspected。
//  依据（编码器实测上限，见 tests/test_version_compare.cpp 的实测段）：
//    FLAC 无损 @44.1k 满带宽（实测检出 21.3 kHz，源母带 21 kHz 以上本就滚降）；
//    320k MP3 实测 19.9 kHz；128k MP3 实测 16.6 kHz；低通 14k 的假高解析实测 12.6 kHz。
//  ⚠ 判据的唯一来源是 fake_lossless.h（本文件不许复述门限常数 —— 2026-10-27 就踩过：
//    这里写着「截止 < 0.78×奈奎斯特」，而当时 fake_lossless 用的是绝对门 20.5 kHz，
//    结果 320k MP3（20.1~20.2 kHz = 0.92×Nyq）被当成「确证有损」吃了 −12 固定罚，
//    注释与代码各自叙述，谁都不知道哪一条在生效）。
//    现行（2026-10-27 修复轮，fake_lossless.h 四档 kFakeVerdict*）：
//      <0.78×奈奎斯特 ⟹ 判假（lossySuspected=true）；
//      0.78~0.95×奈奎斯特 ⟹ 「带宽受限」，母带低通/升频长这样；下降沿达编码器级（≥35 dB/⅙oct）时仍判假；
//      >0.95× ⟹ 不判。
//    实测（build-vcfix，44.1k）：128k MP3 截止 16.79 kHz = 0.761×Nyq；320k MP3 截止 20.23 kHz = 0.918×Nyq。
//    ⚠ 判假这个【布尔量】只用于措辞与 UI 标记（「疑似有损转码」红字），**不再直接决定分数** ——
//    原因见下面两段：布尔量在边际阈值上会翻转，而分数必须是连续量。
//
// ================== 两笔扣分，罚的是两种不同的损失（2026-10-27 P1/P2/P3 修复） ==================
//  上一轮报告 §6-BUG-5：同一份证据（截止频率）被罚了两次 —— 连续带宽罚 + 「确证有损 ⟹ −12」。
//  128k vs 无损的 26.3 分差里 12 分就是这么来的，而 320k 也因此只拿 78.3 分（约 18 分不是可闻差异）。
//  上一轮报告 §6-BUG-6：同一个 320k 文件，两种分析窗口下 cutoff 20244.6→20240.0 Hz，判假档翻转 ⟹
//  总分 78.94→90.98（12 分跳变）。根因是「离散罚由边际阈值（0.78/0.95×Nyq + 置信度≥0.5）决定」。
//  本轮把扣分拆成两笔【性质不同】的损失，并且两笔都是连续量：
//    (a) 带宽损失（连续，权重 0.20，满罚 = 差 6 kHz ≈ 22.05−16 kHz = 128k 边界）
//        罚的是「**频谱少了多少**」：penalty = (bestCut − cut)/6 kHz，纯线性、纯 Hz。
//        这一项与「母带本来就窄」的合法素材无关（相对本批次最高截止比较）。
//    (b) 编解码损伤（0..12 分，codec_damage_points()）
//        罚的是「**保留下来的那段频谱里被编码器动过**」：预回声、频域量化噪声、联合立体声 ——
//        这些损伤没有任何带宽量能表达（带宽只告诉你「到哪儿为止」，不告诉你「里面糊没糊」）。
//        判据仍然只能用信号本身的【编码器级陡沿】：截止必须伴随 >20 dB/⅙oct 的悬崖
//        （模拟母带链实测只有 10~20 dB，LAME 320k 29.6 dB、AAC 128k 42.7 dB），
//        再乘置信度、乘「截止离奈奎斯特还有多远」的**平滑**权重：
//            rRamp = clamp((0.95 − ratio)/(0.95 − 0.82), 0, 1)   // 0.95×Nyq 归零，0.82×Nyq 满额
//            eRamp = clamp((edgeDb − 20)/(35 − 20), 0, 1)
//            cRamp = clamp((conf − 0.45)/(0.65 − 0.45), 0, 1)
//            points = 12 · rRamp · eRamp · cRamp
//        ⟹ 128k（0.761×Nyq）= 12 分满额；320k（0.918×Nyq）= 约 3 分；>0.95×Nyq = 0 分。
//  为什么这样就不再算「同一份证据罚两次」：
//    (a) 的量纲是 Hz（缺了多少频谱），(b) 的量纲是「有没有编码器悬崖 / 有多确定」，
//    两者只在「损失越彻底、编码器留下的可闻损伤也越多」这个单调关系上相关，而不是同一个数值用两遍。
//    原实现把 (b) 写成「只要判假就 −12」，与截止的具体位置无关 ⟹ 320k（几乎满带宽）和 64k
//    （只剩半个带宽）吃同样的固定罚，这才是真正的不合理；现在 (b) 在接近奈奎斯特时自动归零。
//  为什么必须是连续量（P2 的判据）：
//    布尔判假在 0.78/0.95 边界 + 置信度 0.5 附近会因 20 Hz 的窗口漂移翻转，而 12 分是测量噪声
//    （2.47 分，soxr 换 swr）的 5 倍。改成上面的三乘积后，20 Hz 的 cutoff 漂移只让 (b) 变动
//    12·(20/22050)/0.13 ≈ 0.08 分，判决不可能再翻。实测（build-vcfix，2026-10-27）：同一 320k 两次分析
//    cutoff 20233.9 / 20208.4 Hz（差 25.5 Hz，跨过 0.95/0.78 都不影响）、总分 89.25 / 89.04（差 0.22 分），
//    两次都判有损 ⟹ 不再有 12 分跳变（修前同一对窗口的机制是 78.94 ↔ 90.98）。
//  防误伤：两笔都对「整批同一窄带源」免疫 —— (a) 相对本批最高截止，(b) 与截止位置强相关，
//    整批都是 16 kHz 老录音时大家 rRamp 相同 ⟹ 不影响排序。
//  ⚠ 仍有可能误伤的一类：**母带真的用陡峭数字低通**（例如压到 19 kHz 的线性相位滤波器）。
//    它会同时吃 (a) 的带宽罚和 (b) 的编码器损伤罚。判别它需要「频域量化噪声/预回声」的直接测量，
//    本轮没有（见下面 P4 的未解决项）；当前以「陡沿 ≥35 dB/⅙oct 才算编码器级」作回归基线。
//
// ⚠⚠ 已知缺陷（P4，2026-10-27 实测；本轮**不改算法**，只登记 —— 铁律 4）：
//  **音乐素材上的谐波失真在这套指标里不可见**。实测同一段无损素材（build-vc3 的七组对照 B/B'）：
//    A 无损 99.664 vs B  +1% 三次谐波 99.9999  ⟹ 失真版**赢** 0.34 分；
//    A 无损 98.352 vs B' +5% 三次谐波 99.993   ⟹ 失真版**赢** 1.64 分。
//  两个根因（都在本文件之外或需要新指标，故本轮不动）：
//    ① quality.h 的 non_tonal 会陷波「16 个最强谱峰」，三次谐波（196→588 Hz 等）正好落在里面，
//       实测 non_tonal 只动 0.008 dB ⟹ 失真对它是隐形的；THD+N 对音乐素材返回「不适用」。
//    ② x + 0.05x³ 是【扩张型】非线性，峰值/ RMS 反而升高 ⟹ DR +0.28 dB、峰值因子 +0.38 dB，
//       三个维度一起判它「更好」（失真越重越像"动态更大"）。
//  要修需要新指标（按大概工作量排序）：
//    (i) 谐波族能量占比：对每个候选基频（≥200 Hz 的谱峰）累加 2~6 次谐波 bin 的能量，
//        与「非谐波邻域」对比 —— 可捕获 ①（不被 16 峰陷波影响），约 1~2 人日 + 标定；
//    (ii) 高次谐波/基频比（H3/H1 随电平的斜率）：扩张型 vs 压缩型的判别，约 1 人日；
//    (iii) 预回声/瞬态涂抹（前后 5 ms 能量比）—— 顺带解决 (b) 的「母带陡低通」误伤，约 2 人日。
//  在 (i) 落地之前，本页对「轻度谐波失真」的排序**不可信**，解释里也不声称能分辨失真。
//  ★ 打分方式：不用"绝对锚点"，用【相对最好版本的扣分】——
//     score = 100 · (1 − Σ wᵢ·penaltyᵢ / Σ wᵢ)，penaltyᵢ = min(1, |valᵢ − bestᵢ| / 满罚门限ᵢ)
//     满罚门限（"差到这个程度就算全错"）：DR 6 dB、SNR 20 dB、峰值因子 4 dB、LRA 6 LU、
//     非谐波残留 10 dB、单声道损失 3 dB、削波（占比 0.5% 或 2000 段，取大者）。
//   为什么不用绝对锚点：实测 3 个合成版本里 V1/V2 的非谐波残留是 −68.5/−59.0 dB，
//   都低于任何"绝对好"的锚点上限，于是两者同分、排序退化成随机。相对扣分没有饱和区，
//   且解释能直接说"比最好的版本低 9.5 dB，达到满罚门限的 95%"。
//   代价：分数只在【这一批版本内】有意义，跨批次不可比 —— 这一点写进解释里。
#pragma once
#include "quality.h"
#include "dynamics.h"
#include "health_stream.h"
#include "fake_lossless.h"     // 复用其长期平均谱 + 陡沿截止判据（第 8 维：有效带宽/有损转码）
#include "../audio/audio_loader.h"
#include <string>
#include <vector>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <algorithm>
#include <chrono>
#include <functional>
#include <iomanip>
#include <memory>
#include <sstream>

namespace rta::analysis {

// ---------------- PCM 源：优先分块流式（DSD/WAV），否则整段解码 ----------------
class PcmFile {
public:
    bool open(const std::string& path, std::string* why = nullptr) {
        close();
        std::string streamWhy;
        if (src_.open(path, &streamWhy)) { streamed_ = true; return true; }
        // 回退：整段解码（flac/mp3/…），仍然只读
        try {
            rta::audio::LoadNote note;
            buf_ = rta::audio::read_any(path, &note);
            whole_ = true;
            if (buf_.frames() == 0 || buf_.channels <= 0 || buf_.rate == 0) {
                if (why) *why = "解码后无可用样本";
                close();
                return false;
            }
            return true;
        } catch (const std::exception& e) {
            if (why) *why = std::string("解码失败: ") + e.what();
        } catch (...) {
            if (why) *why = "解码失败（未知异常）";
        }
        return false;
    }
    void close() { src_.close(); buf_ = rta::audio::AudioBuffer(); streamed_ = whole_ = false; }
    bool   streamed() const { return streamed_; }
    uint32_t rate() const { return streamed_ ? src_.rate() : buf_.rate; }
    int    channels() const { return streamed_ ? src_.channels() : buf_.channels; }
    int    bits() const { return streamed_ ? src_.bits() : buf_.bits; }
    bool   isFloat() const { return streamed_ ? src_.isFloat() : buf_.isFloat; }
    size_t totalFrames() const { return streamed_ ? src_.totalFrames() : buf_.frames(); }
    size_t read(size_t pos, size_t n, rta::audio::AudioBuffer& out) {
        if (streamed_) return src_.read(pos, n, out);
        const size_t total = buf_.frames();
        if (pos >= total || n == 0) return 0;
        if (n > total - pos) n = total - pos;
        const size_t chn = (size_t)buf_.channels;
        out.rate = buf_.rate; out.channels = buf_.channels; out.bits = buf_.bits; out.isFloat = buf_.isFloat;
        out.raw.assign(buf_.raw.begin() + (long long)(pos * chn),
                       buf_.raw.begin() + (long long)((pos + n) * chn));
        return n;
    }
private:
    detail::HealthSource src_;
    rta::audio::AudioBuffer buf_;
    bool streamed_ = false, whole_ = false;
};

// ---------------- 结果结构 ----------------
struct CompareOptions {
    size_t blockFrames = 65536;
    // 判同曲门限（200 Hz 包络、重叠段 Pearson）。
    // 老口径（原始 5 ms 包络，本机曲库）：同一首不同母带 0.53 / 0.56；不同曲子 0.04 / 0.25 / 0.29。
    // ★ 2026-10-27 起改为【11 点平滑包络】（见 smooth_env），相关值整体上移，实测（build-vc3 合成素材）：
    //     不同曲子 0.037 / 0.061 / 0.068 / 0.075（4 对全部拒绝，余量 6 倍）；
    //     同一内容裁掉 1 s 0.936；前置 0.5/2/3/5 s 静音 0.972 / 0.994 / 0.945 / 0.963；
    //     容器互换（WAV↔FLAC，16/24-bit）1.000；48k 往返 1.000。
    //   0.45 仍落在两簇之间（不同曲子上限 0.075、同一首下限 0.93），余量比老口径大得多。
    //   ⚠ 未解决项：「同一首不同母带」在真实曲库上的平滑包络值本轮没有重测（本机无曲库素材），
    //     沿用老口径 0.53~0.56 作为"会低到 0.5 左右"的下界参考。
    double minEnvCorr = 0.45;
    bool   verbose = false;       // true 时把对齐/耗时打到 stderr
};

struct MetricRow {
    std::string key, label, unit;
    bool higherBetter = true;
    bool informational = false;   // 无方向性（只展示，不评分）
    bool comparable = true;
    std::string why;              // 不可比时的中文原因
    std::vector<double> values;
    size_t bestIndex = 0;
};

struct VersionData {
    std::string path, label;
    std::string error;
    uint32_t fs = 0;
    int    channels = 0, bits = 0;
    double durationSec = 0.0;
    size_t totalFrames = 0;
    // 对齐
    long long lagSec200 = 0;      // 相对参考版本的内容偏移（1/200 秒，正=该文件内容更早）
    double lagSec = 0.0;
    double envCorr = 1.0;
    long long windowStartFrames = 0;
    size_t windowFrames = 0;
    std::vector<float> env;       // 200 Hz 单声道 RMS 包络（原始 5 ms；诊断用；不参与评分）
    // 平滑后的包络（≈55 ms 移动平均）——【对齐与判同曲只用这一个】。
    // ★ 为什么必须有它（2026-10-27 DEBUG 实测）：5 ms RMS 窗比低音的周期还短（98 Hz 一个周期
    //   10.2 ms ⟹ 一窗只有半个周期），于是每个 bin 的 RMS 强烈依赖窗的起始相位。只要内容平移量
    //   不是 hop（220 样本 @44.1k）的整数倍，各 bin 的相位就全部换位，同一段音频的包络互相关系数
    //   会从 1.00 掉到 0.61，而且最大值还会跑到完全错误的延迟上：
    //     · 同一内容裁掉前 1 s（逐样本验证 max|Δ|=0）⟹ 原实现给出 lag=+8.53 s、corr=0.80，
    //       真值是 lag=−1.0 s（在真延迟处 corr 只有 0.61）——对齐错 9.5 s 仍然被判"同一首"；
    //     · 同一内容前置 3 s 数字静音 ⟹ 原实现给出 lag=−6.51 s（真值 +3.0），公共区段整段错位，
    //       两版分差 8.7 分（99.09 vs 90.41）。
    //   平滑（11 点 ≈ 55 ms）后同一组的峰值回到真延迟、corr 0.93~0.95，见 tests 与报告。
    std::vector<float> envSmooth;
    // 公共区段上的指标
    QualityResult quality;
    DynamicsResult dynamicsTimes;
    bool analyzed = false;
    // ---- 第 8 维：有效带宽 / 有损转码（判据只用信号本身，见文件头）----
    double spectralCutoffHz = 0.0;   // 长期平均谱检出的截止；0 = 未检出陡沿（视为满带宽）
    double cutoffConfidence = 0.0;   // 0..1（陡沿深度 × 窗间一致率）
    bool   cutoffDetected = false;
    // 判据的唯一来源是 fake_lossless.h 的四档判定（fakeVerdict == kFakeVerdictLossy）：
    // 现行规则（2026-10-27 修复轮）= 截止 < 0.78×奈奎斯特 + 陡沿 >12 dB/⅙oct + 置信度 ≥0.5。
    // 本文件不复述这个门限（复述过一次，注释与代码就漂了），只消费 suspected 这个布尔量。
    bool   lossySuspected = false;
    std::string cutoffReason;        // 中文人话（UI/解释直接显示）
    // ---- 编解码损伤的连续证据（2026-10-27 P1/P2 修复新增）----
    // 这三个量是 codec_damage_points() 的输入：陡沿深度（编码器特征）、置信度、以及截止离奈奎斯特多远。
    // 为什么不让 score 直接看 lossySuspected：那个布尔量在 0.78/0.95×Nyq + 置信度 0.5 的边界上会因为
    // 20 Hz 的窗口漂移翻转（实测 78.94 ↔ 90.98，12 分跳变）。分数必须由连续量算出来。
    double cutoffEdgeDb  = 0.0;      // 各窗在 f_cut 处 1/6 倍频程能量差的中位数（dB）
    double nyquistHz     = 0.0;      // 本次分析用的奈奎斯特（= fs/2）
    int    fakeVerdict   = 0;        // fake_lossless.h 的四档（只用于措辞/UI，不参与打分）
    double codecPenaltyPoints = 0.0; // score_versions() 填：本版实际被扣的编解码损伤分（0..12）
};

struct VersionCompare {
    bool ok = false;
    std::string error;
    bool sameTrack = false;
    double sameTrackCorr = 1.0;
    size_t windowFrames = 0;
    double windowSec = 0.0;
    std::vector<VersionData> versions;
    std::vector<MetricRow> rows;
    std::vector<double> scores;
    // 各评分维度的扣分与折算分（0..100），供 UI/报告逐步交代「分数是怎么来的」。
    // penalty ∈ [0,1]（1 = 满罚），score = 100·(1−penalty)。
    struct DimRow {
        std::string key, label;
        double weight = 0.0;
        std::vector<double> penalty;
        std::vector<double> score;
    };
    std::vector<DimRow> dims;
    std::vector<size_t> ranking;      // 好→差
    std::string explanation;          // 确定性人话（不依赖 AI）
    // 结论可信度提示（2026-10-27 DEBUG 新增；纯附加：不改任何分数/名次/权重，
    // 只把「什么时候不该把名次当结论」说清楚）。同一条文本也会追加进 explanation，
    // 因此 UI 不改也能看到；单独留一份字段是给以后的 API/测试用。
    std::vector<std::string> caveats;
    double enrollMs = 0.0, alignMs = 0.0, analyzeMs = 0.0;
};

namespace vcdetail {

inline size_t next_pow2(size_t v) { size_t p = 1; while (p < v) p <<= 1; return p; }

// 200 Hz 单声道 RMS 包络（只做一遍幂运算，分析器不在这一步跑）
template <class Src>
inline std::vector<float> envelope_of(Src& src, size_t blockFrames, double* envHzOut) {
    const uint32_t fs = src.rate();
    const size_t hop = (size_t)std::max(1.0, std::floor((double)fs / 200.0));
    *envHzOut = (double)fs / (double)hop;
    std::vector<float> env;
    const size_t total = src.totalFrames();
    env.reserve(total / hop + 2);
    rta::audio::AudioBuffer chunk;
    double acc = 0.0;
    size_t cnt = 0;
    size_t pos = 0;
    while (pos < total) {
        const size_t n = std::min(blockFrames ? blockFrames : size_t(65536), total - pos);
        const size_t got = src.read(pos, n, chunk);
        if (got == 0) break;
        const int chn = src.channels();
        const double inv = 1.0 / (double)std::max(1, chn);
        for (size_t i = 0; i < got; ++i) {
            double s = 0.0;
            for (int c = 0; c < chn; ++c) s += chunk.sample(i, c);
            const double m = s * inv;
            acc += m * m;
            if (++cnt == hop) { env.push_back((float)std::sqrt(acc / (double)hop)); acc = 0.0; cnt = 0; }
        }
        pos += got;
        if (got < n) break;
    }
    return env;
}

// 归一化互相关（FFT）：返回最佳延迟 L（1/200 秒），a[n] ≈ b[n−L] 时的 L 与相关系数。
// 实现：r[k] = Σ_n a[n]·b[n−k]（k 可正可负），由 IFFT(A·conj(B)) 一次性给出。
inline void best_lag(const std::vector<float>& a, const std::vector<float>& b,
                     long long* lagOut, double* corrOut) {
    *lagOut = 0; *corrOut = 0.0;
    const size_t na = a.size(), nb = b.size();
    if (na < 8 || nb < 8) return;
    const size_t N = next_pow2(na + nb);
    std::vector<rta::dsp::cplx> A(N, rta::dsp::cplx(0.0, 0.0)), B(N, rta::dsp::cplx(0.0, 0.0));
    double meanA = 0.0, meanB = 0.0;
    for (float v : a) meanA += v;
    for (float v : b) meanB += v;
    meanA /= (double)na; meanB /= (double)nb;
    double ea = 0.0, eb = 0.0;
    for (size_t i = 0; i < na; ++i) { const double v = (double)a[i] - meanA; A[i] = rta::dsp::cplx(v, 0.0); ea += v * v; }
    for (size_t i = 0; i < nb; ++i) { const double v = (double)b[i] - meanB; B[i] = rta::dsp::cplx(v, 0.0); eb += v * v; }
    if (ea <= 0.0 || eb <= 0.0) return;
    rta::dsp::fft(A, false);
    rta::dsp::fft(B, false);
    for (size_t k = 0; k < N; ++k) A[k] *= std::conj(B[k]);
    rta::dsp::fft(A, true);
    const double norm = 1.0 / std::sqrt(ea * eb);
    double best = -2.0; long long bestLag = 0;
    for (size_t k = 0; k < nb; ++k) {          // 正延迟
        const double r = A[k].real() * norm;
        if (r > best) { best = r; bestLag = (long long)k; }
    }
    for (size_t k = 1; k < na; ++k) {          // 负延迟
        const double r = A[N - k].real() * norm;
        if (r > best) { best = r; bestLag = -(long long)k; }
    }
    *lagOut = bestLag;
    *corrOut = best;
}

// 重叠区段上的 Pearson 相关系数（均值/能量都只按重叠部分算）。
// ★ 为什么必须只用重叠段：两版前导静音不同（0.5 s vs 2.0 s），整段均值差 0.08，
//   减掉各自整段均值后 b 残留一个直流偏置，实测把同内容的相关系数从 0.99 压到 0.73。
//   对齐（求 lag）可以容忍这一点，但"是否同一首"的判据必须用无偏的重叠段 Pearson。
inline double overlap_corr(const std::vector<float>& a, const std::vector<float>& b, long long lag) {
    const long long na = (long long)a.size(), nb = (long long)b.size();
    const long long n0 = std::max<long long>(0, lag);
    const long long n1 = std::min<long long>(na, nb + lag);
    if (n1 - n0 < 8) return 0.0;
    double sa = 0.0, sb = 0.0;
    const long long cnt = n1 - n0;
    for (long long n = n0; n < n1; ++n) { sa += a[(size_t)n]; sb += b[(size_t)(n - lag)]; }
    const double ma = sa / (double)cnt, mb = sb / (double)cnt;
    double cov = 0.0, va = 0.0, vb = 0.0;
    for (long long n = n0; n < n1; ++n) {
        const double x = a[(size_t)n] - ma, y = b[(size_t)(n - lag)] - mb;
        cov += x * y; va += x * x; vb += y * y;
    }
    if (va <= 0.0 || vb <= 0.0) return 0.0;
    return cov / std::sqrt(va * vb);
}

// 对齐用的包络平滑宽度（点数，1 点 = 1/200 s）。11 点 ≈ 55 ms：
// 比最低乐音周期（约 30 Hz ⟹ 33 ms）长，足以把「窗起始相位」的影响平均掉；
// 又远短于音乐的结构（4~8 s 的乐句），不会把相邻的鼓点糊成一个包。
inline constexpr int kAlignSmooth = 11;

// 居中移动平均（两端按实际可用点数平均，不补零 —— 补零会在首尾制造假的下凹）。
inline std::vector<float> smooth_env(const std::vector<float>& x, int w) {
    if (w <= 1 || x.size() < 3) return x;
    std::vector<float> o(x.size(), 0.0f);
    const int h = w / 2;
    for (long long i = 0; i < (long long)x.size(); ++i) {
        double s = 0.0; int n = 0;
        for (int k = -h; k <= h; ++k) {
            const long long j = i + k;
            if (j < 0 || j >= (long long)x.size()) continue;
            s += x[(size_t)j]; ++n;
        }
        o[(size_t)i] = (float)(s / (double)std::max(1, n));
    }
    return o;
}

// 线性锚点映射到 0..100（低于 lo 记 0，高于 hi 记 100）
inline double map_range(double v, double lo, double hi) {
    if (!(hi > lo)) return 0.0;
    return std::clamp((v - lo) / (hi - lo), 0.0, 1.0) * 100.0;
}

} // namespace vcdetail

// 公共区段分析：把 src 的 [start, start+len) 帧喂给三个增量分析器（音质 / 动态 / 带宽）
// lc 非空时顺带算「有效带宽 + 有损转码」：用的是 fake_lossless.h 的同一套长期平均谱算法，
// 这样 UI 上的截止频率（/api/health 的假无损判定）与本页的带宽维度口径完全一致。
template <class Src>
inline bool analyze_window(Src& src, size_t start, size_t len, size_t blockFrames,
                           QualityResult& q, DynamicsResult& d, std::string* why = nullptr,
                           LosslessCheck* lc = nullptr) {
    const int chn = src.channels();
    if (chn <= 0 || len == 0) { if (why) *why = "公共区段为空"; return false; }
    QualityAccumulator qacc;
    DynamicsAccumulator dacc;
    LosslessAccumulator lacc;
    const double fs = (double)src.rate();
    qacc.begin(len, chn, fs);
    dacc.begin(chn, fs);
    if (lc) lacc.begin(len, chn, fs, src.bits());
    rta::audio::AudioBuffer chunk;
    std::vector<double> tmp;
    size_t pos = 0;
    while (pos < len) {
        const size_t n = std::min(blockFrames ? blockFrames : size_t(65536), len - pos);
        const size_t got = src.read(start + pos, n, chunk);
        if (got == 0) break;
        tmp.resize(got * (size_t)chn);
        for (size_t i = 0; i < got; ++i)
            for (int c = 0; c < chn; ++c) tmp[i * (size_t)chn + (size_t)c] = chunk.sample(i, c);
        qacc.add_interleaved(tmp.data(), got);
        dacc.add_interleaved(tmp.data(), got);
        if (lc) lacc.add_interleaved(tmp.data(), got);
        pos += got;
        if (got < n) break;
    }
    q = qacc.finalize();
    d = dacc.finalize();
    if (lc) *lc = lacc.finalize();
    if (pos != len) {
        if (why) *why = "公共区段读取不完整（" + std::to_string(pos) + "/" + std::to_string(len) + " 帧）";
        return false;
    }
    return true;
}

// ==================== 主入口：比较 N 个文件 ====================
inline VersionCompare compare_versions(const std::vector<std::string>& paths, const CompareOptions& opt = {}) {
    VersionCompare out;
    auto t0 = std::chrono::steady_clock::now();
    if (paths.size() < 2) { out.error = "至少需要 2 个文件才能对比"; return out; }

    out.versions.resize(paths.size());
    for (size_t i = 0; i < paths.size(); ++i) {
        out.versions[i].path = paths[i];
        out.versions[i].label = "V" + std::to_string(i + 1);
    }

    // ---- 第一遍：包络 + 元信息（内存只留包络，与曲长弱相关：4 B / 5 ms ≈ 0.8 MB/小时）----
    std::vector<std::unique_ptr<PcmFile>> srcs;
    for (size_t i = 0; i < paths.size(); ++i) {
        auto pf = std::make_unique<PcmFile>();
        std::string why;
        if (!pf->open(paths[i], &why)) {
            out.error = "版本 " + out.versions[i].label + " 无法打开：" + why;
            return out;
        }
        VersionData& v = out.versions[i];
        v.fs = pf->rate(); v.channels = pf->channels(); v.bits = pf->bits();
        v.totalFrames = pf->totalFrames();
        v.durationSec = (v.fs > 0) ? (double)v.totalFrames / (double)v.fs : 0.0;
        double envHz = 200.0;
        std::vector<float> env = vcdetail::envelope_of(*pf, opt.blockFrames, &envHz);
        if (env.empty()) { out.error = "版本 " + v.label + " 无可用样本"; return out; }
        // 统一到「以秒为单位」的包络：按本文件 envHz 重采样到公共 200 Hz 网格（避免不同 fs 的偏差）
        const double targetHz = 200.0;
        std::vector<float> renorm;
        renorm.reserve((size_t)((double)env.size() * targetHz / envHz) + 2);
        for (double t = 0.0; t < (double)env.size() / envHz; t += 1.0 / targetHz) {
            const double fpos = t * envHz;
            const size_t i0 = (size_t)fpos;
            const size_t i1 = std::min(env.size() - 1, i0 + 1);
            const double a = fpos - (double)i0;
            renorm.push_back((float)((1.0 - a) * env[i0] + a * env[i1]));
        }
        srcs.push_back(std::move(pf));
        v.env.swap(renorm);
        v.envSmooth = vcdetail::smooth_env(v.env, vcdetail::kAlignSmooth);
    }
    out.enrollMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    auto t1 = std::chrono::steady_clock::now();

    // ---- 对齐：全部对参考版本（V1）做互相关 ----
    // ★ 只用【平滑包络】求延迟与判同曲（理由见 VersionData::envSmooth 的注释：5 ms 原始包络
    //   对低音相位敏感，实测同一内容裁 1 s 会给出 +8.53 s 的假延迟且相关 0.80 仍能过门）。
    const std::vector<float>& refEnv = out.versions[0].envSmooth;
    // 细化用的中间尺度包络（3 点 ≈ 15 ms）：11 点平滑的峰太宽，实测「前置 0.5 s 静音」会偏 20 ms，
    // 而 20 ms 的偏移足以让同一内容在 30 s 素材上差 2.3 分。所以两级：
    //   ① 11 点平滑上求峰（抗低频相位，绝不滑到别的周期）；
    //   ② 只在粗峰 ±8 点（±40 ms）内用 3 点平滑包络重新取峰（更锐，且不会跑远）。
    std::vector<std::vector<float>> refineEnv(out.versions.size());
    for (size_t i = 0; i < out.versions.size(); ++i)
        refineEnv[i] = vcdetail::smooth_env(out.versions[i].env, 3);
    double worstCorr = 2.0;
    for (size_t i = 1; i < out.versions.size(); ++i) {
        long long lag = 0; double corr = 0.0;
        vcdetail::best_lag(refEnv, out.versions[i].envSmooth, &lag, &corr);
        {
            long long bestLag = lag;
            double bestC = vcdetail::overlap_corr(refineEnv[0], refineEnv[i], lag);
            for (long long dd = -8; dd <= 8; ++dd) {
                const double c = vcdetail::overlap_corr(refineEnv[0], refineEnv[i], lag + dd);
                if (c > bestC) { bestC = c; bestLag = lag + dd; }
            }
            lag = bestLag;
        }
        // r[k] = Σ a[n]·b[n−k] 在 k 处最大 ⟹ 版本 i 的内容比参考【早】k 点，
        // 即「参考时间 0 在该文件里的位置」= −k。符号写反会让公共区段整段错位（实测踩过）。
        out.versions[i].lagSec200 = -lag;
        out.versions[i].lagSec = -(double)lag / 200.0;
        // 判据用重叠段 Pearson（见 vcdetail::overlap_corr 的说明）；FFT 的全局值只用于定位
        out.versions[i].envCorr = vcdetail::overlap_corr(refEnv, out.versions[i].envSmooth, lag);
        worstCorr = std::min(worstCorr, out.versions[i].envCorr);
        if (opt.verbose)
            std::fprintf(stderr, "  对齐 %s: 延迟 %+.3f s, 包络相关 %.4f\n",
                         out.versions[i].label.c_str(), out.versions[i].lagSec, corr);
    }
    out.sameTrackCorr = (worstCorr > 1.0) ? 1.0 : worstCorr;
    out.sameTrack = (out.sameTrackCorr >= opt.minEnvCorr);
    if (!out.sameTrack) {
        char cb[64], tb[32];
        std::snprintf(cb, sizeof(cb), "%.3f", out.sameTrackCorr);
        std::snprintf(tb, sizeof(tb), "%.2f", opt.minEnvCorr);
        out.error = std::string("对齐后包络相关只有 ") + cb + "（门限 " + tb +
                    "）：这些文件不像同一首曲子，对比结果没有意义";
        out.ok = false;
        return out;
    }
    out.alignMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t1).count();
    auto t2 = std::chrono::steady_clock::now();

    // ---- 公共区段（时间轴）----
    // 定义：文件 i 的时间 = 参考文件时间 + lag_i  ⟹  参考文件时间 = 文件时间 − lag_i。
    // 于是文件 i 覆盖的参考时间区间 = [−lag_i, dur_i − lag_i]；
    // 文件 i 里对应参考时间 contentStart 的位置 = contentStart + lag_i。
    // ★ 这两处符号写反，窗口就会落进前导静音里（实测 V2 因此测出 −200 dBFS 的假本底）。
    double contentStart = -1e18, contentEnd = 1e18;
    for (size_t i = 0; i < out.versions.size(); ++i) {
        const VersionData& v = out.versions[i];
        contentStart = std::max(contentStart, -v.lagSec);
        contentEnd = std::min(contentEnd, v.durationSec - v.lagSec);
    }
    const double commonSec = contentEnd - contentStart;
    if (!(commonSec > 0.5)) {
        out.error = "各版本没有足够的公共区段（重叠 " + std::to_string(commonSec) + " s）";
        return out;
    }
    // 每文件窗口：帧起点 = (contentStart − lag_i) × fs
    for (size_t i = 0; i < out.versions.size(); ++i) {
        VersionData& v = out.versions[i];
        const double t0s = contentStart + v.lagSec;
        long long startF = (long long)std::llround(t0s * (double)v.fs);
        startF = std::max<long long>(0, startF);
        size_t len = (size_t)std::llround(commonSec * (double)v.fs);
        if ((size_t)startF + len > v.totalFrames) len = v.totalFrames - (size_t)startF;
        v.windowStartFrames = startF;
        v.windowFrames = len;
    }
    // 统一到最短（不同 fs 时四舍五入可能差几帧）：公共时长取各版本窗口时长最小值
    double minWinSec = 1e18;
    for (const VersionData& v : out.versions)
        minWinSec = std::min(minWinSec, (v.fs > 0) ? (double)v.windowFrames / (double)v.fs : 0.0);
    out.windowSec = minWinSec;
    out.windowFrames = (size_t)std::llround(minWinSec * (double)out.versions[0].fs);

    // ---- 第二遍：只在公共区段上重算全部指标 ----
    for (size_t i = 0; i < out.versions.size(); ++i) {
        VersionData& v = out.versions[i];
        const size_t len = (size_t)std::llround(minWinSec * (double)v.fs);
        std::string why;
        LosslessCheck lc;
        if (!analyze_window(*srcs[i], (size_t)v.windowStartFrames, len, opt.blockFrames,
                            v.quality, v.dynamicsTimes, &why, &lc)) {
            v.error = why;
        } else {
            v.analyzed = true;
            // 第 8 维：带宽/有损（长度与采样率都来自本文件，与容器位深/码率无关）
            v.spectralCutoffHz = lc.cutoffHz;
            v.cutoffConfidence = lc.confidence;
            v.cutoffDetected = (lc.cutoffHz > 0.0);
            v.lossySuspected = lc.suspected;
            v.cutoffReason = lc.reason;
            // 连续证据（打分用；见 VersionData 的说明与 codec_damage_points）
            v.cutoffEdgeDb = lc.edgeDb;
            v.fakeVerdict = lc.fakeVerdict;
            v.nyquistHz = (lc.nyquistHz > 0.0) ? lc.nyquistHz
                                               : 0.5 * (double)(v.fs ? v.fs : 44100u);
            if (opt.verbose)
                std::fprintf(stderr, "  %s 有效带宽: 截止 %s, 置信度 %.2f, 有损=%d (%s)\n",
                             v.label.c_str(),
                             v.cutoffDetected ? (std::to_string((int)std::lround(v.spectralCutoffHz)) + " Hz").c_str()
                                              : "未检出",
                             v.cutoffConfidence, (int)v.lossySuspected, v.cutoffReason.c_str());
        }
    }
    out.analyzeMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t2).count();
    out.ok = true;
    return out;
}

// ==================== 指标表 + 评分 + 排序 + 解释 ====================
namespace vcdetail {

inline void push_row(VersionCompare& r, const char* key, const char* label, const char* unit,
                     bool higherBetter, bool informational,
                     std::function<double(const VersionData&)> get,
                     const std::string& why = std::string()) {
    MetricRow row;
    row.key = key; row.label = label; row.unit = unit;
    row.higherBetter = higherBetter; row.informational = informational;
    row.comparable = why.empty();
    row.why = why;
    for (const VersionData& v : r.versions) row.values.push_back(get(v));
    // 最佳：方向性指标取极值；信息性指标不标最佳
    row.bestIndex = 0;
    if (!informational && row.comparable) {
        for (size_t i = 1; i < row.values.size(); ++i) {
            const bool better = higherBetter ? (row.values[i] > row.values[row.bestIndex])
                                             : (row.values[i] < row.values[row.bestIndex]);
            if (better) row.bestIndex = i;
        }
    }
    r.rows.push_back(std::move(row));
}

// 单个维度的 0..100 分（锚点写在这里，解释时可直接引用）
inline double dyn_score(const VersionData& v) { return map_range(v.dynamicsTimes.drDb, 3.0, 20.0); }
inline double snr_score(const VersionData& v) { return map_range(v.quality.snrDb, 20.0, 80.0); }
inline double crest_score(const VersionData& v) { return map_range(v.quality.crestDb, 6.0, 20.0); }
inline double lra_score(const VersionData& v) { return map_range(v.dynamicsTimes.lra, 3.0, 15.0); }
inline double nonTonal_score(const VersionData& v) { return map_range(-v.quality.nonTonalDb, 0.0, 30.0); }
inline double mono_score(const VersionData& v) {
    if (!v.quality.stereo) return 100.0;          // 单声道素材没有兼容性问题
    return std::clamp(100.0 - v.quality.monoLossDb * 20.0, 0.0, 100.0);
}
// 有效带宽（第 8 维）：未检出陡沿 ⟹ 视为满带宽，取 min(奈奎斯特, 22.05 kHz)。
// 为什么封顶 22.05 kHz：一个 96 kHz 的无损版本若「未检出截止」，不该因为奈奎斯特更高就在
// 这一维白拿 26 kHz；44.1k 的 22.05 kHz 才是本维度的「满带宽」参照。
inline double effective_bandwidth(const VersionData& v) {
    // ★ 2026-10-27 DEBUG 修：检出截止也要封顶 22.05 kHz。
    // 原来只有「未检出」分支封顶，于是 96 kHz 素材里检出 25.0 kHz 陡沿的版本会拿到 25 kHz 的
    // 「带宽分」，比 44.1k 无损的 22.05 kHz 还高 —— 实测同一段 44.1k 素材原样上采样到 96 kHz，
    // 它检出 25.0 kHz 截止（置信度 0.83），带宽维度给它满分、给 44.1k 原版 0.49 的罚分，
    // 单这一项就差 12.7 分，方向完全反了（上采样不该比原版"带宽更宽"）。
    // 封顶与本维度的定义一致：22.05 kHz 就是本维度的「满带宽参照」，更高没有额外意义。
    const double kCap = 22050.0;
    if (v.spectralCutoffHz > 0.0) return std::min(v.spectralCutoffHz, kCap);
    const double nyq = (v.fs > 0) ? 0.5 * (double)v.fs : kCap;
    return std::min(nyq, kCap);
}
// -------------------- 编解码损伤（附加扣分，0..12 分）--------------------
// 见文件头「两笔扣分，罚的是两种不同的损失」。为什么是附加分而不是第 9 个维度：
//   (a) 它没有「单位」，不该被塞进「可比维度权重和」里重新归一化 —— 归一化会让削波/DR 的相对份额
//       随本批版本数变化，而这一项是「有没有编码器」的绝对附加损失；
//   (b) 它在 0.95×Nyquist 处必须严格归零（否则「未检出截止」的满带宽版本与 0.951×Nyq 的版本
//       之间的距离会等于 12 分，又是一个阶跃）。
// 三个乘子都是**连续**的（这是 P2 的修法：判决不许由边际阈值决定）：
//   rRamp：0.95×Nyq（= fake_lossless 判「已接近奈奎斯特」的同一界线）→ 0；0.82×Nyq → 1。
//          0.82×Nyq @44.1k = 18.1 kHz（≈ 192k 级编码器），@48k = 19.7 kHz。
//          实测 320k MP3 = 0.918×Nyq ⟹ 约 0.25 满额；128k = 0.761×Nyq ⟹ 满额。
//   eRamp：陡沿 20 dB/⅙oct → 0，35 dB → 1。模拟母带链（磁带/动圈/ADC 抗混叠）实测 10~20 dB，
//          LAME 320k 29.6 dB、AAC 128k 42.7 dB ⟹ 这道斜坡把「自然滚降」和「编码器悬崖」分开，
//          但**用斜坡而不是 35 dB 的阶跃**：阶跃在 35 分贝上下抖 1 dB 又会翻 12 分。
//   cRamp：置信度 0.45 → 0，0.65 → 1（fake_lossless 的 0.40/0.50 两档正好落在斜坡内，不再是开关）。
inline constexpr double kCodecDamagePoints = 12.0;
inline constexpr double kCodecRatioFade  = 0.95;   // 到此归零（与「已接近奈奎斯特」同界）
inline constexpr double kCodecRatioFull  = 0.82;   // 到此满额
inline constexpr double kCodecEdgeLo     = 20.0;   // dB / 1/6 倍频程
inline constexpr double kCodecEdgeHi     = 35.0;
inline constexpr double kCodecConfLo     = 0.45;
inline constexpr double kCodecConfHi     = 0.65;

// 本版应扣的编解码损伤分（0..12）。纯函数：同一 VersionData 永远给出同一数值（无随机性）。
inline double codec_damage_points(const VersionData& v) {
    if (!(v.spectralCutoffHz > 0.0)) return 0.0;             // 未检出陡沿 ⟹ 视为满带宽，不罚
    const double nyq = (v.nyquistHz > 0.0) ? v.nyquistHz
                      : ((v.fs > 0) ? 0.5 * (double)v.fs : 22050.0);
    if (!(nyq > 0.0)) return 0.0;
    const double ratio = v.spectralCutoffHz / nyq;
    const double rRamp = std::clamp((kCodecRatioFade - ratio) / (kCodecRatioFade - kCodecRatioFull), 0.0, 1.0);
    const double eRamp = std::clamp((v.cutoffEdgeDb - kCodecEdgeLo) / (kCodecEdgeHi - kCodecEdgeLo), 0.0, 1.0);
    const double cRamp = std::clamp((v.cutoffConfidence - kCodecConfLo) / (kCodecConfHi - kCodecConfLo), 0.0, 1.0);
    return kCodecDamagePoints * rRamp * eRamp * cRamp;
}
// 削波：占比与连续段都罚，但【连续段罚得更重】—— 单个越限样本多为限幅器/抖动残留，
// 几乎不可闻；连续 ≥3 样本的平台才是真削波（听感是"糊/破"）。
//   占比项 20·log10(1+pct/0.01)：0.02% → 0.2 dB，1% → 6 dB，10% → 21 dB
//   段数项 12·log10(1+runs)：10 段 → 12.5 dB，1000 段 → 36 dB，10000 段 → 48 dB
inline double clip_score(const VersionData& v) {
    const double pct = v.quality.clippedPct;
    const double runs = (double)v.quality.clippedRuns;
    if (pct <= 0.0 && runs <= 0.0) return 100.0;
    const double penalty = 20.0 * std::log10(1.0 + pct / 0.01) + 12.0 * std::log10(1.0 + runs);
    return std::clamp(100.0 - penalty, 0.0, 100.0);
}

} // namespace vcdetail

// 填 rows / scores / ranking / explanation。可在 compare_versions 之后单独调用（便于测试）。
inline void score_versions(VersionCompare& r) {
    if (!r.ok) return;
    const size_t n = r.versions.size();

    // ---- 可比性判定（先算，再建行）----
    bool bitsSame = true, fsSame = true, chSame = true;
    for (size_t i = 1; i < n; ++i) {
        if (r.versions[i].bits != r.versions[0].bits) bitsSame = false;
        if (r.versions[i].channels != r.versions[0].channels) chSame = false;
        const double a = (double)r.versions[0].fs, b = (double)r.versions[i].fs;
        if (a > 0 && b > 0 && std::fabs(a - b) / std::max(a, b) > 0.2) fsSame = false;
    }
    const std::string whyBits = bitsSame ? std::string()
        : "版本间位深不同（" + std::to_string(r.versions[0].bits) + "-bit vs 其它）：本底噪声/SNR 受容器量化下限影响，不可直接比";
    const std::string whyFs = fsSame ? std::string()
        : "版本间采样率不同：奈奎斯特不同，频响类指标不在同一带宽内，不可直接比";
    // BUG-3（2026-10-27）：非谐波残留的 FFT 窗长随采样率变化（44.1k 1.49 s / 96k 0.68 s），
    // 同一录音 96/24 vs 44.1/16 实测漂 13.9 dB（> 本维度满罚 10 dB）⟹ 跨 fs 时与谱重心那几行同样处理。
    // 同采样率时这里为空 ⟹ 该维度照常可比、照常参与评分（不是一刀切关掉）。
    const std::string whyFsNonTonal = fsSame ? std::string()
        : "版本间采样率不同：非谐波残留的分析窗长随采样率变化（44.1k 1.49 s vs 96k 0.68 s），同一内容实测漂 13.9 dB，不可直接比";
    std::string whyCh = chSame ? std::string()
        : "版本间声道数不同（单声道 vs 立体声）：重合度/单声道兼容性不可比";
    if (whyCh.empty()) {
        for (const VersionData& v : r.versions)
            if (!v.quality.stereo) { whyCh = "存在非立体声（单声道或多声道）版本：立体声指标不适用"; break; }
    }

    r.rows.clear();
    using VD = const VersionData&;
    vcdetail::push_row(r, "dr_db", "动态范围 TT DR", "dB", true, false,
                       [](VD v) { return v.dynamicsTimes.drDb; });
    vcdetail::push_row(r, "lra", "短时动态 LRA", "LU", true, false,
                       [](VD v) { return v.dynamicsTimes.lra; });
    vcdetail::push_row(r, "crest_db", "峰值因子", "dB", true, false,
                       [](VD v) { return v.quality.crestDb; });
    vcdetail::push_row(r, "snr_db", "信噪比（全轨RMS−本底）", "dB", true, false,
                       [](VD v) { return v.quality.snrDb; }, whyBits);
    vcdetail::push_row(r, "noise_floor_dbfs", "本底噪声（最低 10% 块电平）", "dBFS", false, false,
                       [](VD v) { return v.quality.noiseFloorDbfs; }, whyBits);
    vcdetail::push_row(r, "non_tonal_db", "非谐波残留（噪声+失真上界）", "dB", false, false,
                       [](VD v) { return v.quality.nonTonalDb; }, whyFsNonTonal);
    vcdetail::push_row(r, "thdn_db", "THD+N（单基频素材才有）", "dB", false, false,
                       [](VD v) { return v.quality.thdnAvailable ? v.quality.thdnDb : -300.0; });
    vcdetail::push_row(r, "clipped_pct", "削波样本占比", "%", false, false,
                       [](VD v) { return v.quality.clippedPct; });
    vcdetail::push_row(r, "clipped_runs", "连续削波段数（≥3 样本）", "段", false, false,
                       [](VD v) { return (double)v.quality.clippedRuns; });
    // 第 8 维：有效带宽（信号判据，与容器位深/码率无关）。未检出陡沿 ⟹ 满带宽。
    vcdetail::push_row(r, "cutoff_hz", "有效带宽截止（长期平均谱）", "Hz", true, false,
                       [](VD v) { return vcdetail::effective_bandwidth(v); });
    vcdetail::push_row(r, "centroid_hz", "谱重心（brightness）", "Hz", true, true,
                       [](VD v) { return v.quality.centroidHz; }, whyFs);
    vcdetail::push_row(r, "tilt_db_oct", "频谱倾斜", "dB/oct", true, true,
                       [](VD v) { return v.quality.tiltDbPerOct; }, whyFs);
    vcdetail::push_row(r, "rolloff95_hz", "95% 谱滚降点", "Hz", true, true,
                       [](VD v) { return v.quality.rolloff95Hz; }, whyFs);
    vcdetail::push_row(r, "hf_ratio_10k_db", ">10 kHz 能量占比", "dB", true, false,
                       [](VD v) { return v.quality.hfRatio10kDb; }, whyFs);
    vcdetail::push_row(r, "channel_corr", "声道间相关", "", true, true,
                       [](VD v) { return v.quality.stereo ? v.quality.channelCorr : 0.0; }, whyCh);
    vcdetail::push_row(r, "mono_loss_db", "单声道化能量损失", "dB", false, false,
                       [](VD v) { return v.quality.stereo ? v.quality.monoLossDb : 0.0; }, whyCh);
    vcdetail::push_row(r, "dyn_p95_p10_db", "3 s 块 RMS 的 P95−P10", "dB", true, false,
                       [](VD v) { return v.quality.dynP95P10Db; });
    vcdetail::push_row(r, "dyn_centroid_dbfs", "动态重心（能量加权）", "dBFS", true, true,
                       [](VD v) { return v.quality.dynCentroidDbfs; });
    vcdetail::push_row(r, "peak_dbfs", "样本峰值", "dBFS", true, true,
                       [](VD v) { return v.quality.peakDbfs; });
    vcdetail::push_row(r, "lufs", "积分响度", "LUFS", true, true,
                       [](VD v) { return v.dynamicsTimes.lufsIntegrated; });

    // ---- 评分（相对最好版本的扣分，见文件头）----
    // 权重表（第 8 维 cutoff 于 2026-10-27 加入；合计 0.92，按可比维度权重和归一化）：
    //   cutoff 0.20（满罚 6 kHz ≈ 22.05−16 kHz = 128k 边界）
    //   dr 0.18 / clip 0.14 / snr 0.14 / crest 0.07 / lra 0.07 / non_tonal 0.08 / mono 0.04
    // 带宽 0.20 > 削波 0.14：即使有损版在削波维度上占尽便宜（最多 14 分），也翻不过带宽这一维。
    struct Dim { const char* key; double w; double fullDelta; bool higherBetter; };
    static const Dim dims[] = {
        { "cutoff_hz",    0.20, 6000.0, true  },  // 第 8 维：有效带宽（满罚 = 差 6 kHz）
        { "dr_db",        0.18,  6.0, true  },
        { "snr_db",       0.14, 20.0, true  },
        { "clipped_pct",  0.14,  0.0, false },   // 特殊：penalty 由 clip_penalty 给
        { "crest_db",     0.07,  4.0, true  },
        { "lra",          0.07,  6.0, true  },
        { "non_tonal_db", 0.08, 10.0, false },
        { "mono_loss_db", 0.04,  3.0, false },
    };
    auto row_by_key = [&](const std::string& k) -> const MetricRow* {
        for (const MetricRow& row : r.rows) if (row.key == k) return &row;
        return nullptr;
    };
    // 削波扣分：占比 0.5% 或 2000 段即满罚（取大者）—— 单样本越限几乎不可闻，连续削波才是硬伤
    auto clip_penalty = [&](size_t i) {
        const VersionData& v = r.versions[i];
        if (v.quality.clippedPct <= 0.0 && v.quality.clippedRuns == 0) return 0.0;
        const double pctP = std::min(1.0, v.quality.clippedPct / 0.5);
        const double runP = std::min(1.0, (double)v.quality.clippedRuns / 2000.0);
        return std::max(pctP, runP);
    };
    r.scores.assign(n, 0.0);
    r.dims.clear();
    r.dims.reserve(sizeof(dims) / sizeof(dims[0]));
    for (const Dim& d : dims) {
        VersionCompare::DimRow dr;
        dr.key = d.key;
        const MetricRow* row = row_by_key(d.key);
        dr.label = row ? row->label : d.key;
        dr.weight = d.w;
        dr.penalty.assign(n, 0.0);
        dr.score.assign(n, 100.0);
        r.dims.push_back(std::move(dr));
    }
    for (size_t i = 0; i < n; ++i) {
        double pen = 0.0, wsum = 0.0;
        for (size_t di = 0; di < r.dims.size(); ++di) {
            const Dim& d = dims[di];
            if (std::string(d.key) == "clipped_pct") {
                const MetricRow* row = row_by_key(d.key);
                if (!row || !row->comparable) continue;
                const double bestP = [&] {
                    double b = 1e18;
                    for (size_t k = 0; k < n; ++k) b = std::min(b, clip_penalty(k));
                    return b;
                }();
                const double p = std::min(1.0, clip_penalty(i) - bestP);
                r.dims[di].penalty[i] = p;
                r.dims[di].score[i] = 100.0 * (1.0 - p);
                pen += d.w * p;
                wsum += d.w;
                continue;
            }
            const MetricRow* row = row_by_key(d.key);
            if (!row || !row->comparable) continue;   // 不可比 ⟹ 不参与，权重重新归一化
            const double best = row->values[row->bestIndex];
            const double diff = d.higherBetter ? (best - row->values[i]) : (row->values[i] - best);
            const double penalty = (d.fullDelta > 0.0) ? std::min(1.0, std::max(0.0, diff) / d.fullDelta) : 0.0;
            r.dims[di].penalty[i] = penalty;
            r.dims[di].score[i] = 100.0 * (1.0 - penalty);
            pen += d.w * penalty;
            wsum += d.w;
        }
        r.scores[i] = (wsum > 0.0) ? 100.0 * (1.0 - pen / wsum) : 0.0;
        // 第二笔：编解码损伤（0..12 分，连续量；不是「判假 ⟹ −12」的开关）。
        // 与带宽维度罚的是不同性质的损失：带宽罚「少了多少频谱」，这一笔罚「留下的那段里被编码器动过」。
        const double codecPts = vcdetail::codec_damage_points(r.versions[i]);
        r.versions[i].codecPenaltyPoints = codecPts;
        r.scores[i] -= codecPts;
        if (r.scores[i] < 0.0) r.scores[i] = 0.0;
    }

    // ---- 排序 ----
    r.ranking.resize(n);
    for (size_t i = 0; i < n; ++i) r.ranking[i] = i;
    std::stable_sort(r.ranking.begin(), r.ranking.end(), [&](size_t a, size_t b) {
        if (r.scores[a] != r.scores[b]) return r.scores[a] > r.scores[b];
        return a < b;
    });

    // ---- 确定性人话（不依赖 AI；AI 层只在此基础上做自然语言润色）----
    std::ostringstream o;
    o << "共 " << n << " 个版本；已按 200 Hz 包络互相关对齐，"
      << "在公共区段 " << std::fixed << std::setprecision(1) << r.windowSec << " s 上重算全部指标"
      << "（最小包络相关 " << std::setprecision(3) << r.sameTrackCorr << "）。\n";
    if (r.sameTrackCorr < 0.70)
        o << "⚠ 包络相关 " << std::setprecision(2) << r.sameTrackCorr
          << " 偏低（同一首不同母带实测 0.53~0.56）：可能是母带/剪辑不同，也可能只是同一录音的不同转制；"
             "请自行确认这些文件确实是同一首。\n";
    o << "（分数是【本批次内】相对最好版本的扣分，跨批次不可比）\n综合排序：";
    for (size_t k = 0; k < n; ++k) {
        if (k) o << " > ";
        o << r.versions[r.ranking[k]].label << "(" << std::setprecision(1) << r.scores[r.ranking[k]] << ")";
    }
    o << "\n";
    // 每版的有效带宽与有损判定：这是排序里最容易被误读的一维，必须逐版写出来。
    // 有损版单独占一行，并在括号里给陡沿证据（reason 来自同一套 fake_lossless 算法）。
    {
        bool anyLossy = false;
        for (const VersionData& v : r.versions) anyLossy = anyLossy || v.lossySuspected;
        if (anyLossy) {
            o << "有效带宽 / 有损判定（长期平均谱，截止 = 谱密度连续低于 1–5 kHz 参考段 −40 dB 的最高频点）：\n";
            for (const VersionData& v : r.versions) {
                o << "  " << v.label << "：";
                if (v.spectralCutoffHz > 0.0)
                    o << std::setprecision(1) << (v.spectralCutoffHz / 1000.0) << " kHz";
                else
                    o << "未检出陡沿（满带宽）";
                if (v.lossySuspected) {
                    o << " ⟹ 疑似有损转码（" << v.cutoffReason << "，置信度 "
                      << std::setprecision(2) << v.cutoffConfidence << "）";
                } else if (v.spectralCutoffHz > 0.0) {
                    o << "（" << v.cutoffReason << "）";
                }
                // 两笔扣分分开写：用户要能看出「带宽损失」和「编解码损伤」是两件事，
                // 也要能看出后者在截止接近奈奎斯特时自动变小（不是一刀切 12 分）。
                if (v.codecPenaltyPoints > 0.05) {
                    o << "；编解码损伤 −" << std::setprecision(1) << v.codecPenaltyPoints
                      << " 分（满额 " << std::setprecision(0) << vcdetail::kCodecDamagePoints
                      << " 分，按陡沿/置信度/截止与奈奎斯特的距离平滑折算）";
                } else if (v.cutoffDetected) {
                    o << "；编解码损伤 0 分（截止已接近奈奎斯特，陡沿更可能是母带低通而非编码器）";
                }
                o << "\n";
            }
            o << "  说明：带宽维度罚的是「频谱少了多少」（连续量，按 Hz 折算），"
                 "编解码损伤罚的是「保留下来的那段频谱里被编码器动过」（预回声/量化/联合立体声）——"
                 "两笔性质不同，不重复罚同一个截止值。\n";
        }
    }
    const size_t best = r.ranking.front();
    for (size_t k = 1; k < n; ++k) {
        const size_t other = r.ranking[k];
        o << "为什么 " << r.versions[best].label << " 优于 " << r.versions[other].label << "：";
        bool any = false;
        for (const MetricRow& row : r.rows) {
            if (row.informational || !row.comparable) continue;
            const double a = row.values[best], b2 = row.values[other];
            const double d = a - b2;
            // 只报"够显著"的差异，避免噪声级差值堆成一大段
            double thr = 0.0;
            if (row.key == "dr_db" || row.key == "crest_db" || row.key == "lra") thr = 0.3;
            else if (row.key == "snr_db" || row.key == "noise_floor_dbfs" || row.key == "non_tonal_db") thr = 0.5;
            else if (row.key == "clipped_pct") thr = 1e-9;
            else if (row.key == "clipped_runs") thr = 0.5;
            else if (row.key == "mono_loss_db") thr = 0.2;
            else if (row.key == "cutoff_hz") thr = 500.0;
            else continue;                 // 信息性/无方向的维度不进解释
            if (std::fabs(d) < thr) continue;
            const bool better = row.higherBetter ? (d > 0) : (d < 0);
            if (!better) continue;         // 只列"赢"的维度，输的维度另有汇总
            any = true;
            if (row.key == "cutoff_hz") {
                o << " 有效带宽 " << std::setprecision(1) << (a / 1000.0) << " kHz vs "
                  << (b2 / 1000.0) << " kHz（" << (d > 0 ? "+" : "") << (d / 1000.0) << " kHz";
                if (r.versions[other].lossySuspected) o << "；后者为有损转码级截止";
                o << "）;";
                continue;
            }
            o << " " << row.label << " " << std::setprecision(2) << a << row.unit
              << " vs " << b2 << row.unit << "（" << (d > 0 ? "+" : "") << d << "）;";
        }
        if (!any) o << " 各评分维度差异都在噪声级以内（差别主要来自信息性指标，见对比表）。";
        o << "\n";
    }
    // 自检：若排序第 1 仍是有损版、而批次里存在无损版，说明权重再次失效，必须显式警告
    {
        const VersionData& top = r.versions[r.ranking.front()];
        bool anyClean = false;
        for (const VersionData& v : r.versions) if (!v.lossySuspected) anyClean = true;
        if (top.lossySuspected && anyClean)
            o << "⚠ 排序第 1 的 " << top.label
              << " 被判为疑似有损转码，却排在没有该标记的版本之前 —— 这是评分权重失效的信号，请勿据此选版本。\n";
    }
    // 不可比项
    std::vector<std::string> notComp;
    for (const MetricRow& row : r.rows) if (!row.comparable) notComp.push_back(row.label + "（" + row.why + "）");
    if (!notComp.empty()) {
        o << "不可比项（未参与评分）：";
        for (size_t i = 0; i < notComp.size(); ++i) { if (i) o << "；"; o << notComp[i]; }
        o << "\n";
    }

    // ---- 结论可信度（防御性提示；不改分数、不改名次、不改权重）----
    // 为什么必须有这一段：UI 上「V3 100.0 > V1 93.4」会被直接读成"V3 音质更好"，
    // 而实测分差里有一部分根本不是音质差异（见下面三条各自的实测数字）。
    r.caveats.clear();
    auto addCaveat = [&](const std::string& s) { r.caveats.push_back(s); };
    if (r.ranking.size() >= 2) {
        const size_t a = r.ranking[0], b2 = r.ranking[1];
        const double gap = r.scores[a] - r.scores[b2];
        if (gap < 1.0) {
            char buf[420];
            std::snprintf(buf, sizeof(buf),
                "⚠ 第 1 名 %s 与第 2 名 %s 只差 %.2f 分（< 1.0）：本工具的测量噪声就有这个量级"
                "（实测：同一内容只差前导静音 0.20 分、仅 48k 往返一次 0.67 分、同一次重采样换 soxr 2.47 分），"
                "分差 <1 分时名次不应被当作结论。",
                r.versions[a].label.c_str(), r.versions[b2].label.c_str(), gap);
            addCaveat(buf);
        }
    }
    {
        // 采样率不同：非谐波残留（权重 0.08）已标不可比、不参与评分（BUG-3 修）；DR/峰值仍可比但会随 fs 微变。
        double fsMin = 1e18, fsMax = 0.0;
        for (const VersionData& v : r.versions)
            if (v.fs) { fsMin = std::min(fsMin, (double)v.fs); fsMax = std::max(fsMax, (double)v.fs); }
        if (fsMax > fsMin * 1.0001) {
            char buf[1024];   // 中文按 UTF-8 3 字节/字，460 会把这条 caveat 截断（编译器已报 -Wformat-truncation）
            std::snprintf(buf, sizeof(buf),
                "⚠ 版本间采样率不同（%.1f vs %.1f kHz）：非谐波残留的分析窗长随 fs 变"
                "（44.1k 1.49 s vs 96k 0.68 s，同一内容实测漂 13.9 dB）⟹ 本轮起它在跨采样率对比中"
                "【标为不可比、不参与评分】（权重按可比维度重新归一化）；DR / 峰值 / 峰值因子仍会随采样率轻微变化；"
                "96 kHz 的升频/假高解析素材截止/容器奈奎斯特只有约 0.46 ⟹ 编解码损伤仍可能吃满额 −12 分"
                "（这是「拿容器奈奎斯特当参照」的副作用，不是它真有编码器损伤）。"
                "跨采样率的对比只建议看带宽/有损判定，不要看总分。",
                fsMin / 1000.0, fsMax / 1000.0);
            addCaveat(buf);
        }
    }
    for (const VersionData& v : r.versions) {
        // MP3/AAC 经 ffmpeg 解码是 32-bit WAV ⟹ 位深/SNR 这一组行会被判不可比，别当源文件属性
        if (v.bits >= 32 && v.lossySuspected) {   // 只有「解码器输出 32-bit」才是这个征状（24-bit 升频素材不适用）
            char buf[400];
            std::snprintf(buf, sizeof(buf),
                "⚠ %s 的位深 %d-bit 是【解码器输出】（MP3/AAC 经 ffmpeg 解成 32-bit WAV），不是源文件属性；"
                "它让 SNR/本底噪声被判为不可比，这两项（合计权重 0.14）因此不参与评分。",
                v.label.c_str(), v.bits);
            addCaveat(buf);
            break;
        }
    }
    for (const std::string& c : r.caveats) o << c << "\n";
    r.explanation = o.str();
}

} // namespace rta::analysis
