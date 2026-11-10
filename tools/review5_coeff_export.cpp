#include "dsp_measure.h"
#include <fstream>
#include <filesystem>
#include <cstdio>
int main(int argc,char** argv){
 if(argc<2||argc>3)return 2;const std::string requested=argc==3?argv[2]:"poly-sinc-short-lp";_putenv_s("RLPLAYER_FILTER_CACHE","0");
 for(const auto& preset:rta::dsp::filter_presets())if(std::string(preset.name)==requested){
  dsp_measure::Result r;auto cfg=review2::production_config(preset,44100,88200);
  rta::dsp::PolyphaseResampler rs(cfg);std::vector<double> h;rs.prototype_into(h);
  r.L=rs.upFactor();r.M=rs.downFactor();r.fs=44100.*r.L;
  auto metric=rta::dsp::preset_metrics(preset,44100,20000);r.passHz=metric.passHz;r.stopHz=44100.-r.passHz;
  const std::string stem=requested.substr(std::string("poly-sinc-").size())+"-44100-88200";std::filesystem::path dir(argv[1]);std::filesystem::create_directories(dir);
  std::ofstream f(dir/(stem+".f64"),std::ios::binary);f.write((char*)h.data(),h.size()*8);f.close();
  std::ofstream j(dir/(stem+".spec.json"));j.precision(17);
  j<<"{\"identity\":\"44100/88200/"<<requested<<"\",\"reference_gain\":\""<<r.L<<"\",\"coefficients\":\""<<stem<<".f64\",\"sample_rate\":"<<r.fs<<",\"pass_hz\":"<<r.passHz<<",\"stop_hz\":"<<r.stopHz<<",\"stop_db\":-280,\"pass_ripple_db\":0.01,\"taps\":"<<h.size()<<",\"L\":"<<r.L<<",\"M\":"<<r.M<<"}";
  std::printf("taps=%zu fs=%.17g pass=%.17g stop=%.17g\n",h.size(),r.fs,r.passHz,r.stopHz);return (!f||!j)?1:0;
 }return 3;
}
