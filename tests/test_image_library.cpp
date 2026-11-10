// SPDX-License-Identifier: GPL-3.0-or-later
#include "net/library.h"
#include <cstdio>
#include <set>
using namespace rta::net;
static int fails = 0;
static void ck(bool ok, const char *m) {
    printf("[%s] %s\n", ok ? "PASS" : "FAIL", m);
    fails += !ok;
}
static std::string utf8(const std::filesystem::path &p) {
    auto s = p.u8string();
    return std::string((const char *)s.data(), s.size());
}
int main(int argc, char **argv) {
    if (argc != 4) {
        printf("args raw.iso dst.iso dvd.iso\n");
        return 2;
    }
    try {
        namespace fs = std::filesystem;
        auto base = fs::current_path() / ("image-library-" + std::to_string(GetCurrentProcessId()));
        auto media = base / "media", index = base / "index";
        fs::create_directories(media);
        fs::create_directories(index);
        fs::copy_file(fs::u8path(argv[1]), media / "disc#raw.iso");
        fs::copy_file(fs::u8path(argv[2]), media / "disc#sacd=name.iso");
        fs::copy_file(fs::u8path(argv[3]), media / "dvd#one.iso");
        _putenv_s("RLPLAYER_LIBRARY_DIR", utf8(index).c_str());
        _putenv_s("RLPLAYER_PORT", "");
        auto &lib = Library::instance();
        lib.load();
        for (auto &r : lib.roots())
            lib.removeRoot(r);
        ck(lib.addRootEx(utf8(media)).ok, "private image root accepted");
        lib.startScan(true);
        lib.joinScan();
        printf("scan count=%zu msg=%s\n", lib.trackCount(), lib.scanMsg().c_str());
        ck(lib.trackCount() == 13, "three physical images expand to thirteen virtual tracks");
        ck(lib.needsConvertCount() == 0, "all enumerated audio image tracks are playable");
        std::set<std::string> keys;
        bool info = true;
        for (size_t i = 0; i < lib.trackCount(); ++i) {
            LibTrack t;
            lib.trackAt(i, t);
            keys.insert(t.path);
            info &=
                t.duration > 0 && t.sampleRate > 0 && t.channels > 0 && t.bits > 0 && lib_file_exists(t.path);
        }
        ck(keys.size() == 13 && info, "virtual identity metadata and physical existence are valid");
        lib.load();
        ck(lib.trackCount() == 13, "index reload preserves every virtual track");
        std::set<std::string> reload;
        for (size_t i = 0; i < lib.trackCount(); ++i) {
            LibTrack t;
            lib.trackAt(i, t);
            reload.insert(t.path);
            info &= !t.codec.empty();
        }
        ck(keys == reload && info, "codec and keys survive index serialization");
        lib.startScan(false);
        lib.joinScan();
        ck(lib.trackCount() == 13, "incremental scan neither duplicates nor loses virtual tracks");
        fs::rename(media / "disc#raw.iso", base / "removed-raw.iso");
        lib.startScan(false);
        lib.joinScan();
        ck(lib.trackCount() == 9, "removing one physical image prunes all four virtual tracks");
        lib.load();
        ck(lib.trackCount() == 9, "removed image tracks remain absent after restart");
    } catch (const std::exception &e) {
        printf("ERROR %s\n", e.what());
        return 1;
    }
    return fails ? 1 : 0;
}
