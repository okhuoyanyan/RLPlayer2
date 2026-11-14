/*
 * Direct Stream Transfer (DST) raw DSD decoder.
 * Adapted from FFmpeg n7.1.1 libavcodec/dstdec.c, Copyright (c) 2014
 * Peter Ross <pross@xvid.org>. GNU Lesser General Public License 2.1 or later.
 * Full upstream source retained in third_party/sacd-reference/dstdec-reference.c.
 * RLPlayer adaptation returns the lossless DSD bytes before FFmpeg's PCM FIR.
 * Segmentation/mapping extensions are original RLPlayer code based on ISO syntax.
 * Segmented and channel-dependent filter/probability maps are supported.
 */
// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <vector>
namespace rta::audio::dst {
constexpr int DST_MAX_CHANNELS = 6, DST_MAX_ELEMENTS = 12;
constexpr int AVERROR_INVALIDDATA = -1;
inline int av_log2(unsigned v) {
    if (!v)
        throw std::runtime_error("DST: arithmetic range underflow");
    int n = 0;
    while (v >>= 1)
        ++n;
    return n;
}
struct GetBitContext {
    const uint8_t *p;
    size_t bits, pos = 0;
    bool arithmetic = false;
};
inline unsigned get_bits(GetBitContext *g, unsigned n) {
    if (n > 31 || g->pos + n > g->bits + (g->arithmetic ? 16 : 0))
        throw std::runtime_error("DST: truncated bitstream");
    unsigned v = 0;
    while (n--) {
        v <<= 1;
        if (g->pos < g->bits)
            v |= (g->p[g->pos / 8] >> (7 - g->pos % 8)) & 1;
        ++g->pos;
    }
    return v;
}
inline unsigned get_bits1(GetBitContext *g) { return get_bits(g, 1); }
inline int get_sbits(GetBitContext *g, unsigned n) {
    unsigned v = get_bits(g, n);
    return (int)(v ^ (1u << (n - 1))) - (int)(1u << (n - 1));
}
inline int get_bits_left(GetBitContext *g) { return int(g->bits - g->pos); }
inline unsigned get_ur_golomb_jpegls(GetBitContext *g, unsigned k, int, int) {
    unsigned q = 0;
    while (!get_bits1(g)) {
        if (++q > 65536)
            throw std::runtime_error("DST: invalid Rice coefficient");
    }
    return (q << k) | get_bits(g, k);
}
inline int av_clip(int x, int l, int h) { return std::clamp(x, l, h); }
inline uint8_t reverse(unsigned x) {
    uint8_t v = 0;
    for (int i = 0; i < 8; ++i) {
        v = uint8_t((v << 1) | (x & 1));
        x >>= 1;
    }
    return v;
}
inline std::array<uint8_t, 256> make_reverse() {
    std::array<uint8_t, 256> a{};
    for (unsigned i = 0; i < 256; ++i)
        a[i] = reverse(i);
    return a;
}
inline const auto ff_reverse = make_reverse();
static constexpr int8_t fsets_code_pred_coeff[3][3] = {{-8}, {-16, 8}, {-9, -5, 6}};
static constexpr int8_t probs_code_pred_coeff[3][3] = {{-8}, {-16, 8}, {-24, 24, -8}};
struct ArithCoder {
    unsigned a = 0, c = 0;
};
struct Table {
    unsigned elements = 0;
    unsigned length[DST_MAX_ELEMENTS]{};
    int coeff[DST_MAX_ELEMENTS][128]{};
};
static inline int get_sr_golomb_dst(GetBitContext *gb, unsigned int k) {
    int v = get_ur_golomb_jpegls(gb, k, get_bits_left(gb), 0);
    if (v && get_bits1(gb))
        v = -v;
    return v;
}

static void read_uncoded_coeff(GetBitContext *gb, int *dst, unsigned int elements, int coeff_bits,
                               int is_signed, int offset) {
    int i;

    for (i = 0; i < elements; i++) {
        dst[i] = (is_signed ? get_sbits(gb, coeff_bits) : get_bits(gb, coeff_bits)) + offset;
    }
}

static int read_table(GetBitContext *gb, Table *t, const int8_t code_pred_coeff[3][3], int length_bits,
                      int coeff_bits, int is_signed, int offset) {
    unsigned int i, j, k;
    for (i = 0; i < t->elements; i++) {
        t->length[i] = get_bits(gb, length_bits) + 1;
        if (!is_signed && t->length[i] == 1) {
            t->coeff[i][0] = 128;
            continue;
        }
        if (!get_bits1(gb)) {
            read_uncoded_coeff(gb, t->coeff[i], t->length[i], coeff_bits, is_signed, offset);
        } else {
            int method = get_bits(gb, 2), lsb_size;
            if (method == 3 || unsigned(method + 1) >= t->length[i])
                return AVERROR_INVALIDDATA;

            read_uncoded_coeff(gb, t->coeff[i], method + 1, coeff_bits, is_signed, offset);

            lsb_size = get_bits(gb, 3);
            for (j = method + 1; j < t->length[i]; j++) {
                int c, x = 0;
                for (k = 0; k < method + 1; k++)
                    x += code_pred_coeff[method][k] * (unsigned)t->coeff[i][j - k - 1];
                c = get_sr_golomb_dst(gb, lsb_size);
                if (x >= 0)
                    c -= (x + 4) / 8;
                else
                    c += (-x + 3) / 8;
                if (is_signed && (c < -256 || c > 255))
                    return AVERROR_INVALIDDATA;
                if (!is_signed) {
                    if (c < offset || c >= offset + (1 << coeff_bits))
                        return AVERROR_INVALIDDATA;
                }
                t->coeff[i][j] = c;
            }
        }
    }
    return 0;
}

static void ac_init(ArithCoder *ac, GetBitContext *gb) {
    ac->a = 4095;
    ac->c = get_bits(gb, 12);
}

static inline void ac_get(ArithCoder *ac, GetBitContext *gb, int p, int *e) {
    unsigned int k = (ac->a >> 8) | ((ac->a >> 7) & 1);
    unsigned int q = k * p;
    unsigned int a_q = ac->a - q;

    *e = ac->c < a_q;
    if (*e) {
        ac->a = a_q;
    } else {
        ac->a = q;
        ac->c -= a_q;
    }

    if (ac->a < 2048) {
        int n = 11 - av_log2(ac->a);
        ac->a <<= n;
        ac->c = (ac->c << n) | get_bits(gb, n);
    }
}

static uint8_t prob_dst_x_bit(int c) { return (ff_reverse[c & 127] >> 1) + 1; }

static int build_filter(int16_t table[DST_MAX_ELEMENTS][16][256], const Table *fsets) {
    int i, j, k, l;

    for (i = 0; i < fsets->elements; i++) {
        int length = fsets->length[i];

        for (j = 0; j < 16; j++) {
            int total = av_clip(length - j * 8, 0, 8);

            for (k = 0; k < 256; k++) {
                int64_t v = 0;

                for (l = 0; l < total; l++)
                    v += (((k >> l) & 1) * 2 - 1) * fsets->coeff[i][j * 8 + l];
                if ((int16_t)v != v)
                    return AVERROR_INVALIDDATA;
                table[i][j][k] = v;
            }
        }
    }
    return 0;
}

// ISO/IEC 14496-3 DST syntax facts: filter segments >=1024 bits (max 4),
// probability segments >=32 bits (max 8); segment units are resolution*8.
// Original RLPlayer implementation uses cumulative sample endpoints, so no
// per-sample or file-sized mapping buffer is ever allocated.
struct Region {
    unsigned end = 0, table = 0;
};
using Regions = std::vector<std::vector<Region>>;
inline unsigned fieldWidth(unsigned upper) {
    unsigned n = 0;
    do {
        ++n;
        upper >>= 1;
    } while (upper);
    return n;
}
inline Regions segmentation(GetBitContext *bits, int channels, unsigned minimum, unsigned maximum) {
    const unsigned frameBits = 4704 * 8;
    bool common = get_bits1(bits);
    Regions all(channels);
    unsigned resolution = 0;
    for (int c = 0; c < (common ? 1 : channels); ++c) {
        unsigned used = 0;
        while (!get_bits1(bits)) {
            if (all[c].size() + 1 >= maximum)
                throw std::runtime_error("DST: too many segments");
            if (!resolution) {
                resolution = get_bits(bits, fieldWidth(4704 - minimum / 8));
                if (!resolution || resolution > 4704 - minimum / 8)
                    throw std::runtime_error("DST: invalid segment resolution");
            }
            unsigned limit = (4704 - minimum / 8 - used / 8) / resolution;
            unsigned units = get_bits(bits, fieldWidth(limit));
            uint64_t span = uint64_t(units) * resolution * 8;
            if (span < minimum || span > frameBits - used - minimum)
                throw std::runtime_error("DST: segment length outside frame");
            used += unsigned(span);
            all[c].push_back({used, 0});
        }
        all[c].push_back({frameBits, 0});
    }
    if (common)
        for (int c = 1; c < channels; ++c)
            all[c] = all[0];
    return all;
}
inline unsigned mapping(GetBitContext *bits, Regions &r, int channels) {
    bool common = get_bits1(bits);
    unsigned count = 1;
    for (int c = 0; c < (common ? 1 : channels); ++c)
        for (size_t j = 0; j < r[c].size(); ++j) {
            if (!c && !j)
                continue;
            unsigned t = get_bits(bits, fieldWidth(count));
            if (t > count)
                throw std::runtime_error("DST: table referenced before definition");
            if (t == count)
                ++count;
            if (count > unsigned(channels * 2))
                throw std::runtime_error("DST: too many coefficient tables");
            r[c][j].table = t;
        }
    if (common)
        for (int c = 1; c < channels; ++c) {
            if (r[c].size() != r[0].size())
                throw std::runtime_error("DST: shared map has different segment counts");
            for (size_t j = 0; j < r[c].size(); ++j)
                r[c][j].table = r[0][j].table;
        }
    return count;
}
inline std::vector<uint8_t> decode(const std::vector<uint8_t> &packet, int channels, size_t bytesPerChannel,
                                   size_t *consumedBits = nullptr) {
    if (channels < 1 || channels > 6 || bytesPerChannel != 4704 || packet.size() < 2 || packet.size() > 65536)
        throw std::runtime_error("DST: invalid frame parameters");
    GetBitContext bits{packet.data(), packet.size() * 8};
    auto *gb = &bits;
    if (!get_bits1(gb)) {
        get_bits1(gb);
        if (get_bits(gb, 6) || packet.size() != 1 + bytesPerChannel * channels)
            throw std::runtime_error("DST: truncated/unrecognized raw frame");
        return {packet.begin() + 1, packet.end()};
    }
    bool sharedSeg = get_bits1(gb);
    Regions fs = segmentation(gb, channels, 1024, 4), ps = sharedSeg ? fs : segmentation(gb, channels, 32, 8);
    unsigned half[6]{};
    Table f, p;
    bool same = get_bits1(gb);
    f.elements = mapping(gb, fs, channels);
    if (same) {
        p.elements = f.elements;
        for (int ch = 0; ch < channels; ++ch) {
            if (fs[ch].size() != ps[ch].size())
                throw std::runtime_error("DST: reused map has unequal segment counts");
            for (size_t j = 0; j < fs[ch].size(); ++j)
                ps[ch][j].table = fs[ch][j].table;
        }
    } else
        p.elements = mapping(gb, ps, channels);
    for (int ch = 0; ch < channels; ++ch)
        half[ch] = get_bits1(gb);
    if (read_table(gb, &f, fsets_code_pred_coeff, 7, 9, 1, 0) < 0 ||
        read_table(gb, &p, probs_code_pred_coeff, 6, 7, 0, 1) < 0)
        throw std::runtime_error("DST: invalid coefficient table");
    if (get_bits1(gb))
        throw std::runtime_error("DST: reserved arithmetic bit set");
    struct Filter {
        int16_t data[DST_MAX_ELEMENTS][16][256];
    };
    auto filter = std::make_unique<Filter>();
    if (build_filter(filter->data, &f) < 0)
        throw std::runtime_error("DST: filter coefficient overflow");
    gb->arithmetic = true;
    ArithCoder ac;
    ac_init(&ac, gb);
    int x;
    ac_get(&ac, gb, prob_dst_x_bit(f.coeff[0][0]), &x);
    uint8_t status[6][16];
    memset(status, 0xAA, sizeof status);
    std::vector<uint8_t> out(bytesPerChannel * channels, 0);
    for (size_t i = 0; i < bytesPerChannel * 8; ++i)
        for (int ch = 0; ch < channels; ++ch) {
            int sum = 0;
            unsigned fe = 0, pe = 0;
            for (const auto &r : fs[ch])
                if (i < r.end) {
                    fe = r.table;
                    break;
                }
            for (const auto &r : ps[ch])
                if (i < r.end) {
                    pe = r.table;
                    break;
                }
            for (int j = 0; j < 16; ++j)
                sum += filter->data[fe][j][status[ch][j]];
            int16_t predict = (int16_t)sum;
            int probability = 128;
            if (!half[ch] || i >= f.length[fs[ch][0].table])
                probability =
                    p.coeff[pe][std::min<unsigned>(unsigned(std::abs(int(predict))) >> 3, p.length[pe] - 1)];
            int residual;
            ac_get(&ac, gb, probability, &residual);
            unsigned v = ((predict >> 15) ^ residual) & 1;
            out[(i / 8) * channels + ch] |= uint8_t(v << (7 - i % 8));
            for (int j = 15; j >= 0; --j)
                status[ch][j] = uint8_t((status[ch][j] << 1) | (j ? status[ch][j - 1] >> 7 : v));
        }
    if (consumedBits)
        *consumedBits = gb->pos;
    return out;
}
} // namespace rta::audio::dst
