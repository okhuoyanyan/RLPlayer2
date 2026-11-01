#include "dsp_measure.h"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <iostream>
#include <iomanip>
int main(int argc,char**argv) {
 if(argc<2)return 2;std::filesystem::path dir(argv[1]);
 int maxL=argc>2?std::stoi(argv[2]):32;
 std::ifstream csv(dir/"coefficients.csv");std::string line;std::getline(csv,line);
 std::ofstream out(dir.parent_path()/(dir.filename().string()+"-dd-L"+std::to_string(maxL)+".csv"));
 out<<"preset,L,T,mode,file,passDb,stopDb,stopHz,dcDb,F2Db,midDb,fftN,status\n";
 int fails=0;
 while(std::getline(csv,line)) {
  std::vector<std::string> f;std::istringstream s(line);std::string z;
  while(std::getline(s,z,','))f.push_back(z);
  int L=std::stoi(f[3]);if(L>maxL)continue;
  if(maxL>32 && L<=32)continue;
  if(maxL>32 && f[11]!="quality")continue;
  std::ifstream bin(dir/f[12],std::ios::binary|std::ios::ate);size_t bytes=(size_t)bin.tellg();bin.seekg(0);
  std::vector<double> h(bytes/8);bin.read((char*)h.data(),bytes);
  dsp_measure::Result r;r.L=L;r.M=1;r.T=std::stoi(f[4]);r.taps=h.size();r.fs=44100.*L;
  double f1=std::stod(f[7]),f2=std::stod(f[8]);r.passHz=f1*22050.;r.stopHz=(2-f1)*22050.;
  r.target=f[9]=="gauss"?100:280;r.finite=true;dsp_measure::measure(h,r);
  double f2db=dsp_measure::direct(h,f2*22050,r.fs),middb=dsp_measure::direct(h,(f1+f2)*11025,r.fs);
  out<<std::setprecision(17)<<f[0]<<','<<L<<','<<r.T<<','<<f[11]<<','<<f[12]<<','<<r.passDb<<','<<r.stopDb<<','<<r.stopHz<<','<<r.dcDb<<','<<f2db<<','<<middb<<','<<r.fftN<<','<<r.status<<'\n';out.flush();
  std::cout<<f[12]<<" pass="<<r.passDb<<" stop="<<r.stopDb<<" "<<r.status<<std::endl;
  if(f[11]=="quality"&&(r.passFail||r.stopFail))++fails;
 }
 std::cout<<"QUALITY_FAIL="<<fails<<std::endl;return fails?1:0;
}
