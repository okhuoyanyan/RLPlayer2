// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "audio/sacd_reader.h"
namespace rta::audio {
class DstDffReader {
    std::unique_ptr<FILE, decltype(&fclose)> file_{nullptr, fclose};
    uint64_t begin_ = 0, end_ = 0, next_ = 0, pos_ = 0;
    uint32_t frames_ = 0, rate_ = 0;
    int channels_ = 0;
    std::vector<uint8_t> frame_;
    size_t index_ = 0;
    static uint64_t be64(const uint8_t *p) {
        return uint64_t(sacd_detail::be32(p)) << 32 | sacd_detail::be32(p + 4);
    }
    bool header(uint64_t off, uint8_t *h) { return sacd_detail::at(file_.get(), off, h, 12); }

  public:
    bool open(const std::string &p) {
        file_.reset(sacd_detail::open(p));
        if (!file_)
            return false;
        uint8_t h[16];
        auto length = sacd_detail::length(file_.get());
        if (length < 16 || !sacd_detail::at(file_.get(), 0, h, 16) || memcmp(h, "FRM8", 4))
            return false;
        uint64_t end = be64(h + 4);
        if (end > length - 12)
            throw std::runtime_error("DFF: truncated FRM8");
        end += 12;
        bool compressed = false;
        uint64_t dstStart = 0, dstEnd = 0;
        for (uint64_t off = 16; off + 12 <= end;) {
            if (!header(off, h))
                throw std::runtime_error("DFF: truncated chunk header");
            uint64_t size = be64(h + 4);
            if (size > end - off - 12)
                throw std::runtime_error("DFF: chunk exceeds FRM8");
            if (!memcmp(h, "PROP", 4) && size <= 65536) {
                std::vector<uint8_t> b(size);
                if (!sacd_detail::at(file_.get(), off + 12, b.data(), b.size()))
                    throw std::runtime_error("DFF: truncated PROP");
                for (size_t i = 4; i + 12 <= b.size();) {
                    auto n = be64(b.data() + i + 4);
                    if (n > b.size() - i - 12)
                        throw std::runtime_error("DFF: malformed PROP subchunk");
                    if (!memcmp(b.data() + i, "FS  ", 4) && n >= 4)
                        rate_ = sacd_detail::be32(b.data() + i + 12);
                    if (!memcmp(b.data() + i, "CHNL", 4) && n >= 2)
                        channels_ = sacd_detail::be16(b.data() + i + 12);
                    if (!memcmp(b.data() + i, "CMPR", 4) && n >= 4)
                        compressed = !memcmp(b.data() + i + 12, "DST ", 4);
                    i += 12 + size_t(n) + (n & 1);
                }
            }
            if (!memcmp(h, "DST ", 4)) {
                dstStart = off + 12;
                dstEnd = dstStart + size;
            }
            off += 12 + size + (size & 1);
        }
        if (!compressed && !dstStart) {
            file_.reset();
            return false;
        }
        if (!dstStart || rate_ != 2822400 || channels_ < 1 || channels_ > 6)
            throw std::runtime_error("DST DFF: missing DST data or unsupported sample rate/channels");
        begin_ = dstStart;
        end_ = dstEnd;
        for (uint64_t off = begin_; off + 12 <= end_;) {
            if (!header(off, h))
                throw std::runtime_error("DST DFF: short subchunk");
            uint64_t n = be64(h + 4);
            if (n > end_ - off - 12)
                throw std::runtime_error("DST DFF: subchunk exceeds DST container");
            if (!memcmp(h, "FRTE", 4) && n == 6) {
                uint8_t info[6];
                if (!sacd_detail::at(file_.get(), off + 12, info, 6) || sacd_detail::be16(info + 4) != 75)
                    throw std::runtime_error("DST DFF: invalid frame rate");
                frames_ = sacd_detail::be32(info);
            }
            off += 12 + n + (n & 1);
        }
        if (!frames_)
            throw std::runtime_error("DST DFF: missing FRTE frame count");
        rewind();
        return true;
    }
    int channels() const { return channels_; }
    uint32_t rate() const { return rate_; }
    uint64_t bits() const { return uint64_t(frames_) * 37632; }
    void rewind() {
        next_ = begin_;
        pos_ = 0;
        frame_.clear();
        index_ = 0;
    }
    size_t read(size_t count, std::vector<std::vector<uint8_t>> &out) {
        out.assign(channels_, {});
        size_t done = 0;
        while (done < count && pos_ < uint64_t(frames_) * 4704) {
            if (index_ >= frame_.size()) {
                frame_.clear();
                uint8_t h[12];
                while (next_ + 12 <= end_) {
                    if (!header(next_, h))
                        throw std::runtime_error("DST DFF: truncated frame header");
                    uint64_t n = be64(h + 4), body = next_ + 12;
                    if (n > end_ - body)
                        throw std::runtime_error("DST DFF: truncated frame");
                    next_ = body + n + (n & 1);
                    if (!memcmp(h, "DSTF", 4)) {
                        if (n > 65536)
                            throw std::runtime_error("DST DFF: frame exceeds 64 KiB");
                        std::vector<uint8_t> b(n);
                        if (!sacd_detail::at(file_.get(), body, b.data(), b.size()))
                            throw std::runtime_error("DST DFF: short frame");
                        frame_ = dst::decode(b, channels_, 4704);
                        break;
                    }
                }
                index_ = 0;
                if (frame_.empty())
                    throw std::runtime_error("DST DFF: fewer frames than FRTE declares");
            }
            size_t n = std::min<size_t>({count - done, (frame_.size() - index_) / channels_,
                                         size_t(uint64_t(frames_) * 4704 - pos_)});
            for (size_t i = 0; i < n; ++i)
                for (int c = 0; c < channels_; ++c)
                    out[c].push_back(frame_[index_ + i * channels_ + c]);
            index_ += n * channels_;
            done += n;
            pos_ += n;
        }
        return done;
    }
    void seek(uint64_t byte) {
        if (byte > uint64_t(frames_) * 4704)
            throw std::runtime_error("DST DFF: seek outside track");
        if (byte < pos_)
            rewind();
        std::vector<std::vector<uint8_t>> scratch;
        while (pos_ < byte)
            read(size_t(std::min<uint64_t>(4704, byte - pos_)), scratch);
    }
};
} // namespace rta::audio
