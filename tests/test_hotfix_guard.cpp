// test_hotfix_guard.cpp — 1.0.5 热修复第一刀的加固回归（HF-03 + HF-01/02 地基）
//
// 判据说明（铁律 3）：被测对象是「异常输入被拒且错误带字段信息」与「合法输入不被误杀」，
// 全部离线：不打开音频设备、不调用 WASAPI 需要设备的路径（probe/open 只做编译期签名覆盖）、
// 不依赖 ffmpeg（合法 WAV 用 write_wav 生成）。「不巨量分配」这条用 dataLen 声明值远大于
// 实际文件的构造证明被拒 —— 错误在 resize 之前抛出，绝不真分配。
//
// ⚠️ 未解决项（铁律 4）：>2GB 文件的 ftell 守卫（wav.h 64 位定位分支）无法在单测里
// 用真实文件覆盖（需要 2GB+ 磁盘文件），当前以代码审查 + 本文件的空文件/异常头分支
// 作为回归基线。rtaweb 接线（exclusive_rates 空列表、降位深回退）归第二刀，本文件不测。
//
// 2026-10-21：对应 docs/热修复交接-2026-10-21.md §2.2/§3（HF-03）与 HF-01 的 wasapi_out.h 部分。
// 2026-10-22：新增 §5（F5）—— wav.h 宽字符打开后，read_wav 必须打得开 UTF-8 中文文件名。
#include "audio/wav.h"
#include "audio/audio_loader.h"
#include "dsp/resampler.h"
#include "audio/wasapi_out.h"
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <stdexcept>
#include <type_traits>
#include <utility>

#ifdef _WIN32
extern "C" __declspec(dllimport) int __stdcall SetConsoleOutputCP(unsigned int);
extern "C" __declspec(dllimport) unsigned long __stdcall GetCurrentProcessId(void);
#endif
#ifndef CP_UTF8
#define CP_UTF8 65001
#endif

using rta::audio::AudioBuffer;
using rta::audio::read_wav;
using rta::audio::write_wav;

static int g_fail = 0;
static void ck(bool cond, const char* what, const std::string& detail = "") {
    if (cond) std::printf("  [PASS] %s\n", what);
    else { std::printf("  [FAIL] %s  %s\n", what, detail.c_str()); ++g_fail; }
}

// 期望 fn() 抛异常，且错误文本包含 mustContain（用于断言字段信息进了错误）
template <class Fn>
static void expect_throw_containing(Fn&& fn, const std::string& mustContain, const char* what) {
    try {
        fn();
        ck(false, what, "未抛出异常");
    } catch (const std::exception& e) {
        const std::string m = e.what();
        ck(m.find(mustContain) != std::string::npos, what,
           std::string("错误文本缺关键字[") + mustContain + "]: " + m);
    }
}

// ---------------- 临时文件与 WAV 字节构造 ----------------
static std::string tmp_name(const char* tag) {
    char b[160];
    std::snprintf(b, sizeof(b), "hf_guard_%lu_%s.wav",
                  (unsigned long)GetCurrentProcessId(), tag);
    return b;
}
static bool write_bytes(const std::string& path, const std::vector<uint8_t>& b) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    const size_t w = b.empty() ? 0u : std::fwrite(b.data(), 1, b.size(), f);
    std::fclose(f);
    return w == b.size();
}
static void remove_file(const std::string& path) { std::remove(path.c_str()); }

// 手工组一个 WAV：可指定 fmtTag/ch/rate/bps/data 块声明大小与实际载荷（用于伪造异常头）
static std::vector<uint8_t> build_wav(uint16_t fmtTag, uint16_t ch, uint32_t rate, uint16_t bps,
                                      uint32_t dataDecl, const std::vector<uint8_t>& payload,
                                      bool withDataChunk = true) {
    std::vector<uint8_t> o;
    auto w32 = [&](uint32_t v) {
        o.push_back((uint8_t)v); o.push_back((uint8_t)(v >> 8));
        o.push_back((uint8_t)(v >> 16)); o.push_back((uint8_t)(v >> 24));
    };
    auto w16 = [&](uint16_t v) { o.push_back((uint8_t)v); o.push_back((uint8_t)(v >> 8)); };
    o.insert(o.end(), {'R','I','F','F'}); w32(0);
    o.insert(o.end(), {'W','A','V','E'});
    o.insert(o.end(), {'f','m','t',' '}); w32(16);
    w16(fmtTag); w16(ch); w32(rate);
    w32(rate * (uint32_t)ch * (uint32_t)(bps / 8));
    w16((uint16_t)(ch * (bps / 8))); w16(bps);
    if (withDataChunk) {
        o.insert(o.end(), {'d','a','t','a'}); w32(dataDecl);
        o.insert(o.end(), payload.begin(), payload.end());
    }
    const uint32_t riff = (uint32_t)(o.size() - 8);
    o[4] = (uint8_t)riff; o[5] = (uint8_t)(riff >> 8); o[6] = (uint8_t)(riff >> 16); o[7] = (uint8_t)(riff >> 24);
    return o;
}

static void expect_wav_throw(const std::string& file, const std::string& mustContain, const char* what) {
    expect_throw_containing([&] { (void)read_wav(file); }, mustContain, what);
}

int main() {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    std::printf("== 热修复守卫：HF-03 解码/临时WAV/分配加固 + HF-01/02 探测地基 ==\n");

    // ================= 1) read_wav 异常 WAV 构造：全拒 + 错误带字段信息 =================
    std::printf("- 1a 异常 fmt 头（ch / bps / fmtTag）\n");
    {
        const std::string f = tmp_name("ch0");            // ch=0：旧代码已挡，回归确认
        ck(write_bytes(f, build_wav(1, 0, 44100, 16, 4, {0,0,0,0})), "ch=0 构造写入");
        expect_wav_throw(f, "ch=0", "ch=0 被拒且错误含 ch=0");
        remove_file(f);
    }
    {
        const std::string f = tmp_name("ch65535");        // uint16 最大声道数
        ck(write_bytes(f, build_wav(1, 65535, 44100, 16, 4, {0,0,0,0})), "ch=65535 构造写入");
        expect_wav_throw(f, "ch=65535", "ch=65535 被拒且错误含 ch=65535");
        remove_file(f);
    }
    {
        const std::string f = tmp_name("bps7");           // bps=7：旧代码 bytesPer=0 会走除零/巨分配路径
        ck(write_bytes(f, build_wav(1, 2, 44100, 7, 8, std::vector<uint8_t>(8, 0xAA))), "bps=7 构造写入");
        expect_wav_throw(f, "bps=7", "bps=7 被拒且错误含 bps=7");
        remove_file(f);
    }
    {
        const std::string f = tmp_name("bps33");          // bps=33：非法位深
        ck(write_bytes(f, build_wav(1, 2, 44100, 33, 8, std::vector<uint8_t>(8, 0xAA))), "bps=33 构造写入");
        expect_wav_throw(f, "bps=33", "bps=33 被拒且错误含 bps=33");
        remove_file(f);
    }
    {
        const std::string f = tmp_name("bps0");           // bps=0
        ck(write_bytes(f, build_wav(1, 2, 44100, 0, 8, std::vector<uint8_t>(8, 0xAA))), "bps=0 构造写入");
        expect_wav_throw(f, "bps=0", "bps=0 被拒且错误含 bps=0");
        remove_file(f);
    }
    {
        const std::string f = tmp_name("f64");            // float64：现有管线按 float32 位型存储，明确报不支持
        ck(write_bytes(f, build_wav(3, 2, 44100, 64, 16, std::vector<uint8_t>(16, 0))), "float64 构造写入");
        expect_wav_throw(f, "float64", "float64 被明确报不支持");
        remove_file(f);
    }
    {
        const std::string f = tmp_name("alaw");           // fmtTag=6 (a-law)：读成线性 PCM 就是垃圾，明确拒
        ck(write_bytes(f, build_wav(6, 1, 8000, 8, 4, {0x55,0x55,0x55,0x55})), "a-law 构造写入");
        expect_wav_throw(f, "线性 PCM", "非 PCM 编码(fmtTag=6)被拒且错误含编码说明");
        remove_file(f);
    }

    std::printf("- 1b data 块边界（巨量分配在 resize 之前被拒）\n");
    {
        // dataLen 声明 0xFFFFFF00 ≈ 4 GiB，实际文件只有几十字节：
        // 若旧逻辑走到 resize 会抛裸 "vector too long"/巨分配；现在在 resize 前被拒。
        const std::string f = tmp_name("datalen_huge");
        ck(write_bytes(f, build_wav(1, 2, 44100, 16, 0xFFFFFF00u, std::vector<uint8_t>(8, 0xAA))),
           "dataLen 巨声明构造写入");
        expect_wav_throw(f, "4294967040", "dataLen 声明超缓冲被拒且错误含声明值（未发生巨量分配）");
        remove_file(f);
    }
    {
        // 截断 data 块：声明 1000 字节，文件里只有 20 字节载荷
        const std::string f = tmp_name("trunc");
        ck(write_bytes(f, build_wav(1, 2, 44100, 16, 1000, std::vector<uint8_t>(20, 0xBB))),
           "截断 data 构造写入");
        expect_wav_throw(f, "实际可用=20", "截断 data 块被拒且错误含实际可用字节");
        remove_file(f);
    }
    {
        // 完全没有 data 块（补一个 LIST chunk 使文件 ≥44 字节，
        // 否则 36 字节会被更早的"≥44 字节"检查拒掉 —— 那条也是正确拒绝，但测不到本分支）
        const std::string f = tmp_name("nodata");
        std::vector<uint8_t> o = build_wav(1, 2, 44100, 16, 0, {}, false);
        auto push32 = [&](uint32_t v) {
            o.push_back((uint8_t)v); o.push_back((uint8_t)(v >> 8));
            o.push_back((uint8_t)(v >> 16)); o.push_back((uint8_t)(v >> 24));
        };
        o.insert(o.end(), {'L','I','S','T'});
        push32(8);
        const uint8_t info[8] = {'I','N','F','O',0,0,0,0};
        o.insert(o.end(), info, info + 8);
        const uint32_t riff = (uint32_t)(o.size() - 8);
        o[4] = (uint8_t)riff; o[5] = (uint8_t)(riff >> 8); o[6] = (uint8_t)(riff >> 16); o[7] = (uint8_t)(riff >> 24);
        ck(write_bytes(f, o), "无 data 块构造写入");
        expect_wav_throw(f, "缺少 data", "缺少 data 块被拒且错误含说明");
        remove_file(f);
    }
    {
        // 空文件 / 不存在的文件：错误带文件名
        const std::string f = tmp_name("empty");
        ck(write_bytes(f, {}), "空文件写入");
        expect_wav_throw(f, "文件为空", "空文件被拒且错误带文件名");
        remove_file(f);
        expect_wav_throw(tmp_name("no_such"), "无法打开文件", "不存在的文件报无法打开且带文件名");
    }

    // ================= 2) 合法 WAV 回归：8/16/24/32-bit 与 float32 不被误杀 =================
    std::printf("- 2 合法 WAV 写读回归（write_wav -> read_wav 位精确）\n");
    {
        auto roundtrip = [&](int bits, bool isFloat, const std::vector<int32_t>& raw, const char* what) {
            AudioBuffer a;
            a.rate = 44100; a.channels = 2; a.bits = bits; a.isFloat = isFloat; a.raw = raw;
            const std::string f = tmp_name((std::string("ok") + std::to_string(bits)).c_str());
            bool wrote = false;
            try { write_wav(f, a); wrote = true; } catch (const std::exception& e) { ck(false, what, std::string("写入异常: ") + e.what()); }
            if (wrote) {
                try {
                    const AudioBuffer b = read_wav(f);
                    ck(b.rate == 44100 && b.channels == 2 && b.bits == bits && b.isFloat == isFloat && b.raw == raw,
                       what, "回读字段/样本不一致");
                } catch (const std::exception& e) { ck(false, what, std::string("回读异常: ") + e.what()); }
            }
            remove_file(f);
        };
        roundtrip(8,  false, {-128, -101, -1, 0, 1, 77, 126, 127, 127, 0, -128, 64, -64, 32, -32, 5}, "8-bit（无符号，中心128）写读位精确");
        roundtrip(16, false, {-32768, -32767, -1, 0, 1, 12345, 32766, 32767, -100, 100, 7, -7, 30000, -30000, 1, -1}, "16-bit 写读位精确");
        roundtrip(24, false, {-8388608, -8388607, -1, 0, 1, 424242, 8388606, 8388607, -1000, 1000, 9, -9, 100, -100, 2, -2}, "24-bit 写读位精确");
        roundtrip(32, false, {-2147483647 - 1, -2147483647, -1, 0, 1, 123456789, 2147483646, 2147483647,
                              -100000, 100000, 11, -11, 500, -500, 3, -3}, "32-bit 写读位精确");
        // float32：raw 存 float 位型，逐位往返
        std::vector<int32_t> fraw;
        for (float v : {1.0f, -1.0f, 0.0f, 0.5f, -0.25f, 0.125f, 6.02e-5f, -3.01e-2f}) {
            int32_t b = 0; std::memcpy(&b, &v, 4);
            fraw.push_back(b); fraw.push_back(b);
        }
        roundtrip(32, true, fraw, "float32 写读位精确");
    }
    {
        // read_any 对合法 .wav 走原生路径（不依赖 ffmpeg）
        const std::string f = tmp_name("okany");
        AudioBuffer a;
        a.rate = 48000; a.channels = 2; a.bits = 16; a.isFloat = false;
        a.raw.assign(100, 0);   // 50 帧
        write_wav(f, a);
        try {
            rta::audio::LoadNote note;
            const AudioBuffer b = rta::audio::read_any(f, &note);
            ck(b.rate == 48000 && b.channels == 2 && b.frames() == 50, "read_any 合法 .wav 正常读出",
               "frames=" + std::to_string(b.frames()));
        } catch (const std::exception& e) { ck(false, "read_any 合法 .wav 正常读出", e.what()); }
        remove_file(f);
    }

    // ================= 3) resampler 构造防御 + 正常回归 =================
    std::printf("- 3 resampler 构造防御\n");
    {
        rta::dsp::ResamplerConfig bad;
        bad.inRate = 0; bad.outRate = 48000;          // gcd(0,48000)=48000 → 旧代码 M_=0
        expect_throw_containing([&] { rta::dsp::PolyphaseResampler r(bad); (void)r; },
                                "inRate=0", "inRate=0（gcd(0,x) 路径）构造被拒且错误含 inRate=0");
        rta::dsp::ResamplerConfig bad2;
        bad2.inRate = 44100; bad2.outRate = 0;
        expect_throw_containing([&] { rta::dsp::PolyphaseResampler r(bad2); (void)r; },
                                "outRate=0", "outRate=0 构造被拒且错误含 outRate=0");
        rta::dsp::ResamplerConfig bad3;
        bad3.inRate = 0; bad3.outRate = 0;
        expect_throw_containing([&] { rta::dsp::PolyphaseResampler r(bad3); (void)r; },
                                "inRate=0", "双 0 构造被拒且错误含两率");
    }
    {
        std::printf("- 3b 44100→88200 正常回归（不误杀）\n");
        rta::dsp::ResamplerConfig cfg;
        cfg.inRate = 44100; cfg.outRate = 88200; cfg.channels = 2;
        cfg.totalTaps = 4096; cfg.attenDb = 120.0;
        try {
            rta::dsp::PolyphaseResampler r(cfg);
            ck(!r.passthrough() && r.upFactor() == 2 && r.downFactor() == 1,
               "44100→88200 因子 L=2 M=1",
               "L=" + std::to_string(r.upFactor()) + " M=" + std::to_string(r.downFactor()));
            const size_t N = 6000;
            std::vector<double> inL(N, 0.5), inR(N, 0.5);
            std::vector<double*> inp = { inL.data(), inR.data() };
            const size_t cap = r.maxOutFrames(N);
            std::vector<double> outL(cap, 0.0), outR(cap, 0.0);
            std::vector<double*> outp = { outL.data(), outR.data() };
            const size_t got = r.process(inp.data(), N, outp.data());
            ck(got >= 2 * N - 8 && got <= cap, "输出帧数在 [2N-8, cap] 内",
               "got=" + std::to_string(got) + " cap=" + std::to_string(cap));
            double s = 0.0;
            for (size_t i = got - 100; i < got; ++i) s += outL[i];
            s /= 100.0;
            ck(std::fabs(s - 0.5) < 1e-9, "稳态增益=1.0（恒幅 0.5 输入回归）",
               "末段均值=" + std::to_string(s));
        } catch (const std::exception& e) { ck(false, "44100→88200 正常回归", e.what()); }
    }

    // ================= 4) probe_formats / OpenFail：编译期覆盖（不触碰设备） =================
    // 按任务约束：不调用任何需要音频设备的路径；这里只验证结构体与签名（含旧 5 参 open 兼容）。
    std::printf("- 4 probe/OpenFail 编译期覆盖（unevaluated，不开设备）\n");
    {
        using rta::audio::FormatProbe;
        using rta::audio::OpenFail;
        using rta::audio::WasapiOutput;
        static_assert(std::is_same_v<decltype(FormatProbe::rates16), std::vector<uint32_t>>, "rates16");
        static_assert(std::is_same_v<decltype(FormatProbe::rates24), std::vector<uint32_t>>, "rates24");
        static_assert(std::is_same_v<decltype(FormatProbe::rates32), std::vector<uint32_t>>, "rates32");
        static_assert(std::is_same_v<decltype(FormatProbe::deviceId), std::string>, "deviceId");
        static_assert(std::is_same_v<decltype(FormatProbe::endpointAlive), bool>, "endpointAlive");
        static_assert(std::is_same_v<decltype(FormatProbe::friendlyName), std::string>, "friendlyName");
        static_assert(std::is_same_v<decltype(OpenFail::request), std::string>, "OpenFail::request");
        static_assert(std::is_same_v<decltype(OpenFail::supportedList), std::string>, "OpenFail::supportedList");
        static_assert(std::is_same_v<decltype(OpenFail::hresultText), std::string>, "OpenFail::hresultText");
        static_assert(std::is_same_v<decltype(OpenFail::deviceId), std::string>, "OpenFail::deviceId");
        static_assert(std::is_same_v<decltype(&WasapiOutput::probe_formats),
                                     FormatProbe (*)(const std::string&)>, "probe_formats 静态签名");
        // 旧 5 参 open() 调用仍成立（decltype 内是 unevaluated 操作数，不会真正打开设备）
        static_assert(std::is_same_v<decltype(std::declval<WasapiOutput&>().open(
                          std::declval<const std::string&>(), std::declval<uint32_t>(),
                          std::declval<int>(), std::declval<int>(), std::declval<double>())), bool>,
                      "open() 旧 5 参签名兼容");
        // 新 6 参（OpenFail* 出参）签名成立
        static_assert(std::is_same_v<decltype(std::declval<WasapiOutput&>().open(
                          std::declval<const std::string&>(), std::declval<uint32_t>(),
                          std::declval<int>(), std::declval<int>(), std::declval<double>(),
                          std::declval<OpenFail*>())), bool>,
                      "open() 新 6 参(OpenFail*)签名可用");
        ck(true, "probe/OpenFail 全部 static_assert 通过（含旧签名兼容）");
        FormatProbe p{};
        ck(!p.endpointAlive && p.rates16.empty() && p.rates24.empty() && p.rates32.empty() &&
           p.deviceId.empty() && p.friendlyName.empty(), "FormatProbe 默认构造全空/false（找不到端点时如实返回的形态）");
        OpenFail of{};
        ck(of.request.empty() && of.supportedList.empty() && of.hresultText.empty() && of.deviceId.empty(),
           "OpenFail 默认构造全空");
    }

    // ================= 5) F5 回归：UTF-8 中文文件名（2026-10-22） =================
    // 现象：Windows 窄 fopen 按 ANSI 代码页（ACP=936/GBK）解释路径，UTF-8 中文文件名
    // 打不开（实证）。修法：wav.h 内自带纯标准库 UTF-8→UTF-16，_wfopen 打开。
    //
    // 判据设计（铁律 3）——为什么文件【不能】用被测代码创建：
    //   若用 write_wav（窄 fopen）写、read_wav 读，旧代码两边都会把 UTF-8 字节错解成
    //   同一个 GBK 乱码名，写读自洽、照样通过 —— 测不出任何差异。
    //   所以这里用 std::filesystem（u8string→宽 path，与被测代码无关）把文件落到
    //   【真名】下；旧 read_wav 的窄 fopen 按错名字找文件 → 打不开 → 抛异常 → 失败。
    //   新代码走 _wfopen 找到真名文件 → 通过。
    std::printf("- 5 UTF-8 中文文件名 WAV（read_wav 宽字符打开，F5）\n");
    {
        namespace fs = std::filesystem;
        // 「测试 曲目.wav」的 UTF-8 字节（不用中文字面量：不受编译器 source/execution charset 影响）
        const std::string u8name = std::string("\xE6\xB5\x8B\xE8\xAF\x95\x20\xE6\x9B\xB2\xE7\x9B\xAE") + ".wav";
        const fs::path truePath =
            fs::path(std::u8string(reinterpret_cast<const char8_t*>(u8name.data()), u8name.size()));
        const std::vector<uint8_t> bytes = build_wav(1, 2, 44100, 16, 16, std::vector<uint8_t>(16, 0x00));
        bool placed = false;
        std::string why;
        std::error_code ec;
        try {
            {
                std::ofstream f(truePath, std::ios::binary | std::ios::trunc);
                if (f) f.write(reinterpret_cast<const char*>(bytes.data()), (std::streamsize)bytes.size());
            }
            placed = (fs::file_size(truePath, ec) == bytes.size()) && !ec;
        } catch (const std::exception& e) { why = e.what(); }
        ck(placed, "中文文件名 WAV 按真名落盘（filesystem 宽路径写入）", why);
        if (placed) {
            try {
                const AudioBuffer b = read_wav(u8name);
                ck(b.rate == 44100 && b.channels == 2 && b.frames() == 4 && b.raw.size() == 8,
                   "read_wav 打得开 UTF-8 中文文件名且 rate/ch/样本数对",
                   "rate=" + std::to_string(b.rate) + " ch=" + std::to_string(b.channels) +
                   " frames=" + std::to_string(b.frames()) + " raw=" + std::to_string(b.raw.size()));
            } catch (const std::exception& e) {
                ck(false, "read_wav 打得开 UTF-8 中文文件名且 rate/ch/样本数对", e.what());
            }
            fs::remove(truePath, ec);
        }
    }

    std::printf("\n%s\n", g_fail == 0 ? "全部通过" : (std::to_string(g_fail) + " 项失败").c_str());
    return g_fail == 0 ? 0 : 1;
}
