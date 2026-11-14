#include "dsp/resampler.h"
#include <filesystem>
#include <fstream>
#include <iterator>
#include <cstdio>
#include <cstring>
using namespace rta::dsp;
int main(){
    const auto dir=std::filesystem::temp_directory_path()/
        ("rta-cache-observation-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(GetTickCount64()));
    std::filesystem::create_directory(dir);
    _putenv_s("RLPLAYER_FILTER_CACHE","1");
    _putenv_s("RLPLAYER_FILTER_CACHE_DIR",dir.string().c_str());
    FILE* captured=nullptr;
    const auto trace=dir/"diagnostic.log";
    if(freopen_s(&captured,trace.string().c_str(),"w",stderr)!=0)return 2;
    ResamplerConfig c;c.inRate=44100;c.outRate=88200;c.channels=1;
    c.minTapsPerInput=100001;c.attenDb=40;
    std::vector<double> expected,actual;
    int checks=0,failures=0;
    auto check=[&](bool ok,const char* label){++checks;failures+=!ok;std::printf("%s %s\n",ok?"PASS":"FAIL",label);};
    {PolyphaseResampler r(c);r.prototype_into(expected);}
    {PolyphaseResampler r(c);r.prototype_into(actual);}
    check(actual.size()==expected.size()&&std::memcmp(actual.data(),expected.data(),expected.size()*8)==0,"cache hit output bit-exact");
    std::filesystem::path cached;
    for(const auto& entry:std::filesystem::directory_iterator(dir))
        if(entry.path().extension()==".bin")cached=entry.path();
    check(!cached.empty(),"private cache entry created");
    if(cached.empty())return 3;
    {std::ofstream broken(cached,std::ios::binary|std::ios::trunc);broken<<"broken";}
    {PolyphaseResampler r(c);r.prototype_into(actual);}
    check(actual.size()==expected.size()&&std::memcmp(actual.data(),expected.data(),expected.size()*8)==0,"corrupt cache recomputed bit-exact");
    // A directory at the expected entry path models an actual filesystem
    // failure. Only this process's newly created disposable entry is removed.
    std::filesystem::remove(cached);std::filesystem::create_directory(cached);
    {PolyphaseResampler r(c);r.prototype_into(actual);}
    check(actual.size()==expected.size()&&std::memcmp(actual.data(),expected.data(),expected.size()*8)==0,"cache I/O failure preserves correct recomputation");
    std::fflush(stderr);std::fclose(stderr);
    std::ifstream file(trace);const std::string log{std::istreambuf_iterator<char>(file),{}};file.close();
    for(const char* status:{"load=not_found","load=hit","load=invalid","store=stored","load=io_error","store=io_error"})
        check(log.find(status)!=std::string::npos,status);
    std::printf("%s",log.c_str());
    std::filesystem::remove_all(dir);
    std::printf("checks=%d failures=%d\n",checks,failures);return failures?1:0;
}
