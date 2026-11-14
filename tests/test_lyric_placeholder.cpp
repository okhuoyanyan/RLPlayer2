// test_lyric_placeholder.cpp — 占位歌词识别（2026-11-02，用户 261102-01 #2）
//
// 现象：同样"没有填词"的两首歌，一首（Clear Sky）正常显示歌词，另一首（19：30）
//       只在歌词区显示孤零零一行「纯音乐，请欣赏」——那是源站返回的占位词，被当成了歌词。
// 判据（铁律 3）：不是"能不能跑"，而是**同一条占位句必须被判成占位，真歌词必须不被误杀**。
//       特别是"含『纯音乐』三个字的真歌词"这一类边界，必须给出反例。
#include "meta/provider.h"
#include <cstdio>
#include <string>

static int g_fail = 0;
static void expect(bool got, bool want, const char* what) {
    if (got == want) { std::printf("  ok    %s -> %s\n", what, got ? "占位" : "真歌词"); return; }
    std::printf("  FAIL  %s -> got %s want %s\n", what, got ? "占位" : "真歌词", want ? "占位" : "真歌词");
    ++g_fail;
}

int main() {
    using rta::meta::pv_lyric_is_placeholder;
    std::printf("== 占位歌词识别 ==\n");
    // 1) 源站常见占位句（中/繁/日/英）
    expect(pv_lyric_is_placeholder("纯音乐，请欣赏"), true, "纯音乐，请欣赏");
    expect(pv_lyric_is_placeholder("[00:00.00]纯音乐，请欣赏"), true, "[00:00.00]纯音乐，请欣赏");
    expect(pv_lyric_is_placeholder("[00:00.00]此歌曲为没有填词的纯音乐,请您欣赏"), true, "此歌曲为没有填词的纯音乐…");
    expect(pv_lyric_is_placeholder("暂无歌词"), true, "暂无歌词");
    expect(pv_lyric_is_placeholder("純音樂，請欣賞"), true, "純音樂，請欣賞（繁）");
    expect(pv_lyric_is_placeholder("Instrumental"), true, "Instrumental");
    expect(pv_lyric_is_placeholder("[00:01.20]インスト曲です"), true, "インスト曲です");
    // 2) 反例：真歌词里出现"纯音乐"三个字，但不能被误杀（长度也远超阈值）
    expect(pv_lyric_is_placeholder(
               "[00:12.30]你说的纯音乐我听不懂\n[00:18.00]那就让鼓点替我回答\n"
               "[00:24.10]把所有心事都写成和弦\n[00:30.00]再一点点拆开给你听\n"
               "[00:36.20]夜色落在琴键上\n[00:42.00]我们把沉默弹成歌"),
           false, "含『纯音乐』的真歌词（6 行）");
    // 3) 反例：普通多行歌词
    expect(pv_lyric_is_placeholder(
               "[00:05.00]晴天里有风\n[00:09.50]吹过你的发梢\n[00:14.00]我在原地等一个回答"),
           false, "普通三行歌词");
    // 4) 空串 = 没歌词（按占位处理，界面走空态）
    expect(pv_lyric_is_placeholder(""), true, "空串");

    if (g_fail) { std::printf("\n失败 %d 项\n", g_fail); return 1; }
    std::printf("\n全部通过\n");
    return 0;
}
