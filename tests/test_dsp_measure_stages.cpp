#include "../tools/dsp_measure_stages.h"
#include <cstdio>
int main() {
  using namespace dsp_measure;
  auto p=rta::dsp::find_preset("poly-sinc-short-lp");
  auto s=production_stages(*p,44100,705600);
  auto impulse=actual_process_impulse(s);
  StageData actual=s;actual.first={1};actual.second=impulse;
  size_t stripe=1;while(stripe<16*((impulse.size()+s.f2-1)/s.f2))stripe*=2;
  const size_t n=stripe*s.f2;
  std::vector<DC> whole(n);
  for(size_t i=0;i<impulse.size();++i)whole[i].r.h=impulse[i];
  fft(whole);
  double error=0;size_t bins=0;
  measure_stages(actual,16,[&](size_t j,DC value){++bins;error=std::max(error,mag(sub(value,whole[j])));});
  printf("ACTUAL_PROCESS_FALLBACK bins=%zu expected=%zu max_absolute_error=%.17g fft=%zu\n",bins,n/2+1,error,n);
  auto bad=s;bad.first={1};bad.second=impulse;bad.second[0]+=.001;bad.second[1]-=.001;
  measure_stages(bad);
  printf("ACTUAL_PROCESS_NEGATIVE stop=%.12f status=%s\n",bad.result.stopDb,bad.result.status.c_str());
  return bins==n/2+1&&error<1e-25&&bad.result.stopFail?0:1;
}
