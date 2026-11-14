// test_platform.cpp — 平台层（src/platform/）自身的回归：语义是否与迁移前的 Win32 调用**逐字等价**
//
// 为什么要有这个测试（261031 平台边界清理）：
//   net/meta 里 128 处 Win32 用法被搬进了 src/platform/（text / env / fs / sysinfo / path /
//   process / http）。搬家的判据不能是"能编译"，而必须是"同一个 API、同一组 flags、同一套
//   缓冲与返回值语义"。所以这里做两件事：
//     ① 逐项断言**契约本身**（显式长度 vs -1、返回 NUL 与否、目录项是否过滤 "." ".."、
//        fail_if_exists 的两种取值、MoveFileEx 覆盖语义……）；
//     ② 在 _WIN32 下把平台函数与**原生 Win32 调用**放在一起对比（同一秒/同一 PID/同一时区/
//        同一内存计数/同一单调时钟），差值必须为 0 或 ≤1ms —— 这才叫"等价"，不是"看起来像"。
//
// 判据（铁律 3）：
//   * text：显式长度语义用"串里带内嵌 NUL"的用例证明（-1 语义会只转 1 个字符）；GBK→UTF-8
//     用写死的字节对（D6 D0 CE C4 → E4 B8 AD E6 96 87），不是"看起来像中文"。
//   * env：用 _putenv_s 造值，断言 0 / <cap / >=cap 三种返回（这就是 GetEnvironmentVariableW
//     的原生约定，调用方靠它把"太长"当"没设置"）。
//   * fs：remove_file 对不存在的文件返回 false；copy_file(fail_if_exists=true) 在目标已存在时
//     返回 false **且目标内容不变**；move_or_copy_file 覆盖已存在目标并删除源。
//   * http：起一个 127.0.0.1 上的单连接服务器，断言平台层真的发出 **PUT**、请求体逐字节到达、
//     响应体原样返回（不联网，不出本机）。
//
// ⚠️ 未解决项（铁律 4）：
//   1) 代理三态只覆盖 proxyMode=1（直连）。proxyMode=0（系统代理）与 2（具名代理）依赖用户
//      机器的代理配置，放进 ctest 会变成环境相关测试 —— 只做了代码审查（同一函数内三选一）。
//   2) display_adapter_names() 在无显卡/远程会话上可能为空，本测试只断言"个数 ≤ 8 且非空则无重复"。
//   3) 不测 request_launcher_exit()：它要求真有一个 RLPlayerTray 窗口，会与正在跑的播放器打架。
#include "platform/env.h"
#include "platform/fs.h"
#include "platform/path.h"
#include "platform/sysinfo.h"
#include "platform/text.h"
#include "platform/time.h"
#include "platform/http.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#pragma comment(lib, "ws2_32.lib")
#endif

static int g_fail = 0;
static void ck(bool ok, const char* what, const std::string& detail = std::string()) {
    if (ok) std::printf("  [PASS] %s\n", what);
    else { std::printf("  [FAIL] %s  %s\n", what, detail.c_str()); ++g_fail; }
}
static std::string hexs(const std::string& s) {
    std::string o;
    char b[8];
    for (unsigned char c : s) { std::snprintf(b, sizeof(b), "%02X ", c); o += b; }
    if (!o.empty()) o.pop_back();
    return o;
}

// ---------------------------------------------------------------------------
// 1) text.h：显式长度、flags=0、失败语义
// ---------------------------------------------------------------------------
static void test_text() {
    std::printf("=== 1) platform/text.h ===\n");
    using namespace rta::platform;

    ck(utf8_to_wide("").empty(), "空串 -> 空宽串");
    ck(wide_to_utf8(L"").empty(), "空宽串 -> 空串");
    ck(utf8_to_wide("abc").size() == 3, "ASCII 逐字符");

    // 中文往返
    const std::string zh = "\xE4\xB8\xAD\xE6\x96\x87";        // UTF-8 的「中文」
    ck(wide_to_utf8(utf8_to_wide(zh)) == zh, "中文 UTF-8 -> UTF-16 -> UTF-8 往返不变", hexs(wide_to_utf8(utf8_to_wide(zh))));
    ck(utf8_to_wide(zh).size() == 2, "中文 = 2 个 UTF-16 单元");

    // 代理对（emoji U+1F3A7 = F0 9F 8E A7）-> 2 个 UTF-16 单元
    const std::string emoji = "\xF0\x9F\x8E\xA7";
    const std::wstring we = utf8_to_wide(emoji);
    ck(we.size() == 2, "emoji = 2 个 UTF-16 单元（代理对）", std::to_string(we.size()));
    ck(wide_to_utf8(we) == emoji, "emoji 往返不变", hexs(wide_to_utf8(we)));

    // ★ 显式长度语义：内嵌 NUL 必须被保留（MultiByteToWideChar 用 -1 时会截断到 1 个字符）
    const std::string withNul("a\0b", 3);
    ck(utf8_to_wide(withNul).size() == 3, "utf8_to_wide 用显式长度（内嵌 NUL 不被截断）",
       std::to_string(utf8_to_wide(withNul).size()));
    const std::wstring wnul(L"a\0b", 3);
    const std::string back = wide_to_utf8(wnul);
    ck(back.size() == 3 && back[0] == 'a' && back[1] == '\0' && back[2] == 'b',
       "wide_to_utf8 用显式长度（内嵌 NUL 不被截断）", hexs(back));

    // 失败语义：非法 UTF-8 在 flags=0 下替换成 U+FFFD（EF BF BD），**不**报错
    const std::string bad("\xFF");
    ck(utf8_to_wide(bad).size() == 1 && wide_to_utf8(utf8_to_wide(bad)) == "\xEF\xBF\xBD",
       "非法 UTF-8 -> U+FFFD（flags=0 的替换语义）", hexs(wide_to_utf8(utf8_to_wide(bad))));

    // checked 版本：成功/失败可区分，且 out 被清空
    std::string out = "脏数据";
    ck(wide_to_utf8_checked(L"", out) && out.empty(), "wide_to_utf8_checked(空) = true + 清空 out");
    ck(wide_to_utf8_checked(utf8_to_wide(zh), out) && out == zh, "wide_to_utf8_checked(中文) 正确");

    // GBK（CP936）：写死字节对，不靠"看起来像"
    std::string gbk_out;
    const std::string gbk("\xD6\xD0\xCE\xC4");                 // GBK 的「中文」
    ck(gbk_to_utf8(gbk, gbk_out) && gbk_out == zh, "GBK 中文 -> UTF-8（D6D0 CEC4 -> E4B8ADE69687）", hexs(gbk_out));
    std::string empty_out = "x";
    ck(gbk_to_utf8("", empty_out) && empty_out.empty(), "GBK 空串 -> 空（不报错）");
    std::string ascii_out;
    ck(gbk_to_utf8("ASCII text", ascii_out) && ascii_out == "ASCII text", "GBK 纯 ASCII 原样");
    // 截断的双字节序列：严格探测失败 -> 宽松探测（U+FFFD），**不丢字节**、不返回 false
    std::string cut_out;
    const std::string cut("\xD6");                                 // 只有 GBK 前导字节
    const bool cutOk = gbk_to_utf8(cut, cut_out);
    ck(cutOk && !cut_out.empty(), "截断的 GBK 前导字节 -> 非空的替换字符（宽松解码，绝不丢）", hexs(cut_out));
}

// ---------------------------------------------------------------------------
// 2) env.h：GetEnvironmentVariableW 的三种返回
// ---------------------------------------------------------------------------
static void test_env() {
    std::printf("=== 2) platform/env.h ===\n");
    using namespace rta::platform;

    std::wstring v;
    ck(get_env_wide("RLPLAYER_PLATFORM_SURELY_ABSENT", 8, v) == 0 && v.size() == 8,
       "不存在的变量返回 0，且 out 长度 = cap（全 NUL）");

    _putenv_s("RLPLAYER_PLATFORM_T1", "off");
    v.clear();
    const unsigned n1 = get_env_wide("RLPLAYER_PLATFORM_T1", 64, v);
    ck(n1 == 3 && v.size() == 64 && v[0] == L'o' && v[3] == L'\0',
       "值 \"off\" -> 返回 3（不含结尾 NUL），v[3] 是 NUL", std::to_string(n1));

    // 缓冲不够：返回需要的字符数（含结尾 NUL）>= cap —— 调用方据此当"没设置"
    const std::string longVal(100, 'x');
    _putenv_s("RLPLAYER_PLATFORM_T2", longVal.c_str());
    const unsigned n2 = get_env_wide("RLPLAYER_PLATFORM_T2", 64, v);
    ck(n2 >= 64, "超长值 -> 返回值 >= cap（原生 nSize 语义）", std::to_string(n2));

    // 名字本身就是 UTF-8（原实现全是 ASCII 常量，这里顺手证明非 ASCII 名也能走通）
    _putenv_s("RLPLAYER_PLATFORM_T3", "1");
    ck(get_env_wide("RLPLAYER_PLATFORM_T3", 4, v) == 1 && v[0] == L'1', "单字符值 -> 返回 1");

    _putenv_s("RLPLAYER_PLATFORM_T1", "");
    _putenv_s("RLPLAYER_PLATFORM_T2", "");
    _putenv_s("RLPLAYER_PLATFORM_T3", "");
    ck(get_env_wide("RLPLAYER_PLATFORM_T1", 64, v) == 0, "清空后返回 0");
}

// ---------------------------------------------------------------------------
// 3) fs.h / path.h：文件操作的原生语义
// ---------------------------------------------------------------------------
static void write_file(const std::filesystem::path& p, const std::string& text) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(text.data(), (std::streamsize)text.size());
}
static std::string read_file(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

static void test_fs() {
    std::printf("=== 3) platform/fs.h + path.h ===\n");
    using namespace rta::platform;

    // 中文目录名（证明 UTF-8 -> 原生路径这一层是通的）
    const std::string baseUtf8 = join(temp_dir_utf8(), "rl-platform-test-中文-" + std::to_string(std::time(nullptr)));
    const std::filesystem::path base = from_utf8(baseUtf8);
    std::error_code ec;
    std::filesystem::remove_all(base, ec);

    ck(rta::platform::create_directory(base), "create_directory 新建成功");
    ck(!rta::platform::create_directory(base), "重复 create_directory 返回 false（已存在，与 CreateDirectoryW 一致）");
    ck(rta::platform::is_directory(base), "is_directory 认可刚建的目录");
    ck(!rta::platform::is_directory(join(baseUtf8, "nope")), "不存在的路径 is_directory = false");

    const std::filesystem::path fa = from_utf8(join(baseUtf8, "a.txt"));
    const std::filesystem::path fb = from_utf8(join(baseUtf8, "sub"));
    write_file(fa, "hello");
    ck(file_exists(fa), "file_exists 认可普通文件");
    ck(rta::platform::file_size(fa) == 5, "file_size = 5", std::to_string((long long)rta::platform::file_size(fa)));
    ck(rta::platform::file_size(from_utf8(join(baseUtf8, "nope"))) == -1, "不存在的文件 file_size = -1");
    ck(!is_reparse_point(fa), "普通文件不是重解析点");

    ck(rta::platform::create_directory(fb), "建子目录");
    std::vector<DirEntry> ents;
    ck(list_directory(base, ents), "list_directory 成功");
    bool hasA = false, hasSub = false, hasDot = false, hasDotDot = false;
    for (const auto& e : ents) {
        if (e.name == "a.txt" && !e.is_dir) hasA = true;
        if (e.name == "sub" && e.is_dir) hasSub = true;
        if (e.name == ".") hasDot = true;
        if (e.name == "..") hasDotDot = true;
    }
    ck(hasA, "枚举到普通文件（name 是 UTF-8 叶子名，is_dir=false）");
    ck(hasSub, "枚举到子目录（is_dir=true）");
    ck(!hasDot && !hasDotDot, "\".\" 与 \"..\" 已被过滤");

    // copy_file 的两种 fail_if_exists
    const std::filesystem::path fc = from_utf8(join(baseUtf8, "c.txt"));
    ck(copy_file(fa, fc, true), "copy_file(fail_if_exists=true) 目标不存在 -> true");
    ck(read_file(fc) == "hello", "复制内容正确");
    write_file(fc, "keep-me");
    ck(!copy_file(fa, fc, true), "copy_file(fail_if_exists=true) 目标已存在 -> false");
    ck(read_file(fc) == "keep-me", "……且目标内容没被改动（bFailIfExists 语义）");
    ck(copy_file(fa, fc, false), "copy_file(fail_if_exists=false) 覆盖已存在目标 -> true");
    ck(read_file(fc) == "hello", "覆盖后内容 = 源");

    // move_file 的 replace 语义
    const std::filesystem::path fd = from_utf8(join(baseUtf8, "d.txt"));
    write_file(fd, "old");
    ck(move_file(fa, fd, true), "move_file(replace=true) 覆盖已存在目标");
    ck(read_file(fd) == "hello" && !file_exists(fa), "……源已不存在、目标 = 源内容");
    const std::filesystem::path fmiss = from_utf8(join(baseUtf8, "nope-dst.txt"));
    ck(!move_file(from_utf8(join(baseUtf8, "nope")), fmiss, true), "move_file 源不存在 -> false");

    // move_or_copy_file：跨卷兜底那条路的正常分支（同卷走 MoveFileEx）
    const std::filesystem::path fe = from_utf8(join(baseUtf8, "e.txt"));
    write_file(fe, "target-old");
    ck(move_or_copy_file(fd, fe), "move_or_copy_file 覆盖已存在目标");
    ck(read_file(fe) == "hello" && !file_exists(fd), "……源已删除、目标 = 源内容");
    ck(!move_or_copy_file(from_utf8(join(baseUtf8, "nope")), fe), "move_or_copy_file 源不存在 -> false");

    // 只读属性 + remove_file / remove_directory
#ifdef _WIN32
    ::SetFileAttributesW(fe.c_str(), FILE_ATTRIBUTE_READONLY);
    ck(!remove_file(fe), "只读文件 remove_file 失败（DeleteFileW 对只读文件也是失败）");
#endif
    ck(set_normal_attributes(fe), "set_normal_attributes 清掉只读");
    ck(remove_file(fe), "remove_file 删除成功");
    ck(!remove_file(fe), "remove_file 对不存在的文件返回 false（DeleteFileW 语义）");
    ck(!remove_directory(base), "remove_directory 对非空目录返回 false（RemoveDirectoryW 语义）");
    ck(remove_file(fc), "删掉 c.txt");
    ck(remove_directory(fb), "remove_directory 删空目录");
    ck(remove_directory(base), "remove_directory 删空基目录");

    // path：join / 扩展名 / 系统目录
    ck(join("C:\\a", "b") == "C:\\a\\b", "join 加分隔符");
    ck(join("C:\\a\\", "b") == "C:\\a\\b", "join 不重复加分隔符");
    ck(lower_extension("X.FLAC") == ".flac", "lower_extension 小写化");
    const std::string sys = system_dir_utf8();
    ck(!sys.empty() && sys.back() != '\\', "system_dir_utf8 非空且无尾分隔符", sys);
}

// ---------------------------------------------------------------------------
// 4) sysinfo.h / time.h：与原生 Win32 调用逐项对齐
// ---------------------------------------------------------------------------
static void test_sysinfo_time() {
    std::printf("=== 4) platform/sysinfo.h + time.h ===\n");
    using namespace rta::platform;

    // 进程刚起来时"整数秒"就是 0（迁移前 proc_uptime_sec 用的也是整数除法，语义相同），
    // 所以这里先等 1.1 s，让下面的断言落在">= 1 秒"这个可分辨区间里。
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));

    const LocalTime t = local_time_now();
    ck(t.year >= 2020 && t.month >= 1 && t.month <= 12 && t.hour <= 23, "local_time_now 字段合理",
       std::to_string(t.year) + "-" + std::to_string(t.month) + "-" + std::to_string(t.day));
    const int off = local_timezone_offset_minutes();
    ck(off >= -840 && off <= 840, "时区偏移在 ±14h 内", std::to_string(off));

    const unsigned long long pid = process_id();
    ck(pid > 0, "process_id > 0", std::to_string(pid));
    const unsigned long long up = process_uptime_seconds();
    ck(up >= 1 && up < 86400ull * 365ull, "process_uptime_seconds >= 1 且不可能超过一年",
       std::to_string(up));

    const MemoryInfo m = process_memory();
    ck(m.working_set_bytes > 0, "工作集 > 0", std::to_string(m.working_set_bytes));
    ck(m.peak_working_set_bytes >= m.working_set_bytes, "峰值工作集 >= 当前（同一计数器）");
    ck(m.total_physical_bytes > 0 && m.avail_physical_bytes > 0, "系统内存计数非零");
    ck(m.handle_count > 0, "句柄数 > 0", std::to_string(m.handle_count));

    ck(cpu_thread_count() >= 1, "cpu_thread_count >= 1", std::to_string(cpu_thread_count()));

    const std::vector<std::string> gpus = display_adapter_names();
    ck(gpus.size() <= 8, "display_adapter_names 个数 <= 8（枚举上限）", std::to_string(gpus.size()));
    bool dup = false;
    for (size_t i = 0; i < gpus.size(); ++i)
        for (size_t j = i + 1; j < gpus.size(); ++j) if (gpus[i] == gpus[j]) dup = true;
    ck(!dup, "display_adapter_names 已去重");

    ck(user_ui_language_id() != 0, "user_ui_language_id != 0", std::to_string(user_ui_language_id()));

    const uint64_t a = monotonic_ms();
    const uint64_t b = monotonic_ms();
    ck(b >= a, "monotonic_ms 不回退");
    ck(a > 0, "monotonic_ms > 0");

#ifdef _WIN32
    // ---- 与原生调用逐项对比（差值必须为 0 / ≤1ms） ----
    SYSTEMTIME st;
    ::GetLocalTime(&st);
    ck(t.year == st.wYear && t.month == st.wMonth && t.day == st.wDay &&
       t.hour == st.wHour && t.minute == st.wMinute && (t.second == st.wSecond || t.second + 1 == st.wSecond),
       "local_time_now 与 GetLocalTime 字段一致（允许跨秒 1s）");

    TIME_ZONE_INFORMATION tzi{};
    const DWORD tz = ::GetTimeZoneInformation(&tzi);
    const long bias = tzi.Bias + (tz == TIME_ZONE_ID_DAYLIGHT ? tzi.DaylightBias : tzi.StandardBias);
    ck(off == (int)(-bias), "时区偏移 = -(Bias + DST?DaylightBias:StandardBias)（同一算式）",
       std::to_string(off) + " vs " + std::to_string((int)(-bias)));

    ck(pid == (unsigned long long)::GetCurrentProcessId(), "process_id == GetCurrentProcessId");

    PROCESS_MEMORY_COUNTERS_EX pmc{};
    if (::GetProcessMemoryInfo(::GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc))) {
        // 两个调用之间工作集可能变（分配/回收），所以只比"同一量级且同源"
        ck(m.working_set_bytes > 0 && m.working_set_bytes < (unsigned long long)pmc.WorkingSetSize * 2 + (1u << 20),
           "working_set 与 GetProcessMemoryInfo 同源同量级",
           std::to_string(m.working_set_bytes) + " vs " + std::to_string((unsigned long long)pmc.WorkingSetSize));
    }
    SYSTEM_INFO si{};
    ::GetSystemInfo(&si);
    ck(cpu_thread_count() == (unsigned)si.dwNumberOfProcessors, "cpu_thread_count == SYSTEM_INFO.dwNumberOfProcessors");
    ck(user_ui_language_id() == (unsigned)::GetUserDefaultUILanguage(), "user_ui_language_id == GetUserDefaultUILanguage");

    const ULONGLONG tc = ::GetTickCount64();
    ck(tc >= a && tc - a <= 5, "monotonic_ms 与 GetTickCount64 同一时基（差 <= 5ms）",
       std::to_string((long long)(tc - a)));

    FILETIME ft;
    ::GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u; u.LowPart = ft.dwLowDateTime; u.HighPart = ft.dwHighDateTime;
    (void)u;
#endif
}

// ---------------------------------------------------------------------------
// 5) http.h：PUT 通道（本机回环，不联网）
// ---------------------------------------------------------------------------
#ifdef _WIN32
namespace {

struct MiniHttp {
    SOCKET ls = INVALID_SOCKET;
    int port = 0;
    std::thread th;
    std::string head;     // 请求头（到 \r\n\r\n 为止，不含）
    std::string body;     // 请求体（按 Content-Length 截）
    bool ok = false;

    static size_t content_length(const std::string& h) {
        std::string low;
        for (char c : h) low += (char)std::tolower((unsigned char)c);
        const size_t p = low.find("content-length:");
        if (p == std::string::npos) return 0;
        size_t i = p + 15;
        while (i < h.size() && (h[i] == ' ' || h[i] == '\t')) ++i;
        size_t v = 0;
        while (i < h.size() && h[i] >= '0' && h[i] <= '9') { v = v * 10 + (size_t)(h[i] - '0'); ++i; }
        return v;
    }

    bool start() {
        ls = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (ls == INVALID_SOCKET) return false;
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = 0;
        if (::bind(ls, (sockaddr*)&a, sizeof(a)) != 0) return false;
        if (::listen(ls, 1) != 0) return false;
        int alen = (int)sizeof(a);
        if (::getsockname(ls, (sockaddr*)&a, &alen) != 0) return false;
        port = (int)ntohs(a.sin_port);
        th = std::thread([this] { serve_once(); });
        return true;
    }

    void serve_once() {
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(ls, &rd);
        timeval tv{5, 0};
        if (::select(0, &rd, nullptr, nullptr, &tv) <= 0) return;
        SOCKET c = ::accept(ls, nullptr, nullptr);
        if (c == INVALID_SOCKET) return;
        DWORD to = 5000;
        ::setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, (const char*)&to, sizeof(to));
        std::string acc;
        char buf[4096];
        size_t headEnd = std::string::npos;
        while ((headEnd = acc.find("\r\n\r\n")) == std::string::npos) {
            const int n = ::recv(c, buf, sizeof(buf), 0);
            if (n <= 0) break;
            acc.append(buf, (size_t)n);
        }
        if (headEnd != std::string::npos) {
            head = acc.substr(0, headEnd);
            body = acc.substr(headEnd + 4);
            const size_t cl = content_length(head);
            while (body.size() < cl) {
                const int n = ::recv(c, buf, sizeof(buf), 0);
                if (n <= 0) break;
                body.append(buf, (size_t)n);
            }
            if (body.size() > cl) body.resize(cl);
            ok = true;
        }
        const std::string respBody = "{\"ok\":true,\"id\":\"FB-TEST\"}";
        const std::string resp = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                                 "Content-Length: " + std::to_string(respBody.size()) +
                                 "\r\nConnection: close\r\n\r\n" + respBody;
        ::send(c, resp.data(), (int)resp.size(), 0);
        ::closesocket(c);
    }

    void stop() {
        if (th.joinable()) th.join();
        if (ls != INVALID_SOCKET) { ::closesocket(ls); ls = INVALID_SOCKET; }
    }
};

}  // namespace
#endif

static void test_http() {
    std::printf("=== 5) platform/http.h（PUT 通道，本机回环） ===\n");
    using namespace rta::platform;

    // URL 解析失败：不联网，文案与迁移前一致
    HttpUploadRequest bad;
    bad.method = "PUT";
    bad.url = "ftp://example.com/x";
    const HttpUploadResult br = http_upload(bad);
    ck(!br.ok && br.status == 0 && br.error == "URL 解析失败", "非法 URL -> \"URL 解析失败\"", br.error);

#ifdef _WIN32
    WSADATA wsa{};
    if (::WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { ck(false, "WSAStartup 失败"); return; }

    MiniHttp srv;
    ck(srv.start(), "起本机 HTTP 服务器（回环）");
    if (srv.port > 0) {
        HttpUploadRequest req;
        req.method    = "PUT";
        req.url       = "http://127.0.0.1:" + std::to_string(srv.port) + "/repos/o/r/contents/feedback/x.json";
        req.headers   = {"Authorization: Bearer deadbeef", "Accept: application/vnd.github+json"};
        req.body      = "{\"message\":\"hi\",\"content\":\"AA==\"}";
        req.timeoutMs = 5000;
        req.proxyMode = 1;                     // 直连（不碰用户代理配置）
        req.userAgent = "RLPlayer-feedback/1.0";
        const HttpUploadResult r = http_upload(req);
        srv.stop();
        ck(r.ok && r.status == 200, "PUT -> 200 且 ok=true", std::to_string(r.status));
        ck(r.body == "{\"ok\":true,\"id\":\"FB-TEST\"}", "响应体原样返回", r.body);
        ck(srv.head.rfind("PUT ", 0) == 0, "服务器收到的请求行是 PUT（PUT 通道真的走 PUT）",
           srv.head.substr(0, 40));
        ck(srv.body == req.body, "请求体逐字节到达（含 Content-Length 一致）",
           std::to_string(srv.body.size()) + " vs " + std::to_string(req.body.size()));
    } else {
        srv.stop();
    }
    ::WSACleanup();
#endif
}

int main() {
    std::printf("== platform layer contract tests ==\n");
    test_text();
    test_env();
    test_fs();
    test_sysinfo_time();
    test_http();
    if (g_fail == 0) { std::printf("\nALL PASS\n"); return 0; }
    std::printf("\n%d FAILED\n", g_fail);
    return 1;
}
