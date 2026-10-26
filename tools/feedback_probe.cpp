// feedback_probe.cpp — 「一键反馈」独立自检 CLI
//
// 为什么单独一个 exe：tools/rtaweb.cpp 是别的代理的战场（本工作包一行都不改它），
// 但"打包 + 上传"这件事必须有可重复、可脚本化的判据。于是把 src/net/feedback.h
// 直接编进一个 CLI，用同一份代码跑：
//   * --dry-run 只看生成的 JSON（脱敏是否干净、结构是否合理）
//   * 不带 --dry-run 就真的 PUT 到 GitHub 的 feedback/ 目录，打印状态码/路径/sha
//   * 把 --no-token 打开就模拟"三个凭据来源都没有"，验证 15 秒内降级到 feedback-outbox\
//
// 用法：
//   feedback_probe --type bug --title "标题" --body "正文" [--steps "复现步骤"]
//                  [--dry-run] [--out 文件] [--log-file 日志路径]... [--outbox 目录]
//                  [--no-token] [--keep-local] [--proxy none|host:port]
//                  [--repo owner/name] [--branch main]
//
// 退出码：0 上传成功 / 2 参数错误 / 3 dry-run 成功 / 4 上传失败（已落本地）
//
// 纪律：不碰音频设备、不改系统状态；token 只从环境/conf/gh 取，绝不打印、绝不写盘。
#include "net/feedback.h"

#include <windows.h>
#include <shellapi.h>

#include <cstdio>
#include <string>
#include <vector>

#pragma comment(lib, "shell32.lib")

namespace {

std::string utf8_of(const wchar_t* w) {
    if (!w) return std::string();
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return std::string();
    std::string s((size_t)n - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

void usage() {
    std::fprintf(stderr,
        "用法: feedback_probe --type bug|idea|other --title <标题> --body <正文> [选项]\n"
        "选项:\n"
        "  --steps <文字>        复现步骤（可选）\n"
        "  --dry-run             只生成 JSON 并打印到 stdout，不上传\n"
        "  --out <文件>          把生成的 JSON 另存一份（配合 --dry-run 看结构）\n"
        "  --log-file <路径>     附加一份日志（可重复；视图名取文件名）\n"
        "  --outbox <目录>       本地兜底目录（默认 exe\\feedback-outbox）\n"
        "  --no-token            跳过全部凭据来源，强制走「降级到本地」路径（测试用）\n"
        "  --keep-local          上传成功后也留一份本地副本\n"
        "  --proxy none|host:port  none=强制直连；host:port=具名代理；缺省=系统代理\n"
        "  --repo <owner/name>   目标仓库（默认 okhuoyanyan/RLPlayer）\n"
        "  --branch <分支>       默认 main\n"
        "退出码: 0=成功 2=参数错误 3=dry-run 成功 4=上传失败(已落本地)\n");
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

    rta::feedback::UserReport u;
    rta::feedback::Options opt;
    opt.skipTokenLookup = false;
    rta::feedback::LiveState live;
    live.source = "feedback_probe";
    std::string outFile;

    auto need = [&](int& i, const char* what) -> std::string {
        if (i + 1 >= (int)args.size()) {
            std::fprintf(stderr, "错误: %s 后面缺参数\n", what);
            return std::string();
        }
        return args[++i];
    };

    for (int i = 1; i < (int)args.size(); ++i) {
        const std::string a = args[i];
        if (a == "--help" || a == "-h") { usage(); return 2; }
        else if (a == "--type")    u.type  = need(i, "--type");
        else if (a == "--title")   u.title = need(i, "--title");
        else if (a == "--body")    u.body  = need(i, "--body");
        else if (a == "--steps")   u.steps = need(i, "--steps");
        else if (a == "--dry-run") opt.dryRun = true;
        else if (a == "--out")     outFile = need(i, "--out");
        else if (a == "--outbox")  opt.outboxDir = need(i, "--outbox");
        else if (a == "--no-token") opt.skipTokenLookup = true;
        else if (a == "--keep-local") opt.keepLocalCopy = true;
        else if (a == "--proxy")   opt.proxy = need(i, "--proxy");
        else if (a == "--repo")    opt.repo = need(i, "--repo");
        else if (a == "--branch")  opt.branch = need(i, "--branch");
        else if (a == "--log-file") {
            const std::string p = need(i, "--log-file");
            std::string name = p;
            const size_t s1 = name.find_last_of("\\/");
            if (s1 != std::string::npos) name = name.substr(s1 + 1);
            opt.logFiles.push_back(std::make_pair(name, p));
        } else {
            std::fprintf(stderr, "错误: 不认识的参数 %s\n", a.c_str());
            usage();
            return 2;
        }
    }

    if (u.title.empty() && u.body.empty()) {
        std::fprintf(stderr, "错误: --title 与 --body 不能都为空\n");
        usage();
        return 2;
    }

    const rta::feedback::Result r = rta::feedback::submit(u, live, opt);

    if (opt.dryRun) {
        if (!outFile.empty()) {
            FILE* f = nullptr;
            if (::fopen_s(&f, outFile.c_str(), "wb") == 0 && f) {
                std::fwrite(r.json.data(), 1, r.json.size(), f);
                std::fclose(f);
            } else {
                std::fprintf(stderr, "警告: 写不进 %s\n", outFile.c_str());
            }
        }
        std::fputs(r.json.c_str(), stdout);
        std::fputc('\n', stdout);
        std::fprintf(stderr, "[dry-run] id=%s bytes=%zu remote=%s\n",
                     r.id.c_str(), r.bytes, r.remotePath.c_str());
        return 3;
    }

    std::printf("类型      : %s\n", rta::feedback::type_label(rta::feedback::type_norm(u.type)).c_str());
    std::printf("标题      : %s\n", u.title.c_str());
    std::printf("反馈 ID   : %s\n", r.id.c_str());
    std::printf("JSON 大小 : %zu 字节\n", r.bytes);
    std::printf("凭据来源  : %s\n", r.tokenSource.empty() ? "(无)" : r.tokenSource.c_str());
    std::printf("HTTP 状态 : %d\n", r.status);
    std::printf("路径      : %s\n", r.remotePath.c_str());
    std::printf("文件 sha  : %s\n", r.sha.empty() ? "(无)" : r.sha.c_str());
    std::printf("提交 sha  : %s\n", r.commitSha.empty() ? "(无)" : r.commitSha.c_str());
    std::printf("本地文件  : %s\n", r.localPath.empty() ? "(无)" : r.localPath.c_str());
    std::printf("耗时      : %lld ms\n", r.elapsedMs);
    std::printf("结论      : %s\n", r.message.c_str());
    if (!r.error.empty()) std::printf("错误      : %s\n", r.error.c_str());
    return r.uploaded ? 0 : 4;
}
