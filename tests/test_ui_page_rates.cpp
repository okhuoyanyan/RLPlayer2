// 界面「可选档位」回归 —— 防止再次出现「DSP 能做、界面选不到」的假上限。
//
// 背景（2026-11-01）：3.0.10 的界面手选采样率列表止于 768000，而管线实测 44100→1411200
// （32×）与 48000→1536000（32×）都成立（rtaplay --rate 1411200 --analyze：4410 帧 → 144496 帧）。
// 这不是 DSP 的上限，是列表漏项。这里用 UI 骨架函数本身做判据，而不是 grep 源码：
// 断言的是**真正发给 WebView2 的那段 HTML**。
#include "net/ui_page.h"
#include <cstdio>
#include <string>

static int total = 0, fails = 0;
static void ck(bool condition, const char* name) {
    ++total;
    if (!condition) { ++fails; std::printf("FAIL %s\n", name); }
}

int main() {
    const std::string html = rta::net::rta_ui_page();
    ck(!html.empty(), "UI 骨架非空");
    // PCM 手选档
    ck(html.find("<option value=1411200>1411200</option>") != std::string::npos,
       "PCM 手选列表含 1411200（44.1k 族 32×）");
    ck(html.find("<option value=1536000>1536000</option>") != std::string::npos,
       "PCM 手选列表含 1536000（48k 族 32×）");
    ck(html.find("<option value=768000>768000</option>") != std::string::npos,
       "既有 768000 档仍在（扩展不得回退已有档位）");
    // 设置页的「输出采样率」是运行时复制 sRate.innerHTML，所以只要这里在，那边自动有。
    // DSD 输出档（隐藏态源）：DoP 与 Native 都要有 1024。
    ck(html.find("value=dop1024") != std::string::npos && html.find("value=nat1024") != std::string::npos,
       "DSD 输出档含 1024（DoP 与 Native）");
    std::printf("ui_page_rates: %d checks, %d failures\n", total, fails);
    return fails ? 1 : 0;
}
