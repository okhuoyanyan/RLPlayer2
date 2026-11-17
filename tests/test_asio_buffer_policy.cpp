// ASIO buffer policy: pure selection logic, no driver or device is opened.
#include "audio/asio_out.h"
#include <cstdio>
int main() {
    using namespace rta::audio;
    int failures = 0, checks = 0;
    auto ck = [&](bool ok, const char* what) { ++checks; if (!ok) { ++failures; std::printf("FAIL %s\n", what); } };
    // DSD1024: 100 ms asks for 4.5M bits, driver max is 2M -> clamp to max.
    ck(asio_pick_buffer_size(0, 2097152, 8192, -1, 45158400.0, 100.0) == 2097152, "DSD1024 clamps to max");
    // DSD64: 100 ms asks for 282240 bits, max 131072 -> clamp to max.
    ck(asio_pick_buffer_size(0, 131072, 8192, -1, 2822400.0, 100.0) == 131072, "DSD64 clamps to max");
    // PCM 44.1k at 20 ms = 882 samples, above preferred 1024? 882 < 1024, keep preferred.
    ck(asio_pick_buffer_size(512, 131072, 1024, -1, 44100.0, 20.0) == 1024, "PCM keeps preferred floor");
    // PCM 705.6k at 20 ms = 14112 samples, above preferred.
    ck(asio_pick_buffer_size(512, 131072, 1024, -1, 705600.0, 20.0) == 14112, "PCM targets 20 ms");
    // Granularity rounds up, then min/max clamp.
    ck(asio_pick_buffer_size(0, 0, 0, 64, 44100.0, 10.0) == 512, "granularity and floor");
    ck(asio_pick_buffer_size(0, 0, 0, 0, 44100.0, 100.0) == 4410, "no driver bounds uses requested ms");
    ck(asio_pick_buffer_size(0, 100, 0, 0, 44100.0, 100.0) == 100, "max clamps requested ms");
    ck(asio_dsd_bytes_per_buffer(8192) == 1024, "DSD bytes per buffer");
    ck(asio_dsd_bytes_per_buffer(1) == 1, "DSD minimum byte");
    std::printf("asio buffer policy: checks=%d failures=%d\n", checks, failures);
    return failures ? 1 : 0;
}
