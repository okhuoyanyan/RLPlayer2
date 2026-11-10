// SPDX-License-Identifier: GPL-3.0-or-later
#include "audio/audio_loader.h"
#include <fstream>
#include <random>
using namespace rta::audio;
struct Bits {
    std::vector<uint8_t> b;
    size_t pos = 0;
    void put(unsigned v, unsigned n) {
        while (n--) {
            if (pos / 8 >= b.size())
                b.push_back(0);
            b[pos / 8] |= ((v >> n) & 1) << (7 - pos % 8);
            ++pos;
        }
    }
};
static std::vector<uint8_t> frame(int ch, bool segments, unsigned seed) {
    Bits b;
    b.put(1, 1);
    b.put(1, 1);
    b.put(1, 1);
    b.put(segments ? 0 : 1, 1);
    if (segments) {
        b.put(128, 13);
        b.put(1, 6);
        b.put(1, 1);
    }
    b.put(1, 1);
    b.put(1, 1);
    if (segments)
        b.put(1, 1);
    for (int c = 0; c < ch; ++c)
        b.put(c & 1, 1);
    for (int t = 0; t < (segments ? 2 : 1); ++t) {
        b.put(2, 7);
        b.put(0, 1);
        b.put(24, 9);
        b.put(512 - 12, 9);
        b.put(6, 9);
    }
    for (int t = 0; t < (segments ? 2 : 1); ++t) {
        b.put(1, 6);
        b.put(0, 1);
        b.put(127, 7);
        b.put(127, 7);
    }
    b.put(0, 1);
    std::mt19937 rng(seed);
    b.put(rng() % 2000, 12);
    for (size_t i = 0; i < 40000; ++i)
        b.put(rng() & 255, 8);
    size_t used = 0;
    dst::decode(b.b, ch, 4704, &used);
    b.b.resize((used + 7) / 8);
    return b.b;
}
static void chunk(std::vector<uint8_t> &b, const char *id, const std::vector<uint8_t> &payload) {
    dsd_detail::wrTag(b, id);
    dsd_detail::wrb64(b, payload.size());
    b.insert(b.end(), payload.begin(), payload.end());
    if (payload.size() & 1)
        b.push_back(0);
}
static void dff(const std::string &p, const std::vector<std::vector<uint8_t>> &frames, int ch) {
    using namespace dsd_detail;
    std::vector<uint8_t> prop = {'S', 'N', 'D', ' '}, fs;
    wrb32(fs, 2822400);
    chunk(prop, "FS  ", fs);
    std::vector<uint8_t> chn = {0, (uint8_t)ch};
    for (int c = 0; c < ch; ++c) {
        const char *id = c ? "SRGT" : "SLFT";
        chn.insert(chn.end(), id, id + 4);
    }
    chunk(prop, "CHNL", chn);
    chunk(prop, "CMPR", {'D', 'S', 'T', ' ', 3, 'D', 'S', 'T'});
    std::vector<uint8_t> body = {'D', 'S', 'D', ' '};
    chunk(body, "FVER", {1, 5, 0, 0});
    chunk(body, "PROP", prop);
    std::vector<uint8_t> dst;
    std::vector<uint8_t> frte;
    wrb32(frte, frames.size());
    frte.push_back(0);
    frte.push_back(75);
    chunk(dst, "FRTE", frte);
    for (auto &f : frames)
        chunk(dst, "DSTF", f);
    chunk(body, "DST ", dst);
    std::vector<uint8_t> out;
    wrTag(out, "FRM8");
    wrb64(out, body.size());
    out.insert(out.end(), body.begin(), body.end());
    write_file_bytes(p, out);
}
std::vector<uint8_t> transition_frame(int ch,bool segmented,unsigned seed){Bits b;b.put(1,1);b.put(1,1);b.put(1,1);b.put(segmented?0:1,1);if(segmented){b.put(128,13);b.put(1,6);b.put(1,1);}b.put(1,1);b.put(1,1);if(segmented)b.put(1,1);for(int c=0;c<ch;++c)b.put(0,1);for(int t=0;t<(segmented?2:1);++t){b.put(0,7);b.put(0,1);b.put(t?255:0,9);}for(int t=0;t<(segmented?2:1);++t)b.put(0,6);b.put(0,1);b.put(123,12);std::mt19937 rng(seed);for(int i=0;i<60000;++i)b.put(rng()&255,8);return b.b;}

static int transition_oracle(){size_t allbad=0;for(int ch:{2,5,6})for(unsigned seed:{17,918,12381}){auto a=dst::decode(transition_frame(ch,false,seed),ch,4704),actual=dst::decode(transition_frame(ch,true,seed),ch,4704);size_t bad=0,different=0;for(int c=0;c<ch;++c){unsigned last=0;for(size_t bit=0;bit<4704*8;++bit){unsigned residual=(a[(bit/8)*ch+c]>>(7-bit%8))&1;unsigned expected=bit<1024?residual:(residual^(1-last));unsigned observed=(actual[(bit/8)*ch+c]>>(7-bit%8))&1;bad+=expected!=observed;different+=residual!=observed;last=expected;}}allbad+=bad;printf("ch=%d seed=%u scalar_predictor_mismatch=%zu/%zu changed_after_segment=%zu\n",ch,seed,bad,size_t(ch)*37632,different);}return allbad?1:0;}

int main() {
    int failed = transition_oracle();
    for (int ch : {2, 5, 6}) {
        std::vector<std::vector<uint8_t>> frames;
        DsdStream raw;
        raw.rate = 2822400;
        raw.channels = ch;
        raw.bits = 0;
        raw.data.assign(ch, {});
        for (unsigned seed = 1; seed <= 8; ++seed) {
            auto plain = frame(ch, false, seed), seg = frame(ch, true, seed);
            auto a = dst::decode(plain, ch, 4704), b = dst::decode(seg, ch, 4704);
            bool eq = a == b;
            size_t ones = 0;
            for (auto v : a)
                ones += __builtin_popcount(v);
            bool variable = ones > 2000 && ones < a.size() * 8 - 2000;
            printf("[%s] ch=%d seed=%u segmented invariant, variable ones=%zu/%zu\n",
                   eq && variable ? "PASS" : "FAIL", ch, seed, ones, a.size() * 8);
            failed += !eq || !variable;
            frames.push_back(plain);
            for (size_t i = 0; i < 4704; ++i)
                for (int c = 0; c < ch; ++c)
                    raw.data[c].push_back(a[i * ch + c]);
            raw.bits += 37632;
        }
        std::string base = "dst-diff-" + std::to_string(ch);
        write_dff(base + "-raw.dff", raw);
        dff(base + "-compressed.dff", frames, ch);
        auto restored = read_dsd(base + "-compressed.dff");
        bool native = restored.data == raw.data;
        printf("[%s] native DST DFF reads exact multi-frame DSD\n", native ? "PASS" : "FAIL");
        failed += !native;
        DsdPcmStream ds;
        std::string err;
        bool opened = ds.open(base + "-compressed.dff", dsd_pcm_settings(), &err);
        AudioBuffer actual;
        bool stream = opened && ds.read(0, 71, actual) == 71 && !ds.error();
        printf("[%s] DST DFF streaming PCM opens and produces 71 frames\n", stream ? "PASS" : "FAIL");
        failed += !stream;
    }
    // Hostile segment resolution must fail, no allocation growth or out-of-bounds.
    Bits bad;
    bad.put(1, 1);
    bad.put(1, 1);
    bad.put(1, 1);
    bad.put(0, 1);
    bad.put(0, 13);
    bool reject = false;
    try {
        dst::decode(bad.b, 2, 4704);
    } catch (...) {
        reject = true;
    }
    printf("[%s] zero segmentation resolution rejected\n", reject ? "PASS" : "FAIL");
    failed += !reject;
    return failed ? 1 : 0;
}
