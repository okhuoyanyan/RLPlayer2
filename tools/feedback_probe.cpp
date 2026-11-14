// feedback_probe.cpp — 一键反馈独立自检 CLI。
// 默认只建议 --dry-run 或 --server none；真实上传必须显式给出用户认可的 HTTPS 地址。
#include "net/feedback.h"

#include <windows.h>
#include <shellapi.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "shell32.lib")

namespace {

std::string utf8_of(const wchar_t* w) {
    if (!w) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string s((size_t)n - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

void usage() {
    std::fprintf(stderr,
        "用法: feedback_probe --type bug|idea|other --title <标题> --body <正文> [选项]\n"
        "选项:\n"
        "  --steps <文字>         复现步骤（可选）\n"
        "  --dry-run              只生成诊断 JSON，不读配置、不联网\n"
        "  --out <文件>           dry-run 时把诊断 JSON 另存一份\n"
        "  --log-file <路径>      附加一份日志（可重复）\n"
        "  --outbox <目录>        失败/禁用时保存诊断 JSON 的目录\n"
        "  --config <路径>        反馈配置（默认 exe\\rlplayer-feedback.conf）\n"
        "  --server <地址|none>   覆盖 HTTPS 服务地址；none=禁用并写 outbox\n"
        "  --idempotency-key <键> 指定本次请求的幂等键（默认随机生成）\n"
        "  --timeout-ms <毫秒>    单次上传超时，默认 120000\n"
        "  --keep-local           上传成功后也留一份本地副本\n"
        "  --proxy none|host:port 代理设置；缺省=系统代理\n"
        "  --async                通过 web_submit_start/status 提交并轮询真实 job\n"
        "退出码: 0=成功 2=参数错误 3=dry-run 成功 4=未上传（通常已落本地）\n");
}

std::string report_json(const rta::feedback::UserReport& user) {
    using rta::feedback::jesc;
    return std::string("{\"type\":\"") + jesc(user.type) +
           "\",\"title\":\"" + jesc(user.title) +
           "\",\"body\":\"" + jesc(user.body) +
           "\",\"steps\":\"" + jesc(user.steps) + "\"}";
}

int run_async(const rta::feedback::UserReport& user, const rta::feedback::LiveState& live,
              const rta::feedback::Options& opt) {
    using namespace rta::feedback;
    const std::string started = web_submit_start(report_json(user), live, opt);
    std::printf("异步接收: %s\n", started.c_str());
    rta::net::Json start;
    if (!rta::net::json_parse_strict(started, start) || !start["accepted"].ok() ||
        start["accepted"].type != rta::net::Json::Bool || !start["accepted"].b) return 4;
    const std::string job = start["job"].s();
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(opt.timeoutMs + 10000);
    while (std::chrono::steady_clock::now() < deadline) {
        const std::string status = web_submit_status(job);
        rta::net::Json json;
        if (rta::net::json_parse_strict(status, json) && json["done"].type == rta::net::Json::Bool &&
            json["done"].b) {
            std::printf("最终状态: %s\n", status.c_str());
            if (!json["ok"].ok() || json["ok"].type != rta::net::Json::Bool || !json["ok"].b) return 4;
            return json["result"]["uploaded"].type == rta::net::Json::Bool &&
                   json["result"]["uploaded"].b ? 0 : 4;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    std::fprintf(stderr, "错误: 等待反馈 job 超时：%s\n", job.c_str());
    return 4;
}

} // namespace

int main() {
    ::SetConsoleOutputCP(CP_UTF8);
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);

    int argc = 0;
    LPWSTR* wargv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);
    std::vector<std::string> args;
    for (int i = 0; i < argc; ++i) args.push_back(utf8_of(wargv[i]));
    if (wargv) ::LocalFree(wargv);

    rta::feedback::UserReport user;
    rta::feedback::Options opt;
    rta::feedback::LiveState live;
    live.source = "feedback_probe";
    std::string outFile;
    bool async = false;

    auto need = [&](int& i, const char* what) -> std::string {
        if (i + 1 >= (int)args.size()) {
            std::fprintf(stderr, "错误: %s 后面缺参数\n", what);
            return {};
        }
        return args[++i];
    };

    for (int i = 1; i < (int)args.size(); ++i) {
        const std::string& a = args[i];
        if (a == "--help" || a == "-h") { usage(); return 2; }
        else if (a == "--type") user.type = need(i, "--type");
        else if (a == "--title") user.title = need(i, "--title");
        else if (a == "--body") user.body = need(i, "--body");
        else if (a == "--steps") user.steps = need(i, "--steps");
        else if (a == "--dry-run") opt.dryRun = true;
        else if (a == "--out") outFile = need(i, "--out");
        else if (a == "--outbox") opt.outboxDir = need(i, "--outbox");
        else if (a == "--config") opt.configPath = need(i, "--config");
        else if (a == "--server") opt.serverUrl = need(i, "--server");
        else if (a == "--idempotency-key") opt.idempotencyKey = need(i, "--idempotency-key");
        else if (a == "--timeout-ms") {
            const std::string value = need(i, "--timeout-ms");
            char* end = nullptr;
            const long timeout = std::strtol(value.c_str(), &end, 10);
            if (!end || *end || timeout < 1 || timeout > 600000) {
                std::fprintf(stderr, "错误: --timeout-ms 必须在 1..600000 之间\n");
                return 2;
            }
            opt.timeoutMs = (int)timeout;
        }
        else if (a == "--keep-local") opt.keepLocalCopy = true;
        else if (a == "--proxy") opt.proxy = need(i, "--proxy");
        else if (a == "--async") async = true;
        else if (a == "--log-file") {
            const std::string path = need(i, "--log-file");
            std::string name = path;
            const size_t slash = name.find_last_of("\\/");
            if (slash != std::string::npos) name = name.substr(slash + 1);
            opt.logFiles.emplace_back(name, path);
        } else {
            std::fprintf(stderr, "错误: 不认识的参数 %s\n", a.c_str());
            usage();
            return 2;
        }
    }

    if (user.title.empty() && user.body.empty()) {
        std::fprintf(stderr, "错误: --title 与 --body 不能都为空\n");
        usage();
        return 2;
    }
    if (async && !opt.dryRun) return run_async(user, live, opt);

    const rta::feedback::Result r = rta::feedback::submit(user, live, opt);
    if (opt.dryRun) {
        if (!outFile.empty()) {
            FILE* file = nullptr;
            if (::fopen_s(&file, outFile.c_str(), "wb") == 0 && file) {
                std::fwrite(r.json.data(), 1, r.json.size(), file);
                std::fclose(file);
            } else {
                std::fprintf(stderr, "警告: 写不进 %s\n", outFile.c_str());
            }
        }
        std::fputs(r.json.c_str(), stdout);
        std::fputc('\n', stdout);
        std::fprintf(stderr, "[dry-run] id=%s bytes=%zu\n", r.id.c_str(), r.bytes);
        return 3;
    }

    std::printf("类型          : %s\n", rta::feedback::type_label(rta::feedback::type_norm(user.type)).c_str());
    std::printf("反馈 ID       : %s\n", r.id.c_str());
    std::printf("诊断 JSON     : %zu 字节\n", r.bytes);
    std::printf("Client        : %s\n", r.client.c_str());
    std::printf("Idempotency   : %s\n", r.idempotencyKey.c_str());
    std::printf("HTTP 状态     : %d\n", r.status);
    std::printf("服务端编号    : %s\n", r.remotePath.empty() ? "(无)" : r.remotePath.c_str());
    std::printf("诊断已存储    : %s\n", r.diagStored ? "true" : "false");
    std::printf("Retry-After   : %lld\n", r.retryAfter);
    std::printf("本地文件      : %s\n", r.localPath.empty() ? "(无)" : r.localPath.c_str());
    std::printf("耗时          : %lld ms\n", r.elapsedMs);
    std::printf("结论          : %s\n", r.message.c_str());
    if (!r.error.empty()) std::printf("错误          : %s\n", r.error.c_str());
    return r.uploaded ? 0 : 4;
}
