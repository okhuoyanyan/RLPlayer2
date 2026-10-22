// test_wv2_version.cpp — WebView2 双轨运行时版本护栏纯逻辑回归测试
//
// 固化自 tools/wv2_host.h 的纯函数：
//   - parse_version
//   - compare_version
//   - plan_tracks
//   - env_mode
// 零设备依赖、不创建 WebView2 环境、不开窗口、不联网（只测版本决策纯逻辑）。
#include "wv2_host.h"
#include <cstdio>
#include <string>
#include <vector>

using namespace rta::wv2;

static int g_fail = 0;

static void ck(bool cond, const char* what, const std::string& detail = "") {
    if (cond) {
        std::printf("  [PASS] %s\n", what);
    } else {
        std::printf("  [FAIL] %s  %s\n", what, detail.c_str());
        ++g_fail;
    }
}

static RuntimeInfo mk(bool found, const wchar_t* ver, const wchar_t* src = L"test") {
    RuntimeInfo r;
    r.found = found;
    if (ver) r.version = ver;
    r.source = src;
    return r;
}

static std::string to_narrow(const std::wstring& ws) {
    std::string s;
    for (wchar_t c : ws) {
        if (c <= 127) s.push_back((char)c);
        else s.push_back('?');
    }
    return s;
}

int main() {
    std::printf("== test_wv2_version：WebView2 双轨版本护栏纯逻辑门禁 ==\n");

    // =========================================================================
    // Part 1: parse_version / compare_version 边界测试
    // =========================================================================
    std::printf("[Part 1] parse_version 与 compare_version 边界\n");
    {
        const Version a = parse_version(L"155.0.4283.62");
        const Version b = parse_version(L"v155.0.4283.62 (Official build)");
        const Version c = parse_version(L"155.0.4283");
        const Version d = parse_version(L"155.0.4283.0");
        const Version na = parse_version(L"n/a");
        const Version empty = parse_version(L"");

        ck(a.valid(), "a(155.0.4283.62) 有效");
        ck(a.parts.size() == 4, "a 分段数 == 4");
        ck(b.valid(), "b(v155.0.4283.62 ...) 有效");
        ck(b.parts.size() == 4, "b 提取出 4 段版本号");
        ck(c.valid(), "c(155.0.4283) 有效");
        ck(c.parts.size() == 3, "c 分段数 == 3");
        ck(!na.valid(), "na(\"n/a\") 无效");
        ck(!empty.valid(), "空字符串无效");

        ck(compare_version(a, b) == 0, "compare(a, b) == 0 (带前缀后缀等价)");
        ck(compare_version(a, c) > 0, "compare(a, c) > 0 (155.0.4283.62 > 155.0.4283)");
        ck(compare_version(c, a) < 0, "compare(c, a) < 0 (155.0.4283 < 155.0.4283.62)");
        ck(compare_version(c, d) == 0, "compare(c, d) == 0 (末尾补零相等: 155.0.4283 == 155.0.4283.0)");
        ck(compare_version(d, c) == 0, "compare(d, c) == 0");

        const Version v1 = parse_version(L"154.9.9999.99");
        const Version v2 = parse_version(L"155.0.100.1");
        ck(compare_version(v1, v2) < 0, "compare(154.x, 155.x) < 0");
        ck(compare_version(v2, v1) > 0, "compare(155.x, 154.x) > 0");
    }

    // =========================================================================
    // Part 2: guard_test.cpp 固化的 10 个用例
    // =========================================================================
    std::printf("[Part 2] plan_tracks 10 用例\n");

    // 1. 同版本: auto 模式下两版相同 -> [System, Bundled]
    {
        std::wstring reason;
        auto order = plan_tracks(L"auto", mk(true, L"155.0.4283.62"), mk(true, L"155.0.4283.62"), reason);
        ck(order.size() == 2 && order[0] == Track::System && order[1] == Track::Bundled,
           "用例 1: 同版本 -> [System, Bundled]");
        ck(reason.find(L"同版本") != std::wstring::npos, "用例 1 reason 包含 '同版本'", to_narrow(reason));
    }

    // 2. 系统旧: 154 < 155 -> [Bundled, System] (版本护栏防御旧运行库)
    {
        std::wstring reason;
        auto order = plan_tracks(L"auto", mk(true, L"154.0.2000.10"), mk(true, L"155.0.4283.62"), reason);
        ck(order.size() == 2 && order[0] == Track::Bundled && order[1] == Track::System,
           "用例 2: 系统旧 -> [Bundled, System]");
        ck(reason.find(L"旧于包内版") != std::wstring::npos, "用例 2 reason 包含 '旧于包内版'", to_narrow(reason));
    }

    // 3. 系统新: 156 > 155 -> [System, Bundled]
    {
        std::wstring reason;
        auto order = plan_tracks(L"auto", mk(true, L"156.1.1000.5"), mk(true, L"155.0.4283.62"), reason);
        ck(order.size() == 2 && order[0] == Track::System && order[1] == Track::Bundled,
           "用例 3: 系统新 -> [System, Bundled]");
        ck(reason.find(L"新于包内版") != std::wstring::npos, "用例 3 reason 包含 '新于包内版'", to_narrow(reason));
    }

    // 4. 系统版本读不出 -> [Bundled, System] (优先包内版)
    {
        std::wstring reason;
        auto order = plan_tracks(L"auto", mk(true, L""), mk(true, L"155.0.4283.62"), reason);
        ck(order.size() == 2 && order[0] == Track::Bundled && order[1] == Track::System,
           "用例 4: 系统版本读不出 -> [Bundled, System]");
        ck(reason.find(L"版本号读不出") != std::wstring::npos, "用例 4 reason 包含 '版本号读不出'", to_narrow(reason));
    }

    // 5. 只有系统版 -> [System]
    {
        std::wstring reason;
        auto order = plan_tracks(L"auto", mk(true, L"155.0.4283.62"), mk(false, nullptr), reason);
        ck(order.size() == 1 && order[0] == Track::System,
           "用例 5: 只有系统 -> [System]");
        ck(reason.find(L"包内固定版不存在") != std::wstring::npos, "用例 5 reason 包含 '包内固定版不存在'", to_narrow(reason));
    }

    // 6. 只有包内版 -> [Bundled]
    {
        std::wstring reason;
        auto order = plan_tracks(L"auto", mk(false, nullptr), mk(true, L"155.0.4283.62"), reason);
        ck(order.size() == 1 && order[0] == Track::Bundled,
           "用例 6: 只有包内 -> [Bundled]");
        ck(reason.find(L"系统 Evergreen 未安装") != std::wstring::npos, "用例 6 reason 包含 '系统 Evergreen 未安装'", to_narrow(reason));
    }

    // 7. 都没有 -> []
    {
        std::wstring reason;
        auto order = plan_tracks(L"auto", mk(false, nullptr), mk(false, nullptr), reason);
        ck(order.empty(), "用例 7: 都没有 -> []");
        ck(reason.find(L"都不存在") != std::wstring::npos, "用例 7 reason 包含 '都不存在'", to_narrow(reason));
    }

    // 8. 补零相等: 155.0.4283.62 vs 155.0.4283.062 -> [System, Bundled]
    {
        std::wstring reason;
        auto order = plan_tracks(L"auto", mk(true, L"155.0.4283.62"), mk(true, L"155.0.4283.062"), reason);
        ck(order.size() == 2 && order[0] == Track::System && order[1] == Track::Bundled,
           "用例 8: 补零相等 -> [System, Bundled]");
        ck(reason.find(L"同版本") != std::wstring::npos, "用例 8 reason 包含 '同版本'", to_narrow(reason));
    }

    // 9. 强制 system: mode=system -> [System]
    {
        std::wstring reason;
        auto order = plan_tracks(L"system", mk(true, L"155.0.4283.62"), mk(true, L"155.0.4283.62"), reason);
        ck(order.size() == 1 && order[0] == Track::System,
           "用例 9: 强制 system -> [System]");
        ck(reason.find(L"强制系统版") != std::wstring::npos, "用例 9 reason 包含 '强制系统版'", to_narrow(reason));
    }

    // 10. 强制 bundled: mode=bundled -> [Bundled]
    {
        std::wstring reason;
        auto order = plan_tracks(L"bundled", mk(true, L"155.0.4283.62"), mk(true, L"155.0.4283.62"), reason);
        ck(order.size() == 1 && order[0] == Track::Bundled,
           "用例 10: 强制 bundled -> [Bundled]");
        ck(reason.find(L"强制包内固定版") != std::wstring::npos, "用例 10 reason 包含 '强制包内固定版'", to_narrow(reason));
    }

    // =========================================================================
    // Part 3: env_mode 环境变量解析测试
    // =========================================================================
    std::printf("[Part 3] env_mode 环境变量解析\n");
    {
        // 备份原有变量
        const wchar_t* const varName = L"RLPLAYER_WEBVIEW2_MODE";
        wchar_t orig[256] = {0};
        const DWORD origLen = GetEnvironmentVariableW(varName, orig, 256);
        const bool hadOrig = (origLen > 0 && origLen < 256);

        // 未设置 -> auto
        SetEnvironmentVariableW(varName, nullptr);
        ModeSel m = env_mode();
        ck(m.mode == L"auto" && !m.unknown, "未设置环境变量 -> mode=auto, unknown=false");

        // 空串 -> auto
        SetEnvironmentVariableW(varName, L"");
        m = env_mode();
        ck(m.mode == L"auto" && !m.unknown, "空字符串 -> mode=auto, unknown=false");

        // system 大小写与空白
        SetEnvironmentVariableW(varName, L"  SyStEm  \n");
        m = env_mode();
        ck(m.mode == L"system" && !m.unknown, "大小写混杂空白 '  SyStEm  \n' -> mode=system");

        // bundled
        SetEnvironmentVariableW(varName, L"BUNDLED");
        m = env_mode();
        ck(m.mode == L"bundled" && !m.unknown, "'BUNDLED' -> mode=bundled");

        // 未知值 -> unknown=true, mode=auto (容错回退)
        SetEnvironmentVariableW(varName, L"invalid_track_xxx");
        m = env_mode();
        ck(m.mode == L"auto" && m.unknown, "非法值 'invalid_track_xxx' -> mode=auto, unknown=true");

        // 恢复环境
        if (hadOrig) SetEnvironmentVariableW(varName, orig);
        else SetEnvironmentVariableW(varName, nullptr);
    }

    std::printf("== 测试完成: fail=%d ==\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
