// Pure command-line tests. This header cannot load COM or an audio driver.
#include "audio/asio_probe_options.h"
#include <cstdio>
#include <initializer_list>
#include <vector>

using namespace rta::audio;
static int checks = 0, failures = 0;
static void check(bool ok, const char* label) {
    ++checks;
    if (!ok) { ++failures; std::printf("FAIL %s\n", label); }
}
static bool parse(std::initializer_list<const char*> args, AsioProbeOptions& o) {
    std::vector<const char*> argv{"asio_probe"};
    argv.insert(argv.end(), args.begin(), args.end());
    std::string error;
    const bool ok = parse_asio_probe_options(int(argv.size()), argv.data(), o, error);
    check(ok ? error.empty() : !error.empty(), "every invalid request has a diagnostic");
    return ok;
}
int main() {
    const char* id = "{01234567-89AB-CDEF-0123-456789ABCDEF}";
    AsioProbeOptions o;
    check(parse({}, o) && o.action == AsioProbeAction::Inspect && o.deviceIndex == -1 && o.clsid.empty(), "default is read-only inspection");
    check(parse({"--clsid", id}, o) && o.action == AsioProbeAction::Inspect && o.clsid == id, "CLSID alone never requests an active test");
    check(parse({"--dsd", "--clsid", id}, o) && o.action == AsioProbeAction::Inspect && o.withDsd, "DSD capability query remains read-only");
    for (auto action : {AsioProbeAction::PcmCheck, AsioProbeAction::PcmStream, AsioProbeAction::DsdCreate}) {
        const char* flag = action == AsioProbeAction::PcmCheck ? "--check" : action == AsioProbeAction::PcmStream ? "--stream" : "--dsd-create";
        check(parse({flag, "--clsid", id}, o) && o.action == action && o.clsid == id && o.deviceIndex == -1, "CLSID after action preserves exactly the chosen action");
        check(parse({"--clsid", id, flag}, o) && o.action == action && o.clsid == id, "CLSID before action preserves exactly the chosen action");
        check(parse({flag, "3"}, o) && o.action == action && o.deviceIndex == 3 && o.clsid.empty(), "numeric selector preserves exactly the chosen action");
        check(!parse({flag}, o), "active action without device is rejected");
        check(!parse({flag, "3", "--clsid", id}, o), "ambiguous index and CLSID are rejected");
        check(!parse({flag, "-1"}, o), "negative index cannot trigger a fallback probe");
    }
    check(parse({"--stream", "1", "--rate", "96000", "--ms", "700"}, o) && o.action == AsioProbeAction::PcmStream && o.deviceIndex == 1 && o.rate == 96000 && o.milliseconds == 700, "existing backend preflight arguments remain PCM");
    check(parse({"--dsd-create", "--clsid", id, "--rate", "45158400"}, o) && o.dsdRate == 45158400, "documented --rate alias selects DSD1024, never DSD64");
    check(parse({"--rate", "45158400", "--dsd-create", "1", "--dsd-rate", "45158400"}, o) && o.dsdRate == 45158400, "equal DSD rate aliases work in either order");
    check(!parse({"--dsd-create", "1", "--rate", "45158400", "--dsd-rate", "2822400"}, o), "conflicting DSD rate aliases fail explicitly");
    check(parse({"--check", "--clsid", "01234567-89ab-cdef-0123-456789abcdef", "--rate", "48000.0"}, o) && o.clsid.front() == '{' && o.rate == 48000, "canonical GUID normalization and decimal rate work");
    for (const char* flag : {"--clsid", "--rate", "--dsd-rate", "--ch", "--ms", "--buf", "--bytes"}) {
        check(!parse({"--dsd-create", "1", flag}, o), "missing value rejected before any device access");
        check(!parse({"--dsd-create", "1", flag, "--nop"}, o), "another switch cannot be consumed as a value");
    }
    for (const char* value : {"", "bad", "NaN", "Inf", "-1", "0", "1e309", "100000001", "48000Hz", "0x100", " 48000"})
        check(!parse({"--stream", "0", "--rate", value}, o), "non-finite, malformed and out-of-range rates rejected");
    for (const char* value : {"", "1x", "-1", "1.5", "2147483648"})
        check(!parse({"--stream", value}, o), "device index must be a complete bounded integer");
    for (const char* value : {"", "bad", "{01234567-89AB-CDEF-0123-456789ABCDEG}", "{01234567_89AB-CDEF-0123-456789ABCDEF}"})
        check(!parse({"--clsid", value}, o), "malformed CLSID rejected");
    check(!parse({"--check", "--stream", "1"}, o), "conflicting PCM actions rejected");
    check(!parse({"--stream", "1", "--dsd-create", "1"}, o), "PCM and DSD actions cannot coexist");
    check(!parse({"--check", "1", "--dsd"}, o), "read-only DSD flag cannot alter a PCM check");
    check(!parse({"--stream", "1", "--dsd-rate", "2822400"}, o), "DSD rate cannot silently alter a PCM stream");
    check(!parse({"--check", "1", "--rate-first"}, o), "DSD flags rejected on PCM check");
    check(!parse({"--clsid", id, "--rate", "48000"}, o), "read-only request cannot carry ignored write parameters");
    check(!parse({"--stream", "1", "--rate", "48000", "--rate", "96000"}, o), "repeated values do not silently override each other");
    check(!parse({"--stream", "1", "--ms", "49"}, o) && !parse({"--stream", "1", "--ms", "5001"}, o), "duration is validated instead of silently clamped");
    check(parse({"--stream", "1", "--ms", "50", "--ch", "1"}, o) && parse({"--stream", "1", "--ms", "5000", "--ch", "32"}, o), "documented duration and channel boundaries are accepted");
    check(!parse({"--stream", "1", "--ch", "0"}, o) && !parse({"--stream", "1", "--ch", "33"}, o), "channel limits rejected before driver initialization");
    check(!parse({"--dsd-create", "1", "--allch", "--ch", "2"}, o), "contradictory channel selection rejected");
    check(!parse({"--dsd-create", "1", "--buf", "7"}, o), "DSD buffer alignment checked before mode switch");
    check(!parse({"--dsd-create", "1", "--probe-buf"}, o), "unsafe buffer experiment rejected before driver initialization");
    check(!parse({"--unknown"}, o) && !parse({"2"}, o), "unknown option and stray positional value rejected");
    check(parse({"--help"}, o) && o.help && !parse({"--help", "--stream", "1"}, o), "help is an isolated non-device action");
    std::printf("checks=%d failures=%d actual_audio_device_calls=0\n", checks, failures);
    return failures ? 1 : 0;
}
