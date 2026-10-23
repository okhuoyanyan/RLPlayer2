// test_year.cpp — E-14：年份清洗纯函数（pv_year4 统一窗口与清洗规则）回归
//
// 判据说明（铁律 3）：
//   * 覆盖任务书要求的 4 大类边界：
//     ① '1999' 原样 4 位年；
//     ② '1999-05-01' ISO 日期；
//     ③ '[1000,2100] 窗口'：含 1000/2100 端点，排除 0999/2101；
//     ④ 脏值：'0'、'9999'、'-1'、空串，以及 P2 事故形态 '0000'/'0000-00-00'。
//   * 纯函数、无网络、零外部依赖、不依赖 Windows SDK。
#include "meta/year.h"
#include <cstdio>
#include <string>

using rta::meta::pv_year4;

static int g_fail = 0;
static void ck(bool cond, const char* what, const std::string& detail = "") {
    if (cond) {
        std::printf("  [PASS] %s\n", what);
    } else {
        std::printf("  [FAIL] %s  %s\n", what, detail.c_str());
        ++g_fail;
    }
}

int main() {
    std::printf("== E-14 pv_year4 年份清洗边界回归 ==\n");

    // 1. 常规标准形态
    ck(pv_year4("1999") == "1999", "常规 4 位年 '1999' → '1999'", pv_year4("1999"));
    ck(pv_year4("1999-05-01") == "1999", "ISO 日期 '1999-05-01' → '1999'", pv_year4("1999-05-01"));
    ck(pv_year4("2003-07-31") == "2003", "QQ 日期 '2003-07-31' → '2003'", pv_year4("2003-07-31"));
    ck(pv_year4("2003-07-31T07:00:00Z") == "2003", "iTunes ISO 全形态 → '2003'", pv_year4("2003-07-31T07:00:00Z"));
    ck(pv_year4("20030731") == "2003", "紧凑日期 YYYYMMDD → '2003'", pv_year4("20030731"));
    ck(pv_year4("我 1998 年") == "1998", "混排文本取首段 ≥4 位数字 → '1998'", pv_year4("我 1998 年"));

    // 2. 合理窗口 [1000, 2100] 边界
    ck(pv_year4("1000") == "1000", "窗口下界端点 1000 含", pv_year4("1000"));
    ck(pv_year4("2100") == "2100", "窗口上界端点 2100 含", pv_year4("2100"));
    ck(pv_year4("0999") == "", "窗口外下界 0999 (<1000) → 空", pv_year4("0999"));
    ck(pv_year4("2101") == "", "窗口外上界 2101 (>2100) → 空", pv_year4("2101"));

    // 3. 脏值防御（0 / 9999 / -1 / 空）
    ck(pv_year4("0") == "", "脏值 '0' → 空串", pv_year4("0"));
    ck(pv_year4("9999") == "", "脏值 '9999'(>2100) → 空串", pv_year4("9999"));
    ck(pv_year4("-1") == "", "脏值 '-1' → 空串", pv_year4("-1"));
    ck(pv_year4("") == "", "脏值 空串 → 空串", pv_year4(""));
    ck(pv_year4("0000") == "", "P2 真实事故形态 '0000' → 空串", pv_year4("0000"));
    ck(pv_year4("0000-00-00") == "", "QQ 源真实事故形态 '0000-00-00' → 空串", pv_year4("0000-00-00"));
    ck(pv_year4("99999") == "", "超长纯数字 '99999' → 空串", pv_year4("99999"));
    ck(pv_year4("abc") == "", "纯非数字 'abc' → 空串", pv_year4("abc"));
    ck(pv_year4("未出版") == "", "非数字中文 '未出版' → 空串", pv_year4("未出版"));

    // 4. 幂等性
    ck(pv_year4(pv_year4("1999-05-01")) == "1999", "清洗结果幂等");

    std::printf("\n结果：%s  （通过 %s，失败 %d）\n",
                g_fail == 0 ? "ALL PASS" : "FAILED",
                g_fail == 0 ? "全部" : "见上方 [FAIL]",
                g_fail);
    return g_fail ? 1 : 0;
}
