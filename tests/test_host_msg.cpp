// test_host_msg.cpp — src/platform/host_msg.h：页面 ↔ 宿主的 postMessage 协议解析
//
// 为什么要有这个测试（261024-02 #2）：资料里的链接要交给系统浏览器打开，这是**安全边界**。
// 白名单如果只藏在 launcher.cpp 的 lambda 里，就只能靠肉眼看 —— 这里把判定抽成纯函数后
// 直接断言：http/https 放行，file:/javascript:/data:/vbscript:/ftp:/相对路径/带换行注入一律拒绝。
//
// 判据（铁律 3）：被测对象是纯字符串规则，逐条断言输入→输出；不依赖网络、不打开浏览器、
// 不碰磁盘。真正的 ShellExecuteW 调用属于 Win32 副作用，本测试**不覆盖**（见文末未解决项）。
//
// ⚠️ 未解决项（铁律 4）：
//   1) ShellExecuteW 本身没有单测（会真的拉起系统浏览器，不能进自动化）；
//      真实 WebView2 下 launcher.log 的「openurl OK」记录需要实机窗口跑一次（本轮未做，风险低）。
//   2) 白名单只校验协议与主机名非空，不校验 URL 是否真的可达（打开失败由 ShellExecute 的返回值记日志）。
#include "platform/host_msg.h"

#include <cstdio>
#include <string>

// 与其它测试一致：不引 Windows 头，只声明用到的 C 接口（kernel32!SetConsoleOutputCP）
#ifdef _WIN32
extern "C" __declspec(dllimport) int __stdcall SetConsoleOutputCP(unsigned int);
#endif
#ifndef CP_UTF8
#define CP_UTF8 65001
#endif

using rta::platform::HostAction;
using rta::platform::HostMessage;
using rta::platform::external_url_allowed;
using rta::platform::parse_host_message;

static int g_fail = 0;
static void ck(bool ok, const char* what, const std::string& detail = std::string()) {
    if (ok) std::printf("  [PASS] %s\n", what);
    else { std::printf("  [FAIL] %s  %s\n", what, detail.c_str()); ++g_fail; }
}

int main() {
#ifdef _WIN32
    SetConsoleOutputCP(65001);
#endif
    std::printf("=== 1) 既有协议不回归（min/max/close/drag/theme） ===\n");
    ck(parse_host_message("min").action == HostAction::Minimize, "min → Minimize");
    ck(parse_host_message("max").action == HostAction::Maximize, "max → Maximize");
    ck(parse_host_message("close").action == HostAction::Close, "close → Close");
    ck(parse_host_message("drag").action == HostAction::Drag, "drag → Drag");
    {
        const auto m = parse_host_message("theme:blue");
        ck(m.action == HostAction::Theme && m.arg == "blue", "theme:blue → Theme(arg=blue)");
        ck(parse_host_message("theme:dark").arg == "dark", "theme:dark → arg=dark");
    }
    ck(parse_host_message("openurl:https://x.y/z").action == HostAction::OpenUrl, "openurl: → OpenUrl");
    ck(parse_host_message("nonsense").action == HostAction::None, "不认识的消息 → None（不误触发动作）");

    std::printf("=== 2) 外链白名单：http / https 放行 ===\n");
    ck(external_url_allowed("http://example.com"), "http://example.com 放行");
    ck(external_url_allowed("https://monster-siren.hypergryph.com/music/779484"), "https 带路径放行");
    ck(external_url_allowed("HTTPS://Example.COM/a?b=1#c"), "大小写不敏感");
    ck(external_url_allowed("https://zh.wikipedia.org/wiki/%E4%BC%8A%E5%88%A9%E4%B9%8B"), "URL 编码路径放行");
    ck(external_url_allowed("https://user:pw@host:8443/p"), "带端口/认证信息放行（仍是 http(s)）");

    std::printf("=== 3) 非法协议必须拒绝 ===\n");
    ck(!external_url_allowed("file:///C:/Windows/System32/calc.exe"), "file: 拒绝");
    ck(!external_url_allowed("javascript:alert(1)"), "javascript: 拒绝");
    ck(!external_url_allowed("JavaScript:alert(1)"), "JavaScript: 大小写混写也拒绝");
    ck(!external_url_allowed("data:text/html,<script>alert(1)</script>"), "data: 拒绝");
    ck(!external_url_allowed("vbscript:msgbox(1)"), "vbscript: 拒绝");
    ck(!external_url_allowed("ftp://example.com/x"), "ftp: 拒绝");
    ck(!external_url_allowed("ms-settings:display"), "ms-settings: 拒绝");
    ck(!external_url_allowed("C:\\Windows\\notepad.exe"), "本地盘符路径拒绝");
    ck(!external_url_allowed("//example.com/x"), "协议相对 URL 拒绝");
    ck(!external_url_allowed("/local/path"), "相对路径拒绝");
    ck(!external_url_allowed(""), "空串拒绝");
    ck(!external_url_allowed("http://"), "只有协议没有主机拒绝");
    ck(!external_url_allowed("https:///path"), "三斜杠（空主机）拒绝");
    ck(!external_url_allowed("http://a b.com"), "主机里带空格拒绝");
    ck(!external_url_allowed("http://a.com/\r\nHost: evil"), "换行注入拒绝");
    ck(!external_url_allowed("http://a.com/\t"), "制表符拒绝");

    std::printf("=== 4) parse_host_message 对非法外链的标记 ===\n");
    {
        const auto m = parse_host_message("openurl:file:///C:/secret.txt");
        ck(m.action == HostAction::OpenUrl && !m.allowed, "openurl:file: → OpenUrl 但 allowed=false");
        ck(!m.reason.empty(), "拒绝原因非空（会写进 launcher.log）", m.reason);
    }
    {
        const auto m = parse_host_message("openurl:https://ok.example/x");
        ck(m.action == HostAction::OpenUrl && m.allowed && m.arg == "https://ok.example/x",
           "合法外链：allowed=true 且 URL 原样传递（不截断、不改写）", m.arg);
    }

    std::printf("\n%s（%d 项失败）\n", g_fail == 0 ? "host_msg 全部通过" : "host_msg 有失败", g_fail);
    return g_fail == 0 ? 0 : 1;
}
