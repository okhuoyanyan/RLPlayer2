// 滤波器原型磁盘缓存：必须"只加速、不改结果"。
//
// 判据（铁律 3）：不是"第二次更快"，而是**冷启动设计的原型与热启动读回的原型逐样本相等**。
// 时间只作参考输出，不做断言（机器负载会让计时抖动）。
#include "dsp/resampler.h"
#include "platform/path.h"
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <vector>

static int total = 0, fails = 0;
static void ck(bool c, const char* name) {
    ++total;
    if (!c) { ++fails; std::printf("FAIL %s\n", name); }
}

int main() {
    using namespace rta::dsp;
    // Never purge the executable's real cache. Every test run owns a fresh directory.
    const auto isolated = std::filesystem::temp_directory_path() /
        ("rlplayer-filter-cache-test-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    _putenv_s("RLPLAYER_FILTER_CACHE_DIR", rta::platform::to_utf8(isolated).c_str());
    const std::string dir = filter_cache::directory();
    ck(!dir.empty(), "cache directory resolvable");
    if (dir.empty()) { std::printf("filter_cache: %d checks, %d failures\n", total, fails); return 1; }

    std::error_code ec;
    ck(std::filesystem::is_empty(rta::platform::from_utf8(dir)), "isolated cache starts empty");

    ResamplerConfig c;
    c.inRate = 44100; c.outRate = 384000; c.channels = 2;
    c.minTapsPerInput = 528;           // 528 × L(1280) = 675840 抽头 ≥ kMinTapsToCache
    c.phase = PhaseKind::Minimum;      // mp：相位变换最慢的那条路径
    c.window = WindowKind::Kaiser;
    c.transition = 0.02;
    c.attenDb = 280;
    c.maxPrototypeTaps = 1 << 23;
    c.apodizing = false;

    const auto t0 = std::chrono::steady_clock::now();
    PolyphaseResampler a(c);
    const double cold = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::vector<double> pa;
    a.prototype_into(pa);
    ck(pa.size() >= filter_cache::kMinTapsToCache, "prototype is large enough to be cached");

    std::size_t files = 0;
    for (auto it = std::filesystem::directory_iterator(rta::platform::from_utf8(dir), ec);
         !ec && it != std::filesystem::directory_iterator(); it.increment(ec))
        ++files;
    ck(files >= 1, "cache file written after cold design");

    const auto t1 = std::chrono::steady_clock::now();
    PolyphaseResampler b(c);
    const double warm = std::chrono::duration<double>(std::chrono::steady_clock::now() - t1).count();
    std::vector<double> pb;
    b.prototype_into(pb);

    ck(pa.size() == pb.size(), "cached prototype has identical length");
    std::size_t different = 0;
    double maxAbs = 0.0;
    for (std::size_t i = 0; i < pa.size() && i < pb.size(); ++i) {
        if (std::memcmp(&pa[i], &pb[i], sizeof(double)) != 0) ++different;
        const double d = pa[i] > pb[i] ? pa[i] - pb[i] : pb[i] - pa[i];
        if (d > maxAbs) maxAbs = d;
    }
    ck(different == 0, "cached prototype is bit-identical to the freshly designed one");
    std::printf("filter_cache: cold=%.2fs warm=%.2fs files=%zu different=%zu maxAbs=%.17g\n",
                cold, warm, files, different, maxAbs);

    // 关闭开关时必须不走缓存（不能"看起来用了缓存"却其实没有开关）
    ck(filter_cache::enabled(), "cache enabled by default");
    _putenv_s("RLPLAYER_FILTER_CACHE", "0");
    ck(!filter_cache::enabled(), "cache switch disables cache");
    std::vector<double> sentinel{123};
    ck(!filter_cache::load("missing", sentinel) && sentinel.empty(), "disabled read returns no coefficients");
    _putenv_s("RLPLAYER_FILTER_CACHE", "");
    std::printf("filter_cache: %d checks, %d failures\n", total, fails);
    return fails ? 1 : 0;
}
