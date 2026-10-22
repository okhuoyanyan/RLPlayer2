// test_ratepick.cpp — 输出采样率选择回归（2026-10-22 现场事故：第二首 44.1k FLAC 按 44100 开流）
//
// 事故形态（C:\Users\Administrator\Downloads\rlplayer-playback (4).log 第 9~23 行）：
//   第一首 44.1k FLAC，rate=max → 探到 96000 → 正常播放（有 PLAY + STAT）；
//   紧接着第二首 44.1k FLAC，rate=max 的 exclusive_rates() 【瞬时返回空】→ 旧代码 want=0 →
//   被当成「跟随源率」→ 按 44100 去开独占流 → 端点只支持 48k/96k 族 → 在 PLAY 之前报错退出。
//
// 本测试只测纯逻辑（src/audio/rate_pick.h），不开任何音频设备（铁律 1），
// 判据就是「探测为空不能被解释成探测结果是源率」这一条 —— 旧逻辑下必然 FAIL。
#include "audio/rate_pick.h"
#include <cstdio>
#include <string>
#include <vector>

#ifdef _WIN32
extern "C" __declspec(dllimport) int __stdcall SetConsoleOutputCP(unsigned int);
#endif
#ifndef CP_UTF8
#define CP_UTF8 65001
#endif

using rta::audio::RatePick;
using rta::audio::pick_output_rate;
using rta::audio::pick_supported_rate;

static int g_fail = 0;
static void ck(bool cond, const char* what, const std::string& detail = "") {
    if (cond) std::printf("  [PASS] %s\n", what);
    else { std::printf("  [FAIL] %s  %s\n", what, detail.c_str()); ++g_fail; }
}
static std::string num(uint32_t v) { return std::to_string((unsigned long long)v); }

// 便捷断言：rate 相等时把实际值打出来，方便看差在哪
static void ck_rate(uint32_t got, uint32_t want, const char* what) {
    ck(got == want, what, "want=" + num(want) + " got=" + num(got));
}

int main() {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    std::printf("== ratepick：输出采样率选择纯逻辑（不开设备）==\n");

    // ---------------- 1. pick_output_rate：具体数值 ----------------
    {
        std::printf("[1] 明确指定 96000\n");
        const RatePick r = pick_output_rate("96000", 44100, {48000});
        ck_rate(r.rate, 96000, "具体数值 96000 → rate=96000");
        ck(r.decided, "具体数值 → decided=true");
        // 明确选择优先：即使设备列表里没有也不改（能力检查在调用方照旧报错）
        ck(r.rate != 48000, "不因 supported={48000} 被改成 48000（明确选择优先）", "got=" + num(r.rate));
        ck(r.note.empty(), "具体数值 → note 留空", r.note);
        ck(r.logTag.empty(), "具体数值 → logTag 留空", r.logTag);
    }

    // ---------------- 2. pick_output_rate：跟随源 ----------------
    {
        std::printf("[2] \"src\" 与空字符串 → 跟随源\n");
        for (const char* req : {"src", ""}) {
            const RatePick r = pick_output_rate(req, 44100, {48000, 96000});
            ck_rate(r.rate, 44100, (std::string("requested=\"") + req + "\" → rate=源率").c_str());
            ck(r.decided, "跟随源也是确定结论 (decided=true)");
            ck(r.note.empty(), "跟随源 → note 留空", r.note);
        }
    }

    // ---------------- 3. pick_output_rate：max + 非空列表 ----------------
    {
        std::printf("[3] rate=max，supported={44100,48000,88200,96000}\n");
        const RatePick r = pick_output_rate("max", 44100, {44100, 48000, 88200, 96000});
        ck_rate(r.rate, 96000, "max → 取支持列表最大值 96000");
        ck(r.decided, "max 且列表非空 → decided=true");
        ck(r.note.empty() && r.logTag.empty(), "max 且列表非空 → note/logTag 留空",
           r.note + "|" + r.logTag);
    }

    // ---------------- 4. ★ 事故直接回归：max + 空列表 ----------------
    // 旧代码：want=0 → 下方按「跟随源」放行 44100 → 端点不支持 → PLAY 之前报错。
    // 新语义：既给可继续的 rate（=源率），又用 decided=false + logTag 明确「这不是探测结论」。
    {
        std::printf("[4] ★事故回归：rate=max，supported={}（瞬时探测为空），sourceRate=44100\n");
        const RatePick r = pick_output_rate("max", 44100, {});
        ck(!r.decided, "探测为空 → decided=false（绝不把「探测为空」当成探测结论）");
        ck(r.logTag == "RATEPROBE_EMPTY", "探测为空 → logTag=RATEPROBE_EMPTY", "got=\"" + r.logTag + "\"");
        ck(!r.note.empty(), "探测为空 → note 非空（要能进日志/信号链）");
        ck(r.note.find("44100") != std::string::npos, "note 里写明源采样率 44100", r.note);
        ck_rate(r.rate, 44100, "探测为空 → rate=源率（调用方仍可继续尝试）");
    }

    // ---------------- 5. pick_supported_rate：目标率不被接受时的自适应 ----------------
    {
        std::printf("[5] pick_supported_rate 自适应\n");

        // 本日志形态：44.1k 源，设备只有 48k 族 → 取不小于 want 的最小值
        ck_rate(pick_supported_rate(44100, 44100, {48000, 96000}), 48000,
                "44.1k 源 + 只有 48k 族 → 48000（不小于 want 的最小值）");

        // 44.1k 族且 >= want：优先同族（避免换族引入非整数比重采样）
        ck_rate(pick_supported_rate(44100, 44100, {88200, 96000, 176400}), 88200,
                "44.1k 族有 >= want → 取同族最小值 88200");

        // ★ 同族优先真正有区别的一条：混族列表里 48000 比 88200 小，但 88200 同族 → 必须选 88200。
        // 这一条是「同族分支」的判据（去掉同族分支会落到 ③ 变成 48000）。
        ck_rate(pick_supported_rate(44100, 44100, {48000, 88200}), 88200,
                "混族 {48000,88200} → 88200（同族优先于『最小值』）");

        // want 在列表里没有、同族也没有 >= want 的 → 落到「不小于 want 的最小值」
        ck_rate(pick_supported_rate(96000, 44100, {44100, 48000, 88200}), 88200,
                "want=96000 不在列表 → 88200（同族/最小值同结论，取 88200）");

        // 全都低于 want → 取最大（宁可降一点也不出声）
        ck_rate(pick_supported_rate(192000, 44100, {44100, 48000}), 48000,
                "全低于 want=192000 → 取最大 48000（宁可降也不能不出声）");

        // 空列表 → 无可挑
        ck_rate(pick_supported_rate(44100, 44100, {}), 0, "supported={} → 0（无可挑，调用方报错）");

        // 完全相等优先：即使同族里还有更合适的
        ck_rate(pick_supported_rate(48000, 44100, {44100, 48000, 96000}), 48000,
                "want 恰在列表里 → 原样返回 48000");
    }

    // ---------------- 6. 乱序 / 重复 / 边界：结果与有序一致 ----------------
    {
        std::printf("[6] 乱序 / 重复 / 边界（不许崩、不许 UB）\n");
        ck_rate(pick_output_rate("max", 44100, {96000, 44100, 88200, 48000}).rate, 96000,
                "乱序列表 max → 仍取最大值 96000");
        ck_rate(pick_output_rate("max", 44100, {96000, 48000, 96000, 48000}).rate, 96000,
                "重复列表 max → 仍取最大值 96000");
        // 乱序：与有序输入必须给出完全相同的结论（含 44.1k 族与非族两种形态）
        {
            const std::vector<uint32_t> ordered  = {44100, 48000, 88200, 96000};
            const std::vector<uint32_t> shuffled = {96000, 44100, 88200, 48000};
            ck_rate(pick_supported_rate(44100, 44100, shuffled),
                    pick_supported_rate(44100, 44100, ordered),
                    "乱序输入结论 == 有序输入结论");
            // 该列表里恰好有 44100 本身 → 命中 ①「完全相等」，原样返回 44100（不必升频）
            ck_rate(pick_supported_rate(44100, 44100, shuffled), 44100,
                    "乱序列表含 want 本身 → 原样 44100（命中『完全相等』）");
            ck_rate(pick_output_rate("max", 44100, shuffled).rate,
                    pick_output_rate("max", 44100, ordered).rate,
                    "乱序 max 结论 == 有序 max 结论");
        }
        ck_rate(pick_supported_rate(44100, 44100, {88200, 88200, 48000, 48000}), 88200,
                "重复 {88200,88200,48000,48000} → 同族 88200");
        // sourceRate==0：不参与同族判定，也不能把结果带成 0（supported 非空时）
        ck_rate(pick_supported_rate(44100, 0, {48000, 96000}), 48000,
                "sourceRate=0 → 仍返回可用率 48000");
        {
            const RatePick r = pick_output_rate("max", 0, {});
            ck(r.rate == 0 && !r.decided && r.logTag == "RATEPROBE_EMPTY",
               "sourceRate=0 + 空列表 → rate=0/decided=false/有 logTag（不崩）",
               "rate=" + num(r.rate) + " tag=" + r.logTag);
        }
        // 支持率列表里的 0 是垃圾值：必须被剔除，不能当成「最高支持率」
        ck_rate(pick_output_rate("max", 44100, {0, 48000, 96000}).rate, 96000,
                "列表含 0（垃圾值）→ 剔除后取 96000");
        ck_rate(pick_supported_rate(44100, 44100, {0}), 0,
                "列表只含 0 → 视作无可挑 0");
        // 非法数值字符串：不能静默变成 0 Hz 去开流
        {
            const RatePick r = pick_output_rate("abc", 44100, {48000});
            ck(r.rate == 44100 && !r.note.empty(),
               "非法数值 \"abc\" → 跟随源且留话（不悄悄开 0 Hz）",
               "rate=" + num(r.rate) + " note=" + r.note);
        }
    }

    // ---------------- 7. ★ 功能1（2026-10-22）：follow = 跟随系统采样率 ----------------
    // 用户原话：「软件打开后默认输出采样率为本设备最高，这样直接播放通常是不出声的。
    // 请首次打开锁定的是声卡当前设置的采样率」。
    // 判据：传了 systemRate 就必须用它；没传（0）时必须【留话】并且不猜高率。
    {
        std::printf("[7] follow：跟随系统采样率（用户功能1）\n");
        const RatePick r = pick_output_rate("follow", 44100, {44100, 48000, 96000, 192000}, 48000);
        ck_rate(r.rate, 48000, "follow + 系统 48000 → 用 48000（不是设备最高 192000）");
        ck(r.decided, "follow 取到系统率 → decided=true");
        ck(r.note.empty() && r.logTag.empty(), "follow 正常 → note/logTag 留空",
           r.note + "|" + r.logTag);

        // 系统率恰好等于源率：也不能被换成设备最高率
        ck_rate(pick_output_rate("follow", 44100, {44100, 192000}, 44100).rate, 44100,
                "follow + 系统 44100 → 44100（不升到 192000）");

        // 系统率不在 supported 里：follow 的语义是「按系统配置开流」，
        // 所以仍然返回系统率；能力检查由调用方如实报错（与"明确数值"一致）
        ck_rate(pick_output_rate("follow", 44100, {48000, 96000}, 192000).rate, 192000,
                "follow + 系统率不在 supported → 仍返回系统率（由调用方做能力检查）");

        // ★ 读不到系统率：绝不能悄悄换成设备最高率（那正是用户抱怨的行为）
        const RatePick rn = pick_output_rate("follow", 44100, {48000, 192000}, 0);
        ck_rate(rn.rate, 44100, "follow + 系统率未知(0) → 退回源率 44100");
        ck(rn.logTag == "SYSRATE_EMPTY", "系统率未知 → logTag=SYSRATE_EMPTY",
           "got=\"" + rn.logTag + "\"");
        ck(!rn.note.empty(), "系统率未知 → note 非空（要能进日志）");

        // max 的旧行为不能被 follow 改动（回归保护）
        ck_rate(pick_output_rate("max", 44100, {48000, 192000}, 48000).rate, 192000,
                "max 仍取设备最高 192000（follow 不影响旧档）");
    }

    if (g_fail == 0) std::printf("== 全部通过 ==\n");
    else std::printf("== 失败 %d 项 ==\n", g_fail);
    return g_fail ? 1 : 0;
}