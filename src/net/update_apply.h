// src/net/update_apply.h — 在线更新（Windows 版）第二层：受保护清单 / 落地 / 回滚 / 台账
//
// ============================ 为什么这样替换 ============================
// 核心约束：
//   1) 程序正在运行，**不能覆盖自己**：被映射的 exe/dll 不允许删除或覆盖（ERROR_ACCESS_DENIED）。
//      但 Windows **允许改名**（同卷 MoveFileEx）—— 这是自我替换的通用技巧：
//          MoveFileEx(RLPlayer.exe -> updates/<ver>/backup/RLPlayer.exe)   成功（进程还在跑）
//          MoveFileEx(staging/RLPlayer.exe -> RLPlayer.exe)                成功（名字腾出来了）
//      下次启动就是新版；旧映像仍在内存里跑完这一轮，不影响。
//   2) 更**不能丢用户数据**：受保护清单里的文件一个字节都不动（见 protected_files/dirs）。
//   3) 失败必须能回滚，且回滚材料（backup）必须**在替换之前**就已经落盘。
//
// 流程（用户原话：「完全可以做到自动更新，然后重启或者刷新，就可以完成」）：
//   · rtaweb 端点 /api/update/apply —— 只做三件事：把 staging + 计划写进 updates/pending.txt、
//     返回"即将重启"、然后请自己与启动器退出（**不在这里换文件**，此刻文件都在用）。
//   · 下次启动 RLPlayer.exe（启动器）—— main() 一开始、**在起 rtaweb 之前**调用
//     apply_pending_on_startup()：此刻 rtaweb 没在跑，所有文件都不被占用；逐个
//     "旧文件改名进 backup → 新文件改名到位"，连它自己（RLPlayer.exe）也是这样换掉。
//   · 新 rtaweb 起来后调用 confirm_on_startup()：版本对上 ⟹ 台账写 done、清 staging。
//   · 起不来（launcher 的端口等待窗口内子进程退出）⟹ rollback_pending() 把 backup 搬回去。
//
// 为什么不再用"生成 update.ps1 再拉起 PowerShell"（我最初的实现）：
//   ① 依赖 powershell.exe 与 ExecutionPolicy（企业 AllSigned 会直接失败）；
//   ② 多一个进程就多一个失败面（脚本被杀/被杀软拦），且它还得轮询 PID 等我们退出；
//   ③ 用户的语义是「重启就完成」，启动器启动时本来就有"文件不被占用"的完美时机。
//   ⟹ 改成启动器内直接落地，链路更短、无外部依赖、可被同一份单测覆盖。
#pragma once
#include <tlhelp32.h>

#include <algorithm>
#include <string>
#include <vector>

#include "net/update.h"
#include "net/update_assets_hashes.h"

namespace rta::update {

// ===========================================================================
// 1. 受保护清单（纯函数，可离线断言；脚本/代码/UI 共用同一份语义）
// ===========================================================================
// 判据来源：需求原文「要保护的（绝对不能被覆盖）」+ 本仓实际落盘约定。
inline const std::vector<std::string>& protected_files() {
    static const std::vector<std::string> v = {
        // 需求点名的 7 个
        "rlplayer-settings.conf",       // 设置（后端/滤波/抖动/音量…）
        "rlplayer-library.conf",        // 音乐库根目录
        "rlplayer-llm.conf",            // DeepSeek key / 代理
        "rlplayer-feedback.conf",       // 一键反馈的令牌/仓库
        "rlplayer-queue.txt",           // 当前播放队列
        "rlplayer-savedqueues.conf",    // 具名保存的队列
        "rlplayer-favs.json",           // 收藏
        // 同族：用户数据 / 运行期产物
        "rlplayer-sources.conf", "rlplayer-peq.txt", "rlplayer-index.tsv",
        "rlplayer-recent.txt", "rlplayer-diag.json", "rlplayer-playback.log",
        "launcher.log", "rlplayer-llm-usage.json", "rlplayer-update.conf",
    };
    return v;
}

// 整目录保护（data 里的纯应用资产另见 protected_allow()）
inline const std::vector<std::string>& protected_dirs() {
    static const std::vector<std::string> v = {
        "data",            // 库索引 / 封面缓存 / 房间校正集
        "covercache",
        "webview2runtime", // 127 MB 运行时，几乎不变
        "webview2",
        "playlists",
        "rlplayer-meta",
        "updates",         // 我们自己的工作目录
        "dumps",
        "feedback-outbox",
    };
    return v;
}

// 保护目录里的**例外**：随包发布的纯应用资产（不更新会与新代码不匹配）
inline const std::vector<std::string>& protected_allow() {
    static const std::vector<std::string> v = {
        "data\\opra_index.json", "data\\i18n\\ar.json", "data\\i18n\\en.json",
        "data\\i18n\\es.json", "data\\i18n\\fr.json", "data\\i18n\\ja.json",
        "data\\i18n\\ru.json"
    };
    return v;
}

inline std::string norm_rel2(std::string rel) {
    for (char& c : rel) {
        if (c == '/') c = '\\';
        else if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    }
    while (!rel.empty() && (rel.front() == '\\' || rel.front() == '.')) rel.erase(rel.begin());
    return rel;
}

inline bool starts_with_dir(const std::string& rel, const std::string& dir) {
    if (rel.size() < dir.size()) return false;
    if (rel.compare(0, dir.size(), dir) != 0) return false;
    return rel.size() == dir.size() || rel[dir.size()] == '\\';
}

// 相对路径是否「绝对不能碰」
inline bool is_protected_rel(const std::string& rawRel) {
    const std::string rel = norm_rel2(rawRel);
    if (rel.empty()) return true;
    for (const auto& a : protected_allow()) if (rel == a) return false;
    for (const auto& d : protected_dirs()) if (starts_with_dir(rel, d)) return true;
    const size_t s = rel.find_last_of('\\');
    const std::string leaf = (s == std::string::npos) ? rel : rel.substr(s + 1);
    for (const auto& f : protected_files()) if (rel == f || leaf == f) return true;
    return false;
}

// ===========================================================================
// 2. 台账 updates/pending.txt
// ===========================================================================
inline std::string pending_path() { return join_path(updates_root(), "pending.txt"); }

struct PendingPlan {
    bool exists = false;
    std::string phase;      // ready / applied / rolled_back / apply_failed / done
    std::string version;
    std::string from;
    std::string staging;
    std::string time;
    std::string detail;
    std::vector<std::string> files;    // 计划安装的文件（相对路径）
    std::vector<std::string> applied;  // 实际替换成功的（回滚用）
    std::vector<std::string> added;    // 本来不存在、本次新增的（回滚时删掉）
};

inline PendingPlan read_pending() {
    PendingPlan p;
    if (!path_exists(pending_path())) return p;
    p.exists = true;
    const std::string t = read_text_file(pending_path());
    size_t pos = 0;
    while (pos < t.size()) {
        size_t e = t.find('\n', pos);
        if (e == std::string::npos) e = t.size();
        std::string line = t.substr(pos, e - pos);
        pos = e + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        if (k == "phase") p.phase = v;
        else if (k == "version") p.version = v;
        else if (k == "from") p.from = v;
        else if (k == "staging") p.staging = v;
        else if (k == "time") p.time = v;
        else if (k == "detail") p.detail = v;
        else if (k == "file") p.files.push_back(v);
        else if (k == "applied") p.applied.push_back(v);
        else if (k == "added") p.added.push_back(v);
    }
    return p;
}

inline bool write_pending(const PendingPlan& p) {
    ensure_dir(updates_root());
    std::string t = "phase=" + p.phase + "\r\n";
    t += "version=" + p.version + "\r\n";
    t += "from=" + p.from + "\r\n";
    t += "staging=" + p.staging + "\r\n";
    t += "time=" + p.time + "\r\n";
    if (!p.detail.empty()) t += "detail=" + p.detail + "\r\n";
    for (const auto& f : p.files) t += "file=" + f + "\r\n";
    for (const auto& f : p.applied) t += "applied=" + f + "\r\n";
    for (const auto& f : p.added) t += "added=" + f + "\r\n";
    return write_text_file(pending_path(), t);
}

inline std::string now_text() {
    char b[64];
    std::time_t t = std::time(nullptr);
    std::tm v{};
    localtime_s(&v, &t);
    std::snprintf(b, sizeof(b), "%04d-%02d-%02d %02d:%02d:%02d",
                  v.tm_year + 1900, v.tm_mon + 1, v.tm_mday, v.tm_hour, v.tm_min, v.tm_sec);
    return std::string(b);
}

inline std::string backup_dir(const std::string& ver) { return join_path(version_dir(ver), "backup"); }

// 把 staging 里的文件清单写进台账（端点 /apply 调用）
inline bool write_ready_plan(const std::string& ver, const std::string& from,
                             const std::string& staging, std::string& err) {
    if (!is_dir(staging)) { err = "暂存目录不存在：" + staging; return false; }
    // ★ 2026-10-29（两轮审查 F08）：安装资格必须由**完成标记 + 逐文件校验**决定，
    //   不能枚举目录里现有文件就当清单（半套文件会被当成完整安装集合）。
    {
        std::string verr;
        if (!rta::update::staging_verify(staging, verr)) {
            err = "暂存不完整，拒绝安装：" + verr;
            return false;
        }
    }
    std::vector<std::string> rels;
    list_files_rel(staging, "", rels);
    // 完成标记本身不是要安装的文件
    rels.erase(std::remove(rels.begin(), rels.end(), std::string("complete.txt")), rels.end());
    if (rels.empty()) { err = "暂存目录里没有可安装文件"; return false; }
    PendingPlan p;
    p.phase = "ready";
    p.version = ver;
    p.from = from;
    p.staging = staging;
    p.time = now_text();
    for (const auto& r : rels) p.files.push_back(r);
    if (!write_pending(p)) { err = "无法写台账：" + pending_path(); return false; }
    return true;
}

// 同卷改名优先；跨卷（极端情况）退回 复制+删除
inline bool move_file(const std::string& from, const std::string& to) {
    const std::wstring wf = rta::platform::from_utf8(from);
    const std::wstring wt = rta::platform::from_utf8(to);
    ::SetFileAttributesW(wt.c_str(), FILE_ATTRIBUTE_NORMAL);
    if (::MoveFileExW(wf.c_str(), wt.c_str(), MOVEFILE_REPLACE_EXISTING)) return true;
    if (!::CopyFileW(wf.c_str(), wt.c_str(), FALSE)) return false;
    ::SetFileAttributesW(wf.c_str(), FILE_ATTRIBUTE_NORMAL);
    ::DeleteFileW(wf.c_str());
    return true;
}

// ===========================================================================
// 3. 落地（启动器 main() 最开头，rtaweb 尚未启动时调用）
// ===========================================================================
inline std::string apply_pending_on_startup() {
    PendingPlan p = read_pending();
    if (!p.exists) return std::string();
    if (p.phase != "ready") return std::string();
    const std::string inst = install_dir();
    if (p.staging.empty() || !is_dir(p.staging)) {
        p.phase = "apply_failed";
        p.detail = "staging 目录不存在";
        write_pending(p);
        return "UPDATE 失败：暂存目录不见了（" + p.staging + "）";
    }

    std::vector<std::string> todo;
    int skipped = 0;
    for (const auto& rel : p.files) {
        if (is_protected_rel(rel)) { ++skipped; continue; }
        if (!path_exists(join_path(p.staging, rel))) continue;
        todo.push_back(rel);
    }
    if (todo.empty()) {
        p.phase = "apply_failed";
        p.detail = "可安装文件为 0";
        write_pending(p);
        return "UPDATE 失败：暂存里没有可安装的文件";
    }

    const std::string bak = backup_dir(p.version);
    if (!ensure_dir(bak)) return "UPDATE 失败：无法创建备份目录（" + bak + "）";

    p.applied.clear();
    p.added.clear();
    // ★ 2026-10-29（两轮审查 F07）：回滚必须**检查恢复动作本身的结果**。
    //
    // 缺陷：旧 abort() 对每个已登记文件做 DeleteFile + move_file(b, tgt)，但从不看
    //   move_file 的返回值，最后无条件写"UPDATE 失败并已回滚"。恢复失败时用户以为
    //   文件都回来了，实际安装目录里缺文件、备份还躺在 backup 里没人知道。
    // 现在：恢复失败的文件逐个记下、写进台账 detail、并在返回文案里点名（备份保留）。
    auto abort = [&](const std::string& why) -> std::string {
        std::string restoreFail;
        // 立刻按已做的记录回滚，绝不留半新半旧
        for (auto it = p.applied.rbegin(); it != p.applied.rend(); ++it) {
            const std::string tgt = join_path(inst, *it);
            const std::string b = join_path(bak, *it);
            ::DeleteFileW(rta::platform::from_utf8(tgt).c_str());
            if (path_exists(b)) {
                if (!move_file(b, tgt)) restoreFail += (restoreFail.empty() ? "" : "; ") + *it;
            }
        }
        for (const auto& a : p.added) ::DeleteFileW(rta::platform::from_utf8(join_path(inst, a)).c_str());
        p.phase = "apply_failed";
        p.detail = why;
        if (!restoreFail.empty())
            p.detail += "；恢复失败的文件（备份仍在 " + bak + "）：" + restoreFail;
        write_pending(p);
        if (restoreFail.empty()) return "UPDATE 失败并已回滚：" + why;
        return "UPDATE 失败，且以下文件未能恢复（备份保留在 " + bak + "）：" + restoreFail + "；原因：" + why;
    };

    for (const auto& rel : todo) {
        const std::string tgt = join_path(inst, rel);
        const std::string src = join_path(p.staging, rel);
        const size_t sp = tgt.find_last_of('\\');
        if (sp != std::string::npos) {
            if (!ensure_dir(tgt.substr(0, sp))) return abort("无法创建目录 " + tgt.substr(0, sp));
        }
        const bool existed = path_exists(tgt);
        std::string curBackup;
        if (existed) {
            const std::string b = join_path(bak, rel);
            const size_t bs = b.find_last_of('\\');
            if (bs != std::string::npos) ensure_dir(b.substr(0, bs));
            ::DeleteFileW(rta::platform::from_utf8(b).c_str());
            if (!move_file(tgt, b)) return abort("备份失败：" + rel);
            curBackup = b;
        }
        if (!move_file(src, tgt)) {
            // ★ F07：当前文件已经被备份走了、新文件却没装上 —— 它还没进 p.applied，
            //   旧 abort() 不会碰它 ⟹ 安装目录少一个文件却报"已回滚"。
            //   必须先把它恢复回来，再回滚之前那些文件。
            if (!curBackup.empty()) {
                ::DeleteFileW(rta::platform::from_utf8(tgt).c_str());
                if (!move_file(curBackup, tgt)) {
                    p.phase = "apply_failed";
                    p.detail = "安装失败且当前文件无法恢复：" + rel + "（备份在 " + curBackup + "）";
                    write_pending(p);
                    return "UPDATE 失败：安装 " + rel + " 失败，且无法恢复原文件（备份保留在 " +
                           curBackup + "）";
                }
            }
            return abort("安装失败：" + rel);
        }
        p.applied.push_back(rel);
        if (!existed) p.added.push_back(rel);
    }

    p.phase = "applied";
    p.time = now_text();
    p.detail = "替换 " + std::to_string(p.applied.size()) + " 个文件，跳过受保护 " + std::to_string(skipped) + " 个";
    write_pending(p);
    return "UPDATE 已替换 " + std::to_string(p.applied.size()) + " 个文件（跳过受保护 " +
           std::to_string(skipped) + " 个），备份在 " + bak;
}

// ===========================================================================
// 4. 回滚（启动器发现新版本起不来时调用）
// ===========================================================================
inline std::string rollback_pending(const std::string& why) {
    PendingPlan p = read_pending();
    if (!p.exists || p.phase != "applied") return std::string();
    const std::string inst = install_dir();
    const std::string bak = backup_dir(p.version);
    int restored = 0, removed = 0, failed = 0;
    for (auto it = p.applied.rbegin(); it != p.applied.rend(); ++it) {
        const std::string tgt = join_path(inst, *it);
        const std::string b = join_path(bak, *it);
        ::DeleteFileW(rta::platform::from_utf8(tgt).c_str());
        if (path_exists(b)) { if (move_file(b, tgt)) ++restored; else ++failed; }
    }
    for (const auto& a : p.added) {
        const std::string t = join_path(inst, a);
        if (path_exists(t) && ::DeleteFileW(rta::platform::from_utf8(t).c_str())) ++removed;
    }
    p.phase = "rolled_back";
    p.time = now_text();
    p.detail = why + "（恢复 " + std::to_string(restored) + " 个 / 删除新增 " +
               std::to_string(removed) + " 个 / 失败 " + std::to_string(failed) + " 个）";
    write_pending(p);
    return "UPDATE 已回滚：" + p.detail;
}

// ===========================================================================
// 5. 确认（新 rtaweb 启动时调用）
// ===========================================================================
// Reject junctions/symlinks at every component below the installation root.
// Both source and destination are derived here; pending.staging is never trusted.
inline bool asset_path_without_reparse(const std::string& root, const std::string& rel) {
    std::string path = root;
    auto plain = [](const std::string& p) {
        const DWORD a = GetFileAttributesW(rta::platform::from_utf8(p).c_str());
        return a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_REPARSE_POINT);
    };
    if (!plain(path)) return false;
    size_t begin = 0;
    while (begin < rel.size()) {
        const size_t end = rel.find_first_of("\\/", begin);
        path = join_path(path, rel.substr(begin, end == std::string::npos ? end : end - begin));
        if (!plain(path)) return false;
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    return true;
}

inline bool migrate_application_update_assets(PendingPlan& p, std::string& err) {
    const std::string inst = install_dir();
    if (!safe_install_rel(p.version) || p.version.find_first_of("\\/") != std::string::npos) {
        err = "更新版本目录非法"; return false;
    }
    const std::string stageRel = "updates/" + p.version + "/staging/";
    const std::string backupRel = "updates/" + p.version + "/backup/";
    std::vector<const ApplicationUpdateAsset*> todo;
    // Preflight every file before writing any target. Old launchers do not write
    // complete.txt, so the trusted digest is generated from this build's assets.
    for (const auto& a : kApplicationUpdateAssets) {
        if (!asset_path_without_reparse(inst, a.path) ||
            !asset_path_without_reparse(inst, stageRel + a.path) ||
            !asset_path_without_reparse(inst, backupRel + a.path)) {
            err = std::string("应用资源路径含链接，拒绝迁移：") + a.path; return false;
        }
        std::string hash, e;
        if (sha256_file(join_path(inst, a.path), hash, e) && hex_equals_ci(hash, a.sha256)) continue;
        if (!sha256_file(join_path(inst, stageRel + a.path), hash, e) || !hex_equals_ci(hash, a.sha256)) {
            err = std::string("应用资源缺失或校验失败：") + a.path + "（暂存已保留，可修复后重启重试）";
            return false;
        }
        todo.push_back(&a);
    }
    for (const auto* a : todo) {
        const std::string rel = norm_rel2(a->path);
        const std::string target = join_path(inst, a->path);
        const std::string backup = join_path(inst, backupRel + a->path);
        const std::string source = join_path(inst, stageRel + a->path);
        const bool existed = path_exists(target);
        if (!ensure_dir(target.substr(0, target.find_last_of("\\/"))) ||
            !ensure_dir(backup.substr(0, backup.find_last_of("\\/")))) {
            err = std::string("无法创建应用资源目录：") + a->path; return false;
        }
        // Preserve any backup already made by a newer launcher.
        if (existed && !path_exists(backup) &&
            !CopyFileW(rta::platform::from_utf8(target).c_str(), rta::platform::from_utf8(backup).c_str(), TRUE)) {
            err = std::string("应用资源备份失败：") + a->path; return false;
        }
        if (!CopyFileW(rta::platform::from_utf8(source).c_str(), rta::platform::from_utf8(target).c_str(), FALSE)) {
            err = std::string("应用资源迁移失败：") + a->path; return false;
        }
        std::string hash, e;
        if (!sha256_file(target, hash, e) || !hex_equals_ci(hash, a->sha256)) {
            err = std::string("应用资源落盘校验失败：") + a->path; return false;
        }
        if (std::find(p.applied.begin(), p.applied.end(), rel) == p.applied.end()) p.applied.push_back(rel);
        if (!existed && std::find(p.added.begin(), p.added.end(), rel) == p.added.end()) p.added.push_back(rel);
        if (!write_pending(p)) { err = "应用资源迁移台账写入失败"; return false; }
    }
    return true;
}

inline std::string confirm_on_startup() {
    PendingPlan p = read_pending();
    if (!p.exists) return std::string();
    if (p.phase == "applied") {
        if (cmp_versions(p.version, current_version()) == 0) {
            std::string assetError;
            if (!migrate_application_update_assets(p, assetError)) {
                p.detail = assetError;
                write_pending(p);
                return "UPDATE 未完成：" + assetError;
            }
            p.phase = "done";
            p.time = now_text();
            if (!write_pending(p)) return "UPDATE 未完成：确认台账写入失败（暂存已保留）";
            // 成功之后 staging 可以清；backup 保留（用户想手工回退还有料）
            rmtree(join_path(version_dir(p.version), "staging"));
            return "UPDATE ok " + p.from + " -> " + p.version + "（" + p.detail + "）";
        }
        return "UPDATE 台账说装的是 v" + p.version + "，但当前进程是 v" + current_version() +
               " —— 换文件可能没落到本目录";
    }
    if (p.phase == "rolled_back") return "UPDATE 上次更新失败已回滚：" + p.detail;
    if (p.phase == "apply_failed") return "UPDATE 上次替换失败已回滚：" + p.detail;
    if (p.phase == "ready") return "UPDATE 有已下载但尚未安装的更新（重启后生效）";
    return std::string();
}

// ===========================================================================
// 6. 给 /api/update/state 用
// ===========================================================================
inline std::string state_json() {
    const PendingPlan p = read_pending();
    auto& s = state();
    bool staged = false;
    std::string ver, staging;
    {
        std::lock_guard<std::mutex> lk(s.mtx);
        staged = s.staged;
        ver = s.dlVersion;
        staging = s.stagingDir;
    }
    std::string j = "{";
    j += "\"ok\":true";
    j += ",\"current\":\"" + json_escape(current_version()) + "\"";
    j += ",\"installDir\":\"" + json_escape(install_dir()) + "\"";
    j += ",\"skipVersion\":\"" + json_escape(skip_version()) + "\"";
    j += ",\"staged\":" + std::string(staged ? "true" : "false");
    j += ",\"stagedVersion\":\"" + json_escape(ver) + "\"";
    j += ",\"stagingDir\":\"" + json_escape(staging) + "\"";
    j += ",\"pendingExists\":" + std::string(p.exists ? "true" : "false");
    j += ",\"pendingPhase\":\"" + json_escape(p.phase) + "\"";
    j += ",\"pendingVersion\":\"" + json_escape(p.version) + "\"";
    j += ",\"pendingFrom\":\"" + json_escape(p.from) + "\"";
    j += ",\"pendingTime\":\"" + json_escape(p.time) + "\"";
    j += ",\"pendingDetail\":\"" + json_escape(p.detail) + "\"";
    j += ",\"pendingFiles\":" + std::to_string((long long)p.applied.size());
    j += ",\"backupDir\":\"" + json_escape(backup_dir(p.version)) + "\"";
    j += "}";
    return j;
}

// ===========================================================================
// 7. 请启动器退出（"立即重启并更新"用；铁律 2：只发它本来就会处理的 WM_COMMAND）
// ===========================================================================
inline bool request_launcher_exit() {
    HWND h = ::FindWindowW(L"RLPlayerTray", nullptr);
    if (!h) h = ::FindWindowW(L"RLPlayerTray", L"RLPlayerTray");
    if (!h) return false;
    ::PostMessageW(h, WM_COMMAND, (WPARAM)1003, 0);   // ID_TRAY_EXIT（tools/launcher.cpp）
    return true;
}

inline std::string powershell_unused_marker() { return std::string(); }

} // namespace rta::update
