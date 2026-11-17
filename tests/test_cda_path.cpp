// cda_path —— .cda 路径解析回归（2026-11-15）
//
// 为什么单独测：.cda 只有 44 字节索引，真正音频在 CD 轨道里；用户拖进来的
// 文件名可能是 Track01.cda，也可能被上传链路存成
// <hash>-<字节数>-Track01.cda。旧解析取"第一段数字"，上传路径会把 hash 当轨道号；
// 改成"最后一段数字"后又曾在扩展名处把 got 清零。这里把两种路径都钉死。
#include "audio/cda_mci.h"
#include <cstdio>
#include <string>

int main() {
    struct Case { const char* p; bool ok; char letter; int track; };
    const Case cases[] = {
        { "H:\\Track01.cda", true, 'H', 1 },
        { "C:\\Users\\x\\AppData\\Local\\Temp\\e6b811254f6f9f13-2c-Track01.cda", true, 'C', 1 },
        { "D:\\music\\Track 12.CDA", true, 'D', 12 },
        { "H:\\CD1 Track02.cda", true, 'H', 2 },
        { "H:\\Track99.cda", true, 'H', 99 },
        { "H:\\Track100.cda", false, 0, 0 },
        { "H:\\no_digits.cda", false, 0, 0 },
        { "H:\\Track01.wav", false, 0, 0 },
        { "H:\\Track01.cda.bak", false, 0, 0 },
        { "H:\\Track01 (1).cda", true, 'H', 1 },
    };
    int fail = 0;
    for (const auto& c : cases) {
        std::string letter;
        int track = 0;
        const bool ok = rta::audio::CdaMciPlayer::parseCdaPath(c.p, letter, track);
        const bool pass = (ok == c.ok) && (!ok || (letter == std::string(1, c.letter) && track == c.track));
        if (!pass) {
            std::printf("FAIL %s -> ok=%d letter=%s track=%d (want ok=%d %c/%d)\n",
                        c.p, ok ? 1 : 0, letter.c_str(), track, c.ok ? 1 : 0, c.letter, c.track);
            ++fail;
        }
    }
    std::printf("cda_path: %zu cases, %d failed\n", sizeof(cases) / sizeof(cases[0]), fail);
    return fail ? 1 : 0;
}
