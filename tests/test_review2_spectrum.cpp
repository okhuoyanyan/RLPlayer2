#include "../tools/dsp_measure_cli.h"
#include <cstdio>
int main() {
  int fails=0;
  auto ck=[&](bool b,const char*n){printf("%s %s\n",b?"PASS":"FAIL",n);fails+=!b;};
  using namespace rta::dsp;
  const auto*p=find_preset("poly-sinc-short-lp");
  dsp_measure::Result r;auto h=dsp_measure::prototype(*p,44100,705600,r);
  dsp_measure::measure(h,r);
  ck(r.finite&&r.T>=211&&r.fftN>=16*h.size(),"real-production-DD16-path");
  ck(r.stopDb<-280&&std::abs(r.passDb)<=.01,"short-lp-strict-sampled-targets");
  h[0]+=.001;h[1]-=.001;
  dsp_measure::Result broken;dsp_measure::prototype(*p,44100,705600,broken);
  dsp_measure::measure(h,broken);
  ck(broken.finite&&broken.stopFail&&broken.status=="CONFIRMED_SAMPLED_FAIL", "coefficient-degradation-detected-in-actual-measurement-entry");
  auto cfg=review2::production_config(*p,44100,192000);PolyphaseResampler rs(cfg);
  ck(review2::prototype_rate(rs,44100,192000)==28224000.,"noninteger-prototype-domain");
  ck(review2::quality_exit(0,1,999)==3,"unresolved-cannot-be-overridden-by-fail-count");
  ck(review2::quality_exit(5,0,999)==1,"known-failure-cannot-be-certified-by-count");
  ck(!review2::identity_gate({"44100/705600/long-mp"},{"48000/768000/long-ip"}),"same-count-different-identities-rejected");
  ck(!std::isfinite(magnitude_db_exact({},.5)),"exact-rejects-empty");
  ck(!std::isfinite(magnitude_db_exact({1},NAN)),"exact-rejects-nonfinite-frequency");
  ck(!std::isfinite(magnitude_db_exact({NAN},.5)),"exact-rejects-nonfinite-coefficient");
  ck(magnitude_db_exact({1},0)==0&&magnitude_db_exact({1},1)==0,"exact-dc-nyquist");
  ck(magnitude_db_exact({.2,.3,.3,.2},1)==-6000,"exact-even-symmetric-nyquist-analytic-zero-floor");
  ck(!std::isfinite(magnitude_db_exact({1e308,1e308},0)),"exact-rejects-accumulator-overflow");
  ck(std::abs(magnitude_db_exact({.5,.5},.5)+3.010299956639812)<1e-12,"exact-known-polynomial");
  const double last=1.0+1e-14;
  const double deepExpected=20*std::log10(last-1.0);
  ck(std::abs(magnitude_db_exact({1e-8,1,1e-8,last},.5)-deepExpected)<1e-11,
     "exact-280dB-cancellation-known-polynomial");
  ck(magnitude_db_exact({.5,.5},.25)==magnitude_db_exact({.5,.5},2.25),"exact-frequency-periodicity");
  return fails?1:0;
}
