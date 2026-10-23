// year.h — 元数据年份清洗（统一窗口 [1000, 2100] 与清洗规则）
//
// 对应 docs/元数据补全调研与规划.md §1.6.E 的 E-14：
//   * 消除 entities.h 与 provider.h 之间的重复实现；
//   * 保证候选年份（provider 侧）与 Release 判等键年份（实体侧）使用完全同一套清洗与窗口规则；
//   * 纯函数、无第三方依赖、不依赖网络与 Windows SDK。
#pragma once
#include <cstddef>
#include <string>

namespace rta::meta {

// 取年份：找第一段「连续数字且长度 >= 4」的数字串，取其前 4 位；找不到返回 ""。
//   "1999" → "1999"；"1999-05-01" → "1999"；"2003-07-31" → "2003"；"20030731" → "2003"（紧凑日期形态）。
// 合理窗口 [1000, 2100]：取到的 4 位年份落在窗口内才返回，否则返回 ""。
//   1000 / 2100 端点均包含；0999 (<1000) 与 2101 (>2100) 均过滤为空串。
// 脏值防御：
//   "0" / "0000" / "0000-00-00"（P2 真实事故形态）→ ""；
//   "9999" / "99999"（窗口外）→ ""；
//   "-1" / "abc" / ""（非数字或长度不足）→ ""。
inline std::string pv_year4(const std::string& s) {
    const size_t n = s.size();
    size_t i = 0;
    while (i < n) {
        if (s[i] < '0' || s[i] > '9') { ++i; continue; }
        size_t j = i;
        while (j < n && s[j] >= '0' && s[j] <= '9') ++j;
        if (j - i >= 4) {
            const std::string y = s.substr(i, 4);
            const int v = (y[0] - '0') * 1000 + (y[1] - '0') * 100 +
                          (y[2] - '0') * 10 + (y[3] - '0');
            return (v >= 1000 && v <= 2100) ? y : std::string();
        }
        i = j;
    }
    return "";
}

} // namespace rta::meta
