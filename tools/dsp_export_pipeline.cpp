// Export the exact coefficient objects prepared by the real PCM Pipeline.
// No device, no playback, no reconstructed autoTaps implementation.
#include "engine/pipeline.h"
#include <charconv>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs=std::filesystem;
using rta::dsp::PolyphaseResampler;
uint32_t rate(const char* text) {
    uint32_t result=0;
    const std::string s=text;
    auto [end,ec]=std::from_chars(s.data(),s.data()+s.size(),result);
    if(ec!=std::errc{}||end!=s.data()+s.size()||!result)
        throw std::invalid_argument("Invalid positive integer sample rate");
    return result;
}
void leaf(const PolyphaseResampler& rs,const fs::path& dir,const char* name,std::ostream& json) {
    std::vector<double> h;rs.prototype_into(h);
    if(h.empty())throw std::runtime_error("Empty coefficient object");
    for(double v:h)if(!std::isfinite(v))throw std::runtime_error("Nonfinite coefficient object");
    const fs::path file=dir/(std::string(name)+".f64le");
    std::ofstream stream(file,std::ios::binary|std::ios::trunc);
    stream.write(reinterpret_cast<const char*>(h.data()),std::streamsize(h.size()*sizeof(double)));
    stream.flush();if(!stream)throw std::runtime_error("Coefficient output failed");
    const auto& c=rs.config();
    json<<"{\"file\":"<<std::quoted(file.filename().string())
        <<",\"count\":"<<h.size()<<",\"input_rate\":"<<c.inRate<<",\"output_rate\":"<<c.outRate
        <<",\"L\":"<<rs.upFactor()<<",\"M\":"<<rs.downFactor()
        <<",\"coefficient_rate\":"<<uint64_t(c.inRate)*rs.upFactor()
        <<",\"reference_gain\":"<<std::quoted(std::to_string(rs.upFactor()))
        <<",\"T\":"<<rs.tapsPerPhase()<<",\"transition\":"<<c.transition
        <<",\"high_precision_design\":"<<(c.highPrecisionDesign?"true":"false")
        <<",\"attenuation\":"<<c.attenDb<<",\"window\":"<<std::quoted(rta::dsp::window_name(c.window))
        <<",\"phase\":"<<std::quoted(rta::dsp::phase_name(c.phase))<<"}";
}
int main(int argc,char**argv) {
    try {
        if(argc<5)throw std::invalid_argument("Usage: dsp_export_pipeline <preset> <input-rate> <output-rate> <directory> [--single] [--taps-per-input N] [--atten dB]");
        const auto* preset=rta::dsp::find_preset(argv[1]);
        if(!preset)throw std::invalid_argument("Unknown preset");
        const uint32_t in=rate(argv[2]),out=rate(argv[3]);
        if(in==out)throw std::invalid_argument("Equal rates are bit-perfect bypass, not a filter design");
        const auto dir=fs::u8path(argv[4]);fs::create_directories(dir);
        rta::engine::PipelineConfig cfg;cfg.outRate=out;cfg.autoTaps=true;
        cfg.useFixed=false;cfg.gpuResample=false;
        rta::engine::apply_filter_preset(cfg,*preset);
        for(int i=5;i<argc;++i){
            const std::string option=argv[i];
            if(option=="--single"){cfg.allowAutomaticCascade=false;cfg.twoStage=false;}
            else if(option=="--taps-per-input"&&i+1<argc){
                const auto count=rate(argv[++i]);if(count>INT_MAX)throw std::invalid_argument("Tap override exceeds int range");
                cfg.minTapsPerInput=int(count);
            }else if(option=="--atten"&&i+1<argc){
                const std::string value=argv[++i];size_t used=0;cfg.attenDb=std::stod(value,&used);
                if(used!=value.size()||!std::isfinite(cfg.attenDb)||cfg.attenDb<0||cfg.attenDb>1000)throw std::invalid_argument("Invalid attenuation override");
            }else throw std::invalid_argument("Unknown or incomplete option");
        }
        rta::engine::Pipeline pipeline(cfg);
        rta::audio::AudioBuffer input;input.rate=in;input.bits=24;input.channels=1;input.isFloat=false;input.raw={0};
        std::vector<std::vector<double>> samples;pipeline.process(input,nullptr,&samples);
        const auto* rs=pipeline.prepared_resampler();
        if(!rs)throw std::runtime_error("Pipeline did not prepare a resampler");
        const auto bandRate=std::min(in,out);
        const auto metrics=rta::dsp::preset_metrics(*preset,bandRate,rta::dsp::content_hz_for_rate(bandRate));
        std::ostringstream result;result<<std::setprecision(17)
            <<"{\"schema\":1,\"object\":\"actual Pipeline prepared_resampler\",\"preset\":"<<std::quoted(preset->name)
            <<",\"input_rate\":"<<in<<",\"output_rate\":"<<out<<",\"requested_two_stage\":"<<(cfg.twoStage?"true":"false")
            <<",\"automatic_cascade\":"<<(cfg.allowAutomaticCascade?"true":"false")
            <<",\"requested_taps_per_input\":"<<cfg.minTapsPerInput<<",\"requested_attenuation\":"<<cfg.attenDb
            <<",\"effective_cascade\":"<<(rs->two_stage()?"true":"false")<<",\"pass_hz\":"<<metrics.passHz
            <<",\"stop_hz\":"<<double(bandRate)-metrics.passHz<<",\"stop_target_db\":"<<metrics.ceilingDb
            <<",\"phase_mix\":"<<rta::dsp::phase_mix()<<",\"latency_input_samples\":"<<rs->latencyInputSamples()
            <<",\"tail_input_samples\":"<<rs->tailInputSamples()<<",\"coefficient_count\":"<<rs->coefficient_count()
            <<",\"stages\":[";
        if(rs->two_stage()){leaf(*rs->first_stage(),dir,"stage1",result);result<<',';leaf(*rs->second_stage(),dir,"stage2",result);}
        else leaf(*rs,dir,"stage1",result);
        result<<"]}\n";
        std::ofstream manifest(dir/"pipeline.json",std::ios::binary|std::ios::trunc);
        manifest<<result.str();manifest.flush();if(!manifest)throw std::runtime_error("Manifest write failed");
        std::cout<<result.str();return 0;
    } catch(const std::exception& e) {std::cerr<<"ERROR "<<e.what()<<'\n';return 2;}
}
