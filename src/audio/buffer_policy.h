// buffer_policy.h — 独占输出缓冲策略（C1）与起播看门狗（L2）
//
// 背景（反馈和开发\debug\20261024-01\N300-欠载-修改方案.md §5）：
//   tools/rtaweb.cpp 的 WasapiSink::open 曾无条件把独占缓冲封顶 20 ms（旧代码 438 行），
//   把设备缓冲压到 2 个周期；render_loop 的轮询/预攒节奏喂不上 ⟹ pad 反复触底 ⟹ 欠载爆音。
//   本文件把「用户设置 → 学习缓存 → L1 预检 → 降级」这条决策序列抽成不依赖 WASAPI 的小模块：
//   生产侧由 rtaweb 的 WasapiSink 注入真实打开动作，测试侧注入 Fake（绝不打开音频设备，
//   铁律 1）。这样「缓冲请求透传 / L1 放大回退 / 学习缓存命中」测的是与生产【同一份】决策代码
//   （铁律 3），而不是测试里另抄一遍。
//
// 设计约束（方案 §4 / §12 红线）：
//   · 只决定「按多少毫秒打开」，不改 DSP、不改 STARVING 判据、不改 ZeroAttrib 归因本体；
//   · RTA_BUF_RESPECT=0 回到旧的 20 ms 行为（A/B 对照与即时回滚开关）；
//   · 学习缓存 = <exe>\rlplayer-buffer-learned.json：命中直接用、设置变更即失效、
//     POST /api/buffer/reset 可重置（见 rtaweb.cpp）。
//
// ⚠️ 未验证（铁律 4）：L1/L2 的自适应回退需要 MOTU 这类「Initialize 接受但流不推进」的真机
//    才能验证；本仓库目前【只有 mock 覆盖】，未经真机复测。
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <functional>
#include <map>
#include <string>
#include <utility>

namespace rta::audio {

// 独占缓冲的硬边界（方案 §5.1/§5.2）：下限 20 ms（保护驱动周期），上限 500 ms。
inline constexpr double kBufferMinMs     = 20.0;
inline constexpr double kBufferMaxMs     = 500.0;
inline constexpr double kBufferDegradeMs = 20.0;

// L1 判据：驱动实际给的缓冲超过请求的 1.5 倍 ⟹ 认为驱动「对齐放大」了。
inline constexpr double kBufferOvershootRatio = 1.5;

// L2 起播看门狗：Start 后多久「有数据却零写入」就判设备流不推进（方案 §5.2 的 0.8s）。
inline constexpr double kBufferL2DeadlineMs = 800.0;

struct BufferPolicy {
    double requestedMs = 100.0;   // 用户设置（已 clamp 到 [20,500]）
    double effectiveMs = 20.0;    // 实际打开用的毫秒数
    bool   degraded   = false;    // 走了回退（L1 放大 / 请求值打不开）
    bool   learned    = false;    // 命中学习缓存
    bool   respect    = true;     // false = RTA_BUF_RESPECT=0（旧行为）
    std::string why;              // 降级原因（人话，进 PLAYBUF/日志）
    // requested | learned | degraded | forced20
    std::string policy = "requested";
};

inline double buffer_clamp_ms(double ms) {
    if (!(ms > 0.0)) return kBufferDegradeMs;       // NaN / 0 / 负数
    if (ms < kBufferMinMs) return kBufferMinMs;
    if (ms > kBufferMaxMs) return kBufferMaxMs;
    return ms;
}

// RTA_BUF_RESPECT：未设置或非 "0" ⟹ 尊重用户设置（新行为）；"0" ⟹ 旧的 20 ms 行为。
inline bool buffer_respect_env() {
    const char* v = std::getenv("RTA_BUF_RESPECT");
    if (!v || !*v) return true;
    return !(v[0] == '0' && v[1] == '\0');
}

// ---------------- 学习缓存 ----------------
// 文件格式（一行一个端点，便于出问题直接肉眼读；键里的 GUID 含 { }，所以不用嵌套对象）：
//   {"version":1,"endpoints":{
//   "<deviceId>|<rate>|<bits>|<reqMs>":{"ms":20,"why":"…","at":"2026-10-24"}
//   }}
//
// 键里带 reqMs 是刻意的：**用户改设置 ⟹ 键变化 ⟹ 自然失效**，不需要额外的失效逻辑。
// （方案 §5.3 只写 deviceId|rate|bits；照那样做，「改成 200ms 后仍套用 20ms 的旧学习值」
//  会与 §5.3 自己的要求「设置变更即失效该端点缓存」矛盾。已在交付报告里列为不一致项。）
struct BufferLearnEntry {
    double      ms  = kBufferDegradeMs;
    std::string why;
    std::string at;
};

inline std::string buffer_today() {
    const std::time_t t = std::time(nullptr);
    std::tm tmv{};
#ifdef _WIN32
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    char b[32] = {0};
    std::strftime(b, sizeof(b), "%Y-%m-%d", &tmv);
    return std::string(b);
}

inline std::string bp_escape(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 8);
    for (const char c : s) {
        switch (c) {
            case '\\': o += "\\\\"; break;
            case '"':  o += "\\\""; break;
            case '\n': o += "\\n";  break;
            case '\r': o += "\\r";  break;
            case '\t': o += "\\t";  break;
            default:   o += c;      break;
        }
    }
    return o;
}

// 从 pos（指向开引号之后）解析一个 JSON 字符串到 out；成功时 pos 指向闭引号之后。
inline bool bp_parse_string(const std::string& line, size_t& pos, std::string& out) {
    out.clear();
    while (pos < line.size()) {
        const char c = line[pos++];
        if (c == '\\' && pos < line.size()) {
            const char e = line[pos++];
            if (e == 'n') out += '\n';
            else if (e == 'r') out += '\r';
            else if (e == 't') out += '\t';
            else out += e;
        } else if (c == '"') {
            return true;
        } else {
            out += c;
        }
    }
    return false;
}

class BufferLearnStore {
public:
    BufferLearnStore() = default;
    explicit BufferLearnStore(std::string path) : path_(std::move(path)) {}

    void set_path(std::string p) { path_ = std::move(p); }
    const std::string& path() const { return path_; }
    bool valid() const { return !path_.empty(); }

    bool get(const std::string& key, BufferLearnEntry& out) const {
        std::map<std::string, BufferLearnEntry> m;
        if (!load(m)) return false;
        const auto it = m.find(key);
        if (it == m.end()) return false;
        out = it->second;
        return true;
    }

    bool put(const std::string& key, const BufferLearnEntry& e) {
        if (path_.empty() || key.empty()) return false;
        std::map<std::string, BufferLearnEntry> m;
        load(m);
        m[key] = e;
        return save(m);
    }

    bool erase(const std::string& key) {
        std::map<std::string, BufferLearnEntry> m;
        if (!load(m)) return false;
        if (m.erase(key) == 0) return false;
        return save(m);
    }

    // 重置：删掉整个文件（/api/buffer/reset）。文件本来就不存在也算成功。
    bool reset() {
        if (path_.empty()) return false;
        std::remove(path_.c_str());
        std::ifstream f(path_);
        return !f.good();
    }

    size_t size() const {
        std::map<std::string, BufferLearnEntry> m;
        load(m);
        return m.size();
    }

private:
    bool load(std::map<std::string, BufferLearnEntry>& out) const {
        out.clear();
        if (path_.empty()) return false;
        std::ifstream f(path_);
        if (!f) return false;
        std::string line;
        while (std::getline(f, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty() || line[0] != '"') continue;
            size_t p = 1;                      // 跳过开引号
            std::string key;
            if (!bp_parse_string(line, p, key)) continue;
            while (p < line.size() && (line[p] == ' ' || line[p] == '\t')) ++p;
            if (p >= line.size() || line[p] != ':') continue;
            ++p;
            while (p < line.size() && (line[p] == ' ' || line[p] == '\t')) ++p;
            if (p >= line.size() || line[p] != '{') continue;
            ++p;
            const size_t mpos = line.find("\"ms\":", p);
            if (mpos == std::string::npos) continue;
            BufferLearnEntry e;
            e.ms = std::strtod(line.c_str() + mpos + 5, nullptr);
            e.at = buffer_today();
            const size_t wpos = line.find("\"why\":", mpos);
            if (wpos != std::string::npos) {
                size_t q = line.find('"', wpos + 6);
                if (q != std::string::npos) { ++q; bp_parse_string(line, q, e.why); }
            }
            const size_t apos = line.find("\"at\":", mpos);
            if (apos != std::string::npos) {
                size_t q = line.find('"', apos + 5);
                if (q != std::string::npos) { ++q; std::string tmp; if (bp_parse_string(line, q, tmp)) e.at = tmp; }
            }
            out[key] = e;
        }
        return true;
    }

    bool save(const std::map<std::string, BufferLearnEntry>& m) const {
        if (path_.empty()) return false;
        std::ofstream f(path_, std::ios::trunc);
        if (!f) return false;
        f << "{\"version\":1,\"endpoints\":{\n";
        bool first = true;
        for (const auto& kv : m) {
            if (!first) f << ",\n";
            first = false;
            f << '"' << bp_escape(kv.first) << "\":{\"ms\":" << kv.second.ms
              << ",\"why\":\"" << bp_escape(kv.second.why) << "\""
              << ",\"at\":\"" << bp_escape(kv.second.at) << "\"}";
        }
        f << "\n}}\n";
        return static_cast<bool>(f);
    }

    std::string path_;
};

// 学习缓存的键：端点 id（空 = 默认端点）| 采样率 | 位深 | 请求毫秒
inline std::string buffer_key(const std::string& deviceId, uint32_t rate, int bits, double reqMs) {
    char b[64];
    std::snprintf(b, sizeof(b), "|%u|%d|%.0f", (unsigned)rate, bits, buffer_clamp_ms(reqMs));
    return (deviceId.empty() ? std::string("(default)") : deviceId) + b;
}

// ---------------- L1：确定性预检（不需要真正开始播放） ----------------
struct L1Check {
    bool        degrade  = false;
    double      actualMs = 0.0;
    std::string why;
};

inline L1Check buffer_l1_check(double requestedMs, size_t frames, uint32_t rate) {
    L1Check r;
    if (rate == 0 || frames == 0) return r;              // 未知：不判
    r.actualMs = 1000.0 * static_cast<double>(frames) / static_cast<double>(rate);
    if (r.actualMs > requestedMs * kBufferOvershootRatio) {
        r.degrade = true;
        char b[256];
        std::snprintf(b, sizeof(b),
                      "驱动把缓冲放大到 %.1f ms（请求 %.0f ms，超过 %.1f 倍），回退 %.0f ms",
                      r.actualMs, requestedMs, kBufferOvershootRatio, kBufferDegradeMs);
        r.why = b;
    }
    return r;
}

// ---------------- L2：起播看门狗（纯判据） ----------------
// 语义（方案 §5.2）：Start 之后 deadlineMs 内，【手里已有数据】却一个字节都没写进去 ⟹
// 设备流不推进（MOTU 形态），调用方重开 20 ms。
// 抽成结构只为让时序判据能用虚拟时钟确定性测试，不含 I/O。
struct StartWatchdog {
    double deadlineMs = kBufferL2DeadlineMs;
    double startedMs  = -1.0;

    void arm(double nowMs) { startedMs = nowMs; }
    bool armed() const { return startedMs >= 0.0; }
    bool should_degrade(double nowMs, size_t writtenFrames, bool hasData) const {
        if (!armed() || !hasData || writtenFrames > 0) return false;
        return (nowMs - startedMs) >= deadlineMs;
    }
};

// ---------------- 决策序列 ----------------
// 打开动作抽象：生产 = WasapiSink 包 WasapiOutput；测试 = Fake（记录每次尝试的 ms）。
struct BufferOpener {
    std::function<bool(double ms, std::string& why)> open;   // true = 成功
    std::function<size_t()> frames;                          // 实际缓冲帧数（0 = 未知）
    std::function<void()> close;
};

struct BufferRequest {
    double   requestedMs = 100.0;
    uint32_t rate        = 48000;
    bool     respect     = true;    // RTA_BUF_RESPECT != 0
    bool     useLearn    = true;
};

// 按 C1 的三层决策打开；返回最终策略。*ok = 最终是否已成功打开。
// 顺序：RTA_BUF_RESPECT 闸门 → 学习缓存命中 → 按请求打开（失败即降级 20ms）
//       → L1 放大预检（>1.5 倍则降级 20ms 并写入学习缓存）。
inline BufferPolicy buffer_apply(const BufferRequest& in, const BufferOpener& op,
                                 BufferLearnStore* store, const std::string& key,
                                 bool* okOut) {
    bool okLocal = false;
    bool* ok = okOut ? okOut : &okLocal;

    BufferPolicy pol;
    pol.requestedMs = buffer_clamp_ms(in.requestedMs);
    pol.effectiveMs = pol.requestedMs;
    pol.respect     = in.respect;

    auto try_open = [&](double ms, std::string* why) -> bool {
        if (!op.open) return false;
        std::string e;
        const bool r = op.open(ms, e);
        if (!r && why) *why = e.empty() ? std::string("(未说明)") : e;
        return r;
    };
    auto remember = [&](const BufferPolicy& p, const std::string& why) {
        if (!store || !in.useLearn || key.empty()) return;
        BufferLearnEntry e;
        e.ms = p.effectiveMs;
        e.why = why;
        e.at = buffer_today();
        store->put(key, e);
    };

    // ① 回滚闸门：RTA_BUF_RESPECT=0 ⟹ 与旧代码一致的 20 ms，跳过全部学习/预检
    if (!in.respect) {
        pol.policy      = "forced20";
        pol.effectiveMs = kBufferDegradeMs;
        pol.why         = "RTA_BUF_RESPECT=0：强制旧的 20ms 行为";
        *ok = try_open(kBufferDegradeMs, nullptr);
        return pol;
    }

    // ② 学习缓存命中：直接用学习值，跳过 L1（不再先失败一次）
    if (in.useLearn && store && !key.empty()) {
        BufferLearnEntry e;
        if (store->get(key, e)) {
            pol.learned     = true;
            pol.policy      = "learned";
            pol.effectiveMs = buffer_clamp_ms(e.ms);
            pol.why         = e.why;
            if (try_open(pol.effectiveMs, nullptr)) { *ok = true; return pol; }
            // 学习值本身打不开：删掉它，按用户设置重试（不让一次坏缓存永久废掉该端点）
            store->erase(key);
            pol.learned     = false;
            pol.policy      = "requested";
            pol.effectiveMs = pol.requestedMs;
            pol.why.clear();
        }
    }

    // ③ 按用户设置打开；打不开就降级 20ms（例如驱动拒绝过大缓冲）
    std::string err;
    *ok = try_open(pol.requestedMs, &err);
    if (!*ok && pol.requestedMs > kBufferDegradeMs + 0.5) {
        pol.degraded    = true;
        pol.policy      = "degraded";
        pol.effectiveMs = kBufferDegradeMs;
        pol.why         = "以请求缓冲 " + std::to_string((int)(pol.requestedMs + 0.5)) + "ms 打开失败（" +
                          err + "），回退 20ms";
        *ok = try_open(kBufferDegradeMs, &err);
        if (*ok) remember(pol, pol.why);
        return pol;
    }
    if (!*ok) return pol;

    // ④ L1 预检：驱动把缓冲对齐放大（MOTU 100ms → 8192 帧 ≈ 186ms）
    if (pol.requestedMs > kBufferDegradeMs + 0.5 && op.frames) {
        const L1Check l1 = buffer_l1_check(pol.requestedMs, op.frames(), in.rate);
        if (l1.degrade) {
            if (op.close) op.close();
            std::string err2;
            if (try_open(kBufferDegradeMs, &err2)) {
                pol.degraded    = true;
                pol.policy      = "degraded";
                pol.effectiveMs = kBufferDegradeMs;
                pol.why         = l1.why;
                remember(pol, l1.why);
            } else {
                // 回退也打不开：退回请求值（可能再次失败，交给上层如实报错）
                *ok = try_open(pol.requestedMs, &err2);
                pol.why = "驱动放大缓冲（" + l1.why + "），且回退 20ms 也失败：" + err2;
            }
        }
    }
    return pol;
}

// ---------------- PLAYBUF 日志行（方案 §5.4）----------------
// 把「请求/实际/实际帧数/周期/策略/原因/配方/对齐」格式化成一行 JSON。
// 抽成函数的理由：日志字段名也是对外契约 —— 现场排查要靠它和 STAT 对照，字段一改名
// 旧日志就废了。测试里直接调用它把样例钉住（tests/test_buffer_policy.cpp）。
inline std::string buffer_playbuf_json(const BufferPolicy& p, size_t bf, long long per,
                                       const std::string& recipe, bool realigned) {
    char head[256];
    std::snprintf(head, sizeof(head),
                  "{\"bufferReqMs\":%.0f,\"bufferMs\":%.1f,\"bf\":%zu,\"per\":%lld,"
                  "\"bufPolicy\":\"%s\",\"bufWhy\":\"",
                  p.requestedMs, p.effectiveMs, bf, per, p.policy.c_str());
    std::string s = head;
    s += bp_escape(p.why);
    s += "\",\"initRecipe\":\"";
    s += bp_escape(recipe);
    s += "\",\"realigned\":";
    s += realigned ? "true" : "false";
    s += "}";
    return s;
}

} // namespace rta::audio
