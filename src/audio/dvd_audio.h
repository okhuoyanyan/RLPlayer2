// SPDX-License-Identifier: GPL-3.0-or-later
// DVD-Audio IFO navigation; no mounting, devices, encryption bypass or shell.
// Syntax reference: DVD-Audio Tools project spec/ats_ifo.shtml and aob.shtml.
#pragma once
#include "audio/iso_filesystem.h"
#include "audio/wav.h"
#include <map>
namespace rta::audio {
class Iso9660 {
    std::unique_ptr<FILE, decltype(&fclose)> file_{nullptr, fclose};
    uint64_t length_ = 0;
    UdfNavigation udf_;

  public:
    void open(const std::string &p) {
        file_.reset(sacd_detail::open(p));
        if (!file_)
            throw std::runtime_error("ISO: cannot open image");
        length_ = sacd_detail::length(file_.get());
        udf_.attach(file_.get(), length_);
    }
    std::vector<uint8_t> read(const IsoExtent &e, size_t max = 1048576) {
        if (e.bytes > max || (!e.udf && uint64_t(e.sector) * 2048 + e.bytes > length_))
            throw std::runtime_error("ISO: extent exceeds file/budget");
        std::vector<uint8_t> b(e.bytes);
        if (!iso_extent_at(file_.get(), e, 0, b.data(), b.size()))
            throw std::runtime_error("ISO: short extent");
        return b;
    }
    static uint32_t le(const uint8_t *p) {
        return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
    }
    IsoExtent root() {
        std::array<uint8_t, 2048> b{};
        for (int i = 16; i < 32; ++i) {
            if (!sacd_detail::at(file_.get(), uint64_t(i) * 2048, b.data(), 2048) ||
                memcmp(b.data() + 1, "CD001", 5))
                break;
            if (b[0] == 1)
                return {le(b.data() + 158), le(b.data() + 166)};
        }
        return udf_.root();
    }
    std::map<std::string, IsoExtent> directory(IsoExtent e) {
        if (e.udf)
            return udf_.directory(e);
        auto b = read(e);
        std::map<std::string, IsoExtent> out;
        for (size_t i = 0; i < b.size();) {
            unsigned n = b[i];
            if (!n) {
                i = (i / 2048 + 1) * 2048;
                continue;
            }
            if (n < 34 || i + n > b.size() || 33 + b[i + 32] > n)
                throw std::runtime_error("ISO: malformed directory record");
            std::string name((char *)b.data() + i + 33, b[i + 32]);
            auto sem = name.find(';');
            if (sem != std::string::npos)
                name.resize(sem);
            for (auto &c : name)
                c = (char)std::toupper((unsigned char)c);
            if (b[i + 25] & 128)
                throw std::runtime_error("ISO: multi-extent files are unsupported");
            if (!(name.size() == 1 && (name[0] == 0 || name[0] == 1)))
                out[name] = {le(b.data() + i + 2), le(b.data() + i + 10)};
            i += n;
        }
        return out;
    }
    void copy(const IsoExtent &e, uint64_t offset, uint64_t bytes, FILE *out) {
        if (offset > e.bytes || bytes > e.bytes - offset)
            throw std::runtime_error("DVD-Audio: AOB range outside extent");
        std::array<uint8_t, 65536> b{};
        while (bytes) {
            size_t n = size_t(std::min<uint64_t>(b.size(), bytes));
            if (!iso_extent_at(file_.get(), e, offset, b.data(), n) || fwrite(b.data(), 1, n, out) != n)
                throw std::runtime_error("DVD-Audio: track extraction short I/O");
            offset += n;
            bytes -= n;
        }
    }
};
inline std::string dvd_physical_path(const std::string &p) { return image_track_key(p).physical; }
struct DvdAudioTrack {
    std::string path;
    int group = 0, title = 0, track = 0;
    uint32_t firstSector = 0, lastSector = 0, pts = 0, durationPts = 0;
    bool mlp = false;
    std::vector<IsoExtent> aobs;
    double seconds() const { return double(durationPts) / 90000.; }
};
inline uint64_t dvd_mlp_trim_samples(uint32_t trackPts, uint64_t packetPts, uint32_t rate);
inline std::vector<DvdAudioTrack> dvd_audio_tracks(const std::string &p) {
    std::vector<DvdAudioTrack> out;
    Iso9660 iso;
    iso.open(dvd_physical_path(p));
    std::map<std::string, IsoExtent> root;
    try {
        root = iso.directory(iso.root());
    } catch (const std::exception &) {
        return out;
    }
    auto audio = root.find("AUDIO_TS");
    if (audio == root.end())
        return out;
    auto files = iso.directory(audio->second);
    auto mgr = files.find("AUDIO_TS.IFO");
    if (mgr == files.end())
        return out;
    auto amg = iso.read(mgr->second);
    if (amg.size() < 12 || memcmp(amg.data(), "DVDAUDIO-AMG", 12))
        throw std::runtime_error("DVD-Audio: invalid AUDIO_TS.IFO");
    for (int group = 1; group <= 99; ++group) {
        char name[32];
        snprintf(name, sizeof name, "ATS_%02d_0.IFO", group);
        auto it = files.find(name);
        if (it == files.end())
            continue;
        auto b = iso.read(it->second);
        if (b.size() < 2048 || memcmp(b.data(), "DVDAUDIO-ATS", 12))
            throw std::runtime_error("DVD-Audio: invalid ATS IFO");
        size_t base = size_t(sacd_detail::be32(b.data() + 204)) * 2048;
        if (base < 2048 || base + 8 > b.size())
            throw std::runtime_error("DVD-Audio: invalid title table offset");
        unsigned titles = sacd_detail::be16(b.data() + base);
        if (!titles || titles > 99 || base + 8 + 8 * titles > b.size())
            throw std::runtime_error("DVD-Audio: invalid title count");
        std::vector<IsoExtent> aobs;
        uint64_t aobBytes = 0;
        for (int n = 1; n <= 9; ++n) {
            snprintf(name, sizeof name, "ATS_%02d_%d.AOB", group, n);
            auto a = files.find(name);
            if (a == files.end())
                break;
            aobs.push_back(a->second);
            aobBytes += a->second.bytes;
        }
        for (unsigned title = 0; title < titles; ++title) {
            size_t off = base + sacd_detail::be32(b.data() + base + 8 + title * 8 + 4);
            if (off + 18 > b.size())
                throw std::runtime_error("DVD-Audio: title record outside IFO");
            unsigned tracks = b[off + 2];
            if (!tracks || tracks != b[off + 3])
                throw std::runtime_error("DVD-Audio: invalid track count");
            size_t sectors = off + sacd_detail::be16(b.data() + off + 14);
            if (off + 18 + 20 * tracks > b.size() || sectors + 12 * tracks > b.size())
                throw std::runtime_error("DVD-Audio: track tables truncated");
            for (unsigned t = 0; t < tracks; ++t) {
                DvdAudioTrack tr;
                tr.group = group;
                tr.title = title + 1;
                tr.track = t + 1;
                tr.firstSector = sacd_detail::be32(b.data() + sectors + t * 12 + 4);
                tr.lastSector = sacd_detail::be32(b.data() + sectors + t * 12 + 8);
                tr.pts = sacd_detail::be32(b.data() + off + 18 + t * 20 + 6);
                tr.durationPts = sacd_detail::be32(b.data() + off + 18 + t * 20 + 10);
                tr.mlp = b[256 + 16 * std::min<unsigned>(title, 7)] == 1;
                tr.aobs = aobs;
                if (!tr.durationPts || tr.lastSector < tr.firstSector ||
                    (uint64_t(tr.lastSector) + 1) * 2048 > aobBytes)
                    throw std::runtime_error("DVD-Audio: invalid/truncated AOB track range");
                tr.path = dvd_physical_path(p) + "#dvda=" + std::to_string(group) + ":" +
                          std::to_string(title + 1) + ":" + std::to_string(t + 1);
                out.push_back(std::move(tr));
            }
        }
    }
    return out;
}
inline DvdAudioTrack dvd_audio_select(const std::string &p) {
    auto v = dvd_audio_tracks(p);
    if (v.empty())
        throw std::runtime_error("DVD-Audio: no playable ATS title");
    auto key=image_track_key(p);
    if(!key.kind.empty() && key.kind!="dvda")throw std::runtime_error("DVD-Audio: foreign virtual track selector");
    if(key.kind.empty())return v.front();
    for (auto &t : v)
        if (t.path == p)
            return t;
    throw std::runtime_error("DVD-Audio: virtual track not found");
}
inline void dvd_audio_extract(const std::string &p, const DvdAudioTrack &t, FILE *out) {
    Iso9660 iso;
    iso.open(dvd_physical_path(p));
    uint64_t start = uint64_t(t.firstSector) * 2048, end = (uint64_t(t.lastSector) + 1) * 2048, base = 0;
    for (const auto &e : t.aobs) {
        uint64_t a = std::max(base, start), z = std::min(base + e.bytes, end);
        if (z > a)
            iso.copy(e, a - base, z - a, out);
        base += e.bytes;
    }
}

// Channel grouping and two-sample word packing adapted from FFmpeg pcm-dvda.c
// Copyright (c) 2026 Kacper Michajlow, LGPL-2.1-or-later. Upstream/notice retained.
struct DvdPcmFormat {
    int channels = 0, bits = 0, rate = 0, assignment = 0;
    int groupChannels[2]{}, groupBits[2]{};
    std::vector<int> map;
    size_t pairBytes = 0;
};
inline DvdPcmFormat dvd_pcm_format(const uint8_t *h, size_t n) {
    if (n < 12 || h[0] != 0xa0 || h[3] < 8 || size_t(4 + h[3]) > n)
        throw std::runtime_error("DVD-A LPCM: invalid private header");
    static constexpr int g1[] = {1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 3, 3, 3, 3, 3, 4, 4, 4};
    static constexpr int g2[] = {0, 0, 1, 2, 1, 2, 3, 1, 2, 3, 2, 3, 4, 1, 2, 1, 2, 3, 1, 1, 2};
    // Windows/WAVE canonical channel rank: FL,FR,FC,LFE,BL,BR,BC.
    static const std::vector<std::vector<int>> order = {{2},
                                                        {0, 1},
                                                        {0, 1, 6},
                                                        {0, 1, 4, 5},
                                                        {0, 1, 3},
                                                        {0, 1, 3, 6},
                                                        {0, 1, 3, 4, 5},
                                                        {0, 1, 2},
                                                        {0, 1, 2, 6},
                                                        {0, 1, 2, 4, 5},
                                                        {0, 1, 2, 3},
                                                        {0, 1, 2, 3, 6},
                                                        {0, 1, 2, 3, 4, 5},
                                                        {0, 1, 2, 6},
                                                        {0, 1, 2, 4, 5},
                                                        {0, 1, 2, 3},
                                                        {0, 1, 2, 3, 6},
                                                        {0, 1, 2, 3, 4, 5},
                                                        {0, 1, 4, 5, 3},
                                                        {0, 1, 4, 5, 2},
                                                        {0, 1, 4, 5, 2, 3}};
    DvdPcmFormat f;
    f.assignment = h[10] & 31;
    if (f.assignment > 20 || (h[9] & 15))
        throw std::runtime_error("DVD-A LPCM: invalid channel assignment/type");
    f.groupChannels[0] = g1[f.assignment];
    f.groupChannels[1] = g2[f.assignment];
    for (int g = 0; g < 2; ++g) {
        unsigned q = g ? h[7] & 15 : h[7] >> 4, r = g ? h[8] & 15 : h[8] >> 4;
        if (g && (q == 15 || r == 15))
            f.groupChannels[g] = 0;
        if (!f.groupChannels[g])
            continue;
        if (q > 2 || (r & 7) > 2)
            throw std::runtime_error("DVD-A LPCM: invalid precision/rate");
        int rate = ((r & 8) ? 44100 : 48000) << (r & 7);
        if (g && rate != f.rate)
            throw std::runtime_error("DVD-A LPCM: mixed channel-group rates are not supported");
        f.rate = rate;
        f.groupBits[g] = 16 + int(q) * 4;
        f.bits = std::max(f.bits, f.groupBits[g]);
        f.channels += f.groupChannels[g];
        f.pairBytes += size_t(f.groupChannels[g]) * 2 * f.groupBits[g] / 8;
    }
    auto ranks = order[f.assignment];
    ranks.resize(f.channels);
    auto sorted = ranks;
    std::sort(sorted.begin(), sorted.end());
    for (int v : ranks)
        f.map.push_back(int(std::find(sorted.begin(), sorted.end(), v) - sorted.begin()));
    return f;
}
inline std::vector<int32_t> dvd_pcm_unpack(const DvdPcmFormat &f, const uint8_t *b, size_t bytes) {
    size_t pairs = bytes / f.pairBytes;
    std::vector<int32_t> out(pairs * 2 * f.channels);
    size_t off = 0;
    for (size_t pair = 0; pair < pairs; ++pair)
        for (int g = 1; g >= 0; --g) {
            int ch = f.groupChannels[g], bits = f.groupBits[g];
            if (!ch)
                continue;
            std::vector<uint32_t> v(size_t(ch) * 2);
            for (auto &x : v) {
                x = uint32_t(sacd_detail::be16(b + off)) << 16;
                off += 2;
            }
            if (bits == 24)
                for (auto &x : v)
                    x |= uint32_t(b[off++]) << 8;
            if (bits == 20) {
                for (size_t j = 0; j < v.size(); j += 2) {
                    unsigned lo = b[off++];
                    v[j] |= uint32_t(lo >> 4) << 12;
                    v[j + 1] |= uint32_t(lo & 15) << 12;
                }
            }
            int base = g ? f.groupChannels[0] : 0;
            for (int sample = 0; sample < 2; ++sample)
                for (int c = 0; c < ch; ++c)
                    out[(pair * 2 + sample) * f.channels + f.map[base + c]] = int32_t(v[sample * ch + c]);
        }
    return out;
}
class DvdLpcmReader {
    std::unique_ptr<FILE, decltype(&fclose)> file_{nullptr, fclose};
    DvdAudioTrack tr_;
    DvdPcmFormat format_;
    uint32_t sector_ = 0;
    uint64_t pos_ = 0, total_ = 0;
    std::vector<int32_t> pending_;
    size_t cursor_ = 0;
    bool started_ = false;
    uint64_t startSkip_ = 0;
    std::vector<uint8_t> carry_;
    bool packet() {
        if (sector_ > tr_.lastSector)
            return false;
        uint64_t relative = uint64_t(sector_++) * 2048, base = 0;
        IsoExtent extent;
        bool found = false;
        for (auto &e : tr_.aobs) {
            if (relative >= base && relative + 2048 <= base + e.bytes) {
                extent = e;
                relative -= base;
                found = true;
                break;
            }
            base += e.bytes;
        }
        if (!found)
            throw std::runtime_error("DVD-A LPCM: sector outside AOB");
        std::array<uint8_t, 2048> b{};
        if (!iso_extent_at(file_.get(), extent, relative, b.data(), 2048))
            throw std::runtime_error("DVD-A LPCM: short sector");
        for (size_t i = 0; i + 9 < 2048; ++i) {
            if (b[i] || b[i + 1] || b[i + 2] != 1 || b[i + 3] != 0xbd)
                continue;
            size_t end = i + 6 + sacd_detail::be16(b.data() + i + 4), h = i + 9 + b[i + 8];
            if (end > 2048 || h + 12 > end)
                throw std::runtime_error("DVD-A LPCM: truncated PES");
            if (b[h] != 0xa0)
                throw std::runtime_error("DVD-A LPCM: unexpected substream");
            auto fmt = dvd_pcm_format(b.data() + h, end - h);
            if (!format_.channels)
                format_ = fmt;
            else if (fmt.channels != format_.channels || fmt.rate != format_.rate ||
                     fmt.bits != format_.bits || fmt.assignment != format_.assignment || fmt.groupBits[0] != format_.groupBits[0] || fmt.groupBits[1] != format_.groupBits[1])
                throw std::runtime_error("DVD-A LPCM: format changes within track");
            size_t start = h + 4 + b[h + 3];
            if (!started_) {
                size_t access = h + 6 + sacd_detail::be16(b.data() + h + 4);
                if (access < start || access > end)
                    throw std::runtime_error("DVD-A LPCM: first access unit outside PES");
                start = access;
                started_ = true;
                if (!(b[i + 7] & 0x80) || b[i + 8] < 5)
                    throw std::runtime_error("DVD-A LPCM: first audio PES has no PTS; exact track start cannot be determined");
                const uint8_t *ts = b.data() + i + 9;
                if (!(ts[0] & 1) || !(ts[2] & 1) || !(ts[4] & 1))
                    throw std::runtime_error("DVD-A LPCM: invalid PES PTS marker bits");
                uint64_t pts = (uint64_t((ts[0] >> 1) & 7) << 30) | (uint64_t(ts[1]) << 22) |
                               (uint64_t(ts[2] >> 1) << 15) | (uint64_t(ts[3]) << 7) | (ts[4] >> 1);
                try { startSkip_ = dvd_mlp_trim_samples(tr_.pts, pts, format_.rate); }
                catch (const std::exception &) { throw std::runtime_error("DVD-A LPCM: IFO track starts before available timestamped audio"); }

            }
            carry_.insert(carry_.end(), b.begin() + start, b.begin() + end);
            size_t bytes = (carry_.size() / fmt.pairBytes) * fmt.pairBytes;
            pending_ = dvd_pcm_unpack(fmt, carry_.data(), bytes);
            carry_.erase(carry_.begin(), carry_.begin() + bytes);
            cursor_ = 0;
            if (startSkip_) {
                size_t skip = size_t(std::min<uint64_t>(startSkip_, pending_.size() / format_.channels));
                cursor_ = skip * format_.channels;
                startSkip_ -= skip;
            }
            return true;
        }
        return true; // padding/system-only sector, still bounded
    }

  public:
    void open(const std::string &p) {
        format_ = {};
        tr_ = dvd_audio_select(p);
        if (tr_.mlp)
            throw std::runtime_error("DVD-A: use MLP decoder for compressed track");
        file_.reset(sacd_detail::open(dvd_physical_path(p)));
        if (!file_)
            throw std::runtime_error("DVD-A: cannot open image");
        rewind();
        while (!format_.channels) {
            if (!packet())
                throw std::runtime_error("DVD-A LPCM: no audio packets");
        }
        total_ = (uint64_t(tr_.durationPts) * format_.rate + 45000) / 90000;
    }
    void rewind() {
        sector_ = tr_.firstSector;
        pos_ = 0;
        pending_.clear();
        cursor_ = 0;
        carry_.clear();
        started_ = false;
        startSkip_ = 0;
    }
    const DvdPcmFormat &format() const { return format_; }
    uint64_t frames() const { return total_; }
    size_t read(size_t pos, size_t n, AudioBuffer &out) {
        out.rate = format_.rate;
        out.channels = format_.channels;
        out.bits = 32;
        out.isFloat = false;
        out.raw.clear();
        if (pos >= total_ || !n)
            return 0;
        if (pos < pos_)
            rewind();
        size_t wanted = size_t(std::min<uint64_t>(n, total_ - pos));
        while (pos_ < uint64_t(pos) + wanted) {
            if (cursor_ >= pending_.size()) {
                if (!packet())
                    throw std::runtime_error("DVD-A LPCM: stream ends before IFO duration");
                continue;
            }
            size_t available = (pending_.size() - cursor_) / format_.channels;
            size_t skip = pos_ < pos ? size_t(std::min<uint64_t>(pos - pos_, available)) : 0;
            cursor_ += skip * format_.channels;
            pos_ += skip;
            available -= skip;
            if (!available)
                continue;
            size_t take = size_t(std::min<uint64_t>(available, uint64_t(pos) + wanted - pos_));
            out.raw.insert(out.raw.end(), pending_.begin() + cursor_,
                           pending_.begin() + cursor_ + take * format_.channels);
            cursor_ += take * format_.channels;
            pos_ += take;
        }
        return out.frames();
    }
};

// IFO uses 32-bit 90-kHz ticks; MPEG PES carries 33 bits. Select the nearest
// IFO epoch, including a track boundary crossing timestamp wrap.
inline uint64_t dvd_mlp_trim_samples(uint32_t trackPts, uint64_t packetPts, uint32_t rate) {
    uint32_t modulo = trackPts - uint32_t(packetPts);
    int64_t delta = modulo;
    if (modulo >= 0x80000000u)
        delta -= 0x100000000ll;
    if (delta < 0)
        throw std::runtime_error("DVD-A MLP: IFO track starts before available timestamped audio");
    return (uint64_t(delta) * rate + 45000) / 90000;
}
inline uint64_t dvd_first_mlp_pes_pts(const std::string &p, const DvdAudioTrack &t) {
    std::unique_ptr<FILE, decltype(&fclose)> f(sacd_detail::open(dvd_physical_path(p)), fclose);
    if (!f)
        throw std::runtime_error("DVD-A MLP: image cannot be opened");
    for (uint32_t sector = t.firstSector; sector <= t.lastSector && sector - t.firstSector < 64; ++sector) {
        uint64_t logical = uint64_t(sector) * 2048, base = 0;
        std::array<uint8_t, 2048> b{};
        bool found = false;
        for (const auto &e : t.aobs) {
            if (logical >= base && logical + 2048 <= base + e.bytes) {
                if (!iso_extent_at(f.get(), e, logical - base, b.data(), b.size()))
                    throw std::runtime_error("DVD-A MLP: short PES sector");
                found = true;
                break;
            }
            base += e.bytes;
        }
        if (!found)
            throw std::runtime_error("DVD-A MLP: PES extent missing");
        for (size_t i = 0; i + 14 <= b.size(); ++i) {
            if (b[i] || b[i + 1] || b[i + 2] != 1 || b[i + 3] != 0xbd)
                continue;
            size_t end = i + 6 + sacd_detail::be16(b.data() + i + 4), payload = i + 9 + b[i + 8];
            if (end > b.size() || payload >= end)
                throw std::runtime_error("DVD-A MLP: malformed PES timestamp packet");
            if (b[payload] != 0xa1)
                continue;
            if (!(b[i + 7] & 0x80) || b[i + 8] < 5)
                throw std::runtime_error(
                    "DVD-A MLP: first audio PES has no PTS; exact track start cannot be determined");
            const uint8_t *ts = b.data() + i + 9;
            if (!(ts[0] & 1) || !(ts[2] & 1) || !(ts[4] & 1))
                throw std::runtime_error("DVD-A MLP: invalid PES PTS marker bits");
            return (uint64_t((ts[0] >> 1) & 7) << 30) | (uint64_t(ts[1]) << 22) |
                   (uint64_t(ts[2] >> 1) << 15) | (uint64_t(ts[3]) << 7) | (ts[4] >> 1);
        }
    }
    throw std::runtime_error("DVD-A MLP: first audio PES PTS not found within 64 sectors");
}
struct DvdAudioInfo {
    int rate = 0, channels = 0, bits = 0;
};
inline DvdAudioInfo dvd_audio_info(const std::string &p) {
    auto t = dvd_audio_select(p);
    if (!t.mlp) {
        DvdLpcmReader r;
        r.open(p);
        return {r.format().rate, r.format().channels, r.format().bits};
    }
    std::unique_ptr<FILE, decltype(&fclose)> f(sacd_detail::open(dvd_physical_path(p)), fclose);
    if (!f)
        throw std::runtime_error("DVD-A: cannot read MLP major sync");
    std::vector<uint8_t> b;
    for (uint32_t sector = t.firstSector; sector <= t.lastSector && sector - t.firstSector < 64; ++sector) {
        uint64_t off = uint64_t(sector) * 2048, base = 0;
        for (auto &e : t.aobs) {
            if (off >= base && off + 2048 <= base + e.bytes) {
                size_t n = b.size();
                b.resize(n + 2048);
                if (!iso_extent_at(f.get(), e, off - base, b.data() + n, 2048))
                    throw std::runtime_error("DVD-A: short MLP header");
                break;
            }
            base += e.bytes;
        }
    }
    static constexpr int counts[] = {1, 2, 3, 4, 3, 4, 5, 3, 4, 5, 4, 5, 6, 4, 5, 4, 5, 6, 5, 5, 6};
    for (size_t i = 0; i + 28 <= b.size(); ++i)
        if (b[i] == 0xf8 && b[i + 1] == 0x72 && b[i + 2] == 0x6f && b[i + 3] == 0xbb) {
            unsigned quant = b[i + 4] >> 4, rate = b[i + 5] >> 4, assignment = b[i + 7] & 31;
            if (quant <= 2 && (rate & 7) <= 2 && assignment <= 20)
                return {int((rate & 8 ? 44100 : 48000) << (rate & 7)), counts[assignment],
                        int(16 + quant * 4)};
        }
    throw std::runtime_error("DVD-A: no valid MLP major sync in first 64 sectors");
}
} // namespace rta::audio
