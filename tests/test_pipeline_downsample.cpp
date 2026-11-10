#include "engine/pipeline.h"
#include <cstdio>
#include <cmath>
#include <vector>
using namespace rta::dsp;
int main(){int failures=0,checks=0;
    for(auto rates:{std::pair{96000u,44100u},{192000u,48000u},{48000u,44100u}}){
        rta::engine::PipelineConfig cfg;cfg.outRate=rates.second;cfg.autoTaps=true;cfg.useFixed=false;cfg.gpuResample=false;
        rta::engine::apply_filter_preset(cfg,*find_preset("poly-sinc-short-lp"));
        rta::engine::Pipeline pipeline(cfg);rta::audio::AudioBuffer in;
        in.rate=rates.first;in.bits=24;in.channels=1;in.isFloat=false;in.raw={0};
        std::vector<std::vector<double>> signal;pipeline.process(in,nullptr,&signal);
        const auto* rs=pipeline.prepared_resampler();if(!rs||rs->two_stage())return 2;
        std::vector<double> h;rs->prototype_into(h);
        const double fs=double(in.rate)*rs->upFactor(),gainDb=20*std::log10(double(rs->upFactor()));
        const double passHz=.45*rates.second,stopHz=.55*rates.second;
        const double pass=magnitude_db_exact(h,2*passHz/fs)-gainDb,stop=magnitude_db_exact(h,2*stopHz/fs)-gainDb;
        const auto check=[&](bool ok,const char* label){++checks;if(!ok)++failures;std::printf("%s %s\n",ok?"PASS":"FAIL",label);};
        std::printf("rate=%u/%u T=%d coeff=%zu pass_hz=%.17g pass_db=%.17g stop_hz=%.17g stop_db=%.17g\n",rates.first,rates.second,rs->tapsPerPhase(),h.size(),passHz,pass,stopHz,stop);
        check(std::isfinite(pass)&&std::abs(pass)<=.01,"actual Pipeline preserves the output-domain pass edge");
        check(std::isfinite(stop)&&stop<=-280.,"actual Pipeline rejects alias at the fixed stop edge");
        check(rs->tapsPerPhase()>=int(std::ceil(211.*rates.first/rates.second)),"auto tap budget accounts for the rational decimation factor");
    }
    // Edge witnesses reproduce this bug; separate interval certificates prove
    // the whole pass/stop bands. Do not promote this test to a full certificate.
    std::printf("checks=%d failures=%d\n",checks,failures);return failures?1:0;
}
