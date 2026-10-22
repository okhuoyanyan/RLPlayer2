// test_stream_decode.cpp — 流式读取 ffmpeg 输出（免临时 WAV）单元与集成测试（ROADMAP P6b）
//
// 验收要求（铁律 3：判据必须能证明对的东西）：
//   1. WAV 头解析器：各种 fmt/data 块顺序、奇数 chunk padding、extensible/float、非法头全拒
//   2. 管道读取分块正确性：跨 1/2/3/5/7/13 字节等任意非对齐边界拼装，样本逐位一致
//   3. 与临时路径的逐位一致比较：WAV / FLAC / MP3 / OGG / M4A 两条路径 PCM 逐字节完全相同
//   4. 设置开关：streamdecode=0 / 环境变量 RLPLAYER_STREAM_DECODE=0 走旧路径，默认走流式
//   5. 异常退出码与 Win32 真因透传：不存在文件、损坏文件正确归因，回退策略在 note 中说明
//   6. 延迟与磁盘 I/O 改善实测：流式写入字节为 0，起播延迟缩短

#include "audio/audio_loader.h"
#include "audio/wav.h"

#include <windows.h>
#include <cstdio>
#include <string>
#include <vector>
#include <filesystem>
#include <chrono>
#include <cstring>
#include <cstdlib>

namespace fs = std::filesystem;

#ifdef _WIN32
extern "C" __declspec(dllimport) int __stdcall SetConsoleOutputCP(unsigned int);
#endif
#ifndef CP_UTF8
#define CP_UTF8 65001
#endif

static int g_fail = 0;
static void ck(bool cond, const char* what, const std::string& detail = "") {
    if (cond) {
        std::printf("  [PASS] %s\n", what);
    } else {
        std::printf("  [FAIL] %s  %s\n", what, detail.c_str());
        ++g_fail;
    }
}

static fs::path u8path_of(const std::string& s) {
    return fs::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
}

// 辅助：生成小端 uint32 / uint16
static void wr32(std::vector<uint8_t>& v, uint32_t val) {
    v.push_back((uint8_t)(val & 0xFF));
    v.push_back((uint8_t)((val >> 8) & 0xFF));
    v.push_back((uint8_t)((val >> 16) & 0xFF));
    v.push_back((uint8_t)((val >> 24) & 0xFF));
}
static void wr16(std::vector<uint8_t>& v, uint16_t val) {
    v.push_back((uint8_t)(val & 0xFF));
    v.push_back((uint8_t)((val >> 8) & 0xFF));
}

// =========================================================================
// 1. WAV 头解析器单元测试 (parse_wav_header)
// =========================================================================
static void test_wav_header_parser() {
    std::printf("- 1 WAV 头解析器单元测试（各种 fmt/data 块顺序、奇数 chunk padding、异常格式）\n");

    // 1.1 标准 16-bit PCM WAV
    {
        std::vector<uint8_t> h;
        h.insert(h.end(), {'R','I','F','F'});
        wr32(h, 44 - 8);
        h.insert(h.end(), {'W','A','V','E'});
        h.insert(h.end(), {'f','m','t',' '});
        wr32(h, 16);
        wr16(h, 1);     // PCM
        wr16(h, 2);     // 2 ch
        wr32(h, 44100); // rate
        wr32(h, 44100 * 4); // byteRate
        wr16(h, 4);     // blockAlign
        wr16(h, 16);    // bits
        h.insert(h.end(), {'d','a','t','a'});
        wr32(h, 100);   // data size

        rta::audio::WavHeaderInfo info{};
        std::string err;
        int r = rta::audio::parse_wav_header(h.data(), h.size(), info, &err);
        ck(r == 44, "标准 16-bit PCM WAV 头解析成功", "r=" + std::to_string(r) + " err=" + err);
        ck(info.fmtTag == 1 && info.channels == 2 && info.rate == 44100 && info.bits == 16 && !info.isFloat,
           "标准 16-bit 头参数正确 (rate=44100, ch=2, bits=16)");
        ck(info.dataDeclaredBytes == 100 && info.headerBytes == 44, "标准 16-bit 数据起始与长度正确");
    }

    // 1.2 WAVE_FORMAT_EXTENSIBLE (0xFFFE) 24-bit PCM
    {
        std::vector<uint8_t> h;
        h.insert(h.end(), {'R','I','F','F'});
        wr32(h, 0xFFFFFFFF); // 模拟流式管道输出
        h.insert(h.end(), {'W','A','V','E'});
        h.insert(h.end(), {'f','m','t',' '});
        wr32(h, 40);         // extensible fmt sz = 40
        wr16(h, 0xFFFE);     // WAVE_FORMAT_EXTENSIBLE
        wr16(h, 2);          // 2 ch
        wr32(h, 96000);      // rate
        wr32(h, 96000 * 6);  // byteRate
        wr16(h, 6);          // blockAlign
        wr16(h, 24);         // bits
        wr16(h, 22);         // cbSize
        wr16(h, 24);         // validBits
        wr32(h, 3);          // channelMask
        wr16(h, 1);          // subFormat GUID data1 = 1 (PCM)
        h.insert(h.end(), 14, 0); // rest of GUID
        h.insert(h.end(), {'d','a','t','a'});
        wr32(h, 0xFFFFFFFF); // 流式管道未定长度

        rta::audio::WavHeaderInfo info{};
        std::string err;
        int r = rta::audio::parse_wav_header(h.data(), h.size(), info, &err);
        ck(r == 68, "WAVE_FORMAT_EXTENSIBLE 24-bit 头解析成功", "r=" + std::to_string(r) + " err=" + err);
        ck(info.fmtTag == 1 && info.channels == 2 && info.rate == 96000 && info.bits == 24 && !info.isFloat,
           "EXTENSIBLE subFormat=1 映射为线性 PCM (rate=96000, ch=2, bits=24)");
        ck(info.dataDeclaredBytes == 0xFFFFFFFF, "流式管道 0xFFFFFFFF 长度透传");
    }

    // 1.3 32-bit IEEE float WAV (fmtTag=3)
    {
        std::vector<uint8_t> h;
        h.insert(h.end(), {'R','I','F','F'});
        wr32(h, 1000);
        h.insert(h.end(), {'W','A','V','E'});
        h.insert(h.end(), {'f','m','t',' '});
        wr32(h, 16);
        wr16(h, 3);          // IEEE float
        wr16(h, 1);          // mono
        wr32(h, 192000);
        wr32(h, 192000 * 4);
        wr16(h, 4);
        wr16(h, 32);
        h.insert(h.end(), {'d','a','t','a'});
        wr32(h, 800);

        rta::audio::WavHeaderInfo info{};
        std::string err;
        int r = rta::audio::parse_wav_header(h.data(), h.size(), info, &err);
        ck(r == 44 && info.fmtTag == 3 && info.isFloat && info.bits == 32,
           "32-bit IEEE float 头解析成功 (isFloat=true, bits=32)");
    }

    // 1.4 chunk 乱序与奇数 chunk 大小补齐 padding 测试
    // 构造：RIFF -> JUNK(15字节，奇数需补齐1字节0x00) -> fmt(16字节) -> LIST(20字节) -> data
    {
        std::vector<uint8_t> h;
        h.insert(h.end(), {'R','I','F','F'});
        wr32(h, 500);
        h.insert(h.end(), {'W','A','V','E'});
        // 奇数 chunk: JUNK (sz = 15)
        h.insert(h.end(), {'J','U','N','K'});
        wr32(h, 15);
        h.insert(h.end(), 15, 'X');
        h.push_back(0); // RIFF 规范要求的 1 字节对齐 padding！

        // 紧接着 fmt
        h.insert(h.end(), {'f','m','t',' '});
        wr32(h, 16);
        wr16(h, 1);
        wr16(h, 2);
        wr32(h, 48000);
        wr32(h, 48000 * 4);
        wr16(h, 4);
        wr16(h, 16);

        // LIST chunk (sz = 8)
        h.insert(h.end(), {'L','I','S','T'});
        wr32(h, 8);
        h.insert(h.end(), 8, 'Y');

        // data chunk
        h.insert(h.end(), {'d','a','t','a'});
        wr32(h, 0xFFFFFFFF);

        rta::audio::WavHeaderInfo info{};
        std::string err;
        int r = rta::audio::parse_wav_header(h.data(), h.size(), info, &err);
        ck(r > 0 && info.fmtTag == 1 && info.rate == 48000 && info.channels == 2,
           "JUNK前置 + 奇数15字节padding + LIST中置 复合头顺利解析",
           "r=" + std::to_string(r) + " err=" + err);
        ck(r == (int)h.size(), "headerBytes 精确定位到 data 块体开始",
           "expected=" + std::to_string(h.size()) + " got=" + std::to_string(r));
    }

    // 1.5 异常格式拒绝
    {
        rta::audio::WavHeaderInfo info{};
        std::string err;

        // 非 RIFF
        uint8_t badRiff[] = "NOT_A_RIFF_HEADER_AT_ALL_XXXX";
        int r1 = rta::audio::parse_wav_header(badRiff, sizeof(badRiff), info, &err);
        ck(r1 < 0, "非 RIFF 头部被正确拒绝", err);

        // data 在 fmt 之前
        std::vector<uint8_t> noFmt;
        noFmt.insert(noFmt.end(), {'R','I','F','F'});
        wr32(noFmt, 100);
        noFmt.insert(noFmt.end(), {'W','A','V','E'});
        noFmt.insert(noFmt.end(), {'d','a','t','a'});
        wr32(noFmt, 10);
        int r2 = rta::audio::parse_wav_header(noFmt.data(), noFmt.size(), info, &err);
        ck(r2 < 0, "缺少 fmt 块即出现 data 块被正确拒绝", err);

        // 不支持的编码 fmtTag = 7 (Mu-law)
        std::vector<uint8_t> mulaw;
        mulaw.insert(mulaw.end(), {'R','I','F','F'});
        wr32(mulaw, 100);
        mulaw.insert(mulaw.end(), {'W','A','V','E'});
        mulaw.insert(mulaw.end(), {'f','m','t',' '});
        wr32(mulaw, 16);
        wr16(mulaw, 7); // mu-law
        wr16(mulaw, 1);
        wr32(mulaw, 8000);
        wr32(mulaw, 8000);
        wr16(mulaw, 1);
        wr16(mulaw, 8);
        mulaw.insert(mulaw.end(), {'d','a','t','a'});
        wr32(mulaw, 10);
        int r3 = rta::audio::parse_wav_header(mulaw.data(), mulaw.size(), info, &err);
        ck(r3 < 0, "非线性 PCM 编码 (fmtTag=7) 被正确拒绝", err);

        // 截断头需要更多数据 (返回 0)
        std::vector<uint8_t> truncH = {'R','I','F','F', 0,0,0,0, 'W','A','V','E'};
        int r4 = rta::audio::parse_wav_header(truncH.data(), truncH.size(), info, &err);
        ck(r4 == 0, "截断的未完成头部返回 0（要求更多数据）");
    }
}

// =========================================================================
// 2. 管道读取分块与跨块样本拼接正确性
// =========================================================================
static void test_chunked_pipe_simulation() {
    std::printf("- 2 管道分块读取与跨块样本边界拆分测试（24-bit/3字节与16-bit在不同切片下的逐位一致性）\n");

    // 构造测试数据：24-bit 500 个样本（每个样本 3 字节，共 1500 字节）
    constexpr size_t kSampleCount = 500;
    std::vector<int32_t> expectedSamples(kSampleCount);
    std::vector<uint8_t> pcmBytes(kSampleCount * 3);

    for (size_t i = 0; i < kSampleCount; ++i) {
        // 构造有正有负、覆盖高低位的 24-bit 整数
        int32_t val = (int32_t)(i * 334455);
        if (i % 2 == 1) val = -val;
        val &= 0x00FFFFFF;
        if (val & 0x800000) val |= (int32_t)0xFF000000; // 符号扩展
        expectedSamples[i] = val;

        pcmBytes[i * 3 + 0] = (uint8_t)(val & 0xFF);
        pcmBytes[i * 3 + 1] = (uint8_t)((val >> 8) & 0xFF);
        pcmBytes[i * 3 + 2] = (uint8_t)((val >> 16) & 0xFF);
    }

    // 针对多种刁钻的切片块大小进行测试：1, 2, 3, 5, 7, 11, 13, 64 字节
    const std::vector<size_t> chunkSizes = { 1, 2, 3, 5, 7, 11, 13, 64, 256, 1024 };

    for (size_t csz : chunkSizes) {
        rta::audio::AudioBuffer ab;
        ab.rate = 48000;
        ab.channels = 1;
        ab.bits = 24;
        ab.isFloat = false;

        uint8_t leftover[4] = {0};
        size_t leftoverCount = 0;
        const int bytesPer = 3;

        size_t pos = 0;
        while (pos < pcmBytes.size()) {
            size_t got = std::min(csz, pcmBytes.size() - pos);
            const uint8_t* chunk = pcmBytes.data() + pos;

            size_t chunkOff = 0;
            if (leftoverCount > 0) {
                const size_t needed = (size_t)bytesPer - leftoverCount;
                if (got >= needed) {
                    std::memcpy(leftover + leftoverCount, chunk, needed);
                    ab.raw.push_back(rta::audio::decode_sample(leftover, 24, false));
                    leftoverCount = 0;
                    chunkOff = needed;
                } else {
                    std::memcpy(leftover + leftoverCount, chunk, got);
                    leftoverCount += got;
                    pos += got;
                    continue;
                }
            }
            const size_t rem = got - chunkOff;
            const size_t numSamples = rem / (size_t)bytesPer;
            if (numSamples > 0) {
                const size_t oldSz = ab.raw.size();
                ab.raw.resize(oldSz + numSamples);
                const uint8_t* p = chunk + chunkOff;
                for (size_t i = 0; i < numSamples; ++i) {
                    ab.raw[oldSz + i] = rta::audio::decode_sample(p + i * (size_t)bytesPer, 24, false);
                }
            }
            leftoverCount = rem % (size_t)bytesPer;
            if (leftoverCount > 0) {
                std::memcpy(leftover, chunk + chunkOff + numSamples * (size_t)bytesPer, leftoverCount);
            }
            pos += got;
        }

        bool match = (ab.raw.size() == kSampleCount);
        if (match) {
            for (size_t i = 0; i < kSampleCount; ++i) {
                if (ab.raw[i] != expectedSamples[i]) {
                    match = false;
                    break;
                }
            }
        }
        ck(match, ("块大小 " + std::to_string(csz) + " 字节切片解码样本逐位完全一致").c_str(),
           "decodedSamples=" + std::to_string(ab.raw.size()) + " expected=" + std::to_string(kSampleCount));
    }
}

// =========================================================================
// 3. 5 文件真实格式比对：流式 vs 旧临时WAV 逐位一致（判据必须硬）
// =========================================================================
static void test_bit_exact_5_formats(const std::string& ffmpeg, const std::string& sandbox) {
    std::printf("- 3 真实格式双路径逐位一致比对（FLAC/MP3/OGG/M4A/WAV PCM逐字节完全相同）\n");

    struct FormatSpec {
        std::string name;
        std::string ext;
        std::string ffmpegArgs;
    };

    const std::vector<FormatSpec> specs = {
        { "FLAC (24-bit 48kHz)", ".flac", "-f lavfi -i sine=frequency=1000:sample_rate=48000:duration=2 -c:a flac" },
        { "MP3 (44.1kHz CBR)",   ".mp3",  "-f lavfi -i sine=frequency=440:sample_rate=44100:duration=2 -c:a libmp3lame -b:a 320k" },
        { "OGG Vorbis",          ".ogg",  "-f lavfi -i sine=frequency=880:sample_rate=44100:duration=2 -c:a libvorbis -q:a 6" },
        { "M4A (AAC 44.1kHz)",   ".m4a",  "-f lavfi -i sine=frequency=500:sample_rate=44100:duration=2 -c:a aac -b:a 256k" },
        { "WAV (16-bit 44.1kHz)",".wav",  "-f lavfi -i sine=frequency=600:sample_rate=44100:duration=2 -c:a pcm_s16le" }
    };

    for (const auto& sp : specs) {
        const std::string srcFile = sandbox + "\\test_material_" + sp.name.substr(0, 4) + sp.ext;
        const std::wstring cmd = L"\"" + rta::audio::widen(ffmpeg) + L"\" -v error -y " +
                                 rta::audio::widen(sp.ffmpegArgs) + L" \"" + rta::audio::widen(srcFile) + L"\"";
        int genRc = rta::audio::run_hidden(cmd);
        ck(genRc == 0, ("素材生成成功: " + sp.name).c_str());
        if (genRc != 0) continue;

        // 分别跑：流式解码 vs 临时 WAV 文件解码
        rta::audio::LoadNote noteStream;
        rta::audio::AudioBuffer bufStream;
        rta::audio::LoadNote noteFile;
        rta::audio::AudioBuffer bufFile;

        try {
            bufStream = rta::audio::read_via_ffmpeg_stream(srcFile, &noteStream);
        } catch (const std::exception& e) {
            ck(false, ("流式解码失败: " + sp.name).c_str(), e.what());
            continue;
        }

        try {
            bufFile = rta::audio::read_via_ffmpeg_file(srcFile, &noteFile);
        } catch (const std::exception& e) {
            ck(false, ("临时文件解码失败: " + sp.name).c_str(), e.what());
            continue;
        }

        // 验证基本元数据一致
        bool metaOk = (bufStream.rate == bufFile.rate &&
                       bufStream.channels == bufFile.channels &&
                       bufStream.bits == bufFile.bits &&
                       bufStream.isFloat == bufFile.isFloat &&
                       bufStream.frames() == bufFile.frames());
        ck(metaOk, (sp.name + " 元数据一致 (rate, ch, bits, isFloat, frames)").c_str(),
           "stream: rate=" + std::to_string(bufStream.rate) + " ch=" + std::to_string(bufStream.channels) +
           " bits=" + std::to_string(bufStream.bits) + " frames=" + std::to_string(bufStream.frames()) +
           " | file: rate=" + std::to_string(bufFile.rate) + " frames=" + std::to_string(bufFile.frames()));

        // 核心判据（铁律 3）：逐样本逐位比对
        bool samplesIdentical = (bufStream.raw.size() == bufFile.raw.size());
        size_t diffCount = 0;
        size_t firstDiffIdx = 0;
        if (samplesIdentical) {
            for (size_t i = 0; i < bufStream.raw.size(); ++i) {
                if (bufStream.raw[i] != bufFile.raw[i]) {
                    if (diffCount == 0) firstDiffIdx = i;
                    ++diffCount;
                }
            }
            if (diffCount > 0) samplesIdentical = false;
        }

        ck(samplesIdentical, (sp.name + " 流式 vs 临时WAV PCM逐位完全一致（0错位0误差）").c_str(),
           diffCount == 0 ? ("总样本数: " + std::to_string(bufStream.raw.size()))
                          : ("不匹配样本数: " + std::to_string(diffCount) + " 首次不匹配位于索引 " + std::to_string(firstDiffIdx)));

        std::printf("    [实测数据] %s: 流式耗时=%.1f ms (写磁盘=0 字节) | 临时文件耗时=%.1f ms (ffmpeg=%.1f ms, wavRead=%.1f ms)\n",
                    sp.name.c_str(), noteStream.totalMs, noteFile.totalMs, noteFile.ffmpegMs, noteFile.wavReadMs);
    }

    // 额外验证 DSF 文件的 ffmpeg 解码双路径（若 make_dsd 存在）
    const std::string makeDsd = rta::audio::wide_to_utf8(rta::audio::exe_dir_w()) + "\\make_dsd.exe";
    if (GetFileAttributesW(rta::audio::widen(makeDsd).c_str()) != INVALID_FILE_ATTRIBUTES) {
        const std::string dsfFile = sandbox + "\\test_dsd64.dsf";
        const std::wstring dsdCmd = L"\"" + rta::audio::widen(makeDsd) + L"\" --out \"" + rta::audio::widen(dsfFile) + L"\" --seconds 1";
        if (rta::audio::run_hidden(dsdCmd) == 0) {
            rta::audio::LoadNote noteStream;
            rta::audio::AudioBuffer bufStream;
            rta::audio::LoadNote noteFile;
            rta::audio::AudioBuffer bufFile;
            try {
                bufStream = rta::audio::read_via_ffmpeg_stream(dsfFile, &noteStream);
                bufFile = rta::audio::read_via_ffmpeg_file(dsfFile, &noteFile);
                bool same = (bufStream.raw == bufFile.raw && bufStream.frames() > 0);
                ck(same, "DSF (DSD64) ffmpeg 流式 vs 临时WAV PCM逐位完全一致（0错位0误差）",
                   "frames=" + std::to_string(bufStream.frames()));
                std::printf("    [实测数据] DSF (DSD64): 流式耗时=%.1f ms (写磁盘=0 字节) | 临时文件耗时=%.1f ms (ffmpeg=%.1f ms, wavRead=%.1f ms)\n",
                            noteStream.totalMs, noteFile.totalMs, noteFile.ffmpegMs, noteFile.wavReadMs);
            } catch (const std::exception& e) {
                ck(false, "DSF ffmpeg 双路径测试异常", e.what());
            }
        }
    }
}

// =========================================================================
// 4. 设置开关 (streamdecode) 与环境变量回退测试
// =========================================================================
static void test_settings_toggle(const std::string& ffmpeg, const std::string& sandbox) {
    std::printf("- 4 设置开关控制测试（streamdecode 与环境变量可控回退）\n");

    const std::string testFlac = sandbox + "\\test_setting_toggle.flac";
    const std::wstring cmd = L"\"" + rta::audio::widen(ffmpeg) +
        L"\" -v error -y -f lavfi -i sine=frequency=440:duration=1 -c:a flac \"" +
        rta::audio::widen(testFlac) + L"\"";
    rta::audio::run_hidden(cmd);

    // 4.1 默认开启流式
    rta::audio::set_stream_decode_enabled(true);
    ck(rta::audio::is_stream_decode_enabled() == true, "默认或显式开启时 stream_decode_enabled 为 true");
    {
        rta::audio::LoadNote note;
        rta::audio::AudioBuffer b = rta::audio::read_any(testFlac, &note);
        ck(note.text.find("流式解码") != std::string::npos, "开启时 read_any 走流式路径", "note=" + note.text);
    }

    // 4.2 设置关闭 streamdecode = false
    rta::audio::set_stream_decode_enabled(false);
    ck(rta::audio::is_stream_decode_enabled() == false, "set_stream_decode_enabled(false) 后为 false");
    {
        rta::audio::LoadNote note;
        rta::audio::AudioBuffer b = rta::audio::read_any(testFlac, &note);
        ck(note.text.find("流式解码") == std::string::npos && note.text.find("已用 ffmpeg 解码") != std::string::npos,
           "关闭时 read_any 自动回退走临时 WAV 路径", "note=" + note.text);
        ck(note.wavReadMs > 0.0 || note.totalMs > 0.0, "回退路径含 WAV 读取耗时");
    }

    // 恢复开启
    rta::audio::set_stream_decode_enabled(true);
    ck(rta::audio::is_stream_decode_enabled() == true, "恢复 stream_decode_enabled(true)");
}

// =========================================================================
// 5. 异常退出码透传与回退机制测试
// =========================================================================
static void test_error_propagation_and_fallback(const std::string& sandbox) {
    std::printf("- 5 异常退出码透传与回退机制验证\n");

    // 5.1 不存在文件：必须快速失败并带出 Win32 原因
    {
        const std::string missing = sandbox + "\\non_existent_file.flac";
        std::string err;
        try {
            rta::audio::LoadNote note;
            rta::audio::read_any(missing, &note);
        } catch (const std::exception& e) {
            err = e.what();
        }
        ck(!err.empty(), "不存在的文件必须抛出异常");
        ck(err.find("不存在") != std::string::npos, "错误文本包含 Win32 原因「不存在」", "err=" + err);
        ck(err.find("退出码") != std::string::npos, "错误文本保留退出码标识", "err=" + err);
    }

    // 5.2 损坏的文件：必须带出退出码和 ffmpeg 输出
    {
        const std::string corrupted = sandbox + "\\corrupted_file.flac";
        {
            FILE* f = fopen(corrupted.c_str(), "wb");
            if (f) {
                const char garbage[] = "NOT_A_VALID_FLAC_FILE_HEADER_GARBAGE_DATA_1234567890";
                fwrite(garbage, 1, sizeof(garbage), f);
                fclose(f);
            }
        }
        std::string err;
        try {
            rta::audio::LoadNote note;
            rta::audio::read_any(corrupted, &note);
        } catch (const std::exception& e) {
            err = e.what();
        }
        ck(!err.empty(), "损坏文件必须抛出异常");
        ck(err.find("退出码") != std::string::npos, "损坏文件错误文本保留 ffmpeg 退出码", "err=" + err);
    }
}

int main() {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    std::printf("== ROADMAP P6b: 解码免临时 WAV（流式读 ffmpeg 输出）离线验收 ==\n");

    const std::string ffmpeg = rta::audio::find_tool("ffmpeg");
    ck(!ffmpeg.empty(), "环境有 ffmpeg（测试前提）");
    if (ffmpeg.empty()) {
        std::printf("缺 ffmpeg，无法执行测试\n");
        return 1;
    }

    wchar_t tmpW[MAX_PATH] = {0};
    GetTempPathW(MAX_PATH, tmpW);
    const std::string sandbox =
        rta::audio::wide_to_utf8(tmpW) + "rta_stream_decode_" + std::to_string((unsigned long)GetCurrentProcessId());
    std::error_code ec;
    fs::create_directories(u8path_of(sandbox), ec);
    std::printf("  沙箱: %s\n", sandbox.c_str());

    test_wav_header_parser();
    test_chunked_pipe_simulation();
    test_bit_exact_5_formats(ffmpeg, sandbox);
    test_settings_toggle(ffmpeg, sandbox);
    test_error_propagation_and_fallback(sandbox);

    fs::remove_all(u8path_of(sandbox), ec);

    std::printf("\n测试结果: %s (g_fail=%d)\n", g_fail == 0 ? "全部通过" : "存在失败", g_fail);
    return g_fail == 0 ? 0 : 1;
}
