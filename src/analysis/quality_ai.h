// quality_ai.h — 音频体检 B4：把确定性指标翻译成人话（DeepSeek 解释层，纯附加）
//
// 定位（用户 261027-01 最终提醒 #2）：
//   AI 只做【解释】——把 version_compare.h 算好的数字说成人话（"这个版本高频少了 3 dB，
//   可能是低码率转制"）。指标一律来自确定性算法，AI 不许猜、不许编。
//
// 三条硬约束：
//  1) key 只从这些地方读，且【读到的 key 绝不写进任何文件、日志、报告】：
//       ① 仓库根目录 KEY.txt（单行 sk-...，用户指定的位置）
//       ② 环境变量 RLPLAYER_LLM_KEY
//       ③ rlplayer-llm.conf 的 api_key= 行
//     所有 error / 返回文本都过 rta::meta::llm_redact，服务端把 key 回显进错误也会被替换成 ****。
//  2) 无 key / 无网 / 服务端报错 ⟹ ok=false + 中文原因，调用方回落到确定性解释
//     （version_compare 的 explanation），指标结果照常可用。
//  3) 【纯附加】：本文件不改变任何指标，也不参与评分；AI 输出只进 UI 的"解释"字段。
//
// 可测试性：请求体构造、key 解析、降级判定都是纯函数/可注入 transport 的形式，
//   离线就能断言（见 tests/test_quality_ai.cpp）；真实联网验证单独跑（tests/measure_quality_lib.cpp）。
#pragma once
#include "version_compare.h"
#include "../meta/llm.h"
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <cstdio>
#include <functional>

namespace rta::analysis {

struct AiExplainOptions {
    std::string keyFile = "KEY.txt";                       // 仓库根目录的单行 key 文件
    std::string confPath = "rlplayer-llm.conf";            // 兼容既有配置
    std::string model;                                     // 空 = 用 conf 里的 model
    int maxTokens = 700;
    std::function<rta::net::HttpResponse(const std::string& url, const std::string& body,
                                        const std::vector<std::string>& headers)> transport;  // 测试注入
};

struct AiExplain {
    bool ok = false;
    std::string text;        // AI 输出（已脱敏）
    std::string error;       // 失败原因（已脱敏）
    std::string keySource;   // "KEY.txt" / "env" / "conf" / ""（只写来源，不写 key）
    std::string model;
    int totalTokens = 0;
    bool usedAi = false;
};

// ---------------- key 解析（纯函数，可离线测） ----------------
// 返回 key 与来源标签。绝不打印/返回 key 的明文片段（掩码在调用方用 llm_mask_key）。
inline std::string load_explain_key(const AiExplainOptions& opt, std::string* source) {
    // ① KEY.txt：单行、去空白、去 BOM
    if (!opt.keyFile.empty()) {
        std::ifstream f(opt.keyFile, std::ios::binary);
        if (f) {
            std::string line;
            std::getline(f, line);
            line = rta::meta::llm_trim(line);
            if (line.size() >= 3 && (unsigned char)line[0] == 0xEF) line = rta::meta::llm_trim(line.substr(3));
            if (!line.empty()) { if (source) *source = "KEY.txt"; return line; }
        }
    }
    // ② 环境变量
    if (const char* env = std::getenv("RLPLAYER_LLM_KEY")) {
        std::string v = rta::meta::llm_trim(env);
        if (!v.empty()) { if (source) *source = "env"; return v; }
    }
    // ③ rlplayer-llm.conf
    if (!opt.confPath.empty()) {
        rta::meta::LlmConfig c = rta::meta::llm_config_load(opt.confPath);
        if (!c.apiKey.empty()) { if (source) *source = "conf"; return c.apiKey; }
    }
    if (source) source->clear();
    return std::string();
}

// ---------------- 提示词（纯函数：同输入 ⟹ 逐字节相同的请求体） ----------------
// 只喂【已经算好的数字】+ 确定性解释。明确禁止编造。
inline std::vector<rta::meta::LlmMessage> build_explain_messages(const VersionCompare& r) {
    std::vector<rta::meta::LlmMessage> msgs;
    rta::meta::LlmMessage sys;
    sys.role = "system";
    sys.content =
        "你是音频工程助手。用户给了同一首曲子若干版本（已对齐、同一区段）的【确定性测量结果】。"
        "你的任务只是把这些数字翻译成中文人话，指出哪个版本更好、差在哪些指标上、可能的成因。"
        "硬规则：1) 只能引用下面给出的指标名与数值，不得编造任何指标、数值或未提供的结论；"
        "2) 标了「不可比」的指标不许拿来比较；"
        "3) 若某指标差异在噪声级，明确说「差异可忽略」；"
        "4) 输出纯中文段落（不超过 350 字），不要 Markdown 表格，不要复述全部数字。";
    msgs.push_back(sys);

    std::ostringstream o;
    o << "文件与公共区段：\n";
    char b[256];
    for (size_t i = 0; i < r.versions.size(); ++i) {
        const VersionData& v = r.versions[i];
        std::snprintf(b, sizeof(b), "  %s：%d ch / %d bit / %u Hz，公共区段 %.1f s，对齐偏移 %+.3f s\n",
                      v.label.c_str(), v.channels, v.bits, v.fs, r.windowSec, v.lagSec);
        o << b;
    }
    o << "\n指标对比（方向：DR/LRA/峰值因子/SNR 越高越好；本底噪声/非谐波残留/削波/单声道损失越低越好）：\n";
    for (const MetricRow& row : r.rows) {
        if (row.informational) continue;
        o << "  " << row.label << " [" << row.key << "]";
        if (!row.comparable) { o << " —— 不可比：" << row.why << "\n"; continue; }
        o << "：";
        for (size_t i = 0; i < row.values.size(); ++i) {
            std::snprintf(b, sizeof(b), " %s=%.3f%s", r.versions[i].label.c_str(), row.values[i], row.unit.c_str());
            o << b;
        }
        o << "\n";
    }
    o << "\n综合评分（权重：DR 0.25 / 削波 0.20 / SNR 0.20 / 峰值因子 0.10 / LRA 0.10 / 非谐波残留 0.10 / 单声道 0.05）：\n  ";
    for (size_t i = 0; i < r.ranking.size(); ++i) {
        if (i) o << " > ";
        std::snprintf(b, sizeof(b), "%s(%.1f)", r.versions[r.ranking[i]].label.c_str(), r.scores[r.ranking[i]]);
        o << b;
    }
    o << "\n\n确定性解释（供参考，措辞可改，结论不可改）：\n" << r.explanation;
    rta::meta::LlmMessage user;
    user.role = "user";
    user.content = o.str();
    msgs.push_back(user);
    return msgs;
}

// ---------------- 主入口 ----------------
// 前置条件：已调用 score_versions(r)（否则 ranking/scores 为空，这里直接判失败）。
inline AiExplain explain_versions(const VersionCompare& r, const AiExplainOptions& opt = {}) {
    AiExplain out;
    if (!r.ok) { out.error = "版本对比未成立，无法生成解释：" + r.error; return out; }
    if (r.ranking.empty() || r.scores.empty()) {
        out.error = "对比结果尚未评分（请先调用 score_versions）";
        return out;
    }
    std::string source;
    const std::string key = load_explain_key(opt, &source);
    out.keySource = source;
    rta::meta::LlmConfig cfg = rta::meta::llm_config_load(opt.confPath);
    if (!opt.model.empty()) cfg.model = opt.model;
    out.model = cfg.model;
    if (key.empty()) {
        out.error = "未配置 API key（KEY.txt / 环境变量 RLPLAYER_LLM_KEY / rlplayer-llm.conf 均无）："
                    "已跳过 AI 解释，确定性指标与排序照常可用";
        return out;
    }
    cfg.apiKey = key;
    if (opt.transport) cfg.transport = opt.transport;

    const auto msgs = build_explain_messages(r);
    rta::meta::LlmReply rep = rta::meta::llm_chat(cfg, msgs, opt.maxTokens, 0.3f, false);
    if (!rep.ok) {
        out.error = rta::meta::llm_redact(cfg.apiKey, "AI 解释失败（指标结果不受影响）：" + rep.error);
        return out;
    }
    // 输出也过一遍脱敏：模型把 key 复述出来也不会外泄
    out.text = rta::meta::llm_redact(cfg.apiKey, rep.content);
    out.totalTokens = rep.totalTokens;
    out.ok = true;
    out.usedAi = true;
    return out;
}

// 降级友好入口：AI 成功用 AI 的话，否则用确定性解释（保证 UI 永远有东西可显示）
inline std::string explain_or_fallback(const VersionCompare& r, const AiExplainOptions& opt = {},
                                      AiExplain* detail = nullptr) {
    AiExplain e = explain_versions(r, opt);
    if (detail) *detail = e;
    if (e.ok && !e.text.empty()) return e.text;
    return r.explanation;
}

} // namespace rta::analysis
