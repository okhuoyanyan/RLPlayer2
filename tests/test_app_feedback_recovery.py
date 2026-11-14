"""Windows feedback contract tests with synthetic data and injected transport only."""
import os
import pathlib
import subprocess
import sys
import tempfile

root = pathlib.Path(os.environ.get('RLPLAYER_TEST_SOURCE_ROOT', pathlib.Path(__file__).resolve().parents[1]))
source = (root / 'src/net/feedback.h').read_text(encoding='utf-8')
static_failures = []
for forbidden in (
    'https://rlplayer-feedback.okhuoyanyan.workers.dev',
    'https://api.github.com/repos/', 'GITHUB_TOKEN', 'RLPLAYER_GH_TOKEN',
    'find_executable("gh")', 'http_put_once', 'skipTokenLookup', 'workerUrl', '["issue"]',
):
    if forbidden in source:
        static_failures.append('forbidden production fallback: ' + forbidden)
for required in (
    'https://rlplayer.rainlain.com/api/v1/feedback/windows',
    'BCryptGenRandom', 'Idempotency-Key: ', 'X-RLPlayer-Client: ',
    'web_submit_start(', 'web_submit_status(',
):
    if required not in source:
        static_failures.append('missing production contract: ' + required)
protected = '\n'.join((root / 'src/net' / name).read_text(encoding='utf-8-sig')
                      for name in ('update.h', 'update_apply.h', 'update_check.h'))
if '"rlplayer-feedback.conf"' not in protected:
    static_failures.append('feedback config is not update-protected')
for failure in static_failures:
    print('FAIL ' + failure)
if static_failures:
    sys.exit(1)
print('PASS static production endpoint, no legacy fallback, CSPRNG and update protection')

code = r'''
#include "net/feedback.h"
#include <condition_variable>
#include <cstdio>
#include <regex>

using namespace rta::feedback;
using rta::net::Json;
using rta::net::json_parse_strict;

namespace {

int checks = 0, failures = 0;
void check(bool ok, const char* label) {
    ++checks;
    if (!ok) ++failures;
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", label);
}

std::string header(const ServerRequest& request, const std::string& name) {
    const std::string prefix = name + ": ";
    for (const auto& value : request.headers)
        if (value.rfind(prefix, 0) == 0) return value.substr(prefix.size());
    return {};
}

std::string base64_decode(const std::string& input) {
    static const std::string alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    unsigned value = 0;
    int bits = 0;
    for (unsigned char c : input) {
        if (c == '=') break;
        const size_t pos = alphabet.find(c);
        if (pos == std::string::npos) return {};
        value = (value << 6) | (unsigned)pos;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += (char)((value >> bits) & 255);
        }
    }
    return out;
}

struct FakeTransport {
    std::vector<ServerRequest> requests;
    std::vector<PutResult> replies;
    PutResult call(const ServerRequest& request) {
        requests.push_back(request);
        if (replies.empty()) throw std::runtime_error("unexpected synthetic transport call");
        const PutResult reply = replies.front();
        replies.erase(replies.begin());
        return reply;
    }
};

Options options_for(const std::filesystem::path& dir, std::shared_ptr<FakeTransport> fake) {
    Options opt;
    opt.configPath = (dir / "rlplayer-feedback.conf").string();
    opt.outboxDir = (dir / "outbox").string();
    opt.transport = [fake](const ServerRequest& request) { return fake->call(request); };
    return opt;
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    std::ostringstream text;
    text << file.rdbuf();
    return text.str();
}

struct BlockingTransport {
    std::mutex mutex;
    std::condition_variable changed;
    bool release = false;
    int calls = 0;
    ServerRequest lastRequest;
    PutResult call(const ServerRequest& request) {
        std::unique_lock<std::mutex> lock(mutex);
        ++calls;
        lastRequest = request;
        changed.notify_all();
        changed.wait(lock, [&] { return release; });
        return {true, 201, "", "{\"ok\":true,\"id\":\"FB-async\",\"diagStored\":true}"};
    }
};

int exit_with_pending(const char* directory) {
    const std::filesystem::path dir(directory);
    auto fake = std::make_shared<FakeTransport>();
    fake->replies.push_back({true, 201, "", "{\"ok\":true,\"id\":\"FB-late\",\"diagStored\":true}"});
    Options opt = options_for(dir, fake);
    opt.transport = [fake](const ServerRequest& request) {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        return fake->call(request);
    };
    LiveState live;
    live.gpu = "offline fixture";
    const std::string started = web_submit_start("{\"title\":\"pending exit fixture\"}", live, opt);
    Json json;
    check(json_parse_strict(started, json) && json["accepted"].type == Json::Bool && json["accepted"].b,
          "detached pending job was accepted before process exit");
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc > 2 && std::string(argv[2]) == "--exit-with-pending") return exit_with_pending(argv[1]);
    const std::filesystem::path dir(argv[1]);
    UserReport user;
    user.type = "bug";
    user.title = "Offline synthetic report";
    user.body = "Synthetic details";
    user.steps = "Synthetic steps";
    LiveState live;
    live.source = "offline-test";
    live.gpu = "offline fixture";

    auto fake = std::make_shared<FakeTransport>();
    Options opt = options_for(dir, fake);
    opt.idempotencyKey = "offline-idempotency-0001";
    fake->replies.push_back({true, 201, "", "{\"ok\":true,\"id\":\"FB-windows-1\",\"diagStored\":true,\"issue\":999}"});
    Result first = submit(user, live, opt);
    check(first.uploaded && first.status == 201 && first.id == "FB-windows-1" && first.diagStored,
          "201 strict acknowledgement succeeds without depending on issue field");
    check(fake->requests.size() == 1 && fake->requests[0].url == kServerUrlDefault,
          "default request targets the Windows feedback endpoint exactly once");
    check(fake->requests[0].timeoutMs == 120000, "default single-upload timeout is 120 seconds");
    const std::string expectedAgent = std::string("RLPlayer/") + RLPLAYER_VERSION;
    check(header(fake->requests[0], "User-Agent") == expectedAgent && fake->requests[0].userAgent == expectedAgent,
          "User-Agent is RLPlayer/<version> in both transport and explicit header");
    check(std::regex_match(first.client, std::regex("[0-9a-f]{32}")) &&
          header(fake->requests[0], "X-RLPlayer-Client") == first.client,
          "persistent 32-hex client is sent in X-RLPlayer-Client");
    check(header(fake->requests[0], "Idempotency-Key") == opt.idempotencyKey,
          "Idempotency-Key header uses the request idempotency key");
    Json uploaded;
    check(json_parse_strict(fake->requests[0].body, uploaded) && uploaded.type == Json::Obj,
          "upload body is strict JSON");
    check(uploaded["text"].s().find("Synthetic steps") != std::string::npos &&
          uploaded["version"].s() == RLPLAYER_VERSION && uploaded["os"].s().find("Windows") == 0,
          "upload contains text, version and Windows os fields");
    check(uploaded["client"].s() == first.client && uploaded["idempotency_key"].s() == opt.idempotencyKey,
          "JSON client and idempotency_key equal their request headers");
    check(base64_decode(uploaded["diag"].s()) == first.json && !first.json.empty(),
          "diag is base64 of the complete diagnostic JSON without truncation");
    Payload bigPayload;
    bigPayload.id = "offline-8mib";
    bigPayload.json = "{\"pad\":\"" +
        std::string(8 * 1024 * 1024 - std::string("{\"pad\":\"").size() - 2, 'x') + "\"}";
    const ServerRequest bigRequest = make_server_request(bigPayload, user, live, opt, kServerUrlDefault,
                                                          std::string(32, 'a'), "offline-big-key");
    Json bigJson;
    check(bigPayload.json.size() == 8 * 1024 * 1024 && bigRequest.timeoutMs == 120000 &&
          json_parse_strict(bigRequest.body, bigJson) && base64_decode(bigJson["diag"].s()) == bigPayload.json,
          "8 MiB diagnostic remains complete after base64 encoding with the 120-second timeout");

    fake->replies.push_back({true, 200, "", "{\"ok\":true,\"id\":\"FB-windows-1\",\"diagStored\":false}"});
    Result duplicate = submit(user, live, opt);
    check(duplicate.uploaded && duplicate.status == 200 && duplicate.id == first.id && !duplicate.diagStored,
          "idempotent duplicate accepts HTTP 200 with the same id and boolean diagStored");
    check(fake->requests.size() == 2 && fake->requests[1].body.find(opt.idempotencyKey) != std::string::npos &&
          duplicate.client == first.client,
          "manual duplicate reuses the explicit idempotency key and persistent client");
    const std::string config = read_file(dir / "rlplayer-feedback.conf");
    const std::regex clientAssignment("client=");
    check(std::regex_search(config, std::regex("client=[0-9a-f]{32}")) &&
          std::distance(std::sregex_iterator(config.begin(), config.end(), clientAssignment), std::sregex_iterator()) == 1,
          "configuration persists exactly one random client across submissions");
    Json duplicateRequest;
    check(json_parse_strict(fake->requests[1].body, duplicateRequest) &&
          duplicateRequest["idempotency_key"].s() == uploaded["idempotency_key"].s(),
          "both submissions carry byte-identical idempotency_key values");

    for (const std::string response : {
        "{\"ok\":false,\"error\":\"rejected\",\"retryAfter\":0}", "<html>proxy page</html>", "{}",
        "{\"ok\":true,\"diagStored\":true}", "{\"ok\":true,\"id\":42,\"diagStored\":true}",
        "{\"ok\":true,\"id\":\"FB-invalid\"}", "{\"ok\":true,\"id\":\"FB-invalid\",\"diagStored\":\"true\"}",
        "{\"ok\":true,\"id\":\"FB-invalid\",\"diagStored\":true} trailing"
    }) {
        auto localFake = std::make_shared<FakeTransport>();
        localFake->replies.push_back({true, 200, "", response});
        Options local = options_for(dir, localFake);
        Result result = submit(user, live, local);
        check(!result.uploaded && result.status == 200 && !result.error.empty() && !result.localPath.empty() &&
              localFake->requests.size() == 1,
              "invalid HTTP 200 acknowledgement fails closed, writes outbox and is not replayed");
    }
    {
        auto localFake = std::make_shared<FakeTransport>();
        localFake->replies.push_back({true, 202, "", "{\"ok\":true,\"id\":\"FB-202\",\"diagStored\":true}"});
        Options local = options_for(dir, localFake);
        Result result = submit(user, live, local);
        check(!result.uploaded && result.status == 202 && !result.localPath.empty(),
              "only 201 and idempotent 200 are successful acknowledgements");
    }

    for (int status : {413, 429, 503}) {
        auto localFake = std::make_shared<FakeTransport>();
        const std::string reason = status == 429 ? "rate_limited" : (status == 413 ? "too_large" : "unavailable");
        localFake->replies.push_back({false, status, "HTTP " + std::to_string(status),
            "{\"ok\":false,\"error\":\"" + reason + "\",\"retryAfter\":60}"});
        Options local = options_for(dir, localFake);
        Result result = submit(user, live, local);
        check(!result.uploaded && result.status == status && result.error == reason && result.retryAfter == 60 &&
              !result.localPath.empty() && localFake->requests.size() == 1,
              "413/429/503 server JSON and retryAfter survive; failure writes outbox once");
    }
    for (const char* reason : {"synthetic DNS failure", "synthetic timeout"}) {
        auto localFake = std::make_shared<FakeTransport>();
        localFake->replies.push_back({false, 0, reason, ""});
        Options local = options_for(dir, localFake);
        Result result = submit(user, live, local);
        check(!result.uploaded && result.status == 0 && result.error.find(reason) != std::string::npos &&
              !result.localPath.empty() && localFake->requests.size() == 1,
              "DNS/timeout network error is honest, recoverable and never automatically retried");
    }

    {
        std::ofstream(dir / "disabled.conf") << "server=none\n";
        auto localFake = std::make_shared<FakeTransport>();
        Options local = options_for(dir, localFake);
        local.configPath = (dir / "disabled.conf").string();
        Result result = submit(user, live, local);
        check(!result.uploaded && localFake->requests.empty() && !result.localPath.empty(),
              "server=none disables transport and retains the report locally");
    }
    {
        std::ofstream(dir / "legacy-disabled.conf") << "server=https://offline.fixture/feedback\nworker=none\n";
        auto localFake = std::make_shared<FakeTransport>();
        Options local = options_for(dir, localFake);
        local.configPath = (dir / "legacy-disabled.conf").string();
        Result result = submit(user, live, local);
        check(!result.uploaded && localFake->requests.empty() && !result.localPath.empty(),
              "legacy worker=none only disables upload and cannot enable another backend");
    }
    {
        std::ofstream(dir / "legacy-url.conf") << "worker=https://legacy.invalid/do-not-use\n";
        auto localFake = std::make_shared<FakeTransport>();
        localFake->replies.push_back({true, 201, "", "{\"ok\":true,\"id\":\"FB-default\",\"diagStored\":true}"});
        Options local = options_for(dir, localFake);
        local.configPath = (dir / "legacy-url.conf").string();
        Result result = submit(user, live, local);
        check(result.uploaded && localFake->requests.size() == 1 && localFake->requests[0].url == kServerUrlDefault,
              "legacy worker URL is ignored rather than contacted or used as a fallback");
    }
    {
        std::ofstream(dir / "plain.conf") << "server=http://127.0.0.1:9/feedback\n";
        auto localFake = std::make_shared<FakeTransport>();
        Options local = options_for(dir, localFake);
        local.configPath = (dir / "plain.conf").string();
        Result result = submit(user, live, local);
        check(!result.uploaded && localFake->requests.empty() && !result.localPath.empty() &&
              result.error.find("https://") != std::string::npos,
              "plaintext server is rejected without default-endpoint fallback");
    }
    {
        _putenv_s("RLPLAYER_FEEDBACK_ALLOW_LOCAL", "1");
        check(valid_server_url("http://127.0.0.1:18765/api/v1/feedback/windows"),
              "explicit local test flag accepts an exact loopback feedback endpoint");
        for (const std::string url : {
            "http://localhost.example.invalid/feedback", "http://127.0.0.1@other.invalid/feedback",
            "http://127.0.0.10/feedback", "http://127.0.0.1:99999/feedback", "http://127.0.0.1"
        }) check(!valid_server_url(url), "local feedback test flag cannot expand to another host or malformed authority");
        _putenv_s("RLPLAYER_FEEDBACK_ALLOW_LOCAL", "");
        check(!valid_server_url("http://127.0.0.1:18765/api/v1/feedback/windows"),
              "loopback feedback is disabled again after the explicit test flag is cleared");
    }
    {
        auto fakeA = std::make_shared<FakeTransport>();
        auto fakeB = std::make_shared<FakeTransport>();
        Options a = options_for(dir, fakeA), b = options_for(dir, fakeB);
        a.configPath = (dir / "client-a.conf").string();
        b.configPath = (dir / "client-b.conf").string();
        a.serverUrl = b.serverUrl = "none";
        Result ra = submit(user, live, a), rb = submit(user, live, b);
        check(valid_client_id(ra.client) && valid_client_id(rb.client) && ra.client != rb.client,
              "independent installations receive independent CSPRNG client identifiers");
    }
    {
        const auto blocked = dir / "blocked-outbox";
        std::ofstream(blocked) << "file";
        auto localFake = std::make_shared<FakeTransport>();
        localFake->replies.push_back({false, 503, "unavailable", ""});
        Options local = options_for(dir, localFake);
        local.outboxDir = blocked.string();
        Result result = submit(user, live, local);
        check(!result.uploaded && result.localPath.empty() && result.message.find("本地兜底也没写成") != std::string::npos,
              "unwritable outbox reports both upload and local persistence failures");
    }

    auto blocking = std::make_shared<BlockingTransport>();
    Options asyncOpt = options_for(dir, nullptr);
    asyncOpt.transport = [blocking](const ServerRequest& request) { return blocking->call(request); };
    const auto asyncStarted = std::chrono::steady_clock::now();
    const std::string acceptedText = web_submit_start(
        "{\"type\":\"bug\",\"title\":\"Async offline report\",\"body\":\"Synthetic\","
        "\"idempotency_key\":\"ui-stable-key\"}", live, asyncOpt);
    const auto acceptMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - asyncStarted).count();
    Json accepted;
    check(json_parse_strict(acceptedText, accepted) && accepted["ok"].type == Json::Bool && accepted["ok"].b &&
          accepted["accepted"].type == Json::Bool && accepted["accepted"].b && !accepted["job"].s().empty(),
          "web_submit_start returns the 202-semantics accepted/job contract");
    check(acceptMs < 1000, "web_submit_start does not wait for the blocked upload");
    const std::string job = accepted["job"].s();
    {
        std::unique_lock<std::mutex> lock(blocking->mutex);
        blocking->changed.wait_for(lock, std::chrono::seconds(5), [&] { return blocking->calls == 1; });
    }
    Json pending;
    check(blocking->calls == 1 && json_parse_strict(web_submit_status(job), pending) &&
          pending["ok"].type == Json::Bool && pending["ok"].b &&
          pending["done"].type == Json::Bool && !pending["done"].b,
          "status reports done:false while the real background transport is blocked");
    {
        std::lock_guard<std::mutex> lock(blocking->mutex);
        blocking->release = true;
        blocking->changed.notify_all();
    }
    Json done;
    for (int i = 0; i < 500; ++i) {
        json_parse_strict(web_submit_status(job), done);
        if (done["done"].type == Json::Bool && done["done"].b) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    check(done["ok"].type == Json::Bool && done["ok"].b && done["done"].type == Json::Bool && done["done"].b &&
          done["result"].type == Json::Obj && done["result"]["uploaded"].type == Json::Bool &&
          done["result"]["uploaded"].b && done["result"]["id"].s() == "FB-async" &&
          done["result"]["status"].n() == 201,
          "real asynchronous job completes with the original result_json object");
    check(done["result"]["idempotency_key"].s() == "ui-stable-key" &&
          header(blocking->lastRequest, "Idempotency-Key") == "ui-stable-key",
          "asynchronous web input preserves the UI idempotency key through to the server request");
    Json retained;
    check(json_parse_strict(web_submit_status(job), retained) && retained["result"]["id"].s() == "FB-async" &&
          blocking->calls == 1,
          "polling a completed job does not replay its transport");
    auto& registry = detail::async_registry();
    {
        std::lock_guard<std::mutex> lock(registry.mutex);
        registry.jobs[job]->completed = std::chrono::steady_clock::now() - std::chrono::minutes(10);
    }
    check(json_parse_strict(web_submit_status(job), retained) && retained["ok"].type == Json::Bool && retained["ok"].b,
          "completed result remains available at ten minutes");
    {
        std::lock_guard<std::mutex> lock(registry.mutex);
        registry.jobs[job]->completed = std::chrono::steady_clock::now() - std::chrono::minutes(16);
    }
    Json expired;
    check(json_parse_strict(web_submit_status(job), expired) && expired["ok"].type == Json::Bool && !expired["ok"].b,
          "completed result is pruned after the fifteen-minute retention window");
    {
        std::lock_guard<std::mutex> lock(registry.mutex);
        while (registry.jobs.size() < kAsyncMaxJobs)
            registry.jobs["synthetic-pending-" + std::to_string(registry.jobs.size())] =
                std::make_shared<detail::AsyncJob>();
    }
    Json full;
    check(json_parse_strict(web_submit_start("{\"title\":\"capacity fixture\"}", live, asyncOpt), full) &&
          full["accepted"].type == Json::Bool && !full["accepted"].b,
          "job registry rejects new work at its fixed capacity instead of growing without bound");
    {
        std::lock_guard<std::mutex> lock(registry.mutex);
        for (auto it = registry.jobs.begin(); it != registry.jobs.end();)
            if (it->first.rfind("synthetic-pending-", 0) == 0) it = registry.jobs.erase(it); else ++it;
    }
    check(kAsyncResultRetentionSeconds >= 600 && kAsyncMaxJobs > 0,
          "retention and capacity constants enforce the documented lower bound");

    check(sanitize_json_fragment("{\"nested\":{\"PaSsWoRd\":[\"SyntheticSecret\"]},\"normal\":\"safe\"}").find("SyntheticSecret") == std::string::npos,
          "nested sensitive JSON values remain wholly redacted");
    check(sanitize_json_fragment("{\"Cookie\":\"second=SyntheticCookieTwo\"}").find("SyntheticCookieTwo") == std::string::npos,
          "structured Cookie field remains wholly redacted");
    check(redact("Authorization: Bearer SyntheticHeaderValue\n").find("SyntheticHeaderValue") == std::string::npos,
          "authorization header remains redacted");
    const auto cookie = redact("Cookie: first=SyntheticCookieOne; second=SyntheticCookieTwo\nnext=safe");
    check(cookie.find("SyntheticCookieOne") == std::string::npos && cookie.find("SyntheticCookieTwo") == std::string::npos &&
          cookie.find("next=safe") != std::string::npos,
          "cookie redaction does not consume the next log line");
    const auto userPath = redact("C:\\Users\\Synthetic Person\\Music\\song.wav");
    check(userPath.find("Synthetic") == std::string::npos && userPath.find("Person") == std::string::npos &&
          userPath.find("song.wav") != std::string::npos,
          "space-containing Windows username remains wholly redacted");
    check(redact("x@example.invalid") == "<user>@example.invalid", "one-character email local part remains redacted");
    check(redact("duration=123 track=normal.wav") == "duration=123 track=normal.wav",
          "ordinary diagnostic text remains unchanged");
    const auto proxy = redact("proxy=https://SyntheticProxyUser:SyntheticProxyPassword@proxy.example.invalid:7897/path");
    check(proxy.find("SyntheticProxyUser") == std::string::npos && proxy.find("SyntheticProxyPassword") == std::string::npos &&
          proxy.find("proxy.example.invalid:7897/path") != std::string::npos,
          "proxy userinfo remains redacted without losing host/path");
    const auto log = dir / "synthetic.log";
    std::ofstream(log) << "Cookie: first=SyntheticCookieOne; second=SyntheticCookieTwo\n"
                          "path=C:\\Users\\Synthetic Person\\Music\\song.wav\nemail=x@example.invalid\n";
    LiveState snapshot;
    snapshot.gpu = "offline fixture";
    snapshot.track = "C:\\Users\\Synthetic Person\\Music\\song.wav";
    snapshot.settingsJson = "{\"nested\":{\"password\":\"SyntheticSecret\"},\"volume\":-60}";
    Options pack;
    pack.logFiles = {{"synthetic.log", log.string()}};
    const Payload payload = build_payload(user, snapshot, pack);
    Json parsed;
    check(json_parse_strict(payload.json, parsed), "actual diagnostic payload remains complete strict JSON");
    check(payload.json.find("SyntheticSecret") == std::string::npos && payload.json.find("SyntheticCookieOne") == std::string::npos &&
          payload.json.find("SyntheticCookieTwo") == std::string::npos && payload.json.find("Synthetic Person") == std::string::npos &&
          payload.json.find("x@example.invalid") == std::string::npos,
          "full payload composition does not reintroduce synthetic secrets or identities");
    check(parsed["settings"]["volume"].n() == -60 && parsed["playback"]["track"].s().find("song.wav") != std::string::npos,
          "redaction preserves useful nonsecret settings and filename");

    std::printf("checks=%d failures=%d\n", checks, failures);
    return failures ? 1 : 0;
}
'''

compiler = sys.argv[1] if len(sys.argv) > 1 else r'C:/Program Files/AMD/ROCm/7.2/bin/clang++.exe'
with tempfile.TemporaryDirectory(prefix='rlplayer-feedback-recovery-') as directory:
    path = pathlib.Path(directory)
    cpp = path / 'test.cpp'
    exe = path / 'test.exe'
    cpp.write_text(code, encoding='utf-8')
    subprocess.run([
        compiler, '-std=c++20', '-O1', '-I' + str(root / 'src'), str(cpp), '-o', str(exe),
        '-lwinhttp', '-lws2_32', '-lbcrypt', '-lpsapi', '-luser32',
    ], check=True)
    fixtures = path / 'fixtures'
    fixtures.mkdir()
    result = subprocess.run([str(exe), str(fixtures)], cwd=directory)
    if result.returncode:
        sys.exit(result.returncode)
    exited = subprocess.run([str(exe), str(fixtures), '--exit-with-pending'], cwd=directory, timeout=10)
    if exited.returncode:
        print(f'FAIL pending-job process exit returned {exited.returncode}')
        sys.exit(1)
    print('PASS process exits with a detached pending job without joinable-static terminate')
