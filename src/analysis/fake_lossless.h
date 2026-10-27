// fake_lossless.h — 音频体检 B1a：假无损（有损源转制）检测 + 24-bit 容器 LSB 检查
//
// 用途：判断「容器/位深是无损，内容却像从有损版本转制」的素材，reason 直接供 UI 展示。
//
// 设计要点（原则：宁可漏报不可误报 —— 老录音天然窄带绝不判假）：
//  1) 频谱估计：全轨均匀取 ≤12 个 65536 样窗（radix-2 FFT，src/dsp/fft.h），加 Hann 窗
//     （压制乐音泛音的泄漏裙边对截止点判定的污染），幅度谱按窗平均；声道先混迭成单声道
//     （有损转制的截止特征各声道强相关，混迭不损失判据）。
//  2) 截止点：从高频向低频扫，找「谱密度连续 ≥8 bin 低于 [1k..5k] 段平均 −40 dB」的
//     最高一段的【低频端】f_cut（8 bin ≈ 5 Hz @44.1k/65536 点，滤掉单点谱陷）。
//  3) 陡沿：**每个谱窗用【自己的】截止点各量一次** f_cut 上下各 1/6 倍频程的能量差，
//     取各窗的【中位数】；>12 dB 才算 codec 式硬截止。温和滚降（磁带/老录音/动圈话筒）
//     在 −40 dB 点附近的局部斜率只有几十 dB/oct，换算到 1/6 倍频程只有几个 dB ——
//     判不过这条门，正是不误报老录音的关键。
//     ★ 2026-10-27 修复（标定报告 P1）：原实现是「12 个窗先平均幅度谱、再量一次陡沿」，
//       平均把陡沿抹平且窗越多抹得越狠 ⟹ 同一素材 30 s 未发现 / 3 s 有损，结论随片段长度翻转。
//  4) 截止 = 各窗各自截止的【低十分位】。安静窗的停带被量化噪声填满，−40 dB 相对门被噪声
//     顶住 ⟹ 单窗截止只会偏高不会偏低（单侧偏置），故低分位是最小偏估计；平均谱则会随窗数漂。
//  5) 置信度 = 陡沿 dB（12→25 dB 线性映射 0.5→1.0）× 窗间一致率（各窗截止落在报告的截止
//     max(1000 Hz, 5%) 内的比例）；置信度 <0.5 不判假。
//  6) 判定四档（2026-10-27 修复）：截止 <0.78×Nyq ⟹ 疑似有损；0.78~0.95×Nyq ⟹ 有效带宽
//     受限（母带低通/升频，不判假）；>0.95×Nyq ⟹ 未发现；素材过短/置信度擦线 ⟹ 证据不足。
//     另两条护栏：截止必须 ≥6 kHz 且 [f_cut/2, f_cut] 内 ≥120 bin 高于 ref−40 dB
//     （否则纯音/窄带会被当成"陡峭截止"）。
//  5) fake24bit 与 suspected 相互独立：srcBits ≥ 24 时把样本乘回整数域统计低 8 位，
//     恒零占比 >99.9% ⟹ 16-bit 内容装进 24-bit 容器（真 24-bit 素材低 8 位近似均匀，
//     恒零占比约 1/256 ≈ 0.4%，差 250 倍，判据余量充足）。
//
// ================== 增量（流式）接口 ==================
// 背景（261026-03 遗留项）：整曲体检要把整段样本握在手里才能取 12 个谱窗，DSD 大文件
//   因此峰值 ~2.6 GB。增量接口把「取哪 12 个窗」预先算成计划，边流边把窗内样本抄进
//   12×65536 的小缓冲（立体声 6.3 MB，与曲长无关），其余样本随到随丢。
//
// ★ 前提与硬约定：
//   - 窗位置 start[w] 依赖【总帧数】（全轨均匀铺满），所以 begin() 必须先拿到总帧数
//     （DSD/WAV 从文件头就知道；拿不到就别用增量接口）。
//   - 同一组样本，check_lossless(整段) 与 LosslessAccumulator（任意分块）的输出必须
//     【逐位一致（==）】：cutoffHz / confidence / suspected / fake24bit / reason 全同。
//     实现上 check_lossless 就是「按总帧数建计划 + 整段喂 + finalize」的包装。
//     分块一致性靠两点：① 每块样本先按【同一顺序】混迭成单声道（Σ_c ch[c][i]/channels，
//     声道升序、i 升序），窗重叠也只重放同一个已算好的值；② 统计量是整数计数。
//   - 若实际喂进来的帧数 != begin() 声明的总帧数（截断/读失败），计划就不成立，
//     planMatched() 返回 false，调用方必须回退整段路径 —— 不要拿它的结果当数。
#pragma once
#include "../dsp/fft.h"      // rta::dsp::fft（radix-2，n 须为 2 的幂）/ kPi
#include <string>
#include <vector>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstddef>
#include <cstring>

namespace rta::analysis {

// 判定档（2026-10-27 修复轮）：把「疑似有损」从二元扩成四档 —— UI 不能只会给绿/红。
//   0 未发现   —— 没有构成判据的证据（可能什么都没检出，也可能检出但边沿不陡/太窄带/置信度不够）
//   1 疑似有损 —— 截止 < 0.78×奈奎斯特 + 陡沿 + 置信度 ≥0.5（**判假**）
//   2 带宽受限 —— 截止落在 0.78~0.95×奈奎斯特：母带低通/升频也长这样，**不判假**
//                 （依据：320k MP3 实测 19979 Hz = 0.906×Nyq @44.1k，正落此档；
//                   库内 98 首 18~20 kHz 被旧的绝对门 20500 一刀切成"有损"，改档后不再判假）
//   3 证据不足 —— 素材过短（谱窗填充 <50%）或置信度擦线（0.40~0.50）：不给确定结论
constexpr int kFakeVerdictNone       = 0;
constexpr int kFakeVerdictLossy      = 1;
constexpr int kFakeVerdictBandLimited = 2;
constexpr int kFakeVerdictInconclusive = 3;

struct LosslessCheck {
    bool   suspected = false;   // 疑似有损源转制（假无损）⟺ fakeVerdict == kFakeVerdictLossy
    double cutoffHz = 0.0;      // 检出的频谱截止（0=未见）
    double confidence = 0.0;    // 0..1
    std::string reason;         // 中文人话，供 UI 直接显示
    bool   fake24bit = false;   // LSB 恒零（16bit 内容搬进 24bit 容器）

    // ---- 判定档 + 判据量（2026-10-27 修复轮；UI 直接显示，别在 JS 里重算）----
    int    fakeVerdict = kFakeVerdictNone;   // 见上面的四档常量
    double nyquistHz = 0.0;     // 本次分析用的奈奎斯特（= fs/2）
    double cutoffRatio = 0.0;   // cutoffHz / nyquistHz（0 = 未检出）

    // ---- 诊断字段（2026-10-27 DEBUG 标定轮新增；windows / windowFill 只影响措辞与判定档）----
    // 为什么要暴露它们：UI 只显示一句话时无法区分「没检出」「检出但边沿不陡」「检出但置信度不够」。
    // 标定实测（tools/fl_probe.cpp + build-fl/media 的编码矩阵）需要逐档看到这些量，
    // 否则「320k MP3 为什么没报警」这类问题只能靠猜。
    double edgeDb = 0.0;        // 陡沿：**各窗**在 f_cut 处 1/6 倍频程能量差的【中位数】（dB）
    int    windows = 0;         // 实际参与的谱窗数（满额 12）
    double windowFill = 1.0;    // 谱窗填充率：<1 表示总样本短于一个 65536 点窗（尾部补零）
    int    guardBins = 0;       // [f_cut/2, f_cut] 内高于参考段 −40 dB 的 bin 数（P3 纯音护栏）
};

namespace detail {

// 从高频向低频扫：找第一段「连续 ≥minRun bin 低于阈值」的 run 的低频端 bin（无则 0）。
// kTop 为扫描起点（含），kLow 为终点（含）。扫到底仍是同一段（整带皆阈下，近纯音/超窄带）
// 时不报 —— 宁可漏报。
inline size_t scan_cutoff_bin(const std::vector<double>& magDb, double thrDb,
                              size_t kTop, size_t kLow, size_t minRun) {
    size_t run = 0;
    for (size_t k = kTop + 1; k-- > kLow;) {
        if (magDb[k] < thrDb) { ++run; continue; }
        if (run >= minRun) return k + 1;   // 刚结束的这段 run 的低频端
        run = 0;
    }
    return 0;
}

// 块内取样本的三个取值器（平面 / 交错 double / 交错 float）。i 一律是【本块内的帧偏移】。
struct PlanarGet {
    const double* const* ch;
    double operator()(size_t c, size_t i) const { return ch[c][i]; }
};
struct InterleavedGet {
    const double* p;
    size_t nch;
    double operator()(size_t c, size_t i) const { return p[i * nch + c]; }
};
struct InterleavedF32Get {
    const float* p;
    size_t nch;
    double operator()(size_t c, size_t i) const { return (double)p[i * nch + c]; }
};

// ---- 谱窗计划：整段接口与增量接口共用（窗位置的唯一来源）----
struct LosslessWindowPlan {
    size_t kW = 65536;                  // radix-2，且窗越长谱密度分辨率越高
    size_t half = 32768;
    size_t nWin = 1;
    double binHz = 1.0;
    std::vector<size_t> start;          // 各窗起始帧（全轨均匀铺满：首窗贴头、末窗贴尾）
    size_t kRefLo = 1, kRefHi = 0;      // 参考段 [1k..5k]
    bool   refOk = false;
};

inline LosslessWindowPlan make_window_plan(size_t n, double fs) {
    LosslessWindowPlan p;
    p.kW = 65536;
    p.half = p.kW / 2;
    p.binHz = fs / (double)p.kW;
    p.nWin = std::max<size_t>(1, std::min<size_t>(12, n / p.kW));
    p.start.assign(p.nWin, 0);
    for (size_t w = 1; w < p.nWin; ++w)
        p.start[w] = (n > p.kW) ? w * (n - p.kW) / (p.nWin - 1) : 0;
    p.kRefLo = (size_t)std::max(1.0, std::floor(1000.0 / p.binHz));
    p.kRefHi = std::min(p.half - 1, (size_t)std::ceil(5000.0 / p.binHz));
    p.refOk = (p.kRefHi > p.kRefLo);
    return p;
}

// 频谱部分：winMix[w][i] = 第 w 个窗第 i 点的「已混迭成单声道」原值（未加窗）。
// 整段接口与增量接口都把样本收进这个形态，再走同一段算术 ⟹ 逐位一致。
inline void spectrum_from_windows(const LosslessWindowPlan& plan,
                                  const std::vector<std::vector<double>>& winMix,
                                  LosslessCheck& out, double windowFill = 1.0) {
    const size_t kW = plan.kW, half = plan.half, nWin = plan.nWin;
    const double binHz = plan.binHz;
    out.windowFill = windowFill;
    out.nyquistHz = 0.5 * binHz * (double)kW;

    // 各窗加 Hann → FFT → 幅度谱；同时保留各窗自己的 dB 谱（窗间一致率用）与
    // 线性幅度谱（陡沿逐窗量、取中位数用 —— 见下面 P1 的说明）
    std::vector<double> avgMag(half + 1, 0.0);
    std::vector<std::vector<double>> winDb;
    winDb.reserve(nWin);
    std::vector<std::vector<double>> winMag;
    winMag.reserve(nWin);
    std::vector<rta::dsp::cplx> buf(kW);
    for (size_t w = 0; w < nWin; ++w) {
        const std::vector<double>& src = winMix[w];
        for (size_t i = 0; i < kW; ++i) {
            const double mix = src[i];
            const double hann = 0.5 * (1.0 - std::cos(2.0 * rta::dsp::kPi * (double)i / (double)(kW - 1)));
            buf[i] = rta::dsp::cplx(mix * hann, 0.0);
        }
        rta::dsp::fft(buf, false);
        std::vector<double> mdb(half + 1, -300.0);
        std::vector<double> ml(half + 1, 0.0);
        for (size_t k = 0; k <= half; ++k) {
            const double m = std::abs(buf[k]);
            avgMag[k] += m;
            ml[k] = m;
            mdb[k] = 20.0 * std::log10(m + 1e-300);
        }
        winDb.push_back(std::move(mdb));
        winMag.push_back(std::move(ml));
    }
    for (size_t k = 0; k <= half; ++k) avgMag[k] /= (double)nWin;

    // 参考段 [1k..5k] 平均谱密度（线性均值，混迭与窗函数的常数尺度在相对比较中抵消）
    const size_t kRefLo = plan.kRefLo;
    const size_t kRefHi = plan.kRefHi;
    if (!plan.refOk) { out.reason = "采样率异常，无法分析"; return; }
    double refSum = 0.0;
    for (size_t k = kRefLo; k <= kRefHi; ++k) refSum += avgMag[k];
    const double refDb = 20.0 * std::log10(refSum / (double)(kRefHi - kRefLo + 1) + 1e-300);

    std::vector<double> avgDb(half + 1, -300.0);
    for (size_t k = 0; k <= half; ++k) avgDb[k] = 20.0 * std::log10(avgMag[k] + 1e-300);

    // 平均谱上再扫一次：只用来判「到底有没有截止」（后面每个窗都会各自扫一次）
    const size_t kCutAvg = scan_cutoff_bin(avgDb, refDb - 40.0, half, kRefLo, 8);

    // ★ P1（2026-10-27 修复）：截止与陡沿都【逐窗各算一次】。
    // 原实现是「12 个窗先平均幅度谱、再在平均谱上量一次截止 + 一次陡沿」：
    //   · 陡沿被平均抹平且窗口越多抹得越狠（同一 16 kHz(8 阶) 素材 2 窗 21.9 dB → 6 窗 19.2 → 12 窗 18.1）；
    //   · 截止位置也跟着漂（同一素材 3 s 19.86 kHz / 20 s 21.5 kHz）。
    // 于是「30 s 未发现(0.43)、3 s 有损(0.76)」——同一素材因片段长度给出相反结论。
    // 逐窗各量一次后同一 16 kHz 素材：30 s 22.0 / 10 s 22.0 / 3 s 20.9 dB（与长度无关）。
    // ★ 为什么报"低十分位"而不是中位数当截止：安静的窗里，停带被 16-bit 量化噪声填满，
    //   −40 dB 相对门被噪声顶住 ⟹ 该窗的截止只能【偏高】，不会偏低（物理上的单侧偏置）。
    //   所以低四分位才是"该素材的有效带宽"的最小偏估计；中位数会被安静窗系统性抬高。
    // ★ 陡沿一律量在【本窗自己的】截止点周围：拿别人的截止去套，会量到过渡带里，
    //   同一素材实测从 22.0 dB 被压到 17.9 dB。
    double cutoffHz = 0.0, edgeDb = 0.0, consist = 0.0;
    size_t guardBins = 0;
    if (kCutAvg > 0) {
        const double oct6 = std::pow(2.0, 1.0 / 6.0);
        std::vector<double> edges, cuts;
        edges.reserve(nWin);
        cuts.reserve(nWin);
        for (size_t w = 0; w < nWin; ++w) {
            const std::vector<double>& mdb = winDb[w];
            const std::vector<double>& mag = winMag[w];
            double rs = 0.0;
            for (size_t k = kRefLo; k <= kRefHi; ++k) rs += mag[k];
            const double lthr = 20.0 * std::log10(rs / (double)(kRefHi - kRefLo + 1) + 1e-300) - 40.0;
            const size_t kc = scan_cutoff_bin(mdb, lthr, half, kRefLo, 8);
            if (kc == 0) continue;
            const double cutW = (double)kc * binHz;
            cuts.push_back(cutW);
            const size_t kLo = (size_t)std::floor(cutW / oct6 / binHz);
            const size_t kHi = std::min(half, (size_t)std::ceil(cutW * oct6 / binHz));
            double eLo = 0.0, eHi = 0.0;
            for (size_t k = kLo; k < kc; ++k) eLo += mag[k] * mag[k];
            for (size_t k = kc; k <= kHi; ++k) eHi += mag[k] * mag[k];
            if (eLo > 0.0 && eHi > 0.0) edges.push_back(10.0 * std::log10(eLo / eHi));
        }
        // 截止 = 各窗截止的低十分位（见上面的单侧偏置理由；取 10% 而不是最小值，
        // 是为了不让单个异常窗（淡出段/静音段）把结论拖到很低）
        if (!cuts.empty()) {
            std::sort(cuts.begin(), cuts.end());
            const double pos = 0.10 * (double)(cuts.size() - 1);
            const size_t i0 = (size_t)pos;
            const size_t i1 = std::min(cuts.size() - 1, i0 + 1);
            cutoffHz = cuts[i0] * (1.0 - (pos - (double)i0)) + cuts[i1] * (pos - (double)i0);
        } else {
            cutoffHz = (double)kCutAvg * binHz;
        }
        // 陡沿 = 各窗陡沿的中位数（对个别被内容污染的窗免疫）
        if (!edges.empty()) {
            std::sort(edges.begin(), edges.end());
            const size_t m = edges.size();
            edgeDb = (m % 2) ? edges[m / 2] : 0.5 * (edges[m / 2 - 1] + edges[m / 2]);
        }
        // 窗间一致率：各窗截止与报告的截止的偏离 < max(1000 Hz, 5%) 算一致
        // （5% 是为了容纳「同一母带不同段落本身带宽就有细微差别」；固定 1000 Hz 在 20 kHz
        //   附近只有 5%，而实测同一首真实曲的窗间差异可达 ±1.5 kHz）
        const double tol = std::max(1000.0, 0.05 * cutoffHz);
        size_t matched = 0;
        for (double c : cuts) if (std::fabs(c - cutoffHz) <= tol) ++matched;
        consist = (double)matched / (double)nWin;
        // ★ P3 护栏（2026-10-27 修复）：截止必须落在乐音区（≥6 kHz），且 [f_cut/2, f_cut]
        // 这一段真有宽带能量（≥120 bin 高于参考段 −40 dB）。否则 997 Hz 纯音/narrowband
        // 会被当成「1.2 kHz 陡峭截止」判成有损（标定实测 1/1 误报，而且文案还指向错误原因）。
        const size_t kCutB = (size_t)std::llround(cutoffHz / binHz);
        for (size_t k = kCutB / 2; k <= kCutB && k <= half; ++k)
            if (avgDb[k] > refDb - 40.0) ++guardBins;
    }
    const bool steep = edgeDb > 12.0;
    const bool wideEnough = (cutoffHz >= 6000.0 && guardBins >= 120);

    out.cutoffHz = cutoffHz;
    out.edgeDb = edgeDb;
    out.windows = (int)nWin;
    out.guardBins = (int)guardBins;
    out.cutoffRatio = (out.nyquistHz > 0.0) ? cutoffHz / out.nyquistHz : 0.0;

    const double confRaw = (cutoffHz > 0.0)
        ? std::clamp((edgeDb - 12.0) / (25.0 - 12.0), 0.0, 1.0) * consist
        : 0.0;
    // ★ P4（2026-10-27 修复）：谱窗填充 50%~90%（约 0.74~1.34 s @44.1k）时素材偏短，
    // 陡沿与窗间一致率都只由一两个窗决定 ⟹ 置信度打七折并在 reason 里写明。
    // 填充 <50%（<0.74 s）另行处理：直接不给确定结论（见下），因为实测 6/6 已无判别力。
    const bool shortDiscount = (windowFill >= 0.5 && windowFill < 0.9);
    out.confidence = shortDiscount ? confRaw * 0.7 : confRaw;

    // ---- 四档判定（两档相对奈奎斯特 + 陡沿 + 两条护栏 + 置信度）----
    int verdict = kFakeVerdictNone;
    if (cutoffHz <= 0.0) {
        verdict = kFakeVerdictNone;                       // 根本没有截止
    } else if (windowFill < 0.5) {
        verdict = kFakeVerdictInconclusive;               // P4：素材过短，不构成判据
    } else if (!steep) {
        verdict = kFakeVerdictNone;                       // 缓坡：老录音/磁带/话筒，放过的关键
    } else if (!wideEnough) {
        verdict = kFakeVerdictNone;                       // P3：纯音/窄带，不构成判据
    } else if (out.cutoffRatio < 0.78) {
        // 有损编码器带宽档：<0.78×Nyq（44.1k ⟹ 17.2 kHz）
        verdict = (out.confidence >= 0.5) ? kFakeVerdictLossy
                : (out.confidence >= 0.40 ? kFakeVerdictInconclusive : kFakeVerdictNone);
    } else if (out.cutoffRatio <= 0.95) {
        // 母带低通/升频档：不判假，只报告"有效带宽受限"。
        // ★ 这一档【不设置信度门】：它描述的是"信号的有效带宽到哪里"，只要陡沿过门、
        //   位置在 78%~95% 之间就成立，置信度只用来修饰措辞（"证据强度偏低"）。
        //   理由（2026-10-27 实测）：真实曲目各段落本身带宽就有 ±1.5 kHz 差别，
        //   20 s 与 3 s 片段的置信度可以差 4 倍（0.19 vs 1.00），若让结论卡在置信度门上，
        //   同一首歌听 20 秒说"未发现"、听 3 秒说"带宽受限"—— 又变成"结论随长度翻转"。
        // ★ 唯一例外：下降沿达到【编码器级】≥40 dB/6 倍频程、且窗数 ≥6、置信度 ≥0.9 时判有损。
        //   依据：模拟母带链实测只有 10~20 dB；LAME 320k 29.6 dB、AAC 128k 42.7 dB。
        //   要求 6 个窗以上，是为了不让"3 秒片段里一个陡峭的淡出"单独触发判假。
        const bool codecGradeEdge = (edgeDb >= 40.0 && nWin >= 6 && out.confidence >= 0.9);
        verdict = codecGradeEdge ? kFakeVerdictLossy : kFakeVerdictBandLimited;
    } else {
        verdict = kFakeVerdictNone;                       // 已接近奈奎斯特
    }
    out.fakeVerdict = verdict;
    out.suspected = (verdict == kFakeVerdictLossy);

    char msg[384];
    if (verdict == kFakeVerdictLossy) {
        if (out.cutoffRatio >= 0.78) {
            std::snprintf(msg, sizeof(msg),
                          "%.1f kHz 处存在陡峭截止（下降 %.0f dB，约为奈奎斯特的 %.0f%%）；"
                          "虽然截止位置属于「母带低通」档，但下降沿 %.0f dB/6 倍频程是编码器级"
                          "（模拟母带链只有 10~20 dB），判有损源转制",
                          cutoffHz / 1000.0, edgeDb, out.cutoffRatio * 100.0, edgeDb);
        } else {
            std::snprintf(msg, sizeof(msg),
                          "%.1f kHz 处存在陡峭截止（下降 %.0f dB，约为奈奎斯特的 %.0f%%），典型的有损源转制特征",
                          cutoffHz / 1000.0, edgeDb, out.cutoffRatio * 100.0);
        }
    } else if (verdict == kFakeVerdictBandLimited) {
        std::snprintf(msg, sizeof(msg),
                      "有效带宽受限：%.1f kHz 处有陡峭截止（下降 %.0f dB，约为奈奎斯特的 %.0f%%），"
                      "置信度 %.0f%%%s。母带低通/升频也长这样（此档 78%%~95%%），不据此判假",
                      cutoffHz / 1000.0, edgeDb, out.cutoffRatio * 100.0, out.confidence * 100.0,
                      (out.confidence >= 0.9 ? "" : "（各谱窗不一致，证据强度偏低）"));
    } else if (verdict == kFakeVerdictInconclusive) {
        if (windowFill < 0.5) {
            std::snprintf(msg, sizeof(msg),
                          "素材过短（谱窗仅 %.0f%% 有实样），不构成判据（需 ≥1.5 s）；"
                          "检出 %.1f kHz、下降沿 %.0f dB，方向与长片段一致",
                          windowFill * 100.0, cutoffHz / 1000.0, edgeDb);
        } else {
            std::snprintf(msg, sizeof(msg),
                          "置信度未达阈值（%.2f<0.50），检出 %.1f kHz、下降沿 %.0f dB ⟹ 证据不足",
                          out.confidence, cutoffHz / 1000.0, edgeDb);
        }
    } else if (cutoffHz <= 0.0) {
        std::snprintf(msg, sizeof(msg), "未检出明显频谱截止");
    } else if (cutoffHz < 6000.0) {
        std::snprintf(msg, sizeof(msg),
                      "检出 %.1f kHz 附近的谱下降，但低于 6 kHz（纯音/极窄带素材），不构成转码判据",
                      cutoffHz / 1000.0);
    } else if (guardBins < 120) {
        std::snprintf(msg, sizeof(msg),
                      "检出 %.1f kHz 附近的谱下降，但该段窄带内能量不足（仅 %d bin，疑似纯音/窄带），不构成判据",
                      cutoffHz / 1000.0, (int)guardBins);
    } else if (!steep) {
        std::snprintf(msg, sizeof(msg),
                      "%.1f kHz 处频谱走低但下降沿缓和（1/6 倍频程约 %.0f dB），更像老录音自然滚降，不判假",
                      cutoffHz / 1000.0, edgeDb);
    } else if (out.cutoffRatio > 0.95) {
        std::snprintf(msg, sizeof(msg),
                      "检出 %.1f kHz 附近的频谱下降（已接近奈奎斯特 %.1f kHz，不判假）",
                      cutoffHz / 1000.0, out.nyquistHz / 1000.0);
    } else {
        std::snprintf(msg, sizeof(msg),
                      "检出 %.1f kHz、下降沿 %.0f dB、置信度 %.0f%% ⟹ 未达判据（需 ≥50%%）",
                      cutoffHz / 1000.0, edgeDb, out.confidence * 100.0);
    }
    if (shortDiscount) {
        char caveat[128];
        std::snprintf(caveat, sizeof(caveat),
                      "；素材偏短（谱窗 %.0f%% 有实样），置信度已按 70%% 折算", windowFill * 100.0);
        std::strncat(msg, caveat, sizeof(msg) - std::strlen(msg) - 1);
    }
    out.reason = msg;
}

} // namespace detail

// ==================== 增量（流式）假无损分析器 ====================
//
// 用法：
//     LosslessAccumulator acc;
//     acc.begin(totalFrames, channels, fs, srcBits);      // 总帧数必须已知（窗位置取决于它）
//     while (还有数据) acc.add_interleaved(buf, frames);   // 或 add_planar / add_interleaved_f32
//     const LosslessCheck c = acc.finalize();             // 先看 planMatched()
class LosslessAccumulator {
public:
    // active: 每声道 1/0（nullptr = 全有效）
    void begin(size_t totalFrames, int channels, double fs, int srcBits, const char* active = nullptr) {
        totalFrames_ = totalFrames;
        channels_ = channels > 0 ? channels : 0;
        fs_ = fs;
        srcBits_ = srcBits;
        pos_ = 0;
        tot_ = 0;
        low0_ = 0;
        scale_ = std::pow(2.0, (double)(srcBits - 1));
        active_.assign((size_t)channels_, 0);
        for (int c = 0; c < channels_; ++c)
            active_[(size_t)c] = (active ? (active[c] ? 1 : 0) : 1);
        bad_ = (totalFrames_ == 0 || channels_ <= 0 || fs_ <= 0.0);
        if (bad_) {
            plan_ = detail::LosslessWindowPlan();
            plan_.start.clear();
            winMix_.clear();
            mixBuf_.clear();
            return;
        }
        plan_ = detail::make_window_plan(totalFrames_, fs_);
        winMix_.clear();
        winMix_.resize(plan_.nWin);
        for (size_t w = 0; w < plan_.nWin; ++w) winMix_[w].assign(plan_.kW, 0.0);
        mixBuf_.clear();
    }

    // 平面 double。★ 寻址约定（与 add_interleaved 一致）：ch[c] 指向【本块第 0 帧】，
    // 即块内相对寻址 ch[c][0..frames-1]；块在整轨里的绝对位置由内部游标 pos_ 负责。
    // —— 2026-10-27 实测踩过的坑：早先这里写成 ch[c][pos_ + i]，而调用方传的是已偏移的
    //    指针（ch[c] + pos），偏移被算了两次，分块跑到 441000 帧时读越界崩溃（ASan 抓到）。
    // ch[c] 为空 ⟹ 该声道按"无数据"处理（混迭时贡献 0，与整段接口一致）
    void add_planar(const double* const* ch, size_t frames) {
        if (bad_ || frames == 0) return;
        if (!ch) { pos_ += frames; return; }
        for (int c = 0; c < channels_; ++c) if (!ch[c]) active_[(size_t)c] = 0;
        const detail::PlanarGet get{ ch };
        capture(get, frames);
        if (srcBits_ >= 24) fake24_planar(ch, frames);
        pos_ += frames;
    }

    // 交错 double（interleaved，块内相对寻址 interleaved[i*channels + c]）
    void add_interleaved(const double* interleaved, size_t frames) {
        if (bad_ || frames == 0) return;
        if (!interleaved) { pos_ += frames; return; }
        const detail::InterleavedGet get{ interleaved, (size_t)channels_ };
        capture(get, frames);
        if (srcBits_ >= 24) fake24_interleaved(interleaved, (size_t)channels_, frames);
        pos_ += frames;
    }

    // 交错 float（便利重载；float 只有 24 bit 尾数，与 double 整段接口不保证逐位一致）
    void add_interleaved_f32(const float* interleaved, size_t frames) {
        if (bad_ || frames == 0) return;
        if (!interleaved) { pos_ += frames; return; }
        const detail::InterleavedF32Get get{ interleaved, (size_t)channels_ };
        capture(get, frames);
        if (srcBits_ >= 24) {
            const size_t nch = (size_t)channels_;
            for (size_t c = 0; c < nch; ++c) {
                if (!active_[c]) continue;
                for (size_t i = 0; i < frames; ++i) {
                    const long long v = (long long)std::llround((double)interleaved[i * nch + c] * scale_);
                    ++tot_;
                    if ((v & 0xFFll) == 0) ++low0_;
                }
            }
        }
        pos_ += frames;
    }

    // 结果。planMatched() == false 时【不可信】（窗位置与实际帧数不符）—— 调用方须回退整段路径。
    LosslessCheck finalize() const {
        LosslessCheck out;
        if (bad_) { out.reason = "样本不足，无法分析"; return out; }
        // 谱窗填充率：总帧数不足一个 65536 点窗时，winMix_ 尾部全是补零。补零本身不改变
        // 截止判据的相对门，但它让「每个窗」都退化成同一个窗、且窗内数据只占窗长的一部分，
        // 因此窗间一致率与置信度的含义都变弱 —— 判定档与 reason 里都要说人话
        // （2026-10-27 修复：<50% 直接判「证据不足」，50%~90% 置信度打七折）。
        out.windowFill = (plan_.kW > 0)
            ? std::min(1.0, (double)totalFrames_ / (double)plan_.kW) : 1.0;
        detail::spectrum_from_windows(plan_, winMix_, out, out.windowFill);
        // fake24bit：srcBits ≥ 24 才有意义。乘回整数域看低 8 位 —— 16-bit 内容左移装填时恒零，
        // 真 24-bit 素材低 8 位近似均匀（≈0.4% 恒零），阈值 99.9% 两侧余量都很大。
        if (srcBits_ >= 24 && tot_ > 0 && (double)low0_ / (double)tot_ > 0.999) {
            out.fake24bit = true;
            out.reason += "；低 8 位恒零，疑似 16-bit 内容装入 24-bit 容器";
        }
        return out;
    }

    bool   planMatched() const { return !bad_ && pos_ == totalFrames_; }
    bool   bad() const { return bad_; }
    size_t framesSeen() const { return pos_; }
    size_t plannedFrames() const { return totalFrames_; }
    size_t windowCount() const { return plan_.nWin; }
    // 计划占用的窗缓冲字节数（诊断/内存报告用）
    size_t windowBytes() const { return plan_.nWin * plan_.kW * sizeof(double); }

private:
    // fake24：把样本乘回整数域统计低 8 位（整数计数，顺序无关；整块全扫，不只谱窗）
    void fake24_planar(const double* const* ch, size_t frames) {
        const size_t nch = (size_t)channels_;
        for (size_t c = 0; c < nch; ++c) {
            if (!active_[c]) continue;
            const double* p = ch[c];                  // 块内相对寻址
            for (size_t i = 0; i < frames; ++i) {
                const long long v = (long long)std::llround(p[i] * scale_);
                ++tot_;
                if ((v & 0xFFll) == 0) ++low0_;
            }
        }
    }
    void fake24_interleaved(const double* interleaved, size_t nch, size_t frames) {
        for (size_t c = 0; c < nch; ++c) {
            if (!active_[c]) continue;
            for (size_t i = 0; i < frames; ++i) {
                const long long v = (long long)std::llround(interleaved[i * nch + c] * scale_);
                ++tot_;
                if ((v & 0xFFll) == 0) ++low0_;
            }
        }
    }

    // 把与 [pos_, pos_+frames) 相交的谱窗区间抄进 winMix_。
    // 只有落入谱窗的帧才做混迭（12 个窗 × 65536 = 786k 帧，与曲长无关）；
    // 全轨逐帧混迭会白烧 67.7M 次加法（实测 +1.3 s / 271 MB DSD）。
    template <class Get>
    void capture(const Get& get, size_t frames) {
        const size_t begin = pos_, end = begin + frames;
        for (size_t w = 0; w < plan_.nWin; ++w) {
            const size_t ws = plan_.start[w];
            const size_t we = ws + plan_.kW;
            if (we <= begin || ws >= end) continue;
            const size_t lo = (begin > ws) ? begin : ws;
            const size_t hi = (end < we) ? end : we;
            const size_t cnt = hi - lo;
            mixBuf_.resize(cnt);
            for (size_t i = 0; i < cnt; ++i) {
                double mix = 0.0;
                const size_t rel = lo - begin + i;      // 本块内的帧偏移
                for (size_t c = 0; c < (size_t)channels_; ++c) {
                    if (!active_[c]) continue;
                    mix += get(c, rel);
                }
                mixBuf_[i] = mix / (double)channels_;
            }
            double* dst = winMix_[w].data();
            for (size_t g = lo; g < hi; ++g) dst[g - ws] = mixBuf_[g - lo];
        }
    }

    size_t totalFrames_ = 0;
    int    channels_ = 0;
    double fs_ = 0.0;
    int    srcBits_ = 0;
    double scale_ = 1.0;
    bool   bad_ = false;
    size_t pos_ = 0;
    size_t tot_ = 0, low0_ = 0;
    std::vector<char> active_;
    detail::LosslessWindowPlan plan_;
    std::vector<std::vector<double>> winMix_;
    std::vector<double> mixBuf_;      // 当前块的单声道混迭值（复用，容量随块大小）
};

// 一次性接口：整段平面 double。实现上是「建计划 + 整段喂 + finalize」，与增量接口逐位一致。
inline LosslessCheck check_lossless(const double* const* ch, int channels, size_t n, double fs, int srcBits) {
    LosslessCheck out;
    if (!ch || channels <= 0 || n == 0 || fs <= 0.0) {
        out.reason = "样本不足，无法分析";
        return out;
    }
    std::vector<char> active((size_t)channels, 0);
    for (int c = 0; c < channels; ++c) active[(size_t)c] = ch[c] ? 1 : 0;
    LosslessAccumulator acc;
    acc.begin(n, channels, fs, srcBits, active.data());
    acc.add_planar(ch, n);
    return acc.finalize();
}

} // namespace rta::analysis
