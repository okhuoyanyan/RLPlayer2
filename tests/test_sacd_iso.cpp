// SPDX-License-Identifier: GPL-3.0-or-later
// Entirely synthetic Scarlet Book sectors. No music or third-party media.
#include "audio/audio_loader.h"
#include <cstdio>
#include <fstream>
#include <random>
using namespace rta::audio;
static int fails = 0;
static void ck(bool b, const char *m) {
    printf("[%s] %s\n", b ? "PASS" : "FAIL", m);
    fails += !b;
}
static void be32(std::vector<uint8_t> &b, size_t p, uint32_t n) {
    for (int i = 3; i >= 0; --i) {
        b[p + i] = uint8_t(n);
        n >>= 8;
    }
}
static void be16(std::vector<uint8_t> &b, size_t p, unsigned n) {
    b[p] = n >> 8;
    b[p + 1] = n;
}
static void id(std::vector<uint8_t> &b, size_t o, const char *s) { memcpy(b.data() + o, s, 8); }
static std::vector<uint8_t> fixture(bool dst = false, bool compressed = false) {
    std::vector<uint8_t> b(1000 * 2048);
    size_t m = 510 * 2048;
    id(b, m, "SACDMTOC");
    b[m + 8] = 1;
    b[m + 9] = 20;
    be32(b, m + 64, 520);
    be16(b, m + 84, 4);
    be32(b, m + 72, 530);
    be16(b, m + 86, 4);
    id(b, 511 * 2048, "SACDText");
    be16(b, 511 * 2048 + 16, 64);
    memcpy(b.data() + 511 * 2048 + 64, "Numeric Synthetic Album", 24);
    m = 510 * 2048;
    b[m + 138] = 1;
    for (int a = 0; a < 2; ++a) {
        size_t toc = (520 + a * 10) * 2048;
        id(b, toc, a ? "MULCHTOC" : "TWOCHTOC");
        b[toc + 8] = 1;
        b[toc + 9] = 20;
        be16(b, toc + 10, 4);
        b[toc + 20] = 4;
        b[toc + 21] = dst ? 0 : 2;
        b[toc + 32] = a ? 6 : 2;
        b[toc + 69] = 2;
        id(b, toc + 2048, "SACDTRL1");
        id(b, toc + 4096, "SACDTRL2");
        id(b, toc + 6144, "SACDTTxt");
        b[toc + 90] = 1;
        for (int textTrack = 0; textTrack < 2; ++textTrack) {
            size_t block = toc + 6144, offset = 64 + textTrack * 100;
            be16(b, block + 8 + textTrack * 2, unsigned(offset));
            b[block + offset] = 1;
            b[block + offset + 4] = 1;
            b[block + offset + 5] = 0x20;
            std::string title = "Synthetic Track " + std::to_string(textTrack + 1);
            memcpy(b.data() + block + offset + 6, title.c_str(), title.size() + 1);
        }
        for (int t = 0; t < 2; ++t) {
            int ch = a ? 6 : 2;
            unsigned start = 550 + a * 150 + t * 60;
            unsigned frames = 3;
            std::vector<uint8_t> pay;
            unsigned nsec = 0;
            for (unsigned f = 0; f < frames; ++f) {
                std::vector<uint8_t> data;
                if (compressed) {
                    size_t bp = 0;
                    auto put = [&](unsigned v, unsigned n) {
                        while (n--) {
                            if (bp / 8 >= data.size())
                                data.push_back(0);
                            data[bp / 8] |= ((v >> n) & 1) << (7 - bp % 8);
                            ++bp;
                        }
                    };
                    put(15, 4);
                    put(1, 1);
                    put(1, 1);
                    for (int c = 0; c < ch; ++c)
                        put(c & 1, 1);
                    put(0, 7);
                    put(0, 1);
                    put(64, 9);
                    put(1, 6);
                    put(0, 1);
                    put(127, 7);
                    put(127, 7);
                    put(0, 1);
                    put(123, 12);
                    std::mt19937 rng(17 + f * 991 + t * 43 + a * 31);
                    for (int j = 0; j < 40000; ++j)
                        put(rng() & 255, 8);
                    size_t used = 0;
                    rta::audio::dst::decode(data, ch, 4704, &used);
                    data.resize((used + 7) / 8);
                } else {
                    if (dst)
                        data.push_back(0);
                    for (size_t i = 0; i < 4704u * ch; ++i)
                        data.push_back(uint8_t(i * 13 + f * 17 + t * 29 + a * 41));
                }
                size_t consumed = 0;
                while (consumed < data.size()) {
                    size_t sec = size_t(start + nsec++) * 2048;
                    size_t n = std::min<size_t>(data.size() - consumed, 2048 - 7);
                    b[sec] = uint8_t(0x20 + (consumed == 0 ? 4 : 0) + (dst ? 1 : 0));
                    size_t hdr = consumed == 0 ? (dst ? 7 : 6) : 3;
                    n = std::min<size_t>(data.size() - consumed, 2048 - hdr);
                    b[sec + 1] = uint8_t((consumed == 0 ? 128 : 0) + 16 + (n >> 8));
                    b[sec + 2] = uint8_t(n);
                    if (consumed == 0) {
                        b[sec + 3] = 0;
                        b[sec + 4] = 0;
                        b[sec + 5] = f + t * 3;
                    }
                    memcpy(b.data() + sec + hdr, data.data() + consumed, n);
                    consumed += n;
                }
            }
            be32(b, toc + 2048 + 8 + t * 4, start);
            be32(b, toc + 2048 + 1028 + t * 4, nsec);
            b[toc + 4096 + 8 + t * 4 + 2] = t * 3;
            b[toc + 4096 + 1028 + t * 4 + 2] = frames;
        }
    }
    return b;
}
static void write(const char *p, const std::vector<uint8_t> &b) {
    std::ofstream f(p, std::ios::binary);
    f.write((const char *)b.data(), b.size());
}

static std::vector<uint8_t> tiny_dst(int coefficient) {
    std::vector<uint8_t> b;
    size_t bit = 0;
    auto put = [&](unsigned v, unsigned n) {
        while (n--) {
            if (bit / 8 >= b.size())
                b.push_back(0);
            b[bit / 8] |= ((v >> n) & 1) << (7 - bit % 8);
            ++bit;
        }
    };
    put(63, 6);
    put(0, 2);
    put(0, 7);
    put(0, 1);
    put(coefficient, 9);
    put(1, 6);
    put(0, 1);
    put(0, 7);
    put(0, 7);
    put(0, 1);
    put(0, 12);
    for (int i = 0; i < 20000; ++i)
        put(255, 8);
    size_t consumed = 0;
    dst::decode(b, 2, 4704, &consumed);
    b.resize((consumed + 7) / 8);
    return b;
}
static void test_shared_sector(const char *file) {
    auto b = fixture(true);
    size_t toc = 520 * 2048;
    b[toc + 69] = 1;
    be32(b, 510 * 2048 + 72, 0);
    be32(b, 510 * 2048 + 76, 0);
    be32(b, toc + 2048 + 8, 550);
    be32(b, toc + 2048 + 1028, 1);
    b[toc + 4096 + 10] = 1;
    b[toc + 4096 + 1030] = 1;
    auto previous = tiny_dst(0), selected = tiny_dst(255), expected = dst::decode(selected, 2, 4704);
    size_t sector = 550 * 2048;
    std::fill(b.begin() + sector, b.begin() + sector + 2048, 0);
    b[sector] = 0x49;
    size_t payload = 13;
    for (int i = 0; i < 2; ++i) {
        auto &frame = i ? selected : previous;
        b[sector + 1 + i * 2] = 0x90 + (frame.size() >> 8);
        b[sector + 2 + i * 2] = frame.size();
        b[sector + 5 + i * 4 + 2] = i;
        std::copy(frame.begin(), frame.end(), b.begin() + sector + payload);
        payload += frame.size();
    }
    write(file, b);
    SacdReader r;
    r.open(file);
    std::vector<std::vector<uint8_t>> out;
    r.read(4704, out);
    bool equal = true;
    for (size_t i = 0; i < 4704; ++i)
        for (int c = 0; c < 2; ++c)
            equal &= out[c][i] == expected[i * 2 + c];
    ck(equal, "shared sector is trimmed by TRL2 start and frame timecode, every selected DSD byte matches");
    r.seek(473);
    r.read(99, out);
    ck(out[0][0] == expected[473 * 2] && out[1].back() == expected[(473 + 98) * 2 + 1],
       "seek inside shared-sector track preserves first/last selected bytes");
    r.seek(4704);
    ck(r.read(1, out) == 0, "shared-sector track ends exactly at duration, no next-track bytes");
    b[sector + 11] = 2;
    write(file, b);
    bool reject = false;
    try {
        SacdReader damaged;
        damaged.open(file);
        damaged.read(4704, out);
    } catch (...) {
        reject = true;
    }
    ck(reject, "missing selected frame timecode is corruption, not substituted previous-track audio");
}
int main(int argc, char **argv) {
    if (argc == 3 && std::string(argv[1]) == "--emit-fixtures") {
        std::filesystem::create_directories(std::filesystem::u8path(argv[2]));
        for (auto name : {"sacd-raw.iso", "sacd-dst.iso"}) {
            auto path = std::filesystem::u8path(argv[2]) / name;
            bool dst = std::string(name).find("dst") != std::string::npos;
            auto bytes = fixture(dst, dst);
            std::ofstream f(path, std::ios::binary);
            f.write((const char *)bytes.data(), bytes.size());
            printf("%s %zu bytes\n", path.string().c_str(), bytes.size());
        }
        return 0;
    }
    const char *file = "synthetic-scarlet.iso";
    auto b = fixture();
    write(file, b);
    write("sacd-valid-synthetic.iso", b);
    auto tracks = sacd_tracks(file);
    bool foreignRejected=false;try{(void)sacd_select(std::string(file)+"#dvda=1:1:1");}catch(...){foreignRejected=true;}
    ck(foreignRejected,"SACD rejects foreign-format selector instead of guessing first track");
    write("name#sacd=physical.iso",b);ck(sacd_select("name#sacd=physical.iso").number==1,"marker inside physical SACD name preserves default selection");std::remove("name#sacd=physical.iso");
    ck(tracks[0].album == "Numeric Synthetic Album" && tracks[0].title == "Synthetic Track 1",
       "bounded SACDText and SACDTTxt metadata decoded");
    ck(tracks.size() == 4, "real Master TOC exposes stereo/multichannel tracks");
    ck(tracks[0].bits() == 3 * 37632 && tracks[0].seconds() == .04, "duration from TRL2 (75 frames/s)");
    ck(tracks[0].path != tracks[1].path && tracks[0].path != tracks[2].path,
       "virtual keys include area and track");
    for (auto &t : tracks) {
        DsdPcmStream pcm;
        std::string why;
        AudioBuffer buf;
        ck(pcm.open(t.path, dsd_pcm_settings(), &why) && pcm.read(0, 113, buf) == 113,
           "SACD virtual key reaches existing PCM stream");
        SacdReader r;
        r.open(t.path);
        std::vector<std::vector<uint8_t>> out;
        ck(r.read(20000, out) == 14112, "exact first/last frame extent");
        bool eq = true;
        int a = t.area == "mch" ? 1 : 0;
        for (int c = 0; c < t.channels; ++c)
            for (size_t i = 0; i < out[c].size(); ++i) {
                size_t f = i / 4704, j = i % 4704;
                eq &= out[c][i] == uint8_t((j * t.channels + c) * 13 + f * 17 + (t.number - 1) * 29 + a * 41);
            }
        ck(eq, "independently generated channel/time formula equals every byte");
        r.seek(4711);
        r.read(33, out);
        ck(out[0][0] == uint8_t((7 * t.channels) * 13 + 17 + (t.number - 1) * 29 + a * 41),
           "backward seek restores exact byte position");
        ck(r.bufferedBytes() < 200000, "reader bounded below 200 KiB");
    }
    std::vector<uint8_t> compressed(16000, 0);
    size_t bp = 0;
    auto put = [&](unsigned v, unsigned n) {
        while (n--) {
            compressed[bp / 8] |= ((v >> n) & 1) << (7 - bp % 8);
            ++bp;
        }
    };
    put(15, 4);
    put(1, 1);
    put(1, 1);
    put(0, 2);
    put(0, 7);
    put(0, 1);
    put(0, 9);
    put(0, 6);
    put(0, 1);
    auto decoded = rta::audio::dst::decode(compressed, 2, 4704);
    ck(decoded.size() == 9408 &&
           std::all_of(decoded.begin(), decoded.end(), [](uint8_t v) { return v == 255; }),
       "compressed DST arithmetic-coded constant fixture reconstructs every bit");
    auto d = fixture(true);
    write(file, d);
    SacdReader dst;
    dst.open(file);
    std::vector<std::vector<uint8_t>> out;
    ck(dst.read(14112, out) == 14112, "DST lossless raw-frame flag reconstructs all DSD bytes");
    b.resize(550 * 2048 + 100);
    write(file, b);
    bool rejected = false;
    try {
        (void)sacd_tracks(file);
    } catch (...) {
        rejected = true;
    }
    ck(rejected, "truncated track extent rejected before decoding");
    std::vector<uint8_t> fake(81920);
    memcpy(fake.data() + 32769, "CD001", 5);
    write(file, fake);
    ck(sacd_tracks(file).empty(), "ISO9660 label cannot impersonate Scarlet Book TOC");
    for (const std::string &bad :
         {"a.iso#sacd=2ch:0", "a.iso#sacd=2ch:-1", "a.iso#sacd=2ch:9999999999999999999", "a.iso#sacd=oops:1",
          "a.iso#sacd=2ch:1#sacd=2ch:2", "a.iso#dvda=1:0:1"}) {
        bool reject = false;
        try {
            image_track_key(bad);
        } catch (...) {
            reject = true;
        }
        ck(reject, "malformed/overflow/zero/repeated image key rejected");
    }
    ck(sacd_physical_path("C:/music/a#part1.iso#sacd=2ch:1") == "C:/music/a#part1.iso",
       "ordinary hash in physical name is preserved");
    ck(sacd_physical_path("C:/music/a#sacd=name.iso") == "C:/music/a#sacd=name.iso",
       "marker inside physical file name is preserved");
    test_shared_sector(file);
    std::remove(file);
    printf("FAILURES=%d\n", fails);
    return fails ? 1 : 0;
}
