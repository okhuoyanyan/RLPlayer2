#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include "net/update.h"
#include "net/update_apply.h"

#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <vector>

namespace protocol_tests {

static int passes = 0;
static int failures = 0;

static void check(bool condition, const std::string& name) {
    if (condition) {
        ++passes;
        std::printf("  PASS  %s\n", name.c_str());
    } else {
        ++failures;
        std::printf("  FAIL  %s\n", name.c_str());
    }
}

static std::string manifest_json(const std::string& version = "3.2.3",
                                 const std::string& mode = "incremental",
                                 const std::string& path = "app.exe") {
    const std::string raw = rta::update::sha256_hex(std::string("NEW"));
    const std::string zip = rta::update::sha256_hex(std::string("ZIP"));
    return "{\"schema\":2,\"platform\":\"windows\",\"channel\":\"stable\","
           "\"metadata_version\":1,\"version\":\"" + version + "\","
           "\"published_at\":\"2026-11-01T00:00:00Z\",\"expires_at\":\"2027-01-01T00:00:00Z\","
           "\"install_mode\":\"" + mode + "\",\"reason\":\"test reason\","
           "\"files\":[{\"path\":\"" + path + "\",\"sha256\":\"" + raw +
           "\",\"size\":3,\"object\":\"" + raw + ".zip\",\"object_sha256\":\"" + zip +
           "\",\"object_size\":3}],\"delete\":[\"obsolete.dll\"],"
           "\"manual\":{\"cn_url\":\"https://pan.baidu.com/s/test\","
           "\"global_url\":\"https://github.com/okhuoyanyan/RLPlayer2/releases/tag/v" + version +
           "\",\"extraction_code\":\"abcd\",\"sha256\":\"" + zip + "\",\"size\":12345},"
           "\"notes\":\"signed incremental\"}";
}

static long long test_now() {
    return rta::update::parse_rfc3339_epoch("2026-11-03T00:00:00Z");
}

static rta::update::ManifestFile test_file(const std::string& zipBytes) {
    rta::update::ManifestFile file;
    file.path = "app.exe";
    file.sha256 = rta::update::sha256_hex(std::string("NEW"));
    file.size = 3;
    file.object = file.sha256 + ".zip";
    file.objectSha256 = rta::update::sha256_hex(zipBytes);
    file.objectSize = (long long)zipBytes.size();
    return file;
}

struct MockResponse {
    int status = 200;
    std::string body;
    std::string etag;
    std::string retryAfter;
    std::string contentRange;
    long long contentLength = -2;
};

static rta::platform::HttpResult respond(const rta::platform::HttpRequest& request,
                                         const MockResponse& mock) {
    rta::platform::HttpResult result;
    result.status = mock.status;
    result.ok = mock.status >= 200 && mock.status < 300;
    result.etag = mock.etag;
    result.retryAfter = mock.retryAfter;
    result.contentRange = mock.contentRange;
    result.contentLength = mock.contentLength == -2 ? (long long)mock.body.size() : mock.contentLength;
    if (!result.ok) result.error = "HTTP " + std::to_string(mock.status);
    if (request.onChunk && !mock.body.empty())
        request.onChunk(mock.body.data(), mock.body.size());
    if (!request.discardBody)
        result.body.assign(mock.body.begin(), mock.body.end());
    return result;
}

static rta::platform::HttpResult local_winhttp_request(const std::string& response) {
    rta::platform::HttpResult result;
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { result.error = "WSAStartup"; return result; }
    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (listener == INVALID_SOCKET || bind(listener, (sockaddr*)&address, sizeof(address)) != 0 ||
        listen(listener, 1) != 0) {
        if (listener != INVALID_SOCKET) closesocket(listener);
        WSACleanup(); result.error = "local listen"; return result;
    }
    int addressSize = sizeof(address);
    getsockname(listener, (sockaddr*)&address, &addressSize);
    const int port = ntohs(address.sin_port);
    std::thread server([&] {
        fd_set readSet; FD_ZERO(&readSet); FD_SET(listener, &readSet);
        timeval timeout{5, 0};
        if (select(0, &readSet, nullptr, nullptr, &timeout) <= 0) { closesocket(listener); return; }
        SOCKET client = accept(listener, nullptr, nullptr);
        if (client != INVALID_SOCKET) {
            char input[8192];
            const int received = recv(client, input, sizeof(input), 0);
            if (received > 0) {
                size_t sent = 0;
                while (sent < response.size()) {
                    const int n = send(client, response.data() + sent, (int)(response.size() - sent), 0);
                    if (n <= 0) break;
                    sent += (size_t)n;
                }
                shutdown(client, SD_SEND);
            }
            closesocket(client);
        }
        closesocket(listener);
    });
    rta::platform::HttpRequest request;
    request.url = "http://127.0.0.1:" + std::to_string(port) + "/api/v1/update/windows/objects/test.zip";
    request.timeoutMs = 2000;
    request.allowRedirects = false;
    result = rta::platform::http_request(request);
    server.join();
    WSACleanup();
    return result;
}

static int start_slow_receipt_server(std::thread& server, std::string& received, int delayMs) {
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return 0;
    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (listener == INVALID_SOCKET || bind(listener, (sockaddr*)&address, sizeof(address)) != 0 || listen(listener, 1) != 0) {
        if (listener != INVALID_SOCKET) closesocket(listener); WSACleanup(); return 0;
    }
    int size = sizeof(address); getsockname(listener, (sockaddr*)&address, &size);
    const int port = ntohs(address.sin_port);
    server = std::thread([listener, delayMs, &received] {
        SOCKET client = accept(listener, nullptr, nullptr);
        if (client != INVALID_SOCKET) {
            char buffer[8192]; int n = recv(client, buffer, sizeof(buffer), 0);
            if (n > 0) received.assign(buffer, buffer + n);
            for (int reads = 0; reads < 8; ++reads) {
                const size_t headerEnd = received.find("\r\n\r\n");
                if (headerEnd == std::string::npos) { n = recv(client, buffer, sizeof(buffer), 0); if (n <= 0) break; received.append(buffer, buffer + n); continue; }
                const size_t key = received.rfind("Content-Length: ", headerEnd);
                if (key == std::string::npos) break;
                const size_t lineEnd = received.find("\r\n", key);
                long long contentLength = -1;
                if (!rta::update::parse_decimal(received.substr(key + 16, lineEnd - key - 16), contentLength)) break;
                if (received.size() >= headerEnd + 4 + (size_t)contentLength) break;
                n = recv(client, buffer, sizeof(buffer), 0); if (n <= 0) break; received.append(buffer, buffer + n);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
            const std::string response = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\n{}";
            send(client, response.data(), (int)response.size(), 0); shutdown(client, SD_SEND); closesocket(client);
        }
        closesocket(listener); WSACleanup();
    });
    return port;
}

static unsigned long crc32_bytes(const std::string& data) {
    unsigned long crc = 0xfffffffful;
    for (unsigned char byte : data) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320ul & (unsigned long)(-(long)(crc & 1)));
    }
    return crc ^ 0xfffffffful;
}

static void put16(std::string& out, unsigned value) {
    out.push_back((char)(value & 255));
    out.push_back((char)((value >> 8) & 255));
}
static void put32(std::string& out, unsigned long value) {
    put16(out, value & 0xffffu);
    put16(out, (value >> 16) & 0xffffu);
}

static std::string stored_zip(const std::string& name, const std::string& content) {
    std::string out;
    put32(out, 0x04034b50); put16(out, 20); put16(out, 0x800); put16(out, 0);
    put16(out, 0); put16(out, 0); put32(out, crc32_bytes(content));
    put32(out, content.size()); put32(out, content.size()); put16(out, name.size()); put16(out, 0);
    out += name; out += content;
    const unsigned long central = out.size();
    put32(out, 0x02014b50); put16(out, 20); put16(out, 20); put16(out, 0x800); put16(out, 0);
    put16(out, 0); put16(out, 0); put32(out, crc32_bytes(content));
    put32(out, content.size()); put32(out, content.size()); put16(out, name.size());
    put16(out, 0); put16(out, 0); put16(out, 0); put16(out, 0); put32(out, 0x81a40000);
    put32(out, 0); out += name;
    const unsigned long centralSize = out.size() - central;
    put32(out, 0x06054b50); put16(out, 0); put16(out, 0); put16(out, 1); put16(out, 1);
    put32(out, centralSize); put32(out, central); put16(out, 0);
    return out;
}

static int run() {
    using namespace rta::update;
    passes = failures = 0;
    std::printf("== signed update protocol and incremental safety ==\n");

    {
        ManifestExt manifest = parse_manifest_payload(manifest_json(), "3.2.2", test_now(), "stable");
        check(manifest.ok, "valid schema-2 payload accepted");
        check(manifest.ok && !manifest.manual && manifest.files.size() == 1 && manifest.deletes.size() == 1,
              "incremental payload files/delete/manual metadata parsed");
        check(manifest.ok && manifest.files[0].object == manifest.files[0].sha256 + ".zip",
              "deterministic raw-sha object name preserved");
        check(manifest.ok && object_url_for(kDefaultManifestUrl, manifest.files[0].object) ==
                  std::string("https://rlplayer.rainlain.com/api/v1/update/windows/objects/") +
                      manifest.files[0].object,
              "object URL is fixed same-origin and ignores external hosts");
        check(manual_download_url_allowed("https://github.com/owner/repo/releases/download/v1/a.zip"),
              "manual HTTPS GitHub link accepted by separate function");
        check(manual_download_url_allowed("https://pan.baidu.com/s/abc"),
              "manual HTTPS Baidu link accepted by separate function");
        check(!manual_download_url_allowed("http://github.com/owner/repo") &&
                  !manual_download_url_allowed("https://github.com.evil.test/a") &&
                  !manual_download_url_allowed("https://user@github.com/a") &&
                  !manual_download_url_allowed("https://github.com:443/a"),
              "manual links reject HTTP, suffix domains, userinfo and ports");
        check(!update_download_url_allowed("https://github.com/owner/repo/a.zip") &&
                  !update_download_url_allowed("https://raw.githubusercontent.com/a/b/c") &&
                  !update_download_url_allowed("https://rlplayer.rainlain.com.evil.test/api/v1/update/windows/latest"),
              "automatic transport never allows GitHub/raw/full hosts");
        check(!safe_object_name("../x.zip") && !safe_object_name(std::string(64, 'A') + ".zip") &&
                  safe_object_name(std::string(64, '0') + ".zip"),
              "object name requires exactly raw-sha lowercase hex plus .zip");
    }
    {
        const std::string raw = rta::update::sha256_hex(std::string("NEW"));
        const std::string zip = rta::update::sha256_hex(std::string("ZIP"));
        const std::string duplicate = "{\"path\":\"copy.exe\",\"sha256\":\"" + raw +
            "\",\"size\":3,\"object\":\"" + raw + ".zip\",\"object_sha256\":\"" + zip +
            "\",\"object_size\":3}]";
        std::string shared = manifest_json();
        shared.replace(shared.find("}],\"delete\""), 2, "}," + duplicate);
        ManifestExt manifest = parse_manifest_payload(shared, "3.2.2", test_now(), "stable");
        check(manifest.ok && manifest.files.size() == 2,
              "identical raw object may be shared by two install paths: " + manifest.error);
        std::string inconsistent = shared;
        const size_t objectSha = inconsistent.rfind("\"object_sha256\":");
        inconsistent.replace(objectSha, 16, "\"object_sha256\":\"" + std::string(64, '0'));
        check(!parse_manifest_payload(inconsistent, "3.2.2", test_now(), "stable").ok,
              "shared object with inconsistent compressed digest is rejected");
    }

    check(!parse_manifest_payload(manifest_json().replace(manifest_json().find("\"schema\":2"), 10,
                                                         "\"schema\":3"),
                                  "3.2.2", test_now(), "stable").ok,
          "wrong schema rejected");
    check(!parse_manifest_payload(manifest_json(), "3.2.2", test_now(), "beta").ok,
          "channel mismatch rejected");
    {
        std::string value = manifest_json();
        value.replace(value.find("\"platform\":\"windows\""), 20, "\"platform\":\"mac\"");
        check(!parse_manifest_payload(value, "3.2.2", test_now(), "stable").ok,
              "non-Windows platform rejected");
    }
    {
        std::string value = manifest_json();
        value.replace(value.find("\"metadata_version\":1"), 20, "\"metadata_version\":2");
        check(!parse_manifest_payload(value, "3.2.2", test_now(), "stable").ok,
              "unsupported metadata version rejected");
    }
    {
        ManifestExt expired = parse_manifest_payload(manifest_json(), "3.2.2",
            parse_rfc3339_epoch("2027-01-02T00:00:00Z"), "stable");
        check(!expired.ok && expired.errorCode == "expired", "expired signed payload rejected distinctly");
        ManifestExt rollback = parse_manifest_payload(manifest_json(), "3.2.4", test_now(), "stable");
        check(!rollback.ok && rollback.errorCode == "rollback", "version rollback rejected distinctly");
        ManifestExt same = parse_manifest_payload(manifest_json(), "3.2.3", test_now(), "stable");
        check(same.ok, "equal target version remains valid for up-to-date decision");
        ManifestExt manual = parse_manifest_payload(manifest_json("3.2.3", "manual"),
                                                    "3.2.2", test_now(), "stable");
        check(manual.ok && manual.manual && manual.manualInfo.extractionCode == "abcd",
              "explicit manual mode produces manual-only metadata");
        ManifestExt major = parse_manifest_payload(manifest_json("4.0.0"), "3.2.2", test_now(), "stable");
        check(major.ok && major.manual, "cross-major update forced to manual plan");
        ManifestExt runtime = parse_manifest_payload(
            manifest_json("3.2.3", "incremental", "WebView2Runtime/msedgewebview2.exe"),
            "3.2.2", test_now(), "stable");
        check(runtime.ok && !runtime.manual, "WebView2Runtime entries wait for local SHA comparison");
    }
    {
        std::string value = manifest_json("3.2.3", "incremental", "../outside.exe");
        check(!parse_manifest_payload(value, "3.2.2", test_now(), "stable").ok,
              "manifest file traversal rejected");
        value = manifest_json();
        value.insert(value.size() - 1, ",\"object_url\":\"https://evil.test/x.zip\"");
        check(!parse_manifest_payload(value, "3.2.2", test_now(), "stable").ok,
              "unknown external object address rejected by strict parser");
        value = manifest_json();
        value.replace(value.find(".zip\",\"object_sha256"), 4, "-evil.zip");
        check(!parse_manifest_payload(value, "3.2.2", test_now(), "stable").ok,
              "object name must match raw payload sha");
        value = manifest_json();
        value.insert(value.size() - 1, ",\"full\":{\"url\":\"https://github.com/x/y.zip\"}");
        check(!parse_manifest_payload(value, "3.2.2", test_now(), "stable").ok,
              "legacy automatic full fallback rejected");
    }

    const std::string root = join_path(rta::platform::temp_dir_utf8(),
        "update-protocol-test-" + std::to_string(GetCurrentProcessId()));
    rmtree(root);
    ensure_dir(root);
    _putenv_s("RLPLAYER_UPDATE_DIR", root.c_str());
    _putenv_s("RLPLAYER_UPDATE_ALLOW_LOCAL", "1");
    _putenv_s("RLPLAYER_UPDATE_MANIFEST", "http://127.0.0.1:8765/api/v1/update/windows/latest");

    {
        write_text_file(join_path(root, "app.exe"), "OLD");
        write_text_file(join_path(root, "same.dll"), "SAME");
        write_text_file(join_path(root, "rlplayer-settings.conf"), "USER");
        ManifestExt manifest = parse_manifest_payload(manifest_json(), "3.2.2", test_now(), "stable");
        ManifestFile same;
        same.path = "same.dll"; same.sha256 = sha256_hex(std::string("SAME")); same.size = 4;
        same.object = same.sha256 + ".zip"; same.objectSha256 = std::string(64, '1'); same.objectSize = 9;
        ManifestFile user;
        user.path = "rlplayer-settings.conf"; user.sha256 = std::string(64, '2'); user.size = 9;
        user.object = std::string(64, '2') + ".zip"; user.objectSha256 = std::string(64, '3'); user.objectSize = 9;
        manifest.files[0].objectSize = 10;
        manifest.files.push_back(same);
        manifest.files.push_back(user);
        manifest.deletes.push_back("rlplayer-settings.conf");
        Plan plan = plan_incremental(manifest.files, root, manifest.deletes);
        check(plan.ok && plan.toDownload == 1 && plan.unchanged == 1 && plan.protectedSkipped == 1,
              "cross-version plan downloads changed only and never user data");
        check(plan.ok && plan.downloadBytes == 10, "plan transfer bytes use deterministic ZIP object size");
        check(plan.ok && plan.deletes.size() == 1 && plan.protectedDeletes == 1,
              "delete plan skips protected user data");
    }

    {
        const char* queuePaths[] = {"rlplayer-playlist.txt", "nested\\RLPLAYER-PLAYLIST.TXT",
                                   "rlplayer-queue.txt", "nested/rlplayer-queue.txt"};
        const std::string queueBytes = "# RLPlayer playlist v1\nindex=0\nrepeat=0\nshuffle=0\nitem=private.wav\n";
        ensure_dir(join_path(root, "nested"));
        for (const char* path : queuePaths) {
            const std::string diskPath = join_path(root, norm_rel(path));
            write_text_file(diskPath, queueBytes);
            auto replacement = parse_manifest_payload(
                manifest_json("3.2.3", "incremental", json_escape(path)), "3.2.2", test_now(), "stable");
            auto replacePlan = plan_incremental(replacement.files, root);
            check(replacement.ok && replacePlan.ok && replacePlan.toDownload == 0 &&
                      replacePlan.missing.empty() && replacePlan.protectedSkipped == 1,
                  std::string("manifest replacement preserves queue basename: ") + path);
            std::string deletionJson = manifest_json();
            deletionJson.replace(deletionJson.find("obsolete.dll"), std::strlen("obsolete.dll"), json_escape(path));
            auto deletion = parse_manifest_payload(deletionJson, "3.2.2", test_now(), "stable");
            auto deletePlan = plan_incremental(deletion.files, root, deletion.deletes);
            check(deletion.ok && deletePlan.ok && deletePlan.deletes.empty() && deletePlan.protectedDeletes == 1,
                  std::string("manifest deletion preserves queue basename: ") + path);
            check(read_text_file(diskPath) == queueBytes, std::string("queue bytes remain intact: ") + path);
        }
        check(!is_protected_rel("nested/rlplayer-playlist.txt.backup"),
              "queue protection matches the whole basename");
    }

    {
        const std::string runtimePath = "WebView2Runtime\\update-probe.txt";
        ensure_dir(join_path(root, "WebView2Runtime"));
        write_text_file(join_path(root, runtimePath), "SAME");
        ManifestFile app = test_file("ZIP");
        ManifestFile runtimeFile;
        runtimeFile.path = runtimePath; runtimeFile.sha256 = sha256_hex(std::string("SAME")); runtimeFile.size = 4;
        runtimeFile.object = runtimeFile.sha256 + ".zip";
        runtimeFile.objectSha256 = sha256_hex(std::string("RUNTIME-ZIP")); runtimeFile.objectSize = 11;
        Plan unchangedRuntime = plan_incremental({app, runtimeFile}, root);
        check(unchangedRuntime.ok && !unchangedRuntime.manual && unchangedRuntime.toDownload == 1 &&
                  unchangedRuntime.protectedSkipped == 1,
              "unchanged WebView2Runtime permits incremental app update without runtime download");
        runtimeFile.sha256 = sha256_hex(std::string("DIFFERENT")); runtimeFile.size = 9;
        runtimeFile.object = runtimeFile.sha256 + ".zip";
        Plan changedRuntime = plan_incremental({app, runtimeFile}, root);
        check(changedRuntime.ok && changedRuntime.manual && changedRuntime.reason == "webview2-update" &&
                  changedRuntime.toDownload == 1 && changedRuntime.protectedSkipped == 1 && changedRuntime.missing.size() == 1,
              "changed WebView2Runtime forces manual plan and never schedules runtime object");
        Plan deletedRuntime = plan_incremental({}, root, {runtimePath});
        check(deletedRuntime.ok && deletedRuntime.manual && deletedRuntime.reason == "webview2-update" &&
                  deletedRuntime.deletes.empty() && deletedRuntime.protectedDeletes == 1,
              "WebView2Runtime deletion forces manual plan without automatic delete");
    }

    {
        const std::string outside = join_path(root, "outside.txt");
        const std::string badZip = join_path(root, "traversal.zip");
        write_text_file(badZip, stored_zip("../outside.txt", "EVIL"));
        std::string error;
        check(!zip_validate_single_file(badZip, "../outside.txt", 4, error),
              "ZIP ../ traversal rejected before extraction");
        check(!path_exists(outside), "ZIP traversal never creates outside file");
        const std::string goodZip = join_path(root, "good.zip");
        write_text_file(goodZip, stored_zip("payload.bin", "NEW"));
        const bool validStoredZip = zip_validate_single_file(goodZip, "payload.bin", 3, error);
        check(validStoredZip, "deterministic single-file stored ZIP accepted: " + error);
        const std::string unpacked = join_path(root, "unpacked");
        const std::string destination = join_path(root, "staging/app.exe");
        ManifestFile file = test_file(read_text_file(goodZip));
        check(extract_verified_object(goodZip, file, unpacked, destination, error) &&
                  read_text_file(destination) == "NEW",
              "safe extraction rechecks raw sha and size: " + error);
        check(extract_verified_object(goodZip, file, unpacked, destination, error),
              "repeat extraction is deterministic");
    }

    auto& transport = rta::net::http_detail::transport_for_test();
    const std::string zipBytes = stored_zip("app.exe", "NEW");
    const ManifestFile file = test_file(zipBytes);
    const std::string localManifest = "http://127.0.0.1:8765/api/v1/update/windows/latest";

    {
        const std::string work = join_path(root, "range-200"); ensure_dir(work);
        write_text_file(join_path(work, file.object + ".part"), "OLD-PART-MUST-NOT-SURVIVE");
        std::vector<rta::platform::HttpRequest> requests;
        transport = [&](const rta::platform::HttpRequest& request) {
            requests.push_back(request);
            return respond(request, MockResponse{200, zipBytes, "\"new\"", "", "", -2});
        };
        auto result = download_object_with_resume(file, localManifest, work, 1000);
        check(result.ok && read_text_file(result.path) == zipBytes,
              "Range request answered 200 truncates old .part instead of appending");
        check(!requests.empty() && requests[0].url ==
                  "http://127.0.0.1:8765/api/v1/update/windows/objects/" + file.object,
              "Range object request stays on fixed local test origin");
    }
    {
        const std::string work = join_path(root, "range-corrupt-full"); ensure_dir(work);
        std::string corrupt = zipBytes; corrupt[0] ^= 1;
        write_text_file(join_path(work, file.object + ".part"), corrupt);
        int calls = 0;
        transport = [&](const rta::platform::HttpRequest& request) {
            ++calls; return respond(request, MockResponse{200, zipBytes, "", "", "", -2});
        };
        auto result = download_object_with_resume(file, localManifest, work, 1000);
        check(result.ok && calls == 1 && read_text_file(result.path) == zipBytes,
              "complete-size but SHA-corrupt .part is discarded and fetched again");
    }
    {
        const std::string work = join_path(root, "range-206"); ensure_dir(work);
        write_text_file(join_path(work, file.object + ".part"), zipBytes.substr(0, 5));
        write_text_file(join_path(work, file.object + ".part.meta"), "etag=\"v1\"\r\n");
        std::string range, ifRange;
        transport = [&](const rta::platform::HttpRequest& request) {
            for (const auto& h : request.headers) {
                if (h.rfind("Range:", 0) == 0) range = h;
                if (h.rfind("If-Range:", 0) == 0) ifRange = h;
            }
            MockResponse mock{206, zipBytes.substr(5), "\"v1\"", "",
                "bytes 5-" + std::to_string(zipBytes.size() - 1) + "/" +
                    std::to_string(zipBytes.size()), -2};
            return respond(request, mock);
        };
        auto result = download_object_with_resume(file, localManifest, work, 1000);
        check(result.ok && read_text_file(result.path) == zipBytes,
              "206 Content-Range resumes to byte-identical object");
        check(range == "Range: bytes=5-" && ifRange == "If-Range: \"v1\"",
              "resume emits Range and persisted ETag If-Range");
    }
    {
        const std::string work = join_path(root, "range-bad-206"); ensure_dir(work);
        write_text_file(join_path(work, file.object + ".part"), zipBytes.substr(0, 5));
        transport = [&](const rta::platform::HttpRequest& request) {
            MockResponse mock{206, zipBytes.substr(5), "", "",
                "bytes 4-" + std::to_string(zipBytes.size() - 2) + "/" +
                    std::to_string(zipBytes.size()), -2};
            return respond(request, mock);
        };
        auto result = download_object_with_resume(file, localManifest, work, 1000);
        check(!result.ok && result.errorCode == "bad-content-range",
              "206 wrong offset/total rejected without final object");
    }
    {
        const std::string work = join_path(root, "range-416"); ensure_dir(work);
        write_text_file(join_path(work, file.object + ".part"), "BAD");
        int calls = 0;
        std::string secondRange = "missing";
        transport = [&](const rta::platform::HttpRequest& request) {
            ++calls;
            if (calls == 1) {
                MockResponse mock{416, "", "", "", "bytes */" + std::to_string(zipBytes.size()), 0};
                return respond(request, mock);
            }
            for (const auto& h : request.headers) if (h.rfind("Range:", 0) == 0) secondRange = h;
            return respond(request, MockResponse{200, zipBytes, "", "", "", -2});
        };
        auto result = download_object_with_resume(file, localManifest, work, 1000);
        check(result.ok && calls == 2 && secondRange == "missing",
              "416 discards unsatisfiable part and retries once without Range");
    }
    {
        const std::string work = join_path(root, "range-429"); ensure_dir(work);
        int calls = 0;
        transport = [&](const rta::platform::HttpRequest& request) {
            ++calls;
            return respond(request, MockResponse{429, "{\"errorCode\":\"quota_exceeded\"}",
                "", "0", "", -2});
        };
        auto result = download_object_with_resume(file, localManifest, work, 1000);
        check(!result.ok && result.errorCode == "quota_exceeded" && calls >= 1 && calls <= 4,
              "429 quota_exceeded bounded and never falls back to full/GitHub");
    }

    {
        int callsBefore = 0;
        transport = [&](const rta::platform::HttpRequest& request) {
            ++callsBefore;
            return respond(request, MockResponse{500, "must-not-download", "", "", "", -2});
        };
        auto& updateState = state();
        {
            std::lock_guard<std::mutex> lock(updateState.mtx);
            updateState.downloading = false;
            updateState.cr.decision = Decision::UpdateAvailable;
            updateState.cr.remoteVersion = "3.2.3";
            updateState.man = parse_manifest_payload(manifest_json("3.2.3", "incremental"), "3.2.2", test_now(), "stable");
            updateState.plan = Plan{};
            updateState.plan.manual = true;
            updateState.plan.reason = "webview2-update";
        }
        const std::string manualJson = check_json();
        rta::net::Json manualDocument;
        check(rta::net::json_parse_strict(manualJson, manualDocument) && manualDocument.type == rta::net::Json::Obj,
              "check_json is strict valid JSON");
        check(manualJson.find("\"manual\":true") != std::string::npos &&
                  manualJson.find("\"installMode\":\"manual\"") != std::string::npos &&
                  manualJson.find("\"cn\":\"https://pan.baidu.com/s/test\"") != std::string::npos &&
                  manualJson.find("\"global\":\"https://github.com/okhuoyanyan/RLPlayer2/releases/tag/v3.2.3\"") != std::string::npos &&
                  manualJson.find("\"extractionCode\":\"abcd\"") != std::string::npos &&
                  manualJson.find("\"fullSize\":12345") != std::string::npos &&
                  manualJson.find("\"fullBytes\":12345") != std::string::npos &&
                  manualJson.find("\"reason\":\"webview2-update\"") != std::string::npos &&
                  manualJson.find("\"filesToDownload\":0") != std::string::npos,
              "plan-driven manual check_json exposes runtime reason, rescue links and zero auto downloads");
        check(manualJson.find("\"fullSha\":\"" + sha256_hex(std::string("ZIP")) + "\"") != std::string::npos,
              "manual fullSha is link metadata, not a server full fallback");
        std::string error;
        check(!start_download(error) && callsBefore == 0,
              "manual plan never triggers automatic object/full/manual-link download");
    }

    {
        bool posted = false;
        std::string reportUrl, reportBody, reportMethod;
        transport = [&](const rta::platform::HttpRequest& request) {
            posted = true; reportUrl = request.url; reportBody = request.body; reportMethod = request.method;
            return respond(request, MockResponse{200, "{}", "", "", "", -2});
        };
        const bool ok = post_update_report(kDefaultManifestUrl, "client-test", "3.2.2", "3.2.3",
                                           false, 1234, "rollback");
        check(ok && posted && reportMethod == "POST" &&
                  reportUrl == "https://rlplayer.rainlain.com/api/v1/update/windows/report",
              "failure receipt POSTs only to fixed production report endpoint through test transport");
        check(reportBody.find("\"client\":\"client-test\"") != std::string::npos &&
                  reportBody.find("\"from\":\"3.2.2\"") != std::string::npos &&
                  reportBody.find("\"to\":\"3.2.3\"") != std::string::npos &&
                  reportBody.find("\"ok\":false") != std::string::npos &&
                  reportBody.find("\"ms\":1234") != std::string::npos &&
                  reportBody.find("\"errorCode\":\"rollback\"") != std::string::npos,
              "receipt contains exactly client/from/to/ok/ms/errorCode data");
        std::string clickUrl, clickBody;
        transport = [&](const rta::platform::HttpRequest& request) {
            clickUrl = request.url; clickBody = request.body;
            return respond(request, MockResponse{200, "{}", "", "", "", -2});
        };
        check(update_download_url_allowed("https://rlplayer.rainlain.com/api/v1/events/external"),
              "external click route is admitted by production URL policy");
        check(post_external_click(kDefaultManifestUrl, "baidu") &&
                  clickUrl == "https://rlplayer.rainlain.com/api/v1/events/external" &&
                  clickBody == "{\"target\":\"baidu\"}",
              "manual external click is reported without downloading the external package: " + clickUrl + " body=" + clickBody);
        check(!post_external_click(kDefaultManifestUrl, "invalid"),
              "invalid external click target is rejected");
    }

    transport = nullptr;
    {
        std::thread slowServer;std::string received;
        const int port=start_slow_receipt_server(slowServer,received,1000);
        check(port>0,"slow local receipt server started");
        if(port>0){
            _putenv_s("RLPLAYER_UPDATE_MANIFEST",("http://127.0.0.1:"+std::to_string(port)+"/api/v1/update/windows/latest").c_str());
            const auto begin=std::chrono::steady_clock::now();
            report_update_async("3.2.2","3.2.3",true,1234,"");
            const auto elapsed=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-begin).count();
            slowServer.join();
            check(elapsed<500,"success receipt call returns before 1000ms server responds: elapsed="+std::to_string(elapsed));
            check(received.rfind("POST /api/v1/update/windows/report ",0)==0&&
                      received.find("\"ok\":true")!=std::string::npos&&received.find("\"ms\":1234")!=std::string::npos,
                  "success receipt is really POSTed: "+received.substr(0,300));
            _putenv_s("RLPLAYER_UPDATE_MANIFEST","http://127.0.0.1:8765/api/v1/update/windows/latest");
        }
    }
    {
        const std::string response =
            "HTTP/1.1 206 Partial Content\r\nContent-Type: application/zip\r\n"
            "Content-Length: 3\r\nContent-Range: bytes 5-7/20\r\nETag: \"v1\"\r\n"
            "Retry-After: 7\r\nConnection: close\r\n\r\nxyz";
        const auto actual = local_winhttp_request(response);
        const std::string body(actual.body.begin(), actual.body.end());
        check(actual.ok && actual.status == 206 && body == "xyz" && actual.etag == "\"v1\"" &&
                  actual.retryAfter == "7" && actual.contentRange == "bytes 5-7/20" &&
                  actual.contentLength == 3,
              "real WinHTTP exposes bounded ETag/Retry-After/Content-Range/Content-Length");
        const std::string redirect =
            "HTTP/1.1 302 Found\r\nLocation: http://127.0.0.1:1/not-followed\r\n"
            "Content-Length: 0\r\nConnection: close\r\n\r\n";
        const auto redirected = local_winhttp_request(redirect);
        check(redirected.status == 302 && !redirected.ok,
              "signed-update redirect policy does not follow even to another local endpoint");
    }
    {
        const std::string version = "3.2.3";
        const std::string staging = join_path(version_dir(version), "staging");
        rmtree(join_path(version_dir(version), "staging"));
        ensure_dir(staging);
        write_text_file(join_path(root, "app.exe"), "OLD");
        write_text_file(join_path(root, "obsolete.dll"), "DELETE-ME");
        write_text_file(join_path(root, "rlplayer-settings.conf"), "USER-DATA");
        write_text_file(join_path(staging, "app.exe"), "NEW");
        std::string marker = "version=" + version + "\r\nfile=app.exe\t3\t" +
                             sha256_hex(std::string("NEW")) +
                             "\r\ndelete=obsolete.dll\r\ndelete=rlplayer-settings.conf\r\n";
        write_text_file(staging_marker_path(staging), marker);
        std::string error;
        check(write_ready_plan(version, "3.2.2", staging, error), "delete transaction ledger ready");
        apply_pending_on_startup();
        PendingPlan applied = read_pending();
        check(applied.phase == "applied" && read_text_file(join_path(root, "app.exe")) == "NEW" &&
                  !path_exists(join_path(root, "obsolete.dll")),
              "launcher transaction installs new file and deletes listed obsolete file");
        check(read_text_file(join_path(root, "rlplayer-settings.conf")) == "USER-DATA" &&
                  std::find(applied.deleted.begin(), applied.deleted.end(), "rlplayer-settings.conf") ==
                      applied.deleted.end(),
              "transaction delete list cannot touch protected user data");
        rollback_pending("protocol-test");
        check(read_pending().phase == "rolled_back" && read_text_file(join_path(root, "app.exe")) == "OLD" &&
                  read_text_file(join_path(root, "obsolete.dll")) == "DELETE-ME" &&
                  read_text_file(join_path(root, "rlplayer-settings.conf")) == "USER-DATA",
              "rollback restores replacements and transactional deletions without user-data regression");
    }

    transport = nullptr;
    _putenv_s("RLPLAYER_UPDATE_ALLOW_LOCAL", "");
    _putenv_s("RLPLAYER_UPDATE_MANIFEST", "");
    _putenv_s("RLPLAYER_UPDATE_DIR", "");
    rmtree(root);
    std::printf("protocol tests: PASS %d / FAIL %d\n", passes, failures);
    return failures;
}

} // namespace protocol_tests

#ifdef RLPLAYER_UPDATE_PROTOCOL_TEST_MAIN
int main() {
    try {
        return protocol_tests::run() == 0 ? 0 : 1;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "protocol test setup failed: %s\n", e.what());
        return 2;
    }
}
#endif
