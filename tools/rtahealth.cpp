// rtahealth.cpp — 音频体检 B1b CLI：对文件/目录批量出体检结果（DR/LUFS/LRA/Peak + 假无损）
//
// 用法:
//   rtahealth <文件或目录...> [--json] [--force] [--cache <path>]
//
// 行为:
//   * 目录递归收集音频扩展名（wav/flac/mp3/aac/m4a/ogg/opus/ape/wv/alac/dsf/dff），排序去重；
//   * 默认人类可读表格；--json 每行一个 JSON 对象（汇总行走 stderr，stdout 保持可解析）；
//   * 默认【无缓存文件】——内存内不缓存、不落任何盘；只有 --cache 才落盘（三元组键:
//     规范路径+mtime+size，见 src/analysis/health_store.h）；--force 与 --cache 连用表示
//     忽略命中全部重算；
//   * 只读分析：绝不写被分析的文件，绝不打开音频设备（铁律 1）。
//
// 说明：用宽字符入口 wmain + wide_to_utf8()（同 tools/rtatag.cpp 的既证做法），
//       中文路径不经过 ANSI 代码页。
#include "analysis/track_health.h"
#include "analysis/health_store.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <set>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

using namespace rta::analysis;
namespace fsx = std::filesystem;

static double ms_since(const std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

static const char* kAudioExts[] = {
    ".wav", ".flac", ".mp3", ".aac", ".m4a", ".ogg",
    ".opus", ".ape", ".wv", ".alac", ".dsf", ".dff"
};

static bool is_audio_ext(std::string ext) {
    for (char& c : ext) c = (char)std::tolower((unsigned char)c);
    for (const char* e : kAudioExts) if (ext == e) return true;
    return false;
}

// 取前 n 个字节，回退到 UTF-8 序列边界再截断（不切坏中文），超长补省略号
static std::string first_n(const std::string& s, const size_t n) {
    if (s.size() <= n) return s;
    size_t k = n;
    while (k > 0 && ((unsigned char)s[k] & 0xC0) == 0x80) --k;
    return s.substr(0, k) + "…";
}

struct Job {
    std::string path;
    std::string collectError;   // 非空 = 还没到分析就该报的错（路径不存在等）
};

// 展开输入：目录递归收集音频扩展名（排序去重），文件原样收，坏路径带 collectError 返回
static std::vector<Job> collect_jobs(const std::vector<std::string>& inputs) {
    std::vector<Job> jobs;
    std::set<std::string> seen;   // 规范化路径去重（同一文件经多个入口传进来只分析一次）
    for (const std::string& in : inputs) {
        std::error_code ec;
        const fsx::path p = fsx::path(rta::audio::widen(in));
        if (fsx::is_directory(p, ec)) {
            for (fsx::recursive_directory_iterator it(p, fsx::directory_options::skip_permission_denied, ec),
                 end; !ec && it != end; it.increment(ec)) {
                const fsx::path f = it->path();
                std::error_code ec2;
                if (!it->is_regular_file(ec2) || ec2) continue;
                std::string ext = f.extension().string();
                for (char& c : ext) c = (char)std::tolower((unsigned char)c);
                if (!is_audio_ext(ext)) continue;
                const std::string fp = rta::audio::wide_to_utf8(f.wstring());
                if (seen.insert(normalize_path_key(fp)).second) jobs.push_back({fp, ""});
            }
            // 目录本身不可进入等：不视为致命，继续处理其余输入
        } else if (fsx::exists(p, ec) && !ec) {
            if (seen.insert(normalize_path_key(in)).second) jobs.push_back({in, ""});
        } else {
            jobs.push_back({in, "路径不存在或不可访问"});
        }
    }
    std::stable_sort(jobs.begin(), jobs.end(),
                     [](const Job& a, const Job& b) { return normalize_path_key(a.path) < normalize_path_key(b.path); });
    return jobs;
}

static void usage() {
    printf("用法:\n"
           "  rtahealth <文件或目录...> [--json] [--force] [--cache <path>]\n"
           "\n"
           "  --json          每行一个 JSON 对象（含 track_health_json 全部字段 + path/mtime/size/cached）\n"
           "  --force         与 --cache 连用时忽略缓存命中，全部重算\n"
           "  --cache <path>  启用体检缓存文件（默认无缓存、不写盘）；缓存键 = 规范路径+mtime+size\n");
}

int wmain(int argc, wchar_t** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    std::vector<std::string> inputs;
    bool jsonOut = false, force = false;
    std::string cachePath;
    for (int i = 1; i < argc; ++i) {
        const std::string a = rta::audio::wide_to_utf8(argv[i]);
        if (a == "--json") jsonOut = true;
        else if (a == "--force") force = true;
        else if (a == "--cache" && i + 1 < argc) cachePath = rta::audio::wide_to_utf8(argv[++i]);
        else if (a == "-h" || a == "--help") { usage(); return 0; }
        else inputs.push_back(a);
    }
    if (inputs.empty()) { usage(); return 2; }

    std::unique_ptr<HealthStore> store;
    if (!cachePath.empty()) {
        store = std::make_unique<HealthStore>(cachePath);
        if (!store->loadError().empty()) {
            // 如实报错且本次不回写（save() 会拒绝）；提示人工处理，绝不静默清空用户缓存
            printf("!! 缓存文件损坏，本次忽略缓存且不会回写: %s\n   %s\n   （请删除或修复该文件后重试）\n\n",
                   cachePath.c_str(), store->loadError().c_str());
        }
    }

    if (!jsonOut)
        printf("路径 | DR dB | LUFS | LRA LU | Peak dBFS | 截止 | 疑似假无损 | 24bit假 | 状态\n\n");

    const std::vector<Job> jobs = collect_jobs(inputs);
    int nAnalyzed = 0, nCached = 0, nErr = 0, nUp = 0;
    double sumMs = 0.0;
    const auto tAll0 = std::chrono::steady_clock::now();

    for (const Job& job : jobs) {
        TrackHealth h;
        bool hit = false;
        double ms = 0.0;
        int64_t mtime = 0, fsize = 0;
        std::string statErr;

        if (job.collectError.empty()) {
            std::error_code ec;
            const fsx::path p = fsx::path(rta::audio::widen(job.path));
            const auto wtime = fsx::last_write_time(p, ec);
            if (ec) { statErr = "无法取文件属性（mtime）: " + ec.message(); }
            else {
                mtime = (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                    wtime.time_since_epoch()).count();
                const uintmax_t sz = fsx::file_size(p, ec);
                if (ec) statErr = "无法取文件属性（size）: " + ec.message();
                else fsize = (int64_t)sz;
            }
        }

        if (!job.collectError.empty()) {
            h.status = "error";
            h.error = job.collectError;
        } else if (!statErr.empty()) {
            h.status = "error";
            h.error = statErr;
        } else if (store && !force && store->lookup(job.path, mtime, fsize, h)) {
            hit = true;
        } else {
            const auto t0 = std::chrono::steady_clock::now();
            h = analyze_file(job.path);
            ms = ms_since(t0);
            ++nAnalyzed;
            sumMs += ms;
            // 只缓存 ready 结果：瞬时读不了的文件不该被错误结果钉死（文件变更后键会失效，
            // 但「当时读不了」与「文件变了」无从区分，保守起见错误不入库）
            if (store && h.status == "ready") { store->upsert(job.path, mtime, fsize, h); ++nUp; }
        }
        if (hit) ++nCached;
        if (h.status != "ready") ++nErr;

        if (jsonOut) {
            std::string line = "{\"path\":\"" + detail::json_escape(job.path) + "\"";
            line += ",\"mtime\":" + std::to_string((long long)mtime);
            line += ",\"size\":" + std::to_string((long long)fsize);
            line += std::string(",\"cached\":") + (hit ? "true" : "false");
            if (!hit) line += ",\"analysis_ms\":" + detail::jnum(ms, "%.1f");
            // track_health_json 是完整对象 {…}：去掉它的 '{' 拼进来，它自带的 '}' 就是本行收尾
            line += "," + track_health_json(h).substr(1);
            printf("%s\n", line.c_str());
        } else {
            char cut[32];
            if (h.status == "ready") {
                if (h.cutoffHz > 0.0) std::snprintf(cut, sizeof(cut), "%.1fk", h.cutoffHz / 1000.0);
                else std::snprintf(cut, sizeof(cut), "-");
                char sus[32];
                if (h.losslessSuspected) std::snprintf(sus, sizeof(sus), "是(%.2f)", h.confidence);
                else std::snprintf(sus, sizeof(sus), "否");
                printf("%s\n    DR %6.2f | LUFS %7.2f | LRA %5.2f | Peak %7.2f | 截止 %-7s | 疑似假无损 %-9s | 24bit假 %-3s | 状态 ready | 分析 %.0f ms%s\n",
                       first_n(job.path, 100).c_str(),
                       h.drDb, h.lufs, h.lra, h.peakDbfs, cut, sus,
                       h.fake24bit ? "是" : "否", ms, hit ? "（缓存）" : "");
            } else {
                printf("%s\n    状态 error | %s\n", first_n(job.path, 100).c_str(),
                       first_n(h.error, 200).c_str());
            }
        }
    }

    const double totalSec = ms_since(tAll0) / 1000.0;
    const double avgMs = (nAnalyzed > 0) ? sumMs / (double)nAnalyzed : 0.0;
    if (jsonOut) {
        fprintf(stderr, "files=%d analyzed=%d cached=%d errors=%d avg_ms_per_file=%.1f total_s=%.2f\n",
                (int)jobs.size(), nAnalyzed, nCached, nErr, avgMs, totalSec);
    } else {
        printf("\n共 %d 个文件：分析 %d（缓存命中 %d），错误 %d；平均分析 %.1f ms/文件，总耗时 %.2f s\n",
               (int)jobs.size(), nAnalyzed, nCached, nErr, avgMs, totalSec);
    }

    if (store) {
        if (store->loadError().empty()) {
            const bool ok = store->save();
            printf("缓存: %s（%zu 条，本次新写 %d 条，%s）\n", cachePath.c_str(),
                   store->size(), nUp, ok ? "已保存" : "保存失败");
            if (jsonOut) fprintf(stderr, "cache=%s entries=%zu saved=%s\n",
                                 cachePath.c_str(), store->size(), ok ? "true" : "false");
        } else {
            printf("缓存: 未写盘（加载时已报错，见上）\n");
        }
    }
    return nErr > 0 ? 1 : 0;
}
