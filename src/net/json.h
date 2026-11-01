// json.h — 极简 JSON 解析（只读）
//
// 用途：解析 Subsonic / Emby / DeepSeek 的 REST 响应、本地缓存文件、更新清单。
//
// ★ 2026-10-29（两轮审查 F28/F29）：两处**契约性**修复，其余行为保持不变。
//
//   F28 严格模式：旧 parse() 不校验结尾、不要求字符串闭引号、不要求对象/数组闭合 ——
//       把 {"path":"track.wav","fields":{"title":{"value":"PARTIAL 这样的截断文本
//       解析成"合法对象"，MetaStore.load 于是返回 true、title=PARTIAL，下一次 save
//       还会用它覆盖原文件。现在提供 json_parse_strict()：缺闭合、无效 token、
//       尾部垃圾、非法数字一律失败，并给出出错位置。**文件/清单类调用者必须用它**；
//       网络/LLM 响应仍走宽松的 json_parse()（模型输出常有围栏/杂质，放宽是刻意的）。
//
//   F29 代理对：旧实现把 \uD83D\uDE00 分别编码成 3 字节 UTF-8（CESU-8，即
//       ED A0 BD ED B8 80），而正确结果是 F0 9F 98 80。现在按 JSON 规范组合高低
//       代理；孤立代理在严格模式报错、宽松模式替换成 U+FFFD（绝不产出非法 UTF-8）。
#pragma once
#include <string>
#include <vector>
#include <map>
#include <cstdlib>
#include <cmath>
#include <cstdio>

namespace rta::net {

struct Json {
    enum Type { Null, Bool, Num, Str, Arr, Obj } type = Null;
    bool   b = false;
    double num = 0.0;
    std::string str;
    std::vector<Json> arr;
    std::map<std::string, Json> obj;

    bool has(const std::string& k) const { return type == Obj && obj.count(k) > 0; }
    const Json& operator[](const std::string& k) const {
        static const Json nil;
        auto it = obj.find(k);
        return (type == Obj && it != obj.end()) ? it->second : nil;
    }
    const Json& at(size_t i) const {
        static const Json nil;
        return (type == Arr && i < arr.size()) ? arr[i] : nil;
    }
    size_t size() const { return type == Arr ? arr.size() : 0; }
    std::string s() const { return type == Str ? str : (type == Num ? std::to_string((long long)num) : std::string()); }
    double n() const { return type == Num ? num : (type == Str ? atof(str.c_str()) : 0.0); }
    bool ok() const { return type != Null; }
};

namespace json_detail {
// 把一个 Unicode 码点写成 UTF-8（严格：拒绝代理区与越界）
inline bool utf8_append(std::string& out, unsigned cp) {
    if (cp <= 0x7Fu) { out += (char)cp; return true; }
    if (cp <= 0x7FFu) {
        out += (char)(0xC0 | (cp >> 6));
        out += (char)(0x80 | (cp & 0x3F));
        return true;
    }
    if (cp >= 0xD800u && cp <= 0xDFFFu) return false;         // 代理区不能单独编码
    if (cp <= 0xFFFFu) {
        out += (char)(0xE0 | (cp >> 12));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
        return true;
    }
    if (cp <= 0x10FFFFu) {
        out += (char)(0xF0 | (cp >> 18));
        out += (char)(0x80 | ((cp >> 12) & 0x3F));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
        return true;
    }
    return false;
}
// U+FFFD（宽松模式下替代孤立代理，保证输出始终是合法 UTF-8）
inline void utf8_replacement(std::string& out) { out += "\xEF\xBF\xBD"; }
} // namespace json_detail

class JsonParser {
public:
    explicit JsonParser(const std::string& s, bool strict = false) : s_(s), strict_(strict) {}

    // 宽松：失败时返回已解析的部分（历史行为）
    Json parse() { Json j; skip(); if (p_ < s_.size()) value(j); return j; }

    // 严格：必须恰好消费一个完整的 JSON 值，且其后只有空白
    bool parse_strict(Json& out, std::string* err = nullptr) {
        p_ = 0; err_.clear(); failPos_ = 0;
        skip();
        if (p_ >= s_.size()) { fail(0, "空文档"); if (err) *err = err_; return false; }
        Json j;
        if (!value(j)) { if (err) *err = err_; return false; }
        skip();
        if (p_ != s_.size()) {
            fail(p_, "JSON 结束后还有多余内容（截断/拼接/尾部垃圾）");
            if (err) *err = err_;
            return false;
        }
        out = std::move(j);
        return true;
    }

    const std::string& error() const { return err_; }
    size_t error_pos() const { return failPos_; }

private:
    const std::string& s_;
    bool   strict_ = false;
    size_t p_ = 0;
    std::string err_;
    size_t failPos_ = 0;

    bool fail(size_t pos, const std::string& msg) {
        if (err_.empty()) { err_ = msg + "（位置 " + std::to_string(pos) + "）"; failPos_ = pos; }
        return false;
    }

    void skip() {
        while (p_ < s_.size()) {
            const char c = s_[p_];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') ++p_;
            else break;
        }
    }
    bool eat(char c) { skip(); if (p_ < s_.size() && s_[p_] == c) { ++p_; return true; } return false; }

    bool hex4(unsigned& v) {
        if (p_ + 4 > s_.size()) return false;
        v = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = s_[p_ + (size_t)i];
            unsigned d;
            if (c >= '0' && c <= '9') d = (unsigned)(c - '0');
            else if (c >= 'a' && c <= 'f') d = (unsigned)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') d = (unsigned)(c - 'A' + 10);
            else return false;
            v = (v << 4) | d;
        }
        p_ += 4;
        return true;
    }

    // 返回 false = 严格模式下必须失败（宽松模式会把问题吞掉并继续）
    bool str(std::string& out) {
        if (!eat('"')) { return fail(p_, "字符串缺少起始引号"); }
        out.clear();
        while (p_ < s_.size()) {
            const char c = s_[p_++];
            if (c == '"') return true;
            if (c != '\\') {
                // 控制字符在 JSON 里必须转义；宽松模式照收（历史行为）
                if (strict_ && (unsigned char)c < 0x20u)
                    return fail(p_ - 1, "字符串里出现未转义的控制字符");
                out += c;
                continue;
            }
            if (p_ >= s_.size()) break;                 // 反斜杠后被截断
            const char e = s_[p_++];
            switch (e) {
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'r': out += '\r'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case '/': out += '/'; break;
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case 'u': {
                    unsigned cp = 0;
                    if (!hex4(cp)) {
                        if (strict_) return fail(p_, "\\u 转义不足 4 位或含非法十六进制");
                        json_detail::utf8_replacement(out);
                        break;
                    }
                    if (cp >= 0xD800u && cp <= 0xDBFFu) {
                        // 高代理：必须紧跟 \uDC00-\uDFFF
                        const size_t save = p_;
                        if (p_ + 1 < s_.size() && s_[p_] == '\\' && s_[p_ + 1] == 'u') {
                            p_ += 2;
                            unsigned lo = 0;
                            if (hex4(lo) && lo >= 0xDC00u && lo <= 0xDFFFu) {
                                const unsigned full = 0x10000u + ((cp - 0xD800u) << 10) + (lo - 0xDC00u);
                                json_detail::utf8_append(out, full);
                                break;
                            }
                            p_ = save;
                        }
                        if (strict_) return fail(p_ - 6, "孤立的高代理（没有配对的低代理）");
                        json_detail::utf8_replacement(out);
                        break;
                    }
                    if (cp >= 0xDC00u && cp <= 0xDFFFu) {
                        if (strict_) return fail(p_ - 6, "孤立的低代理（前面没有高代理）");
                        json_detail::utf8_replacement(out);
                        break;
                    }
                    if (!json_detail::utf8_append(out, cp)) {
                        if (strict_) return fail(p_ - 6, "非法码点");
                        json_detail::utf8_replacement(out);
                    }
                    break;
                }
                default:
                    if (strict_) return fail(p_ - 1, "非法的反斜杠转义");
                    out += e;
            }
        }
        if (strict_) return fail(p_, "字符串没有闭合引号（文档被截断）");
        return true;
    }

    bool number(Json& j) {
        const size_t st = p_;
        if (p_ < s_.size() && (s_[p_] == '-' || s_[p_] == '+')) ++p_;
        bool anyDigit = false;
        while (p_ < s_.size() && isdigit((unsigned char)s_[p_])) { ++p_; anyDigit = true; }
        if (p_ < s_.size() && s_[p_] == '.') {
            ++p_;
            while (p_ < s_.size() && isdigit((unsigned char)s_[p_])) { ++p_; anyDigit = true; }
        }
        if (p_ < s_.size() && (s_[p_] == 'e' || s_[p_] == 'E')) {
            ++p_;
            if (p_ < s_.size() && (s_[p_] == '-' || s_[p_] == '+')) ++p_;
            while (p_ < s_.size() && isdigit((unsigned char)s_[p_])) ++p_;
        }
        if (strict_ && !anyDigit) return fail(st, "非法数字");
        j.type = Json::Num;
        j.num = atof(s_.substr(st, p_ - st).c_str());
        return true;
    }

    bool value(Json& j) {
        skip();
        if (p_ >= s_.size()) { if (strict_) return fail(p_, "值缺失（文档被截断）"); return true; }
        const char c = s_[p_];
        if (c == '{') {
            ++p_; j.type = Json::Obj;
            skip();
            if (eat('}')) return true;
            for (;;) {
                std::string k;
                skip();
                if (strict_ && (p_ >= s_.size() || s_[p_] != '"')) return fail(p_, "对象的键必须是字符串");
                if (!str(k)) { if (strict_) return false; }
                if (!eat(':')) { if (strict_) return fail(p_, "对象缺少冒号"); break; }
                Json v;
                if (!value(v)) { if (strict_) return false; }
                j.obj[k] = std::move(v);
                if (eat(',')) continue;
                if (!eat('}')) { if (strict_) return fail(p_, "对象没有闭合 }（文档被截断）"); }
                break;
            }
            return true;
        }
        if (c == '[') {
            ++p_; j.type = Json::Arr;
            skip();
            if (eat(']')) return true;
            for (;;) {
                Json v;
                if (!value(v)) { if (strict_) return false; }
                j.arr.push_back(std::move(v));
                if (eat(',')) continue;
                if (!eat(']')) { if (strict_) return fail(p_, "数组没有闭合 ]（文档被截断）"); }
                break;
            }
            return true;
        }
        if (c == '"') { j.type = Json::Str; return str(j.str) || !strict_; }
        if (c == 't' || c == 'f') {
            const bool wantTrue = (c == 't');
            const char* lit = wantTrue ? "true" : "false";
            const size_t n = wantTrue ? 4u : 5u;
            if (strict_) {
                if (p_ + n > s_.size() || s_.compare(p_, n, lit) != 0)
                    return fail(p_, wantTrue ? "非法字面量（应为 true）" : "非法字面量（应为 false）");
                p_ += n;
            } else {
                while (p_ < s_.size() && isalpha((unsigned char)s_[p_])) ++p_;
            }
            j.type = Json::Bool; j.b = wantTrue;
            return true;
        }
        if (c == 'n') {
            if (strict_) {
                if (p_ + 4 > s_.size() || s_.compare(p_, 4, "null") != 0)
                    return fail(p_, "非法字面量（应为 null）");
                p_ += 4;
            } else {
                while (p_ < s_.size() && isalpha((unsigned char)s_[p_])) ++p_;
            }
            j.type = Json::Null;
            return true;
        }
        if (strict_ && !(isdigit((unsigned char)c) || c == '-'))
            return fail(p_, "无法识别的 JSON 值");
        return number(j);
    }
};

// 宽松解析（网络/LLM 响应用；失败时返回已解析的部分）
inline Json json_parse(const std::string& text) { return JsonParser(text, false).parse(); }

// ★ F28：严格解析（缓存文件 / 更新清单 / 配置文件用）。
//   返回 false 时 out 不被修改，err 给出「原因 + 位置」。
inline bool json_parse_strict(const std::string& text, Json& out, std::string* err = nullptr) {
    JsonParser p(text, true);
    return p.parse_strict(out, err);
}

} // namespace rta::net