// SPDX-License-Identifier: GPL-3.0-or-later
#include "net/library.h"
#include <cstdio>
using namespace rta::net;
int main() {
    try {
        namespace fs = std::filesystem;
        auto base = fs::current_path() / ("depth-migration-" + std::to_string(GetCurrentProcessId()));
        fs::create_directories(base / "media");
        fs::create_directories(base / "index");
        auto file = base / "media" / "actual-float32.wav";
        auto u8 = [](const fs::path &p) {
            auto s = p.u8string();
            return std::string((const char *)s.data(), s.size());
        };
        rta::audio::AudioBuffer b;
        b.rate = 48000;
        b.channels = 2;
        b.bits = 32;
        b.isFloat = true;
        b.raw.assign(960, 0);
        rta::audio::write_wav(u8(file), b);
        {
            std::ofstream root(base / "index" / "rlplayer-library.conf", std::ios::binary);
            root << u8(base / "media") << "\n";
            std::ofstream idx(base / "index" / "rlplayer-index.tsv", std::ios::binary);
            idx << "#libIndexV\t6\n"
                << u8(file) << "\t" << fs::last_write_time(file).time_since_epoch().count() << "\t"
                << fs::file_size(file)
                << "\tUSER-ALBUM-SENTINEL\tUSER-ALBUM-ARTIST\tUSER-ARTIST\tUSER-"
                   "TITLE\t7\t9\t1999\t48000\t16\t2\t0.010\t0\tUSER-GENRE\t\t0\t0\t0\tuser_assigned\n";
        }
        _putenv_s("RLPLAYER_LIBRARY_DIR", u8(base / "index").c_str());
        _putenv_s("RLPLAYER_PORT", "");
        auto &lib = Library::instance();
        lib.load();
        bool needsUpgradeAtStartup=lib.indexNeedsRetag();
        lib.startScan(false);
        lib.joinScan();
        int fails = 0;
        auto check = [&](bool ok, const char *m) {
            printf("[%s] %s\n", ok ? "PASS" : "FAIL", m);
            fails += !ok;
        };
        check(needsUpgradeAtStartup&&!lib.indexNeedsRetag(),"missing depth marker triggers one startup scan and clears after technical migration");
        for (int round = 0; round < 2; ++round) {
            if (round)
                lib.load();
            LibTrack t;
            check(lib.trackCount() == 1 && lib.trackAt(0, t),
                  "legacy v6 cached track survives upgrade/restart");
            check(t.bits == 32 && t.codec == "pcm_f32le", "only technical source precision/codec upgraded");
            check(t.title == "USER-TITLE" && t.artist == "USER-ARTIST" && t.album == "USER-ALBUM-SENTINEL" &&
                      t.albumArtist == "USER-ALBUM-ARTIST" && t.genre == "USER-GENRE" && t.trackNo == 7 &&
                      t.discNo == 9 && t.year == 1999,
                  "all editorial metadata sentinels survive");
            check(t.albumIdentitySource == LibAlbumIdentitySource::UserAssigned,
                  "user-assigned album ownership survives");
        }
        lib.startScan(false);
        lib.joinScan();
        check(lib.snapshot().cached == 1 && lib.snapshot().probed == 0,
              "next incremental scan reuses migrated cache");
        return fails ? 1 : 0;
    } catch (const std::exception &e) {
        printf("ERROR %s\n", e.what());
        return 1;
    }
}
