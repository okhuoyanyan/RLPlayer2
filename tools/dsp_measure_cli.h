#pragma once
#include "dsp_measure.h"
#include <charconv>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
namespace dsp_measure {
struct Options {
  std::string preset, output="filter_sweep_audit.tsv", coefficients, points, baseline;
  std::string topology="preset";
  std::string manifest;
  unsigned in=0,out=0,factor=16,offset=0,limit=0;
  bool structure=false,apod=false,inject=false,list=false,help=false,manual=false;
  bool inSeen=false,outSeen=false;
};
inline unsigned uint_arg(const std::string& value) {
  unsigned n=0;auto v=std::from_chars(value.data(),value.data()+value.size(),n);
  if(v.ec!=std::errc()||v.ptr!=value.data()+value.size()) throw std::runtime_error("invalid unsigned integer: "+value);
  return n;
}
inline Options parse(int argc,char**argv,bool apod=false) {
  Options o;o.apod=apod;if(apod)o.output="apod_check_audit.tsv";
  auto val=[&](int& i){if(++i>=argc||std::string(argv[i]).starts_with("--"))throw std::runtime_error("missing option value");return std::string(argv[i]);};
  for(int i=1;i<argc;++i){std::string a=argv[i];
    if(a=="--preset")o.preset=val(i);
    else if(a=="--two-stage"){o.topology=val(i);if(o.topology!="preset"&&o.topology!="on"&&o.topology!="off")throw std::runtime_error("--two-stage must be preset, on or off");}
    else if(a=="--manifest")o.manifest=val(i);
    else if(a=="--in-rate"){o.in=uint_arg(val(i));o.inSeen=true;}
    else if(a=="--out-rate"){o.out=uint_arg(val(i));o.outSeen=true;}
    else if(a=="--output")o.output=val(i);
    else if(a=="--coefficients")o.coefficients=val(i);
    else if(a=="--points")o.points=val(i);
    else if(a=="--configuration") {auto mode=val(i);if(mode!="production"&&mode!="manual")throw std::runtime_error("configuration must be production or manual");o.manual=mode=="manual";}
    else if(a=="--fft-factor")o.factor=uint_arg(val(i));
    else if(a=="--offset")o.offset=uint_arg(val(i));
    else if(a=="--limit")o.limit=uint_arg(val(i));
    else if(a=="--regression-baseline")o.baseline=val(i);
    else if(a=="--structure-only")o.structure=true;
    else if(a=="--apod-only")o.apod=true;
    else if(a=="--inject-regression")o.inject=true;
    else if(a=="--list")o.list=true;
    else if(a=="--help")o.help=true;
    else throw std::runtime_error("unknown option or positional argument: "+a);
  }
  if(o.inSeen!=o.outSeen||(o.inSeen&&(!o.in||!o.out)))throw std::runtime_error("--in-rate and --out-rate must be paired and positive");
  if(o.in && (o.out<=o.in||o.in>1536000||o.out>98304000))throw std::runtime_error("require 0 < input < output <= 98304000 Hz");
  if(o.in && o.out/std::gcd(o.in,o.out)>(1u<<19))throw std::runtime_error("rational interpolation factor exceeds safe prototype capacity");
  if(o.factor<16||o.factor>64)throw std::runtime_error("--fft-factor must be 16..64");
  if(!o.preset.empty()&&!rta::dsp::find_preset(o.preset))throw std::runtime_error("unknown exact preset name: "+o.preset);
  if(!o.points.empty()&&(o.in==0||o.preset.empty()))throw std::runtime_error("--points requires one exact --preset and paired rates");
  if(o.structure&&!o.points.empty())throw std::runtime_error("--points conflicts with --structure-only");
  if(o.manual&&o.points.empty())throw std::runtime_error("manual configuration is calibration-only: requires --points");
  if(o.output.empty())throw std::runtime_error("empty --output");
  if(!o.manifest.empty()&&(!o.points.empty()||o.list))throw std::runtime_error("--manifest requires quality/structure rows");
  return o;
}
inline std::vector<double> frequencies(const std::string& s,double fs) {
  std::vector<double> out;std::istringstream ss(s);std::string part;
  while(std::getline(ss,part,',')){size_t n=0;double hz=std::stod(part,&n);if(n!=part.size()||!std::isfinite(hz)||hz<0||hz>fs/2)throw std::runtime_error("invalid frequency Hz: "+part);out.push_back(hz);}
  if(out.empty()||s.back()==',')throw std::runtime_error("empty frequency list item");return out;
}
inline std::string identity(unsigned in,unsigned out,const char* p){return std::to_string(in)+"/"+std::to_string(out)+"/"+p;}
inline int run(int argc,char**argv,bool apod=false) {
 try {
  const auto start=std::chrono::steady_clock::now();
  const auto o=parse(argc,argv,apod);
  if(o.help){puts("Usage: filter_sweep [--preset EXACT_NAME --in-rate HZ --out-rate HZ] [--output FILE.tsv] [--fft-factor 16..64] [--offset N --limit N] [--apod-only] [--structure-only] [--list] [--coefficients DIR] [--points HZ,HZ,...] [--configuration production|manual] [--two-stage preset|on|off] [--manifest FILE] [--regression-baseline FILE] [--inject-regression]\nDefaults: all 395 production identities; strict stated targets (no dB allowance). Manual configuration is allowed only with --points for explicitly labelled calibration. --points is a direct finite-point calibration/segmentation mode. --max-fails is rejected. Exit: 0 sampled targets met, 1 sampled target failed, 2 structural/argument error, 3 unresolved. Regression mode may return 0 with KNOWN failures; NEVER a quality certificate.");return 0;}
  setvbuf(stdout,nullptr,_IONBF,0);
  std::vector<std::pair<unsigned,unsigned>> groups={{44100,705600},{44100,1411200},{48000,768000},{48000,1536000},{96000,384000}};
  if(o.in)groups={{o.in,o.out}};
  std::filesystem::path path=std::filesystem::u8path(o.output);
  if(!o.list&&path.has_parent_path())std::filesystem::create_directories(path.parent_path());
  std::ofstream os;if(!o.list){os.open(path);if(!os)throw std::runtime_error("cannot open --output");}
  std::ofstream manifest;if(!o.manifest.empty()){auto mp=std::filesystem::u8path(o.manifest);if(mp.has_parent_path())std::filesystem::create_directories(mp.parent_path());manifest.open(mp);if(!manifest)throw std::runtime_error("cannot open --manifest");manifest<<"# requested_topology="<<o.topology<<"; finite-grid identities/statuses; not continuous-band proof\n";}
  os<<std::setprecision(17);
  os<<"# configuration="<<(o.manual?"MANUAL_CALIBRATION_NOT_PRODUCTION":"PRODUCTION_AUTO_TAPS")<<"; requested_topology="<<o.topology<<"; finite-grid DD16+endpoint+17-point refinement; NOT a continuous-band proof.\n";
  if(o.points.empty())os<<"identity\tin_rate\tout_rate\tpreset\tT\ttaps\tL\tM\tprototype_rate\tfft_n\tstop_bins\tstop_hz\tstop_db\tpass_hz\tpass_db\ttarget\tkind\tstatus\tdc_db\tstage2_T\ttopology\trequested_topology\tcoefficient_file\n";
  else os<<"identity\tin_rate\tout_rate\tpreset\tT\ttaps\tL\tM\tprototype_rate\tfrequency_hz\tdd_db\texact_db\tdelta_exact_minus_dd\ttopology\trequested_topology\tcoefficient_file\n";
  std::set<std::string> actual,failures;unsigned selected=0,measured=0,quality=0,excluded=0,errors=0,unresolved=0;
  for(auto [in,out]:groups)for(const auto&p:rta::dsp::filter_presets()) {
    if((o.apod&&!p.apodizing)||(!o.preset.empty()&&o.preset!=p.name))continue;
    if(selected++<o.offset)continue;
    if(o.limit&&measured>=o.limit)continue;
    ++measured;auto id=identity(in,out,p.name);if(o.topology!="preset")id+="/topology="+o.topology;actual.insert(id);
    if(o.list){puts(id.c_str());continue;}
    try {
      Result r;auto h=prototype(p,in,out,r,o.manual,o.topology=="preset"?-1:o.topology=="on"?1:0);
      if(o.inject){h.at(0)+=1e-3;h.at(1)-=1e-3;r.finite=true;}
      auto coeffile=std::to_string(in)+"-"+std::to_string(out)+"-"+p.name+(o.topology=="preset"?"":"-topology-"+o.topology)+".f64";
      if(!o.coefficients.empty()) {auto bpath=std::filesystem::u8path(o.coefficients)/coeffile;std::filesystem::create_directories(bpath.parent_path());std::ofstream b(bpath,std::ios::binary);b.write((char*)h.data(),h.size()*sizeof(double));if(!b)throw std::runtime_error("cannot write coefficients");}
      if(!o.points.empty()) {
        for(double hz:frequencies(o.points,r.fs)){double dd=direct(h,hz,r.fs),exact=rta::dsp::magnitude_db_exact(h,2*hz/r.fs);os<<id<<'\t'<<in<<'\t'<<out<<'\t'<<p.name<<'\t'<<r.T<<'\t'<<r.taps<<'\t'<<r.L<<'\t'<<r.M<<'\t'<<r.fs<<'\t'<<hz<<'\t'<<dd<<'\t'<<exact<<'\t'<<exact-dd<<'\t'<<(r.cascade?"cascade":"single")<<'\t'<<o.topology<<'\t'<<coeffile<<'\n';}
        printf("POINTS %s T=%d N=%zu fs=%.0f\n",id.c_str(),r.T,h.size(),r.fs);
      } else {
        measure(h,r,o.factor,o.structure);if(!r.finite)++errors;
        if(r.excluded)++excluded;else if(r.stopFail||r.passFail){++quality;failures.insert(id);}else if(r.uncertain)++unresolved;
        os<<id<<'\t'<<in<<'\t'<<out<<'\t'<<p.name<<'\t'<<r.T<<'\t'<<r.taps<<'\t'<<r.L<<'\t'<<r.M<<'\t'<<r.fs<<'\t'<<r.fftN<<'\t'<<r.stopBins<<'\t'<<r.stopHz<<'\t'<<r.stopDb<<'\t'<<r.passWorstHz<<'\t'<<r.passDb<<'\t'<<r.target<<'\t'<<r.kind<<'\t'<<r.status<<'\t'<<r.dcDb<<'\t'<<r.stage2T<<'\t'<<(r.cascade?"cascade":"single")<<'\t'<<o.topology<<'\t'<<coeffile<<'\n';
        printf("%s T=%d N=%zu stop=%.12f Hz=%.12f pass=%.12f %s %s\n",id.c_str(),r.T,h.size(),r.stopDb,r.stopHz,r.passDb,r.kind.c_str(),r.status.c_str());
        if(manifest.is_open()){manifest<<id<<'/'<<r.status<<'\n';manifest.flush();}
      }
      os.flush();if(!os)throw std::runtime_error("report write failed");
    }catch(const std::exception&e){++errors;fprintf(stderr,"STRUCTURE_FAIL %s: %s\n",id.c_str(),e.what());}
  }
  if(!measured)throw std::runtime_error("zero selected identities");
  printf("RESULT measured=%u sampled_fail=%u excluded=%u structural_fail=%u unresolved=%u elapsed_s=%.3f\n",measured,quality,excluded,errors,unresolved,std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count());
  if(errors)return 2;
  if(o.structure)puts("STRUCTURE_ONLY; QUALITY NOT CERTIFIED");
  if(!o.points.empty())puts("POINTS_ONLY; requested finite points evaluated, QUALITY NOT CERTIFIED");
  if(!o.baseline.empty()) {
    if(o.structure||!o.points.empty()||o.list)throw std::runtime_error("baseline requires measured quality rows");
    std::ifstream b(std::filesystem::u8path(o.baseline));if(!b)throw std::runtime_error("cannot read baseline");
    std::set<std::string> universe,known;std::string s;
    while(std::getline(b,s)){if(s.empty()||s[0]=='#')continue;if(s.back()=='\r')s.pop_back();auto pos=s.rfind('/');if(pos==std::string::npos)throw std::runtime_error("malformed baseline identity");std::string id=s.substr(0,pos),status=s.substr(pos+1);universe.insert(id);if(status=="CONFIRMED_SAMPLED_FAIL")known.insert(id);else if(status!="DENSE_SAMPLED_TARGET_MET"&&status!="EXCLUDED")throw std::runtime_error("unresolved/invalid baseline status");}
    unsigned regressions=0;for(auto&id:actual)if(!universe.contains(id))++regressions;
    for(auto&id:failures)if(!known.contains(id)){++regressions;printf("NEW_FAILURE %s\n",id.c_str());}
    if(!o.in&&o.preset.empty()&&!o.apod&&!o.limit&&!o.offset&&actual!=universe)++regressions;
    printf("REGRESSION_ONLY known_failures=%zu current_failures=%zu regressions=%u; QUALITY NOT CERTIFIED\n",known.size(),failures.size(),regressions);return regressions?1:(unresolved?3:0);
  }
  return quality?1:(unresolved?3:0);
 }catch(const std::exception&e){fprintf(stderr,"ERROR: %s\n",e.what());return 2;}
}
}
