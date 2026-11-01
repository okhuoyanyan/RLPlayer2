// mic_cal.h — 测量麦克风校准曲线：解析 / 插值 / 频域补偿（2026-10-29）
//
// 用户需求（原文）：「设置-声音校准，缺少导入校正麦克风曲线功能，有一部分出厂校准的麦克风
// 是自带校准文件的。」UMIK-1 / UMIK-2 / Dayton EMM-6 这类测量话筒出厂自带一份频响校准
// 文件（.txt/.csv，两列或三列：频率 Hz、dB、可选相位）。
//
// 为什么必须补偿：房间校正测到的是「房间 + 话筒」的合成响应。话筒自身频响（典型 ±2~5 dB，
// 超低/超高处可达 ±10 dB）不扣掉，就会被当成房间的峰谷写进校正 FIR —— 用户听到的是
// 「把话筒的误差反向加进房间」。补偿 = 读数 − 校准值（话筒在某频点更灵敏 ⟹ 读数偏高 ⟹ 减掉）。
//
// 分层（与 room_correction.h / room_measurement.h 同一约定）：
//   本文件只做纯算法：解析文本 → 曲线 → 对数频率插值 → 对 IR 做**只改幅度、不动相位**的补偿。
//   不碰设备、不开线程、不 include src/audio/*（铁律 1 红线）。唯一的 IO 是 mic_cal_load()
//   读取一个 ≤4 MB 的文本文件（UTF-8 路径，Windows 中文目录可用）。
//
// 解析容忍的真实文件形态（每一条都有独立自测；测试是 build\ 下的临时文件，未入库）：
//   20.0 -1.23                空格/制表符两列
//   20.0,-1.23                逗号两列（CSV）
//   20.0, -1.23, 0.0          逗号三列：第三列相位**忽略**（校准只有幅度信息）
//   *1000.0 0.0               miniDSP 文件首行（灵敏度/注释行，整行跳过）
//   # ... / ; ... / // ...    注释行
//   "freq" "dB"               表头行（非数字 ⟹ 跳过，不算错误）
//   1.0e3 -1.23               科学计数法（strtod 直接吃）
//   \r\n 行尾、UTF-8 BOM       Windows 记事本存出来的文件
// 规则：按 hz 升序；hz<=0 或非有限值丢弃；同频**取文件中后者**；点数 < 3 判无效并写 error。
//
// 插值语义（db_at）：对数频率线性插值；范围外取端点值（**不外推成 0**，也不外推斜率——
// 校准文件两端本来就没有可信数据，端点保持是业界（REW）默认做法）。
#pragma once
#include "fp_strict.h"   // 261030-01 审计：DSP 路径严格 IEEE（输出进入音频样本）
#include "fft.h"          // rta::dsp::fft / cplx（补偿走频域）
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>         // fmt1 的 snprintf
#include <filesystem>     // 只在 mic_cal_load 里用：fs::u8path 让中文路径能打开
#include <fstream>
#include <string>
#include <system_error>   // std::error_code（file_size 的错误出口）
#include <vector>

namespace rta::dsp {

// 校准文件大小上限（与 /api/miccal 的校验一致）：UMIK 文件典型 10~100 KB，4 MB 已是两个数量级的余量
inline constexpr size_t kMicCalMaxBytes = 4u * 1024u * 1024u;

struct MicCalPoint { double hz = 0.0; double db = 0.0; };

struct MicCalCurve {
    std::vector<MicCalPoint> pts;   // 按 hz 升序（构造后保证：hz>0、有限、无重复）
    std::string name;               // 文件名（不含路径），供 UI / 日志显示
    std::string error;              // 空 = 成功
    double minDb = 0.0, maxDb = 0.0;   // 统计，供 UI 显示（如「−4.2 ~ +3.1 dB」）

    bool ok() const { return error.empty() && pts.size() >= 3; }
    double min_hz() const { return pts.empty() ? 0.0 : pts.front().hz; }
    double max_hz() const { return pts.empty() ? 0.0 : pts.back().hz; }

    // 在 hz 处插值（对数频率线性插值）；超出 [min_hz, max_hz] 取端点值，不外推成 0。
    // 空曲线返回 0 dB（= 不补偿，绝不因为缺数据把整条响应改掉）。
    double db_at(double hz) const {
        if (pts.empty()) return 0.0;
        if (!(hz > 0.0)) return pts.front().db;        // hz<=0 / NaN ⟹ 按最低端处理
        if (pts.size() == 1) return pts.front().db;
        if (hz <= pts.front().hz) return pts.front().db;
        if (hz >= pts.back().hz) return pts.back().db;
        // 二分：找最后一个 hz_i <= hz 的下标（pts 已升序）
        size_t lo = 0, hi = pts.size() - 1;
        while (hi - lo > 1) {
            const size_t mid = lo + (hi - lo) / 2;
            if (pts[mid].hz <= hz) lo = mid; else hi = mid;
        }
        const double a = pts[lo].hz, b = pts[hi].hz;
        if (!(b > a)) return pts[hi].db;
        const double t = (std::log(hz) - std::log(a)) / (std::log(b) - std::log(a));
        return pts[lo].db + (pts[hi].db - pts[lo].db) * t;
    }
};

// ---------------- 解析（纯函数，不碰文件） ----------------

namespace mic_cal_detail {

inline bool is_sep(char c) {
    return c == ' ' || c == '\t' || c == ',' || c == ';' || c == '\r';
}

// 一个 token 严格转 double：必须整 token 被吃掉（"1.0e3" 可以，"12dB" 不行），且必须有限。
inline bool to_double(const char* b, const char* e, double& out) {
    if (b == e) return false;
    std::string tok(b, e);
    char* end = nullptr;
    const double v = std::strtod(tok.c_str(), &end);
    if (!end || end != tok.c_str() + tok.size()) return false;
    if (!std::isfinite(v)) return false;
    out = v;
    return true;
}

// 取一行里的前两个数值（第 3 列及以后一律忽略：相位、注释、额外列）。
// 任何 token 不是数字 ⟹ 整行不是数据行（表头 "freq" "dB" 走这里被跳过）。
inline bool parse_pair(const std::string& s, double& hz, double& db) {
    double v[2] = { 0.0, 0.0 };
    int nv = 0;
    size_t i = 0;
    while (i < s.size() && nv < 2) {
        while (i < s.size() && is_sep(s[i])) ++i;
        const size_t b = i;
        while (i < s.size() && !is_sep(s[i])) ++i;
        if (i == b) break;
        if (!to_double(s.data() + b, s.data() + i, v[nv])) return false;
        ++nv;
    }
    if (nv < 2) return false;
    hz = v[0];
    db = v[1];
    return true;
}

// 文件名（不含路径）。用 fs::path 而不是手找最后一个 '\\'：'/' 分隔、UNC、尾斜杠都能对。
inline std::string file_name_of(const std::string& path) {
    if (path.empty()) return std::string();
    const auto p = std::filesystem::u8path(path);
    const auto u8 = p.filename().u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

inline std::string fmt1(double v) {
    if (!std::isfinite(v)) return "?";
    char b[64];
    std::snprintf(b, sizeof(b), "%.1f", v);
    return b;
}

} // namespace mic_cal_detail

// text → 曲线。name 只用于显示（调用方传文件名；解析本身不看它）。
inline MicCalCurve mic_cal_parse(const std::string& text, const std::string& name) {
    MicCalCurve c;
    c.name = name;
    if (text.empty()) { c.error = "文件为空（0 字节）"; return c; }
    std::vector<MicCalPoint> raw;

    size_t pos = 0;
    if (text.size() >= 3 && (unsigned char)text[0] == 0xEF &&
        (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF)
        pos = 3;   // UTF-8 BOM（Windows 记事本另存为 UTF-8 会带）
    size_t skipped = 0;   // 非数据行计数（表头/注释/坏值；只用于「一个数都没解析到」时的报错）

    while (pos <= text.size()) {
        size_t eol = text.find('\n', pos);
        if (eol == std::string::npos) eol = text.size();
        std::string line = text.substr(pos, eol - pos);
        pos = eol + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();

        size_t b = 0, e = line.size();
        while (b < e && (line[b] == ' ' || line[b] == '\t')) ++b;
        while (e > b && (line[e - 1] == ' ' || line[e - 1] == '\t')) --e;
        if (b >= e) continue;                                      // 空行（不计入 skipped）

        const char c0 = line[b];
        if (c0 == '#' || c0 == ';' || c0 == '*' || c0 == '/') {    // 注释（含 miniDSP 的 *灵敏度行）
            ++skipped;
            continue;
        }
        double hz = 0.0, db = 0.0;
        if (!mic_cal_detail::parse_pair(line.substr(b, e - b), hz, db)) { ++skipped; continue; }
        if (!std::isfinite(hz) || !std::isfinite(db) || hz <= 0.0) { ++skipped; continue; }
        raw.push_back(MicCalPoint{ hz, db });
    }

    // 升序 + 同频取后者：stable_sort 保留文件次序，等频段里**最后出现**的那条覆盖前面的。
    // （miniDSP 文件若把灵敏度行写成不带 '*' 的「1000.0 0.0」，也会被后面真正的 1000 Hz 点覆盖。）
    std::stable_sort(raw.begin(), raw.end(),
                     [](const MicCalPoint& a, const MicCalPoint& b) { return a.hz < b.hz; });
    c.pts.reserve(raw.size());
    for (const MicCalPoint& p : raw) {
        if (!c.pts.empty() && c.pts.back().hz == p.hz) c.pts.back() = p;
        else c.pts.push_back(p);
    }

    if (c.pts.empty()) {
        c.error = "没有解析到有效数据点（需要两列：频率 Hz、幅度 dB；已跳过 " +
                  std::to_string(skipped) + " 行非数据行）";
        return c;
    }
    if (c.pts.size() < 3) {
        c.error = "有效数据点太少（" + std::to_string(c.pts.size()) + " 个，至少需要 3 个）";
        return c;
    }
    c.minDb = c.maxDb = c.pts.front().db;
    for (const MicCalPoint& p : c.pts) {
        c.minDb = std::min(c.minDb, p.db);
        c.maxDb = std::max(c.maxDb, p.db);
    }
    return c;
}

// ---------------- 读文件（唯一的 IO；只读、有大小上限、UTF-8 路径） ----------------

inline MicCalCurve mic_cal_load(const std::string& path) {
    MicCalCurve c;
    c.name = mic_cal_detail::file_name_of(path);
    if (path.empty()) { c.error = "路径为空"; return c; }
    std::error_code ec;
    const auto p = std::filesystem::u8path(path);
    const auto sz = std::filesystem::file_size(p, ec);
    if (ec) { c.error = "读不到文件（不存在、是目录或无权访问）"; return c; }
    if (sz == 0) { c.error = "文件为空"; return c; }
    if (sz > kMicCalMaxBytes) { c.error = "文件超过 4 MB 上限（" + std::to_string((unsigned long long)sz) + " 字节）"; return c; }
    std::ifstream f(p, std::ios::binary);
    if (!f) { c.error = "打不开文件"; return c; }
    std::string text((size_t)sz, '\0');
    f.read(&text[0], (std::streamsize)sz);
    text.resize((size_t)(f.gcount() > 0 ? f.gcount() : 0));
    if (text.empty()) { c.error = "文件读取失败或为空"; return c; }
    return mic_cal_parse(text, c.name);
}

// ---------------- 补偿（频域，只改幅度、不动相位） ----------------

struct MicCalApply {
    bool applied = false;
    size_t binsBelow = 0, binsAbove = 0;   // 落在校准范围外的半谱频点数（低频/高频各多少）
    std::string note;                      // 人话（范围外按端点值处理时非空），供 status / 日志
};

// 对时域 IR 做「测量麦克风校准补偿」：H(f) ← H(f) · 10^(−cal_db(f)/20)。
//
// 为什么在频域做：现有链路（room_measurement.h）拿到的只有 IR，没有显式幅度谱；而补偿本身
// 是逐频点的实数增益，频域做最直接、也最容易证明「相位不变」——
//   · 增益是**正实数** ⟹ 每个 bin 的 arg 严格不变（乘法只缩放模长）；
//   · 镜像 bin（k 与 n−k）用同一个实数增益 ⟹ 共轭对称保持 ⟹ IFFT 结果仍是严格实值，
//     不会因为「取实部」把虚部丢掉而改变幅度。
// 麦克风校准只有幅度信息：任何相位改动（哪怕只是把 IR 改成最小相位）都会引入真实房间
// 里不存在的时域假象，所以这里绝不碰相位。
//
// 校准范围外（低于最低点/高于最高点）：按端点值处理（db_at 的语义），并记一条 note。
inline MicCalApply mic_cal_apply_ir(std::vector<double>& ir, double fs, const MicCalCurve& cal) {
    MicCalApply out;
    if (ir.size() < 4 || !(fs > 0.0)) return out;
    if (!cal.ok()) return out;

    size_t n = 1;
    while (n < ir.size()) n <<= 1;          // fft() 要求 2 的幂；deconvolve_ir 的输出本来就满足
    std::vector<cplx> X(n, cplx(0.0, 0.0));
    for (size_t i = 0; i < ir.size(); ++i) X[i] = cplx(ir[i], 0.0);
    fft(X);

    const double loHz = cal.pts.front().hz, hiHz = cal.pts.back().hz;
    for (size_t k = 0; k <= n / 2; ++k) {
        const double f = (double)k * fs / (double)n;
        // k=0 是 DC（f=0）：既不是「低频不够低」，也没有信号，不计入范围外统计
        if (k > 0) {
            if (f < loHz) ++out.binsBelow;
            else if (f > hiHz) ++out.binsAbove;
        }
        const double g = std::pow(10.0, -cal.db_at(f) / 20.0);
        X[k] *= g;
        if (k != 0 && k != n / 2) X[n - k] *= g;   // 镜像 bin 同一实数增益（保持实值 IFFT）
    }
    fft(X, true);
    for (size_t i = 0; i < ir.size(); ++i) ir[i] = X[i].real();
    out.applied = true;

    if (out.binsBelow || out.binsAbove) {
        // 人话：说清「曲线管到哪儿、管不到的地方怎么办」，UI / 日志直接显示
        out.note = "麦克风校准曲线覆盖 " + mic_cal_detail::fmt1(loHz) + "~" +
                   mic_cal_detail::fmt1(hiHz) + " Hz；范围外（低频 " +
                   std::to_string(out.binsBelow) + " 个、高频 " + std::to_string(out.binsAbove) +
                   " 个频点）按端点值处理，不外推";
    }
    return out;
}

} // namespace rta::dsp
