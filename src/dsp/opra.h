// opra.h — OPRA (Open Profiles for Revealing Audio) 耳机 EQ 数据层与预设管理
//
// 对应 docs/耳机EQ-OPRA调研与设计.md 已拍板决策：
//   1. 轻量索引 + 按需缓存：只带 data/opra_index.json (<= 400KB)，点选预设按需拉取并落盘 profiles/opra/
//   2. Preamp 增益施加于 ChannelProcess::gainDb，主音量滑块保持解耦
//   3. CC BY-SA 4.0 归属声明 + 复制链接芯片（不打开浏览器）
//   4. 平台边界纪律：禁止 include windows.h / winsock2.h
#pragma once

#include "dsp/matrix.h"
#include "dsp/eq_text.h"
#include "net/json.h"
#include "net/http_client.h"

#include <string>
#include <vector>
#include <map>
#include <memory>
#include <mutex>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cmath>
#include <cctype>

namespace rta::dsp {

// ---------------------------------------------------------------------------
// 基础数据模型
// ---------------------------------------------------------------------------

// 单款耳机产品索引条目（轻量）
struct OpraProductItem {
    std::string id;      // 例: "sennheiser::hd_600"
    std::string vendor;  // 例: "Sennheiser"
    std::string name;    // 例: "HD 600"
};

// 单个滤波器频段定义
struct OpraBand {
    std::string type;       // "peak_dip", "low_shelf", "high_shelf", "low_pass", "high_pass", "band_pass", "band_stop"
    double frequency = 1000.0;
    double gainDb    = 0.0;
    double q         = 0.7071;
};

// OPRA 滤波器类型向 AutoEq 文本简写映射
inline std::string opra_type_to_eq_type(const std::string& opraType) {
    std::string t;
    for (char c : opraType) t += (char)std::tolower((unsigned char)c);
    if (t == "peak_dip" || t == "peak" || t == "pk") return "PK";
    if (t == "low_shelf" || t == "lowshelf" || t == "ls") return "LS";
    if (t == "high_shelf" || t == "highshelf" || t == "hs") return "HS";
    if (t == "low_pass" || t == "lowpass" || t == "lp") return "LP";
    if (t == "high_pass" || t == "highpass" || t == "hp") return "HP";
    if (t == "band_pass" || t == "bandpass" || t == "bp") return "BP";
    if (t == "band_stop" || t == "notch" || t == "no") return "NO";
    if (t == "all_pass" || t == "allpass" || t == "ap") return "AP";
    return "PK";
}

// 单个预设定义
struct OpraPreset {
    std::string id;         // 例: "sennheiser:hd_600::oratory1990_harman_target"
    std::string productId;  // 例: "sennheiser::hd_600"
    std::string author;     // 例: "oratory1990"
    std::string details;    // 例: "Harman Target"
    std::string link;       // 来源 URL（若有）
    double      gainDb = 0.0; // 预放大增益（通常为负值以防止削波）
    std::vector<OpraBand> bands;

    // 转换为本项目通用 EqSpec
    EqSpec to_eq_spec() const {
        EqSpec spec;
        spec.preampDb = gainDb;
        spec.bands.reserve(bands.size());
        for (const auto& b : bands) {
            EqBand eb;
            eb.enabled = true;
            eb.type = opra_type_to_eq_type(b.type);
            eb.fc = b.frequency;
            eb.gain = b.gainDb;
            eb.q = b.q;
            eb.hasQ = true;
            spec.bands.push_back(eb);
        }
        return spec;
    }

    // 导出为标准 AutoEq 格式文本
    std::string to_autoeq_text() const {
        std::ostringstream ss;
        ss << std::fixed << std::setprecision(1);
        ss << "Preamp: " << gainDb << " dB\n";
        for (size_t i = 0; i < bands.size(); ++i) {
            const auto& b = bands[i];
            const std::string t = opra_type_to_eq_type(b.type);
            ss << "Filter " << (i + 1) << ": ON " << t << " Fc ";
            if (std::abs(b.frequency - std::round(b.frequency)) < 1e-4) {
                ss << (long long)std::round(b.frequency);
            } else {
                ss << b.frequency;
            }
            ss << " Hz Gain " << b.gainDb << " dB Q " << std::setprecision(2) << b.q << "\n";
            ss << std::setprecision(1);
        }
        return ss.str();
    }

    // 从单对象 JSON 解析
    static bool parse_json(const std::string& jsonText, OpraPreset& out, std::string* err = nullptr) {
        rta::net::Json j = rta::net::json_parse(jsonText);
        if (!j.ok() || j.type != rta::net::Json::Obj) {
            if (err) *err = "JSON 格式非法或非对象";
            return false;
        }
        return from_json_obj(j, out, err);
    }

    // 从 JSON 数组解析多个预设（GET /opra/eqs?productId=... 返回）
    static bool parse_presets_json(const std::string& jsonText, std::vector<OpraPreset>& out, std::string* err = nullptr) {
        out.clear();
        rta::net::Json j = rta::net::json_parse(jsonText);
        if (!j.ok()) {
            if (err) *err = "JSON 格式非法";
            return false;
        }
        if (j.type == rta::net::Json::Arr) {
            for (size_t i = 0; i < j.size(); ++i) {
                OpraPreset p;
                if (from_json_obj(j.at(i), p, nullptr)) {
                    out.push_back(std::move(p));
                }
            }
            return !out.empty();
        } else if (j.type == rta::net::Json::Obj) {
            // 单个对象也兼容
            OpraPreset p;
            if (from_json_obj(j, p, err)) {
                out.push_back(std::move(p));
                return true;
            }
            return false;
        }
        if (err) *err = "未预期的 JSON 类型";
        return false;
    }

private:
    static bool from_json_obj(const rta::net::Json& j, OpraPreset& p, std::string* err) {
        if (!j.has("id")) {
            if (err) *err = "缺少 id 字段";
            return false;
        }
        p.id = j["id"].s();
        p.productId = j.has("productId") ? j["productId"].s() : (j.has("product_id") ? j["product_id"].s() : "");
        p.author = j.has("author") ? j["author"].s() : "";
        p.details = j.has("details") ? j["details"].s() : "";
        p.link = j.has("link") ? j["link"].s() : "";

        p.gainDb = 0.0;
        p.bands.clear();

        const rta::net::Json& params = j.has("parameters") ? j["parameters"] : j;
        if (params.type == rta::net::Json::Obj) {
            if (params.has("gainDb")) p.gainDb = params["gainDb"].n();
            else if (params.has("gain_db")) p.gainDb = params["gain_db"].n();

            if (params.has("bands") && params["bands"].type == rta::net::Json::Arr) {
                const auto& arr = params["bands"];
                for (size_t i = 0; i < arr.size(); ++i) {
                    const auto& b = arr.at(i);
                    OpraBand band;
                    band.type = b.has("type") ? b["type"].s() : "peak_dip";
                    band.frequency = b.has("frequency") ? b["frequency"].n() : (b.has("freq") ? b["freq"].n() : 1000.0);
                    band.gainDb = b.has("gainDb") ? b["gainDb"].n() : (b.has("gain_db") ? b["gain_db"].n() : 0.0);
                    band.q = b.has("q") ? b["q"].n() : (b.has("Q") ? b["Q"].n() : 0.7071);
                    p.bands.push_back(band);
                }
            }
        }
        return true;
    }
};

// ---------------------------------------------------------------------------
// 索引管理（轻量）
// ---------------------------------------------------------------------------

class OpraIndex {
public:
    OpraIndex() = default;

    bool load_from_json(const std::string& jsonText) {
        rta::net::Json j = rta::net::json_parse(jsonText);
        if (!j.ok()) return false;
        items_.clear();

        const rta::net::Json* arr = nullptr;
        if (j.type == rta::net::Json::Arr) {
            arr = &j;
        } else if (j.type == rta::net::Json::Obj) {
            if (j.has("items") && j["items"].type == rta::net::Json::Arr) arr = &j["items"];
            else if (j.has("products") && j["products"].type == rta::net::Json::Arr) arr = &j["products"];
        }
        if (!arr) return false;

        items_.reserve(arr->size());
        for (size_t i = 0; i < arr->size(); ++i) {
            const auto& elem = arr->at(i);
            OpraProductItem item;
            if (elem.type == rta::net::Json::Arr && elem.size() >= 3) {
                item.id = elem.at(0).s();
                item.vendor = elem.at(1).s();
                item.name = elem.at(2).s();
            } else if (elem.type == rta::net::Json::Obj) {
                item.id = elem.has("id") ? elem["id"].s() : "";
                item.vendor = elem.has("vendor") ? elem["vendor"].s() : (elem.has("v") ? elem["v"].s() : "");
                item.name = elem.has("name") ? elem["name"].s() : (elem.has("n") ? elem["n"].s() : "");
            } else {
                continue;
            }
            if (!item.id.empty()) items_.push_back(std::move(item));
        }
        return !items_.empty();
    }

    bool load_from_file(const std::string& filePath) {
        std::ifstream f(filePath, std::ios::binary);
        if (!f) return false;
        std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        return load_from_json(text);
    }

    size_t size() const { return items_.size(); }
    const std::vector<OpraProductItem>& items() const { return items_; }

    const OpraProductItem* find_by_id(const std::string& id) const {
        for (const auto& it : items_) {
            if (it.id == id) return &it;
        }
        return nullptr;
    }

    // 本地快速过滤（不区分大小写多关键字子串匹配）
    std::vector<OpraProductItem> filter(const std::string& query, size_t limit = 100) const {
        std::vector<OpraProductItem> res;
        std::string q = to_lower(query);
        // 去除前后空白
        while (!q.empty() && std::isspace((unsigned char)q.front())) q.erase(q.begin());
        while (!q.empty() && std::isspace((unsigned char)q.back())) q.pop_back();

        if (q.empty()) {
            for (size_t i = 0; i < items_.size() && res.size() < limit; ++i) {
                res.push_back(items_[i]);
            }
            return res;
        }

        // 分割搜索 token
        std::vector<std::string> tokens;
        std::istringstream iss(q);
        std::string tok;
        while (iss >> tok) tokens.push_back(tok);

        for (const auto& item : items_) {
            const std::string target = to_lower(item.vendor + " " + item.name + " " + item.id);
            bool match = true;
            for (const auto& t : tokens) {
                if (target.find(t) == std::string::npos) {
                    match = false;
                    break;
                }
            }
            if (match) {
                res.push_back(item);
                if (res.size() >= limit) break;
            }
        }
        return res;
    }

private:
    static std::string to_lower(std::string s) {
        for (char& c : s) c = (char)std::tolower((unsigned char)c);
        return s;
    }
    std::vector<OpraProductItem> items_;
};

// ---------------------------------------------------------------------------
// 令牌桶限频器（纯函数/纯对象，100次/60秒，防上游 429）
// ---------------------------------------------------------------------------

class TokenBucket {
public:
    explicit TokenBucket(double capacity = 100.0, double fillRatePerSec = 100.0 / 60.0)
        : capacity_(capacity), fillRate_(fillRatePerSec), tokens_(capacity),
          lastTime_(std::chrono::steady_clock::now()) {}

    // 纯函数判定：以外部指定的时间点模拟
    bool try_consume(double count, std::chrono::steady_clock::time_point now) {
        std::lock_guard<std::mutex> lk(mu_);
        const double elapsed = std::chrono::duration<double>(now - lastTime_).count();
        if (elapsed > 0.0) {
            tokens_ = std::min(capacity_, tokens_ + elapsed * fillRate_);
            lastTime_ = now;
        }
        if (tokens_ >= count) {
            tokens_ -= count;
            return true;
        }
        return false;
    }

    bool try_consume(double count = 1.0) {
        return try_consume(count, std::chrono::steady_clock::now());
    }

    double tokens() const {
        std::lock_guard<std::mutex> lk(mu_);
        return tokens_;
    }

    void reset(double initialTokens = -1.0) {
        std::lock_guard<std::mutex> lk(mu_);
        tokens_ = (initialTokens >= 0.0) ? initialTokens : capacity_;
        lastTime_ = std::chrono::steady_clock::now();
    }

private:
    mutable std::mutex mu_;
    double capacity_ = 100.0;
    double fillRate_ = 100.0 / 60.0;
    double tokens_   = 100.0;
    std::chrono::steady_clock::time_point lastTime_;
};

// ---------------------------------------------------------------------------
// URL 与文件路径纯函数
// ---------------------------------------------------------------------------

inline std::string opra_url_products(const std::string& vendorId, const std::string& base = "https://opra.roon.app/opra") {
    return base + "/products?vendorId=" + vendorId;
}

inline std::string opra_url_eqs(const std::string& productId, const std::string& base = "https://opra.roon.app/opra") {
    return base + "/eqs?productId=" + productId;
}

inline std::string opra_url_eq(const std::string& eqId, const std::string& base = "https://opra.roon.app/opra") {
    return base + "/eqs/" + eqId;
}

// 跨平台安全文件名净化（替换 : / \ * ? " < > | 为 _）
inline std::string sanitize_filename_part(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (std::isalnum((unsigned char)c) || c == '-' || c == '_') {
            out += c;
        } else {
            out += '_';
        }
    }
    return out;
}

// 预设对应的标准相对文件路径
inline std::string opra_profile_relpath(const std::string& productId, const std::string& presetId) {
    return "profiles/opra/" + sanitize_filename_part(productId) + "_" + sanitize_filename_part(presetId) + ".txt";
}

// 保存预设文件至指定路径（自动递归建目录）
inline bool save_autoeq_profile(const std::string& fullPath, const std::string& text, std::string* err = nullptr) {
    namespace fs = std::filesystem;
    try {
        fs::path p(fullPath);
        if (p.has_parent_path()) {
            fs::create_directories(p.parent_path());
        }
        std::ofstream f(fullPath, std::ios::trunc | std::ios::binary);
        if (!f) {
            if (err) *err = "无法创建文件: " + fullPath;
            return false;
        }
        f.write(text.data(), (std::streamsize)text.size());
        return true;
    } catch (const std::exception& e) {
        if (err) *err = e.what();
        return false;
    }
}

// 读取已缓存的预设文本
inline bool load_cached_profile(const std::string& fullPath, std::string& outText) {
    std::ifstream f(fullPath, std::ios::binary);
    if (!f) return false;
    outText.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return !outText.empty();
}

// ---------------------------------------------------------------------------
// OPRA 运行时数据服务（按需拉取、缓存、限频、落盘）
// ---------------------------------------------------------------------------

class OpraService {
public:
    static OpraService& instance() {
        static OpraService s;
        return s;
    }

    bool init(const std::string& indexFilePath = "") {
        std::lock_guard<std::mutex> lk(mu_);
        namespace fs = std::filesystem;
        std::string path = indexFilePath;
        if (path.empty()) {
            if (fs::exists("data/opra_index.json")) path = "data/opra_index.json";
            else if (fs::exists("../data/opra_index.json")) path = "../data/opra_index.json";
        }
        if (!path.empty() && fs::exists(path)) {
            indexPath_ = path;
            return index_.load_from_file(path);
        }
        return false;
    }

    const OpraIndex& index() const {
        std::lock_guard<std::mutex> lk(mu_);
        return index_;
    }

    TokenBucket& token_bucket() { return tokenBucket_; }

    // 获取某耳机的全部预设：先查内存/本地缓存，若无则按需向 API 请求
    bool get_presets(const std::string& productId,
                     std::vector<OpraPreset>& outPresets,
                     bool& fromCache,
                     std::string& err,
                     bool forceRefresh = false,
                     const std::string& customBaseUrl = "https://opra.roon.app/opra") {
        fromCache = false;
        err.clear();
        outPresets.clear();

        namespace fs = std::filesystem;
        const std::string metaCachePath = "profiles/opra/metadata/" + sanitize_filename_part(productId) + ".json";

        // 1. 内存缓存检查
        {
            std::lock_guard<std::mutex> lk(mu_);
            auto it = memCache_.find(productId);
            if (it != memCache_.end() && !forceRefresh) {
                outPresets = it->second;
                fromCache = true;
                return true;
            }
        }

        // 2. 本地元数据缓存文件检查
        if (!forceRefresh && fs::exists(metaCachePath)) {
            std::string jsonStr;
            if (load_cached_profile(metaCachePath, jsonStr)) {
                if (OpraPreset::parse_presets_json(jsonStr, outPresets, nullptr)) {
                    std::lock_guard<std::mutex> lk(mu_);
                    memCache_[productId] = outPresets;
                    fromCache = true;
                    return true;
                }
            }
        }

        // 3. 令牌桶限频检查
        if (!tokenBucket_.try_consume(1.0)) {
            err = "OPRA 请求频次达到上限（100次/60秒），已触发本地限频保护";
            // 若有本地元数据缓存，回退使用
            if (fs::exists(metaCachePath)) {
                std::string jsonStr;
                if (load_cached_profile(metaCachePath, jsonStr) &&
                    OpraPreset::parse_presets_json(jsonStr, outPresets, nullptr)) {
                    fromCache = true;
                    return true;
                }
            }
            return false;
        }

        // 4. 发起网络请求
        const std::string url = opra_url_eqs(productId, customBaseUrl);
        auto resp = rta::net::http_get(url, { "User-Agent: RLPlayer/1.0.9 (OPRA-EQ)" }, 15000);
        if (!resp.ok || resp.status != 200) {
            std::string netErr = resp.error.empty() ? ("HTTP " + std::to_string(resp.status)) : resp.error;
            // 网络失败尝试离线回退
            if (fs::exists(metaCachePath)) {
                std::string jsonStr;
                if (load_cached_profile(metaCachePath, jsonStr) &&
                    OpraPreset::parse_presets_json(jsonStr, outPresets, nullptr)) {
                    fromCache = true;
                    err = "网络离线: 使用已缓存预设 (" + netErr + ")";
                    return true;
                }
            }
            err = "获取预设失败: " + netErr;
            return false;
        }

        std::string jsonText(resp.body.begin(), resp.body.end());
        if (!OpraPreset::parse_presets_json(jsonText, outPresets, &err)) {
            err = "解析预设失败: " + err;
            return false;
        }

        // 5. 写入内存与磁盘缓存
        {
            std::lock_guard<std::mutex> lk(mu_);
            memCache_[productId] = outPresets;
        }
        save_autoeq_profile(metaCachePath, jsonText, nullptr);
        fromCache = false;
        return true;
    }

    // 应用选定预设：确保 AutoEq 格式落盘至 profiles/opra/<id>.txt 并返回路径
    bool apply_preset(const std::string& productId,
                      const std::string& presetId,
                      const std::string& baseDir,
                      std::string& outRelPath,
                      std::string& err,
                      const std::string& customBaseUrl = "https://opra.roon.app/opra") {
        err.clear();
        outRelPath = opra_profile_relpath(productId, presetId);
        std::string fullPath = baseDir.empty() ? outRelPath : (baseDir + "/" + outRelPath);

        namespace fs = std::filesystem;
        // 如果文件已存在，直接返回成功（已永久缓存）
        if (fs::exists(fullPath)) {
            return true;
        }

        // 先看是否有现成预设数据
        std::vector<OpraPreset> presets;
        bool fromCache = false;
        if (!get_presets(productId, presets, fromCache, err, false, customBaseUrl)) {
            // 如果拉取列表失败，尝试单预设接口拉取
            if (!tokenBucket_.try_consume(1.0)) {
                err = "OPRA 限频保护中";
                return false;
            }
            const std::string url = opra_url_eq(presetId, customBaseUrl);
            auto resp = rta::net::http_get(url, { "User-Agent: RLPlayer/1.0.9 (OPRA-EQ)" }, 15000);
            if (!resp.ok || resp.status != 200) {
                err = "预设不存在或网络离线";
                return false;
            }
            OpraPreset singlePreset;
            std::string bodyStr(resp.body.begin(), resp.body.end());
            if (!OpraPreset::parse_json(bodyStr, singlePreset, &err)) {
                return false;
            }
            const std::string autoEq = singlePreset.to_autoeq_text();
            return save_autoeq_profile(fullPath, autoEq, &err);
        }

        for (const auto& p : presets) {
            if (p.id == presetId) {
                const std::string autoEq = p.to_autoeq_text();
                return save_autoeq_profile(fullPath, autoEq, &err);
            }
        }

        err = "在产品预设列表中未找到该预设: " + presetId;
        return false;
    }

    void clear_cache() {
        std::lock_guard<std::mutex> lk(mu_);
        memCache_.clear();
    }

private:
    OpraService() : tokenBucket_(100.0, 100.0 / 60.0) {}
    mutable std::mutex mu_;
    OpraIndex index_;
    std::string indexPath_;
    TokenBucket tokenBucket_;
    std::map<std::string, std::vector<OpraPreset>> memCache_;
};

} // namespace rta::dsp
