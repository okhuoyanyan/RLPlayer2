#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "audio/media_info.h"
#include "meta/enrich.h"
#include "meta/entities.h"
#include "meta/store.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>
#include <windows.h>

using namespace rta::meta;
namespace fs_std = std::filesystem;

static int g_fail = 0;
static void ck(bool cond, const std::string& what, const std::string& detail = "") {
    if (cond) std::printf("  [PASS] %s\n", what.c_str());
    else { std::printf("  [FAIL] %s  %s\n", what.c_str(), detail.c_str()); ++g_fail; }
}

static std::string run_cmd(const std::wstring& cmd) {
    std::string out;
    rta::audio::run_hidden(cmd, &out);
    return out;
}

int main() {
    SetConsoleOutputCP(CP_UTF8);
    std::printf("== E-15 古典 COMPOSER->Work 四层端到端 (合成样本 + 真实批量入口) ==\n");

    const std::string ffmpeg = rta::audio::find_tool("ffmpeg");
    const std::string ffprobe = rta::audio::find_tool("ffprobe");
    if (ffmpeg.empty() || ffprobe.empty()) {
        std::printf("  [FAIL] 环境中缺少 ffmpeg 或 ffprobe 工具，无法合成与探测样本\n");
        return 1;
    }

    // 1. 在 %TEMP% 创建隔离沙箱目录
    char tmpEnv[MAX_PATH];
    GetTempPathA(MAX_PATH, tmpEnv);
    const std::string testId = std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
    const fs_std::path rootDir = fs_std::path(tmpEnv) / ("rlplayer_e15_" + testId);
    const fs_std::path classicalDir = rootDir / "classical" / "Beethoven Symphonies";
    const fs_std::path popDir = rootDir / "pop" / "Jay Album";
    const fs_std::path metaBase = rootDir / "rlplayer-meta";

    fs_std::create_directories(classicalDir);
    fs_std::create_directories(popDir);
    fs_std::create_directories(metaBase);

    // 隔离 MetaStore 与 EntityStore 基准目录到沙箱
    MetaStore::instance().setBaseDir(metaBase.string());
    EntityStore::instance().setBaseDir(metaBase.string());

    const fs_std::path flac1 = classicalDir / "01. Allegro.flac";
    const fs_std::path flac2 = classicalDir / "02. Andante.flac";
    const fs_std::path flac3 = popDir / "01. Qingtian.flac";

    std::printf("-- 1. ffmpeg 合成样本 --\n");
    // 样本 1：贝多芬第五交响曲 第一乐章（古典）
    {
        std::wstring cmd = L"\"" + rta::audio::widen(ffmpeg) + L"\" -y -v error -f lavfi -i anullsrc=r=44100:cl=stereo -t 0.1 "
                           L"-metadata TITLE=\"Symphony No. 5 in C minor, Op. 67: I. Allegro con brio\" "
                           L"-metadata COMPOSER=\"Ludwig van Beethoven\" "
                           L"-metadata WORK=\"Symphony No. 5 in C minor, Op. 67\" "
                           L"-metadata CONDUCTOR=\"Herbert von Karajan\" "
                           L"-metadata PERFORMER=\"Berliner Philharmoniker\" "
                           L"-metadata ALBUM=\"Beethoven: 9 Symphonies\" "
                           L"-metadata DATE=\"1963\" \"" + flac1.wstring() + L"\"";
        run_cmd(cmd);
        ck(fs_std::exists(flac1), "FLAC 1 合成成功: 贝五第一乐章");
    }

    // 样本 2：贝多芬第五交响曲 第二乐章（古典同一 Work/Release）
    {
        std::wstring cmd = L"\"" + rta::audio::widen(ffmpeg) + L"\" -y -v error -f lavfi -i anullsrc=r=44100:cl=stereo -t 0.1 "
                           L"-metadata TITLE=\"Symphony No. 5 in C minor, Op. 67: II. Andante con moto\" "
                           L"-metadata COMPOSER=\"Ludwig van Beethoven\" "
                           L"-metadata WORK=\"Symphony No. 5 in C minor, Op. 67\" "
                           L"-metadata CONDUCTOR=\"Herbert von Karajan\" "
                           L"-metadata PERFORMER=\"Berliner Philharmoniker\" "
                           L"-metadata ALBUM=\"Beethoven: 9 Symphonies\" "
                           L"-metadata DATE=\"1963\" \"" + flac2.wstring() + L"\"";
        run_cmd(cmd);
        ck(fs_std::exists(flac2), "FLAC 2 合成成功: 贝五第二乐章");
    }

    // 样本 3：周杰伦 晴天（流行退化，无 COMPOSER 标签）
    {
        std::wstring cmd = L"\"" + rta::audio::widen(ffmpeg) + L"\" -y -v error -f lavfi -i anullsrc=r=44100:cl=stereo -t 0.1 "
                           L"-metadata TITLE=\"晴天\" "
                           L"-metadata ARTIST=\"周杰伦\" "
                           L"-metadata ALBUM=\"叶惠美\" "
                           L"-metadata DATE=\"2003\" \"" + flac3.wstring() + L"\"";
        run_cmd(cmd);
        ck(fs_std::exists(flac3), "FLAC 3 合成成功: 流行单曲《晴天》");
    }

    std::printf("-- 2. probe_media 标签读取 --\n");
    const auto mi1 = rta::audio::probe_media(flac1.string());
    ck(mi1.composer == "Ludwig van Beethoven", "probe_media 读取 COMPOSER 成功: " + mi1.composer);
    ck(mi1.work == "Symphony No. 5 in C minor, Op. 67", "probe_media 读取 WORK 成功: " + mi1.work);
    ck(mi1.conductor == "Herbert von Karajan", "probe_media 读取 CONDUCTOR 成功: " + mi1.conductor);
    ck(mi1.performer == "Berliner Philharmoniker", "probe_media 读取 PERFORMER 成功: " + mi1.performer);

    const auto mi3 = rta::audio::probe_media(flac3.string());
    ck(mi3.composer.empty(), "流行样本 probe_media COMPOSER 为空");
    ck(mi3.artist == "周杰伦" && mi3.title == "晴天", "流行样本读取 ARTIST/TITLE 成功");

    std::printf("-- 3. Enrich / Store 流程落盘 --\n");
    auto store_file_tags = [](const std::string& file) {
        const auto mi = rta::audio::probe_media(file);
        TrackMeta tm;
        tm.path = file;
        const long long now = 1790000000;
        auto add_tag = [&](const char* field, const std::string& val) {
            if (!val.empty()) {
                MetaValue mv;
                mv.value = val;
                mv.source = "tag";
                mv.confidence = 1.0f;
                mv.ts = now;
                MetaStore::mergeField(tm, field, mv);
            }
        };
        add_tag("title", mi.title);
        add_tag("artist", mi.artist);
        add_tag("album", mi.album);
        add_tag("year", mi.date);
        add_tag("genre", mi.genre);
        add_tag("composer", mi.composer);
        add_tag("work", mi.work);
        add_tag("conductor", mi.conductor);
        add_tag("performer", mi.performer);
        MetaStore::instance().save(tm);
    };

    store_file_tags(flac1.string());
    store_file_tags(flac2.string());
    store_file_tags(flac3.string());

    // 验证 Enrich 编排在已有本地标签场景下的幂等行为
    const EnrichOpts offOpts = make_enrich_opts(false, false, false, false, false);
    const auto st1 = enrich_track(flac1.string(), false, offOpts);
    const auto st2 = enrich_track(flac2.string(), false, offOpts);
    const auto st3 = enrich_track(flac3.string(), false, offOpts);
    ck(st1.skipped, "FLAC 1 已有完整本地标签，enrich 流程正确识别并保留原有字段");
    ck(st2.skipped, "FLAC 2 已有完整本地标签，enrich 流程正确识别并保留原有字段");
    ck(st3.skipped, "FLAC 3 已有完整本地标签，enrich 流程正确识别并保留原有字段");

    TrackMeta tm1, tm3;
    ck(MetaStore::instance().load(flac1.string(), tm1), "FLAC 1 成功存入并读回 MetaStore");
    ck(tm1.fields["composer"].value == "Ludwig van Beethoven", "MetaStore 中保存了 composer 字段");
    ck(tm1.fields["work"].value == "Symphony No. 5 in C minor, Op. 67", "MetaStore 中保存了 work 字段");
    ck(tm1.fields["conductor"].value == "Herbert von Karajan", "MetaStore 中保存了 conductor 字段");
    ck(tm1.fields["performer"].value == "Berliner Philharmoniker", "MetaStore 中保存了 performer 字段");

    ck(MetaStore::instance().load(flac3.string(), tm3), "FLAC 3 成功存入 MetaStore");
    ck(tm3.fields["title"].value == "晴天" && tm3.fields["artist"].value == "周杰伦", "流行曲目字段完整");
    ck(tm3.fields.find("composer") == tm3.fields.end(), "流行曲目 MetaStore 无 composer 字段");

    std::printf("-- 4. 真实批量入口: entity_batch_from_paths --\n");
    const std::vector<std::string> paths = { flac1.string(), flac2.string(), flac3.string() };
    const EntityGraph g = entity_batch_from_paths(paths, 1790000000);

    std::printf("-- 5. 古典四层实体与边断言 (Composer -> Work -> Recording -> Release) --\n");
    // Layer 1: Composer (Person)
    const std::string composerId = entity_id(EntityKind::Person, "Ludwig van Beethoven");
    const Entity* composerEnt = g.find(composerId);
    ck(composerEnt != nullptr, "Layer 1: Composer 实体存在 (ID=" + composerId + ")");
    if (composerEnt) {
        ck(composerEnt->kind == EntityKind::Person, "Composer 是 Person 实体（角色化，非独立类型）");
        const auto itRole = composerEnt->attrs.find("role");
        ck(itRole != composerEnt->attrs.end() && itRole->second.value == "composer",
           "Composer 实体的 role 属性为 composer");
    }

    // Layer 2: Work
    const std::string workId = entity_make_id("work", entity_norm_key(EntityKind::Work, { "Symphony No. 5 in C minor, Op. 67", "Ludwig van Beethoven" }));
    const Entity* workEnt = g.find(workId);
    ck(workEnt != nullptr, "Layer 2: Work 实体存在 (ID=" + workId + ")");
    if (workEnt) {
        ck(workEnt->kind == EntityKind::Work, "Work 实体类型正确");
        ck(workEnt->name == "Symphony No. 5 in C minor, Op. 67", "Work 实体名称匹配");
        const EntityEdge* creditEdge = workEnt->findEdge(entity_rel::kCredit, composerId, "composer");
        ck(creditEdge != nullptr, "★ Work --credit(composer)--> Composer(Person) 边正确存在");
    }

    // Layer 3: Recording
    if (workEnt) {
        std::vector<const EntityEdge*> recEdges;
        for (const auto& ed : workEnt->edges) {
            if (ed.rel == entity_rel::kHasRecording) recEdges.push_back(&ed);
        }
        ck(recEdges.size() == 2, "★ Work --has_recording--> Recording: 同一 Work 下聚合了两个乐章录音", "数量=" + std::to_string(recEdges.size()));
        for (const auto* re : recEdges) {
            const Entity* recEnt = g.find(re->to);
            ck(recEnt != nullptr && recEnt->kind == EntityKind::Recording, "录音实体存在且类型为 Recording");
            if (recEnt) {
                // 指挥是 Person
                const std::string condId = entity_id(EntityKind::Person, "Herbert von Karajan");
                const EntityEdge* condEdge = recEnt->findEdge(entity_rel::kCredit, condId, "conductor");
                ck(condEdge != nullptr, "Recording --credit(conductor)--> Karajan(Person)");
                const Entity* condEnt = g.find(condId);
                ck(condEnt != nullptr && condEnt->kind == EntityKind::Person, "指挥是 Person 实体");

                // 演奏者是 Group (role=performer) —— 乐团名启发式扩展后正确判定为 Group
                const std::string bpoId = entity_id(EntityKind::Group, "Berliner Philharmoniker");
                const EntityEdge* bpoEdge = recEnt->findEdge(entity_rel::kCredit, bpoId, "performer");
                ck(bpoEdge != nullptr, "Recording --credit(performer)--> Berliner Philharmoniker(Group)");
                const Entity* bpoEnt = g.find(bpoId);
                ck(bpoEnt != nullptr && bpoEnt->kind == EntityKind::Group, "演奏者是 Group 实体（乐团）");
            }
        }
    }

    // Layer 4: Release
    const std::string releaseId = entity_make_id("release", entity_norm_key(EntityKind::Release, { "Beethoven: 9 Symphonies", "1963" }));
    const Entity* relEnt = g.find(releaseId);
    ck(relEnt != nullptr, "Layer 4: Release 实体存在");
    if (relEnt && workEnt) {
        ck(relEnt->kind == EntityKind::Release, "实体类型为 Release");
        std::vector<const EntityEdge*> recEdges;
        for (const auto& ed : workEnt->edges) {
            if (ed.rel == entity_rel::kHasRecording) recEdges.push_back(&ed);
        }
        for (const auto* re : recEdges) {
            const Entity* recEnt = g.find(re->to);
            if (recEnt) {
                ck(recEnt->findEdge(entity_rel::kReleasedAs, releaseId, "") != nullptr,
                   "★ Recording --released_as--> Release 正确链接到古典专辑");
            }
        }
    }

    std::printf("-- 6. 流行退化并存断言 (Artist -> Release -> Track) --\n");
    const std::string popArtistId = entity_id(EntityKind::Person, "周杰伦");
    const Entity* popArtistEnt = g.find(popArtistId);
    ck(popArtistEnt != nullptr && popArtistEnt->kind == EntityKind::Person, "流行艺人是 Person 实体");

    const std::string popReleaseId = entity_make_id("release", entity_norm_key(EntityKind::Release, { "叶惠美", "2003" }));
    const Entity* popReleaseEnt = g.find(popReleaseId);
    ck(popReleaseEnt != nullptr && popReleaseEnt->kind == EntityKind::Release, "流行专辑 Release 实体存在");
    if (popArtistEnt && popReleaseEnt) {
        ck(popArtistEnt->findEdge(entity_rel::kReleaseArtist, popReleaseId, "") != nullptr,
           "★ Artist --release_artist--> Release: 流行退化路径生效（跳过 Work 层）");
    }

    // 全图计数断言：古典四层与流行退化并存且无冲突
    ck(g.count(EntityKind::Work) == 1, "全局严格为 1 个 Work 实体 (流行曲绝不产生空壳 Work 噪声)");
    ck(g.count(EntityKind::Release) == 2, "全局严格为 2 个 Release 实体 (古典专辑与流行专辑并存不冲突)");
    ck(g.count(EntityKind::Track) == 3, "全局严格为 3 个 Track 实体 (2 首古典 + 1 首流行)");
    ck(g.count(EntityKind::Recording) == 3, "全局严格为 3 个 Recording 实体");

    // 7. 持久化落盘测试
    std::printf("-- 7. EntityStore 落盘与读回 --\n");
    size_t saved = 0;
    for (const auto& kv : g.entities) {
        if (EntityStore::instance().save(kv.second)) ++saved;
    }
    ck(saved == g.entities.size(), "图内全部实体成功写入磁盘: " + std::to_string(saved));

    Entity loadedComp, loadedWork;
    ck(EntityStore::instance().load(composerId, loadedComp) && loadedComp.kind == EntityKind::Person, "落盘读回: Composer 是 Person");
    ck(EntityStore::instance().load(workId, loadedWork) && loadedWork.kind == EntityKind::Work, "落盘读回: Work 实体完整");

    // 清理沙箱目录
    std::error_code ec;
    fs_std::remove_all(rootDir, ec);

    std::printf("\n测试结果: %s (失败数: %d)\n", g_fail == 0 ? "全部通过" : "存在失败", g_fail);
    return g_fail == 0 ? 0 : 1;
}
