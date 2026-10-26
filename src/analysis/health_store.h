// health_store.h — 音频体检 B1b：按 (规范路径, mtime, size) 三元组为键的健康缓存
//
// 设计要点：
//  1) 键 = std::filesystem::weakly_canonical 规范化后的路径（generic 形式，'/' 分隔），
//     命中还须 mtime、size 逐字相等 —— 文件被改写/替换后自然失效，不需要 TTL。
//     mtime 取「自时钟纪元的毫秒数」而不是原生 count：JSON number 走 double（53 位尾数），
//     MSVC file_clock 的 100ns count 约 1.3e17，超出 2^53 会失真导致缓存永不命中（踩坑点）。
//  2) 序列化复用 track_health_json（单条体检的对象体），外层是数组，每条
//     {"path":..,"mtime":..,"size":..,"health":{..}}；解析用 src/net/json.h（只读解析器，
//     项目约定不引第三方 JSON 库）。
//  3) 损坏文件如实报错、绝不静默清空：构造时解析失败 → loadError() 非空，此时
//     save() 拒绝写盘（防止用一个空库把用户缓存覆盖掉），需要人工删掉/修复缓存文件。
//  4) save() 原子写：先写 <path>.tmp 再 rename，崩溃最多留下一个 tmp，不会写坏正身。
#pragma once
#include "track_health.h"
#include "../audio/audio_loader.h"   // rta::audio::widen（UTF-8 → UTF-16，中文缓存路径必需）
#include "../net/json.h"             // rta::net::json_parse（只读解析）
#include <filesystem>
#include <map>
#include <string>
#include <cstdint>
#include <cstdio>
#include <cmath>

namespace rta::analysis {

// 路径规范化：weakly_canonical 失败（如悬空盘符）时退回原串，绝不抛异常。
inline std::string normalize_path_key(const std::string& path) {
    try {
        return std::filesystem::weakly_canonical(rta::audio::widen(path)).generic_string();
    } catch (...) {
        return path;
    }
}

class HealthStore {
public:
    // jsonPath 不存在 → 空库（不是错误）；存在但损坏/为空 → loadError() 非空且拒绝 save()。
    explicit HealthStore(const std::string& jsonPath) : jsonPath_(jsonPath) {
        if (jsonPath_.empty()) { loadError_ = "缓存路径为空"; return; }
        std::string text, readErr;
        if (!read_whole_file(jsonPath_, &text, &readErr)) {
            loadError_ = readErr;      // 文件不存在时 readErr 为空 → 空库，不算错误
            return;
        }
        const rta::net::Json root = rta::net::json_parse(text);
        if (root.type != rta::net::Json::Arr) {
            loadError_ = "缓存文件损坏（不是合法 JSON 或根不是数组），已按空库处理且不会回写: " + jsonPath_;
            return;
        }
        for (size_t i = 0; i < root.size(); ++i) {
            const rta::net::Json& e = root.at(i);
            if (e.type != rta::net::Json::Obj) { ++skipped_; continue; }
            const std::string p = e["path"].s();
            const rta::net::Json& hj = e["health"];
            if (p.empty() || hj.type != rta::net::Json::Obj || hj["status"].s().empty()) { ++skipped_; continue; }
            Entry en;
            en.mtime = (int64_t)std::llround(e["mtime"].n());
            en.size  = (int64_t)std::llround(e["size"].n());
            TrackHealth& h = en.health;
            h.drDb = hj["dr_db"].n();
            h.lufs = hj["lufs"].n();
            h.lra = hj["lra"].n();
            h.peakDbfs = hj["peak_dbfs"].n();
            h.losslessSuspected = hj["lossless_suspected"].b || hj["lossless_suspected"].n() != 0.0;
            h.cutoffHz = hj["cutoff_hz"].n();
            h.confidence = hj["confidence"].n();
            h.reason = hj["reason"].s();
            h.fake24bit = hj["fake24"].b || hj["fake24"].n() != 0.0;
            h.durationSec = hj["duration_sec"].n();
            h.fs = (uint32_t)std::llround(hj["fs"].n());
            h.channels = (int)std::llround(hj["channels"].n());
            h.bits = (int)std::llround(hj["bits"].n());
            h.status = hj["status"].s();
            h.error = hj["error"].s();
            h.analyzedAtMs = (int64_t)std::llround(hj["analyzed_at_ms"].n());
            entries_[normalize_path_key(p)] = std::move(en);
        }
    }

    // 命中 = 路径（规范化后）+ mtime + size 三者全等。返回 false 表示需重算。
    bool lookup(const std::string& path, int64_t mtime, int64_t size, TrackHealth& out) const {
        if (!loadError_.empty()) return false;
        const auto it = entries_.find(normalize_path_key(path));
        if (it == entries_.end()) return false;
        if (it->second.mtime != mtime || it->second.size != size) return false;
        out = it->second.health;
        return true;
    }

    void upsert(const std::string& path, int64_t mtime, int64_t size, const TrackHealth& h) {
        Entry en;
        en.mtime = mtime;
        en.size = size;
        en.health = h;
        entries_[normalize_path_key(path)] = std::move(en);
    }

    // 原子写（tmp + rename）。加载时报过错的库拒绝写盘（见类头注释），返回 false。
    bool save() {
        if (!loadError_.empty()) return false;
        if (jsonPath_.empty()) return false;
        std::string data = "[";
        bool first = true;
        for (const auto& kv : entries_) {
            if (!first) data += ",";
            first = false;
            data += "{\"path\":\"" + detail::json_escape(kv.first) + "\"";
            data += ",\"mtime\":" + std::to_string((long long)kv.second.mtime);
            data += ",\"size\":" + std::to_string((long long)kv.second.size);
            data += ",\"health\":" + track_health_json(kv.second.health);
            data += "}";
        }
        data += "]";
        const std::string tmp = jsonPath_ + ".tmp";
        if (!write_whole_file(tmp, data)) return false;
        std::error_code ec;
        const std::filesystem::path from = std::filesystem::path(rta::audio::widen(tmp));
        const std::filesystem::path to   = std::filesystem::path(rta::audio::widen(jsonPath_));
        std::filesystem::rename(from, to, ec);
        if (ec) {
            // 个别实现/场景 rename 不覆盖已存在目标：退化为删正身再 rename（窗口极小，可接受）
            std::error_code ec2;
            std::filesystem::remove(to, ec2);
            ec.clear();
            std::filesystem::rename(from, to, ec);
            if (ec) { std::filesystem::remove(from, ec2); return false; }
        }
        return true;
    }

    size_t size() const { return entries_.size(); }
    const std::string& loadError() const { return loadError_; }
    int skippedEntries() const { return skipped_; }   // 解析时跳过的损坏条数（非致命）

private:
    struct Entry {
        int64_t mtime = 0;
        int64_t size = 0;
        TrackHealth health;
    };

    // 读整个文件。文件不存在返回 false 且 *err 为空（= 合法的空库）；其他读失败 *err 带原因。
    static bool read_whole_file(const std::string& path, std::string* out, std::string* err) {
        out->clear();
        err->clear();
        FILE* f = _wfopen(rta::audio::widen(path).c_str(), L"rb");   // 宽字符打开：中文路径不落 ANSI 代码页
        if (!f) {
            WIN32_FILE_ATTRIBUTE_DATA fa{};
            if (GetFileAttributesExW(rta::audio::widen(path).c_str(), GetFileExInfoStandard, &fa))
                *err = "无法读取缓存文件（可能无权限或被占用）: " + path;
            return false;   // 属性都查不到 = 不存在 = 空库
        }
        std::string data;
        char buf[65536];
        size_t got = 0;
        while ((got = std::fread(buf, 1, sizeof(buf), f)) > 0) data.append(buf, got);
        const bool ioErr = std::ferror(f) != 0;
        std::fclose(f);
        if (ioErr) { *err = "读取缓存文件失败（I/O 错误）: " + path; return false; }
        // 空文件也按「损坏」处理：正常 save() 永远写完整 JSON，不该出现 0 字节
        if (data.empty()) { *err = "缓存文件为空（0 字节，正常不应出现）: " + path; return false; }
        *out = std::move(data);
        return true;
    }

    static bool write_whole_file(const std::string& path, const std::string& data) {
        FILE* f = _wfopen(rta::audio::widen(path).c_str(), L"wb");
        if (!f) return false;
        const bool ok = std::fwrite(data.data(), 1, data.size(), f) == data.size();
        const bool closed = (std::fclose(f) == 0);
        return ok && closed;
    }

    std::string jsonPath_;
    std::map<std::string, Entry> entries_;   // 键 = normalize_path_key
    std::string loadError_;                  // 非空 = 加载失败（此时 save() 拒绝写盘）
    int skipped_ = 0;
};

} // namespace rta::analysis
