// SPDX-License-Identifier: GPL-3.0-or-later
// Original independent fixtures by the review agent; no audio or optical devices.
#include "audio/dvd_audio.h"
#include <cstdio>
int main(int argc, char **argv) {
    if (argc != 2)
        return 2;
    using namespace rta::audio;
    int failed = 0;
    for (auto name : {"udf-aed-fragmented", "udf-zero-terminator", "udf-after-continuation",
                      "udf-sparse-large", "udf-extended-info", "udf-long-ad", "udf-cycle", "udf-outside",
                      "udf-extended-zero-tail", "udf-fid-wrong-location", "udf-bad-crc", "udf-embedded", "udf-fid-cross-fragments", "udf-fid-cross-wronglocation"}) {
        std::string n = name;
        bool reject = n == "udf-zero-terminator" || n == "udf-after-continuation" || n == "udf-cycle" ||
                      n == "udf-outside" || n == "udf-fid-wrong-location" || n == "udf-bad-crc" || n == "udf-fid-cross-wronglocation";
        try {
            Iso9660 f;
            f.open(std::string(argv[1]) + "/" + name + ".iso");
            auto dir = f.directory(f.root());
            auto e = dir.at("DATA.BIN");
            auto data = f.read(e, 1000000);
            size_t bad = 0;
            for (size_t i = 0; i < data.size(); ++i) {
                uint8_t expected = 0;
                if (n == "udf-aed-fragmented" || n == "udf-long-ad" || n == "udf-fid-cross-fragments")
                    expected = uint8_t((i % 2048) * 37 + (i < 2048 ? 17 : 91));
                else if (n == "udf-extended-info")
                    expected = uint8_t((i % 1024) * 37 + (i < 1024 ? 17 : 91));
                else if (n == "udf-extended-zero-tail")
                    expected = i < 1024 ? uint8_t(i * 37 + 17) : 0;
                else if (n == "udf-embedded")
                    expected = uint8_t(i * 37 + 17);
                bad += data[i] != expected;
            }
            bool ok = !reject && !bad;
            printf("[%s] %s bytes=%zu mismatches=%zu\n", ok ? "PASS" : "FAIL", name, data.size(), bad);
            failed += !ok;
        } catch (std::exception &e) {
            printf("[%s] %s rejects: %s\n", reject ? "PASS" : "FAIL", name, e.what());
            failed += !reject;
        }
    }
    return failed ? 1 : 0;
}
