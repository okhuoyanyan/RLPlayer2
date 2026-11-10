// SPDX-License-Identifier: GPL-3.0-or-later
#include "audio/audio_loader.h"
#include <cstdio>
#include <fstream>
using namespace rta::audio;
static int fails = 0;
static void ck(bool b, const char *m) {
    printf("[%s] %s\n", b ? "PASS" : "FAIL", m);
    fails += !b;
}
static int sample(size_t i, int c) { return int((i * 43 + c * 991) % 20001) - 10000; }

static void test_mlp_pts(const std::string &source) {
    std::ifstream input(std::filesystem::u8path(source), std::ios::binary);
    std::vector<uint8_t> b((std::istreambuf_iterator<char>(input)), {});
    auto put = [&](size_t at, unsigned value) {
        for (int i = 3; i >= 0; --i) {
            b[at + i] = uint8_t(value);
            value >>= 8;
        }
    };
    size_t timestamp = 27 * 2048 + 2048 + 16 + 18;
    put(timestamp + 6, 1125);
    put(timestamp + 10, 1125);
    const std::string path = "dvd-intrasector-pts.iso";
    auto write = [&]() {
        std::ofstream f(path, std::ios::binary);
        f.write((const char *)b.data(), b.size());
    };
    write();
    DvdAudioPcmStream stream;
    std::string why;
    ck(stream.open(path + "#dvda=3:1:1", &why), "MLP same-sector IFO subtrack opens");
    AudioBuffer out;
    auto n = stream.read(0, 1000, out);
    bool same = n == 600;
    for (size_t i = 0; i < n; ++i)
        for (int c = 0; c < 2; ++c)
            same &= out.raw[i * 2 + c] == sample(i + 600, c) * 65536;
    ck(same, "MLP first PES PTS trims all 1200 samples to selected IFO range");
    stream.read(37, 9, out);
    ck(out.raw.size() == 18 && out.raw[0] == sample(637, 0) * 65536,
       "MLP seek remains relative to trimmed track");
    stream.close();
    b[40 * 2048 + 14 + 7] = 0;
    write();
    ck(!stream.open(path + "#dvda=3:1:1", &why) && why.find("no PTS") != std::string::npos,
       "missing first MLP PES PTS is explicitly rejected, no guessed track start");
    ck(dvd_mlp_trim_samples(101, 0xfffffc00ull, 48000) == 600 &&
           dvd_mlp_trim_samples(101, 0x1fffffc00ull, 48000) == 600,
       "IFO32/PES33 timestamp wrap selects exact sample range");
    size_t lpcmTimestamp=23*2048+2048+16+18;
    put(lpcmTimestamp+6,450);put(lpcmTimestamp+10,450);write();
    ck(stream.open(path+"#dvda=1:1:1",&why),"LPCM same-sector IFO subtrack opens with exact PTS");
    n=stream.read(0,1000,out);same=n==240;
    for(size_t i=0;i<n;++i)for(int c=0;c<2;++c)same&=out.raw[i*2+c]==sample(i+240,c)*65536;
    ck(same,"LPCM IFO PTS trims 480 samples to exact selected range");
    stream.read(17,11,out);ck(out.raw.size()==22&&out.raw[0]==sample(257,0)*65536,"LPCM seek remains relative to selected PTS range");
    stream.close();b[30*2048+14+7]=0;write();
    ck(!stream.open(path+"#dvda=1:1:1",&why)&&why.find("no PTS")!=std::string::npos,"LPCM missing first PES PTS rejects instead of guessing first sample");
    b[30*2048+14+7]=0x80;b[30*2048+14+9]&=0xfe;write();
    ck(!stream.open(path+"#dvda=1:1:1",&why)&&why.find("marker")!=std::string::npos,"LPCM broken timestamp marker bits reject before decoding");
    std::remove(path.c_str());
}
int main(int argc, char **argv) {
    if (argc < 2) {
        printf("fixture argument required\n");
        return 2;
    }
    try {
        Iso9660 fs;
        fs.open(argv[1]);
        if (!fs.root().udf)
            test_mlp_pts(argv[1]);
        auto v = dvd_audio_tracks(argv[1]);
        bool foreignRejected=false;try{(void)dvd_audio_select(std::string(argv[1])+"#sacd=2ch:1");}catch(...){foreignRejected=true;}
        ck(foreignRejected,"DVD rejects foreign-format selector instead of guessing first track");
        std::filesystem::copy_file(std::filesystem::u8path(argv[1]),"name#dvda=physical.iso",std::filesystem::copy_options::overwrite_existing);
        ck(dvd_audio_select("name#dvda=physical.iso").track==1,"marker inside physical DVD name preserves default selection");std::remove("name#dvda=physical.iso");
        ck(v.size() == 5, "IFO enumerates 3 groups / 5 tracks");
        for (auto &t : v) {
            DvdAudioPcmStream s;
            std::string why;
            ck(s.open(t.path, &why), ("open " + t.path + " " + why).c_str());
            if (!s.is_open())
                continue;
            size_t frames = t.group == 1 ? 480 : t.group == 2 ? 320 : 1200;
            ck(s.totalFrames() == frames, "PTS track duration maps to exact sample count");
            AudioBuffer b;
            size_t n = s.read(0, 10000, b);
            bool equal = n == frames;
            size_t start = t.group == 3 ? 0 : (t.track - 1) * frames;
            for (size_t i = 0; i < n; ++i)
                for (int c = 0; c < 2; ++c)
                    equal &= b.raw[i * 2 + c] == sample(start + i, c) * 65536;
            ck(equal, "independent formula matches every lossless PCM sample");
            s.read(37, 9, b);
            ck(b.raw.size() == 18 && b.raw[0] == sample(start + 37, 0) * 65536,
               "backward seek returns exact sample");
            ck(should_stream_decode(t.path), "ISO playback selects bounded stream facade");
            DsdPcmStream unified;
            ck(unified.open(t.path, dsd_pcm_settings(), &why) && unified.read(0, 13, b) == 13,
               "existing playback stream facade accepts DVD-A track");
        }
    } catch (const std::exception &e) {
        printf("EXCEPTION %s\n", e.what());
        return 1;
    }
    return fails ? 1 : 0;
}
