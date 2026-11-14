// 2026-11-01（用户 261101 #1）：模型把「原文 + 译文」写在同行时，必须剥掉回显的原文。
// 这个判据证明的是"界面上每一行只显示译文"，不是"函数被调用过"。
#include "meta/lyrics_ai.h"
#include <cstdio>
#include <string>

using rta::meta::lyrics_strip_echoed_source;

static int fails = 0;
static void ck(const char* name, const std::string& got, const std::string& want) {
    if (got != want) {
        std::printf("FAIL %s: got=[%s] want=[%s]\n", name, got.c_str(), want.c_str());
        ++fails;
    } else {
        std::printf("ok   %-28s -> [%s]\n", name, got.c_str());
    }
}

int main() {
    ck("jp-space", lyrics_strip_echoed_source("あばいたはずの暗闇", "あばいたはずの暗闇 当我划破了黑暗"), "当我划破了黑暗");
    ck("ascii-dash", lyrics_strip_echoed_source("abc", "ABC — 甲乙丙"), "甲乙丙");
    ck("colon", lyrics_strip_echoed_source("abc", "abc：译文"), "译文");
    ck("arrow", lyrics_strip_echoed_source("abc", "abc → translated"), "translated");
    ck("plain-only", lyrics_strip_echoed_source("abc", "纯译文"), "纯译文");
    ck("identical", lyrics_strip_echoed_source("abc", "abc"), "abc");
    ck("echo-empty-rest", lyrics_strip_echoed_source("abc", "abc - "), "abc - ");
    ck("no-prefix", lyrics_strip_echoed_source("abc", "ab 不同"), "ab 不同");
    std::printf("lyrics_echo: fails=%d\n", fails);
    return fails ? 1 : 0;
}
