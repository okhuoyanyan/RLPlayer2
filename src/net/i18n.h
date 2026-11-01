// i18n.h — 界面多语言：语种表、系统语言探测、语言包加载。
//
// 设计取舍（2026-10-30 用户 261029-03）：
//   ① 语言包放【运行期文件】`data\i18n\<code>.json`，不编进 exe。
//      理由：6 种语言 × 数百条词条塞进头文件会让每次改一个词都要重编整个 rtaweb；
//      而语言包是纯数据，放盘上还能让用户自己补词条（缺失的键自动回落中文）。
//   ② 键 = **中文原文**。界面里本来就有 1 万多个中文字面量，逐处改成 t("key") 等于重写 UI；
//      用中文原文当键，前端只要在渲染后做一次文本节点替换即可，零侵入。
//   ③ 系统语言探测走 Win32 `GetUserDefaultUILanguage()`（不是 GetSystemDefaultUILanguage ——
//      前者是"当前登录用户的界面语言"，用户改了显示语言立刻生效，后者是安装时的系统语言）。
#pragma once
#include <string>
#include <vector>
#include <cstdio>
#include <fstream>
#include <sstream>
#include "net/json.h"
#include "platform/path.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace rta::net {

struct I18nLang {
    const char* code;      // BCP-47 短码（也用作文件名）
    const char* native;    // 母语写法（下拉框里显示这个，用户认得自己的语言）
    const char* zh;        // 中文名（日志/诊断用）
    bool rtl;              // 从右到左（阿拉伯语）
};

// 首批 7 种（含中文原文）。顺序 = 下拉框顺序：中文在最前（它是键语言）。
inline const std::vector<I18nLang>& i18n_supported() {
    static const std::vector<I18nLang> v = {
        { "zh", "中文",             "中文",     false },
        { "en", "English",          "英语",     false },
        { "ja", "日本語",           "日语",     false },
        { "fr", "Français",         "法语",     false },
        { "ru", "Русский",          "俄语",     false },
        { "es", "Español",          "西班牙语", false },
        { "ar", "العربية",          "阿拉伯语", true  },
    };
    return v;
}

inline bool i18n_is_supported(const std::string& code) {
    for (const auto& l : i18n_supported()) if (code == l.code) return true;
    return false;
}

inline bool i18n_is_rtl(const std::string& code) {
    for (const auto& l : i18n_supported()) if (code == l.code) return l.rtl;
    return false;
}

// Win32 主语言 ID → 我们的语种码。覆盖首批 7 种 + 几个近亲（用同一份译文）：
//   zh-Hant / zh-HK / zh-TW 仍走中文原文（键语言，零成本）。
//   法语/西班牙语的地区变体（fr-CA、es-MX…）统一归到 fr / es —— 首批不做地区化。
inline std::string i18n_from_lang_id(unsigned langid) {
    const unsigned primary = langid & 0x3FFu;
    switch (primary) {
    case 0x04: return "zh";   // 中文
    case 0x09: return "en";   // 英语
    case 0x11: return "ja";   // 日语
    case 0x0C: return "fr";   // 法语
    case 0x19: return "ru";   // 俄语
    case 0x0A: return "es";   // 西班牙语
    case 0x01: return "ar";   // 阿拉伯语
    default:   return "en";   // 其余语种首批没有译文 ⟹ 回落英语（比给中文更可读）
    }
}

// 系统界面语言（当前登录用户）。取不到时回落 en。
inline std::string i18n_detect_system() {
#ifdef _WIN32
    const LANGID id = ::GetUserDefaultUILanguage();
    if (id == 0) return "en";
    return i18n_from_lang_id((unsigned)id);
#else
    return "en";
#endif
}

// "auto" / 空 ⟹ 用系统语言；其余必须受支持，否则也回落系统语言。
inline std::string i18n_resolve(const std::string& setting) {
    if (setting.empty() || setting == "auto") return i18n_detect_system();
    if (i18n_is_supported(setting)) return setting;
    return i18n_detect_system();
}

inline std::string i18n_pack_path(const std::string& dataDir, const std::string& code) {
    return rta::platform::join(rta::platform::join(dataDir, "i18n"), code + ".json");
}

// 读语言包。返回 false 的情形（都**不是错误**，调用方回落中文即可）：
//   · 文件不存在（首批之外的语种、或用户删了 data\i18n）；
//   · JSON 损坏 / 顶层不是对象（宁可回落中文，也不要拿半份译文把界面搞乱）。
// 中文是键语言，不需要包。
inline bool i18n_load_pack(const std::string& dataDir, const std::string& code, std::string& outJson) {
    outJson.clear();
    if (code.empty() || code == "zh") return false;
    std::ifstream f(rta::platform::from_utf8(i18n_pack_path(dataDir, code)), std::ios::binary);
    if (!f) return false;
    std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (text.empty()) return false;
    rta::net::Json j;
    std::string err;
    if (!rta::net::json_parse_strict(text, j, &err)) return false;
    if (j.type != rta::net::Json::Obj) return false;
    outJson = std::move(text);
    return true;
}

}  // namespace rta::net
