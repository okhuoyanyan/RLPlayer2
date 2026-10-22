// json.h — 极简 JSON 解析（只读，够用即可）
//
// 用途：解析 Subsonic / Emby 的 REST 响应。不追求完备，只覆盖它们实际用到的结构。
#pragma once
#include <string>
#include <vector>
#include <map>
#include <cstdlib>
#include <cmath>

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

class JsonParser {
public:
    explicit JsonParser(const std::string& s) : s_(s) {}
    Json parse() { Json j; skip(); if (p_ < s_.size()) value(j); return j; }
private:
    const std::string& s_;
    size_t p_ = 0;

    void skip() {
        while (p_ < s_.size()) {
            const char c = s_[p_];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') ++p_;
            else break;
        }
    }
    bool eat(char c) { skip(); if (p_ < s_.size() && s_[p_] == c) { ++p_; return true; } return false; }

    void str(std::string& out) {
        if (!eat('"')) return;
        out.clear();
        while (p_ < s_.size()) {
            const char c = s_[p_++];
            if (c == '"') break;
            if (c == '\\' && p_ < s_.size()) {
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
                        if (p_ + 4 <= s_.size()) {
                            const unsigned cp = (unsigned)strtoul(s_.substr(p_, 4).c_str(), nullptr, 16);
                            p_ += 4;
                            // UTF-8 编码（含代理对的简化处理）
                            if (cp < 0x80) out += (char)cp;
                            else if (cp < 0x800) { out += (char)(0xC0 | (cp >> 6)); out += (char)(0x80 | (cp & 0x3F)); }
                            else { out += (char)(0xE0 | (cp >> 12)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
                        }
                        break;
                    }
                    default: out += e;
                }
            } else out += c;
        }
    }

    void value(Json& j) {
        skip();
        if (p_ >= s_.size()) return;
        const char c = s_[p_];
        if (c == '{') {
            ++p_; j.type = Json::Obj;
            skip();
            if (eat('}')) return;
            for (;;) {
                std::string k; skip(); str(k);
                if (!eat(':')) break;
                Json v; value(v);
                j.obj[k] = std::move(v);
                if (eat(',')) continue;
                eat('}');
                break;
            }
        } else if (c == '[') {
            ++p_; j.type = Json::Arr;
            skip();
            if (eat(']')) return;
            for (;;) {
                Json v; value(v);
                j.arr.push_back(std::move(v));
                if (eat(',')) continue;
                eat(']');
                break;
            }
        } else if (c == '"') {
            j.type = Json::Str; str(j.str);
        } else if (c == 't' || c == 'f') {
            j.type = Json::Bool;
            j.b = (c == 't');
            while (p_ < s_.size() && isalpha((unsigned char)s_[p_])) ++p_;
        } else if (c == 'n') {
            j.type = Json::Null;
            while (p_ < s_.size() && isalpha((unsigned char)s_[p_])) ++p_;
        } else {
            j.type = Json::Num;
            const size_t st = p_;
            while (p_ < s_.size() && (isdigit((unsigned char)s_[p_]) || s_[p_] == '-' ||
                                      s_[p_] == '+' || s_[p_] == '.' || s_[p_] == 'e' || s_[p_] == 'E')) ++p_;
            j.num = atof(s_.substr(st, p_ - st).c_str());
        }
    }
};

inline Json json_parse(const std::string& text) { return JsonParser(text).parse(); }

} // namespace rta::net
