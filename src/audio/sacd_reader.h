// SPDX-License-Identifier: GPL-3.0-or-later
// Scarlet Book sector/TOC layout derived from SACD Ripper scarletbook.h/read.c.
// Copyright (c) 2010-2015 SACD Ripper respective authors, GPL-2.0-or-later.
// This bounded reader never mounts an image or opens a physical device.
#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif
#include "audio/dst_decoder.h"
namespace rta::audio {

struct ImageTrackKey {
    std::string physical, kind, area;
    std::vector<unsigned> numbers;
};
inline ImageTrackKey image_track_key(const std::string &p) {
    ImageTrackKey k;
    k.physical = p;
    size_t marker = std::string::npos;
    for (const char *token : {"#sacd=", "#dvda="}) {
        size_t at = p.find(token);
        while (at != std::string::npos) {
            if (at >= 4) {
                std::string ext = p.substr(at - 4, 4);
                for (auto &c : ext)
                    c = (char)std::tolower((unsigned char)c);
                if (ext == ".iso") {
                    if (marker != std::string::npos)
                        throw std::runtime_error("ISO: duplicate virtual track suffix");
                    marker = at;
                }
            }
            at = p.find(token, at + 1);
        }
    }
    if (marker == std::string::npos)
        return k;
    k.physical = p.substr(0, marker);
    std::string tail = p.substr(marker + 6);
    k.kind = p.compare(marker, 6, "#sacd=") == 0 ? "sacd" : "dvda";
    if (k.kind == "sacd") {
        size_t colon = tail.find(':');
        if (colon == std::string::npos)
            throw std::runtime_error("SACD: malformed virtual track key");
        k.area = tail.substr(0, colon);
        if (k.area != "2ch" && k.area != "mch")
            throw std::runtime_error("SACD: area must be 2ch or mch");
        tail.erase(0, colon + 1);
    }
    for (size_t start = 0;;) {
        size_t end = tail.find(':', start);
        auto value = tail.substr(start, end == std::string::npos ? end : end - start);
        if (value.empty() || value[0] < '1' || value[0] > '9')
            throw std::runtime_error("ISO: virtual track indices must be positive integers");
        unsigned number = 0;
        for (char c : value) {
            if (c < '0' || c > '9' || number > (2147483647u - unsigned(c - '0')) / 10)
                throw std::runtime_error("ISO: malformed or overflowing track index");
            number = number * 10 + unsigned(c - '0');
        }
        k.numbers.push_back(number);
        if (end == std::string::npos)
            break;
        start = end + 1;
    }
    if (k.numbers.size() != (k.kind == "sacd" ? 1u : 3u))
        throw std::runtime_error("ISO: wrong virtual track index count");
    return k;
}
inline std::string sacd_physical_path(const std::string &p) { return image_track_key(p).physical; }
inline bool sacd_path(const std::string &p) {
    auto key = image_track_key(p);
    if (key.kind == "dvda")
        return false;
    auto &s = key.physical;
    if (s.size() < 4)
        return false;
    auto e = s.substr(s.size() - 4);
    for (auto &c : e)
        c = (char)std::tolower((unsigned char)c);
    return e == ".iso";
}
namespace sacd_detail {
inline uint16_t be16(const uint8_t *p) { return uint16_t(p[0] * 256u + p[1]); }
inline uint32_t be32(const uint8_t *p) {
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
inline FILE *open(const std::string &p) {
#ifdef _WIN32
    return _wfopen(std::filesystem::u8path(p).c_str(), L"rb");
#else
    return fopen(p.c_str(), "rb");
#endif
}
inline bool at(FILE *f, uint64_t o, void *b, size_t n) {
#ifdef _WIN32
    if (_fseeki64(f, (long long)o, SEEK_SET))
        return false;
#else
    if (fseeko(f, (off_t)o, SEEK_SET))
        return false;
#endif
    return fread(b, 1, n, f) == n;
}
inline uint64_t length(FILE *f) {
#ifdef _WIN32
    _fseeki64(f, 0, SEEK_END);
    return (uint64_t)_ftelli64(f);
#else
    fseeko(f, 0, SEEK_END);
    return (uint64_t)ftello(f);
#endif
}

inline std::string text(const uint8_t *b, size_t size, size_t offset, unsigned charset) {
    if (!offset || offset >= size)
        return {};
    size_t end = offset;
    while (end < size && b[end])
        ++end;
    if (end == size || end - offset > 4096)
        return {};
    std::string source((const char *)b + offset, end - offset);
#ifdef _WIN32
    static constexpr unsigned pages[] = {65001, 20127, 28591, 932, 949, 936, 950, 28591};
    unsigned cp = pages[charset & 7];
    int n = MultiByteToWideChar(cp, 0, source.data(), int(source.size()), nullptr, 0);
    if (!n)
        return {};
    std::wstring w(n, 0);
    MultiByteToWideChar(cp, 0, source.data(), int(source.size()), w.data(), n);
    n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string out(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), out.data(), n, nullptr, nullptr);
    return out;
#else
    return source;
#endif
}
inline uint32_t time(const uint8_t *p) {
    if (p[1] >= 60 || p[2] >= 75)
        throw std::runtime_error("SACD: invalid track time");
    return uint32_t(p[0]) * 4500 + p[1] * 75 + p[2];
}
} // namespace sacd_detail
struct SacdTrack {
    std::string path, area, title, artist, album, albumArtist;
    int year = 0;
    int number = 0, channels = 0;
    bool dst = false;
    uint32_t startSector = 0, sectorCount = 0, startFrame = 0, frames = 0;
    uint32_t rate = 2822400;
    uint64_t bits() const { return uint64_t(frames) * 37632; }
    double seconds() const { return double(frames) / 75; }
};
inline std::vector<SacdTrack> sacd_tracks(const std::string &path) {
    using namespace sacd_detail;
    std::vector<SacdTrack> out;
    auto physical = sacd_physical_path(path);
    std::unique_ptr<FILE, decltype(&fclose)> f(open(physical), fclose);
    if (!f)
        return out;
    uint64_t bytes = length(f.get());
    std::array<uint8_t, 2048> m{};
    if (!at(f.get(), 510ull * 2048, m.data(), 2048) || memcmp(m.data(), "SACDMTOC", 8))
        return out;
    if (m[8] != 1 || m[9] > 20)
        throw std::runtime_error("SACD: unsupported Master TOC version");
    std::array<uint8_t, 2048> masterText{};
    std::string album, albumArtist;
    if (at(f.get(), 511ull * 2048, masterText.data(), 2048) && !memcmp(masterText.data(), "SACDText", 8)) {
        album = text(masterText.data(), 2048, be16(masterText.data() + 16), m[138]);
        albumArtist = text(masterText.data(), 2048, be16(masterText.data() + 18), m[138]);
    }
    for (int a = 0; a < 2; ++a) {
        uint32_t lba = be32(m.data() + 64 + a * 8), backup = be32(m.data() + 68 + a * 8);
        uint16_t count = be16(m.data() + 84 + a * 2);
        if (!lba && !backup)
            continue;
        if (count < 1 || count > 96)
            throw std::runtime_error("SACD: area TOC size outside 1..96 sectors");
        std::vector<uint8_t> b(size_t(count) * 2048);
        auto valid = [&](uint32_t s) {
            return s && uint64_t(s) * 2048 + b.size() <= bytes &&
                   at(f.get(), uint64_t(s) * 2048, b.data(), b.size()) &&
                   (!memcmp(b.data(), "TWOCHTOC", 8) || !memcmp(b.data(), "MULCHTOC", 8));
        };
        if (!valid(lba) && !valid(backup))
            throw std::runtime_error("SACD: both area TOC copies are missing or corrupt");
        const bool stereo = !memcmp(b.data(), "TWOCHTOC", 8);
        int ch = b[32], nt = b[69];
        int fmt = b[21] & 15;
        if (ch < 1 || ch > 6 || (stereo && ch != 2) || !nt || (fmt != 0 && fmt != 2 && fmt != 3))
            throw std::runtime_error("SACD: invalid area channels/tracks/frame format");
        const uint8_t *offsets = nullptr, *times = nullptr, *trackText = nullptr;
        for (size_t j = 2048; j + 2048 <= b.size(); j += 2048) {
            if (!memcmp(b.data() + j, "SACDTRL1", 8))
                offsets = b.data() + j;
            if (!memcmp(b.data() + j, "SACDTRL2", 8))
                times = b.data() + j;
            if (!trackText && !memcmp(b.data() + j, "SACDTTxt", 8))
                trackText = b.data() + j;
        }
        if (!offsets || !times)
            throw std::runtime_error("SACD: missing SACDTRL1/SACDTRL2 track table");
        uint64_t previousEnd = 0;
        for (int t = 0; t < nt; ++t) {
            SacdTrack tr;
            tr.number = t + 1;
            tr.area = stereo ? "2ch" : "mch";
            tr.channels = ch;
            tr.dst = fmt == 0;
            tr.startSector = be32(offsets + 8 + t * 4);
            tr.sectorCount = be32(offsets + 1028 + t * 4);
            tr.startFrame = time(times + 8 + t * 4);
            tr.frames = time(times + 1028 + t * 4);
            if (!tr.sectorCount || !tr.frames ||
                uint64_t(tr.startSector) * 2048 + uint64_t(tr.sectorCount) * 2048 > bytes ||
                uint64_t(tr.startSector) + 1 < previousEnd)
                throw std::runtime_error("SACD: track extent truncated, overlapping or empty");
            previousEnd = uint64_t(tr.startSector) + tr.sectorCount;
            tr.path = physical + "#sacd=" + tr.area + ":" + std::to_string(tr.number);
            tr.title = "Track " + std::to_string(tr.number) + " (" + tr.area + ")";
            tr.album = album;
            tr.albumArtist = albumArtist;
            tr.year = be16(m.data() + 120);
            if (trackText) {
                size_t at = be16(trackText + 8 + t * 2);
                if (at && at + 4 < 2048) {
                    unsigned entries = trackText[at];
                    at += 4;
                    for (unsigned j = 0; j < entries && at + 2 < 2048; ++j) {
                        unsigned type = trackText[at];
                        at += 2;
                        auto value = text(trackText, 2048, at, b[90]);
                        if (type == 1 && !value.empty())
                            tr.title = value;
                        if (type == 2)
                            tr.artist = value;
                        while (at < 2048 && trackText[at])
                            ++at;
                        while (at < 2048 && !trackText[at])
                            ++at;
                    }
                }
            }
            out.push_back(std::move(tr));
        }
    }
    return out;
}
inline SacdTrack sacd_select(const std::string &p) {
    auto tracks = sacd_tracks(p);
    if (tracks.empty())
        throw std::runtime_error("SACD: no Scarlet Book Master TOC at sector 510");
    auto key = image_track_key(p);
    if (!key.kind.empty() && key.kind != "sacd") throw std::runtime_error("SACD: foreign virtual track selector");
    if (key.kind == "sacd") {
        for (auto &t : tracks)
            if (t.path == p)
                return t;
        throw std::runtime_error("SACD: virtual track not found");
    }
    for (auto &t : tracks)
        if (t.area == "2ch")
            return t;
    return tracks.front();
}
class SacdReader {
    std::unique_ptr<FILE, decltype(&fclose)> f_{nullptr, fclose};
    SacdTrack tr_;
    uint32_t sector_ = 0, expectedTime_ = 0;
    std::vector<uint32_t> frameTimes_;
    uint64_t position_ = 0;
    std::vector<uint8_t> assembled_, decoded_;
    size_t decodedPos_ = 0;
    std::array<uint8_t, 2048> sectorData_{};
    std::vector<std::vector<uint8_t>> ready_;
    bool haveFrame_ = false, end_ = false;
    std::string error_;
    void finish() {
        if (!haveFrame_)
            return;
        if (tr_.dst)
            decoded_ = dst::decode(assembled_, tr_.channels, 4704);
        else {
            if (assembled_.size() != size_t(tr_.channels) * 4704)
                throw std::runtime_error("SACD: raw DSD frame is not 4704 bytes/channel");
            decoded_ = assembled_;
        }
        decodedPos_ = 0;
        assembled_.clear();
        haveFrame_ = false;
    }
    bool sector() {
        using namespace sacd_detail;
        if (sector_ >= tr_.sectorCount) {
            finish();
            end_ = true;
            return !decoded_.empty();
        }
        if (!at(f_.get(), uint64_t(tr_.startSector + sector_++) * 2048, sectorData_.data(), 2048))
            throw std::runtime_error("SACD: truncated audio sector");
        const uint8_t *h = sectorData_.data();
        unsigned np = h[0] >> 5, nf = (h[0] >> 2) & 7;
        bool dst = (h[0] & 1) != 0;
        if (dst != tr_.dst)
            throw std::runtime_error("SACD: audio sector encoding disagrees with area TOC");
        size_t off = 1 + 2 * np + (dst ? 4 : 3) * nf;
        if (off > 2048)
            throw std::runtime_error("SACD: invalid audio packet table");
        // At most one pending completed frame per sector is insufficient for short DST frames.
        // Queue packet descriptors, retaining this sector until all have been consumed.
        frameTimes_.clear();
        for (unsigned i = 0; i < nf; ++i)
            frameTimes_.push_back(time(h + 1 + 2 * np + i * (dst ? 4 : 3)));
        unsigned starts = 0;
        for (unsigned i = 0; i < np; ++i) {
            unsigned p = h[1 + i * 2];
            if ((p & 128) && ((p >> 3) & 7) == 2)
                ++starts;
        }
        if (starts != nf)
            throw std::runtime_error("SACD: frame-info count disagrees with audio starts");
        packet_ = 0;
        packets_ = np;
        payload_ = off;
        return true;
    }
    unsigned packet_ = 0, packets_ = 0;
    size_t payload_ = 0;
    bool nextFrame() {
        decoded_.clear();
        decodedPos_ = 0;
        for (;;) {
            if (packet_ >= packets_) {
                if (end_)
                    return false;
                if (!sector())
                    return false;
                if (end_)
                    return !decoded_.empty();
                if (!packets_)
                    continue;
            }
            const uint8_t *p = sectorData_.data() + 1 + packet_ * 2;
            size_t n = ((p[0] & 7) << 8) | p[1];
            bool start = p[0] & 128;
            unsigned type = (p[0] >> 3) & 7;
            if (payload_ + n > 2048)
                throw std::runtime_error("SACD: packet exceeds sector boundary");
            if (type == 2 && start && haveFrame_) {
                finish();
                return true;
            }
            if (type == 2) {
                if (start) {
                    unsigned fi = 0;
                    for (unsigned j = 0; j < packet_; ++j) {
                        unsigned previous = sectorData_[1 + j * 2];
                        if ((previous & 128) && ((previous >> 3) & 7) == 2)
                            ++fi;
                    }
                    if (fi >= frameTimes_.size())
                        throw std::runtime_error("SACD: missing frame timecode");
                    uint32_t stamp = frameTimes_[fi];
                    if (stamp < tr_.startFrame) {
                        haveFrame_ = false;
                        assembled_.clear();
                    } else if (uint64_t(stamp) >= uint64_t(tr_.startFrame) + tr_.frames) {
                        end_ = true;
                        return false;
                    } else {
                        if (stamp != expectedTime_)
                            throw std::runtime_error(
                                "SACD: missing or nonconsecutive frame timecode inside selected track");
                        ++expectedTime_;
                        haveFrame_ = true;
                    }
                }
                if (haveFrame_) {
                    if (assembled_.size() + n > 65536)
                        throw std::runtime_error("SACD: frame exceeds 64 KiB");
                    assembled_.insert(assembled_.end(), sectorData_.begin() + payload_,
                                      sectorData_.begin() + payload_ + n);
                }
            }
            payload_ += n;
            ++packet_;
        }
    }

  public:
    void open(const std::string &p) {
        tr_ = sacd_select(p);
        f_.reset(sacd_detail::open(sacd_physical_path(p)));
        if (!f_)
            throw std::runtime_error("SACD: cannot open image");
        rewind();
    }
    const SacdTrack &track() const { return tr_; }
    bool is_open() const { return bool(f_); }
    void rewind() {
        sector_ = 0;
        expectedTime_ = tr_.startFrame;
        position_ = 0;
        packet_ = packets_ = 0;
        assembled_.clear();
        decoded_.clear();
        decodedPos_ = 0;
        haveFrame_ = end_ = false;
        error_.clear();
    }
    size_t read(size_t bytesPerChannel, std::vector<std::vector<uint8_t>> &out) {
        out.assign(tr_.channels, {});
        if (!f_)
            return 0;
        size_t done = 0;
        uint64_t total = uint64_t(tr_.frames) * 4704;
        while (done < bytesPerChannel && position_ < total) {
            if (decodedPos_ >= decoded_.size()) {
                if (!nextFrame())
                    throw std::runtime_error("SACD: audio ends before declared track duration");
            }
            size_t avail = (decoded_.size() - decodedPos_) / tr_.channels;
            size_t take = std::min<size_t>({avail, bytesPerChannel - done, size_t(total - position_)});
            if (!take)
                throw std::runtime_error("SACD: incomplete channel cluster");
            for (size_t i = 0; i < take; ++i)
                for (int c = 0; c < tr_.channels; ++c)
                    out[c].push_back(decoded_[decodedPos_ + i * tr_.channels + c]);
            decodedPos_ += take * tr_.channels;
            position_ += take;
            done += take;
        }
        return done;
    }
    void seek(uint64_t bytePerChannel) {
        if (bytePerChannel > uint64_t(tr_.frames) * 4704)
            throw std::runtime_error("SACD: seek outside track");
        if (bytePerChannel < position_)
            rewind();
        std::vector<std::vector<uint8_t>> discard;
        while (position_ < bytePerChannel)
            read(size_t(std::min<uint64_t>(4704, bytePerChannel - position_)), discard);
    }
    uint64_t position() const { return position_; }
    size_t bufferedBytes() const { return assembled_.capacity() + decoded_.capacity() + 2048; }
};
} // namespace rta::audio
