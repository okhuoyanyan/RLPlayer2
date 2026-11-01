#include "review_response.h"
#include "dsp/response_precision.h"
#include <complex>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <random>
#include <string>

using namespace rta::dsp;
namespace {
class CheckedOutput {
public:
    explicit CheckedOutput(const std::filesystem::path& path,
                           std::ios::openmode mode = std::ios::out) {
        stream.exceptions(std::ios::failbit | std::ios::badbit);
        stream.open(path, mode | std::ios::out | std::ios::trunc);
    }
    void finish() {
        stream.flush();
        stream.close();
    }
    std::ofstream stream;
};

void write_binary(const std::filesystem::path& path, const std::vector<double>& values) {
    if (values.size() > size_t(std::numeric_limits<std::streamsize>::max()) / sizeof(double))
        throw std::length_error("Binary export exceeds stream size limit");
    CheckedOutput file(path, std::ios::binary);
    file.stream.write(reinterpret_cast<const char*>(values.data()),
                      std::streamsize(values.size() * sizeof(double)));
    file.finish();
}

size_t parse_block_frames(int argc, char** argv) {
    if (argc == 2) return 257;
    if (argc != 4 || std::string(argv[2]) != "--block-frames")
        throw std::invalid_argument("Usage: filter_numerical_audit OUTDIR [--block-frames N]");
    size_t used = 0;
    const auto frames = std::stoull(argv[3], &used);
    if (used != std::string(argv[3]).size() || frames == 0 || frames > 1048576)
        throw std::invalid_argument("Block frames must be 1..1048576");
    return size_t(frames);
}

size_t export_stream(const ResamplerConfig& config, const std::vector<double>& input,
                     const std::filesystem::path& directory, int id, int mode, int amplitude,
                     size_t block_frames, std::ostream& execution) {
    auto cfg = config;
    cfg.cpuKernel = mode ? ResamplerCpuKernel::Scalar : ResamplerCpuKernel::Auto;
    PolyphaseResampler stream(cfg);
    auto x = input;
    const int exponent = amplitude == 0 ? 0 : amplitude == 1 ? -4 : -20;
    for (auto& value : x) value *= std::ldexp(1.0, exponent);
    std::vector<double> output;
    size_t position = 0, simd_outputs = 0, max_threads = 0;
    while (position < x.size()) {
        const size_t frames = std::min(block_frames, x.size() - position);
        std::vector<double> block(stream.maxOutFrames(frames));
        const double* source[] = {x.data() + position};
        double* destination[] = {block.data()};
        const size_t produced = stream.process(source, frames, destination);
        simd_outputs += stream.cpu_simd_outputs_last_block();
        max_threads = std::max(max_threads, stream.cpu_parallel_threads_last_block());
        output.insert(output.end(), block.begin(), block.begin() + produced);
        position += frames;
    }
    execution << id << '\t' << (mode ? "scalar" : "auto") << '\t' << amplitude << '\t'
              << block_frames << '\t' << simd_outputs << '\t' << max_threads << '\n';
    write_binary(directory / (std::to_string(id) + "-m" + std::to_string(mode) +
                 "-y" + std::to_string(amplitude) + ".bin"), output);
    return output.size();
}

void export_case(const std::filesystem::path& directory, int id, size_t block_frames,
                 std::ostream& metadata, std::ostream& frequencies, std::ostream& execution) {
    const char* name = id == 1 ? "poly-sinc-long-lp" : "poly-sinc-short-lp";
    const unsigned in_rate = 44100, out_rate = id == 2 ? 48000 : 176400;
    const auto* preset = find_preset(name);
    if (!preset) throw std::runtime_error("Required audit preset missing");
    auto cfg = review2::production_config(*preset, in_rate, out_rate);
    cfg.cpuParallel = false;
    PolyphaseResampler reference(cfg);
    std::vector<double> coefficients;
    reference.prototype_into(coefficients);
    write_binary(directory / (std::to_string(id) + "-h.bin"), coefficients);

    const size_t frames = std::max<size_t>(2048, reference.tapsPerPhase() * 2 + 128);
    std::vector<double> input(frames + reference.tapsPerPhase() + 2);
    std::mt19937 generator(2802026);
    for (size_t i = 0; i < frames; ++i)
        input[i] = (int32_t(generator() >> 8) - 8388608) / 8388608.0;
    write_binary(directory / (std::to_string(id) + "-x.bin"), input);
    size_t output_count = 0;
    for (int mode = 0; mode < 2; ++mode)
        for (int amplitude = 0; amplitude < 3; ++amplitude)
            output_count = export_stream(cfg, input, directory, id, mode, amplitude,
                                         block_frames, execution);

    const auto metrics = preset_metrics(*preset, in_rate, 20000);
    metadata << id << '\t' << name << '\t' << in_rate << '\t' << out_rate << '\t'
             << reference.upFactor() << '\t' << reference.downFactor() << '\t'
             << reference.tapsPerPhase() << '\t' << input.size() << '\t' << output_count << '\t'
             << std::numeric_limits<long double>::digits << '\t' << metrics.passHz << '\t'
             << in_rate - metrics.passHz << '\n';
    for (double hz : {0.0, metrics.passHz, std::nextafter(metrics.passHz, 0.0),
                     in_rate - metrics.passHz,
                     std::nextafter(in_rate - metrics.passHz, double(in_rate)), in_rate * .73}) {
        const double frequency = hz / (in_rate * reference.upFactor());
        const auto rotation = std::polar(1.0, -2 * std::acos(-1.0) * frequency);
        std::complex<double> sum{}, power{1, 0};
        auto normalized = coefficients;
        for (auto& value : normalized) {
            value /= reference.upFactor();
            sum += value * power;
            power *= rotation;
        }
        frequencies << id << '\t' << hz << '\t' << 20 * std::log10(std::abs(sum)) << '\t'
                    << response_precision::magnitude_db(normalized, 2 * frequency) << '\n';
    }
}
} // namespace

int main(int argc, char** argv) {
    std::filesystem::path completion_marker;
    try {
        const size_t block_frames = parse_block_frames(argc, argv);
        const std::filesystem::path directory(argv[1]);
        std::filesystem::create_directories(directory);
        completion_marker = directory / "export-complete.json";
        std::filesystem::remove(completion_marker);
        CheckedOutput metadata(directory / "cases.tsv");
        CheckedOutput frequencies(directory / "frequencies.tsv");
        CheckedOutput execution(directory / "execution.tsv");
        metadata.stream << "id\tpreset\tin\tout\tL\tM\tT\tinput_count\toutput_count\tlong_double_digits\tpass_hz\tstop_hz\n";
        frequencies.stream << std::setprecision(17) << "id\thz\tdouble_db\tdd_db\n";
        execution.stream << "id\tkernel\tamplitude_index\tblock_frames\tsimd_outputs\tmax_threads\n";
        for (int id = 0; id < 3; ++id)
            export_case(directory, id, block_frames, metadata.stream, frequencies.stream, execution.stream);
        metadata.finish();
        frequencies.finish();
        execution.finish();
        CheckedOutput completed(completion_marker);
        completed.stream << "{\"complete\":true,\"case_count\":3,\"block_frames\":"
                         << block_frames << "}\n";
        completed.finish();
        return 0;
    } catch (const std::exception& error) {
        if (!completion_marker.empty()) {
            std::error_code ignored;
            std::filesystem::remove(completion_marker, ignored);
        }
        std::cerr << "Audit export failed: " << error.what() << '\n';
        return 2;
    }
}
