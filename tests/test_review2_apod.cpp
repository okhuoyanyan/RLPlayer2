#include "../tools/dsp_measure.h"
#include "../tools/review2_apod_response.h"
#include <cstdio>
int main(){
  const auto*p=rta::dsp::find_preset("apod-20k");
  dsp_measure::Result r;auto h=dsp_measure::prototype(*p,44100,705600,r);
  const double f1=2*r.passHz/r.fs;
  auto value=review2_apod::strict_f1(h,f1);
  const double expected=rta::dsp::magnitude_db_exact(h,f1);
  bool ok=std::abs(value.db-expected)<1e-13&&value.pass==(std::abs(expected)<=.01);
  auto bad=h;bad[0]=NAN;
  ok=ok&&!review2_apod::strict_f1({},f1).pass&&!review2_apod::strict_f1(bad,f1).pass
    &&!review2_apod::strict_f1(h,NAN).pass&&review2_apod::exit_status(0,0)==2;
  printf("F1Hz=%.12f actual=%.12f expected=%.12f strict_pass=%d result=%s\n",r.passHz,value.db,expected,value.pass,ok?"PASS":"FAIL");
  return ok?0:1;
}
