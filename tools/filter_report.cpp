// Production configuration inventory, not a separate quality-measurement engine.
// Spectral gates live in filter_sweep; this report reuses its exact prototype path.
#include "dsp_measure_cli.h"
#include <iostream>

namespace {
struct Row {
    std::string name, window, phase, coefficientFile;
    dsp_measure::Result actual;
    rta::dsp::ResamplerConfig config;
    int presetTapBudget = 0;
    bool forNx = false;
};
std::string csv_string(const std::string& value) {
    std::string result = "\"";
    for (char c : value) { result += c; if (c == '"') result += c; }
    return result + '"';
}
std::string quality_command(const Row& row, unsigned in, unsigned out) {
    return "filter_sweep --preset " + row.name + " --in-rate " + std::to_string(in) +
           " --out-rate " + std::to_string(out) + " --output filter_sweep_audit.tsv";
}
Row evaluate(const rta::dsp::FilterPreset& preset, unsigned in, unsigned out,
             const std::string& coefficientDirectory) {
    Row row;
    row.name = preset.name;
    row.window = rta::dsp::window_name(preset.window);
    row.phase = rta::dsp::phase_name(preset.phase);
    row.presetTapBudget = preset.taps;
    row.forNx = preset.forNx;
    row.config = review2::production_config(preset, in, out);
    // Includes apply_preset, source autoTaps, apod F1/F2, full-support phase
    // construction and the actual cascade impulse, exactly as filter_sweep.
    auto coefficients = dsp_measure::prototype(preset, in, out, row.actual);
    dsp_measure::measure(coefficients, row.actual, 16, true);
    if (!row.actual.finite || row.actual.status != "STRUCTURE_ONLY")
        throw std::runtime_error("production prototype structure/DC check failed: " + row.name);
    if (!coefficientDirectory.empty()) {
        const auto filename = std::to_string(in) + "-" + std::to_string(out) + "-" + row.name + ".f64";
        const auto path = std::filesystem::u8path(coefficientDirectory) / std::filesystem::u8path(filename);
        std::filesystem::create_directories(path.parent_path());
        std::ofstream file(path, std::ios::binary);
        file.write(reinterpret_cast<const char*>(coefficients.data()),
                   static_cast<std::streamsize>(coefficients.size() * sizeof(double)));
        file.flush();
        if (!file) throw std::runtime_error("cannot flush production coefficients: " + path.string());
        file.close();
        if (!file) throw std::runtime_error("cannot close production coefficients: " + path.string());
        row.coefficientFile = filename;
    }
    return row;
}
std::string csv_report(const std::vector<Row>& rows, unsigned in, unsigned out) {
    std::ostringstream report;
    report << std::setprecision(17);
    report << "name,in_rate,out_rate,configuration,status,quality,prototype_samples,T,stage2_T,L,M,prototype_rate,window,configured_phase,topology,forNx,preset_tap_budget,transition,design_atten_db,target_atten_db,apodizing,apod_f1_hz,apod_f2_hz,pass_edge_hz,stop_edge_hz,dc_db,coefficient_file,quality_command\n";
    for (const auto& row : rows) {
        const auto& r = row.actual;
        const auto& c = row.config;
        report << csv_string(row.name) << ',' << in << ',' << out
               << ",PRODUCTION_AUTO_TAPS,STRUCTURE_ONLY,NOT_CERTIFIED,"
               << r.taps << ',' << r.T << ',' << r.stage2T << ',' << r.L << ',' << r.M << ',' << r.fs << ','
               << csv_string(row.window) << ',' << csv_string(row.phase) << ','
               << (r.cascade ? "cascade" : "single") << ',' << (row.forNx ? "true" : "false") << ','
               << row.presetTapBudget << ',' << c.transition << ',' << c.attenDb << ',' << r.target << ','
               << (c.apodizing ? "true" : "false") << ',' << c.apodF1 * in * .5 << ',' << c.apodF2 * in * .5 << ','
               << r.passHz << ',' << r.stopHz << ',' << r.dcDb << ',' << csv_string(row.coefficientFile) << ','
               << csv_string(quality_command(row, in, out)) << '\n';
    }
    return report.str();
}
std::string markdown_report(const std::vector<Row>& rows, unsigned in, unsigned out) {
    std::ostringstream report;
    report << "# RLPlayer 生产滤波器配置与实际原型报告\n\n"
           << "STRUCTURE_ONLY / QUALITY NOT CERTIFIED：本报告只确认生产参数、实际系数长度与有限性/DC，未做频响、镜像或振铃验收。\n\n"
           << "源率 " << in << " Hz → 输出 " << out << " Hz；配置为 PRODUCTION_AUTO_TAPS，与 filter_sweep 共用生产原型入口。\n\n"
           << "| 预设 | 实际原型样点 | 第一级 T | 第二级 T | 实际拓扑 | 窗 | 配置相位 | 切趾 F1/F2 (Hz) | 设计衰减 (dB) | 验收目标 (dB) |\n"
           << "|---|---:|---:|---:|---|---|---|---|---:|---:|\n";
    for (const auto& row : rows) {
        report << "| " << row.name << " | " << row.actual.taps << " | " << row.actual.T << " | " << row.actual.stage2T
               << " | " << (row.actual.cascade ? "cascade" : "single") << " | " << row.window << " | " << row.phase
               << " | " << row.config.apodF1 * in * .5 << " / " << row.config.apodF2 * in * .5
               << " | " << row.config.attenDb << " | " << row.actual.target << " |\n";
    }
    report << "\n实际原型样点：单级为运行时相位表还原的原型长度；级联为真实 process() 冲激响应长度。T 是实际每相抽头数，预设历史 taps 预算不再冒充实际长度。\n\n"
           << "配置相位描述所选生产构造，未单独测量相位响应。设计衰减和验收目标均为配置值，不能当作已达到的实测阻带。\n\n"
           << "严格质量需运行下列同一身份的 filter_sweep；该工具报告有限网格结果与失败，不代表连续全频段证明。\n\n";
    for (const auto& row : rows) report << "- `" << quality_command(row, in, out) << "`\n";
    return report.str();
}
} // namespace

int main(int argc, char** argv) {
    try {
        bool csv = false;
        std::string output;
        std::vector<std::string> arguments{argv[0], "--structure-only"};
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--csv") csv = true;
            else if (arg == "--markdown") csv = false;
            else if (arg == "--help" || arg == "-h") {
                std::cout << "Usage: filter_report [--markdown | --csv] [--out FILE] [--preset EXACT_NAME] [--in-rate HZ --out-rate HZ] [--coefficients DIR]\n"
                             "Defaults: all presets at 44100 -> 705600 Hz, production autoTaps. STRUCTURE_ONLY; QUALITY NOT CERTIFIED.\n"
                             "Reports actual production prototype/phase configuration only. For calibrated spectral quality use filter_sweep.\n";
                return 0;
            } else if (arg == "--out" || arg == "--preset" || arg == "--in-rate" || arg == "--out-rate" || arg == "--coefficients") {
                if (++i >= argc || std::string(argv[i]).starts_with("--"))
                    throw std::runtime_error("missing value for " + arg);
                if (arg == "--out") output = argv[i];
                arguments.push_back(arg == "--out" ? "--output" : arg);
                arguments.emplace_back(argv[i]);
            } else throw std::runtime_error("unknown option: " + arg);
        }
        std::vector<char*> pointers;
        for (auto& argument : arguments) pointers.push_back(argument.data());
        auto options = dsp_measure::parse(static_cast<int>(pointers.size()), pointers.data());
        if (!options.in) { options.in = 44100; options.out = 705600; }
        std::vector<Row> rows;
        for (const auto& preset : rta::dsp::filter_presets())
            if (options.preset.empty() || options.preset == preset.name)
                rows.push_back(evaluate(preset, options.in, options.out, options.coefficients));
        if (rows.empty()) throw std::runtime_error("zero selected presets");
        const auto report = csv ? csv_report(rows, options.in, options.out) : markdown_report(rows, options.in, options.out);
        if (!output.empty()) {
            std::ofstream file(std::filesystem::u8path(output));
            file << report;
            file.flush();
            if (!file) throw std::runtime_error("cannot flush report: " + output);
            file.close();
            if (!file) throw std::runtime_error("cannot close report: " + output);
        }
        std::cout << report;
        std::cout.flush();
        if (!std::cout) throw std::runtime_error("cannot write report to stdout");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "ERROR: " << error.what() << '\n';
        return 2;
    }
}
