// Generated filter-detail data contract. No audio device is opened.
#include "filter_detail_reference.h"
#include "dsp/filter_families.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>
#include <string>
int main() {
    using namespace rta::ui_reference;
    int failures = 0, checks = 0;
    auto ck = [&](bool ok, const char* what) {
        ++checks;
        if (!ok) { ++failures; std::printf("FAIL %s\n", what); }
    };
    ck(kFilterDetailCount == (int)rta::dsp::filter_presets().size(), "one detail per preset");
    std::set<std::string> names;
    for (int i = 0; i < kFilterDetailCount; ++i) {
        const auto& d = kFilterDetails[i];
        ck(d.name && *d.name, "name");
        ck(names.insert(d.name).second, "unique name");
        ck(rta::dsp::find_preset(d.name) != nullptr, "preset exists");
        ck(d.taps > 0 && d.T > 0 && d.L > 0, "tap geometry");
        ck(std::isfinite(d.passHz) && d.passHz > 0, "passband");
        ck(std::isfinite(d.stopStartHz) && d.stopStartHz > d.passHz, "stopband start after passband");
        ck(d.targetDb > 0 && d.designDb >= d.targetDb - 1e-3, "design attenuation reaches family target");
        ck(std::isfinite(d.passDb) && std::abs(d.passDb) < 0.5, "measured passband deviation is small");
        ck(std::isfinite(d.stopDb) && d.stopDb < -100.0, "measured stopband below -100 dB");
        ck(std::isfinite(d.dcDb) && std::abs(d.dcDb) < 1e-3, "DC unity");
        ck(std::isfinite(d.gd1kMs) && d.gd1kMs >= 0.0 && d.gd1kMs < 1000.0, "group delay finite");
        ck(std::isfinite(d.preMs) && std::isfinite(d.postMs) && d.preMs >= 0 && d.postMs >= 0, "ringing envelope");
        ck(d.response && d.responseCount >= 512, "dense response curve");
        ck(d.passCurve && d.passCount >= 256, "dense passband curve");
        ck(d.gdCurve && d.gdCount >= 32, "group-delay curve");
        ck(d.impulse && d.impulseCount >= 129, "dense impulse curve");
        // 全频带证书：32769 个 FFT 频点（65536 点 FFT），阻带/通带必须用稠密网格复核。
        ck(d.fullBandPoints >= 32769, "full-band FFT grid");
        ck(std::isfinite(d.fullBandStopDb) && d.fullBandStopDb < -100.0, "full-band stopband");
        ck(d.fullBandStopDb <= d.stopDb + 1e-3, "dense stopband covers sparse result");
        ck(std::isfinite(d.fullBandPassDevDb) && std::abs(d.fullBandPassDevDb) >= std::abs(d.passDb) - 1e-3,
           "dense passband deviation covers sparse result");
        ck(std::isfinite(d.fullBandMaxStepDb) && d.fullBandMaxStepDb >= 0.0 && d.fullBandMaxStepDb < 400.0,
           "full-band adjacent step finite");
        float peak = 0.0f;
        for (int k = 0; k < d.impulseCount; ++k) peak = std::max(peak, std::fabs(d.impulse[k]));
        ck(peak > 0.95f && peak <= 1.0001f, "normalized impulse peak");
        ck(d.response[d.responseCount - 1] <= 0.0f, "full-band response is relative to unity");
    }
    std::printf("filter detail reference: checks=%d failures=%d presets=%d\n", checks, failures, kFilterDetailCount);
    return failures ? 1 : 0;
}
