// dev_caps.cpp —— 只读探测：各端点的混音声道数/采样率。不起流、不写样本、不出声。
#include <cstdio>
#include <string>
#include <vector>
#include "audio/wasapi_capture.h"
#include "audio/wasapi_out.h"
#include "audio/wasapi_shared.h"

int main() {
    std::printf("=== 输出端点（共享混音格式；仅 open 不 start，不出声）===\n");
    for (const auto& d : rta::audio::WasapiOutput::enumerate(eRender)) {
        rta::audio::WasapiSharedOutput so;
        if (so.open(d.id, 48000, 2, 32, 200.0)) {
            std::printf("  %-46s mix=%uch @ %uHz%s\n", d.name.c_str(),
                        (unsigned)so.mix_channels(), (unsigned)so.mix_rate(),
                        d.isDefault ? "  [默认]" : "");
            so.close();
        } else {
            const std::string e = so.last_error();
            std::printf("  %-46s <打不开: %s>%s\n", d.name.c_str(), e.c_str(),
                        d.isDefault ? "  [默认]" : "");
        }
    }
    std::printf("\n=== 采集端点 ===\n");
    for (const auto& d : rta::audio::WasapiCapture::enumerate_capture(true)) {
        std::printf("  %-46s mix=%uch @ %uHz%s%s\n", d.name.c_str(),
                    d.mixChannels, d.mixRate,
                    d.isLoopback ? " [Loopback]" : "",
                    d.isDefault ? "  [默认]" : "");
    }
    return 0;
}
