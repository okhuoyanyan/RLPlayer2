#pragma once
// Pure CLI parsing: no Windows, registry, COM, ASIO or audio dependency.
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <set>
#include <string>

namespace rta::audio {

enum class AsioProbeAction { Inspect, PcmCheck, PcmStream, DsdCreate };

struct AsioProbeOptions {
    AsioProbeAction action = AsioProbeAction::Inspect;
    int deviceIndex = -1;
    std::string clsid;
    double rate = 48000.0, dsdRate = 2822400.0;
    int channels = 2, milliseconds = 1000;
    long bufferUnits = 0, fillBytes = 0;
    bool withDsd = false, useHwnd = false, nopCallback = false;
    bool rateFirst = false, outReady = false, allChannels = false, spin = false;
    bool help = false;
};

inline bool asio_probe_uint(const std::string& value, unsigned long long maximum,
                            unsigned long long& result) {
    if (value.empty()) return false;
    result = 0;
    for (char c : value) {
        if (c < '0' || c > '9') return false;
        const unsigned digit = unsigned(c - '0');
        if (result > maximum / 10 || (result == maximum / 10 && digit > maximum % 10)) return false;
        result = result * 10 + digit;
    }
    return true;
}

inline bool asio_probe_rate(const std::string& value, double& result) {
    if (value.empty() || value[0] < '0' || value[0] > '9') return false;
    for (char c : value)
        if (!((c >= '0' && c <= '9') || c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-')) return false;
    errno = 0;
    char* end = nullptr;
    result = std::strtod(value.c_str(), &end);
    return errno != ERANGE && end == value.c_str() + value.size() &&
           std::isfinite(result) && result > 0 && result <= 100000000.0;
}

inline bool asio_probe_clsid(std::string& value) {
    if (value.size() == 36) value = '{' + value + '}';
    if (value.size() != 38 || value.front() != '{' || value.back() != '}') return false;
    for (size_t i = 1; i < 37; ++i) {
        if (i == 9 || i == 14 || i == 19 || i == 24) {
            if (value[i] != '-') return false;
        } else if (!((value[i] >= '0' && value[i] <= '9') ||
                     (value[i] >= 'a' && value[i] <= 'f') || (value[i] >= 'A' && value[i] <= 'F'))) return false;
    }
    return true;
}

inline bool parse_asio_probe_options(int argc, const char* const* argv,
                                     AsioProbeOptions& output, std::string& error) {
    AsioProbeOptions o;
    error.clear();
    auto fail = [&](const std::string& message) { error = message; return false; };
    bool actionSeen = false, rateSeen = false, dsdRateSeen = false, dsdOnly = false;
    bool streamOnly = false, commonWrite = false, channelsSeen = false;
    std::set<std::string> seen;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i] ? argv[i] : "";
        if (!seen.insert(arg).second) return fail("重复参数: " + arg);
        if (arg == "--help" || arg == "-h") {
            if (argc != 2) return fail("--help 不可与测试参数混用");
            o.help = true;
        } else if (arg == "--check" || arg == "--stream" || arg == "--dsd-create") {
            if (actionSeen) return fail("--check、--stream、--dsd-create 只能选择一个动作");
            actionSeen = true;
            o.action = arg == "--check" ? AsioProbeAction::PcmCheck :
                       arg == "--stream" ? AsioProbeAction::PcmStream : AsioProbeAction::DsdCreate;
            // A following switch is never an index. A bare token must be an exact integer.
            if (i + 1 < argc && argv[i + 1] && std::string(argv[i + 1]).rfind("--", 0) != 0) {
                unsigned long long index = 0;
                if (!asio_probe_uint(argv[++i], 2147483647, index)) return fail(arg + " 后的设备序号必须是非负整数");
                o.deviceIndex = int(index);
            }
        } else if (arg == "--clsid" || arg == "--rate" || arg == "--dsd-rate" ||
                   arg == "--ch" || arg == "--ms" || arg == "--buf" || arg == "--bytes") {
            if (i + 1 >= argc || !argv[i + 1] || std::string(argv[i + 1]).rfind("--", 0) == 0)
                return fail("参数缺少值: " + arg);
            const std::string value = argv[++i];
            unsigned long long number = 0;
            if (arg == "--clsid") {
                o.clsid = value;
                if (!asio_probe_clsid(o.clsid)) return fail("--clsid 必须是有效的 GUID");
            } else if (arg == "--rate" || arg == "--dsd-rate") {
                double parsed = 0;
                if (!asio_probe_rate(value, parsed)) return fail(arg + " 必须是有限正数且不超过 100000000 Hz");
                if (arg == "--rate") { o.rate = parsed; rateSeen = true; }
                else { o.dsdRate = parsed; dsdRateSeen = true; dsdOnly = true; }
                commonWrite = true;
            } else {
                if (!asio_probe_uint(value, 2147483647, number)) return fail(arg + " 必须是有效非负整数");
                if (arg == "--ch") {
                    if (number < 1 || number > 32) return fail("--ch 必须在 1..32 之间");
                    o.channels = int(number); channelsSeen = true; commonWrite = true;
                } else if (arg == "--ms") {
                    if (number < 50 || number > 5000) return fail("--ms 必须在 50..5000 之间；不静默截断时长");
                    o.milliseconds = int(number); streamOnly = true;
                } else {
                    if (arg == "--buf") {
                        if (number % 8 != 0) return fail("--buf 必须是 8 的整数倍（单位为 1-bit 样本）");
                        o.bufferUnits = long(number);
                    } else o.fillBytes = long(number);
                    dsdOnly = true;
                }
            }
        } else if (arg == "--dsd") o.withDsd = true;
        else if (arg == "--hwnd") { o.useHwnd = true; streamOnly = true; }
        else if (arg == "--nop") { o.nopCallback = true; streamOnly = true; }
        else if (arg == "--rate-first") { o.rateFirst = true; dsdOnly = true; }
        else if (arg == "--outready") { o.outReady = true; dsdOnly = true; }
        else if (arg == "--allch") { o.allChannels = true; dsdOnly = true; }
        else if (arg == "--spin") { o.spin = true; dsdOnly = true; }
        else if (arg == "--probe-buf") return fail("拒绝 --probe-buf：越界试写不属于安全静音探针");
        else return fail("未知参数: " + arg);
    }
    if (!o.clsid.empty() && o.deviceIndex >= 0) return fail("设备序号与 --clsid 不可同时指定");
    if (actionSeen && o.deviceIndex < 0 && o.clsid.empty()) return fail("该动作必须指定设备序号或 --clsid");
    if (o.withDsd && o.action != AsioProbeAction::Inspect) return fail("--dsd 只用于只读能力查询；开流请使用 --dsd-create");
    if (dsdOnly && o.action != AsioProbeAction::DsdCreate) return fail("DSD 参数必须显式配合 --dsd-create");
    if (streamOnly && o.action != AsioProbeAction::PcmStream && o.action != AsioProbeAction::DsdCreate)
        return fail("--ms、--hwnd、--nop 只用于 --stream 或 --dsd-create");
    if (commonWrite && o.action == AsioProbeAction::Inspect) return fail("采样率/声道参数必须配合明确测试动作");
    if (o.allChannels && channelsSeen) return fail("--allch 与 --ch 不可同时指定");
    if (o.action == AsioProbeAction::DsdCreate && rateSeen) {
        if (dsdRateSeen && o.rate != o.dsdRate) return fail("--rate 与 --dsd-rate 的 DSD 采样率相互矛盾");
        o.dsdRate = o.rate;
    }
    output = o;
    return true;
}

inline const char* asio_probe_usage() {
    return "用法:\n"
           "  asio_probe [--dsd] [--clsid GUID]  只读能力查询，不创建缓冲/不开流\n"
           "  asio_probe --check <序号|--clsid GUID> [--rate Hz] [--ch 1..32]\n"
           "  asio_probe --stream <序号|--clsid GUID> [--rate Hz] [--ch 1..32] [--ms 50..5000]\n"
           "  asio_probe --dsd-create <序号|--clsid GUID> [--dsd-rate Hz|--rate Hz] [--ch 1..32] [--ms 50..5000]\n"
           "  只有 --dsd-create 会切换 DSD；--check/--stream 始终测试 PCM。\n"
           "  --stream/--dsd-create 可用 --hwnd --nop；DSD 另有 --buf --bytes --rate-first --outready --allch --spin。\n";
}

} // namespace rta::audio
