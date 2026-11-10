// SPDX-License-Identifier: GPL-3.0-or-later
// Read-only ECMA-167 / UDF 1.02/1.50 physical-partition navigation.
// Descriptor offsets cross-checked with libdvdread dvd_udf.c (GPL-2.0-or-later),
// Christian Wolff (1999), Billy Biggs and Bjorn Englund; reference/notice retained.
#pragma once
#include "audio/sacd_reader.h"
#include <limits>
#include <map>
#include <set>
namespace rta::audio {
struct IsoSpan {
    uint64_t offset = 0, bytes = 0;
    bool zero = false;
    uint64_t logicalByte = 0;
};
struct IsoExtent {
    uint32_t sector = 0;
    uint64_t bytes = 0;
    std::vector<IsoSpan> spans;
    std::vector<uint8_t> embedded;
    bool udf = false;
    uint32_t embeddedLocation = 0;
};
inline bool iso_extent_at(FILE *f, const IsoExtent &e, uint64_t off, void *buffer, size_t count) {
    if (off > e.bytes || count > e.bytes - off)
        return false;
    auto *out = (uint8_t *)buffer;
    if (!e.embedded.empty()) {
        if (off + count > e.embedded.size())
            return false;
        memcpy(out, e.embedded.data() + off, count);
        return true;
    }
    if (e.spans.empty())
        return sacd_detail::at(f, uint64_t(e.sector) * 2048 + off, out, count);
    uint64_t base = 0;
    size_t done = 0;
    for (const auto &s : e.spans) {
        if (off >= base + s.bytes) {
            base += s.bytes;
            continue;
        }
        uint64_t local = off > base ? off - base : 0;
        size_t n = size_t(std::min<uint64_t>(count - done, s.bytes - local));
        if (s.zero)
            memset(out + done, 0, n);
        else if (!sacd_detail::at(f, s.offset + local, out + done, n))
            return false;
        done += n;
        off += n;
        base += s.bytes;
        if (done == count)
            return true;
    }
    return done == count;
}
class UdfNavigation {
    FILE *file_ = nullptr;
    uint64_t bytes_ = 0;
    struct Partition {
        uint32_t start = 0, blocks = 0;
    };
    std::map<unsigned, Partition> partitions_;
    std::vector<unsigned> maps_;
    std::set<uint64_t> allocationVisited_;
    uint64_t allocationBudget_ = 0;
    static uint16_t le16(const uint8_t *p) { return uint16_t(p[0]) | uint16_t(p[1]) << 8; }
    static uint32_t le32(const uint8_t *p) { return uint32_t(le16(p)) | uint32_t(le16(p + 2)) << 16; }
    static uint64_t le64(const uint8_t *p) { return uint64_t(le32(p)) | uint64_t(le32(p + 4)) << 32; }
    static uint16_t crc(const uint8_t *p, size_t n) {
        uint16_t c = 0;
        while (n--) {
            c ^= uint16_t(*p++) << 8;
            for (int i = 0; i < 8; ++i)
                c = uint16_t((c << 1) ^ ((c & 0x8000) ? 0x1021 : 0));
        }
        return c;
    }
    static unsigned tag(const uint8_t *p, size_t size, uint32_t expected = 0xffffffffu) {
        if (size < 16)
            throw std::runtime_error("UDF: truncated descriptor tag");
        unsigned checksum = 0;
        for (int i = 0; i < 16; ++i)
            if (i != 4)
                checksum += p[i];
        unsigned version = le16(p + 2), length = le16(p + 10);
        if (uint8_t(checksum) != p[4] || (version != 2 && version != 3) || length > size - 16 ||
            crc(p + 16, length) != le16(p + 8) || (expected != 0xffffffffu && le32(p + 12) != expected))
            throw std::runtime_error("UDF: descriptor checksum/CRC/location invalid");
        return le16(p);
    }
    std::array<uint8_t, 2048> block(uint64_t physical, uint32_t location) {
        if (physical >= bytes_ / 2048)
            throw std::runtime_error("UDF: block outside image");
        std::array<uint8_t, 2048> b{};
        if (!sacd_detail::at(file_, physical * 2048, b.data(), b.size()))
            throw std::runtime_error("UDF: short block");
        tag(b.data(), b.size(), location);
        return b;
    }
    uint64_t physical(unsigned reference, uint32_t logical, uint64_t length) {
        if (reference >= maps_.size() || !partitions_.count(maps_[reference]))
            throw std::runtime_error("UDF: undefined partition reference");
        auto p = partitions_.at(maps_[reference]);
        if (logical >= p.blocks || length > uint64_t(p.blocks - logical) * 2048)
            throw std::runtime_error("UDF: extent outside partition");
        uint64_t offset = (uint64_t(p.start) + logical) * 2048;
        if (offset > bytes_ || length > bytes_ - offset)
            throw std::runtime_error("UDF: partition extent outside image");
        return offset;
    }

    void allocations(IsoExtent &e, const uint8_t *p, size_t size, unsigned mode, unsigned partition,
                     int depth) {
        if (depth > 8)
            throw std::runtime_error("UDF: allocation continuation depth exceeded");
        unsigned stride = mode == 0 ? 8 : mode == 1 ? 16 : 20;
        if (size % stride)
            throw std::runtime_error("UDF: incomplete allocation descriptor");
        for (size_t i = 0; i < size; i += stride) {
            uint32_t raw = le32(p + i), kind = raw >> 30, length = raw & 0x3fffffffu;
            if (!length)
                break;
            unsigned part = mode == 0 ? partition : le16(p + i + (mode == 1 ? 8 : 16));
            uint32_t lbn = le32(p + i + (mode == 2 ? 12 : 4));
            if (kind == 3) {
                if (length < 24 || length > 1048576 || allocationBudget_ + length > 4194304)
                    throw std::runtime_error("UDF: allocation continuation budget exceeded");
                uint64_t address = physical(part, lbn, length);
                if (!allocationVisited_.insert(address).second)
                    throw std::runtime_error("UDF: cyclic allocation continuation");
                allocationBudget_ += length;
                std::vector<uint8_t> b(length);
                if (!sacd_detail::at(file_, address, b.data(), b.size()) ||
                    tag(b.data(), b.size(), lbn) != 258)
                    throw std::runtime_error("UDF: bad allocation extent descriptor");
                unsigned n = le32(b.data() + 20);
                if (n > b.size() - 24)
                    throw std::runtime_error("UDF: allocation extent length exceeds container");
                allocations(e, b.data() + 24, n, mode, part, depth + 1);
                break;
            }
            uint64_t information = mode == 2 ? le32(p + i + 8) : length,
                     recorded = mode == 2 ? le32(p + i + 4) : length;
            if (information > length || recorded > length || (kind != 0 && mode == 2 && recorded))
                throw std::runtime_error("UDF: extended allocation lengths disagree");
            if (kind == 2) {
                e.spans.push_back({0, information, true, 0});
            } else {
                uint64_t address = physical(part, lbn, length),
                         valid = kind == 0 ? std::min(recorded, information) : 0;
                if (valid)
                    e.spans.push_back({address, valid, false, uint64_t(lbn) * 2048});
                if (information > valid)
                    e.spans.push_back({0, information - valid, true, uint64_t(lbn) * 2048 + valid});
            }
            if (e.spans.size() > 65536)
                throw std::runtime_error("UDF: excessive extent fragmentation");
        }
    }
    uint32_t directoryLocation(const IsoExtent &e, uint64_t offset) {
        if (!e.embedded.empty())
            return e.embeddedLocation;
        uint64_t base = 0;
        for (auto &s : e.spans) {
            if (offset < base + s.bytes) {
                if (s.zero)
                    throw std::runtime_error("UDF: file identifier in sparse directory hole");
                return uint32_t((s.logicalByte + offset - base) / 2048);
            }
            base += s.bytes;
        }
        throw std::runtime_error("UDF: file identifier location outside allocation");
    }
    IsoExtent entry(unsigned partition, uint32_t logical) {
        auto data = block(physical(partition, logical, 2048) / 2048, logical);
        unsigned id = le16(data.data());
        if (id != 261 && id != 266)
            throw std::runtime_error("UDF: ICB is not a file entry");
        uint64_t size = le64(data.data() + 56);
        unsigned ea = le32(data.data() + (id == 261 ? 168 : 208)),
                 ads = le32(data.data() + (id == 261 ? 172 : 212)), begin = (id == 261 ? 176 : 216);
        if (ea > 2048 - begin || ads > 2048 - begin - ea)
            throw std::runtime_error("UDF: file entry allocation area overflows block");
        IsoExtent e;
        e.bytes = size;
        e.udf = true;
        e.embeddedLocation = logical;
        allocationVisited_.clear();
        allocationBudget_ = 0;
        unsigned mode = le16(data.data() + 34) & 7;
        if (mode == 3) {
            if (size > ads)
                throw std::runtime_error("UDF: embedded information length exceeds allocation");
            e.embedded.assign(data.begin() + begin + ea, data.begin() + begin + ea + size);
            return e;
        }
        if (mode > 2)
            throw std::runtime_error("UDF: invalid allocation descriptor type");
        allocations(e, data.data() + begin + ea, ads, mode, partition, 0);
        uint64_t total = 0;
        for (auto &s : e.spans) {
            if (total > std::numeric_limits<uint64_t>::max() - s.bytes)
                throw std::runtime_error("UDF: allocation sum overflow");
            total += s.bytes;
        }
        if (total < size)
            throw std::runtime_error("UDF: file information exceeds allocated extents");
        return e;
    }
    static std::string name(const uint8_t *p, size_t n) {
        if (!n)
            return {};
        unsigned encoding = p[0];
        if (encoding != 8 && encoding != 16)
            throw std::runtime_error("UDF: unsupported CS0 string compression");
        std::string s;
        for (size_t i = 1; i < n;) {
            unsigned c = p[i++];
            if (encoding == 16) {
                if (i >= n)
                    throw std::runtime_error("UDF: odd UTF16 file identifier");
                c = (c << 8) | p[i++];
                if (c >= 0xd800 && c <= 0xdfff)
                    throw std::runtime_error("UDF: invalid surrogate in file identifier");
            }
            if (!c || c == '/' || c == '\\')
                throw std::runtime_error("UDF: invalid file identifier");
            if (c < 128)
                s.push_back((char)std::toupper(c));
            else if (c < 2048) {
                s.push_back(char(0xc0 | (c >> 6)));
                s.push_back(char(0x80 | (c & 63)));
            } else {
                s.push_back(char(0xe0 | (c >> 12)));
                s.push_back(char(0x80 | ((c >> 6) & 63)));
                s.push_back(char(0x80 | (c & 63)));
            }
        }
        return s;
    }

  public:
    void attach(FILE *f, uint64_t bytes) {
        file_ = f;
        bytes_ = bytes;
        partitions_.clear();
        maps_.clear();
    }
    IsoExtent root() {
        std::array<uint8_t, 2048> anchor{};
        bool found = false;
        for (uint64_t lba :
             {256ull, bytes_ / 2048 > 256 ? bytes_ / 2048 - 257 : 0, bytes_ / 2048 ? bytes_ / 2048 - 1 : 0}) {
            if (!lba)
                continue;
            try {
                auto b = block(lba, uint32_t(lba));
                if (le16(b.data()) == 2) {
                    anchor = b;
                    found = true;
                    break;
                }
            } catch (const std::exception &) {
            }
        }
        if (!found)
            throw std::runtime_error("UDF: no valid anchor at standard locations");
        for (int sequence = 0; sequence < 2; ++sequence) {
            try {
                partitions_.clear();
                maps_.clear();
                unsigned length = le32(anchor.data() + 16 + sequence * 8),
                         start = le32(anchor.data() + 20 + sequence * 8);
                if (length < 2048 || length > 8388608)
                    throw std::runtime_error("UDF: volume sequence budget invalid");
                std::array<uint8_t, 2048> lvd{};
                bool have = false;
                for (unsigned i = 0; i < length / 2048; ++i) {
                    auto b = block(uint64_t(start) + i, start + i);
                    unsigned id = le16(b.data());
                    if (id == 5) {
                        unsigned number = le16(b.data() + 22);
                        Partition p{le32(b.data() + 188), le32(b.data() + 192)};
                        if (!p.blocks || uint64_t(p.start) + p.blocks > bytes_ / 2048)
                            throw std::runtime_error("UDF: partition outside image");
                        partitions_[number] = p;
                    }
                    if (id == 6) {
                        lvd = b;
                        have = true;
                    }
                    if (id == 8)
                        break;
                }
                if (!have || le32(lvd.data() + 212) != 2048)
                    throw std::runtime_error("UDF: missing logical volume / unsupported block size");
                unsigned mapBytes = le32(lvd.data() + 264), count = le32(lvd.data() + 268);
                if (mapBytes > 1608 || count > 256)
                    throw std::runtime_error("UDF: partition map budget invalid");
                size_t at = 440;
                for (unsigned i = 0; i < count; ++i) {
                    if (at + 2 > 440 + mapBytes)
                        throw std::runtime_error("UDF: partition map truncated");
                    unsigned type = lvd[at], size = lvd[at + 1];
                    if (type != 1 || size != 6 || at + size > 440 + mapBytes)
                        throw std::runtime_error(
                            "UDF: nonphysical partition map is unsupported for DVD-Audio");
                    maps_.push_back(le16(lvd.data() + at + 4));
                    at += size;
                }
                unsigned fsdBlock = le32(lvd.data() + 252), fsdPart = le16(lvd.data() + 256);
                auto fsd = block(physical(fsdPart, fsdBlock, 2048) / 2048, fsdBlock);
                if (le16(fsd.data()) != 256)
                    throw std::runtime_error("UDF: no file set descriptor");
                return entry(le16(fsd.data() + 408), le32(fsd.data() + 404));
            } catch (const std::exception &) {
                if (sequence)
                    throw;
            }
        }
        throw std::runtime_error("UDF: invalid volume sequences");
    }
    std::map<std::string, IsoExtent> directory(const IsoExtent &e) {
        if (e.bytes > 1048576)
            throw std::runtime_error("UDF: directory exceeds 1 MiB budget");
        std::vector<uint8_t> b(size_t(e.bytes));
        if (!iso_extent_at(file_, e, 0, b.data(), b.size()))
            throw std::runtime_error("UDF: directory truncated");
        std::map<std::string, IsoExtent> out;
        for (size_t off = 0; off < b.size();) {
            if (b.size() - off < 38)
                throw std::runtime_error("UDF: truncated file identifier");
            const auto *p = b.data() + off;
            unsigned fid = p[19], impl = le16(p + 36);
            size_t length = (38 + fid + impl + 3) & ~size_t(3);
            if (length > b.size() - off || tag(p, length, directoryLocation(e, off)) != 257)
                throw std::runtime_error("UDF: invalid file identifier descriptor");
            if (!(p[18] & 12) && fid) {
                auto n = name(p + 38 + impl, fid);
                auto child = entry(le16(p + 28), le32(p + 24));
                if (!out.emplace(n, std::move(child)).second)
                    throw std::runtime_error("UDF: duplicate file identifier");
            }
            off += length;
        }
        return out;
    }
};
} // namespace rta::audio
