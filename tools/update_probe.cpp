// tools/update_probe.cpp — 在线更新的自检 / 实测 / 增量计算 / 替换演练工具
//
// 用法：
//   --selftest                          离线单测（版本 / sha256 / 保护清单 / 清单解析 / 增量计划 / 篡改识别）
//   --check [--json]                    真打一次清单（端点同一份代码）
//   --download                          增量下载 → 校验 → 解压到 staging，打印耗时/速度/文件数
//   --mkfixture <dir> [ver]             造假的旧安装目录（含假用户数据与受保护文件）
//   --mkpkg <olddir> <newdir> <ver> [crash]  造"新版本包"目录（只改一部分文件，含试图覆盖受保护文件的坏包）
//   --mkdelta <out.zip> <newdir> <olddir>    把"有变化的文件"打成一个 delta zip
//   --mkmanifest <newdir> <olddir> <version> <out.json> <deltaUrl> <deltaSize> <deltaSha> [fullUrl] [fullSha] [fullSize] [filesBase]
//   --plan <manifest.json> <dir>        离线算一次增量计划（不联网）
//   --apply                             把 staging 写成台账并立刻落地（= 启动器启动时做的事）
//   --rollback [reason]                 回滚（= 启动器发现新版本起不来时做的事）
//   --hashdir <dir>                     递归打印 sha256（前后比对用）
//   --version                           打印当前版本
//   （无参数）                           若 exe 目录有 fake-launcher.txt 则扮演假启动器（演练用）
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

#include "net/update.h"
#include "net/update_apply.h"
#include "version.h"

using namespace rta::update;

static int g_pass = 0, g_fail = 0;
static void check(bool cond, const std::string& name, const std::string& extra = std::string()) {
    if (cond) { ++g_pass; std::printf("  PASS  %s\n", name.c_str()); }
    else { ++g_fail; std::printf("  FAIL  %s   %s\n", name.c_str(), extra.c_str()); }
}

static std::string read_all(const std::string& p) {
    std::ifstream f(rta::platform::from_utf8(p), std::ios::binary);
    if (!f) return std::string();
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// ---------------------------------------------------------------- 假启动器
// fake-launcher.txt: "serve <port> <version>" 或 "crash"
static int fake_launcher() {
    const std::string dir = install_dir();
    const std::string cfg = read_text_file(join_path(dir, "fake-launcher.txt"));
    std::printf("[fake-launcher] dir=%s cfg='%s'\n", dir.c_str(), cfg.c_str());
    std::fflush(stdout);
    if (cfg.rfind("crash", 0) == 0) {
        std::printf("[fake-launcher] 按剧本立刻退出（模拟新版本起不来）\n");
        return 3;
    }
    int port = 8935;
    std::string ver = current_version();
    {
        std::istringstream is(cfg);
        std::string what;
        is >> what;
        if (what == "serve") {
            int p2 = 0;
            std::string v2;
            if (is >> p2 >> v2) { port = p2; if (!v2.empty()) ver = v2; }
        }
    }
    WSADATA wsa{};
    ::WSAStartup(MAKEWORD(2, 2), &wsa);
    SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return 4;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons((u_short)port);
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    if (::bind(s, (sockaddr*)&a, sizeof(a)) != 0) { std::printf("[fake-launcher] bind %d 失败\n", port); return 4; }
    ::listen(s, 8);
    std::printf("[fake-launcher] 监听 127.0.0.1:%d 上报 version=%s\n", port, ver.c_str());
    std::fflush(stdout);
    const std::string body = "{\"ok\":true,\"app\":\"RLPlayer\",\"version\":\"" + ver + "\"}";
    // 有界生命周期：最多回 3 次 / 空闲 6 秒就自己退出 —— 演练里不需要（也不允许）靠杀进程收场。
    int served = 0;
    for (;;) {
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(s, &rd);
        timeval tv{6, 0};
        const int sel = ::select(0, &rd, nullptr, nullptr, &tv);
        if (sel <= 0) { std::printf("[fake-launcher] 空闲超时，退出\n"); break; }
        SOCKET c = ::accept(s, nullptr, nullptr);
        if (c == INVALID_SOCKET) break;
        char buf[2048];
        const int n = ::recv(c, buf, sizeof(buf) - 1, 0);
        if (n > 0) {
            buf[n] = 0;
            std::string resp = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\n";
            resp += "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
            ::send(c, resp.c_str(), (int)resp.size(), 0);
        }
        ::closesocket(c);
        if (++served >= 3) { std::printf("[fake-launcher] 已回 %d 次，退出\n", served); break; }
    }
    ::WSACleanup();
    return 0;
}

// ---------------------------------------------------------------- 目录工具
static std::string sha256_of_file(const std::string& p);
static void hash_dir(const std::string& root, const std::string& rel,
                     std::vector<std::pair<std::string, std::string>>& out) {
    WIN32_FIND_DATAW fd{};
    const std::string dir = rel.empty() ? root : join_path(root, rel);
    HANDLE h = ::FindFirstFileW(rta::platform::from_utf8(join_path(dir, "*")).c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        const std::wstring nm(fd.cFileName);
        if (nm == L"." || nm == L"..") continue;
        const std::string name = rta::platform::detail::narrow(nm);
        const std::string childRel = rel.empty() ? name : (rel + "\\" + name);
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) hash_dir(root, childRel, out);
        else out.push_back({childRel, sha256_of_file(join_path(root, childRel))});
    } while (::FindNextFileW(h, &fd));
    ::FindClose(h);
}

static std::string sha256_of_file(const std::string& p) {
    std::string hex, err;
    if (!sha256_file(p, hex, err)) return std::string("ERR");
    return hex;
}

static void copy_tree(const std::string& from, const std::string& to) {
    std::vector<std::string> rels;
    list_files_rel(from, "", rels);
    for (const auto& rel : rels) {
        // 跳过我们自己的工作目录
        if (rel.rfind("updates\\", 0) == 0) continue;
        const std::string dst = join_path(to, rel);
        const size_t sp = dst.find_last_of('\\');
        if (sp != std::string::npos) ensure_dir(dst.substr(0, sp));
        ::CopyFileW(rta::platform::from_utf8(join_path(from, rel)).c_str(),
                    rta::platform::from_utf8(dst).c_str(), FALSE);
    }
}

static void put(const std::string& root, const std::string& rel, const std::string& text) {
    const std::string p = join_path(root, rel);
    const size_t sp = p.find_last_of('\\');
    if (sp != std::string::npos) ensure_dir(p.substr(0, sp));
    write_text_file(p, text);
}

int main(int argc, char** argv) {
    ::SetConsoleOutputCP(CP_UTF8);
    const std::string mode = argc > 1 ? argv[1] : "";

    if (mode.empty()) {
        if (path_exists(join_path(install_dir(), "fake-launcher.txt"))) return fake_launcher();
        std::printf("update_probe：在线更新自检/实测工具（--selftest / --check / --download / --mkfixture …）\n");
        std::printf("当前版本 %s，安装目录 %s\n", current_version().c_str(), install_dir().c_str());
        return 0;
    }

    if (mode == "--version") { std::printf("%s\n", current_version().c_str()); return 0; }

    // ===================== 离线单测 =====================
    if (mode == "--selftest") {
        std::printf("== [1] 版本比较（update_check.h 的语义化实现）==\n");
        struct { const char* a; const char* b; int want; } cases[] = {
            {"1.5.9", "1.5.10", -1}, {"1.5.10", "1.5.9", 1}, {"1.6.0", "1.5.99", 1},
            {"1.5.9", "1.5.9", 0},   {"2.0", "1.99.99", 1}, {"v1.5.9", "1.5.9", 0},
            {"1.5.9-beta", "1.5.9", -1}, {"1.5.9", "1.5.9-beta", 1},
            {"1.5", "1.5.0", 0},     {"1.10.0", "1.9.9", 1}, {"1.5.9", "1.5.9 ", 0},
        };
        for (const auto& c : cases) {
            const int got = cmp_versions(c.a, c.b);
            char nm[128], ex[64];
            std::snprintf(nm, sizeof(nm), "compare('%s','%s') == %d", c.a, c.b, c.want);
            std::snprintf(ex, sizeof(ex), "got %d", got);
            check(got == c.want, nm, ex);
        }
        check(std::string("1.5.10") < std::string("1.5.9"), "反证：字典序会把 1.5.10 判小");
        check(cmp_versions("1.5.10", "1.5.9") > 0, "反证：语义化比较把 1.5.10 判大");

        std::printf("== [2] SHA-256（update_check.h 的 detail_sha256）==\n");
        check(sha256_hex(std::string("")) ==
              "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "sha256(\"\")");
        check(sha256_hex(std::string("abc")) ==
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "sha256(\"abc\")");
        check(sha256_hex(std::string("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")) ==
              "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1", "sha256(56 字节向量)");

        std::printf("== [3] 受保护清单 ==\n");
        const char* prot[] = {"rlplayer-settings.conf", "rlplayer-library.conf", "rlplayer-llm.conf",
                              "rlplayer-feedback.conf", "rlplayer-queue.txt", "rlplayer-savedqueues.conf",
                              "rlplayer-favs.json", "data\\lib.idx", "data\\roomcorr\\a.json",
                              "covercache\\abc.jpg", "WebView2Runtime\\msedgewebview2.exe",
                              "playlists\\x.m3u8", "updates\\1.5.9\\x.zip", "rlplayer-meta\\a.json",
                              "RLPLAYER-SETTINGS.CONF", "sub\\rlplayer-settings.conf", "data"};
        for (const char* p : prot) check(is_protected_rel(p), std::string("保护 ") + p);
        const char* noprot[] = {"RLPlayer.exe", "rtaweb.exe", "rtagpu.dll", "rtagpu_cuda.dll",
                                "amdhip64_7.dll", "ffmpeg.exe", "ffprobe.exe", "WebView2Loader.dll",
                                "tools\\rtaplay.exe", "tools\\aidecode.exe", "data/opra_index.json",
                                "README.txt", "OPRA-NOTICE.txt"};
        for (const char* p : noprot) check(!is_protected_rel(p), std::string("可替换 ") + p);

        std::printf("== [4] 清单解析（MAC 的 latest.json 语义 + 我们的 files[] 扩展）==\n");
        const std::string full =
            "{\"version\":\"1.6.0\",\"notes\":\"n\",\"published_at\":\"2026-10-27T00:00:00Z\","
            "\"download_url\":\"https://github.com/okhuoyanyan/RLPlayer/releases/download/v1.6.0/d.zip\","
            "\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"size\":111,"
            "\"full\":{\"url\":\"https://github.com/okhuoyanyan/RLPlayer/releases/download/v1.6.0/f.zip\","
            "\"sha256\":\"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\",\"size\":222},"
            "\"files_base\":\"https://raw.githubusercontent.com/x/y/main/files/1.6.0/\","
            "\"files\":[{\"path\":\"rtaweb.exe\",\"sha256\":\"cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc\",\"size\":7449600},"
            "{\"path\":\"tools/rtaplay.exe\",\"sha256\":\"dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd\",\"size\":120000}]}";
        {
            ManifestExt m = parse_manifest_ext(full);
            check(m.ok && m.version == "1.6.0", "版本解析");
            check(m.deltaUrl.find("d.zip") != std::string::npos && m.deltaSize == 111, "delta 地址与大小");
            check(m.fullUrl.find("f.zip") != std::string::npos && m.fullSize == 222, "full 兜底字段");
            check(m.filesBase.find("files/1.6.0/") != std::string::npos, "files_base");
            check(m.files.size() == 2, "files[] 条数");
            check(m.files[1].path == "tools\\rtaplay.exe", "路径 / 归一化成反斜杠");
        }
        {
            ManifestExt m = parse_manifest_ext("{\"version\":\"1.6.0\"}");
            check(!m.ok && m.error.find("files") != std::string::npos, "缺 files[] → 明确拒绝");
        }
        {
            ManifestExt m = parse_manifest_ext(
                "{\"version\":\"1.6.0\",\"files\":[{\"path\":\"../evil.dll\","
                "\"sha256\":\"cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc\"}]}");
            check(!m.ok, "路径穿越 ../ 被拒");
        }
        {
            ManifestExt m = parse_manifest_ext(
                "{\"version\":\"1.6.0\",\"files\":[{\"path\":\"a.dll\",\"sha256\":\"zz\"}]}");
            check(!m.ok, "sha256 非法被拒");
        }
        {
            ManifestExt m = parse_manifest_ext(
                "{\"version\":\"1.6.0\",\"download_url\":\"http://evil.com/x.zip\","
                "\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"}");
            check(m.deltaUrl.empty(), "非白名单下载地址被丢弃（http://evil.com）");
        }
        {
            // 判定入口：五种失败互不相等，且都不等于「已是最新」
            CheckResult up = evaluate_update_check("1.5.9", true, 200, full);
            check(up.decision == Decision::UpdateAvailable, "1.5.9 + 清单 1.6.0 → 有更新");
            CheckResult same = evaluate_update_check("1.6.0", true, 200, full);
            check(same.decision == Decision::UpToDate, "1.6.0 + 清单 1.6.0 → 已是最新");
            CheckResult net = evaluate_update_check("1.5.9", false, 0, "");
            check(net.decision == Decision::FailedNetwork && !net.ok(), "断网 → network-error 且不是最新");
            CheckResult h404 = evaluate_update_check("1.5.9", false, 404, "");
            check(h404.decision == Decision::FailedHttp, "404 → http-error");
            CheckResult bad = evaluate_update_check("1.5.9", true, 200, "not json");
            check(bad.decision == Decision::FailedParse, "非 JSON → parse-error");
        }
        {
            const std::string macOnly =
                "{\"version\":\"1.1.0-beta\",\"download_url\":\"https://github.com/BH2VOQ/x/releases/download/v1/a.dmg\","
                "\"sha256\":\"9e0f392efacd33db531fca9770912391a727b45cd0d507e4788476aec68da684\",\"size\":6877669}";
            Manifest macm = parse_update_manifest(macOnly);
            check(macm.valid, "MAC 的 latest.json 仍能被 update_check.h 原样解析（只增不改）");
            CheckResult r = evaluate_update_check("1.0.0", true, 200, macOnly);
            check(r.decision == Decision::UpdateAvailable, "MAC 清单也能判出更新");
        }

        std::printf("== [5] 增量计划 / 篡改识别 ==\n");
        const std::string tmp = join_path(rta::platform::temp_dir_utf8(), "upd-selftest");
        rmtree(tmp);
        ensure_dir(tmp);
        put(tmp, "a.exe", "AAA");
        put(tmp, "b.dll", "BBB");
        put(tmp, "tools\\c.exe", "CCC");
        {
            std::vector<ManifestFile> files;
            files.push_back({"a.exe", sha256_hex(std::string("AAA")), 3});
            files.push_back({"b.dll", sha256_hex(std::string("BBB")), 3});
            files.push_back({"tools\\c.exe", sha256_hex(std::string("CCC")), 3});
            files.push_back({"d.new", sha256_hex(std::string("DDD")), 3});
            Plan p = plan_incremental(files, tmp);
            check(p.ok && p.toDownload == 1 && p.unchanged == 3,
                  "3 个一致 + 1 个缺失 → 只下 1 个", "toDownload=" + std::to_string(p.toDownload));
            check(p.downloadBytes == 3, "下载字节只算变化的那个");
            // 篡改一个字节
            put(tmp, "b.dll", "BBC");
            Plan p2 = plan_incremental(files, tmp);
            check(p2.toDownload == 2, "改一个字节 → 该文件必须重新下载",
                  "toDownload=" + std::to_string(p2.toDownload));
            bool found = false;
            for (const auto& it : p2.items) if (it.path == "b.dll" && !it.same) found = true;
            check(found, "被改的那个文件本身被标记为 Differ");
        }
        rmtree(tmp);

        std::printf("== [6] 校验（size + sha256 双闸门）==\n");
        {
            const std::string f = join_path(rta::platform::temp_dir_utf8(), "upd-verify.bin");
            write_text_file(f, "abc");
            VerifyResult ok = verify_downloaded_file(f, sha256_hex(std::string("abc")), 3);
            check(ok.ok, "内容与 sha 一致 → 通过");
            VerifyResult bad = verify_downloaded_file(f, sha256_hex(std::string("abd")), 3);
            check(!bad.ok && bad.reason == VerifyFailReason::Sha256Mismatch, "sha 不符 → 拒绝");
            VerifyResult sz = verify_downloaded_file(f, sha256_hex(std::string("abc")), 4);
            check(!sz.ok && sz.reason == VerifyFailReason::SizeMismatch, "size 不符 → 拒绝");
            VerifyResult noSha = verify_downloaded_file(f, "", 3);
            check(!noSha.ok && noSha.reason == VerifyFailReason::MissingExpectedSha, "缺 sha → 拒绝（不是放行）");
            ::DeleteFileW(rta::platform::from_utf8(f).c_str());
        }

        std::printf("\n结果：PASS %d / FAIL %d\n", g_pass, g_fail);
        return g_fail == 0 ? 0 : 1;
    }

    // ===================== 真检查 =====================
    if (mode == "--check") {
        const bool json = argc > 2 && std::string(argv[2]) == "--json";
        const auto t0 = std::chrono::steady_clock::now();
        const bool ok = check_now(8000);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t0).count();
        std::printf("--check 耗时 %lld ms ok=%d\n", (long long)ms, (int)ok);
        if (json) { std::printf("%s\n", check_json().c_str()); return ok ? 0 : 2; }
        {
            auto& s = state();
            CheckResult cr; Plan pl;
            { std::lock_guard<std::mutex> lk(s.mtx); cr = s.cr; pl = s.plan; }
            std::printf("decision=%s current=%s latest=%s\n", cr.code(), current_version().c_str(),
                        cr.remoteVersion.c_str());
            std::printf("增量：需下载 %d 个文件 / %s，跳过 %d 个 / %s（全量 %s）\n",
                        pl.toDownload, size_text(pl.downloadBytes).c_str(),
                        pl.unchanged, size_text(pl.skippedBytes).c_str(),
                        size_text(s.man.fullSize).c_str());
            for (const auto& it : pl.items) {
                if (it.same) continue;
                std::printf("  + %-40s %10lld B  have=%s\n", it.path.c_str(), it.wantSize,
                            it.haveSha.empty() ? "(none)" : it.haveSha.substr(0, 12).c_str());
            }
            if (!cr.error.empty()) std::printf("error=%s\n", cr.error.c_str());
        }
        return ok ? 0 : 2;
    }

    // ===================== 真下载（增量）=====================
    if (mode == "--download") {
        if (!check_now(8000)) { std::printf("检查失败：%s\n", check_json().c_str()); return 2; }
        std::string err;
        const auto t0 = std::chrono::steady_clock::now();
        if (!start_download(err)) { std::printf("下载未能启动：%s\n", err.c_str()); return 3; }
        for (;;) {
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            auto& s = state();
            std::string phase, e;
            long long dl = 0, total = 0;
            double sp = 0;
            {
                std::lock_guard<std::mutex> lk(s.mtx);
                phase = s.phase; e = s.dlError; dl = s.downloaded; total = s.total; sp = s.speedBps;
            }
            std::printf("  [%s] %lld/%lld B  %s/s\n", phase.c_str(), dl, total, size_text((long long)sp).c_str());
            std::fflush(stdout);
            if (phase == "ready" || phase == "error") {
                if (phase == "error") { std::printf("下载失败：%s\n", e.c_str()); return 4; }
                break;
            }
        }
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t0).count();
        std::printf("\n完成：耗时 %.2f s\n%s\n", (double)ms / 1000.0, progress_json().c_str());
        return 0;
    }

    // ===================== 造演练目录 =====================
    if (mode == "--mkfixture" && argc > 2) {
        const std::string dir = argv[2];
        const std::string ver = argc > 3 ? argv[3] : "1.5.3";
        ensure_dir(dir);
        // 假程序文件（内容带版本，便于逐字节判断"是否被换掉"）
        const char* prog[] = {"rtaweb.exe", "rtagpu.dll", "rtagpu_cuda.dll", "amdhip64_7.dll",
                              "ffmpeg.exe", "ffprobe.exe", "WebView2Loader.dll",
                              "tools\\rtaplay.exe", "tools\\rtatag.exe", "README.txt"};
        for (const char* p : prog) put(dir, p, std::string("OLD-") + ver + "-" + p);
        // 旧的"启动器"= 本工具的副本（能被真换掉，也能扮演假启动器）
        {
            wchar_t self[MAX_PATH] = {0};
            ::GetModuleFileNameW(nullptr, self, MAX_PATH);
            ensure_dir(join_path(dir, "."));
            ::CopyFileW(self, rta::platform::from_utf8(join_path(dir, "RLPlayer.exe")).c_str(), FALSE);
        }
        // 假用户数据（必须一字不变）
        put(dir, "rlplayer-settings.conf", "backend=wasapi\r\nfilter=poly-sinc-ext2\r\nvolume=-6\r\n");
        put(dir, "rlplayer-library.conf", "root=D:\\MUSIC\r\n");
        put(dir, "rlplayer-favs.json", "[\"D:\\\\MUSIC\\\\a.flac\"]\r\n");
        put(dir, "rlplayer-queue.txt", "D:\\MUSIC\\b.flac\r\n");
        put(dir, "rlplayer-savedqueues.conf", "night=D:\\MUSIC\\c.flac\r\n");
        put(dir, "rlplayer-llm.conf", "key=sk-xxx\r\n");
        put(dir, "rlplayer-peq.txt", "0,0,1.0\r\n");
        put(dir, "data\\opra_index.json", "{\"old\":true}\r\n");
        put(dir, "data\\lib.idx", "LIBIDX-OLD\r\n");
        put(dir, "data\\roomcorr\\room1.json", "{\"room\":1}\r\n");
        put(dir, "covercache\\abc.jpg", "JPEG-OLD\r\n");
        put(dir, "WebView2Runtime\\msedgewebview2.exe", "WVR-OLD\r\n");
        put(dir, "playlists\\night.m3u8", "#EXTM3U\r\n");
        put(dir, "fake-launcher.txt", "serve 8935 " + ver + "\r\n");
        std::printf("fixture 就绪：%s（旧版本 %s）\n", dir.c_str(), ver.c_str());
        return 0;
    }

    // ===================== 造新版本包目录 =====================
    if (mode == "--mkpkg" && argc > 4) {
        const std::string oldDir = argv[2], newDir = argv[3], ver = argv[4];
        const bool crash = (argc > 5 && std::string(argv[5]) == "crash");
        rmtree(newDir);
        ensure_dir(newDir);
        copy_tree(oldDir, newDir);
        // 真正会变的：主程序 + 工具 + 一个应用资产
        {
            wchar_t self[MAX_PATH] = {0};
            ::GetModuleFileNameW(nullptr, self, MAX_PATH);
            const std::string dst = join_path(newDir, "RLPlayer.exe");
            ensure_dir(join_path(newDir, "."));
            ::CopyFileW(self, rta::platform::from_utf8(dst).c_str(), FALSE);
            // 追加一段标记：PE 允许尾部垃圾字节，仍然可执行；这样"新启动器"与旧的真实不同，
            // 才会进 delta、才谈得上"启动器替换自己"的演练。
            {
                std::ofstream ap(rta::platform::from_utf8(dst), std::ios::binary | std::ios::app);
                if (ap) ap << "RLPLAYER-DRILL-NEW " << ver << "\n";
            }
        }
        put(newDir, "rtaweb.exe", "NEW-" + ver + "-rtaweb.exe");
        put(newDir, "rtagpu.dll", "NEW-" + ver + "-rtagpu.dll");
        put(newDir, "tools\\rtaplay.exe", "NEW-" + ver + "-tools\\rtaplay.exe");
        put(newDir, "data\\opra_index.json", "{\"new\":true,\"v\":\"" + ver + "\"}\r\n");
        // ★ 坏包：试图覆盖受保护文件（data\lib.idx / settings / WebView2Runtime / covercache）
        put(newDir, "data\\lib.idx", "EVIL-LIBIDX\r\n");
        put(newDir, "rlplayer-settings.conf", "EVIL-SETTINGS\r\n");
        put(newDir, "WebView2Runtime\\msedgewebview2.exe", "EVIL-WVR\r\n");
        put(newDir, "covercache\\abc.jpg", "EVIL-JPEG\r\n");
        put(newDir, "fake-launcher.txt", std::string(crash ? "crash" : "serve 8935 ") + ver + "\r\n");
        std::printf("新版本包目录就绪：%s（version=%s%s）\n", newDir.c_str(), ver.c_str(),
                    crash ? "，剧本=崩溃" : "");
        return 0;
    }

    // ===================== 造 delta zip =====================
    if (mode == "--mkdelta" && argc > 4) {
        const std::string out = argv[2], newDir = argv[3], oldDir = argv[4];
        std::vector<std::pair<std::string, std::string>> nf, of;
        hash_dir(newDir, "", nf);
        hash_dir(oldDir, "", of);
        std::map<std::string, std::string> oldMap;
        for (const auto& p : of) oldMap[p.first] = p.second;
        std::string cmd = "\"" + tar_exe() + "\" -a -cf \"" + out + "\" -C \"" + newDir + "\"";
        int n = 0;
        for (const auto& p : nf) {
            auto it = oldMap.find(p.first);
            if (it != oldMap.end() && it->second == p.second) continue;   // 没变的不进 delta
            cmd += " \"" + p.first + "\"";
            ++n;
        }
        if (n == 0) { std::printf("没有变化文件，delta 不生成\n"); return 0; }
        auto pr = rta::platform::run_hidden(cmd, true);
        if (!pr.started || pr.exit_code != 0) {
            std::printf("tar 打包失败（退出码 %d）：%s\n", pr.exit_code, pr.output.substr(0, 300).c_str());
            return 5;
        }
        std::printf("delta zip：%s（%d 个变化文件，%lld B，sha256=%s）\n",
                    out.c_str(), n, file_size(out), sha256_of_file(out).c_str());
        return 0;
    }

    // ===================== 生成清单 =====================
    if (mode == "--mkmanifest" && argc > 8) {
        const std::string newDir = argv[2], oldDir = argv[3], ver = argv[4], out = argv[5];
        const std::string deltaUrl = argv[6];
        const long long deltaSize = std::atoll(argv[7]);
        const std::string deltaSha = argv[8];
        const std::string fullUrl = argc > 9 ? argv[9] : "";
        const long long fullSize = argc > 10 ? std::atoll(argv[10]) : 0;
        const std::string fullSha = argc > 11 ? argv[11] : "";
        const std::string filesBase = argc > 12 ? argv[12] : "";

        std::vector<std::pair<std::string, std::string>> nf, of;
        hash_dir(newDir, "", nf);
        hash_dir(oldDir, "", of);
        std::map<std::string, std::string> oldMap;
        for (const auto& p : of) oldMap[p.first] = p.second;

        std::string js = "{\r\n  \"version\": \"" + ver + "\",\r\n";
        js += "  \"notes\": \"演练清单（由 update_probe --mkmanifest 生成）\",\r\n";
        js += "  \"published_at\": \"2026-10-27T00:00:00Z\",\r\n";
        js += "  \"download_url\": \"" + deltaUrl + "\",\r\n";
        js += "  \"sha256\": \"" + deltaSha + "\",\r\n";
        js += "  \"size\": " + std::to_string(deltaSize) + ",\r\n";
        if (!fullUrl.empty()) {
            js += "  \"full\": {\"url\": \"" + fullUrl + "\", \"sha256\": \"" + fullSha +
                  "\", \"size\": " + std::to_string(fullSize) + "},\r\n";
        }
        if (!filesBase.empty()) js += "  \"files_base\": \"" + filesBase + "\",\r\n";
        js += "  \"files\": [\r\n";
        int changeN = 0;
        long long changeBytes = 0, sameBytes = 0;
        for (size_t i = 0; i < nf.size(); ++i) {
            const auto& p = nf[i];
            std::string rel = p.first;
            for (char& c : rel) if (c == '\\') c = '/';
            const long long sz = file_size(join_path(newDir, p.first));
            js += "    {\"path\": \"" + rel + "\", \"sha256\": \"" + p.second +
                  "\", \"size\": " + std::to_string(sz) + "}";
            js += (i + 1 == nf.size()) ? "\r\n" : ",\r\n";
            auto it = oldMap.find(p.first);
            if (it == oldMap.end() || it->second != p.second) { ++changeN; changeBytes += sz; }
            else sameBytes += sz;
        }
        js += "  ]\r\n}\r\n";
        write_text_file(out, js);
        std::printf("清单：%s（%zu 个文件；变化 %d 个 / %lld B；未变 %lld B）\n",
                    out.c_str(), nf.size(), changeN, changeBytes, sameBytes);
        return 0;
    }

    // ===================== 离线增量计划 =====================
    if (mode == "--plan" && argc > 3) {
        const std::string manFile = argv[2], dir = argv[3];
        ManifestExt m = parse_manifest_ext(read_all(manFile));
        if (!m.ok) { std::printf("清单无效：%s\n", m.error.c_str()); return 5; }
        Plan p = plan_incremental(m.files, dir);
        long long full = 0;
        for (const auto& f : m.files) full += f.size;
        std::printf("version=%s  本地目录=%s\n", m.version.c_str(), dir.c_str());
        std::printf("清单文件 %zu 个 / 全量 %lld B (%s)\n", m.files.size(), full, size_text(full).c_str());
        std::printf("需要下载 %d 个 / %lld B (%s)\n", p.toDownload, p.downloadBytes,
                    size_text(p.downloadBytes).c_str());
        std::printf("跳过 %d 个 / %lld B (%s)\n", p.unchanged, p.skippedBytes,
                    size_text(p.skippedBytes).c_str());
        for (const auto& it : p.items) {
            if (it.same) continue;
            std::printf("  + %-38s %10lld B  本地=%s\n", it.path.c_str(), it.wantSize,
                        it.haveSha.empty() ? "(缺失)" : it.haveSha.substr(0, 12).c_str());
        }
        return 0;
    }

    // ===================== 落地（= 启动器启动时）=====================
    if (mode == "--apply") {
        restore_staging_state();
        auto& s = state();
        std::string ver, staging;
        bool staged = false;
        { std::lock_guard<std::mutex> lk(s.mtx); staged = s.staged; ver = s.dlVersion; staging = s.stagingDir; }
        if (!staged) { std::printf("没有已暂存的更新\n"); return 6; }
        // 端点 /apply 做的那一步：写台账
        if (path_exists(pending_path())) {
            PendingPlan old = read_pending();
            if (old.phase == "ready") {
                std::printf("已有 ready 台账（%s），直接落地\n", old.version.c_str());
            } else {
                std::string err;
                if (!write_ready_plan(ver, current_version(), staging, err)) {
                    std::printf("写台账失败：%s\n", err.c_str()); return 6;
                }
            }
        } else {
            std::string err;
            if (!write_ready_plan(ver, current_version(), staging, err)) {
                std::printf("写台账失败：%s\n", err.c_str()); return 6;
            }
        }
        const std::string r = apply_pending_on_startup();
        std::printf("%s\n", r.c_str());
        PendingPlan p = read_pending();
        std::printf("phase=%s applied=%zu added=%zu staging=%s\n", p.phase.c_str(), p.applied.size(),
                    p.added.size(), p.staging.c_str());
        return p.phase == "applied" ? 0 : 7;
    }

    if (mode == "--rollback") {
        const std::string why = argc > 2 ? argv[2] : "演练：模拟新版本起不来";
        const std::string r = rollback_pending(why);
        std::printf("%s\n", r.empty() ? "没有可回滚的台账" : r.c_str());
        PendingPlan p = read_pending();
        std::printf("phase=%s detail=%s\n", p.phase.c_str(), p.detail.c_str());
        return r.empty() ? 8 : 0;
    }

    if (mode == "--confirm") {
        const std::string r = confirm_on_startup();
        std::printf("%s\n", r.empty() ? "(无台账)" : r.c_str());
        return 0;
    }

    if (mode == "--hashdir" && argc > 2) {
        std::vector<std::pair<std::string, std::string>> v;
        hash_dir(argv[2], "", v);
        std::sort(v.begin(), v.end());
        for (const auto& p : v) std::printf("%s  %s\n", p.second.c_str(), p.first.c_str());
        std::printf("# %zu 个文件\n", v.size());
        return 0;
    }

    std::printf("未知参数：%s\n", mode.c_str());
    return 1;
}
