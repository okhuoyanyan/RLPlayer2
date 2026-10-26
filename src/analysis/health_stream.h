// health_stream.h — 体检流式解码源：按块随机读 PCM，内存与曲长无关
//
// 为什么（261026-03 遗留项）：体检原先走 read_any 整曲解码（DSD64 271 MB 文件 → 517 MB
//   int32 缓冲），再铺成 double 平面又翻一倍，进程峰值实测 2.6 GB。体检只需要"顺序看一遍
//   样本"，不需要握整曲 —— 所以给体检配一个分块的、可随机读的 PCM 源。
//
// 覆盖范围（其余格式仍走 read_any 整段，见 track_health.h 的说明）：
//   * .dsf / .dff：复用播放侧的 rta::audio::DsdPcmStream（已由 tests/test_dsd_stream_decode.cpp
//     证明与 read_dsd_native 逐样本一致）。1-bit 流按块读、按块抽取，绝不整曲缓冲。
//   * .wav：本文件的 WavPcmStream（只解析头部，随后按块 fseek+read+decode_sample，
//     与 read_wav 共用同一套头解析 / 样本解码 / 帧数口径）。
//
// 纪律：open() 只做「这个文件能不能安全地分块读」的判断，任何拿不准的情形（data 在 fmt
//   之前、声明长度超过文件、DST 压缩、需要 ffmpeg 的格式）一律返回 false，由调用方回退
//   整段路径 —— 与其猜，不如走老路，保证结果与老实现逐位相同。
#pragma once
#include "audio/wav.h"           // parse_wav_header / decode_sample / AudioBuffer / wav_open
#include "audio/audio_loader.h"  // DsdPcmStream / dsd_pcm_settings / probe 相关
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>
#include <algorithm>
#include <cmath>

namespace rta::analysis {
namespace detail {

// 分块 WAV 读取器（只读）。帧数口径与 read_wav 完全一致：
//   nSamples = dataDeclaredBytes / bytesPer；frames = nSamples / channels（除不尽的尾巴丢弃）。
class WavPcmStream {
public:
    ~WavPcmStream() { close(); }
    WavPcmStream() = default;
    WavPcmStream(const WavPcmStream&) = delete;
    WavPcmStream& operator=(const WavPcmStream&) = delete;

    bool open(const std::string& path, std::string* errOut = nullptr) {
        close();
        auto fail = [&](const std::string& why) { if (errOut) *errOut = why; close(); return false; };

        FILE* f = rta::audio::detail::wav_open(path, L"rb", "rb");
        if (!f) return fail("无法打开文件: " + path);
        f_ = f;
        ::_fseeki64(f_, 0, SEEK_END);
        const long long fsize = ::_ftelli64(f_);
        ::_fseeki64(f_, 0, SEEK_SET);
        if (fsize <= 0) return fail("文件为空或无法确定大小: " + path);

        // 头最多 1 MB（parse_wav_header 自己的上限）；读满或读完整文件
        const size_t want = (size_t)std::min<long long>(fsize, 1LL << 20);
        std::vector<uint8_t> head(want);
        if (std::fread(head.data(), 1, want, f_) != want) return fail("读取头部失败: " + path);

        rta::audio::WavHeaderInfo hdr{};
        std::string herr;
        const int r = rta::audio::parse_wav_header(head.data(), head.size(), hdr, &herr);
        if (r <= 0) return fail("WAV 头无法解析（" + (herr.empty() ? std::string("需要更多字节") : herr) + "）");

        // —— 以下逐条对齐 read_wav 的接受条件；任何一条拿不准就回退整段路径 ——
        if (hdr.dataDeclaredBytes == 0xFFFFFFFFu) return fail("WAV data 长度为 0xFFFFFFFF（管道流），不适合分块读");
        if (hdr.dataDeclaredBytes == 0) return fail("WAV data 块长度为 0");
        const int bytesPer = (int)hdr.bits / 8;
        if (bytesPer <= 0) return fail("WAV 位深换算字节数非法");
        if (hdr.isFloat) {
            if (hdr.bits != 32) return fail("float WAV 位深不是 32（read_wav 也不接受）");
        } else if (hdr.bits != 8 && hdr.bits != 16 && hdr.bits != 24 && hdr.bits != 32) {
            return fail("WAV 位深非法（允许 8/16/24/32）");
        }
        if (hdr.channels < 1 || hdr.channels > 32) return fail("WAV 声道数非法");
        if (hdr.rate == 0 || hdr.rate > 4000000u) return fail("WAV 采样率非法");
        const uint64_t avail = (uint64_t)fsize - (uint64_t)hdr.headerBytes;
        if (hdr.headerBytes >= (size_t)fsize || hdr.dataDeclaredBytes > avail)
            return fail("WAV data 块越界（声明 " + std::to_string(hdr.dataDeclaredBytes) +
                        " > 可用 " + std::to_string(avail) + "）");

        const uint64_t nSamples = (uint64_t)hdr.dataDeclaredBytes / (uint64_t)bytesPer;
        const size_t frames = (size_t)(nSamples / (uint64_t)hdr.channels);
        if (frames == 0) return fail("WAV 帧数为 0");

        rate_ = hdr.rate;
        channels_ = (int)hdr.channels;
        bits_ = (int)hdr.bits;
        isFloat_ = hdr.isFloat;
        bytesPer_ = bytesPer;
        dataOffset_ = (uint64_t)hdr.headerBytes;
        totalFrames_ = frames;
        return true;
    }

    void close() {
        if (f_) { std::fclose(f_); f_ = nullptr; }
        totalFrames_ = 0;
    }
    bool   is_open() const { return f_ != nullptr; }
    uint32_t rate() const { return rate_; }
    int    channels() const { return channels_; }
    int    bits() const { return bits_; }
    bool   isFloat() const { return isFloat_; }
    size_t totalFrames() const { return totalFrames_; }

    // 读取 [pos, pos+n) 帧（顺序读最快；向后/回头都支持，靠 fseek）。返回实际产出的帧数。
    size_t read(size_t pos, size_t n, rta::audio::AudioBuffer& out) {
        out.rate = rate_; out.channels = channels_; out.bits = bits_; out.isFloat = isFloat_;
        out.raw.clear();
        if (!f_ || n == 0 || pos >= totalFrames_) return 0;
        if (n > totalFrames_ - pos) n = totalFrames_ - pos;
        const uint64_t off = dataOffset_ + (uint64_t)pos * (uint64_t)channels_ * (uint64_t)bytesPer_;
        const size_t bytes = n * (size_t)channels_ * (size_t)bytesPer_;
        if (buf_.size() < bytes) buf_.resize(bytes);
        if (::_fseeki64(f_, (long long)off, SEEK_SET) != 0) return 0;
        const size_t got = std::fread(buf_.data(), 1, bytes, f_);
        const size_t framesGot = got / ((size_t)channels_ * (size_t)bytesPer_);
        if (framesGot == 0) return 0;
        out.raw.resize(framesGot * (size_t)channels_);
        for (size_t i = 0; i < framesGot * (size_t)channels_; ++i)
            out.raw[i] = rta::audio::decode_sample(buf_.data() + i * (size_t)bytesPer_, bits_, isFloat_);
        return framesGot;
    }

private:
    FILE*    f_ = nullptr;
    uint64_t dataOffset_ = 0;
    size_t   totalFrames_ = 0;
    uint32_t rate_ = 0;
    int      channels_ = 0;
    int      bits_ = 0;
    int      bytesPer_ = 0;
    bool     isFloat_ = false;
    std::vector<uint8_t> buf_;
};

// 体检流式源：把 DSD / WAV 两条分块读路径统一成一个接口（供 track_health.h 的驱动循环用）。
class HealthSource {
public:
    enum class Kind { None, Dsd, Wav };

    bool open(const std::string& path, std::string* whyOut = nullptr) {
        close();
        const std::string ext = rta::audio::lowerExt(path);
        if (ext == ".dsf" || ext == ".dff") {
            std::string err;
            if (!dsd_.open(path, rta::audio::dsd_pcm_settings(), &err)) {
                if (whyOut) *whyOut = err;
                return false;
            }
            kind_ = Kind::Dsd;
            rate_ = dsd_.rate(); channels_ = dsd_.channels(); bits_ = 32; isFloat_ = false;
            totalFrames_ = dsd_.totalFrames();
            return true;
        }
        if (ext == ".wav" || ext == ".wave" || ext.empty()) {
            std::string err;
            if (!wav_.open(path, &err)) {
                if (whyOut) *whyOut = err;
                return false;
            }
            kind_ = Kind::Wav;
            rate_ = wav_.rate(); channels_ = wav_.channels(); bits_ = wav_.bits();
            isFloat_ = wav_.isFloat(); totalFrames_ = wav_.totalFrames();
            return true;
        }
        if (whyOut) *whyOut = "非原生可流式格式（" + ext + "），走整段解码";
        return false;
    }

    void close() { dsd_.close(); wav_.close(); kind_ = Kind::None; totalFrames_ = 0; }

    Kind   kind() const { return kind_; }
    uint32_t rate() const { return rate_; }
    int    channels() const { return channels_; }
    int    bits() const { return bits_; }
    bool   isFloat() const { return isFloat_; }
    size_t totalFrames() const { return totalFrames_; }

    size_t read(size_t pos, size_t n, rta::audio::AudioBuffer& out) {
        if (kind_ == Kind::Dsd) return dsd_.read(pos, n, out);
        if (kind_ == Kind::Wav) return wav_.read(pos, n, out);
        return 0;
    }

private:
    Kind     kind_ = Kind::None;
    uint32_t rate_ = 0;
    int      channels_ = 0;
    int      bits_ = 32;
    bool     isFloat_ = false;
    size_t   totalFrames_ = 0;
    rta::audio::DsdPcmStream dsd_;
    WavPcmStream wav_;
};

} // namespace detail
} // namespace rta::analysis
